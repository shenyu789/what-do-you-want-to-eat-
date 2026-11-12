#include "service/profile_types.h"

#include <cmath>
#include <cstdint>

namespace tnycl::profile {
namespace {

bool decode_one(std::string_view text, std::size_t offset, std::uint32_t* cp, std::size_t* len) {
  const auto byte = static_cast<unsigned char>(text[offset]);
  std::size_t length = 0;
  std::uint32_t value = 0;
  if (byte < 0x80) {
    length = 1;
    value = byte;
  } else if ((byte & 0xE0) == 0xC0) {
    length = 2;
    value = byte & 0x1F;
  } else if ((byte & 0xF0) == 0xE0) {
    length = 3;
    value = byte & 0x0F;
  } else if ((byte & 0xF8) == 0xF0) {
    length = 4;
    value = byte & 0x07;
  } else {
    return false;
  }
  if (offset + length > text.size()) {
    return false;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<unsigned char>(text[offset + i]);
    if ((next & 0xC0) != 0x80) {
      return false;
    }
    value = (value << 6) | (next & 0x3F);
  }
  if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
      (length == 4 && value < 0x10000)) {
    return false;
  }
  if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    return false;
  }
  *cp = value;
  *len = length;
  return true;
}

bool has_control_character(std::string_view text) {
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F) {
      return true;
    }
  }
  return false;
}

ValidationIssue issue(std::string field, std::string message) {
  return ValidationIssue{std::move(field), std::move(message)};
}

}  // namespace

std::optional<Goal> parse_goal(std::string_view text) {
  if (text == "均衡饮食") {
    return Goal::kBalanced;
  }
  if (text == "控制体重") {
    return Goal::kWeightControl;
  }
  if (text == "增加蛋白") {
    return Goal::kMoreProtein;
  }
  if (text == "减少添加糖") {
    return Goal::kLessSugar;
  }
  return std::nullopt;
}

std::optional<Activity> parse_activity(std::string_view text) {
  if (text == "久坐") {
    return Activity::kSedentary;
  }
  if (text == "适中") {
    return Activity::kModerate;
  }
  if (text == "较高") {
    return Activity::kHigh;
  }
  return std::nullopt;
}

const char* goal_text(Goal goal) {
  switch (goal) {
    case Goal::kBalanced:
      return "均衡饮食";
    case Goal::kWeightControl:
      return "控制体重";
    case Goal::kMoreProtein:
      return "增加蛋白";
    case Goal::kLessSugar:
      return "减少添加糖";
  }
  return "均衡饮食";
}

const char* activity_text(Activity activity) {
  switch (activity) {
    case Activity::kSedentary:
      return "久坐";
    case Activity::kModerate:
      return "适中";
    case Activity::kHigh:
      return "较高";
  }
  return "久坐";
}

std::size_t utf8_length(std::string_view text) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (!decode_one(text, offset, &cp, &length)) {
      return 0;
    }
    ++count;
    offset += length;
  }
  return count;
}

bool is_valid_utf8(std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (!decode_one(text, offset, &cp, &length)) {
      return false;
    }
    offset += length;
  }
  return true;
}

std::optional<ValidationIssue> validate_profile(const ProfileInput& input) {
  if (!is_valid_utf8(input.name)) {
    return issue("name", "称呼不是合法的 UTF-8 文本");
  }
  const std::size_t name_length = utf8_length(input.name);
  if (name_length < 1 || name_length > 32) {
    return issue("name", "称呼长度必须为 1–32 个字符");
  }
  if (has_control_character(input.name)) {
    return issue("name", "称呼不得包含控制字符");
  }
  if (input.height_cm < kMinHeightCm || input.height_cm > kMaxHeightCm) {
    return issue("height_cm", "身高必须在 120–220 cm 之间");
  }
  if (input.weight_kg < kMinWeightKg || input.weight_kg > kMaxWeightKg) {
    return issue("weight_kg", "体重必须在 30–250 kg 之间");
  }
  if (input.age < kMinAge || input.age > kMaxAge) {
    return issue("age", "年龄必须在 10–100 岁之间");
  }
  if (input.water_ml < kMinWaterMl || input.water_ml > kMaxWaterMl) {
    return issue("water_ml", "饮水目标必须在 1200–2600 ml 之间");
  }
  if ((input.water_ml - kMinWaterMl) % kWaterStepMl != 0) {
    return issue("water_ml", "饮水目标必须以 100 ml 为步长");
  }
  if (input.sleep_hours < kMinSleepHours || input.sleep_hours > kMaxSleepHours) {
    return issue("sleep_hours", "睡眠时长必须在 5–9 小时之间");
  }
  const double doubled = input.sleep_hours * 2.0;
  if (std::fabs(doubled - std::round(doubled)) > 1e-9) {
    return issue("sleep_hours", "睡眠时长必须以 0.5 小时为步长");
  }
  if (!is_valid_utf8(input.note)) {
    return issue("note", "备注不是合法的 UTF-8 文本");
  }
  if (utf8_length(input.note) > kMaxNoteLength) {
    return issue("note", "备注不得超过 255 个字符");
  }
  if (has_control_character(input.note)) {
    return issue("note", "备注不得包含控制字符");
  }
  return std::nullopt;
}

double compute_bmi(int height_cm, double weight_kg) {
  if (height_cm <= 0) {
    return 0.0;
  }
  const double meters = static_cast<double>(height_cm) / 100.0;
  const double bmi = weight_kg / (meters * meters);
  return std::round(bmi * 10.0) / 10.0;
}

}  // namespace tnycl::profile
