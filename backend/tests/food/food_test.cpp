// M5 食物库单元测试与异常场景测试。
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

template <typename T>
bool message_contains(const tnycl::Result<T>& result, const char* needle) {
  return result.message().find(needle) != std::string::npos;
}

}  // namespace

TNYCL_TEST(SeedMatchesFrontendExactlyWithUniqueSlugs) {
  const auto& foods = tnycl::food::builtin_foods();
  CHECK_EQ(foods.size(), std::size_t{6});

  std::set<std::string> slugs;
  for (const auto& food : foods) {
    CHECK(tnycl::food::is_valid_slug(food.slug));
    slugs.insert(food.slug);
  }
  CHECK_EQ(slugs.size(), std::size_t{6});

  CHECK(foods[0].slug == "lemon-chicken");
  CHECK_EQ(foods[0].name, std::string("柠檬鸡胸能量碗"));
  CHECK_EQ(foods[0].calories, 460);
  CHECK_EQ(foods[0].protein_g, 38);
  CHECK_EQ(foods[0].fiber_g, 9);
  CHECK_EQ(foods[0].veggies_g, 320);
  CHECK_EQ(foods[0].water_ml, 250);

  CHECK(foods[5].slug == "pepper-chicken");
  CHECK_EQ(foods[5].calories, 520);
  CHECK_EQ(foods[5].protein_g, 30);
  CHECK_EQ(foods[5].water_ml, 180);
}

TNYCL_TEST(ListDefaultReturnsAllSixWithoutCursor) {
  Fixture fx;
  const auto page = fx.service.list(std::nullopt, std::nullopt);
  CHECK(page.is_ok());
  CHECK_EQ(page.value().items.size(), std::size_t{6});
  CHECK(!page.value().next_cursor.has_value());
  CHECK_EQ(page.value().items.front().slug, std::string("lemon-chicken"));
  CHECK_EQ(page.value().items.back().slug, std::string("pepper-chicken"));
}

TNYCL_TEST(CursorPaginationWalksAllRowsOnce) {
  Fixture fx;
  std::vector<std::string> seen;
  std::optional<std::string> cursor;
  int pages = 0;
  while (true) {
    const auto page = fx.service.list(cursor, 2);
    CHECK(page.is_ok());
    CHECK(page.value().items.size() <= std::size_t{2});
    for (const auto& item : page.value().items) {
      seen.push_back(item.slug);
    }
    ++pages;
    if (!page.value().next_cursor.has_value()) {
      break;
    }
    cursor = page.value().next_cursor;
    CHECK(pages < 10);
  }
  CHECK_EQ(pages, 3);
  CHECK_EQ(seen.size(), std::size_t{6});
  CHECK_EQ(seen.front(), std::string("lemon-chicken"));
  CHECK_EQ(seen.back(), std::string("pepper-chicken"));
}

TNYCL_TEST(LimitIsValidatedAgainstRange) {
  Fixture fx;
  CHECK(fx.service.list(std::nullopt, 1).is_ok());
  CHECK(fx.service.list(std::nullopt, 50).is_ok());

  const std::vector<int> bad_limits = {0, -1, 51, 100};
  for (const int bad : bad_limits) {
    const auto rejected = fx.service.list(std::nullopt, bad);
    CHECK(!rejected.is_ok());
    CHECK(rejected.code() == ErrorCode::kValidationFailed);
    CHECK(message_contains(rejected, "limit"));
    CHECK_EQ(tnycl::http_status(rejected.code()), 400);
  }
}

TNYCL_TEST(DetailReturnsFullNutritionWithoutInternalId) {
  Fixture fx;
  const auto detail = fx.service.get("lemon-chicken");
  CHECK(detail.is_ok());
  CHECK_EQ(detail.value().slug, std::string("lemon-chicken"));
  CHECK_EQ(detail.value().name, std::string("柠檬鸡胸能量碗"));
  CHECK_EQ(detail.value().category, std::string("高蛋白 · 清爽"));
  CHECK_EQ(detail.value().calories, 460);
  CHECK_EQ(detail.value().protein_g, 38);
  CHECK_EQ(detail.value().fiber_g, 9);
  CHECK_EQ(detail.value().veggies_g, 320);
  CHECK_EQ(detail.value().water_ml, 250);
  CHECK(!detail.value().image_url.empty());
}

TNYCL_TEST(UnknownOrInactiveFoodReturns4003) {
  Fixture fx;
  const auto unknown = fx.service.get("unknown-dish");
  CHECK(!unknown.is_ok());
  CHECK(unknown.code() == ErrorCode::kFoodNotFound);
  CHECK_EQ(tnycl::http_status(unknown.code()), 404);
  CHECK_EQ(tnycl::error_code(unknown.code()), 4003);

  fx.foods.deactivate("mushroom-soup");
  const auto inactive = fx.service.get("mushroom-soup");
  CHECK(!inactive.is_ok());
  CHECK(inactive.code() == ErrorCode::kFoodNotFound);
}

TNYCL_TEST(MalformedSlugIsRejectedBeforeQuery) {
  Fixture fx;
  const std::vector<std::string> malformed = {"Lemon-Chicken", "lemon chicken", "-lemon", "lemon-",
                                              "", "lemon_chicken", std::string(33, 'a')};
  for (const auto& bad : malformed) {
    const auto rejected = fx.service.get(bad);
    CHECK(!rejected.is_ok());
    CHECK(rejected.code() == ErrorCode::kValidationFailed);
    CHECK(message_contains(rejected, "slug"));
    CHECK_EQ(tnycl::http_status(rejected.code()), 400);
  }
  CHECK(tnycl::food::is_valid_slug("vegetable-rice"));
  CHECK(!tnycl::food::is_valid_slug("蔬菜"));
}

TNYCL_TEST(UnknownCursorReturns8001) {
  Fixture fx;
  const std::string bad_cursor = "not-a-real-slug";
  const auto rejected = fx.service.list(std::string_view(bad_cursor), std::nullopt);
  CHECK(!rejected.is_ok());
  CHECK(rejected.code() == ErrorCode::kCursorInvalid);
  CHECK_EQ(tnycl::http_status(rejected.code()), 400);
  CHECK_EQ(tnycl::error_code(rejected.code()), 8001);
}

int main() { return tnycl::test::run_all(); }
