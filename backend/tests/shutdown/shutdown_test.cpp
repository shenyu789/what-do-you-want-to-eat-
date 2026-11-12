// T-2.08 优雅停机测试：阶段顺序 / 幂等 / 在途等待 / 超时 / 并发安全
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "infra/shutdown.h"
#include "support/test_harness.h"

using namespace tnycl;

TNYCL_TEST(shutdown_runs_phases_once) {
  std::atomic<int> accept{0}, timer{0}, drain{0}, conn{0}, res{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(100);
  ShutdownCoordinator sc(
      cfg, [&] { accept.fetch_add(1); }, [&] { timer.fetch_add(1); },
      std::vector<ShutdownCoordinator::Step>{[&] { drain.fetch_add(1); }},
      [&] { conn.fetch_add(1); }, [&] { res.fetch_add(1); });

  bool r1 = sc.shutdown();
  bool r2 = sc.shutdown();  // 重复调用 → 不重跑
  CHECK(r1);
  CHECK(r2);
  CHECK_EQ(accept.load(), 1);
  CHECK_EQ(timer.load(), 1);
  CHECK_EQ(drain.load(), 1);
  CHECK_EQ(conn.load(), 1);
  CHECK_EQ(res.load(), 1);
  CHECK(sc.shutting_down());
}

TNYCL_TEST(shutdown_waits_in_flight) {
  std::atomic<int> conn{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(1000);
  ShutdownCoordinator sc(cfg, {}, {}, {}, [&] { conn.fetch_add(1); }, {});
  sc.enter_request();
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    sc.exit_request();
  });
  bool ok = sc.shutdown();
  t.join();
  CHECK(ok);  // 在途排空 → 正常停机
  CHECK_EQ(sc.in_flight(), 0);
  CHECK_EQ(conn.load(), 1);  // 关闭连接仍执行
}

TNYCL_TEST(shutdown_timeout_returns_false) {
  std::atomic<int> res{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(50);
  ShutdownCoordinator sc(cfg, {}, {}, {}, {}, [&] { res.fetch_add(1); });
  sc.enter_request();  // 永不退出 → 在途无法排空
  bool ok = sc.shutdown();
  CHECK(!ok);            // 超时 → false（强制停机）
  CHECK_EQ(res.load(), 1);  // 但资源仍被释放
}

TNYCL_TEST(shutdown_concurrent_safe) {
  std::atomic<int> accept{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(100);
  ShutdownCoordinator sc(cfg, [&] { accept.fetch_add(1); }, {}, {}, {}, {});
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; ++i) ts.emplace_back([&] { sc.shutdown(); });
  for (auto& t : ts) t.join();
  CHECK_EQ(accept.load(), 1);  // 只执行一次
}

int main() { return tnycl::test::run_all(); }
