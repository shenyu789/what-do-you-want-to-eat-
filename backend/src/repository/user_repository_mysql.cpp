#include "repository/user_repository_mysql.h"

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

std::int64_t as_i64(const std::string& text) { return std::stoll(text); }

int as_int(const std::string& text) { return std::stoi(text); }

bool is_duplicate_key(const DbConn& conn, const tnycl::AppError& error) {
  return conn.err_no() == 1062 ||
         std::string(error.what()).find("Duplicate entry") != std::string::npos;
}

account::UserRecord to_user(const DbConn::Row& row) {
  account::UserRecord user;
  user.user_id = as_u64(*row.cols[0]);
  user.username = *row.cols[1];
  user.pass_hash = *row.cols[2];
  user.status = as_int(*row.cols[3]);
  user.fail_count = as_int(*row.cols[4]);
  if (row.cols[5].has_value()) {
    user.locked_until_ms = as_i64(*row.cols[5]);
  }
  user.created_at_ms = as_i64(*row.cols[6]);
  return user;
}

constexpr const char* kUserSelect =
    "SELECT user_id, username, pass_hash, status, fail_count, "
    "CAST(UNIX_TIMESTAMP(locked_until) * 1000 AS SIGNED) + "
    "FLOOR(MICROSECOND(locked_until) / 1000), "
    "CAST(UNIX_TIMESTAMP(created_at) * 1000 AS SIGNED) + "
    "FLOOR(MICROSECOND(created_at) / 1000) "
    "FROM users ";

}  // namespace

MySqlUserRepository::MySqlUserRepository(IDbPool& pool, int acquire_timeout_ms)
    : pool_(pool), timeout_ms_(acquire_timeout_ms) {}

std::unique_ptr<DbConn> MySqlUserRepository::acquire() {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    // 池耗尽按 9002 处理，绝不无限等待（db_pool.h 语义约定）。
    throw tnycl::AppError(tnycl::ErrorCode::kDbBusy);
  }
  return conn;
}

std::optional<account::UserRecord> MySqlUserRepository::find_by_username(
    std::string_view username) {
  auto conn = acquire();
  auto result = conn->exec(std::string(kUserSelect) + "WHERE username = ? LIMIT 1",
                           {std::string(username)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return to_user(result.rows.front());
}

std::optional<account::UserRecord> MySqlUserRepository::find_by_id(std::uint64_t user_id) {
  auto conn = acquire();
  auto result = conn->exec(std::string(kUserSelect) + "WHERE user_id = ? LIMIT 1",
                           {std::to_string(user_id)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return to_user(result.rows.front());
}

tnycl::Result<std::uint64_t> MySqlUserRepository::insert(std::string_view username,
                                                         std::string_view pass_hash,
                                                         std::int64_t now_ms) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kDbBusy);
  }
  try {
    auto result = conn->exec(
        "INSERT INTO users (username, pass_hash, created_at) "
        "VALUES (?, ?, FROM_UNIXTIME(? / 1000))",
        {std::string(username), std::string(pass_hash), std::to_string(now_ms)});
    return tnycl::Result<std::uint64_t>::ok(result.insert_id);
  } catch (const tnycl::AppError& error) {
    // uk_username 冲突必须映射为 2001，不能落到 1010（account_repository.h 契约）。
    if (is_duplicate_key(*conn, error)) {
      return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kUsernameTaken,
                                               "用户名已被占用");
    }
    return tnycl::Result<std::uint64_t>::err(error.code(), error.what());
  }
}

tnycl::Result<void> MySqlUserRepository::update_login_state(
    std::uint64_t user_id, int fail_count, std::optional<std::int64_t> locked_until_ms) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<void>::err(tnycl::ErrorCode::kDbBusy);
  }
  try {
    if (locked_until_ms.has_value()) {
      conn->exec(
          "UPDATE users SET fail_count = ?, locked_until = FROM_UNIXTIME(? / 1000) "
          "WHERE user_id = ?",
          {std::to_string(fail_count), std::to_string(*locked_until_ms),
           std::to_string(user_id)});
    } else {
      conn->exec("UPDATE users SET fail_count = ?, locked_until = NULL WHERE user_id = ?",
                 {std::to_string(fail_count), std::to_string(user_id)});
    }
    return tnycl::Result<void>::ok();
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<void>::err(error.code(), error.what());
  }
}

MySqlSessionRepository::MySqlSessionRepository(IDbPool& pool, int acquire_timeout_ms)
    : pool_(pool), timeout_ms_(acquire_timeout_ms) {}

std::unique_ptr<DbConn> MySqlSessionRepository::acquire() {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    throw tnycl::AppError(tnycl::ErrorCode::kDbBusy);
  }
  return conn;
}

tnycl::Result<std::uint64_t> MySqlSessionRepository::insert(std::uint64_t user_id,
                                                            std::string_view token_hash,
                                                            std::string_view device,
                                                            std::int64_t created_at_ms,
                                                            std::int64_t expires_at_ms) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kDbBusy);
  }
  try {
    auto result = conn->exec(
        "INSERT INTO user_sessions (user_id, token_hash, device, created_at, expires_at) "
        "VALUES (?, ?, NULLIF(?, ''), FROM_UNIXTIME(? / 1000), FROM_UNIXTIME(? / 1000))",
        {std::to_string(user_id), std::string(token_hash), std::string(device),
         std::to_string(created_at_ms), std::to_string(expires_at_ms)});
    return tnycl::Result<std::uint64_t>::ok(result.insert_id);
  } catch (const tnycl::AppError& error) {
    if (is_duplicate_key(*conn, error)) {
      // 32B 随机令牌摘要碰撞不可恢复，按内部错误上报。
      return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kInternalError,
                                               "会话令牌冲突");
    }
    return tnycl::Result<std::uint64_t>::err(error.code(), error.what());
  }
}

std::optional<account::SessionRecord> MySqlSessionRepository::find_by_token_hash(
    std::string_view token_hash) {
  auto conn = acquire();
  auto result = conn->exec(
      "SELECT session_id, user_id, token_hash, device, "
      "CAST(UNIX_TIMESTAMP(created_at) * 1000 AS SIGNED) + "
      "FLOOR(MICROSECOND(created_at) / 1000), "
      "CAST(UNIX_TIMESTAMP(expires_at) * 1000 AS SIGNED) + "
      "FLOOR(MICROSECOND(expires_at) / 1000) "
      "FROM user_sessions WHERE token_hash = ? LIMIT 1",
      {std::string(token_hash)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  const auto& row = result.rows.front();
  account::SessionRecord session;
  session.session_id = as_u64(*row.cols[0]);
  session.user_id = as_u64(*row.cols[1]);
  session.token_hash = *row.cols[2];
  session.device = row.cols[3].value_or("");
  session.created_at_ms = as_i64(*row.cols[4]);
  session.expires_at_ms = as_i64(*row.cols[5]);
  return session;
}

tnycl::Result<void> MySqlSessionRepository::revoke(std::string_view token_hash,
                                                   std::int64_t /*now_ms*/) {
  auto conn = pool_.acquire(timeout_ms_);
  if (!conn) {
    return tnycl::Result<void>::err(tnycl::ErrorCode::kDbBusy);
  }
  try {
    // 行不存在时 affected=0，同样视为成功（幂等）。
    conn->exec("DELETE FROM user_sessions WHERE token_hash = ?", {std::string(token_hash)});
    return tnycl::Result<void>::ok();
  } catch (const tnycl::AppError& error) {
    return tnycl::Result<void>::err(error.code(), error.what());
  }
}

}  // namespace tnycl::repository
