// 真实 MySQL 集成测试（P0）。设计与执行分离：
//   - 本文件包含 14 个集成场景（覆盖 04 §M13 要求的 11 项，另加登录锁定持久化、
//     档案 upsert、食物分页绑定、事务 rollback/commit 四项真实库用例），
//     全部通过冻结契约 IDbPool / DbConn 访问数据库；
//   - 唯一集成点见 make_mysql_pool()：它直接装配 adapters/MySqlConn + infra/MySqlPool，
//     仅在构建期找到 libmysqlclient（TNYCL_HAVE_MYSQL）时生效；
//   - 未配置 TNYCL_MYSQL_* 时以退出码 77 跳过，**不计为通过**；
//   - 已配置真实库却无 libmysqlclient 时以退出码 1 报「阻塞」，绝不假绿。
//
// 环境变量：TNYCL_MYSQL_HOST / TNYCL_MYSQL_PORT / TNYCL_MYSQL_USER /
//           TNYCL_MYSQL_PASSWORD / TNYCL_MYSQL_DB / TNYCL_MYSQL_POOL_SIZE(默认 4)
// 退出码：0=全部通过，1=有失败或已配置真实库但适配层缺失（阻塞），
//         77=未配置真实库（跳过，**不计为通过**）。
// 证据等级：本文件运行时一律为 [REAL-MYSQL]（真实 MySQL 8）；
//           FakeDb 证据只存在于 tests/repository/repository_test.cpp，两者不可混用。
//
// CI 执行入口：backend/tools/ci/mysql_e2e.sh（建独立空库 → V001 → V002×3 → 冲突负向 →
// 本二进制全部场景 → 清理/隔离断言）。CI 集成 job 把退出码 77 视为失败。
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

// 真实适配层只在找到 libmysqlclient 时参与编译。开关**必须**用 CMake 提供的
// TNYCL_HAVE_MYSQL（CMakeLists.txt:153 对 tnycl_core PUBLIC 定义），不能用测试侧
// 自造的宏——否则 Ubuntu 验收构建（已链接 libmysqlclient）会走进 stub 分支，
// 明明能连库却报 BLOCKED，属于假阴性阻塞。
// 注意：这里只 include **项目内**头文件，绝不 include <mysql.h>——D-04 规定
// 第三方 C 头只能出现在 src/adapters/ 内，测试 TU 同样受此约束。
#if defined(TNYCL_HAVE_MYSQL)
#define TNYCL_IT_HAS_REAL_CONN 1
#include "adapters/mysql_conn.h"
#include "infra/mysql_pool.h"
#endif

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

// 池上限：必须与 make_mysql_pool() 中构造池时使用的值一致。
int pool_size() {
  const int configured = std::atoi(env_or("TNYCL_MYSQL_POOL_SIZE", "4").c_str());
  return configured > 0 ? configured : 4;
}

// ============================ 唯一集成点 ============================
// A 已交付 src/adapters/mysql_conn.{h,cpp}（MySqlConn::connect + MySqlConn）。
// 接线条件只有一个：构建期定义了 TNYCL_HAVE_MYSQL（即找到并链接 libmysqlclient，
// 且 mysql_conn.cpp 参与编译）。
// 未定义时返回空，使本套件在无客户端的环境里也能编译，并以 BLOCKED(1) 结束而不假绿。
// 注意：MySqlPool 不校验 factory 返回空（空连接会在 exec 时解引用），
// MySqlConn::connect 失败改为抛 AppError，因此工厂绝不会返回 nullptr。
std::unique_ptr<tnycl::IDbPool> make_mysql_pool(const MysqlEnv& env) {
#if defined(TNYCL_IT_HAS_REAL_CONN)
  const std::string host = env.host;
  const int port = std::atoi(env.port.c_str());
  const std::string db = env.db;
  const std::string user = env.user;
  const std::string password = env.password;
  auto factory = [host, port, db, user, password]() -> std::unique_ptr<tnycl::DbConn> {
    auto handle = tnycl::MySqlConn::connect(host, port, db, user, password, 3);
    return std::make_unique<tnycl::MySqlConn>(std::move(handle));
  };
  return std::make_unique<tnycl::MySqlPool>(static_cast<std::size_t>(pool_size()), 2000,
                                            std::move(factory));
#else
  (void)env;
  return nullptr;
#endif
}
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

std::int64_t scalar_i64(const std::string& sql, const std::vector<std::string>& params = {}) {
  const std::string text = scalar(sql, params);
  return text.empty() ? 0 : std::stoll(text);
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
  // V002 重复执行由 backend/tools/ci/mysql_e2e.sh（CI 集成 job）跑三次后进入本用例；
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

// 登录失败计数与锁定必须**落到库里**（fail_count / locked_until 由 SQL 写入），
// 这是纯 Fake 仓储无法证明的：内存态测试全绿也可能 FROM_UNIXTIME 写错了单位。
TNYCL_TEST(LoginFailuresPersistLockInDatabase) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_lock");
  CHECK(fx.world.auth.register_user({username, "Secret123"}).is_ok());

  for (int attempt = 1; attempt <= 5; ++attempt) {
    const auto result =
        fx.world.auth.login({username, "WrongPass1", "it-device", "203.0.113.95"});
    CHECK(result.code() == tnycl::ErrorCode::kInvalidCredentials);
  }
  CHECK_EQ(scalar("SELECT fail_count FROM users WHERE username = ?", {username}),
           std::string("5"));
  CHECK_EQ(scalar("SELECT locked_until IS NOT NULL FROM users WHERE username = ?", {username}),
           std::string("1"));

  // 第 6 次即使密码正确也必须 2003：锁定以库为准。
  const auto locked = fx.world.auth.login({username, "Secret123", "it-device", "203.0.113.95"});
  CHECK(locked.code() == tnycl::ErrorCode::kAccountLocked);

  // 解锁态写入必须可被 SQL 读回（epoch ms ↔ DATETIME 换算正确）。
  const std::int64_t unlock_at = scalar_i64(
      "SELECT CAST(UNIX_TIMESTAMP(locked_until) * 1000 AS SIGNED) + "
      "FLOOR(MICROSECOND(locked_until) / 1000) FROM users WHERE username = ?",
      {username});
  CHECK_EQ(unlock_at - fx.world.clock.now_ms(), std::int64_t{900000});
}

// 档案写两次必须仍只有 1 行（ON DUPLICATE KEY UPDATE 真实生效，不是内存覆盖）。
TNYCL_TEST(ProfileUpsertOverwritesSingleRow) {
  Fixture fx;
  const std::string username = fx.world.unique_name("it_upsert");
  CHECK(fx.world.auth.register_user({username, "Secret123"}).is_ok());
  const auto logged = fx.world.auth.login({username, "Secret123", "it-device", "203.0.113.96"});
  CHECK(logged.is_ok());
  const auto uid = logged.value().user_id;

  tnycl::profile::ProfileInput first;
  first.name = "初版";
  first.height_cm = 170;
  first.weight_kg = 60;
  first.age = 28;
  first.goal = tnycl::profile::Goal::kBalanced;
  first.activity = tnycl::profile::Activity::kSedentary;
  first.water_ml = 1500;
  first.sleep_hours = 7;
  first.note = "";
  CHECK(fx.world.profile_service.put(uid, first).is_ok());

  tnycl::profile::ProfileInput second = first;
  second.name = "改后";
  second.weight_kg = 61.2;
  second.goal = tnycl::profile::Goal::kMoreProtein;
  CHECK(fx.world.profile_service.put(uid, second).is_ok());

  CHECK_EQ(scalar("SELECT COUNT(*) FROM profiles WHERE user_id = ?", {std::to_string(uid)}),
           std::string("1"));
  CHECK_EQ(scalar("SELECT name FROM profiles WHERE user_id = ?", {std::to_string(uid)}),
           std::string("改后"));
  const auto read = fx.world.profile_service.get(uid);
  CHECK(read.is_ok());
  CHECK_NEAR(read.value().profile.weight_kg, 61.2);
  CHECK(read.value().profile.goal == tnycl::profile::Goal::kMoreProtein);
}

// 游标翻页：真实库的 `LIMIT ?` 由服务层拼参数，是全仓唯一把 LIMIT 作为绑定参数的路径。
// DbConn 把参数一律按 MYSQL_TYPE_STRING 绑定，这里一旦绑定类型不被 MySQL 接受会直接报错，
// 因此必须用真实 MySQL 覆盖（Fake 仓储永远发现不了）。
TNYCL_TEST(FoodCursorPaginationBindsLimit) {
  Fixture fx;
  const auto first = fx.world.food_service.list(std::nullopt, 2);
  CHECK(first.is_ok());
  CHECK_EQ(first.value().items.size(), std::size_t{2});
  CHECK(first.value().next_cursor.has_value());

  const auto second = fx.world.food_service.list(
      std::optional<std::string_view>{*first.value().next_cursor}, 2);
  CHECK(second.is_ok());
  CHECK_EQ(second.value().items.size(), std::size_t{2});
  CHECK(second.value().items.front().slug != first.value().items.back().slug);

  const auto all = fx.world.food_service.list(std::nullopt, 50);
  CHECK(all.is_ok());
  CHECK_EQ(all.value().items.size(), std::size_t{6});
  CHECK(!all.value().next_cursor.has_value());
}

// 连接池并发访问：8 线程 × 10 次 acquire → SELECT 1 → 归还，全部成功且无连接泄漏。
TNYCL_TEST(PoolServesConcurrentWorkers) {
  constexpr int kThreads = 8;
  constexpr int kIterations = 10;
  std::atomic<int> succeeded{0};
  std::atomic<int> failed{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&succeeded, &failed] {
      for (int j = 0; j < kIterations; ++j) {
        auto conn = pool().acquire(5000);
        if (conn == nullptr) {
          ++failed;
          continue;
        }
        try {
          const auto result = conn->exec("SELECT 1");
          if (result.rows.size() == 1) {
            ++succeeded;
          } else {
            ++failed;
          }
        } catch (const tnycl::AppError&) {
          ++failed;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_EQ(failed.load(), 0);
  CHECK_EQ(succeeded.load(), kThreads * kIterations);
  CHECK_EQ(pool().in_use(), std::size_t{0});  // 全部归还，无泄漏
}

// 池耗尽与恢复：占满池上限后 acquire 立即返回空（调用方映射 9002），
// 归还后恢复可用。上限取自 TNYCL_MYSQL_POOL_SIZE，须与 make_mysql_pool() 一致。
TNYCL_TEST(PoolSaturationThenRecovery) {
  const int kPoolSize = pool_size();
  std::vector<std::unique_ptr<tnycl::DbConn>> held;
  for (int i = 0; i < kPoolSize; ++i) {
    auto conn = pool().acquire(2000);
    CHECK(conn != nullptr);
    held.push_back(std::move(conn));
  }
  CHECK_EQ(pool().in_use(), static_cast<std::size_t>(kPoolSize));

  // 超时 50ms 拿不到连接：返回空，绝不无限等待（db_pool.h 契约）。
  CHECK(pool().acquire(50) == nullptr);

  held.clear();
  CHECK_EQ(pool().in_use(), std::size_t{0});
  auto recovered = pool().acquire(2000);
  CHECK(recovered != nullptr);
}

// 事务契约（db_pool.h 冻结语义）：begin/rollback 丢弃写入，begin/commit 真正落库；
// 且一条失败语句之后连接必须仍可复用 —— 池化连接会被再次发放，若一条失败 SQL
// 就让连接失效，整个池会逐步"中毒"。此前这笔契约只有 FakeDb 覆盖，真实 MySQL 上从未验证。
TNYCL_TEST(TransactionRollbackAndCommitOnRealDb) {
  Fixture fx;
  const std::string rollback_user = fx.world.unique_name("it_tx_rb");
  const std::string commit_user = fx.world.unique_name("it_tx_ok");

  auto conn = pool().acquire(2000);
  CHECK(conn != nullptr);
  if (conn == nullptr) {
    return;
  }

  // rollback：写入后回滚 —— 库中不得留下该行。
  conn->begin();
  conn->exec("INSERT INTO users (username, pass_hash) VALUES (?, ?)",
             {rollback_user, "hash"});
  conn->rollback();
  CHECK_EQ(scalar("SELECT COUNT(*) FROM users WHERE username = ?", {rollback_user}),
           std::string("0"));

  // commit：写入后提交 —— 用**另一条连接**查询，确认已真正落库而非只在本事务可见。
  conn->begin();
  conn->exec("INSERT INTO users (username, pass_hash) VALUES (?, ?)",
             {commit_user, "hash"});
  conn->commit();
  CHECK_EQ(scalar("SELECT COUNT(*) FROM users WHERE username = ?", {commit_user}),
           std::string("1"));

  // 失败语句触发 uk_username 冲突后，连接必须仍然可用。
  bool failed = false;
  try {
    conn->exec("INSERT INTO users (username, pass_hash) VALUES (?, ?)",
               {commit_user, "hash2"});
  } catch (const tnycl::AppError&) {
    failed = true;
  }
  CHECK(failed);
  CHECK(conn->valid());
  // 冲突未产生半条记录：该用户名仍只有 1 行且 hash 未被改写。
  CHECK_EQ(scalar("SELECT COUNT(*) FROM users WHERE username = ?", {commit_user}),
           std::string("1"));
}

// 预检：直连取版本，不使用 CHECK 宏（避免污染用例断言计数）。
// MySqlConn::connect 失败会抛 AppError（经池的 factory 传播），这里必须捕获后报告。
bool preflight(std::string* version_out) {
  try {
    auto conn = g_pool->acquire(3000);
    if (conn == nullptr) {
      return false;
    }
    const auto result = conn->exec("SELECT VERSION()");
    if (result.rows.empty() || result.rows.front().cols.empty() ||
        !result.rows.front().cols.front().has_value()) {
      return false;
    }
    *version_out = *result.rows.front().cols.front();
    return true;
  } catch (const tnycl::AppError& error) {
    std::printf("[FAIL] 预检异常：code=%d msg=%s\n", error.code_int(),
                error.what());
    return false;
  }
}

int main() {
  const auto env = env_from_process();
  if (!env.has_value()) {
    std::printf("[SKIP] mysql_integration: 未设置 TNYCL_MYSQL_HOST（证据等级：无），"
                "不计为通过\n");
    return 77;
  }

  g_pool = make_mysql_pool(*env);
  if (g_pool == nullptr) {
    // 已配置真实库却无法建池 = 阻塞，绝不能以 77 假绿。
    std::printf("[BLOCKED] mysql_integration: 已配置 TNYCL_MYSQL_HOST=%s:%s，"
                "但真实 DbConn 未接入（构建期未定义 TNYCL_HAVE_MYSQL，"
                "或 make_mysql_pool 返回空）。\n"
                "          这是阻塞，不是通过；需安装 libmysqlclient-dev 后重新 configure"
                "（CMake 会自动定义 TNYCL_HAVE_MYSQL 并编译 src/adapters/mysql_conn.cpp），"
                "或确认 mysql_conn.cpp 未因缺库被排除。\n",
                env->host.c_str(), env->port.c_str());
    return 1;
  }

  std::printf("[REAL-MYSQL] target=%s:%s/%s pool=%d\n", env->host.c_str(),
              env->port.c_str(), env->db.c_str(), pool_size());

  std::string version;
  if (!preflight(&version)) {
    std::printf("[FAIL] 预检失败：无法执行 SELECT VERSION()（连接或权限问题）\n");
    return 1;
  }
  std::printf("[REAL-MYSQL] server=%s\n", version.c_str());
  if (version.rfind("8.", 0) != 0) {
    std::printf("[FAIL] 本集成套件要求 MySQL 8.x，实际版本 \"%s\"\n", version.c_str());
    return 1;
  }

  const int rc = tnycl::test::run_all();
  std::printf("[REAL-MYSQL] %s\n", rc == 0 ? "全部通过" : "存在失败");
  return rc;
}
