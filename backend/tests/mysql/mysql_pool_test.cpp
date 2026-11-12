// T-2.04 MySQL 连接池测试（注入伪连接工厂，验证池管理逻辑）
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "infra/mysql_pool.h"
#include "support/test_harness.h"
#include "tnycl/db_pool.h"

using namespace tnycl;

namespace {

struct TestConn final : DbConn {
  static int alive;
  bool valid_ = true;
  TestConn() { ++alive; }
  ~TestConn() override { --alive; }
  Result exec(const std::string&, const std::vector<std::string>&) override {
    return Result{};
  }
  void begin() override {}
  void commit() override {}
  void rollback() override {}
  bool valid() const noexcept override { return valid_; }
  int err_no() const noexcept override { return 0; }
  std::string err_msg() const override { return {}; }
};
int TestConn::alive = 0;

std::unique_ptr<DbConn> make_conn() { return std::make_unique<TestConn>(); }

}  // namespace

TNYCL_TEST(pool_acquire_release) {
  TestConn::alive = 0;
  MySqlPool pool(2, 200, make_conn);
  {
    auto c = pool.acquire(200);
    CHECK(c != nullptr);
    CHECK_EQ(pool.in_use(), 1);
    CHECK_EQ(pool.idle(), 0);
  }  // 析构 → 归还
  CHECK_EQ(pool.idle(), 1);
  CHECK_EQ(pool.in_use(), 0);
}

TNYCL_TEST(pool_max_size_and_timeout) {
  TestConn::alive = 0;
  MySqlPool pool(1, 50, make_conn);
  auto c1 = pool.acquire(50);
  CHECK(c1 != nullptr);
  auto c2 = pool.acquire(50);  // 池满且无空闲 → 超时返回 nullptr
  CHECK(c2 == nullptr);
}

TNYCL_TEST(pool_invalid_conn_discarded) {
  TestConn::alive = 0;
  bool make_invalid = true;
  MySqlPool pool(2, 200, [&] {
    auto c = std::make_unique<TestConn>();
    c->valid_ = !make_invalid;
    return c;
  });
  {
    auto c = pool.acquire(200);
    CHECK(c != nullptr);
  }  // valid()==false → 丢弃不归还
  CHECK_EQ(pool.idle(), 0);
}

TNYCL_TEST(pool_shutdown) {
  TestConn::alive = 0;
  MySqlPool pool(1, 200, make_conn);
  auto c = pool.acquire(200);
  CHECK(c != nullptr);
  pool.shutdown();
  c.reset();  // 释放 → stopping → 丢弃
  CHECK_EQ(pool.idle(), 0);
  CHECK(pool.acquire(50) == nullptr);
  CHECK_EQ(TestConn::alive, 0);  // 连接已全部销毁，无泄漏
}

TNYCL_TEST(pool_concurrent_no_overcreate) {
  TestConn::alive = 0;
  const int max = 4;
  std::atomic<int> factory_calls{0};
  MySqlPool pool(max, 500, [&] {
    factory_calls.fetch_add(1);
    return std::make_unique<TestConn>();
  });

  const int threads = 8, iters = 50;
  std::atomic<int> got{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&] {
      for (int i = 0; i < iters; ++i) {
        auto c = pool.acquire(500);
        if (c) got.fetch_add(1);
      }
    });
  }
  for (auto& t : ts) t.join();

  CHECK_EQ(got.load(), threads * iters);  // 每次都应拿到（有归还）
  CHECK(pool.in_use() == 0);
  CHECK(pool.idle() <= max);                    // 池上限不被突破
  CHECK(factory_calls.load() <= max);           // 并发创建被 creating 计数抑制，不超上限
  CHECK_EQ(TestConn::alive, static_cast<int>(pool.idle()));  // 无泄漏
}

int main() { return tnycl::test::run_all(); }
