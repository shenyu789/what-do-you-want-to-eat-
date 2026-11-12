#include "service/account_rate_limiter.h"

namespace tnycl::account {

std::string user_quota_key(std::string_view action, std::uint64_t user_id) {
  return std::string(action) + ":uid:" + std::to_string(user_id);
}

std::optional<std::int64_t> InMemoryRateLimiter::deny_retry_after_ms(std::string_view key,
                                                                    int limit,
                                                                    std::int64_t window_ms,
                                                                    std::int64_t now_ms) {
  if (limit <= 0 || window_ms <= 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  // 惰性回收：按调用次数周期性清理过期键，键容量超限时强制回收，
  // 避免任意 key 造成内存无界增长。现有调用方窗口均为 60s，回收水位取该值。
  // 先回收再取当前键，防止回收把当前键的桶删掉后引用失效。
  if (++calls_ % kSweepInterval == 0 || buckets_.size() > kMaxTrackedKeys) {
    sweep_locked(now_ms);
  }
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

std::size_t InMemoryRateLimiter::tracked_keys() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return buckets_.size();
}

void InMemoryRateLimiter::sweep_locked(std::int64_t now_ms) {
  const std::int64_t cutoff = now_ms - kUserQuotaWindowMs;
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    auto& hits = it->second;
    while (!hits.empty() && hits.front() <= cutoff) {
      hits.pop_front();
    }
    if (hits.empty()) {
      it = buckets_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace tnycl::account
