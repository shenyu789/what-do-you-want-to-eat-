// M3 账号模块单元测试与异常场景测试。
#include <string>

#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/account_token_cache.h"
#include "support/account_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

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
      const std::string& username = "alice2026", const std::string& password = "S3cure!pass") {
    RegisterRequest request;
    request.username = username;
    request.password = password;
    return service.register_user(request);
  }

  tnycl::Result<tnycl::account::LoginResponse> login(
      const std::string& username = "alice2026", const std::string& password = "S3cure!pass",
      const std::string& client_ip = "") {
    LoginRequest request;
    request.username = username;
    request.password = password;
    request.device = "web";
    request.client_ip = client_ip;
    return service.login(request);
  }
};

}  // namespace

TNYCL_TEST(RegisterCreatesUserAndRejectsDuplicate) {
  Fixture fx;
  const auto created = fx.register_user();
  CHECK(created.is_ok());
  CHECK_EQ(created.value().user_id, 1u);
  CHECK_EQ(created.value().username, std::string("alice2026"));
  CHECK_EQ(created.value().created_at_ms, fx.clock.now);
  CHECK_EQ(fx.users.size(), std::size_t{1});

  const auto duplicate = fx.register_user();
  CHECK(!duplicate.is_ok());
  CHECK(duplicate.code() == ErrorCode::kUsernameTaken);
  CHECK_EQ(tnycl::http_status(duplicate.code()), 409);
  CHECK_EQ(fx.users.size(), std::size_t{1});
}

TNYCL_TEST(RegisterRejectsWeakPasswordAndInvalidFields) {
  Fixture fx;

  const auto weak = fx.register_user("alice2026", "12345678");
  CHECK(!weak.is_ok());
  CHECK(weak.code() == ErrorCode::kWeakPassword);
  CHECK_EQ(tnycl::http_status(weak.code()), 422);
  CHECK(weak.message().find("password") != std::string::npos);

  const auto short_username = fx.register_user("abc");
  CHECK(!short_username.is_ok());
  CHECK(short_username.code() == ErrorCode::kValidationFailed);
  CHECK_EQ(tnycl::http_status(short_username.code()), 400);

  const auto missing = fx.register_user("", "");
  CHECK(!missing.is_ok());
  CHECK(missing.code() == ErrorCode::kBadRequest);
  CHECK_EQ(tnycl::http_status(missing.code()), 400);
}

TNYCL_TEST(LoginIssuesSevenDayTokenWithoutStoringPlaintext) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());

  const auto logged_in = fx.login();
  CHECK(logged_in.is_ok());
  CHECK(!logged_in.value().token.empty());
  CHECK_EQ(logged_in.value().expires_at_ms, fx.clock.now + tnycl::account::kSessionTtlMs);
  CHECK_EQ(logged_in.value().user_id, 1u);
  CHECK(!logged_in.value().has_profile);

  CHECK_EQ(fx.sessions.sessions_.size(), std::size_t{1});
  CHECK(fx.sessions.sessions_.front().token_hash != logged_in.value().token);
  CHECK_EQ(fx.sessions.sessions_.front().token_hash,
           std::string("sha256:") + logged_in.value().token);
}

TNYCL_TEST(LoginFailureIsIndistinguishableFromUnknownAccount) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());

  const auto wrong = fx.login("alice2026", "Wrong!pass1");
  CHECK(!wrong.is_ok());
  CHECK(wrong.code() == ErrorCode::kInvalidCredentials);
  CHECK_EQ(tnycl::http_status(wrong.code()), 401);

  const auto nobody = fx.login("nobody2026", "Wrong!pass1");
  CHECK(!nobody.is_ok());
  CHECK(nobody.code() == ErrorCode::kInvalidCredentials);
  CHECK_EQ(nobody.message(), wrong.message());
}

TNYCL_TEST(LoginHashesEvenWhenAccountDoesNotExist) {
  Fixture fx;
  tnycl::test::CountingHasher hasher;
  AuthService service(AuthService::Deps{fx.users, fx.sessions, fx.profiles, hasher, fx.tokens,
                                        fx.clock, fx.limiter, nullptr});
  LoginRequest request;
  request.username = "ghost2026";
  request.password = "Wrong!pass1";
  const auto result = service.login(request);
  CHECK(!result.is_ok());
  CHECK_EQ(hasher.verify_calls, 1);
}

TNYCL_TEST(FiveFailuresLockAccountForFifteenMinutes) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());
  for (int i = 0; i < tnycl::account::kMaxLoginFailures; ++i) {
    const auto failed = fx.login("alice2026", "Wrong!pass1");
    CHECK(!failed.is_ok());
    CHECK(failed.code() == ErrorCode::kInvalidCredentials);
  }

  const auto locked = fx.login();
  CHECK(!locked.is_ok());
  CHECK(locked.code() == ErrorCode::kAccountLocked);
  CHECK_EQ(tnycl::http_status(locked.code()), 423);

  const auto stored = fx.users.find_by_id(1);
  CHECK(stored.has_value());
  CHECK(stored->locked_until_ms.has_value());
  CHECK_EQ(*stored->locked_until_ms, fx.clock.now + tnycl::account::kLockDurationMs);

  fx.clock.now += tnycl::account::kLockDurationMs + 1;
  CHECK(fx.login().is_ok());
}

TNYCL_TEST(TokenExpiryAndLogoutAreDistinguished) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());
  const auto logged_in = fx.login();
  CHECK(logged_in.is_ok());

  fx.clock.now += tnycl::account::kSessionTtlMs + 1;
  const auto expired = fx.service.authenticate(logged_in.value().token);
  CHECK(!expired.is_ok());
  CHECK(expired.code() == ErrorCode::kTokenExpired);
  CHECK_EQ(tnycl::http_status(expired.code()), 401);

  const auto fresh = fx.login();
  CHECK(fresh.is_ok());
  CHECK(fx.service.logout(fresh.value().token).is_ok());
  const auto revoked = fx.service.authenticate(fresh.value().token);
  CHECK(!revoked.is_ok());
  CHECK(revoked.code() == ErrorCode::kTokenInvalid);

  const auto forged = fx.service.authenticate("not-a-real-token");
  CHECK(!forged.is_ok());
  CHECK(forged.code() == ErrorCode::kTokenInvalid);
  CHECK(fx.service.authenticate("").code() == ErrorCode::kTokenInvalid);
}

TNYCL_TEST(MeReturnsProfileFlagAndRejectsBadTokens) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());
  const auto logged_in = fx.login();
  CHECK(logged_in.is_ok());

  const auto before = fx.service.me(logged_in.value().token);
  CHECK(before.is_ok());
  CHECK(!before.value().has_profile);

  fx.profiles.has_profile_ = true;
  const auto after = fx.service.me(logged_in.value().token);
  CHECK(after.is_ok());
  CHECK(after.value().has_profile);
  CHECK_EQ(after.value().username, std::string("alice2026"));

  const auto rejected = fx.service.me("bogus");
  CHECK(!rejected.is_ok());
  CHECK(rejected.code() == ErrorCode::kTokenInvalid);
}

TNYCL_TEST(LoginIsRateLimitedPerIp) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());
  for (int i = 0; i < tnycl::account::kLoginIpLimit; ++i) {
    const auto attempt = fx.login("alice2026", "Wrong!pass1", "10.0.0.8");
    CHECK(!attempt.is_ok());
    CHECK(attempt.code() != ErrorCode::kRateLimited);
  }
  const auto limited = fx.login("alice2026", "Wrong!pass1", "10.0.0.8");
  CHECK(!limited.is_ok());
  CHECK(limited.code() == ErrorCode::kRateLimited);
  CHECK_EQ(tnycl::http_status(limited.code()), 429);

  const auto other_ip = fx.login("alice2026", "Wrong!pass1", "10.0.0.9");
  CHECK(other_ip.code() != ErrorCode::kRateLimited);
}

TNYCL_TEST(DisabledAccountCannotLoginOrUseToken) {
  Fixture fx;
  CHECK(fx.register_user().is_ok());
  const auto logged_in = fx.login();
  CHECK(logged_in.is_ok());

  fx.users.set_status(1, 2);
  const auto blocked = fx.login();
  CHECK(!blocked.is_ok());
  CHECK(blocked.code() == ErrorCode::kUserDisabled);
  CHECK_EQ(tnycl::http_status(blocked.code()), 403);

  const auto token_use = fx.service.authenticate(logged_in.value().token);
  CHECK(!token_use.is_ok());
  CHECK(token_use.code() == ErrorCode::kUserDisabled);
}

TNYCL_TEST(CachedAuthenticationStillHonorsTokenExpiry) {
  Fixture fx(true);
  CHECK(fx.register_user().is_ok());
  const auto logged_in = fx.login();
  CHECK(logged_in.is_ok());

  CHECK(fx.service.authenticate(logged_in.value().token).is_ok());
  fx.clock.now += tnycl::account::kSessionTtlMs + 1;
  const auto expired = fx.service.authenticate(logged_in.value().token);
  CHECK(!expired.is_ok());
  CHECK(expired.code() == ErrorCode::kTokenExpired);
}

TNYCL_TEST(TokenCacheEvictsOnLogoutAndLruKeepsBounded) {
  tnycl::account::LruTokenCache cache(2);
  tnycl::account::Principal principal;
  principal.user_id = 7;
  principal.session_id = 9;
  principal.username = "cache-user";
  principal.expires_at_ms = 42;

  cache.put("a", principal, 30);
  cache.put("b", principal, 30);
  CHECK(cache.get("a").has_value());
  cache.put("c", principal, 30);  // 淘汰 b
  CHECK(cache.get("a").has_value());
  CHECK(!cache.get("b").has_value());
  CHECK(cache.get("c").has_value());

  cache.erase("a");
  CHECK(!cache.get("a").has_value());
}

TNYCL_TEST(ConstantTimeCompareAndValidators) {
  CHECK(tnycl::account::constant_time_equal("abc", "abc"));
  CHECK(!tnycl::account::constant_time_equal("abc", "abd"));
  CHECK(!tnycl::account::constant_time_equal("abc", "abcd"));

  CHECK(!tnycl::account::validate_username("小明小明").has_value());
  CHECK(tnycl::account::validate_username("abc").has_value());
  CHECK_EQ(tnycl::account::utf8_codepoint_count("小明abcd"), std::size_t{6});
  CHECK(tnycl::account::is_valid_utf8("小明"));
  CHECK(!tnycl::account::is_valid_utf8("\xC0\xAF"));

  CHECK(!tnycl::account::validate_password("S3cure!pass").has_value());
  CHECK(tnycl::account::validate_password("12345678").has_value());
  CHECK(tnycl::account::validate_password("aaaaaaaa").has_value());
  CHECK(tnycl::account::validate_password("Ab1!").has_value());
}

int main() { return tnycl::test::run_all(); }
