// T-4.03 越权与数据隔离测试：身份只来自服务端会话，任何请求体身份字段必须无效。
// 覆盖：跨用户读/写、伪造 uid、无 token、登出/过期 token、会话与缓存串用、
// 仓储查询条件、错误响应不泄露身份信息。
#include <cstdint>
#include <optional>
#include <string>

#include "repository/profile_repository_mysql.h"
#include "repository/user_repository_mysql.h"
#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/account_token_cache.h"
#include "service/account_types.h"
#include "service/profile_service.h"
#include "support/account_fakes.h"
#include "support/fake_db.h"
#include "support/profile_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;
using tnycl::account::AuthService;
using tnycl::account::LruTokenCache;
using tnycl::profile::ProfileInput;
using tnycl::profile::ProfileService;
using tnycl::profile::ProfileView;

ProfileInput input_named(const std::string& name, int height_cm, double weight_kg) {
  ProfileInput input;
  input.name = name;
  input.height_cm = height_cm;
  input.weight_kg = weight_kg;
  input.age = 28;
  input.goal = tnycl::profile::Goal::kBalanced;
  input.activity = tnycl::profile::Activity::kModerate;
  input.water_ml = 1800;
  input.sleep_hours = 7.0;
  input.note = "";
  return input;
}

struct World {
  tnycl::test::FakeUserRepository users;
  tnycl::test::FakeSessionRepository sessions;
  tnycl::test::FakeProfileLookup lookup;
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeClock clock;
  tnycl::account::InMemoryRateLimiter limiter;
  tnycl::test::FakeProfileRepository profile_rows;
  LruTokenCache cache{64};
  AuthService auth{AuthService::Deps{users, sessions, lookup, hasher, tokens, clock, limiter,
                                     &cache}};
  ProfileService profile{ProfileService::Deps{profile_rows, clock, limiter}};

  std::uint64_t provision(const std::string& username, std::string* token,
                          const std::string& ip = "203.0.113.20") {
    CHECK(auth.register_user({username, "Secret123"}).is_ok());
    const auto logged = auth.login({username, "Secret123", "dev", ip});
    CHECK(logged.is_ok());
    if (!logged.is_ok()) {
      return 0;
    }
    *token = logged.value().token;
    return logged.value().user_id;
  }
};

std::uint64_t session_uid(World& world, const std::string& token) {
  const auto context = world.auth.authenticate(token);
  CHECK(context.is_ok());
  return context.is_ok() ? context.value().user_id : 0;
}

}  // namespace

TNYCL_TEST(VictimProfileIsInvisibleToOtherUser) {
  World world;
  std::string token_a;
  std::string token_b;
  const auto uid_a = world.provision("victim2026", &token_a);
  world.provision("attacker2026", &token_b);
  CHECK(world.profile.put(uid_a, input_named("受害者", 170, 60)).is_ok());

  // 攻击者用自己的会话读自己：不得拿到受害者的档案。
  const auto attacker_view = world.profile.get(session_uid(world, token_b));
  CHECK(attacker_view.code() == ErrorCode::kProfileNotFound);

  // 受害者本人仍能读到自己的档案（证明数据确实存在）。
  const auto victim_view = world.profile.get(session_uid(world, token_a));
  CHECK(victim_view.is_ok());
  CHECK_EQ(victim_view.value().profile.name, std::string("受害者"));
}

TNYCL_TEST(AttackerCannotOverwriteVictimProfile) {
  World world;
  std::string token_a;
  std::string token_b;
  const auto uid_a = world.provision("victim2026", &token_a);
  const auto uid_b = world.provision("attacker2026", &token_b);
  CHECK(world.profile.put(uid_a, input_named("受害者", 170, 60)).is_ok());

  // 攻击者写入自己的档案，uid 取自自己的会话。
  CHECK(world.profile.put(session_uid(world, token_b), input_named("攻击者", 180, 90)).is_ok());

  const auto& victim_row = world.profile_rows.rows.at(uid_a);
  CHECK_EQ(victim_row.profile.name, std::string("受害者"));
  CHECK_EQ(victim_row.profile.height_cm, 170);
  CHECK_NEAR(victim_row.profile.weight_kg, 60.0);

  const auto& attacker_row = world.profile_rows.rows.at(uid_b);
  CHECK_EQ(attacker_row.profile.name, std::string("攻击者"));
  CHECK_EQ(attacker_row.profile.height_cm, 180);
}

TNYCL_TEST(ForgedUidInRequestBodyCannotChangeIdentity) {
  World world;
  std::string token_a;
  std::string token_b;
  const auto uid_a = world.provision("victim2026", &token_a);
  const auto uid_b = world.provision("attacker2026", &token_b);
  CHECK(world.profile.put(uid_a, input_named("受害者", 170, 60)).is_ok());
  CHECK(world.profile.put(uid_b, input_named("攻击者", 180, 90)).is_ok());

  // 模拟传输层 handler：只从会话取 uid，请求体里的任何身份字段都不参与。
  const std::string forged_body =
      "{\"user_id\":" + std::to_string(uid_a) + ",\"uid\":" + std::to_string(uid_a) + "}";
  const auto handle_get = [&](const std::string& token,
                              const std::string& body) -> std::optional<ProfileView> {
    (void)body;  // 攻击载荷：真实 handler 同样不得解析身份字段
    const auto context = world.auth.authenticate(token);
    if (!context.is_ok()) {
      return std::nullopt;
    }
    const auto view = world.profile.get(context.value().user_id);
    if (!view.is_ok()) {
      return std::nullopt;
    }
    return std::optional<ProfileView>{view.value()};
  };

  const auto seen = handle_get(token_b, forged_body);
  CHECK(seen.has_value());
  CHECK_EQ(seen->profile.name, std::string("攻击者"));
  CHECK(seen->profile.name != std::string("受害者"));
}

TNYCL_TEST(RequestsWithoutValidTokenAreRejected) {
  World world;
  std::string token_a;
  const auto uid_a = world.provision("victim2026", &token_a);
  CHECK(world.profile.put(uid_a, input_named("受害者", 170, 60)).is_ok());

  const auto empty_token = world.auth.authenticate("");
  CHECK(empty_token.code() == ErrorCode::kTokenInvalid);
  CHECK_EQ(tnycl::error_code(empty_token.code()), 2004);

  const auto garbage = world.auth.authenticate("forged-token");
  CHECK(garbage.code() == ErrorCode::kTokenInvalid);

  // 直接用 uid / 用户名当 token 也不行。
  CHECK(world.auth.authenticate(std::to_string(uid_a)).code() == ErrorCode::kTokenInvalid);
  CHECK(world.auth.authenticate("victim2026").code() == ErrorCode::kTokenInvalid);

  // 无 token 时拿不到任何档案数据。
  const auto me = world.auth.me("");
  CHECK(!me.is_ok());
  CHECK(me.code() == ErrorCode::kTokenInvalid);
}

TNYCL_TEST(LoggedOutTokenStopsWorkingImmediately) {
  World world;
  std::string token_a;
  const auto uid_a = world.provision("victim2026", &token_a);
  CHECK(session_uid(world, token_a) == uid_a);

  CHECK(world.auth.logout(token_a).is_ok());
  const auto after_logout = world.auth.authenticate(token_a);
  CHECK(after_logout.code() == ErrorCode::kTokenInvalid);
  CHECK(world.auth.me(token_a).code() == ErrorCode::kTokenInvalid);
}

TNYCL_TEST(ExpiredTokenIsRejected) {
  World world;
  std::string token_a;
  world.provision("victim2026", &token_a);
  world.clock.now += tnycl::account::kSessionTtlMs + 1;

  const auto expired = world.auth.authenticate(token_a);
  CHECK(!expired.is_ok());
  CHECK_EQ(tnycl::error_code(expired.code()), 2005);
}

TNYCL_TEST(SessionAndCacheDoNotCrossUsers) {
  World world;
  std::string token_a;
  std::string token_b;
  const auto uid_a = world.provision("victim2026", &token_a);
  const auto uid_b = world.provision("attacker2026", &token_b);

  const std::string hash_a = world.tokens.sha256_hex(token_a);
  const std::string hash_b = world.tokens.sha256_hex(token_b);
  CHECK(world.auth.authenticate(token_a).is_ok());
  CHECK(world.auth.authenticate(token_b).is_ok());

  CHECK(world.cache.get(hash_a).has_value());
  CHECK(world.cache.get(hash_b).has_value());
  CHECK_EQ(world.cache.get(hash_a)->user_id, uid_a);
  CHECK_EQ(world.cache.get(hash_b)->user_id, uid_b);

  // 登出 A 后：A 立刻失效，B 完全不受影响。
  CHECK(world.auth.logout(token_a).is_ok());
  CHECK(!world.cache.get(hash_a).has_value());
  CHECK(world.auth.authenticate(token_a).code() == ErrorCode::kTokenInvalid);
  const auto still_b = world.auth.authenticate(token_b);
  CHECK(still_b.is_ok());
  CHECK_EQ(still_b.value().user_id, uid_b);
}

TNYCL_TEST(ErrorResponsesDoNotLeakIdentityInformation) {
  World world;
  CHECK(world.auth.register_user({"victim2026", "Secret123"}).is_ok());

  const auto unknown_user =
      world.auth.login({"nobody2026", "Secret123", "d", "198.51.100.11"});
  const auto wrong_password =
      world.auth.login({"victim2026", "WrongPass9", "d", "198.51.100.12"});
  CHECK(unknown_user.code() == ErrorCode::kInvalidCredentials);
  CHECK(wrong_password.code() == ErrorCode::kInvalidCredentials);
  // 账号不存在与密码错误必须返回同文案，避免账号枚举。
  CHECK_EQ(unknown_user.message(), wrong_password.message());
  CHECK(unknown_user.message().find("nobody2026") == std::string::npos);
  CHECK(unknown_user.message().find("argon2") == std::string::npos);

  const auto bad_token = world.auth.authenticate("leaked-token-value");
  CHECK(bad_token.code() == ErrorCode::kTokenInvalid);
  CHECK(bad_token.message().find("leaked-token-value") == std::string::npos);
  CHECK(bad_token.message().find("victim2026") == std::string::npos);

  const auto no_profile = world.profile.get(1);
  CHECK(no_profile.code() == ErrorCode::kProfileNotFound);
  CHECK(no_profile.message().find("1") == std::string::npos);
  CHECK(no_profile.message().find("victim2026") == std::string::npos);
}

TNYCL_TEST(MysqlQueriesAlwaysBindCallerUid) {
  tnycl::test::FakeDbPool pool;
  tnycl::repository::MySqlProfileRepository profiles{pool};
  tnycl::repository::MySqlSessionRepository sessions{pool};

  pool.state->responses.push_back(tnycl::test::make_result({}));
  CHECK(profiles.find(42).is_ok());
  CHECK(pool.state->sqls.back().find("WHERE user_id = ?") != std::string::npos);
  CHECK_EQ(pool.state->params.back().at(0), std::string("42"));

  tnycl::profile::ProfileRecord record;
  record.profile = input_named("我", 170, 60);
  record.updated_at_ms = 1'700'000'000'000;
  CHECK(profiles.upsert(42, record).is_ok());
  CHECK(pool.state->sqls.back().find("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, ''), "
                                     "FROM_UNIXTIME(? / 1000))") != std::string::npos);
  CHECK_EQ(pool.state->params.back().at(0), std::string("42"));

  // 会话查询只按令牌摘要定位，不存在由客户端指定 uid 的入口。
  pool.state->responses.push_back(tnycl::test::make_result({}));
  CHECK(!sessions.find_by_token_hash("abc").has_value());
  CHECK_EQ(pool.state->params.back().size(), std::size_t{1});
  CHECK(pool.state->sqls.back().find("WHERE token_hash = ?") != std::string::npos);
}

int main() { return tnycl::test::run_all(); }
