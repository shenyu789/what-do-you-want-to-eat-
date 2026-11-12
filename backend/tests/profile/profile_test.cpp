// M4 健康档案单元测试与异常场景测试。
#include <string>

#include "service/profile_service.h"
#include "support/account_fakes.h"
#include "support/profile_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;
using tnycl::profile::Activity;
using tnycl::profile::Goal;
using tnycl::profile::ProfileInput;
using tnycl::profile::ProfileService;

ProfileInput valid_input() {
  ProfileInput input;
  input.name = "我";
  input.height_cm = 170;
  input.weight_kg = 60;
  input.age = 28;
  input.goal = Goal::kBalanced;
  input.activity = Activity::kSedentary;
  input.water_ml = 1800;
  input.sleep_hours = 7.0;
  input.note = "减脂期，晚饭少吃";
  return input;
}

struct Fixture {
  tnycl::test::FakeProfileRepository profiles;
  tnycl::test::FakeClock clock;
  ProfileService service{ProfileService::Deps{profiles, clock}};
};

}  // namespace

TNYCL_TEST(PutThenGetReturnsAllNineFieldsWithBmi) {
  Fixture fx;
  const auto input = valid_input();

  const auto saved = fx.service.put(1, input);
  CHECK(saved.is_ok());
  CHECK_NEAR(saved.value().bmi, 20.8);
  CHECK_EQ(saved.value().updated_at_ms, fx.clock.now);

  const auto loaded = fx.service.get(1);
  CHECK(loaded.is_ok());
  CHECK_EQ(loaded.value().profile.name, std::string("我"));
  CHECK_EQ(loaded.value().profile.height_cm, 170);
  CHECK_NEAR(loaded.value().profile.weight_kg, 60.0);
  CHECK_EQ(loaded.value().profile.age, 28);
  CHECK(loaded.value().profile.goal == Goal::kBalanced);
  CHECK(loaded.value().profile.activity == Activity::kSedentary);
  CHECK_EQ(loaded.value().profile.water_ml, 1800);
  CHECK_NEAR(loaded.value().profile.sleep_hours, 7.0);
  CHECK_EQ(loaded.value().profile.note, std::string("减脂期，晚饭少吃"));
  CHECK_NEAR(loaded.value().bmi, 20.8);
  CHECK_EQ(loaded.value().updated_at_ms, fx.clock.now);
}

TNYCL_TEST(GetWithoutProfileReturns3001) {
  Fixture fx;
  const auto missing = fx.service.get(99);
  CHECK(!missing.is_ok());
  CHECK(missing.code() == ErrorCode::kProfileNotFound);
  CHECK_EQ(tnycl::http_status(missing.code()), 404);
}

TNYCL_TEST(InvalidNumericFieldsReturn3002WithFieldInMessage) {
  Fixture fx;

  auto height = valid_input();
  height.height_cm = 230;
  const auto bad_height = fx.service.put(1, height);
  CHECK(!bad_height.is_ok());
  CHECK(bad_height.code() == ErrorCode::kProfileInvalid);
  CHECK(bad_height.message().find("height_cm") != std::string::npos);
  CHECK_EQ(tnycl::http_status(bad_height.code()), 400);

  auto weight = valid_input();
  weight.weight_kg = 20;
  CHECK(fx.service.put(1, weight).message().find("weight_kg") != std::string::npos);

  auto age = valid_input();
  age.age = 9;
  CHECK(fx.service.put(1, age).message().find("age") != std::string::npos);

  auto water = valid_input();
  water.water_ml = 1250;
  CHECK(fx.service.put(1, water).message().find("water_ml") != std::string::npos);

  auto sleep = valid_input();
  sleep.sleep_hours = 7.3;
  CHECK(fx.service.put(1, sleep).message().find("sleep_hours") != std::string::npos);

  auto name = valid_input();
  name.name = "";
  CHECK(fx.service.put(1, name).message().find("name") != std::string::npos);

  auto note = valid_input();
  note.note = std::string(256, 'x');
  CHECK(fx.service.put(1, note).message().find("note") != std::string::npos);

  CHECK_EQ(fx.profiles.upsert_calls, 0);
}

TNYCL_TEST(EnumWhitelistMatchesFrontendExactly) {
  CHECK(tnycl::profile::goal_text(Goal::kBalanced) == std::string("均衡饮食"));
  CHECK(tnycl::profile::goal_text(Goal::kWeightControl) == std::string("控制体重"));
  CHECK(tnycl::profile::goal_text(Goal::kMoreProtein) == std::string("增加蛋白"));
  CHECK(tnycl::profile::goal_text(Goal::kLessSugar) == std::string("减少添加糖"));

  CHECK(tnycl::profile::parse_goal("均衡饮食").has_value());
  CHECK(tnycl::profile::parse_goal("控制体重").has_value());
  CHECK(tnycl::profile::parse_goal("增加蛋白").has_value());
  CHECK(tnycl::profile::parse_goal("减少添加糖").has_value());
  CHECK(!tnycl::profile::parse_goal("减脂").has_value());
  CHECK(!tnycl::profile::parse_goal("增肌").has_value());
  CHECK(!tnycl::profile::parse_goal("控糖").has_value());
  CHECK(!tnycl::profile::parse_goal("").has_value());

  CHECK(tnycl::profile::activity_text(Activity::kSedentary) == std::string("久坐"));
  CHECK(tnycl::profile::activity_text(Activity::kModerate) == std::string("适中"));
  CHECK(tnycl::profile::activity_text(Activity::kHigh) == std::string("较高"));
  CHECK(tnycl::profile::parse_activity("久坐").has_value());
  CHECK(tnycl::profile::parse_activity("适中").has_value());
  CHECK(tnycl::profile::parse_activity("较高").has_value());
  CHECK(!tnycl::profile::parse_activity("轻度").has_value());
  CHECK(!tnycl::profile::parse_activity("中等").has_value());
}

TNYCL_TEST(BoundariesAreInclusive) {
  Fixture fx;
  auto input = valid_input();
  input.height_cm = 120;
  input.weight_kg = 30;
  input.age = 10;
  input.water_ml = 1200;
  input.sleep_hours = 5.0;
  CHECK(fx.service.put(1, input).is_ok());

  input.height_cm = 220;
  input.weight_kg = 250;
  input.age = 100;
  input.water_ml = 2600;
  input.sleep_hours = 9.0;
  CHECK(fx.service.put(1, input).is_ok());

  input.water_ml = 1150;
  CHECK(!fx.service.put(1, input).is_ok());
  input.water_ml = 2600;
  input.sleep_hours = 9.5;
  CHECK(!fx.service.put(1, input).is_ok());
}

TNYCL_TEST(RepeatedPutIsIdempotentAndKeepsSingleRow) {
  Fixture fx;
  CHECK(fx.service.put(1, valid_input()).is_ok());
  fx.clock.now += 5000;

  auto input = valid_input();
  input.weight_kg = 59.5;
  const auto saved = fx.service.put(1, input);
  CHECK(saved.is_ok());
  CHECK_NEAR(saved.value().bmi, 20.6);
  CHECK_EQ(saved.value().updated_at_ms, fx.clock.now);
  CHECK_EQ(fx.profiles.rows.size(), std::size_t{1});
  CHECK_EQ(fx.profiles.upsert_calls, 2);

  const auto loaded = fx.service.get(1);
  CHECK(loaded.is_ok());
  CHECK_NEAR(loaded.value().profile.weight_kg, 59.5);
}

TNYCL_TEST(ProfilesAreIsolatedPerUser) {
  Fixture fx;
  auto first = valid_input();
  first.name = "我";
  first.water_ml = 1800;
  CHECK(fx.service.put(1, first).is_ok());

  auto second = valid_input();
  second.name = "室友";
  second.water_ml = 2000;
  CHECK(fx.service.put(2, second).is_ok());

  const auto one = fx.service.get(1);
  const auto two = fx.service.get(2);
  CHECK(one.is_ok() && two.is_ok());
  CHECK_EQ(one.value().profile.name, std::string("我"));
  CHECK_EQ(two.value().profile.name, std::string("室友"));
  CHECK_EQ(one.value().profile.water_ml, 1800);
  CHECK_EQ(two.value().profile.water_ml, 2000);
  CHECK(!fx.service.get(3).is_ok());
}

TNYCL_TEST(BmiRoundingAndLookupAdapter) {
  CHECK_NEAR(tnycl::profile::compute_bmi(170, 60), 20.8);
  CHECK_NEAR(tnycl::profile::compute_bmi(200, 100), 25.0);
  CHECK_NEAR(tnycl::profile::compute_bmi(150, 45), 20.0);
  CHECK_NEAR(tnycl::profile::compute_bmi(0, 60), 0.0);

  Fixture fx;
  tnycl::profile::ProfileLookupAdapter adapter(fx.profiles);
  CHECK(!adapter.has_profile(1).value());
  CHECK(fx.service.put(1, valid_input()).is_ok());
  CHECK(adapter.has_profile(1).value());
}

int main() { return tnycl::test::run_all(); }
