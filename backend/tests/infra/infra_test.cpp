// M1/M2 基础设施单元测试（错误码 / 配置 / 线程池 / 定时器 / 路由）。
// 使用模块测试骨架 tests/support/test_harness.h（tnycl::test::run_all）。
#include <atomic>
#include <chrono>
#include <thread>

#include "support/test_harness.h"
#include "tnycl/config.h"
#include "tnycl/error_codes.h"
#include "tnycl/http_router.h"
#include "tnycl/status.h"
#include "tnycl/thread_pool.h"
#include "tnycl/timer.h"

using namespace tnycl;

// ---- 错误码 ----
TNYCL_TEST(error_code_ok_is_zero) {
  CHECK_EQ(error_code(ErrorCode::kOk), 0);
  CHECK_EQ(http_status(ErrorCode::kOk), 200);
}

TNYCL_TEST(error_http_status_mapping) {
  CHECK_EQ(http_status(ErrorCode::kPayloadTooLarge), 413);
  CHECK_EQ(http_status(ErrorCode::kRateLimited), 429);
  CHECK_EQ(http_status(ErrorCode::kIdempotencyConflict), 409);
  CHECK_EQ(http_status(ErrorCode::kUnsupportedMediaType), 415);
  CHECK_EQ(http_status(ErrorCode::kInternalError), 500);
  CHECK_EQ(http_status(ErrorCode::kInvalidCredentials), 401);
  CHECK_EQ(http_status(ErrorCode::kAccountLocked), 423);
  CHECK_EQ(http_status(ErrorCode::kWeakPassword), 422);
  CHECK_EQ(http_status(ErrorCode::kPointsInsufficient), 422);
  CHECK_EQ(http_status(ErrorCode::kThemeNotFound), 404);
  CHECK_EQ(http_status(ErrorCode::kLeaderboardForbidden), 403);
  CHECK_EQ(http_status(ErrorCode::kDbUnavailable), 503);
  CHECK_EQ(http_status(ErrorCode::kDbBusy), 503);
  CHECK_EQ(http_status(ErrorCode::kCpuBusy), 503);
  CHECK_EQ(http_status(ErrorCode::kMaintenance), 503);
}

TNYCL_TEST(error_boundary_semantics) {
  CHECK_EQ(http_status(ErrorCode::kValidationFailed), 400);
  CHECK_EQ(http_status(ErrorCode::kProfileInvalid), 400);
  CHECK_EQ(http_status(ErrorCode::kMealTypeInvalid), 400);
  CHECK_EQ(http_status(ErrorCode::kExportEmpty), 422);
  CHECK_EQ(http_status(ErrorCode::kProfileNotFound), 404);
  CHECK_EQ(http_status(ErrorCode::kMealNotFound), 404);
  CHECK_EQ(http_status(ErrorCode::kFoodNotFound), 404);
  CHECK_EQ(http_status(ErrorCode::kUsernameTaken), 409);
  CHECK_EQ(http_status(ErrorCode::kMealDuplicateConflict), 409);
}

TNYCL_TEST(error_message_and_name_nonempty) {
  CHECK(!error_message(ErrorCode::kBadRequest).empty());
  CHECK(!error_message(ErrorCode::kInvalidCredentials).empty());
  CHECK(!error_message(ErrorCode::kDbUnavailable).empty());
  CHECK(!error_message(ErrorCode::kOk).empty());
  CHECK(!error_name(ErrorCode::kPointsInsufficient).empty());
}

// ---- 配置与启动自检（parse_config 直接解析内容，不依赖文件 IO）----
TNYCL_TEST(config_default_valid) {
  Config c;
  validate_config(c);
  CHECK(true);
}

TNYCL_TEST(config_reject_timezone) {
  Config c;
  c.business.timezone = "UTC";
  bool threw = false;
  try { validate_config(c); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

TNYCL_TEST(config_reject_pool_size) {
  Config c;
  c.business.timezone = "Asia/Shanghai";
  c.server.io_threads = 8;
  c.mysql.pool_size = 2;
  bool threw = false;
  try { validate_config(c); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

TNYCL_TEST(config_reject_argon2_memory) {
  Config c;
  c.business.timezone = "Asia/Shanghai";
  c.cpu.argon2_memory_kb = 10000;
  bool threw = false;
  try { validate_config(c); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

TNYCL_TEST(config_parse_from_content) {
  Config c = parse_config(
      "[server]\nport = 9000\n[business]\ntimezone = \"Asia/Shanghai\"\n"
      "[mysql]\npool_size = 8\n[cpu]\nargon2_memory_kb = 65536\n");
  CHECK_EQ(c.server.port, 9000);
  CHECK(c.business.timezone == "Asia/Shanghai");
  CHECK_EQ(c.mysql.pool_size, 8);
}

TNYCL_TEST(config_example_roundtrip) {
  Config c = parse_config(example_config_toml());
  CHECK_EQ(c.server.port, 8080);
  CHECK_EQ(c.server.io_threads, 4);
  CHECK_EQ(c.cpu.argon2_iterations, 3);
  CHECK(c.business.timezone == "Asia/Shanghai");
  CHECK_EQ(c.auth_cache.local_lru_capacity, 10000);
}

TNYCL_TEST(config_parse_bad_syntax_throws) {
  bool threw = false;
  try { parse_config("this is not toml\n"); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

TNYCL_TEST(config_missing_file_throws) {
  bool threw = false;
  try { load_config("__no_such_file__.toml"); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

// ---- 线程池 ----
TNYCL_TEST(thread_pool_executes) {
  ThreadPool pool(4, 256);
  pool.start();
  std::atomic<int> n{0};
  for (int i = 0; i < 100; ++i) pool.submit([&] { n.fetch_add(1); });
  auto barrier = pool.submit([] {});
  if (barrier.valid()) barrier.wait();
  pool.shutdown();
  CHECK_EQ(n.load(), 100);
}

TNYCL_TEST(thread_pool_bounded_queue_rejects) {
  ThreadPool pool(1, 2);
  pool.start();
  std::atomic<bool> started{false}, release{false};
  pool.submit([&] { started = true; while (!release.load()) {} });
  while (!started.load()) { std::this_thread::yield(); }
  auto a = pool.submit([] {});
  auto b = pool.submit([] {});
  auto c = pool.submit([] {});
  CHECK(a.valid());
  CHECK(b.valid());
  CHECK(!c.valid());
  release = true;
  pool.shutdown();
}

// ---- 定时器 ----
TNYCL_TEST(timer_fires) {
  TimerScheduler t;
  t.start();
  std::atomic<int> n{0};
  t.register_task("t1", std::chrono::milliseconds(20), [&] { n.fetch_add(1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  t.stop();
  CHECK(n.load() >= 3);
}

TNYCL_TEST(timer_duplicate_rejected) {
  TimerScheduler t;
  t.start();
  CHECK(t.register_task("x", std::chrono::milliseconds(1000), [] {}));
  CHECK(!t.register_task("x", std::chrono::milliseconds(1000), [] {}));
  t.stop();
}

TNYCL_TEST(timer_unregister) {
  TimerScheduler t;
  CHECK(t.register_task("y", std::chrono::milliseconds(1000), [] {}));
  CHECK(t.unregister_task("y"));
  CHECK(!t.unregister_task("y"));
  t.stop();
}

// ---- 路由 ----
TNYCL_TEST(router_match_static) {
  auto r = make_http_router();
  r->get("/healthz", [](const HttpRequest&, HttpResponse& resp) { resp.status = 200; });
  HttpRequest req;
  req.method = "GET";
  req.path = "/healthz";
  Handler h;
  CHECK(r->match("GET", "/healthz", req, h));
  HttpResponse resp;
  h(req, resp);
  CHECK_EQ(resp.status, 200);
}

TNYCL_TEST(router_path_param) {
  auto r = make_http_router();
  r->get("/v1/foods/{slug}", [](const HttpRequest&, HttpResponse&) {});
  HttpRequest req;
  req.method = "GET";
  req.path = "/v1/foods/lemon-chicken";
  Handler h;
  CHECK(r->match("GET", "/v1/foods/lemon-chicken", req, h));
  CHECK(req.path_params["slug"] == "lemon-chicken");
}

TNYCL_TEST(router_no_match) {
  auto r = make_http_router();
  r->get("/healthz", [](const HttpRequest&, HttpResponse&) {});
  HttpRequest req;
  Handler h;
  CHECK(!r->match("GET", "/unknown", req, h));
  CHECK(!r->match("POST", "/healthz", req, h));
}

TNYCL_TEST(router_query_parse) {
  HttpRequest req;
  req.raw_query = "date=2026-11-12&limit=20";
  auto q = req.query();
  CHECK(q["date"] == "2026-11-12");
  CHECK(q["limit"] == "20");
}

TNYCL_TEST(router_duplicate_rejected) {
  auto r = make_http_router();
  r->get("/x", [](const HttpRequest&, HttpResponse&) {});
  bool threw = false;
  try { r->get("/x", [](const HttpRequest&, HttpResponse&) {}); } catch (const AppError&) { threw = true; }
  CHECK(threw);
}

int main() { return tnycl::test::run_all(); }