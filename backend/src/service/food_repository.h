// M5 食物库持久化端口。SQL 实现见 src/repository/food_repository_mysql.cpp（T-5.02）。
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "service/food_types.h"
#include "tnycl/status.h"

namespace tnycl::food {

class FoodRepository {
 public:
  virtual ~FoodRepository() = default;
  // 只返回 active=1 的行，按 food_id 升序，最多 limit 行；after_food_id 为空表示第一页。
  virtual tnycl::Result<std::vector<Food>> list_after(
      std::optional<std::uint64_t> after_food_id, int limit) = 0;
  // 已下架与不存在同样返回空（service 映射为 4003）。
  virtual tnycl::Result<std::optional<Food>> find_by_slug(std::string_view slug) = 0;
  // 用于把游标 slug 还原为内部 food_id；不存在返回空。
  virtual tnycl::Result<std::optional<std::uint64_t>> id_by_slug(std::string_view slug) = 0;
};

}  // namespace tnycl::food
