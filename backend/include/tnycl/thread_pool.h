// tnycl/thread_pool.h —— 通用有界线程池（CPU 池 / DB 池共用基座，对应 T-2.03）
//
// 语义约定：
//  - 有界队列：队列满时 submit 立即返回失败（不阻塞、不无界排队）。
//  - 线程安全；任务在池线程内执行，返回值经 std::future 取回。
//  - 析构（shutdown）停止取新任务，等待队列排空后 join（优雅停机步骤 5 复用此语义）。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace tnycl {

class ThreadPool {
 public:
  // 构造但不启动。需调用 start() 后再提交。
  explicit ThreadPool(std::size_t threads, std::size_t queue_capacity);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void start();
  void shutdown();  // 停止取新任务，等待在途与队列排空后 join

  // 提交任务。队列满时返回「未就绪的 future」（valid()==false），调用方映射为拒绝错误码。
  // 提交者不得阻塞等待，也不得在池线程内再 submit 阻塞（避免死锁）。
  std::future<void> submit(std::function<void()> fn);

  std::size_t queue_size() const noexcept;
  std::size_t thread_count() const noexcept { return threads_; }
  bool stopping() const noexcept { return stopping_; }

 private:
  void worker_loop();

  std::size_t threads_;
  std::size_t queue_capacity_;
  std::vector<std::thread> workers_;
  std::queue<std::packaged_task<void()>> tasks_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  bool started_ = false;
};

}  // namespace tnycl
