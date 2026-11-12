// 错误响应结构化字段契约测试（T-3.06 / 07 §5）。
//
// 本文件只验证"服务层产出的错误可被 transport 解析出结构化字段"，
// 不修改 Agent A 的 Result / ApiResponse / transport：
//   1006 kRateLimited   → retry_after_ms  （int64，毫秒，1..窗口上限）
//   2003 kAccountLocked → unlock_at       （int64，epoch 毫秒）
//   3002 kProfileInvalid→ field           （string，档案字段名白名单）
// 当前实现把这些字段编码在 message 中（Result 尚无 details 字段），
// 因此这里按 transport 需要的解析规则断言其可解析性与边界值。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/profile_service.h"
#include "support/account_fakes.h"
#include "support/profile_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;
using tnycl::account::AuthService;
using tnycl::account::LoginRequest;
using tnycl::account::RegisterRequest;
using tnycl::profile::Activity;
using tnycl::profile::Goal;
using tnycl::profile::ProfileInput;
using tnycl::profile::ProfileService;

// 解析 "prefix<十进制整数>suffix" 形式的 message；任何多余字符都视为不合契约。
std::optional<std::int64_t> parse_suffix_number(std::string_view text,
                                                std::string_view prefix,
                                                std::string_view suffix) {
  if (text.size() < prefix.size() + suffix.size() + 1) {
    return std::nullopt;
  }
  if (text.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  if (text.substr(text.size() - suffix.size()) != suffix) {
    return std::nullopt;
  }
  const auto digits = text.substr(prefix.size(), text.size() - prefix.size() - suffix.size());
  std::int64_t value = 0;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    value = value * 10 + (ch - '0');
  }
  return value;
}

// 1006：retry_after_ms 必须能从 message 中唯一解析。
std::optional<std::int64_t> parse_retry_after_ms(const std::string& message) {
  const std::string_view marker = "请 ";
  const auto pos = message.find(marker);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return parse_suffix_number(message,
                             std::string_view(message).substr(0, pos + marker.size()),
                             " 毫秒后重试");
}

// 2003：unlock_at 必须是 epoch 毫秒整数。
std::optional<std::int64_t> parse_unlock_at_ms(const std::string& message) {
  return parse_suffix_number(message, "账号已锁定至 ", "（epoch ms）");
}

// 3002：field 必须是 "字段名: 说明" 的前缀，且属于白名单。
std::optional<std::string> parse_profile_field(const std::string& message) {
  const auto pos = message.find(": ");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return message.substr(0, pos);
}

bool is_profile_field(const std::string& field) {
  return field == "name" || field == "height_cm" || field == "weight_kg" ||
         field == "age" || field == "water_ml" || field == "sleep_hours" ||
         field == "note";
}

ProfileInput valid_profile() {
  ProfileInput input;
  input.name = "我";
  input.height_cm = 170;
  input.weight_kg = 60;
  input.age = 28;
  input.goal = Goal::kBalanced;
  input.activity = Activity::kSedentary;
  input.water_ml = 1800;
  input.sleep_hours = 7.0;
  input.note = "";
  return input;
}

struct AuthFixture {
  tnycl::test::FakeClock clock;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeUserRepository users;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup profiles;
  tnycl::account::InMemoryRateLimiter limiter;
  AuthService service;

  AuthFixture()
      : service(AuthService::Deps{users, sessions, profiles, hasher, tokens, clock, limiter,
                                  nullptr}) {}

  tnycl::Result<tnycl::account::RegisterResponse> register_user() {
    RegisterRequest request;
    request.username = "alice2026";
    request.password = "S3cure!pass";
    return service.register_user(request);
  }

  tnycl::Result<tnycl::account::LoginResponse> login(const std::string& password,
                                                     const std::string& client_ip,
                                                     const std::string& username = "alice2026") {
    LoginRequest request;
    request.username = username;
    request.password = password;
    request.device = "web";
    request.client_ip = client_ip;
    return service.login(request);
  }
};

struct ProfileFixture {
  tnycl::test::FakeProfileRepository profiles;
  tnycl::test::FakeClock clock;
  tnycl::account::InMemoryRateLimiter limiter;
  ProfileService service{ProfileService::Deps{profiles, clock, limiter}};
};

}  // namespace

TNYCL_TEST(RateLimitedCarriesRetryAfterMsBoundary) {
  AuthFixture fx;
  CHECK(fx.register_user().is_ok());

  // 同 IP 前 10 次放行，第 11 次触发 1006。
  // 用不存在的用户名避免触发账号锁定（否则第 6 次起会先返回 2003）。
  for (int i = 0; i < tnycl::account::kLoginIpLimit; ++i) {
    const auto attempt = fx.login("Wrong!pass1", "10.1.1.1", "ghost" + std::to_string(i));
    CHECK(attempt.code() == ErrorCode::kInvalidCredentials);
  }

  const auto limited = fx.login("S3cure!pass", "10.1.1.1");
  CHECK(!limited.is_ok());
  CHECK(limited.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::http_status(limited.code()), 429);
  CHECK_EQ(tnycl::error_code(limited.code()), 1006);

  const auto retry = parse_retry_after_ms(limited.message());
  CHECK(retry.has_value());
  if (retry.has_value()) {
    CHECK_EQ(*retry, tnycl::account::kLoginIpWindowMs);  // 窗口刚满，恰好 60000
    CHECK(*retry >= 1);
    CHECK(*retry <= tnycl::account::kLoginIpWindowMs);
  }

  // 窗口结束前 1ms：仍需等待恰好 1ms（不得为 0 或负数）。
  fx.clock.now += tnycl::account::kLoginIpWindowMs - 1;
  const auto almost = fx.login("S3cure!pass", "10.1.1.1");
  CHECK(almost.code() == ErrorCode::kRateLimited);
  const auto almost_retry = parse_retry_after_ms(almost.message());
  CHECK(almost_retry.has_value());
  if (almost_retry.has_value()) {
    CHECK_EQ(*almost_retry, std::int64_t{1});
  }

  // 窗口结束时刻：放行（滑动窗口左闭右开）。
  fx.clock.now += 1;
  CHECK(fx.login("S3cure!pass", "10.1.1.1").is_ok());
}

TNYCL_TEST(ProfileWriteQuotaCarriesRetryAfterMs) {
  ProfileFixture fx;
  for (int i = 0; i < tnycl::account::kWriteQuotaPerMinute; ++i) {
    CHECK(fx.service.put(7, valid_profile()).is_ok());
  }

  const auto limited = fx.service.put(7, valid_profile());
  CHECK(!limited.is_ok());
  CHECK(limited.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::http_status(limited.code()), 429);
  CHECK_EQ(limited.message(), std::string("请求过于频繁，请 60000 毫秒后重试"));

  const auto retry = parse_retry_after_ms(limited.message());
  CHECK(retry.has_value());
  if (retry.has_value()) {
    CHECK_EQ(*retry, tnycl::account::kUserQuotaWindowMs);
  }

  // 限流先于字段校验：超限请求即使字段非法也返回 1006。
  auto broken = valid_profile();
  broken.height_cm = 0;
  CHECK(fx.service.put(7, broken).code() == ErrorCode::kRateLimited);

  fx.clock.now += tnycl::account::kUserQuotaWindowMs;
  CHECK(fx.service.put(7, valid_profile()).is_ok());
}

TNYCL_TEST(AccountLockedCarriesUnlockAtBoundary) {
  AuthFixture fx;
  CHECK(fx.register_user().is_ok());
  for (int i = 0; i < tnycl::account::kMaxLoginFailures; ++i) {
    CHECK(fx.login("Wrong!pass1", "").code() == ErrorCode::kInvalidCredentials);
  }

  const std::int64_t expected_unlock = fx.clock.now + tnycl::account::kLockDurationMs;
  const auto locked = fx.login("S3cure!pass", "");
  CHECK(!locked.is_ok());
  CHECK(locked.code() == ErrorCode::kAccountLocked);
  CHECK_EQ(tnycl::http_status(locked.code()), 423);
  CHECK_EQ(tnycl::error_code(locked.code()), 2003);

  const auto unlock_at = parse_unlock_at_ms(locked.message());
  CHECK(unlock_at.has_value());
  if (unlock_at.has_value()) {
    CHECK_EQ(*unlock_at, expected_unlock);
    CHECK(*unlock_at > fx.clock.now);
  }

  // 锁定期间重复请求：unlock_at 不得漂移（不得每次重算）。
  fx.clock.now += tnycl::account::kLockDurationMs - 1;
  const auto still = fx.login("S3cure!pass", "");
  CHECK(still.code() == ErrorCode::kAccountLocked);
  const auto still_unlock = parse_unlock_at_ms(still.message());
  CHECK(still_unlock.has_value());
  if (still_unlock.has_value()) {
    CHECK_EQ(*still_unlock, expected_unlock);
  }

  // 解锁时刻（now == unlock_at）：放行，并在成功后清零失败计数。
  fx.clock.now += 1;
  const auto unlocked = fx.login("S3cure!pass", "");
  CHECK(unlocked.is_ok());
  const auto stored = fx.users.find_by_id(1);
  CHECK(stored.has_value());
  if (stored.has_value()) {
    CHECK_EQ(stored->fail_count, 0);
    CHECK(!stored->locked_until_ms.has_value());
  }
}

TNYCL_TEST(ProfileInvalidCarriesFieldName) {
  ProfileFixture fx;

  struct Case {
    const char* field;
    void (*mutate)(ProfileInput&);
  };
  const Case cases[] = {
      {"name", [](ProfileInput& in) { in.name = ""; }},
      {"name", [](ProfileInput& in) { in.name = std::string(33, 'a'); }},
      {"height_cm", [](ProfileInput& in) { in.height_cm = 119; }},
      {"weight_kg", [](ProfileInput& in) { in.weight_kg = 29.9; }},
      {"age", [](ProfileInput& in) { in.age = 101; }},
      {"water_ml", [](ProfileInput& in) { in.water_ml = 1250; }},
      {"sleep_hours", [](ProfileInput& in) { in.sleep_hours = 9.5; }},
      {"note", [](ProfileInput& in) { in.note = std::string(256, 'x'); }},
  };

  for (const auto& item : cases) {
    auto input = valid_profile();
    item.mutate(input);
    const auto rejected = fx.service.put(1, input);
    CHECK(!rejected.is_ok());
    CHECK(rejected.code() == ErrorCode::kProfileInvalid);
    CHECK_EQ(tnycl::http_status(rejected.code()), 400);
    CHECK_EQ(tnycl::error_code(rejected.code()), 3002);

    const auto field = parse_profile_field(rejected.message());
    CHECK(field.has_value());
    if (field.has_value()) {
      CHECK_EQ(*field, std::string(item.field));
      CHECK(is_profile_field(*field));
      CHECK_EQ(rejected.message().rfind(*field + ": ", 0), std::size_t{0});
    }
  }

  // 合法输入不会产生 3002，也不会被误判字段。
  CHECK(fx.service.put(1, valid_profile()).is_ok());
}

int main() { return tnycl::test::run_all(); }
