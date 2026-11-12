// T-3.03 / T-3.04 并发与会话边界测试：同名并发注册、多端登录、登出幂等。
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "support/account_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;

// 生产仓储依赖 uk_username 保证唯一；这里用加锁的假实现模拟同等语义。
class ThreadSafeUserRepository final : public tnycl::account::UserRepository {
 public:
  std::optional<tnycl::account::UserRecord> find_by_username(std::string_view username) override {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& [id, user] : users_) {
      if (user.username == username) {
        return user;
      }
    }
    return std::nullopt;
  }

  std::optional<tnycl::account::UserRecord> find_by_id(std::uint64_t user_id) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = users_.find(user_id);
    if (it == users_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  tnycl::Result<std::uint64_t> insert(std::string_view username, std::string_view pass_hash,
                                      std::int64_t now_ms) override {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& [id, user] : users_) {
      if (user.username == username) {
        return tnycl::Result<std::uint64_t>::err(ErrorCode::kUsernameTaken);
      }
    }
    const std::uint64_t id = ++next_id_;
    tnycl::account::UserRecord record;
    record.user_id = id;
    record.username = std::string(username);
    record.pass_hash = std::string(pass_hash);
    record.created_at_ms = now_ms;
    users_[id] = record;
    return tnycl::Result<std::uint64_t>::ok(id);
  }

  tnycl::Result<void> update_login_state(std::uint64_t user_id, int fail_count,
                                         std::optional<std::int64_t> locked_until_ms) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = users_.find(user_id);
    if (it == users_.end()) {
      return tnycl::Result<void>::err(ErrorCode::kInternalError, "用户不存在");
    }
    it->second.fail_count = fail_count;
    it->second.locked_until_ms = locked_until_ms;
    return tnycl::Result<void>::ok();
  }

  std::size_t size() {
    std::lock_guard<std::mutex> guard(mutex_);
    return users_.size();
  }

 private:
  std::mutex mutex_;
  std::uint64_t next_id_{0};
  std::map<std::uint64_t, tnycl::account::UserRecord> users_;
};

struct World {
  ThreadSafeUserRepository users;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup lookup;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeClock clock;
  tnycl::account::InMemoryRateLimiter limiter;
  tnycl::account::AuthService auth{tnycl::account::AuthService::Deps{
      users, sessions, lookup, hasher, tokens, clock, limiter, nullptr}};
};

}  // namespace

TNYCL_TEST(ConcurrentSameUsernameRegistrationOnlyOneSucceeds) {
  World world;
  constexpr int kThreads = 8;
  std::atomic<int> succeeded{0};
  std::atomic<int> conflicted{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&] {
      const auto created = world.auth.register_user({"shared2026", "Secret123"});
      if (created.is_ok()) {
        succeeded.fetch_add(1);
      } else if (created.code() == ErrorCode::kUsernameTaken) {
        conflicted.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_EQ(succeeded.load(), 1);
  CHECK_EQ(conflicted.load(), kThreads - 1);
  CHECK_EQ(world.users.size(), std::size_t{1});
}

TNYCL_TEST(LoginOnTwoDevicesKeepsBothSessions) {
  World world;
  CHECK(world.auth.register_user({"alice2026", "Secret123"}).is_ok());

  const auto phone = world.auth.login({"alice2026", "Secret123", "phone", "203.0.113.31"});
  const auto table = world.auth.login({"alice2026", "Secret123", "tablet", "203.0.113.31"});
  CHECK(phone.is_ok());
  CHECK(table.is_ok());
  CHECK(phone.value().token != table.value().token);
  CHECK_EQ(world.sessions.sessions_.size(), std::size_t{2});

  CHECK(world.auth.authenticate(phone.value().token).is_ok());
  CHECK(world.auth.authenticate(table.value().token).is_ok());

  // 单端登出只影响该端。
  CHECK(world.auth.logout(phone.value().token).is_ok());
  CHECK(world.auth.authenticate(phone.value().token).code() == ErrorCode::kTokenInvalid);
  CHECK(world.auth.authenticate(table.value().token).is_ok());
}

TNYCL_TEST(LogoutIsIdempotent) {
  World world;
  CHECK(world.auth.register_user({"alice2026", "Secret123"}).is_ok());
  const auto logged = world.auth.login({"alice2026", "Secret123", "phone", "203.0.113.32"});
  CHECK(logged.is_ok());

  CHECK(world.auth.logout(logged.value().token).is_ok());
  CHECK(world.auth.logout(logged.value().token).is_ok());
  CHECK(world.auth.logout("never-issued-token").is_ok());
  CHECK(world.auth.authenticate(logged.value().token).code() == ErrorCode::kTokenInvalid);
}

TNYCL_TEST(SessionExpiryIsSevenDaysFromLogin) {
  World world;
  CHECK(world.auth.register_user({"alice2026", "Secret123"}).is_ok());
  const auto logged = world.auth.login({"alice2026", "Secret123", "phone", "203.0.113.33"});
  CHECK(logged.is_ok());
  CHECK_EQ(logged.value().expires_at_ms,
           world.clock.now_ms() + tnycl::account::kSessionTtlMs);
  CHECK_EQ(tnycl::account::kSessionTtlMs, std::int64_t{7} * 24 * 60 * 60 * 1000);
}

int main() { return tnycl::test::run_all(); }
