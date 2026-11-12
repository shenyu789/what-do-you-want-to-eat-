#include "service/account_redis_limiter.h"

#include <string>

namespace tnycl::account {

RedisRateLimiter::RedisRateLimiter(IRedisClient& redis, RateLimiter& fallback)
    : redis_(redis), fallback_(fallback) {}

std::optional<std::int64_t> RedisRateLimiter::deny_retry_after_ms(std::string_view key,
                                                                  int limit,
                                                                  std::int64_t window_ms,
                                                                  std::int64_t now_ms) {
  if (limit <= 0 || window_ms <= 0) {
    return std::nullopt;
  }

  if (redis_.is_available()) {
    const std::int64_t window_start = now_ms - (now_ms % window_ms);
    const std::string redis_key =
        "rl:" + std::string(key) + ":" + std::to_string(window_start);
    const auto count = redis_.incr(redis_key);
    if (count.has_value()) {
      if (*count == 1) {
        // 首次命中设置过期：向上取整到秒并多留 1s，避免边界秒提前丢失计数。
        const int ttl_sec = static_cast<int>((window_ms + 999) / 1000) + 1;
        if (!redis_.expire(redis_key, ttl_sec)) {
          // 过期设置失败则清掉计数，降级本地限流，避免产生永不过期的键。
          redis_.del(redis_key);
          return fallback_.deny_retry_after_ms(key, limit, window_ms, now_ms);
        }
      }
      if (*count > limit) {
        const std::int64_t retry = window_start + window_ms - now_ms;
        return retry > 0 ? retry : 1;
      }
      return std::nullopt;
    }
  }

  return fallback_.deny_retry_after_ms(key, limit, window_ms, now_ms);
}

}  // namespace tnycl::account
