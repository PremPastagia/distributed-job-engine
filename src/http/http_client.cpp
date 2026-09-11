#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "jobengine/http.hpp"
#include "jobengine/util.hpp"

namespace je {
namespace {

bool send_all(int fd, const char* data, std::size_t n, int64_t deadline_ms) {
  std::size_t sent = 0;
  while (sent < n) {
    const ssize_t k = ::send(fd, data + sent, n - sent, 0);
    if (k > 0) { sent += static_cast<std::size_t>(k); continue; }
    if (k < 0 && errno == EINTR) continue;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (now_ms() > deadline_ms) return false;
      struct pollfd pfd{fd, POLLOUT, 0};
      ::poll(&pfd, 1, 50);
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

HttpClient::HttpClient(std::string host, int port, int64_t timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

HttpClient::~HttpClient() { close(); }

void HttpClient::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool HttpClient::ensure_connected(std::string& err) {
  if (fd_ >= 0) return true;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { err = std::string("socket(): ") + std::strerror(errno); return false; }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    // Not a dotted-quad literal: fall back to a name lookup.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
      err = "cannot resolve host: " + host_;
      ::close(fd);
      return false;
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = std::string("connect(): ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  fd_ = fd;
  return true;
}

HttpClientResponse HttpClient::request(const std::string& method, const std::string& path,
                                       const std::string& body,
                                       const std::string& content_type) {
  HttpClientResponse out;
  // One transparent retry: a pooled keep-alive connection can be closed by the server
  // between requests, which looks like a write or read failure on the first attempt.
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string err;
    if (!ensure_connected(err)) {
      out.error = err;
      return out;
    }
    const int64_t deadline = now_ms() + timeout_ms_;

    std::string req;
    req.reserve(body.size() + 256);
    req += method;
    req += ' ';
    req += path;
    req += " HTTP/1.1\r\nHost: ";
    req += host_;
    req += ':';
    req += num(static_cast<int64_t>(port_));
    req += "\r\nConnection: keep-alive\r\n";
    if (!body.empty()) {
      req += "Content-Type: ";
      req += content_type;
      req += "\r\n";
    }
    req += "Content-Length: ";
    req += num(static_cast<int64_t>(body.size()));
    req += "\r\n\r\n";
    req += body;

    if (!send_all(fd_, req.data(), req.size(), deadline)) {
      close();
      if (attempt == 0) continue;
      out.error = "send failed";
      return out;
    }

    // Read the status line and headers, then Content-Length bytes of body.
    std::string buffer;
    char buf[16384];
    std::size_t header_end = std::string::npos;
    bool transport_failed = false;
    while (header_end == std::string::npos) {
      struct pollfd pfd{fd_, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, 100);
      if (rc < 0) {
        if (errno == EINTR) continue;
        transport_failed = true;
        break;
      }
      if (rc == 0) {
        if (now_ms() > deadline) { out.error = "response timeout"; close(); return out; }
        continue;
      }
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
      if (n <= 0) { transport_failed = true; break; }
      buffer.append(buf, static_cast<std::size_t>(n));
      header_end = buffer.find("\r\n\r\n");
      if (buffer.size() > 1u << 22) { out.error = "response headers too large"; close(); return out; }
    }
    if (transport_failed) {
      close();
      if (attempt == 0) continue;
      out.error = "connection closed before a response was received";
      return out;
    }

    const std::string head = buffer.substr(0, header_end);
    const std::vector<std::string> lines = split(head, '\n');
    if (lines.empty()) { out.error = "empty response"; close(); return out; }
    {
      const std::string status_line = trim(lines[0]);
      const std::size_t sp1 = status_line.find(' ');
      if (sp1 == std::string::npos) { out.error = "malformed status line"; close(); return out; }
      const std::size_t sp2 = status_line.find(' ', sp1 + 1);
      const std::string code = status_line.substr(
          sp1 + 1, (sp2 == std::string::npos ? status_line.size() : sp2) - sp1 - 1);
      int64_t v = 0;
      if (!parse_int(code, v)) { out.error = "malformed status code"; close(); return out; }
      out.status = static_cast<int>(v);
    }

    int64_t content_length = -1;
    bool server_closes = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
      const std::string line = trim(lines[i]);
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string name = to_lower(trim(line.substr(0, colon)));
      const std::string value = trim(line.substr(colon + 1));
      if (name == "content-length") parse_int(value, content_length);
      else if (name == "connection" && to_lower(value) == "close") server_closes = true;
    }
    if (content_length < 0) { out.error = "response has no Content-Length"; close(); return out; }

    std::string payload = buffer.substr(header_end + 4);
    while (static_cast<int64_t>(payload.size()) < content_length) {
      struct pollfd pfd{fd_, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, 100);
      if (rc < 0) {
        if (errno == EINTR) continue;
        out.error = "read failed";
        close();
        return out;
      }
      if (rc == 0) {
        if (now_ms() > deadline) { out.error = "body read timeout"; close(); return out; }
        continue;
      }
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
      if (n <= 0) { out.error = "connection closed mid-body"; close(); return out; }
      payload.append(buf, static_cast<std::size_t>(n));
    }
    out.body = payload.substr(0, static_cast<std::size_t>(content_length));
    out.ok = true;
    if (server_closes) close();
    return out;
  }
  out.error = "request failed";
  return out;
}

}  // namespace je
