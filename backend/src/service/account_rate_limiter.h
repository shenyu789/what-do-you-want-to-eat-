// T-3.06 本地滑动窗口限流（Redis 不可用时的降级实现，单机精确、多机不准）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "service/account_repository.h"

namespace tnycl::account {

// 07 §5 限流配额（唯一权威）：写接口 30/分钟/用户，读接口 120/分钟/用户。
inline constexpr int kWriteQuotaPerMinute = 30;
inline constexpr int kReadQuotaPerMinute = 120;
inline constexpr std::int64_t kUserQuotaWindowMs = 60LL * 1000;

// 限流键：动作 + 会话 uid，保证不同接口、不同用户互不干扰。
std::string user_quota_key(std::string_view action, std::uint64_t user_id);

class InMemoryRateLimiter final : public RateLimiter {
 public:
  std::optional<std::int64_t> deny_retry_after_ms(std::string_view key, int limit,
                                                 std::int64_t window_ms,
                                                 std::int64_t now_ms) override;

  // 观测口径：当前仍在跟踪的限流键数量（用于容量回收测试与 /metrics 暴露）。
  std::size_t tracked_keys() const;

  // 键容量上限：超过后按窗口惰性回收，避免任意 key 造成内存无界增长。
  static constexpr std::size_t kMaxTrackedKeys = 100'000;
  static constexpr std::uint64_t kSweepInterval = 256;

 private:
  void sweep_locked(std::int64_t now_ms);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::deque<std::int64_t>> buckets_;
  std::uint64_t calls_{0};
};

}  // namespace tnycl::account
