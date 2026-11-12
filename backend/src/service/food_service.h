// M5 食物库应用服务（T-5.02/05.04）。
// 列表缓存（T-5.03）依赖 Agent A 的 Redis 适配器与统一序列化层，尚未联调，
// 故此处不做进程内缓存，以免与 Redis 版本失效不一致；无缓存实现同样满足 AC-M5-04。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/food_repository.h"
#include "service/food_types.h"
#include "tnycl/status.h"

namespace tnycl::food {

struct FoodPage {
  std::vector<FoodView> items;
  std::optional<std::string> next_cursor;
};

class FoodService {
 public:
  struct Deps {
    FoodRepository& foods;
  };

  explicit FoodService(Deps deps);

  // limit 缺省 20，范围 1–50；非法 → 1002。cursor 为上一页最后一项的 slug。
  tnycl::Result<FoodPage> list(std::optional<std::string_view> cursor, std::optional<int> limit);
  tnycl::Result<FoodView> get(std::string_view slug);

 private:
  Deps deps_;
};

}  // namespace tnycl::food
