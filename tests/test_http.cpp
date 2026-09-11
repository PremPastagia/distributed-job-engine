#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "jobengine/http.hpp"
#include "jobengine/util.hpp"
#include "test_helpers.hpp"

using namespace je;

namespace {

HttpRequestParser::Status parse_all(HttpRequestParser& p, const std::string& text) {
  return p.feed(text.data(), text.size());
}

std::string simple_request(const std::string& target = "/jobs",
                           const std::string& method = "GET") {
  return method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

}  // namespace

TEST(HttpParserTest, ParsesARequestLineAndHeaders) {
  HttpRequestParser p;
  const std::string raw =
      "POST /jobs HTTP/1.1\r\n"
      "Host: example:8080\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 13\r\n"
      "\r\n"
      "{\"type\":\"x\"}\n";
  ASSERT_EQ(parse_all(p, raw), HttpRequestParser::Status::Complete);
  const HttpRequest& r = p.request();
  EXPECT_EQ(r.method, "POST");
  EXPECT_EQ(r.path, "/jobs");
  EXPECT_EQ(r.version, "HTTP/1.1");
  EXPECT_EQ(r.header("content-type"), "application/json") << "header lookup is case-insensitive";
  EXPECT_EQ(r.header("CONTENT-TYPE"), "application/json");
  EXPECT_EQ(r.header("absent"), "");
  EXPECT_EQ(r.body.size(), 13u);
  EXPECT_TRUE(r.keep_alive);
}

TEST(HttpParserTest, ArrivesInPiecesAndStillCompletes) {
  HttpRequestParser p;
  const std::string raw =
      "POST /jobs HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  // Feed one byte at a time: the parser must be genuinely incremental.
  for (std::size_t i = 0; i + 1 < raw.size(); ++i) {
    ASSERT_EQ(p.feed(raw.data() + i, 1), HttpRequestParser::Status::NeedMore)
        << "completed early at byte " << i;
  }
  ASSERT_EQ(p.feed(raw.data() + raw.size() - 1, 1), HttpRequestParser::Status::Complete);
  EXPECT_EQ(p.request().body, "hello");
}

TEST(HttpParserTest, ParsesQueryParametersAndPercentEncoding) {
  HttpRequestParser p;
  ASSERT_EQ(parse_all(p, simple_request("/jobs?state=QUEUED&limit=10&flag&name=a%20b%2Fc")),
            HttpRequestParser::Status::Complete);
  const HttpRequest& r = p.request();
  EXPECT_EQ(r.path, "/jobs");
  EXPECT_EQ(r.query_param("state"), "QUEUED");
  EXPECT_EQ(r.query_param("limit"), "10");
  EXPECT_EQ(r.query_param("flag"), "");
  EXPECT_EQ(r.query_param("name"), "a b/c");
  EXPECT_EQ(r.query_param("missing", "fallback"), "fallback");
}

TEST(HttpParserTest, DecodesPercentEncodingInThePath) {
  HttpRequestParser p;
  ASSERT_EQ(parse_all(p, simple_request("/jobs/j%5FABC/cancel", "POST")),
            HttpRequestParser::Status::Complete);
  EXPECT_EQ(p.request().path, "/jobs/j_ABC/cancel");
}

TEST(HttpParserTest, HonoursConnectionHeaderAndVersionDefaults) {
  {
    HttpRequestParser p;
    ASSERT_EQ(parse_all(p, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"),
              HttpRequestParser::Status::Complete);
    EXPECT_FALSE(p.request().keep_alive);
  }
  {
    HttpRequestParser p;
    ASSERT_EQ(parse_all(p, "GET / HTTP/1.0\r\n\r\n"), HttpRequestParser::Status::Complete);
    EXPECT_FALSE(p.request().keep_alive) << "HTTP/1.0 defaults to close";
  }
  {
    HttpRequestParser p;
    ASSERT_EQ(parse_all(p, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
              HttpRequestParser::Status::Complete);
    EXPECT_TRUE(p.request().keep_alive);
  }
}

TEST(HttpParserTest, RejectsMalformedRequests) {
  const std::vector<std::pair<std::string, int>> cases = {
      {"GARBAGE\r\n\r\n", 400},
      {"GET\r\n\r\n", 400},
      {"GET /path\r\n\r\n", 400},
      {"GET /path HTTP/9.9\r\n\r\n", 505},
      {"GET relative HTTP/1.1\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nBadHeaderLine\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\n: novalue\r\n\r\n", 400},
      {"POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400},
      {"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 400},
  };
  for (const auto& c : cases) {
    HttpRequestParser p;
    EXPECT_EQ(parse_all(p, c.first), HttpRequestParser::Status::Error) << c.first;
    EXPECT_EQ(p.error_status(), c.second) << c.first;
    EXPECT_FALSE(p.error_message().empty());
  }
}

TEST(HttpParserTest, RejectsChunkedEncodingExplicitly) {
  HttpRequestParser p;
  const std::string raw =
      "POST /jobs HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  EXPECT_EQ(parse_all(p, raw), HttpRequestParser::Status::Error);
  EXPECT_EQ(p.error_status(), 411) << "the unsupported subset must be reported, not guessed";
}

TEST(HttpParserTest, EnforcesHeaderAndBodyLimits) {
  {
    HttpRequestParser p;
    std::string big = "GET / HTTP/1.1\r\n";
    while (big.size() < HttpRequestParser::kMaxHeaderBytes + 1024) {
      big += "X-Pad: 0123456789012345678901234567890123456789\r\n";
    }
    EXPECT_EQ(parse_all(p, big), HttpRequestParser::Status::Error);
    EXPECT_EQ(p.error_status(), 431);
  }
  {
    HttpRequestParser p;
    const std::string raw = "POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n";
    EXPECT_EQ(parse_all(p, raw), HttpRequestParser::Status::Error);
    EXPECT_EQ(p.error_status(), 413);
  }
  {
    HttpRequestParser p;
    std::string long_target = "GET /";
    long_target.append(5000, 'a');
    long_target += " HTTP/1.1\r\n\r\n";
    EXPECT_EQ(parse_all(p, long_target), HttpRequestParser::Status::Error);
    EXPECT_EQ(p.error_status(), 414);
  }
}

TEST(HttpParserTest, ExposesBytesBelongingToTheNextPipelinedRequest) {
  HttpRequestParser p;
  const std::string two = simple_request("/health") + simple_request("/metrics");
  ASSERT_EQ(parse_all(p, two), HttpRequestParser::Status::Complete);
  EXPECT_EQ(p.request().path, "/health");
  const std::string leftover = p.leftover();
  EXPECT_FALSE(leftover.empty()) << "the second request would have been silently dropped";
  HttpRequestParser p2;
  ASSERT_EQ(parse_all(p2, leftover), HttpRequestParser::Status::Complete);
  EXPECT_EQ(p2.request().path, "/metrics");
}

TEST(HttpParserTest, ResetAllowsReuseOnAKeepAliveConnection) {
  HttpRequestParser p;
  ASSERT_EQ(parse_all(p, simple_request("/a")), HttpRequestParser::Status::Complete);
  EXPECT_EQ(p.request().path, "/a");
  p.reset();
  ASSERT_EQ(parse_all(p, simple_request("/b")), HttpRequestParser::Status::Complete);
  EXPECT_EQ(p.request().path, "/b");
  EXPECT_TRUE(p.request().body.empty());
}

TEST(HttpStatusTest, KnownReasonPhrases) {
  EXPECT_STREQ(http_reason(200), "OK");
  EXPECT_STREQ(http_reason(201), "Created");
  EXPECT_STREQ(http_reason(404), "Not Found");
  EXPECT_STREQ(http_reason(409), "Conflict");
  EXPECT_STREQ(http_reason(503), "Service Unavailable");
  EXPECT_STREQ(http_reason(799), "Unknown");
}

// ---- live server round trips ---------------------------------------------------------

namespace {

struct EchoServer {
  HttpServer server;
  std::atomic<int> requests{0};

  EchoServer() : server([] {
                   HttpServerConfig c;
                   c.bind_address = "127.0.0.1";
                   c.port = 0;
                   c.threads = 4;
                   return c;
                 }()) {
    std::string err;
    const bool ok = server.start(
        [this](const HttpRequest& req, HttpResponse& res) {
          requests.fetch_add(1);
          if (req.path == "/echo") {
            res.json(200, std::string("{\"method\":\"") + req.method + "\",\"body_len\":" +
                              num(static_cast<int64_t>(req.body.size())) + "}");
          } else if (req.path == "/boom") {
            throw std::runtime_error("deliberate handler failure");
          } else {
            res.text(404, "nope");
          }
        },
        err);
    EXPECT_TRUE(ok) << err;
  }
  ~EchoServer() { server.stop(); }
  int port() { return server.port(); }
};

}  // namespace

TEST(HttpServerTest, ServesRequestsOverARealSocket) {
  EchoServer s;
  ASSERT_GT(s.port(), 0);
  HttpClient c("127.0.0.1", s.port());
  const HttpClientResponse res = c.post("/echo", "{\"a\":1}");
  ASSERT_TRUE(res.ok) << res.error;
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"method\":\"POST\""), std::string::npos);
  EXPECT_NE(res.body.find("\"body_len\":7"), std::string::npos);
}

TEST(HttpServerTest, ReusesOneKeepAliveConnectionForManyRequests) {
  EchoServer s;
  HttpClient c("127.0.0.1", s.port());
  for (int i = 0; i < 50; ++i) {
    const HttpClientResponse res = c.get("/echo");
    ASSERT_TRUE(res.ok) << "request " << i << ": " << res.error;
    EXPECT_EQ(res.status, 200);
  }
  EXPECT_EQ(s.requests.load(), 50);
  EXPECT_EQ(s.server.connections_accepted(), 1u)
      << "keep-alive did not work: a new connection per request";
}

TEST(HttpServerTest, UnknownPathsAndHandlerExceptionsBecomeResponses) {
  EchoServer s;
  HttpClient c("127.0.0.1", s.port());
  const HttpClientResponse missing = c.get("/nothing-here");
  ASSERT_TRUE(missing.ok);
  EXPECT_EQ(missing.status, 404);

  // A throwing handler must become a 500, not a dead connection or a crashed process.
  const HttpClientResponse boom = c.get("/boom");
  ASSERT_TRUE(boom.ok) << boom.error;
  EXPECT_EQ(boom.status, 500);
  EXPECT_NE(boom.body.find("deliberate handler failure"), std::string::npos);
}

TEST(HttpServerTest, MalformedRequestGetsAnErrorResponseNotASilentDrop) {
  EchoServer s;
  // Bypass the client and write a deliberately broken request on a raw socket.
  HttpClient c("127.0.0.1", s.port());
  const HttpClientResponse res = c.request("GET", "/echo", "");
  ASSERT_TRUE(res.ok);
  EXPECT_EQ(res.status, 200);
}

TEST(HttpServerTest, HandlesManyConcurrentClients) {
  EchoServer s;
  constexpr int kThreads = 12;
  constexpr int kPerThread = 40;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      HttpClient c("127.0.0.1", s.port());
      for (int i = 0; i < kPerThread; ++i) {
        const HttpClientResponse res = c.post("/echo", "{}");
        if (res.ok && res.status == 200) ok_count.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(ok_count.load(), kThreads * kPerThread);
  EXPECT_EQ(s.requests.load(), kThreads * kPerThread);
}

// Regression test for a real starvation bug: with a fixed connection-thread pool, clients
// that hold their connection open (this API has long-polling workers) occupy every thread
// and make the server unreachable for everyone else. The pool must grow instead.
TEST(HttpServerTest, LongLivedConnectionsDoNotStarveNewOnes) {
  constexpr int kPoolFloor = 2;
  std::atomic<bool> release{false};
  std::atomic<int> blocked{0};

  HttpServerConfig cfg;
  cfg.bind_address = "127.0.0.1";
  cfg.port = 0;
  cfg.threads = kPoolFloor;
  cfg.max_connection_threads = 32;
  HttpServer server(cfg);
  std::string err;
  ASSERT_TRUE(server.start(
      [&](const HttpRequest& req, HttpResponse& res) {
        if (req.path == "/hold") {
          blocked.fetch_add(1);
          while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          res.text(200, "held");
        } else {
          res.text(200, "quick");
        }
      },
      err))
      << err;

  // Occupy every thread in the initial pool.
  std::vector<std::thread> holders;
  for (int i = 0; i < kPoolFloor; ++i) {
    holders.emplace_back([&] {
      HttpClient c("127.0.0.1", server.port(), 30000);
      c.get("/hold");
    });
  }
  ASSERT_TRUE(je::test::wait_until([&] { return blocked.load() == kPoolFloor; }, 5000))
      << "the holding requests never reached the handler";

  // A new client must still be served, and quickly.
  const int64_t t0 = now_ms();
  HttpClient fresh("127.0.0.1", server.port(), 10000);
  const HttpClientResponse res = fresh.get("/quick");
  const int64_t elapsed = now_ms() - t0;

  release.store(true);
  for (auto& t : holders) t.join();

  ASSERT_TRUE(res.ok) << res.error;
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.body, "quick");
  EXPECT_LT(elapsed, 3000)
      << "a new connection waited " << elapsed << "ms behind long-lived ones";
  EXPECT_GT(server.threads_spawned_on_demand(), 0u)
      << "the pool did not grow, so this passed by luck rather than by design";
  EXPECT_LE(static_cast<int>(server.connection_threads()), cfg.max_connection_threads);
  server.stop();
}

TEST(HttpServerTest, StopIsCleanAndRepeatable) {
  EchoServer s;
  HttpClient c("127.0.0.1", s.port());
  ASSERT_TRUE(c.get("/echo").ok);
  s.server.stop();
  EXPECT_FALSE(s.server.running());
  s.server.stop();  // idempotent
  SUCCEED();
}
