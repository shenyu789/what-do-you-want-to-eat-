// M4 健康档案领域类型与校验（枚举与前端 index.html:151-152 完全一致）。
// 本模块只负责档案存取；不做健康评分（M7，禁止实现）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tnycl::profile {

// 01-SRS §6.2：数据库存原文字符串，不做数字编码。
enum class Goal { kBalanced, kWeightControl, kMoreProtein, kLessSugar };
enum class Activity { kSedentary, kModerate, kHigh };

std::optional<Goal> parse_goal(std::string_view text);
std::optional<Activity> parse_activity(std::string_view text);
const char* goal_text(Goal goal);
const char* activity_text(Activity activity);

struct ProfileInput {
  std::string name;
  int height_cm{0};
  double weight_kg{0.0};
  int age{0};
  Goal goal{Goal::kBalanced};
  Activity activity{Activity::kSedentary};
  int water_ml{0};
  double sleep_hours{0.0};
  std::string note;
};

struct ProfileRecord {
  ProfileInput profile;
  std::int64_t updated_at_ms{0};
};

struct ProfileView {
  ProfileInput profile;
  double bmi{0.0};
  std::int64_t updated_at_ms{0};
};

struct ValidationIssue {
  std::string field;
  std::string message;
};

// 01-SRS FR-B-06：身高 120–220、体重 30–250、年龄 10–100、饮水 1200–2600(步长 100)、睡眠 5–9(步长 0.5)。
inline constexpr int kMinHeightCm = 120;
inline constexpr int kMaxHeightCm = 220;
inline constexpr double kMinWeightKg = 30.0;
inline constexpr double kMaxWeightKg = 250.0;
inline constexpr int kMinAge = 10;
inline constexpr int kMaxAge = 100;
inline constexpr int kMinWaterMl = 1200;
inline constexpr int kMaxWaterMl = 2600;
inline constexpr int kWaterStepMl = 100;
inline constexpr double kMinSleepHours = 5.0;
inline constexpr double kMaxSleepHours = 9.0;
inline constexpr double kSleepStepHours = 0.5;
inline constexpr std::size_t kMaxNoteLength = 255;

std::optional<ValidationIssue> validate_profile(const ProfileInput& input);

// 01-SRS FR-B-03：BMI 仅展示，不参与评分；四舍五入到 1 位小数（60/170 → 20.8）。
double compute_bmi(int height_cm, double weight_kg);

std::size_t utf8_length(std::string_view text);
bool is_valid_utf8(std::string_view text);

}  // namespace tnycl::profile
