// T-3.06 限流测试：IP 级、用户级、窗口边界、并发、隔离、降级与服务文案。
// 全部基于本地 InMemoryRateLimiter（Redis 版待 A 交付后补集成用例）。
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/profile_service.h"
#include "support/account_fakes.h"
#include "support/profile_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;
using tnycl::account::InMemoryRateLimiter;

std::int64_t retry_ms(const std::string& message) {
  const std::size_t pos = message.find_first_of("0123456789");
  if (pos == std::string::npos) {
    return -1;
  }
  return std::stoll(message.substr(pos));
}

struct AccountWorld {
  tnycl::test::FakeUserRepository users;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup lookup;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeClock clock;
  InMemoryRateLimiter limiter;
  tnycl::account::AuthService auth{tnycl::account::AuthService::Deps{
      users, sessions, lookup, hasher, tokens, clock, limiter, nullptr}};

  void seed(const std::string& username = "alice2026",
            const std::string& password = "Secret123") {
    CHECK(auth.register_user({username, password}).is_ok());
  }

  tnycl::Result<tnycl::account::LoginResponse> login(const std::string& ip,
                                                     const std::string& username = "alice2026",
                                                     const std::string& password = "Secret123") {
    return auth.login({username, password, "dev-a", ip});
  }
};

tnycl::profile::ProfileInput valid_input() {
  tnycl::profile::ProfileInput input;
  input.name = "我";
  input.height_cm = 170;
  input.weight_kg = 60;
  input.age = 28;
  input.goal = tnycl::profile::Goal::kBalanced;
  input.activity = tnycl::profile::Activity::kModerate;
  input.water_ml = 1800;
  input.sleep_hours = 7.0;
  input.note = "";
  return input;
}

struct ProfileWorld {
  tnycl::test::FakeProfileRepository profiles;
  tnycl::test::FakeClock clock;
  InMemoryRateLimiter limiter;
  tnycl::profile::ProfileService service{
      tnycl::profile::ProfileService::Deps{profiles, clock, limiter}};
};

}  // namespace

TNYCL_TEST(LoginIpQuotaAllowsTenPerMinuteThenReturns1006) {
  AccountWorld world;
  world.seed();
  for (int i = 0; i < 10; ++i) {
    CHECK(world.login("203.0.113.7").is_ok());
  }

  const auto denied = world.login("203.0.113.7");
  CHECK(!denied.is_ok());
  CHECK(denied.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::error_code(denied.code()), 1006);
  CHECK_EQ(tnycl::http_status(denied.code()), 429);
  CHECK_EQ(tnycl::error_name(denied.code()), std::string("RATE_LIMITED"));

  const auto ms = retry_ms(denied.message());
  CHECK(ms > 0);
  CHECK(ms <= 60000);
  CHECK(denied.message().find("毫秒") != std::string::npos);
}

TNYCL_TEST(IpQuotaCannotBeBypassedByRotatingUsernames) {
  AccountWorld world;
  world.seed("alice2026");
  world.seed("bob2026");
  for (int i = 0; i < 10; ++i) {
    const std::string username = (i % 2 == 0) ? "alice2026" : "bob2026";
    CHECK(world.login("203.0.113.8", username).is_ok());
  }
  // 换用户名不重置 IP 桶：第 11 次依旧被拒。
  CHECK(world.login("203.0.113.8", "bob2026").code() == ErrorCode::kRateLimited);
  CHECK(world.login("203.0.113.8", "alice2026").code() == ErrorCode::kRateLimited);
  // 换设备标识同样不能绕过（限流键只含客户端 IP）。
  CHECK(world.auth.login({"alice2026", "Secret123", "another-device", "203.0.113.8"}).code() ==
        ErrorCode::kRateLimited);
}

TNYCL_TEST(DifferentClientIpsAreIsolated) {
  AccountWorld world;
  world.seed();
  for (int i = 0; i < 10; ++i) {
    CHECK(world.login("198.51.100.1").is_ok());
  }
  CHECK(world.login("198.51.100.1").code() == ErrorCode::kRateLimited);
  CHECK(world.login("198.51.100.2").is_ok());
  CHECK(world.login("198.51.100.3").is_ok());
}

TNYCL_TEST(WindowBoundaryReleasesQuotaExactlyAfterWindow) {
  AccountWorld world;
  world.seed();
  for (int i = 0; i < 10; ++i) {
    CHECK(world.login("192.0.2.5").is_ok());
  }
  CHECK(world.login("192.0.2.5").code() == ErrorCode::kRateLimited);

  world.clock.now += 59999;
  CHECK(world.login("192.0.2.5").code() == ErrorCode::kRateLimited);

  world.clock.now += 1;
  CHECK(world.login("192.0.2.5").is_ok());
}

TNYCL_TEST(ConcurrentBurstNeverExceedsLimit) {
  InMemoryRateLimiter limiter;
  constexpr int kLimit = 50;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;

  std::atomic<int> allowed{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        if (!limiter.deny_retry_after_ms("burst:key", kLimit, 60000, 1'700'000'000'000)) {
          allowed.fetch_add(1);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_EQ(allowed.load(), kLimit);
  CHECK_EQ(limiter.tracked_keys(), std::size_t{1});
}

TNYCL_TEST(StaleBucketsAreReclaimedAfterWindow) {
  InMemoryRateLimiter limiter;
  constexpr std::int64_t kNow = 1'700'000'000'000;
  for (int i = 0; i < 300; ++i) {
    limiter.deny_retry_after_ms("ip:" + std::to_string(i), 10, 60000, kNow);
  }
  CHECK_EQ(limiter.tracked_keys(), std::size_t{300});

  // 窗口外再产生 256 次调用触发惰性回收，旧键应被清空。
  for (int i = 0; i < 256; ++i) {
    limiter.deny_retry_after_ms("ip:fresh", 10, 60000, kNow + 60001);
  }
  CHECK_EQ(limiter.tracked_keys(), std::size_t{1});
}

TNYCL_TEST(ProfileWriteQuotaIsThirtyPerUserAndIsolatedFromRead) {
  ProfileWorld world;
  const auto input = valid_input();
  for (int i = 0; i < 30; ++i) {
    CHECK(world.service.put(7, input).is_ok());
  }
  const auto denied = world.service.put(7, input);
  CHECK(denied.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::http_status(denied.code()), 429);
  CHECK(retry_ms(denied.message()) > 0);

  // 写配额耗尽不影响同一用户的读；也不影响其他用户的写。
  CHECK(world.service.get(7).is_ok());
  CHECK(world.service.put(8, input).is_ok());
}

TNYCL_TEST(ProfileReadQuotaIs120PerUser) {
  ProfileWorld world;
  CHECK(world.service.put(7, valid_input()).is_ok());
  for (int i = 0; i < 120; ++i) {
    CHECK(world.service.get(7).is_ok());
  }
  const auto denied = world.service.get(7);
  CHECK(denied.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::error_code(denied.code()), 1006);
  CHECK(retry_ms(denied.message()) <= 60000);

  CHECK(world.service.get(8).code() == ErrorCode::kProfileNotFound);
}

TNYCL_TEST(QuotaKeysSeparateActionAndUser) {
  CHECK_EQ(tnycl::account::user_quota_key("profile:read", 7), std::string("profile:read:uid:7"));
  CHECK(tnycl::account::user_quota_key("profile:read", 7) !=
        tnycl::account::user_quota_key("profile:write", 7));
  CHECK(tnycl::account::user_quota_key("profile:read", 7) !=
        tnycl::account::user_quota_key("profile:read", 8));

  InMemoryRateLimiter limiter;
  const std::string key_a = tnycl::account::user_quota_key("profile:read", 7);
  const std::string key_b = tnycl::account::user_quota_key("profile:read", 8);
  for (int i = 0; i < 120; ++i) {
    CHECK(!limiter.deny_retry_after_ms(key_a, 120, 60000, 1'700'000'000'000).has_value());
  }
  CHECK(limiter.deny_retry_after_ms(key_a, 120, 60000, 1'700'000'000'000).has_value());
  CHECK(!limiter.deny_retry_after_ms(key_b, 120, 60000, 1'700'000'000'000).has_value());
}

int main() { return tnycl::test::run_all(); }
