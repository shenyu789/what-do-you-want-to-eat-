// M4 档案边界测试（第三阶段 P1）。
// 只覆盖既有用例未涉及的边界：name/note 精确长度与字符集、步长全量枚举、
// BMI 极值一致性、仓储异常时服务层行为。
#include <cstdint>
#include <string>

#include "service/account_rate_limiter.h"
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
  input.note = "";
  return input;
}

struct Fixture {
  tnycl::test::FakeProfileRepository profiles;
  tnycl::test::FakeClock clock;
  tnycl::account::InMemoryRateLimiter limiter;
  ProfileService service{ProfileService::Deps{profiles, clock, limiter}};
};

bool has_field(const tnycl::Result<tnycl::profile::PutProfileResult>& result,
               const std::string& field) {
  return result.message().rfind(field + ": ", 0) == 0;
}

}  // namespace

TNYCL_TEST(ProfileNameAndNoteCharacterBoundaries) {
  Fixture fx;

  auto one = valid_input();
  one.name = "我";  // 1 码点 / 3 字节
  CHECK(fx.service.put(1, one).is_ok());

  auto thirty_two = valid_input();
  thirty_two.name = std::string(32, 'a');
  CHECK(fx.service.put(1, thirty_two).is_ok());

  auto cjk32 = valid_input();
  cjk32.name.clear();
  for (int i = 0; i < 32; ++i) {
    cjk32.name += "我";
  }
  CHECK(fx.service.put(1, cjk32).is_ok());

  auto too_long = valid_input();
  too_long.name = std::string(33, 'a');
  const auto long_name = fx.service.put(1, too_long);
  CHECK(!long_name.is_ok());
  CHECK(long_name.code() == ErrorCode::kProfileInvalid);
  CHECK(has_field(long_name, "name"));

  auto control = valid_input();
  control.name = "ab\tcd";
  CHECK(has_field(fx.service.put(1, control), "name"));

  auto bad_utf8 = valid_input();
  bad_utf8.name = "\xC0\xAF";
  CHECK(has_field(fx.service.put(1, bad_utf8), "name"));

  auto note_255 = valid_input();
  note_255.note = std::string(255, 'x');  // 上限恰好 255
  CHECK(fx.service.put(1, note_255).is_ok());

  auto note_control = valid_input();
  note_control.note = "正常\x01文本";
  CHECK(has_field(fx.service.put(1, note_control), "note"));

  auto note_utf8 = valid_input();
  note_utf8.note = "\xC0\xAF";
  CHECK(has_field(fx.service.put(1, note_utf8), "note"));
}

TNYCL_TEST(WaterAndSleepStepEnumeration) {
  Fixture fx;

  // 饮水：1200–2600，步长 100，共 15 个合法值全部通过。
  for (int ml = tnycl::profile::kMinWaterMl; ml <= tnycl::profile::kMaxWaterMl;
       ml += tnycl::profile::kWaterStepMl) {
    auto input = valid_input();
    input.water_ml = ml;
    CHECK(fx.service.put(1, input).is_ok());
  }
  auto water_off = valid_input();
  water_off.water_ml = 1350;
  const auto water_rejected = fx.service.put(1, water_off);
  CHECK(!water_rejected.is_ok());
  CHECK(has_field(water_rejected, "water_ml"));

  // 睡眠：5.0–9.0，步长 0.5，共 9 个合法值全部通过。
  for (int half = 10; half <= 18; ++half) {
    auto input = valid_input();
    input.sleep_hours = 0.5 * half;
    CHECK(fx.service.put(1, input).is_ok());
  }
  auto sleep_off = valid_input();
  sleep_off.sleep_hours = 5.25;
  const auto sleep_rejected = fx.service.put(1, sleep_off);
  CHECK(!sleep_rejected.is_ok());
  CHECK(has_field(sleep_rejected, "sleep_hours"));
}

TNYCL_TEST(BmiExtremesMatchStoredProfile) {
  Fixture fx;

  struct Case {
    int height_cm;
    double weight_kg;
    double expected_bmi;
  };
  const Case cases[] = {
      {120, 30.0, 20.8},
      {220, 250.0, 51.7},
      {120, 250.0, 173.6},
      {220, 30.0, 6.2},
  };

  for (const auto& item : cases) {
    auto input = valid_input();
    input.height_cm = item.height_cm;
    input.weight_kg = item.weight_kg;
    const auto saved = fx.service.put(1, input);
    CHECK(saved.is_ok());
    if (saved.is_ok()) {
      CHECK_NEAR(saved.value().bmi, item.expected_bmi);
    }
    const auto loaded = fx.service.get(1);
    CHECK(loaded.is_ok());
    if (loaded.is_ok()) {
      CHECK_EQ(loaded.value().profile.height_cm, item.height_cm);
      CHECK_NEAR(loaded.value().profile.weight_kg, item.weight_kg);
      CHECK_NEAR(loaded.value().bmi, item.expected_bmi);
    }
  }
}

TNYCL_TEST(ProfileRepositoryFailureMapsToDependencyCodes) {
  Fixture fx;

  fx.profiles.fail_with = ErrorCode::kDbBusy;
  const auto busy = fx.service.get(1);
  CHECK(!busy.is_ok());
  CHECK(busy.code() == ErrorCode::kDbBusy);
  CHECK_EQ(tnycl::http_status(busy.code()), 503);
  CHECK_EQ(tnycl::error_code(busy.code()), 9002);

  const auto write_failed = fx.service.put(1, valid_input());
  CHECK(!write_failed.is_ok());
  CHECK(write_failed.code() == ErrorCode::kDbBusy);
  CHECK_EQ(tnycl::http_status(write_failed.code()), 503);
  CHECK_EQ(fx.profiles.upsert_calls, 0);
  CHECK(fx.profiles.rows.empty());

  fx.profiles.fail_with = ErrorCode::kDbUnavailable;
  const auto unavailable = fx.service.get(1);
  CHECK(unavailable.code() == ErrorCode::kDbUnavailable);
  CHECK_EQ(tnycl::error_code(unavailable.code()), 9001);

  // 恢复后行为不受影响。
  fx.profiles.fail_with.reset();
  CHECK(fx.service.put(1, valid_input()).is_ok());
  CHECK(fx.service.get(1).is_ok());
}

int main() { return tnycl::test::run_all(); }
