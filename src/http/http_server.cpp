#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "jobengine/http.hpp"

#include <mutex>

#include "jobengine/json.hpp"
#include "jobengine/util.hpp"

namespace je {
namespace {

std::once_flag g_sigpipe_once;
void ignore_sigpipe() {
  // Writing to a socket the peer already closed raises SIGPIPE, which by default kills the
  // process. Every server that writes to sockets has to handle this.
  std::call_once(g_sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });
}

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

// Writes the whole buffer, retrying on partial writes and EINTR.
bool write_all(int fd, const char* data, std::size_t n, int64_t timeout_ms) {
  std::size_t sent = 0;
  const int64_t deadline = now_ms() + timeout_ms;
  while (sent < n) {
    const ssize_t k = ::send(fd, data + sent, n - sent, 0);
    if (k > 0) { sent += static_cast<std::size_t>(k); continue; }
    if (k < 0 && (errno == EINTR)) continue;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (now_ms() > deadline) return false;
      struct pollfd pfd{fd, POLLOUT, 0};
      ::poll(&pfd, 1, 50);
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

const char* http_reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

// ---- HttpRequest ----------------------------------------------------------------------

std::string HttpRequest::header(const std::string& name) const {
  for (const auto& kv : headers) {
    if (iequals(kv.first, name)) return kv.second;
  }
  return std::string();
}

std::string HttpRequest::query_param(const std::string& name, const std::string& def) const {
  const auto it = query.find(name);
  return it == query.end() ? def : it->second;
}

// ---- parser ------------------------------------------------------------------------------

void HttpRequestParser::reset() {
  buffer_.clear();
  req_ = HttpRequest();
  headers_done_ = false;
  body_expected_ = 0;
  body_start_ = 0;
  consumed_ = 0;
  error_status_ = 400;
  error_message_.clear();
}

HttpRequestParser::Status HttpRequestParser::fail(int status, std::string message) {
  error_status_ = status;
  error_message_ = std::move(message);
  return Status::Error;
}

HttpRequestParser::Status HttpRequestParser::feed(const char* data, std::size_t n) {
  buffer_.append(data, n);
  if (!headers_done_) {
    const std::size_t end = buffer_.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (buffer_.size() > kMaxHeaderBytes) {
        return fail(431, "request headers exceed " + num(static_cast<int64_t>(kMaxHeaderBytes)) +
                             " bytes");
      }
      return Status::NeedMore;
    }
    if (end > kMaxHeaderBytes) {
      return fail(431, "request headers too large");
    }
    body_start_ = end + 4;
    const Status s = parse_headers();
    if (s == Status::Error) return s;
    headers_done_ = true;
  }
  if (buffer_.size() - body_start_ < body_expected_) return Status::NeedMore;
  req_.body = buffer_.substr(body_start_, body_expected_);
  consumed_ = body_start_ + body_expected_;
  return Status::Complete;
}

HttpRequestParser::Status HttpRequestParser::parse_headers() {
  const std::string head = buffer_.substr(0, body_start_ - 4);
  const std::vector<std::string> lines = split(head, '\n');
  if (lines.empty()) return fail(400, "empty request");

  // Request line: METHOD SP TARGET SP VERSION
  const std::string request_line = trim(lines[0]);
  const std::size_t sp1 = request_line.find(' ');
  if (sp1 == std::string::npos) return fail(400, "malformed request line");
  const std::size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return fail(400, "malformed request line");
  req_.method = request_line.substr(0, sp1);
  req_.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  req_.version = request_line.substr(sp2 + 1);
  if (req_.method.empty() || req_.target.empty()) return fail(400, "malformed request line");
  if (req_.target.size() > 4096) return fail(414, "request target too long");
  if (!starts_with(req_.version, "HTTP/1.")) return fail(505, "unsupported HTTP version");

  // Split target into path and query string.
  const std::size_t q = req_.target.find('?');
  if (q == std::string::npos) {
    req_.path = url_decode(req_.target);
  } else {
    req_.path = url_decode(req_.target.substr(0, q));
    for (const std::string& pair_text : split(req_.target.substr(q + 1), '&')) {
      if (pair_text.empty()) continue;
      const std::size_t eq = pair_text.find('=');
      if (eq == std::string::npos) req_.query[url_decode(pair_text)] = "";
      else req_.query[url_decode(pair_text.substr(0, eq))] = url_decode(pair_text.substr(eq + 1));
    }
  }
  if (req_.path.empty() || req_.path[0] != '/') return fail(400, "request path must be absolute");

  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string line = trim(lines[i]);
    if (line.empty()) continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return fail(400, "malformed header line");
    const std::string name = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));
    if (name.empty()) return fail(400, "empty header name");
    req_.headers.emplace_back(name, value);
  }

  // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
  req_.keep_alive = req_.version == "HTTP/1.1";
  const std::string conn = to_lower(req_.header("Connection"));
  if (conn == "close") req_.keep_alive = false;
  else if (conn == "keep-alive") req_.keep_alive = true;

  if (!to_lower(req_.header("Transfer-Encoding")).empty()) {
    return fail(411, "chunked transfer encoding is not supported; send Content-Length");
  }
  const std::string cl = req_.header("Content-Length");
  if (!cl.empty()) {
    int64_t v = 0;
    if (!parse_int(cl, v) || v < 0) return fail(400, "invalid Content-Length");
    if (static_cast<std::size_t>(v) > kMaxBodyBytes) {
      return fail(413, "body exceeds " + num(static_cast<int64_t>(kMaxBodyBytes)) + " bytes");
    }
    body_expected_ = static_cast<std::size_t>(v);
  } else if (req_.method == "POST" || req_.method == "PUT" || req_.method == "PATCH") {
    body_expected_ = 0;  // a bodyless POST is allowed; handlers validate their own input
  }
  return Status::NeedMore;
}

// ---- server ------------------------------------------------------------------------------

HttpServer::HttpServer(HttpServerConfig cfg) : cfg_(std::move(cfg)) {}
HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(Handler handler, std::string& err) {
  ignore_sigpipe();
  handler_ = std::move(handler);

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) { err = std::string("socket(): ") + std::strerror(errno); return false; }
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
  if (::inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr) != 1) {
    err = "invalid bind address: " + cfg_.bind_address;
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = std::string("bind(): ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, cfg_.backlog) != 0) {
    err = std::string("listen(): ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  }

  running_.store(true);
  stopping_.store(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < std::max(1, cfg_.threads); ++i) {
      workers_.emplace_back([this] { connection_loop(); });
    }
  }
  acceptor_ = std::thread([this] { accept_loop(); });
  log_info("http", concat({"listening on ", cfg_.bind_address, ":",
                           num(static_cast<int64_t>(bound_port_)), " with ",
                           num(static_cast<int64_t>(cfg_.threads)), " connection threads"}));
  return true;
}

void HttpServer::accept_loop() {
  while (!stopping_.load()) {
    // poll() with a timeout instead of a blocking accept(), so stop() does not depend on
    // closing a file descriptor another thread is blocked on.
    struct pollfd pfd{listen_fd_, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, 100);
    if (rc <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (stopping_.load()) break;
      continue;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    accepted_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(fd);
    }
    cv_.notify_one();
    grow_pool_if_starved();
  }
}

void HttpServer::grow_pool_if_starved() {
  // Every connection thread is busy, so this connection would sit in the queue until one of
  // them finishes - which, with keep-alive or a long poll, may be a long time. Add a thread
  // instead, up to the hard cap. Under ordinary load nothing is starved and this never
  // fires, so it does not change behaviour until it is actually needed.
  if (idle_threads_.load(std::memory_order_relaxed) > 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_.load()) return;
  if (pending_.empty()) return;
  if (static_cast<int>(workers_.size()) >= cfg_.max_connection_threads) return;
  workers_.emplace_back([this] { connection_loop(); });
  threads_spawned_on_demand_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t HttpServer::connection_threads() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workers_.size();
}

// Takes the next accepted connection, or -1 when the server is stopping. Confining the
// lock to this helper keeps the blocking socket work in serve_connection provably outside
// any critical section - which is also what the static analyser needs in order to see it.
int HttpServer::next_connection() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_threads_.fetch_add(1, std::memory_order_relaxed);
  cv_.wait(lock, [this] { return stopping_.load() || !pending_.empty(); });
  idle_threads_.fetch_sub(1, std::memory_order_relaxed);
  if (pending_.empty()) return -1;
  const int fd = pending_.back();
  pending_.pop_back();
  lock.unlock();
  return fd;
}

void HttpServer::connection_loop() {
  for (;;) {
    const int fd = next_connection();
    if (fd < 0) {
      if (stopping_.load()) return;
      continue;
    }
    serve_connection(fd);
  }
}

void HttpServer::serve_connection(int fd) {
  HttpRequestParser parser;
  std::string pending_input;
  char buf[16384];

  for (;;) {
    parser.reset();
    bool complete = false;
    bool transport_error = false;
    int parse_error = 0;
    std::string parse_message;

    // Feed anything left over from a previous pipelined read first.
    if (!pending_input.empty()) {
      const std::string chunk = pending_input;
      pending_input.clear();
      const auto st = parser.feed(chunk.data(), chunk.size());
      if (st == HttpRequestParser::Status::Complete) complete = true;
      else if (st == HttpRequestParser::Status::Error) {
        parse_error = parser.error_status();
        parse_message = parser.error_message();
      }
    }

    const int64_t deadline = now_ms() + cfg_.idle_timeout_ms;
    while (!complete && parse_error == 0) {
      struct pollfd pfd{fd, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, 200);
      if (rc < 0) {
        if (errno == EINTR) continue;
        transport_error = true;
        break;
      }
      if (rc == 0) {
        if (stopping_.load() || now_ms() > deadline) { transport_error = true; break; }
        continue;
      }
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0) { transport_error = true; break; }  // peer closed
      if (n < 0) {
        if (errno == EINTR) continue;
        transport_error = true;
        break;
      }
      const auto st = parser.feed(buf, static_cast<std::size_t>(n));
      if (st == HttpRequestParser::Status::Complete) complete = true;
      else if (st == HttpRequestParser::Status::Error) {
        parse_error = parser.error_status();
        parse_message = parser.error_message();
      }
    }

    if (transport_error) break;

    HttpResponse res;
    bool keep_alive = false;
    if (parse_error != 0) {
      res.status = parse_error;
      res.content_type = "application/json";
      std::string escaped;
      Json::escape_into(parse_message, escaped);
      res.body = std::string("{\"error\":") + escaped + "}";
      keep_alive = false;  // the stream framing is no longer trustworthy
    } else {
      const HttpRequest& req = parser.request();
      keep_alive = req.keep_alive && !stopping_.load();
      try {
        handler_(req, res);
      } catch (const std::exception& e) {
        res.status = 500;
        std::string escaped;
        Json::escape_into(std::string("handler threw: ") + e.what(), escaped);
        res.body = std::string("{\"error\":") + escaped + "}";
      }
      handled_.fetch_add(1, std::memory_order_relaxed);
      // Anything read past this request belongs to the next one on the connection.
      pending_input = parser.leftover();
    }

    std::string out;
    out.reserve(res.body.size() + 256);
    out += "HTTP/1.1 ";
    out += num(static_cast<int64_t>(res.status));
    out += ' ';
    out += http_reason(res.status);
    out += "\r\nContent-Type: ";
    out += res.content_type;
    out += "\r\nContent-Length: ";
    out += num(static_cast<int64_t>(res.body.size()));
    out += "\r\nConnection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n";
    for (const auto& kv : res.headers) {
      out += kv.first;
      out += ": ";
      out += kv.second;
      out += "\r\n";
    }
    out += "\r\n";
    out += res.body;

    if (!write_all(fd, out.data(), out.size(), cfg_.read_timeout_ms)) break;
    if (!keep_alive) break;
  }
  ::close(fd);
}

void HttpServer::stop() {
  if (!running_.exchange(false)) return;
  stopping_.store(true);
  cv_.notify_all();
  if (acceptor_.joinable()) acceptor_.join();
  // The acceptor can add threads while running, so take the list under the lock before
  // joining - the acceptor has stopped by this point, so nothing else will append.
  std::vector<std::thread> to_join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_join.swap(workers_);
  }
  cv_.notify_all();
  for (auto& t : to_join) {
    if (t.joinable()) t.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const int fd : pending_) ::close(fd);
    pending_.clear();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  log_info("http", "server stopped");
}

}  // namespace je
