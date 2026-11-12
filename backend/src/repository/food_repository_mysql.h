// M5 MySQL 持久化实现（WBS T-5.02）：foods 只读查询。
// 对外标识始终是 slug；food_id 仅作内部游标，不进入 API（AC-M5-06）。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "service/food_repository.h"
#include "tnycl/db_pool.h"

namespace tnycl::repository {

class MySqlFoodRepository final : public food::FoodRepository {
 public:
  explicit MySqlFoodRepository(IDbPool& pool, int acquire_timeout_ms = 2000);

  tnycl::Result<std::vector<food::Food>> list_after(
      std::optional<std::uint64_t> after_food_id, int limit) override;
  tnycl::Result<std::optional<food::Food>> find_by_slug(std::string_view slug) override;
  tnycl::Result<std::optional<std::uint64_t>> id_by_slug(std::string_view slug) override;

 private:
  std::unique_ptr<DbConn> acquire();

  IDbPool& pool_;
  int timeout_ms_;
};

}  // namespace tnycl::repository
