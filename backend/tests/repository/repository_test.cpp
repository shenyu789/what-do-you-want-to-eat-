// M3/M4/M5 SQL 适配层测试：用可脚本化的 FakeDb 校验参数绑定、行解析与错误映射。
// 真实 MySQL 的集成验证在后续联调（T-3.02/T-4.01/T-5.02 的集成用例）补齐。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "repository/food_repository_mysql.h"
#include "repository/profile_repository_mysql.h"
#include "repository/user_repository_mysql.h"
#include "support/fake_db.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;

bool has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

struct Fixture {
  tnycl::test::FakeDbPool pool;
  tnycl::repository::MySqlUserRepository users{pool};
  tnycl::repository::MySqlSessionRepository sessions{pool};
  tnycl::repository::MySqlProfileRepository profiles{pool};
  tnycl::repository::MySqlFoodRepository foods{pool};
};

}  // namespace

TNYCL_TEST(UserFindByIdParsesDecimalEpochAndNullLock) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({tnycl::test::make_row(
      {tnycl::test::cell("7"), tnycl::test::cell("alice2026"),
       tnycl::test::cell("$argon2id$v=19$m=65536,t=3,p=1$abc$def"), tnycl::test::cell("1"),
       tnycl::test::cell("3"), std::nullopt, tnycl::test::cell("1762900000123.000000")})}));

  const auto found = fx.users.find_by_id(7);
  CHECK(found.has_value());
  CHECK_EQ(found->user_id, std::uint64_t{7});
  CHECK_EQ(found->username, std::string("alice2026"));
  CHECK_EQ(found->status, 1);
  CHECK_EQ(found->fail_count, 3);
  CHECK(!found->locked_until_ms.has_value());
  CHECK_EQ(found->created_at_ms, std::int64_t{1762900000123});
  CHECK(has(fx.pool.state->sqls.back(), "FROM users"));
  CHECK_EQ(fx.pool.state->params.back().at(0), std::string("7"));
}

TNYCL_TEST(UserFindByUsernameReturnsNulloptWhenNoRow) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({}));
  CHECK(!fx.users.find_by_username("nobody").has_value());
  CHECK_EQ(fx.pool.state->params.back().at(0), std::string("nobody"));
}

TNYCL_TEST(UserInsertReturnsInsertIdAndMapsDuplicateTo2001) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({}, 42));
  const auto created = fx.users.insert("alice2026", "hash", 1762900000000);
  CHECK(created.is_ok());
  CHECK_EQ(created.value(), std::uint64_t{42});
  CHECK(has(fx.pool.state->sqls.back(), "INSERT INTO users"));
  CHECK(has(fx.pool.state->sqls.back(), "FROM_UNIXTIME"));
  CHECK_EQ(fx.pool.state->params.back().at(2), std::string("1762900000000"));

  fx.pool.state->error_numbers.push_back(1062);
  const auto duplicate = fx.users.insert("alice2026", "hash", 1762900000000);
  CHECK(!duplicate.is_ok());
  CHECK(duplicate.code() == ErrorCode::kUsernameTaken);
  CHECK_EQ(tnycl::http_status(duplicate.code()), 409);
}

TNYCL_TEST(UpdateLoginStateWritesNullOrEpoch) {
  Fixture fx;
  CHECK(fx.users.update_login_state(7, 0, std::nullopt).is_ok());
  CHECK(has(fx.pool.state->sqls.back(), "locked_until = NULL"));
  CHECK_EQ(fx.pool.state->params.back().size(), std::size_t{2});

  CHECK(fx.users.update_login_state(7, 5, std::int64_t{1762900900000}).is_ok());
  CHECK(has(fx.pool.state->sqls.back(), "FROM_UNIXTIME(? / 1000)"));
  CHECK_EQ(fx.pool.state->params.back().at(1), std::string("1762900900000"));
}

TNYCL_TEST(SessionInsertAndLookupHandleEmptyDevice) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({}, 9));
  const auto inserted = fx.sessions.insert(7, "aabb", "", 1762900000000, 1763504800000);
  CHECK(inserted.is_ok());
  CHECK_EQ(inserted.value(), std::uint64_t{9});
  CHECK(has(fx.pool.state->sqls.back(), "NULLIF(?, '')"));

  fx.pool.state->responses.push_back(tnycl::test::make_result({tnycl::test::make_row(
      {tnycl::test::cell("9"), tnycl::test::cell("7"), tnycl::test::cell("aabb"), std::nullopt,
       tnycl::test::cell("1762900000000.000000"),
       tnycl::test::cell("1763504800000.000000")})}));
  const auto found = fx.sessions.find_by_token_hash("aabb");
  CHECK(found.has_value());
  CHECK_EQ(found->session_id, std::uint64_t{9});
  CHECK_EQ(found->device, std::string(""));
  CHECK_EQ(found->expires_at_ms, std::int64_t{1763504800000});
}

TNYCL_TEST(SessionRevokeDeletesRowAndIsIdempotent) {
  Fixture fx;
  CHECK(fx.sessions.revoke("aabb", 1762900000000).is_ok());
  CHECK(has(fx.pool.state->sqls.back(), "DELETE FROM user_sessions WHERE token_hash = ?"));
  // 行不存在时 affected=0，仍返回成功。
  CHECK(fx.sessions.revoke("aabb", 1762900000000).is_ok());
}

TNYCL_TEST(ProfileFindParsesChineseEnumsAndDecimals) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({tnycl::test::make_row(
      {tnycl::test::cell("我"), tnycl::test::cell("170"), tnycl::test::cell("60.0"),
       tnycl::test::cell("28"), tnycl::test::cell("均衡饮食"), tnycl::test::cell("适中"),
       tnycl::test::cell("2000"), tnycl::test::cell("7.5"), std::nullopt,
       tnycl::test::cell("1762900000000.000000")})}));

  const auto found = fx.profiles.find(7);
  CHECK(found.is_ok());
  CHECK(found.value().has_value());
  const auto& record = *found.value();
  CHECK_EQ(record.profile.name, std::string("我"));
  CHECK_EQ(record.profile.height_cm, 170);
  CHECK_NEAR(record.profile.weight_kg, 60.0);
  CHECK(record.profile.goal == tnycl::profile::Goal::kBalanced);
  CHECK(record.profile.activity == tnycl::profile::Activity::kModerate);
  CHECK_EQ(record.profile.water_ml, 2000);
  CHECK_NEAR(record.profile.sleep_hours, 7.5);
  CHECK_EQ(record.profile.note, std::string(""));
  CHECK_EQ(record.updated_at_ms, std::int64_t{1762900000000});
}

TNYCL_TEST(ProfileFindRejectsCorruptEnumValue) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({tnycl::test::make_row(
      {tnycl::test::cell("我"), tnycl::test::cell("170"), tnycl::test::cell("60.0"),
       tnycl::test::cell("28"), tnycl::test::cell("减脂"), tnycl::test::cell("适中"),
       tnycl::test::cell("2000"), tnycl::test::cell("7.5"), std::nullopt,
       tnycl::test::cell("1762900000000.000000")})}));

  const auto found = fx.profiles.find(7);
  CHECK(!found.is_ok());
  CHECK(found.code() == ErrorCode::kInternalError);
}

TNYCL_TEST(ProfileUpsertBindsChineseTextAndOneDecimal) {
  Fixture fx;
  tnycl::profile::ProfileRecord record;
  record.profile.name = "我";
  record.profile.height_cm = 170;
  record.profile.weight_kg = 60.0;
  record.profile.age = 28;
  record.profile.goal = tnycl::profile::Goal::kMoreProtein;
  record.profile.activity = tnycl::profile::Activity::kHigh;
  record.profile.water_ml = 2000;
  record.profile.sleep_hours = 7.5;
  record.profile.note = "";
  record.updated_at_ms = 1762900000000;

  CHECK(fx.profiles.upsert(7, record).is_ok());
  const auto& sql = fx.pool.state->sqls.back();
  CHECK(has(sql, "ON DUPLICATE KEY UPDATE"));
  CHECK(has(sql, "updated_at = VALUES(updated_at)"));
  const auto& params = fx.pool.state->params.back();
  CHECK_EQ(params.at(5), std::string("增加蛋白"));
  CHECK_EQ(params.at(6), std::string("较高"));
  CHECK_EQ(params.at(3), std::string("60.0"));
  CHECK_EQ(params.at(8), std::string("7.5"));
  CHECK_EQ(params.at(10), std::string("1762900000000"));
}

TNYCL_TEST(FoodListAfterBuildsCursorPredicateAndParsesRows) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({tnycl::test::make_row(
      {tnycl::test::cell("3"), tnycl::test::cell("mushroom-soup"),
       tnycl::test::cell("菌菇鸡汤面"), tnycl::test::cell("清淡 · 汤食"),
       tnycl::test::cell("清鸡汤、三种菌菇、细面与小青菜"), tnycl::test::cell("430"),
       tnycl::test::cell("24"), tnycl::test::cell("6"), tnycl::test::cell("160"),
       tnycl::test::cell("450"), tnycl::test::cell("https://example.test/a.jpg"),
       tnycl::test::cell("1")})}));

  const auto rows = fx.foods.list_after(std::uint64_t{2}, 21);
  CHECK(rows.is_ok());
  CHECK_EQ(rows.value().size(), std::size_t{1});
  CHECK_EQ(rows.value().front().food_id, std::uint64_t{3});
  CHECK_EQ(rows.value().front().slug, std::string("mushroom-soup"));
  CHECK_EQ(rows.value().front().veggies_g, 160);
  CHECK(rows.value().front().active);
  CHECK(has(fx.pool.state->sqls.back(), "WHERE active = 1"));
  CHECK(has(fx.pool.state->sqls.back(), "food_id > ?"));
  CHECK_EQ(fx.pool.state->params.back().size(), std::size_t{1});
  CHECK_EQ(fx.pool.state->params.back().at(0), std::string("2"));
  // LIMIT **不是**绑定参数：DbConn 把所有参数按 MYSQL_TYPE_STRING 发送，
  // 而 MySQL 预处理语句的 LIMIT 必须是整数字面量，绑定字符串会报
  // ERROR 1210。这个缺陷 FakeDb 永远发现不了——只有真实库集成测试才暴露，
  // 因此这里断言 SQL 文本里的字面量，而不是 params。
  CHECK(has(fx.pool.state->sqls.back(), "LIMIT 21"));
  CHECK(!has(fx.pool.state->sqls.back(), "LIMIT ?"));
}

TNYCL_TEST(FoodWithoutCursorOmitsPredicate) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({}));
  CHECK(fx.foods.list_after(std::nullopt, 20).is_ok());
  CHECK(!has(fx.pool.state->sqls.back(), "food_id > ?"));
  CHECK_EQ(fx.pool.state->params.back().size(), std::size_t{0});
  CHECK(has(fx.pool.state->sqls.back(), "LIMIT 20"));
}

TNYCL_TEST(FoodFindAndSlugLookup) {
  Fixture fx;
  fx.pool.state->responses.push_back(tnycl::test::make_result({}));
  const auto missing = fx.foods.find_by_slug("unknown-dish");
  CHECK(missing.is_ok());
  CHECK(!missing.value().has_value());
  CHECK(has(fx.pool.state->sqls.back(), "slug = ? AND active = 1"));

  fx.pool.state->responses.push_back(
      tnycl::test::make_result({tnycl::test::make_row({tnycl::test::cell("5")})}));
  const auto id = fx.foods.id_by_slug("vegetable-rice");
  CHECK(id.is_ok());
  CHECK(id.value().has_value());
  CHECK_EQ(*id.value(), std::uint64_t{5});
  CHECK(!has(fx.pool.state->sqls.back(), "active = 1"));
}

TNYCL_TEST(PoolExhaustionMapsTo9002) {
  Fixture fx;
  fx.pool.state->acquire_ok = false;

  const auto inserted = fx.users.insert("alice2026", "hash", 1762900000000);
  CHECK(!inserted.is_ok());
  CHECK(inserted.code() == ErrorCode::kDbBusy);

  bool threw = false;
  try {
    fx.users.find_by_id(7);
  } catch (const tnycl::AppError& error) {
    threw = error.code() == ErrorCode::kDbBusy;
  }
  CHECK(threw);
  CHECK_EQ(fx.pool.state->last_timeout_ms, 2000);
}

int main() { return tnycl::test::run_all(); }
