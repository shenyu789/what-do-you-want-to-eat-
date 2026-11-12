// infra/shutdown.h —— 优雅停机协调器（T-2.08）
//
// 严格按 v2.3 八步顺序：
//   ① 停止 accept（关 listen fd）
//   ② 标记 shutting_down（新请求 9004 + Connection: close）
//   ③ 等待在途请求排空（≤ in_flight_timeout，默认 5s）
//   ④ 停止 Timer 线程（join）
//   ⑤ 关闭 CPU/DB 池（停止取新任务，排空队列）
//   ⑥ 关闭连接
//   ⑦ 释放 MySQL/Redis
//   ⑧ 写 task_checkpoint（结算类任务）
//
// 超时策略（安全第一）：③ 超时则**不再执行 ④~⑧**——绝不释放仍被活动请求使用的资源，
// 避免 use-after-free。由调用方记录 shutdown_forced_total 后 _exit(1)（进程终止，OS 回收资源）。
//
// 幂等：std::call_once 保证并发/重复调用只执行一次；超时后再次调用返回同一次结果（不重跑）。
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

  // 参数按八步顺序：stop_accept / stop_timer / drain_steps(池) / close_connections /
  // close_resources / checkpoint。shutting_down 由内部维护（②）。
  ShutdownCoordinator(Config cfg, Step stop_accept, Step stop_timer,
                      std::vector<Step> drain_steps, Step close_connections,
                      Step close_resources, Step checkpoint = {});

  // 执行停机；返回 false 表示在途请求未在期限内排空（强制，未释放资源）。
  bool shutdown();

  bool shutting_down() const noexcept { return shutting_down_.load(); }
  int in_flight() const noexcept { return in_flight_.load(); }

  void enter_request();
  void exit_request();

 private:
  Config cfg_;
  Step stop_accept_, stop_timer_, close_connections_, close_resources_, checkpoint_;
  std::vector<Step> drain_steps_;

  std::once_flag once_;
  bool result_ = false;
  std::atomic<bool> shutting_down_{false};
  std::atomic<int> in_flight_{0};
  std::mutex cv_mu_;
  std::condition_variable cv_;
};

}  // namespace tnycl
