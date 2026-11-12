// infra/shutdown.cpp —— 优雅停机协调器实现（T-2.08）
#include "infra/shutdown.h"

namespace tnycl {

ShutdownCoordinator::ShutdownCoordinator(Config cfg, Step stop_accept,
                                         Step stop_timer,
                                         std::vector<Step> drain_steps,
                                         Step close_connections,
                                         Step close_resources, Step checkpoint)
    : cfg_(cfg),
      stop_accept_(std::move(stop_accept)),
      stop_timer_(std::move(stop_timer)),
      close_connections_(std::move(close_connections)),
      close_resources_(std::move(close_resources)),
      checkpoint_(std::move(checkpoint)),
      drain_steps_(std::move(drain_steps)) {}

bool ShutdownCoordinator::shutdown() {
  // std::call_once：并发/重复调用只执行一次，且其他线程阻塞到执行完成。
  std::call_once(once_, [this] {
    if (stop_accept_) stop_accept_();      // ① 停止 accept
    shutting_down_.store(true);            // ② 新请求由此拒绝（9004）

    // ③ 等待在途请求排空（≤ in_flight_timeout）
    {
      std::unique_lock<std::mutex> lk(cv_mu_);
      result_ = cv_.wait_for(lk, cfg_.in_flight_timeout,
                             [this] { return in_flight_.load() == 0; });
    }
    if (!result_) {
      // 超时：在途请求未排空。安全起见**不释放资源**（避免活动请求 use-after-free），
      // 直接返回，由调用方记录 forced 指标并 _exit(1)。
      return;
    }

    if (stop_timer_) stop_timer_();        // ④ 停止定时任务（join，回调不再执行）
    for (auto& s : drain_steps_)           // ⑤ 排空线程池队列（join，任务不再执行）
      if (s) s();
    if (close_connections_) close_connections_();  // ⑥ 关闭连接
    if (close_resources_) close_resources_();      // ⑦ 释放 MySQL/Redis
    if (checkpoint_) checkpoint_();                // ⑧ 写 task_checkpoint
  });
  return result_;
}

void ShutdownCoordinator::enter_request() { in_flight_.fetch_add(1); }

void ShutdownCoordinator::exit_request() {
  if (in_flight_.fetch_sub(1) == 1) {  // 从 1 → 0
    cv_.notify_all();
  }
}

}  // namespace tnycl
