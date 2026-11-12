// infra/shutdown.h —— 优雅停机协调器（T-2.08）
//
// 按 v2.3 冻结顺序执行 8 步：
//   ① 停止 accept  ② 标记 shutting_down（新请求 9004）  ③ 等待在途请求（≤5s）
//   ④ 停止 Timer   ⑤ 关闭 CPU/DB 池（排空队列）           ⑥ 关闭连接
//   ⑦ 释放 MySQL/Redis  ⑧ 返回；超时记 forced 并返回 false。
//
// 幂等：重复/并发调用 shutdown() 只执行一次（std::call_once）。
// 在途计数用原子变量；等待用 condition_variable（非 sleep 轮询，满足禁 sleep 检查）。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace tnycl {

class ShutdownCoordinator {
 public:
  struct Config {
    std::chrono::milliseconds in_flight_timeout{5000};
  };
  using Step = std::function<void()>;

  ShutdownCoordinator(Config cfg, Step stop_accept, Step stop_timer,
                      std::vector<Step> drain_steps, Step close_connections,
                      Step close_resources);

  // 执行停机；返回 false 表示在途请求未在期限内排空（强制停机）。
  bool shutdown();

  bool shutting_down() const noexcept { return shutting_down_.load(); }
  int in_flight() const noexcept { return in_flight_.load(); }

  // transport 层在进入/离开请求时调用，维护在途计数。
  void enter_request();
  void exit_request();

 private:
  Config cfg_;
  Step stop_accept_, stop_timer_, close_connections_, close_resources_;
  std::vector<Step> drain_steps_;

  std::once_flag once_;
  bool result_ = false;
  std::atomic<bool> shutting_down_{false};
  std::atomic<int> in_flight_{0};
  std::mutex cv_mu_;
  std::condition_variable cv_;
};

}  // namespace tnycl
