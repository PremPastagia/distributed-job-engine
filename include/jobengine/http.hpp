#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace je {

// Supported subset of HTTP/1.1 (docs/DESIGN_DECISIONS.md D-02):
//   * request line + headers up to kMaxHeaderBytes
//   * Content-Length bodies up to kMaxBodyBytes
//   * keep-alive, and Connection: close
//   * NOT supported: chunked request bodies (411), TLS, HTTP/2, request pipelining
struct HttpRequest {
  std::string method;
  std::string target;  // raw request target, e.g. "/jobs?state=QUEUED"
  std::string path;    // decoded path, e.g. "/jobs"
  std::string version;
  std::vector<std::pair<std::string, std::string>> headers;
  std::unordered_map<std::string, std::string> query;
  std::string body;
  bool keep_alive = true;

  std::string header(const std::string& name) const;  // case-insensitive; "" when absent
  std::string query_param(const std::string& name, const std::string& def = "") const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  void json(int code, const std::string& json_body) {
    status = code;
    content_type = "application/json";
    body = json_body;
  }
  void text(int code, const std::string& text_body) {
    status = code;
    content_type = "text/plain; charset=utf-8";
    body = text_body;
  }
};

const char* http_reason(int status);

// Incremental request parser. Kept free of socket code so malformed-input behaviour can be
// unit-tested directly rather than only through a live server.
class HttpRequestParser {
 public:
  enum class Status { NeedMore, Complete, Error };
  static constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
  static constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;

  Status feed(const char* data, std::size_t n);
  Status feed(const std::string& s) { return feed(s.data(), s.size()); }
  const HttpRequest& request() const { return req_; }
  int error_status() const { return error_status_; }
  const std::string& error_message() const { return error_message_; }
  void reset();
  // Bytes consumed past the end of the completed request (start of the next pipelined one).
  std::size_t leftover_offset() const { return consumed_; }
  // Bytes already read that belong to the NEXT request on the connection. A client is
  // allowed to put two requests in one write, so dropping this would silently lose one.
  std::string leftover() const {
    return consumed_ < buffer_.size() ? buffer_.substr(consumed_) : std::string();
  }

 private:
  Status fail(int status, std::string message);
  Status parse_headers();

  std::string buffer_;
  HttpRequest req_;
  bool headers_done_ = false;
  std::size_t body_expected_ = 0;
  std::size_t body_start_ = 0;
  std::size_t consumed_ = 0;
  int error_status_ = 400;
  std::string error_message_;
};

struct HttpServerConfig {
  std::string bind_address = "127.0.0.1";
  int port = 8080;
  // Connection threads started up front. This is a floor, not a ceiling: a client that
  // holds its connection open (this API has long-polling workers) would otherwise be able
  // to occupy every thread and starve everyone else, so the pool grows on demand up to
  // max_connection_threads.
  int threads = 16;
  int max_connection_threads = 256;
  int backlog = 512;
  int64_t idle_timeout_ms = 15000;  // keep-alive idle timeout
  int64_t read_timeout_ms = 15000;
};

// Blocking, thread-per-connection HTTP server with a bounded connection pool.
class HttpServer {
 public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

  explicit HttpServer(HttpServerConfig cfg);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool start(Handler handler, std::string& err);
  void stop();
  bool running() const { return running_.load(); }
  // Actual bound port; useful when the configured port is 0 (ephemeral, used by tests).
  int port() const { return bound_port_; }
  uint64_t connections_accepted() const { return accepted_.load(); }
  uint64_t requests_handled() const { return handled_.load(); }
  // Connection threads added beyond the configured floor because every existing thread was
  // busy. A non-zero value means long-lived connections were about to starve new ones.
  uint64_t threads_spawned_on_demand() const { return threads_spawned_on_demand_.load(); }
  std::size_t connection_threads() const;

 private:
  void accept_loop();
  void connection_loop();
  int next_connection();
  // Adds a connection thread when nothing is idle and the cap allows it.
  void grow_pool_if_starved();
  void serve_connection(int fd);

  HttpServerConfig cfg_;
  Handler handler_;
  int listen_fd_ = -1;
  int bound_port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::vector<std::thread> workers_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<int> pending_;  // accepted fds waiting for a connection thread
  std::atomic<int> idle_threads_{0};
  std::atomic<uint64_t> threads_spawned_on_demand_{0};
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> handled_{0};
};

// ---- client ---------------------------------------------------------------------------
struct HttpClientResponse {
  bool ok = false;         // transport-level success (a 500 reply still has ok == true)
  int status = 0;
  std::string body;
  std::string error;
};

// Blocking HTTP/1.1 client with connection reuse. One instance is not thread-safe; give
// each thread its own.
class HttpClient {
 public:
  HttpClient(std::string host, int port, int64_t timeout_ms = 30000);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpClientResponse request(const std::string& method, const std::string& path,
                             const std::string& body = "",
                             const std::string& content_type = "application/json");
  HttpClientResponse get(const std::string& path) { return request("GET", path); }
  HttpClientResponse post(const std::string& path, const std::string& body) {
    return request("POST", path, body);
  }
  void close();
  void set_timeout_ms(int64_t ms) { timeout_ms_ = ms; }

 private:
  bool ensure_connected(std::string& err);

  std::string host_;
  int port_;
  int64_t timeout_ms_;
  int fd_ = -1;
};

}  // namespace je
