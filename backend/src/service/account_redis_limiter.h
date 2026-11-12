// Redis 版限流（T-3.06 预留实现）：固定窗口 INCR + EXPIRE。
//
// 状态：**仅完成准备与单测**。Agent A 的 IRedisClient 实现尚未交付，
// 本文件只依赖 tnycl/redis_client.h 冻结接口，未与真实 Redis 联调，
// 不得据此宣称 Redis 集成验收通过。
//
// 语义：
//  - Redis 可用时以 "rl:<key>:<窗口起点>" 计数，多实例共享同一计数；
//  - Redis 不可用 / INCR 失败 / EXPIRE 失败 → 降级到本地滑动窗口实现（单机精确）；
//  - 已知偏差 A：Redis 路径是**固定窗口**，窗口切换瞬间最多允许 2×limit；
//    冻结的 IRedisClient 无 ZCARD/ZREMRANGEBYSCORE，无法实现分布式滑动窗口，
//    如需精确滑动窗口须由 A 扩展接口（本文件不擅自修改共享契约）。
//  - 已知偏差 B：断连期间的本地计数不迁移回 Redis，恢复瞬间同一窗口内
//    可能出现"本地配额 + Redis 余量"的叠加放量；降级只保证单机不超限。
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "service/account_repository.h"
#include "tnycl/redis_client.h"

namespace tnycl::account {

class RedisRateLimiter final : public RateLimiter {
 public:
  RedisRateLimiter(IRedisClient& redis, RateLimiter& fallback);

  std::optional<std::int64_t> deny_retry_after_ms(std::string_view key, int limit,
                                                  std::int64_t window_ms,
                                                  std::int64_t now_ms) override;

 private:
  IRedisClient& redis_;
  RateLimiter& fallback_;
};

}  // namespace tnycl::account
