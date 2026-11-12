#include "service/account_rate_limiter.h"

namespace tnycl::account {

std::optional<std::int64_t> InMemoryRateLimiter::deny_retry_after_ms(std::string_view key,
                                                                    int limit,
                                                                    std::int64_t window_ms,
                                                                    std::int64_t now_ms) {
  if (limit <= 0 || window_ms <= 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  auto& hits = buckets_[std::string(key)];
  const std::int64_t window_start = now_ms - window_ms;
  while (!hits.empty() && hits.front() <= window_start) {
    hits.pop_front();
  }
  if (static_cast<int>(hits.size()) >= limit) {
    const std::int64_t retry = hits.front() + window_ms - now_ms;
    return retry > 0 ? retry : 1;
  }
  hits.push_back(now_ms);
  return std::nullopt;
}

}  // namespace tnycl::account
