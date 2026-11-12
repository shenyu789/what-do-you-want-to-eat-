#include "repository/food_repository_mysql.h"

#include <string>
#include <utility>
#include <vector>

#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace tnycl::repository {
namespace {

std::uint64_t as_u64(const std::string& text) {
  return static_cast<std::uint64_t>(std::stoll(text));
}

int as_int(const std::string& text) { return std::stoi(text); }

constexpr const char* kFoodColumns =
    "food_id, slug, name, category, description, calories, protein_g, fiber_g, veggies_g, "
    "water_ml, image_url, active";

food::Food to_food(const DbConn::Row& row) {
  food::Food item;
  item.food_id = as_u64(*row.cols[0]);
  item.slug = *row.cols[1];
  item.name = *row.cols[2];
  item.category = *row.cols[3];
  item.description = *row.cols[4];
  item.calories = as_int(*row.cols[5]);
  item.protein_g = as_int(*row.cols[6]);
  item.fiber_g = as_int(*row.cols[7]);
  item.veggies_g = as_int(*row.cols[8]);
  item.water_ml = as_int(*row.cols[9]);
  item.image_url = *row.cols[10];
  item.active = as_int(*row.cols[11]) != 0;
  return item;
}

}  // namespace

MySqlFoodRepository::MySqlFoodRepository(IDbPool& pool, int acquire_timeout_ms)
    : pool_(pool), timeout_ms_(acquire_timeout_ms) {}

std::unique_ptr<DbConn> MySqlFoodRepository::acquire() {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    throw tnycl::AppError(tnycl::ErrorCode::kDbBusy);
  }
  return conn;
}

tnycl::Result<std::vector<food::Food>> MySqlFoodRepository::list_after(
    std::optional<std::uint64_t> after_food_id, int limit) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::vector<food::Food>>::err(tnycl::ErrorCode::kDbBusy);
  }

  std::string sql = std::string("SELECT ") + kFoodColumns + " FROM foods WHERE active = 1";
  std::vector<std::string> params;
  if (after_food_id.has_value()) {
    sql += " AND food_id > ?";
    params.push_back(std::to_string(*after_food_id));
  }
  // LIMIT 不能作为占位符参数绑定：DbConn 契约把所有参数按 MYSQL_TYPE_STRING 发送，
  // 而 MySQL 预处理语句的 LIMIT 必须是整数字面量，绑定字符串会报
  // ERROR 1210 (Incorrect arguments to mysqld_stmt_execute)。
  // 这里 limit 是调用方已校验过的 int（FoodService 保证 1–50，实际传 page_size+1），
  // 拼成字面量不引入注入面；再做一次下界/上界夹取，防止 0、负数或异常大值。
  const int safe_limit = limit < 1 ? 1 : (limit > 1000 ? 1000 : limit);
  sql += " ORDER BY food_id ASC LIMIT " + std::to_string(safe_limit);

  DbConn::Result result;
  try {
    result = conn->exec(sql, params);
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<std::vector<food::Food>>::err(error.code(), error.what());
  }

  std::vector<food::Food> items;
  items.reserve(result.rows.size());
  for (const auto& row : result.rows) {
    items.push_back(to_food(row));
  }
  return tnycl::Result<std::vector<food::Food>>::ok(std::move(items));
}

tnycl::Result<std::optional<food::Food>> MySqlFoodRepository::find_by_slug(
    std::string_view slug) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::optional<food::Food>>::err(tnycl::ErrorCode::kDbBusy);
  }

  DbConn::Result result;
  try {
    result = conn->exec(std::string("SELECT ") + kFoodColumns +
                            " FROM foods WHERE slug = ? AND active = 1 LIMIT 1",
                        {std::string(slug)});
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<std::optional<food::Food>>::err(error.code(), error.what());
  }

  if (result.rows.empty()) {
    return tnycl::Result<std::optional<food::Food>>::ok(std::nullopt);
  }
  return tnycl::Result<std::optional<food::Food>>::ok(
      std::optional<food::Food>{to_food(result.rows.front())});
}

tnycl::Result<std::optional<std::uint64_t>> MySqlFoodRepository::id_by_slug(
    std::string_view slug) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::optional<std::uint64_t>>::err(tnycl::ErrorCode::kDbBusy);
  }

  DbConn::Result result;
  try {
    // 游标解析不过滤 active：已下架的 slug 仍可继续向后翻页。
    result = conn->exec("SELECT food_id FROM foods WHERE slug = ? LIMIT 1",
                        {std::string(slug)});
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<std::optional<std::uint64_t>>::err(error.code(), error.what());
  }

  if (result.rows.empty()) {
    return tnycl::Result<std::optional<std::uint64_t>>::ok(std::nullopt);
  }
  return tnycl::Result<std::optional<std::uint64_t>>::ok(
      std::optional<std::uint64_t>{as_u64(*result.rows.front().cols[0])});
}

}  // namespace tnycl::repository
