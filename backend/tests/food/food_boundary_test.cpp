// M5 食物库分页边界测试（第三阶段 P1）。
// 只覆盖既有用例未覆盖的边界：limit=1 逐页、末项作游标、下架中间项作游标、
// 剩余行数恰好等于 limit、仓储异常时服务层行为。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "service/food_seed.h"
#include "service/food_service.h"
#include "support/food_fakes.h"
#include "support/test_harness.h"
#include "tnycl/error_codes.h"

namespace {

using tnycl::ErrorCode;
using tnycl::food::FoodService;

struct Fixture {
  tnycl::test::FakeFoodRepository foods;
  FoodService service{FoodService::Deps{foods}};
};

}  // namespace

TNYCL_TEST(LimitOneWalksSixPagesInSeedOrder) {
  Fixture fx;
  std::vector<std::string> seen;
  std::optional<std::string> cursor;
  int pages_with_cursor = 0;

  while (true) {
    const auto page = fx.service.list(cursor, 1);
    CHECK(page.is_ok());
    if (!page.is_ok()) {
      break;
    }
    CHECK_EQ(page.value().items.size(), std::size_t{1});
    if (page.value().items.empty()) {
      break;
    }
    seen.push_back(page.value().items.front().slug);
    if (!page.value().next_cursor.has_value()) {
      CHECK_EQ(seen.size(), std::size_t{6});
      break;
    }
    CHECK_EQ(*page.value().next_cursor, page.value().items.front().slug);
    cursor = page.value().next_cursor;
    ++pages_with_cursor;
    CHECK(pages_with_cursor < 6);
  }

  CHECK_EQ(pages_with_cursor, 5);
  CHECK_EQ(seen.size(), std::size_t{6});
  const auto& seed = tnycl::food::builtin_foods();
  for (std::size_t i = 0; i < seen.size() && i < seed.size(); ++i) {
    CHECK_EQ(seen[i], seed[i].slug);
  }
}

TNYCL_TEST(CursorAtLastRowReturnsEmptyPage) {
  Fixture fx;
  const auto empty = fx.service.list(std::string_view("pepper-chicken"), 1);
  CHECK(empty.is_ok());
  CHECK(empty.value().items.empty());
  CHECK(!empty.value().next_cursor.has_value());
}

TNYCL_TEST(DeactivatedMiddleRowStillWorksAsCursor) {
  Fixture fx;
  fx.foods.deactivate("vegetable-rice");  // food_id = 5

  const auto all = fx.service.list(std::nullopt, std::nullopt);
  CHECK(all.is_ok());
  CHECK_EQ(all.value().items.size(), std::size_t{5});
  for (const auto& item : all.value().items) {
    CHECK(item.slug != "vegetable-rice");
  }

  // 游标仍能解析（id_by_slug 不过滤下架），但结果从下一行开始。
  const auto after = fx.service.list(std::string_view("vegetable-rice"), 1);
  CHECK(after.is_ok());
  CHECK_EQ(after.value().items.size(), std::size_t{1});
  if (!after.value().items.empty()) {
    CHECK_EQ(after.value().items.front().slug, std::string("pepper-chicken"));
  }
  CHECK(!after.value().next_cursor.has_value());
}

TNYCL_TEST(LimitEqualToRemainingRowsHasNoNextCursor) {
  Fixture fx;

  const auto five = fx.service.list(std::nullopt, 5);
  CHECK(five.is_ok());
  CHECK_EQ(five.value().items.size(), std::size_t{5});
  CHECK(five.value().next_cursor.has_value());
  if (five.value().next_cursor.has_value()) {
    CHECK_EQ(*five.value().next_cursor, std::string("vegetable-rice"));
  }

  const auto tail = fx.service.list(five.value().next_cursor, 5);
  CHECK(tail.is_ok());
  CHECK_EQ(tail.value().items.size(), std::size_t{1});
  CHECK(!tail.value().next_cursor.has_value());

  const auto exactly_six = fx.service.list(std::nullopt, 6);
  CHECK(exactly_six.is_ok());
  CHECK_EQ(exactly_six.value().items.size(), std::size_t{6});
  CHECK(!exactly_six.value().next_cursor.has_value());
}

TNYCL_TEST(FoodRepositoryFailureMapsToDependencyCodes) {
  Fixture fx;
  fx.foods.fail_with = ErrorCode::kDbBusy;

  const auto list_failed = fx.service.list(std::nullopt, std::nullopt);
  CHECK(!list_failed.is_ok());
  CHECK(list_failed.code() == ErrorCode::kDbBusy);
  CHECK_EQ(tnycl::http_status(list_failed.code()), 503);
  CHECK_EQ(tnycl::error_code(list_failed.code()), 9002);

  const auto cursor_failed = fx.service.list(std::string_view("lemon-chicken"), 2);
  CHECK(cursor_failed.code() == ErrorCode::kDbBusy);

  const auto detail_failed = fx.service.get("lemon-chicken");
  CHECK(detail_failed.code() == ErrorCode::kDbBusy);
  CHECK_EQ(tnycl::http_status(detail_failed.code()), 503);

  fx.foods.fail_with.reset();
  fx.foods.fail_with = ErrorCode::kDbUnavailable;
  const auto unavailable = fx.service.get("lemon-chicken");
  CHECK(unavailable.code() == ErrorCode::kDbUnavailable);
  CHECK_EQ(tnycl::error_code(unavailable.code()), 9001);

  fx.foods.fail_with.reset();
  CHECK(fx.service.list(std::nullopt, std::nullopt).is_ok());
  CHECK(fx.service.get("lemon-chicken").is_ok());
}

int main() { return tnycl::test::run_all(); }
