// M5 测试替身：以内存表模拟 foods 表（含下架过滤），仅存在于测试目录。
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "service/food_repository.h"
#include "service/food_seed.h"
#include "tnycl/status.h"

namespace tnycl::test {

class FakeFoodRepository final : public tnycl::food::FoodRepository {
 public:
  FakeFoodRepository() : foods_(tnycl::food::builtin_foods()) {}

  tnycl::Result<std::vector<tnycl::food::Food>> list_after(
      std::optional<std::uint64_t> after_food_id, int limit) override {
    if (fail_with.has_value()) {
      return tnycl::Result<std::vector<tnycl::food::Food>>::err(*fail_with);
    }
    std::vector<tnycl::food::Food> rows;
    for (const auto& food : foods_) {
      if (!food.active) {
        continue;
      }
      if (after_food_id.has_value() && food.food_id <= *after_food_id) {
        continue;
      }
      rows.push_back(food);
      if (static_cast<int>(rows.size()) >= limit) {
        break;
      }
    }
    return tnycl::Result<std::vector<tnycl::food::Food>>::ok(std::move(rows));
  }

  tnycl::Result<std::optional<tnycl::food::Food>> find_by_slug(
      std::string_view slug) override {
    if (fail_with.has_value()) {
      return tnycl::Result<std::optional<tnycl::food::Food>>::err(*fail_with);
    }
    for (const auto& food : foods_) {
      if (food.slug == slug) {
        return tnycl::Result<std::optional<tnycl::food::Food>>::ok(
            std::optional<tnycl::food::Food>{food});
      }
    }
    return tnycl::Result<std::optional<tnycl::food::Food>>::ok(std::nullopt);
  }

  tnycl::Result<std::optional<std::uint64_t>> id_by_slug(std::string_view slug) override {
    if (fail_with.has_value()) {
      return tnycl::Result<std::optional<std::uint64_t>>::err(*fail_with);
    }
    for (const auto& food : foods_) {
      if (food.slug == slug) {
        return tnycl::Result<std::optional<std::uint64_t>>::ok(
            std::optional<std::uint64_t>{food.food_id});
      }
    }
    return tnycl::Result<std::optional<std::uint64_t>>::ok(std::nullopt);
  }

  void deactivate(std::string_view slug) {
    for (auto& food : foods_) {
      if (food.slug == slug) {
        food.active = false;
      }
    }
  }

  // 错误注入（数据库异常时服务层行为）；默认不注入。
  std::optional<tnycl::ErrorCode> fail_with;

 private:
  std::vector<tnycl::food::Food> foods_;
};

}  // namespace tnycl::test
