// M5 食物领域类型（对外标识为字符串 slug，禁止在 API 暴露数字 food_id）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tnycl::food {

struct Food {
  std::uint64_t food_id{0};  // 仅内部使用，不进入 API 响应（AC-M5-06）
  std::string slug;
  std::string name;
  std::string category;
  std::string description;
  int calories{0};
  int protein_g{0};
  int fiber_g{0};
  int veggies_g{0};
  int water_ml{0};
  std::string image_url;
  bool active{true};
};

// 对外视图：刻意不含 food_id，transport 只能序列化此结构（AC-M5-06）。
struct FoodView {
  std::string slug;
  std::string name;
  std::string category;
  std::string description;
  int calories{0};
  int protein_g{0};
  int fiber_g{0};
  int veggies_g{0};
  int water_ml{0};
  std::string image_url;
};

FoodView to_view(const Food& food);

// slug 规则：1–32 字节，小写字母/数字/连字符，首尾非连字符（与 app.js:foods.id 一致）。
bool is_valid_slug(std::string_view slug);

inline constexpr int kDefaultPageSize = 20;
inline constexpr int kMaxPageSize = 50;

}  // namespace tnycl::food
