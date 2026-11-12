// Redis 版限流准备阶段单测（T-3.06）。
//
// 重要：使用 FakeRedis（内存替身）验证适配逻辑与降级路径，
// **不是** Redis 集成验收——真实 Redis 未运行、Agent A 的客户端未交付。
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "service/account_rate_limiter.h"
#include "service/account_redis_limiter.h"
#include "support/test_harness.h"

namespace {

using tnycl::account::InMemoryRateLimiter;
using tnycl::account::RedisRateLimiter;

class FakeRedis final : public tnycl::IRedisClient {
 public:
  bool is_available() const noexcept override { return available; }

  std::optional<std::string> get(const std::string&) override { return std::nullopt; }
  bool set(const std::string&, const std::string&, std::optional<int>) override {
    return available;
  }
  std::optional<long long> incr(const std::string& key) override {
    if (!available || fail_incr) {
      return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(mu_);
    return ++counters[key];
  }
  bool del(const std::string& key) override {
    const std::lock_guard<std::mutex> lock(mu_);
    ++del_calls;
    counters.erase(key);
    ttls.erase(key);
    return true;
  }
  bool expire(const std::string& key, int ttl_sec) override {
    if (!available || fail_expire) {
      return false;
    }
    const std::lock_guard<std::mutex> lock(mu_);
    ttls[key] = ttl_sec;
    return true;
  }
  bool zadd(const std::string&, double, const std::string&) override { return true; }
  std::vector<std::string> zrevrange(const std::string&, long, long) override { return {}; }

  bool available{true};
  bool fail_incr{false};
  bool fail_expire{false};
  int del_calls{0};
  std::map<std::string, long long> counters;
  std::map<std::string, int> ttls;

 private:
  std::mutex mu_;
};

constexpr std::int64_t kWindowMs = 60LL * 1000;
constexpr std::int64_t kNow = 1'700'000'000'000;

}  // namespace

TNYCL_TEST(RedisQuotaIsSharedAcrossInstances) {
  FakeRedis redis;
  InMemoryRateLimiter local_a;
  InMemoryRateLimiter local_b;
  RedisRateLimiter instance_a(redis, local_a);
  RedisRateLimiter instance_b(redis, local_b);

  int allowed = 0;
  for (int i = 0; i < 10; ++i) {
    auto& limiter = (i % 2 == 0) ? instance_a : instance_b;
    if (!limiter.deny_retry_after_ms("login:ip:1.2.3.4", 10, kWindowMs, kNow).has_value()) {
      ++allowed;
    }
  }
  CHECK_EQ(allowed, 10);

  // 两个实例看到同一计数：都拒绝，且退避时间一致。
  const auto deny_a = instance_a.deny_retry_after_ms("login:ip:1.2.3.4", 10, kWindowMs, kNow);
  const auto deny_b = instance_b.deny_retry_after_ms("login:ip:1.2.3.4", 10, kWindowMs, kNow);
  CHECK(deny_a.has_value());
  CHECK(deny_b.has_value());
  if (deny_a.has_value() && deny_b.has_value()) {
    CHECK_EQ(*deny_a, *deny_b);
    CHECK(*deny_a >= 1);
    CHECK(*deny_a <= kWindowMs);
  }

  // 只有首个子窗口键，且设置了 >= 窗口长度的过期时间。
  CHECK_EQ(redis.counters.size(), std::size_t{1});
  CHECK_EQ(redis.ttls.size(), std::size_t{1});
  if (!redis.ttls.empty()) {
    CHECK(redis.ttls.begin()->second >= 60);
    CHECK(redis.ttls.begin()->second <= 61);
  }
  CHECK_EQ(local_a.tracked_keys(), std::size_t{0});  // Redis 生效时本地不计数
  CHECK_EQ(local_b.tracked_keys(), std::size_t{0});
}

TNYCL_TEST(RedisUnavailableDegradesToLocalWindow) {
  FakeRedis redis;
  redis.available = false;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);

  for (int i = 0; i < 10; ++i) {
    CHECK(!limiter.deny_retry_after_ms("login:ip:5.6.7.8", 10, kWindowMs, kNow).has_value());
  }
  const auto denied = limiter.deny_retry_after_ms("login:ip:5.6.7.8", 10, kWindowMs, kNow);
  CHECK(denied.has_value());
  if (denied.has_value()) {
    CHECK_EQ(*denied, kWindowMs);
  }
  CHECK_EQ(local.tracked_keys(), std::size_t{1});
  CHECK(redis.counters.empty());
}

TNYCL_TEST(RedisFailureFallsBackWithoutPoisoningCounters) {
  FakeRedis redis;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);

  // INCR 失败：直接走本地窗口。
  redis.fail_incr = true;
  CHECK(!limiter.deny_retry_after_ms("profile:write:uid:7", 2, kWindowMs, kNow).has_value());
  CHECK(!limiter.deny_retry_after_ms("profile:write:uid:7", 2, kWindowMs, kNow).has_value());
  CHECK(limiter.deny_retry_after_ms("profile:write:uid:7", 2, kWindowMs, kNow).has_value());
  CHECK(redis.counters.empty());

  // EXPIRE 失败：清掉刚写入的计数，避免永不过期的键，然后本地降级。
  redis.fail_incr = false;
  redis.fail_expire = true;
  InMemoryRateLimiter local2;
  RedisRateLimiter limiter2(redis, local2);
  CHECK(!limiter2.deny_retry_after_ms("profile:write:uid:8", 1, kWindowMs, kNow).has_value());
  CHECK(redis.del_calls >= 1);
  CHECK(redis.counters.empty());
  // 本地已记 1 次，下一次拒绝。
  const auto denied = limiter2.deny_retry_after_ms("profile:write:uid:8", 1, kWindowMs, kNow);
  CHECK(denied.has_value());
}

TNYCL_TEST(RedisWindowRolloverStartsFreshCounter) {
  FakeRedis redis;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);

  // 固定窗口：同一窗口内用满后拒绝，进入下一窗口立即恢复。
  const std::int64_t window_start = kNow - (kNow % kWindowMs);
  for (int i = 0; i < 3; ++i) {
    CHECK(!limiter.deny_retry_after_ms("food:list:ip", 3, kWindowMs, kNow).has_value());
  }
  CHECK(limiter.deny_retry_after_ms("food:list:ip", 3, kWindowMs, kNow).has_value());
  CHECK(!limiter.deny_retry_after_ms("food:list:ip", 3, kWindowMs,
                                     window_start + kWindowMs).has_value());
  // 新窗口键已建立；旧键保留计数并等待过期（FakeRedis 不模拟时间过期）。
  CHECK_EQ(redis.counters.size(), std::size_t{2});
  for (const auto& [key, ttl] : redis.ttls) {
    CHECK(!key.empty());
    CHECK(ttl > 0);
  }
}

// 并发：INCR 原子性由 Redis 保证，6 线程 × 10 次只能放行 limit 次。
TNYCL_TEST(RedisCounterIsAtomicUnderConcurrency) {
  FakeRedis redis;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);
  constexpr int kThreads = 6;
  constexpr int kAttempts = 10;
  constexpr int kLimit = 30;

  std::atomic<int> allowed{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&limiter, &allowed] {
      for (int j = 0; j < kAttempts; ++j) {
        if (!limiter.deny_retry_after_ms("login:ip:9.9.9.9", kLimit, kWindowMs, kNow)
                 .has_value()) {
          ++allowed;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_EQ(allowed.load(), kLimit);
  CHECK_EQ(redis.counters.size(), std::size_t{1});
  if (!redis.counters.empty()) {
    CHECK_EQ(redis.counters.begin()->second, static_cast<long long>(kThreads * kAttempts));
  }
}

// 恢复：Redis 断连期间完全由本地窗口兜底；恢复后重新走分布式计数。
// 注意（已在报告列为风险）：断连期间的本地计数不迁移到 Redis，
// 恢复瞬间同一窗口内可能出现 本地配额 + Redis 余量 的叠加放量。
TNYCL_TEST(RedisRecoveryRestoresDistributedCounter) {
  FakeRedis redis;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);
  constexpr int kLimit = 5;

  CHECK(!limiter.deny_retry_after_ms("profile:write:uid:9", kLimit, kWindowMs, kNow).has_value());
  CHECK(!limiter.deny_retry_after_ms("profile:write:uid:9", kLimit, kWindowMs, kNow).has_value());

  redis.available = false;
  int degraded_allowed = 0;
  for (int i = 0; i < kLimit; ++i) {
    if (!limiter.deny_retry_after_ms("profile:write:uid:9", kLimit, kWindowMs, kNow).has_value()) {
      ++degraded_allowed;
    }
  }
  CHECK_EQ(degraded_allowed, kLimit);
  CHECK(limiter.deny_retry_after_ms("profile:write:uid:9", kLimit, kWindowMs, kNow).has_value());

  // 恢复：Redis 计数从 2 继续，可再放行 kLimit-2 次。
  redis.available = true;
  int recovered_allowed = 0;
  for (int i = 0; i < kLimit; ++i) {
    if (!limiter.deny_retry_after_ms("profile:write:uid:9", kLimit, kWindowMs, kNow).has_value()) {
      ++recovered_allowed;
    }
  }
  CHECK_EQ(recovered_allowed, kLimit - 2);
}

// 偏差量化：固定窗口在窗口切换瞬间允许最多 2×limit，与本地滑动窗口不同。
// 冻结接口无 ZREMRANGEBYSCORE/ZCARD，无法实现真正的分布式滑动窗口（见报告）。
TNYCL_TEST(FixedWindowAllowsDoubleBurstAtRollover) {
  FakeRedis redis;
  InMemoryRateLimiter local;
  RedisRateLimiter limiter(redis, local);
  constexpr int kLimit = 3;
  const std::int64_t window_start = kNow - (kNow % kWindowMs);

  for (int i = 0; i < kLimit; ++i) {
    CHECK(!limiter.deny_retry_after_ms("food:list:ip:1", kLimit, kWindowMs, window_start)
               .has_value());
  }
  CHECK(limiter.deny_retry_after_ms("food:list:ip:1", kLimit, kWindowMs, window_start)
            .has_value());

  // 窗口结束前 1ms 仍拒绝，退避 1ms。
  const auto tail = limiter.deny_retry_after_ms("food:list:ip:1", kLimit, kWindowMs,
                                                window_start + kWindowMs - 1);
  CHECK(tail.has_value());
  if (tail.has_value()) {
    CHECK_EQ(*tail, std::int64_t{1});
  }

  // 跨入下一窗口立即再放行 kLimit 次 → 任意 60s 区间最多 2×limit。
  for (int i = 0; i < kLimit; ++i) {
    CHECK(!limiter.deny_retry_after_ms("food:list:ip:1", kLimit, kWindowMs,
                                       window_start + kWindowMs)
               .has_value());
  }
}

int main() { return tnycl::test::run_all(); }
