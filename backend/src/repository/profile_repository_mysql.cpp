#include "repository/profile_repository_mysql.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace tnycl::repository {
namespace {

std::int64_t as_i64(const std::string& text) { return std::stoll(text); }

int as_int(const std::string& text) { return std::stoi(text); }

double as_double(const std::string& text) { return std::stod(text); }

std::string one_decimal(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  return std::string(buffer);
}

}  // namespace

MySqlProfileRepository::MySqlProfileRepository(IDbPool& pool, int acquire_timeout_ms)
    : pool_(pool), timeout_ms_(acquire_timeout_ms) {}

std::unique_ptr<DbConn> MySqlProfileRepository::acquire() {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    throw tnycl::AppError(tnycl::ErrorCode::kDbBusy);
  }
  return conn;
}

tnycl::Result<std::optional<profile::ProfileRecord>> MySqlProfileRepository::find(
    std::uint64_t user_id) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::optional<profile::ProfileRecord>>::err(
        tnycl::ErrorCode::kDbBusy);
  }

  DbConn::Result result;
  try {
    result = conn->exec(
        "SELECT name, height_cm, weight_kg, age, goal, activity, water_ml, sleep_hours, note, "
        "CAST(UNIX_TIMESTAMP(updated_at) * 1000 AS SIGNED) + "
        "FLOOR(MICROSECOND(updated_at) / 1000) "
        "FROM profiles WHERE user_id = ? LIMIT 1",
        {std::to_string(user_id)});
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<std::optional<profile::ProfileRecord>>::err(error.code(),
                                                                     error.what());
  }

  if (result.rows.empty()) {
    return tnycl::Result<std::optional<profile::ProfileRecord>>::ok(std::nullopt);
  }

  const auto& row = result.rows.front();
  profile::ProfileInput input;
  input.name = *row.cols[0];
  input.height_cm = as_int(*row.cols[1]);
  input.weight_kg = as_double(*row.cols[2]);
  input.age = as_int(*row.cols[3]);

  const auto goal = profile::parse_goal(*row.cols[4]);
  const auto activity = profile::parse_activity(*row.cols[5]);
  if (!goal.has_value() || !activity.has_value()) {
    // 库里只应存在 01-SRS 规定的四个目标 / 三个活动等级；异常值不得静默兜底。
    return tnycl::Result<std::optional<profile::ProfileRecord>>::err(
        tnycl::ErrorCode::kInternalError, "档案枚举值非法");
  }
  input.goal = *goal;
  input.activity = *activity;
  input.water_ml = as_int(*row.cols[6]);
  input.sleep_hours = as_double(*row.cols[7]);
  input.note = row.cols[8].value_or("");

  profile::ProfileRecord record;
  record.profile = std::move(input);
  record.updated_at_ms = as_i64(*row.cols[9]);
  return tnycl::Result<std::optional<profile::ProfileRecord>>::ok(
      std::optional<profile::ProfileRecord>{std::move(record)});
}

tnycl::Result<void> MySqlProfileRepository::upsert(std::uint64_t user_id,
                                                   const profile::ProfileRecord& record) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<void>::err(tnycl::ErrorCode::kDbBusy);
  }

  const auto& input = record.profile;
  try {
    conn->exec(
        "INSERT INTO profiles (user_id, name, height_cm, weight_kg, age, goal, activity, "
        "water_ml, sleep_hours, note, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, ''), FROM_UNIXTIME(? / 1000)) "
        "ON DUPLICATE KEY UPDATE "
        "name = VALUES(name), height_cm = VALUES(height_cm), weight_kg = VALUES(weight_kg), "
        "age = VALUES(age), goal = VALUES(goal), activity = VALUES(activity), "
        "water_ml = VALUES(water_ml), sleep_hours = VALUES(sleep_hours), "
        "note = VALUES(note), updated_at = VALUES(updated_at)",
        {std::to_string(user_id), input.name, std::to_string(input.height_cm),
         one_decimal(input.weight_kg), std::to_string(input.age),
         std::string(profile::goal_text(input.goal)),
         std::string(profile::activity_text(input.activity)),
         std::to_string(input.water_ml), one_decimal(input.sleep_hours), input.note,
         std::to_string(record.updated_at_ms)});
    return tnycl::Result<void>::ok();
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<void>::err(error.code(), error.what());
  }
}

}  // namespace tnycl::repository
