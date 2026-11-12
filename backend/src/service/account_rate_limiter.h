// T-3.06 本地滑动窗口限流（Redis 不可用时的降级实现，单机精确、多机不准）。
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "service/account_repository.h"

namespace tnycl::account {

class InMemoryRateLimiter final : public RateLimiter {
 public:
  std::optional<std::int64_t> deny_retry_after_ms(std::string_view key, int limit,
                                                 std::int64_t window_ms,
                                                 std::int64_t now_ms) override;

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::deque<std::int64_t>> buckets_;
};

}  // namespace tnycl::account
