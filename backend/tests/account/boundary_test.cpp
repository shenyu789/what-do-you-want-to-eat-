// M3 账号边界测试（第三阶段 P1）。
// 只覆盖此前 852 个断言未覆盖的边界：用户名/密码精确长度、会话过期时刻、
// 多端会话独立、仓储异常向服务层传播。不重复既有用例。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/account_token_cache.h"
#include "support/account_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace {

using tnycl::ErrorCode;
using tnycl::account::AuthService;
using tnycl::account::LoginRequest;
using tnycl::account::RegisterRequest;

struct Fixture {
  tnycl::test::FakeClock clock;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeUserRepository users;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup profiles;
  tnycl::account::InMemoryRateLimiter limiter;
  tnycl::account::LruTokenCache cache{128};
  AuthService service;

  explicit Fixture(bool use_cache = false)
      : service(AuthService::Deps{users, sessions, profiles, hasher, tokens, clock, limiter,
                                  use_cache ? &cache : nullptr}) {}

  tnycl::Result<tnycl::account::RegisterResponse> register_user(
      const std::string& username = "alice2026") {
    RegisterRequest request;
    request.username = username;
    request.password = "S3cure!pass";
    return service.register_user(request);
  }

  tnycl::Result<tnycl::account::LoginResponse> login(const std::string& device = "web") {
    LoginRequest request;
    request.username = "alice2026";
    request.password = "S3cure!pass";
    request.device = device;
    request.client_ip = "";
    return service.login(request);
  }
};

// 仓储层异常：MySQL 实现以 AppError 抛 SQL/连接错误，insert 以 Result 返回 9001。
class ThrowingUserRepository final : public tnycl::account::UserRepository {
 public:
  std::optional<tnycl::account::UserRecord> find_by_username(std::string_view) override {
    throw tnycl::AppError(tnycl::ErrorCode::kDbBusy);
  }
  std::optional<tnycl::account::UserRecord> find_by_id(std::uint64_t) override {
    throw tnycl::AppError(tnycl::ErrorCode::kDbUnavailable);
  }
  tnycl::Result<std::uint64_t> insert(std::string_view, std::string_view,
                                      std::int64_t) override {
    return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kDbUnavailable, "连接失败");
  }
  tnycl::Result<void> update_login_state(std::uint64_t, int,
                                         std::optional<std::int64_t>) override {
    return tnycl::Result<void>::err(tnycl::ErrorCode::kDbBusy);
  }
};

}  // namespace

TNYCL_TEST(UsernameLengthAndCharacterBoundaries) {
  using tnycl::account::validate_username;

  CHECK(!validate_username(std::string(4, 'a')).has_value());
  CHECK(!validate_username(std::string(32, 'a')).has_value());
  CHECK(validate_username(std::string(3, 'a')).has_value());
  CHECK(validate_username(std::string(33, 'a')).has_value());

  // 长度按码点而非字节计算。
  CHECK(!validate_username("用户甲乙").has_value());  // 4 码点 / 12 字节
  std::string cjk33;
  for (int i = 0; i < 33; ++i) {
    cjk33 += "我";  // 33 码点 / 99 字节
  }
  CHECK(validate_username(cjk33).has_value());

  CHECK(validate_username(" abc").has_value());   // 首空格
  CHECK(validate_username("abc ").has_value());   // 尾空格
  CHECK(validate_username("ab\tcd").has_value()); // 制表符（控制字符）
  CHECK(validate_username("ab\x7F" "cd").has_value());
  CHECK(validate_username("\xC0\xAF" "abcd").has_value());  // 过长编码，非法 UTF-8

  // 端到端：32 字符用户名可注册，33 字符被 service 拒绝。
  Fixture ok;
  CHECK(ok.register_user(std::string(32, 'n')).is_ok());
  Fixture bad;
  const auto rejected = bad.register_user(std::string(33, 'n'));
  CHECK(!rejected.is_ok());
  CHECK(rejected.code() == ErrorCode::kValidationFailed);
  CHECK(rejected.message().rfind("username: ", 0) == std::size_t{0});
}

TNYCL_TEST(PasswordLengthBoundariesAndCharacterClasses) {
  using tnycl::account::validate_password;

  CHECK(!validate_password("Abcdefg1").has_value());  // 8 字节、3 类
  CHECK(validate_password("Abcdef1").has_value());    // 7 字节

  const std::string p128 = "A" + std::string(126, 'b') + "1";
  const std::string p129 = "A" + std::string(127, 'b') + "1";
  CHECK_EQ(p128.size(), std::size_t{128});
  CHECK_EQ(p129.size(), std::size_t{129});
  CHECK(!validate_password(p128).has_value());
  CHECK(validate_password(p129).has_value());

  CHECK(validate_password("abcdefgh").has_value());    // 仅小写一类
  CHECK(validate_password("!!!!!!!!").has_value());    // 仅符号一类
  CHECK(!validate_password("Abcdefg!").has_value());   // 小写 + 符号
  CHECK(validate_password("\xC0\xAF\xC0\xAF\xC0\xAF\xC0\xAF").has_value());  // 非法 UTF-8

  // 端到端：合法强度密码可注册，7 字符弱密码被拒。
  Fixture fx;
  RegisterRequest short_request;
  short_request.username = "bob2026";
  short_request.password = "Abcdef1";
  const auto short_rejected = fx.service.register_user(short_request);
  CHECK(!short_rejected.is_ok());
  CHECK(short_rejected.code() == ErrorCode::kWeakPassword);
  CHECK(fx.register_user("bob2026").is_ok());
}

TNYCL_TEST(SessionExpiryBoundaryWithAndWithoutCache) {
  for (const bool use_cache : {false, true}) {
    Fixture fx(use_cache);
    CHECK(fx.register_user().is_ok());
    const auto logged = fx.login();
    CHECK(logged.is_ok());
    const std::string token = logged.value().token;
    const std::int64_t expires_at = logged.value().expires_at_ms;

    // expires_at - 1：仍然有效。
    fx.clock.now = expires_at - 1;
    CHECK(fx.service.authenticate(token).is_ok());

    // expires_at 当刻：过期（2005），且缓存条目被清除。
    fx.clock.now = expires_at;
    const auto expired = fx.service.authenticate(token);
    CHECK(!expired.is_ok());
    CHECK(expired.code() == ErrorCode::kTokenExpired);
    CHECK_EQ(tnycl::http_status(expired.code()), 401);

    // 第二次仍为 2005：说明缓存路径已清除，回落数据库路径同样判定过期。
    const auto again = fx.service.authenticate(token);
    CHECK(again.code() == ErrorCode::kTokenExpired);
  }
}

TNYCL_TEST(MultipleSessionsStayIndependentOnLogout) {
  Fixture fx(true);
  CHECK(fx.register_user().is_ok());

  const auto web = fx.login("web");
  const auto phone = fx.login("phone");
  CHECK(web.is_ok());
  CHECK(phone.is_ok());
  CHECK(web.value().token != phone.value().token);
  CHECK_EQ(fx.sessions.sessions_.size(), std::size_t{2});
  CHECK_EQ(fx.sessions.sessions_[0].device, std::string("web"));
  CHECK_EQ(fx.sessions.sessions_[1].device, std::string("phone"));

  CHECK(fx.service.logout(web.value().token).is_ok());

  // 只吊销 web，phone 会话不受影响。
  const auto revoked = fx.service.authenticate(web.value().token);
  CHECK(revoked.code() == ErrorCode::kTokenInvalid);
  CHECK(fx.service.authenticate(phone.value().token).is_ok());
  CHECK(fx.service.me(phone.value().token).is_ok());
  CHECK_EQ(fx.sessions.sessions_.size(), std::size_t{1});
  CHECK_EQ(fx.sessions.sessions_.front().device, std::string("phone"));
}

TNYCL_TEST(RepositoryFailuresReachServiceCallers) {
  tnycl::test::FakeClock clock;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup profiles;
  tnycl::account::InMemoryRateLimiter limiter;
  ThrowingUserRepository users;
  AuthService service(
      AuthService::Deps{users, sessions, profiles, hasher, tokens, clock, limiter, nullptr});

  // 查询抛 AppError：不得吞错，由 transport 统一映射 503。
  LoginRequest login_request;
  login_request.username = "alice2026";
  login_request.password = "S3cure!pass";
  bool threw = false;
  try {
    service.login(login_request);
  } catch (const tnycl::AppError& error) {
    threw = true;
    CHECK_EQ(error.code_int(), 9002);
    CHECK_EQ(error.http(), 503);
  }
  CHECK(threw);

  // insert 返回错误：service 原样透传 9001，不落成 1010。
  RegisterRequest register_request;
  register_request.username = "alice2026";
  register_request.password = "S3cure!pass";
  const auto rejected = service.register_user(register_request);
  CHECK(!rejected.is_ok());
  CHECK(rejected.code() == ErrorCode::kDbUnavailable);
  CHECK_EQ(tnycl::http_status(rejected.code()), 503);
  CHECK_EQ(tnycl::error_code(rejected.code()), 9001);
  CHECK_EQ(rejected.message(), std::string("连接失败"));
}

int main() { return tnycl::test::run_all(); }
