// tnycl/timer.h —— 单 Timer 线程 + 最小堆的周期任务调度（对应 T-2.06 / 02 §3.2）
//
// 语义约定：
//  - 全局唯一 Timer 线程；所有周期任务（会话过期清理、周结算、榜单重建、对账）只由此线程触发。
//  - 任务注册表全局唯一：同一 task_id 重复注册返回 false，避免「每个 IO 线程各跑一遍」。
//  - 最小堆 O(log n)，n ≤ 10；执行完按周期重新入堆。
//  - 不使用 sleep 轮询（CI 禁 sleep）；等待用条件变量（Linux 构建可换 timerfd 集成为 epoll）。
//  - 崩溃补算：由调用方（结算/对账）写 task_checkpoint；本类提供 next_due 供补算判断。
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tnycl {

using TimerCallback = std::function<void()>;

class TimerScheduler {
 public:
  TimerScheduler();
  ~TimerScheduler();

  TimerScheduler(const TimerScheduler&) = delete;
  TimerScheduler& operator=(const TimerScheduler&) = delete;

  void start();  // 启动唯一 Timer 线程
  void stop();   // 停止线程并 join（优雅停机步骤 4）

  // 注册周期任务。task_id 全局唯一，重复注册返回 false。
  // interval 为周期；首次触发在 start 后一个周期。
  bool register_task(const std::string& task_id,
                     std::chrono::milliseconds interval, TimerCallback cb);

  // 移除任务（停止后再调用无效）。返回是否存在。
  bool unregister_task(const std::string& task_id);

  // 已注册任务数。
  std::size_t task_count() const noexcept;

 private:
  struct Entry {
    std::string task_id;
    std::chrono::milliseconds interval;
    TimerCallback cb;
    std::chrono::steady_clock::time_point next_due;
  };
  // 最小堆按 next_due 排序（用 vector + 手动下滤；任务数小，简单实现即可）。
  struct Greater {
    bool operator()(const Entry& a, const Entry& b) const {
      return a.next_due > b.next_due;
    }
  };

  void loop();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Entry> heap_;  // 最小堆
  std::thread thread_;
  bool running_ = false;
};

}  // namespace tnycl
