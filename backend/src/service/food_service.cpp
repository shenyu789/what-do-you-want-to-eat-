#include "service/food_service.h"

#include <utility>

namespace tnycl::food {

FoodService::FoodService(Deps deps) : deps_(deps) {}

tnycl::Result<FoodPage> FoodService::list(std::optional<std::string_view> cursor,
                                          std::optional<int> limit) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  const int page_size = limit.value_or(kDefaultPageSize);
  if (page_size < 1 || page_size > kMaxPageSize) {
    return Result<FoodPage>::err(ErrorCode::kValidationFailed, "limit 必须在 1–50 之间");
  }

  std::optional<std::uint64_t> after_id;
  if (cursor.has_value()) {
    const auto resolved = deps_.foods.id_by_slug(*cursor);
    if (!resolved.is_ok()) {
      return Result<FoodPage>::err(resolved.code(), resolved.message());
    }
    if (!resolved.value().has_value()) {
      return Result<FoodPage>::err(ErrorCode::kCursorInvalid);
    }
    after_id = *resolved.value();
  }

  auto rows = deps_.foods.list_after(after_id, page_size + 1);
  if (!rows.is_ok()) {
    return Result<FoodPage>::err(rows.code(), rows.message());
  }

  FoodPage page;
  const bool has_more = static_cast<int>(rows.value().size()) > page_size;
  if (has_more) {
    rows.value().resize(static_cast<std::size_t>(page_size));
  }
  for (const auto& food : rows.value()) {
    page.items.push_back(to_view(food));
  }
  if (has_more && !page.items.empty()) {
    page.next_cursor = page.items.back().slug;
  }
  return Result<FoodPage>::ok(std::move(page));
}

tnycl::Result<FoodView> FoodService::get(std::string_view slug) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (!is_valid_slug(slug)) {
    return Result<FoodView>::err(ErrorCode::kValidationFailed, "slug 格式不合法");
  }
  auto found = deps_.foods.find_by_slug(slug);
  if (!found.is_ok()) {
    return Result<FoodView>::err(found.code(), found.message());
  }
  if (!found.value().has_value() || !found.value()->active) {
    return Result<FoodView>::err(ErrorCode::kFoodNotFound);
  }
  return Result<FoodView>::ok(to_view(*found.value()));
}

}  // namespace tnycl::food
