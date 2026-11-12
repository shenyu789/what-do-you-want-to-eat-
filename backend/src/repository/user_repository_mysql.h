// M3 MySQL 持久化实现（WBS T-3.02）：users / user_sessions。
// 依赖 Agent A 冻结的 tnycl/db_pool.h（参数化占位符 '?'，禁止字符串拼接）。
// 时间统一以 epoch 毫秒进出：DATETIME(3) ⇄ UNIX_TIMESTAMP()*1000 + MICROSECOND()/1000。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "service/account_repository.h"
#include "tnycl/db_pool.h"

namespace tnycl::repository {

class MySqlUserRepository final : public account::UserRepository {
 public:
  explicit MySqlUserRepository(IDbPool& pool, int acquire_timeout_ms = 2000);

  std::optional<account::UserRecord> find_by_username(std::string_view username) override;
  std::optional<account::UserRecord> find_by_id(std::uint64_t user_id) override;
  tnycl::Result<std::uint64_t> insert(std::string_view username, std::string_view pass_hash,
                                      std::int64_t now_ms) override;
  tnycl::Result<void> update_login_state(std::uint64_t user_id, int fail_count,
                                         std::optional<std::int64_t> locked_until_ms) override;

 private:
  std::unique_ptr<DbConn> acquire();

  IDbPool& pool_;
  int timeout_ms_;
};

class MySqlSessionRepository final : public account::SessionRepository {
 public:
  explicit MySqlSessionRepository(IDbPool& pool, int acquire_timeout_ms = 2000);

  tnycl::Result<std::uint64_t> insert(std::uint64_t user_id, std::string_view token_hash,
                                      std::string_view device, std::int64_t created_at_ms,
                                      std::int64_t expires_at_ms) override;
  std::optional<account::SessionRecord> find_by_token_hash(std::string_view token_hash) override;
  // V001 user_sessions 无 revoked_at 列：登出即删除行，重复删除幂等。
  tnycl::Result<void> revoke(std::string_view token_hash, std::int64_t now_ms) override;

 private:
  std::unique_ptr<DbConn> acquire();

  IDbPool& pool_;
  int timeout_ms_;
};

}  // namespace tnycl::repository
