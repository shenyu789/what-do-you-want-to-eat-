// T-2.08 优雅停机测试：八步顺序 / 幂等 / 在途等待 / 超时不释放资源 / 重复停机 / 并发安全
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "infra/shutdown.h"
#include "support/test_harness.h"

using namespace tnycl;

TNYCL_TEST(shutdown_eight_steps_in_order) {
  std::vector<std::string> seq;
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(100);
  ShutdownCoordinator sc(
      cfg, [&] { seq.push_back("accept"); }, [&] { seq.push_back("timer"); },
      std::vector<ShutdownCoordinator::Step>{[&] { seq.push_back("drain"); }},
      [&] { seq.push_back("conn"); }, [&] { seq.push_back("res"); },
      [&] { seq.push_back("checkpoint"); });

  bool ok = sc.shutdown();
  CHECK(ok);
  CHECK_EQ(seq.size(), 6);
  CHECK(seq[0] == "accept");
  CHECK(seq[1] == "timer");
  CHECK(seq[2] == "drain");
  CHECK(seq[3] == "conn");
  CHECK(seq[4] == "res");
  CHECK(seq[5] == "checkpoint");  // ⑧ 最后一步
  CHECK(sc.shutting_down());
}

TNYCL_TEST(shutdown_runs_phases_once) {
  std::atomic<int> accept{0}, timer{0}, drain{0}, conn{0}, res{0}, ckpt{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(100);
  ShutdownCoordinator sc(
      cfg, [&] { accept.fetch_add(1); }, [&] { timer.fetch_add(1); },
      std::vector<ShutdownCoordinator::Step>{[&] { drain.fetch_add(1); }},
      [&] { conn.fetch_add(1); }, [&] { res.fetch_add(1); },
      [&] { ckpt.fetch_add(1); });

  bool r1 = sc.shutdown();
  bool r2 = sc.shutdown();
  CHECK(r1);
  CHECK(r2);
  CHECK_EQ(accept.load(), 1);
  CHECK_EQ(timer.load(), 1);
  CHECK_EQ(drain.load(), 1);
  CHECK_EQ(conn.load(), 1);
  CHECK_EQ(res.load(), 1);
  CHECK_EQ(ckpt.load(), 1);
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
  CHECK(ok);
  CHECK_EQ(sc.in_flight(), 0);
  CHECK_EQ(conn.load(), 1);
}

TNYCL_TEST(shutdown_timeout_does_not_release_resources) {
  std::atomic<int> timer{0}, res{0}, ckpt{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(50);
  ShutdownCoordinator sc(cfg, {}, [&] { timer.fetch_add(1); }, {}, {},
                         [&] { res.fetch_add(1); }, [&] { ckpt.fetch_add(1); });
  sc.enter_request();  // 永不退出 → 在途无法排空
  bool ok = sc.shutdown();
  CHECK(!ok);
  // 关键安全语义：超时后**不释放资源**（避免活动请求 use-after-free）
  CHECK_EQ(timer.load(), 0);
  CHECK_EQ(res.load(), 0);
  CHECK_EQ(ckpt.load(), 0);
}

TNYCL_TEST(shutdown_timeout_then_repeat_returns_same) {
  std::atomic<int> res{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(50);
  ShutdownCoordinator sc(cfg, {}, {}, {}, {}, [&] { res.fetch_add(1); });
  sc.enter_request();
  bool r1 = sc.shutdown();
  bool r2 = sc.shutdown();  // 超时后再次调用 → 同结果，不重跑
  CHECK(!r1);
  CHECK(!r2);
  CHECK_EQ(res.load(), 0);
}

TNYCL_TEST(shutdown_concurrent_safe) {
  std::atomic<int> accept{0};
  ShutdownCoordinator::Config cfg;
  cfg.in_flight_timeout = std::chrono::milliseconds(100);
  ShutdownCoordinator sc(cfg, [&] { accept.fetch_add(1); }, {}, {}, {}, {});
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; ++i) ts.emplace_back([&] { sc.shutdown(); });
  for (auto& t : ts) t.join();
  CHECK_EQ(accept.load(), 1);
}

int main() { return tnycl::test::run_all(); }
