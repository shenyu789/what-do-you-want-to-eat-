// infra/mysql_pool.h —— MySQL 连接池实现（T-2.04）
//
// 实现 IDbPool（冻结契约）：acquire 超时返回 nullopt（→ 9002）、连接 RAII 自动归还、
// shutdown 关闭全部连接。连接工厂注入（生产 = mysql_real_connect；测试 = 伪连接）。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "tnycl/db_pool.h"

namespace tnycl {

class MySqlPool final : public IDbPool {
 public:
  using ConnFactory = std::function<std::unique_ptr<DbConn>()>;

  // max_size：池上限；acquire_timeout_ms：默认获取超时（单次 acquire 可覆盖）。
  MySqlPool(std::size_t max_size, int acquire_timeout_ms, ConnFactory factory);
  ~MySqlPool() override;

  // 获取连接；超时/池满返回 nullptr（调用方映射 9002）。返回的连接析构时自动归还。
  std::unique_ptr<DbConn> acquire(int timeout_ms) override;

  std::size_t idle() const noexcept override;
  std::size_t in_use() const noexcept override;
  bool healthy() override;

  // 关闭：停止放发新连接，回收全部空闲连接；在途连接释放时被丢弃。
  void shutdown();

 private:
  struct State {
    std::size_t max_size;
    ConnFactory factory;
    std::deque<std::unique_ptr<DbConn>> idle;
    std::size_t in_use = 0;
    std::size_t creating = 0;  // 锁外创建中的连接数（防并发创建突破上限）
    std::mutex mu;
    std::condition_variable cv;
    bool stopping = false;
  };

  // 包装为池化连接（析构自动归还/丢弃）。
  std::unique_ptr<DbConn> make_pooled(std::unique_ptr<DbConn> conn);

  std::shared_ptr<State> state_;
  int acquire_timeout_ms_;
};

}  // namespace tnycl
