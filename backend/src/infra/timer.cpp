// timer.cpp —— 单 Timer 线程 + 最小堆 + 条件变量（不轮询、无 sleep）
#include "tnycl/timer.h"

#include <algorithm>

namespace tnycl {

TimerScheduler::TimerScheduler() = default;

TimerScheduler::~TimerScheduler() { stop(); }

void TimerScheduler::start() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
  }
  thread_ = std::thread([this] { loop(); });
}

void TimerScheduler::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return;
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool TimerScheduler::register_task(const std::string& task_id,
                                   std::chrono::milliseconds interval,
                                   TimerCallback cb) {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& e : heap_)
    if (e.task_id == task_id) return false;  // 全局唯一，拒绝重复注册
  Entry e;
  e.task_id = task_id;
  e.interval = interval;
  e.cb = std::move(cb);
  e.next_due = std::chrono::steady_clock::now() + interval;
  heap_.push_back(std::move(e));
  std::push_heap(heap_.begin(), heap_.end(), Greater{});
  cv_.notify_all();  // 可能插入了更早的截止时间，唤醒重算
  return true;
}

bool TimerScheduler::unregister_task(const std::string& task_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = std::find_if(heap_.begin(), heap_.end(),
                         [&](const Entry& e) { return e.task_id == task_id; });
  if (it == heap_.end()) return false;
  heap_.erase(it);
  std::make_heap(heap_.begin(), heap_.end(), Greater{});
  return true;
}

std::size_t TimerScheduler::task_count() const noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  return heap_.size();
}

void TimerScheduler::loop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (running_) {
    if (heap_.empty()) {
      cv_.wait(lk, [this] { return !running_ || !heap_.empty(); });
      continue;
    }
    auto now = std::chrono::steady_clock::now();
    if (heap_.front().next_due <= now) {
      // 摘取所有到期任务
      std::vector<Entry> due;
      while (!heap_.empty() && heap_.front().next_due <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), Greater{});
        due.push_back(std::move(heap_.back()));
        heap_.pop_back();
      }
      // 锁外执行回调（此时 due 中 cb 仍有效；回调可再注册/注销任务）
      lk.unlock();
      for (auto& e : due)
        if (e.cb) e.cb();
      lk.lock();
      // 重新入堆（相对周期）
      auto rearm = std::chrono::steady_clock::now();
      for (auto& e : due) {
        e.next_due = rearm + e.interval;
        heap_.push_back(std::move(e));
        std::push_heap(heap_.begin(), heap_.end(), Greater{});
      }
      continue;
    }
    // 等到堆顶截止时间；期间有新更早任务注册会被 notify 唤醒重算。
    cv_.wait_until(lk, heap_.front().next_due);
  }
}

}  // namespace tnycl
