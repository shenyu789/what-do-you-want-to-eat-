// 真实 MySQL 集成测试（P0）。设计与执行分离：
//   - 本文件包含 9 个集成场景，全部通过冻结契约 IDbPool / DbConn 访问数据库；
//   - 唯一集成点见 make_mysql_pool()：A 交付 src/adapters 的真实 DbConn 后替换函数体即可运行；
//   - 未配置 TNYCL_MYSQL_* 或集成点未接入时以退出码 77 跳过，**不计为通过**。
//
// 环境变量：TNYCL_MYSQL_HOST / TNYCL_MYSQL_PORT / TNYCL_MYSQL_USER /
//           TNYCL_MYSQL_PASSWORD / TNYCL_MYSQL_DB
// 退出码：0=全部通过，1=有失败，77=跳过（未接入真实数据库）。
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "repository/food_repository_mysql.h"
#include "repository/profile_repository_mysql.h"
#include "repository/user_repository_mysql.h"
#include "service/account_rate_limiter.h"
#include "service/account_service.h"
#include "service/food_service.h"
#include "service/profile_service.h"
#include "support/account_fakes.h"
#include "support/test_harness.h"
#include "tnycl/db_pool.h"
#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace {

struct MysqlEnv {
  std::string host;
  std::string port;
  std::string user;
  std::string password;
  std::string db;
};

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::string(value);
}

std::optional<MysqlEnv> env_from_process() {
  MysqlEnv env;
  env.host = env_or("TNYCL_MYSQL_HOST", "");
  env.port = env_or("TNYCL_MYSQL_PORT", "3306");
  env.user = env_or("TNYCL_MYSQL_USER", "root");
  env.password = env_or("TNYCL_MYSQL_PASSWORD", "");
  env.db = env_or("TNYCL_MYSQL_DB", "tnycl_it");
  if (env.host.empty()) {
    return std::nullopt;
  }
  return env;
}

// ============================ 唯一集成点 ============================
// A 交付真实 DbConn（mysql_real_connect 适配）后，只替换本函数体，例如：
//   auto factory = [env] {
//     return tnycl::adapters::make_mysql_connection(env.host, env.port, env.user,
//                                                   env.password, env.db);
//   };
//   return std::make_unique<tnycl::MySqlPool>(4, 2000, factory);
// 在适配层就绪前返回 nullptr，使集成用例以 SKIP 结束，绝不伪造通过。
std::unique_ptr<tnycl::IDbPool> make_mysql_pool(const MysqlEnv& /*env*/) { return nullptr; }
// ====================================================================

std::unique_ptr<tnycl::IDbPool> g_pool;

tnycl::IDbPool& pool() { return *g_pool; }

tnycl::DbConn::Result query(const std::string& sql,
                            const std::vector<std::string>& params = {}) {
  auto conn = pool().acquire(2000);
  CHECK(conn != nullptr);
  return conn->exec(sql, params);
}

std::string scalar(const std::string& sql, const std::vector<std::string>& params = {}) {
  auto result = query(sql, params);
  if (result.rows.empty() || result.rows.front().cols.empty()) {
    return "";
  }
  return result.rows.front().cols.front().value_or("");
}

void exec(const std::string& sql, const std::vector<std::string>& params = {}) {
  auto conn = pool().acquire(2000);
  CHECK(conn != nullptr);
  conn->exec(sql, params);
}

struct World {
  tnycl::repository::MySqlUserRepository users{pool()};
  tnycl::repository::MySqlSessionRepository sessions{pool()};
  tnycl::repository::MySqlProfileRepository profiles{pool()};
  tnycl::repository::MySqlFoodRepository foods{pool()};
  tnycl::test::FakeHasher hasher;
  tnycl::test::FakeTokenSource tokens;
  tnycl::test::FakeClock clock;
  tnycl::account::InMemoryRateLimiter limiter;
  tnycl::profile::ProfileService profile_service{
      tnycl::profile::ProfileService::Deps{profiles, clock, limiter}};
  tnycl::food::FoodService food_service{tnycl::food::FoodService::Deps{foods}};

  std::unique_ptr<tnycl::profile::ProfileLookupAdapter> lookup{
      std::make_unique<tnycl::profile::ProfileLookupAdapter>(profiles)};
  tnycl::account::AuthService auth{tnycl::account::AuthService::Deps{
      users, sessions, *lookup, hasher, tokens, clock, limiter, nullptr}};

  std::string unique_name(const std::string& prefix) {
    static int counter = 0;
    ++counter;
    return prefix + "_" + std::to_string(clock.now_ms()) + "_" + std::to_string(counter);
  }
};

struct Fixture {
  World world;
  ~Fixture() {
    exec("DELETE FROM user_sessions WHERE user_id IN (SELECT user_id FROM users WHERE username "
         "LIKE 'it\\_%')");
    exec("DELETE FROM profiles WHERE user_id IN (SELECT user_id FROM users WHERE username LIKE "
         "'it\\_%')");
    exec("DELETE FROM users WHERE username LIKE 'it\\_%'");
  }
};

}  // namespace

TNYCL_TEST(RegisterPersistsUserRow) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_reg");
  const auto created = fx.world.auth.register_user({username, "Secret123"});
  CHECK(created.is_ok());

  CHECK_EQ(scalar("SELECT COUNT(*) FROM users WHERE username = ?", {username}), std::string("1"));
  const auto hash = scalar("SELECT pass_hash FROM users WHERE username = ?", {username});
  CHECK(hash.find("Secret123") == std::string::npos);
  CHECK(!hash.empty());
}

TNYCL_TEST(LoginPersistsSessionWithHashedToken) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_login");
  CHECK(fx.world.auth.register_user({username, "Secret123"}).is_ok());
  const auto logged =
      fx.world.auth.login({username, "Secret123", "it-device", "203.0.113.90"});
  CHECK(logged.is_ok());

  const std::string token_hash = fx.world.tokens.sha256_hex(logged.value().token);
  CHECK_EQ(scalar("SELECT COUNT(*) FROM user_sessions WHERE token_hash = ?", {token_hash}),
           std::string("1"));
  CHECK_EQ(scalar("SELECT CHAR_LENGTH(token_hash) FROM user_sessions WHERE token_hash = ?",
                  {token_hash}),
           std::string("64"));
  // 库里不得出现明文 token。
  CHECK_EQ(scalar("SELECT COUNT(*) FROM user_sessions WHERE token_hash = ?",
                  {logged.value().token}),
           std::string("0"));
}

TNYCL_TEST(LogoutDeletesSessionRow) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_logout");
  CHECK(fx.world.auth.register_user({username, "Secret123"}).is_ok());
  const auto logged =
      fx.world.auth.login({username, "Secret123", "it-device", "203.0.113.91"});
  CHECK(logged.is_ok());
  const std::string token_hash = fx.world.tokens.sha256_hex(logged.value().token);

  CHECK(fx.world.auth.logout(logged.value().token).is_ok());
  CHECK_EQ(scalar("SELECT COUNT(*) FROM user_sessions WHERE token_hash = ?", {token_hash}),
           std::string("0"));
  CHECK(fx.world.auth.authenticate(logged.value().token).code() ==
        tnycl::ErrorCode::kTokenInvalid);
}

TNYCL_TEST(ProfileRoundTripKeepsChineseEnums) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_profile");
  CHECK(fx.world.auth.register_user({username, "Secret123"}).is_ok());
  const auto logged =
      fx.world.auth.login({username, "Secret123", "it-device", "203.0.113.92"});
  CHECK(logged.is_ok());
  const auto uid = logged.value().user_id;

  tnycl::profile::ProfileInput input;
  input.name = "测试用户";
  input.height_cm = 168;
  input.weight_kg = 58.5;
  input.age = 30;
  input.goal = tnycl::profile::Goal::kMoreProtein;
  input.activity = tnycl::profile::Activity::kModerate;
  input.water_ml = 2200;
  input.sleep_hours = 7.5;
  input.note = "少糖";
  CHECK(fx.world.profile_service.put(uid, input).is_ok());

  CHECK_EQ(scalar("SELECT goal FROM profiles WHERE user_id = ?", {std::to_string(uid)}),
           std::string("增加蛋白"));
  CHECK_EQ(scalar("SELECT activity FROM profiles WHERE user_id = ?", {std::to_string(uid)}),
           std::string("适中"));

  const auto read = fx.world.profile_service.get(uid);
  CHECK(read.is_ok());
  CHECK_EQ(read.value().profile.name, std::string("测试用户"));
  CHECK_NEAR(read.value().profile.weight_kg, 58.5);
  CHECK(read.value().profile.goal == tnycl::profile::Goal::kMoreProtein);
}

TNYCL_TEST(TwoUsersProfilesAreIsolated) {
  Fixture fx;
  const std::string user_a = fx.world.unique_name("it_iso_a");
  const std::string user_b = fx.world.unique_name("it_iso_b");
  CHECK(fx.world.auth.register_user({user_a, "Secret123"}).is_ok());
  CHECK(fx.world.auth.register_user({user_b, "Secret123"}).is_ok());
  const auto login_a = fx.world.auth.login({user_a, "Secret123", "d", "203.0.113.93"});
  const auto login_b = fx.world.auth.login({user_b, "Secret123", "d", "203.0.113.94"});
  CHECK(login_a.is_ok());
  CHECK(login_b.is_ok());

  tnycl::profile::ProfileInput a;
  a.name = "甲";
  a.height_cm = 170;
  a.weight_kg = 60;
  a.age = 28;
  a.goal = tnycl::profile::Goal::kBalanced;
  a.activity = tnycl::profile::Activity::kSedentary;
  a.water_ml = 1500;
  a.sleep_hours = 7;
  a.note = "";

  tnycl::profile::ProfileInput b = a;
  b.name = "乙";
  b.weight_kg = 80;

  CHECK(fx.world.profile_service.put(login_a.value().user_id, a).is_ok());
  CHECK(fx.world.profile_service.put(login_b.value().user_id, b).is_ok());

  CHECK_EQ(fx.world.profile_service.get(login_a.value().user_id).value().profile.name,
           std::string("甲"));
  CHECK_EQ(fx.world.profile_service.get(login_b.value().user_id).value().profile.name,
           std::string("乙"));
  CHECK_EQ(scalar("SELECT COUNT(*) FROM profiles WHERE user_id IN (?, ?)",
                  {std::to_string(login_a.value().user_id),
                   std::to_string(login_b.value().user_id)}),
           std::string("2"));
}

TNYCL_TEST(FoodCatalogReadsSeededRows) {
  Fixture fx;
  const auto page = fx.world.food_service.list(std::nullopt, std::nullopt);
  CHECK(page.is_ok());
  CHECK_EQ(page.value().items.size(), std::size_t{6});

  const auto detail = fx.world.food_service.get("lemon-chicken");
  CHECK(detail.is_ok());
  CHECK_EQ(detail.value().calories, 460);
  CHECK_EQ(detail.value().protein_g, 38);

  // 未知 slug → 4003；数据库层不得抛错。
  CHECK(fx.world.food_service.get("unknown-dish").code() ==
        tnycl::ErrorCode::kFoodNotFound);
}

TNYCL_TEST(SeedDataIsIdempotentAndComplete) {
  // V002 重复执行由 run_mysql_integration.ps1 跑两遍后进入本用例；
  // 这里校验最终态：6 条种子齐全、无重复 slug、无重复 name。
  CHECK_EQ(scalar("SELECT COUNT(*) FROM foods WHERE slug IN ('lemon-chicken','tomato-beef',"
                  "'mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken')"),
           std::string("6"));
  CHECK_EQ(scalar("SELECT COUNT(*) FROM (SELECT slug FROM foods GROUP BY slug HAVING "
                  "COUNT(*) > 1) t"),
           std::string("0"));
  CHECK_EQ(scalar("SELECT COUNT(*) FROM (SELECT name FROM foods GROUP BY name HAVING "
                  "COUNT(DISTINCT slug) > 1) t"),
           std::string("0"));
}

TNYCL_TEST(DuplicateNameWithDifferentSlugFails) {
  Fixture fx;
  const std::string temp_slug = fx.world.unique_name("it-conflict");
  bool failed = false;
  std::string message;
  try {
    exec("INSERT INTO foods (slug, name, category, description, calories, protein_g, fiber_g, "
         "veggies_g, water_ml, image_url, active) VALUES (?, '柠檬鸡胸能量碗', 't', 't', 1, 1, "
         "1, 1, 1, '', 1)",
         {temp_slug});
  } catch (const tnycl::AppError& error) {
    failed = true;
    message = error.what();
  }
  CHECK(failed);
  CHECK(message.find("1062") != std::string::npos ||
        message.find("Duplicate entry") != std::string::npos);
  // 防空转：确认没有插入成功。
  CHECK_EQ(scalar("SELECT COUNT(*) FROM foods WHERE slug = ?", {temp_slug}), std::string("0"));
}

int main() {
  const auto env = env_from_process();
  if (!env.has_value()) {
    std::printf("[SKIP] mysql_integration: 未设置 TNYCL_MYSQL_HOST，跳过（不计为通过）\n");
    return 77;
  }
  g_pool = make_mysql_pool(*env);
  if (g_pool == nullptr) {
    std::printf("[SKIP] mysql_integration: 真实 DbConn 尚未接入（make_mysql_pool 返回空），"
                "跳过（不计为通过）\n");
    return 77;
  }
  return tnycl::test::run_all();
}
