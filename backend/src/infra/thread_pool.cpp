// thread_pool.cpp —— 有界线程池
#include "tnycl/thread_pool.h"

namespace tnycl {

ThreadPool::ThreadPool(std::size_t threads, std::size_t queue_capacity)
    : threads_(threads), queue_capacity_(queue_capacity) {}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_) return;
  started_ = true;
  stopping_ = false;
  workers_.reserve(threads_);
  for (std::size_t i = 0; i < threads_; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& w : workers_)
    if (w.joinable()) w.join();
  workers_.clear();
  std::lock_guard<std::mutex> lk(mu_);
  started_ = false;
}

std::future<void> ThreadPool::submit(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!started_ || stopping_ || tasks_.size() >= queue_capacity_) {
    return {};  // 拒绝：返回未就绪 future（valid()==false）
  }
  std::packaged_task<void()> task(std::move(fn));
  auto fut = task.get_future();
  tasks_.emplace(std::move(task));
  cv_.notify_one();
  return fut;
}

std::size_t ThreadPool::queue_size() const noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  return tasks_.size();
}

void ThreadPool::worker_loop() {
  for (;;) {
    std::packaged_task<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace tnycl
