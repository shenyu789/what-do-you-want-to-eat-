// infra/mysql_pool.cpp —— MySQL 连接池实现（T-2.04）
//
// 并发设计：
//  - 空闲归还 / 新建占位 / 等待归还 全部在 state->mu 下原子判定；
//  - 连接工厂 factory() 在锁外调用（慢连接不阻塞池内 acquire/release）；
//  - creating 计数防止"并发新建"突破 max_size；factory 抛异常向上传播（不吞错），
//    creating 在 catch 中回滚；
//  - shutdown 与在途新建竞争：重新拿锁后若 stopping 则丢弃新建连接返回 nullptr。
#include "infra/mysql_pool.h"

#include <chrono>

namespace tnycl {
namespace {

class PooledConn final : public DbConn {
 public:
  PooledConn(std::unique_ptr<DbConn> inner,
             std::function<void(std::unique_ptr<DbConn>)> on_release)
      : inner_(std::move(inner)), on_release_(std::move(on_release)) {}
  ~PooledConn() override {
    if (inner_ && on_release_) on_release_(std::move(inner_));
  }

  Result exec(const std::string& sql,
              const std::vector<std::string>& params) override {
    return inner_->exec(sql, params);
  }
  void begin() override { inner_->begin(); }
  void commit() override { inner_->commit(); }
  void rollback() override { inner_->rollback(); }
  bool valid() const noexcept override { return inner_->valid(); }
  int err_no() const noexcept override { return inner_->err_no(); }
  std::string err_msg() const override { return inner_->err_msg(); }

 private:
  std::unique_ptr<DbConn> inner_;
  std::function<void(std::unique_ptr<DbConn>)> on_release_;
};

}  // namespace

MySqlPool::MySqlPool(std::size_t max_size, int acquire_timeout_ms,
                     ConnFactory factory)
    : state_(std::make_shared<State>()),
      acquire_timeout_ms_(acquire_timeout_ms) {
  state_->max_size = max_size;
  state_->factory = std::move(factory);
}

MySqlPool::~MySqlPool() { shutdown(); }

std::unique_ptr<DbConn> MySqlPool::make_pooled(std::unique_ptr<DbConn> conn) {
  auto state = state_;
  return std::make_unique<PooledConn>(
      std::move(conn), [state](std::unique_ptr<DbConn> c) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (!state->stopping && c && c->valid() &&
            state->idle.size() < state->max_size) {
          state->idle.push_back(std::move(c));
        }
        if (state->in_use > 0) --state->in_use;
        state->cv.notify_one();
      });
}

std::unique_ptr<DbConn> MySqlPool::acquire(int timeout_ms) {
  auto state = state_;
  if (timeout_ms <= 0) timeout_ms = acquire_timeout_ms_;

  std::unique_lock<std::mutex> lk(state->mu);
  if (state->stopping) return nullptr;

  // 快路径：有空闲。
  if (!state->idle.empty()) {
    auto conn = std::move(state->idle.front());
    state->idle.pop_front();
    ++state->in_use;
    return make_pooled(std::move(conn));
  }

  // 可新建：占位 creating，出锁创建。
  if (state->in_use + state->idle.size() + state->creating < state->max_size) {
    ++state->creating;
    lk.unlock();
    std::unique_ptr<DbConn> conn;
    try {
      conn = state->factory();  // 锁外创建，异常向上抛（不吞错）
    } catch (...) {
      lk.lock();
      --state->creating;
      throw;
    }
    lk.lock();
    --state->creating;
    if (state->stopping) return nullptr;  // 停机竞争：丢弃新建连接
    ++state->in_use;
    return make_pooled(std::move(conn));
  }

  // 池满：等待归还（超时）。
  bool ok = state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
    return state->stopping || !state->idle.empty();
  });
  if (!ok || state->stopping) return nullptr;
  auto conn = std::move(state->idle.front());
  state->idle.pop_front();
  ++state->in_use;
  return make_pooled(std::move(conn));
}

std::size_t MySqlPool::idle() const noexcept {
  std::lock_guard<std::mutex> lk(state_->mu);
  return state_->idle.size();
}

std::size_t MySqlPool::in_use() const noexcept {
  std::lock_guard<std::mutex> lk(state_->mu);
  return state_->in_use;
}

bool MySqlPool::healthy() {
  auto conn = acquire(acquire_timeout_ms_);
  if (!conn) return false;
  return conn->valid();
}

void MySqlPool::shutdown() {
  auto state = state_;
  std::lock_guard<std::mutex> lk(state->mu);
  if (state->stopping) return;
  state->stopping = true;
  state->idle.clear();
  state->cv.notify_all();
}

}  // namespace tnycl
