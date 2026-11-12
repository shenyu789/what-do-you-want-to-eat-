// M4 MySQL 持久化实现（WBS T-4.01）：profiles 单行覆盖写。
// goal/activity 存 SRS 规定的中文原文，不做数字编码（01-SRS §6.2）。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "service/profile_repository.h"
#include "tnycl/db_pool.h"

namespace tnycl::repository {

class MySqlProfileRepository final : public profile::ProfileRepository {
 public:
  explicit MySqlProfileRepository(IDbPool& pool, int acquire_timeout_ms = 2000);

  tnycl::Result<std::optional<profile::ProfileRecord>> find(std::uint64_t user_id) override;
  // 单行 upsert（PK=user_id）：重复 PUT 幂等（AC-M4-02）。
  tnycl::Result<void> upsert(std::uint64_t user_id,
                             const profile::ProfileRecord& record) override;

 private:
  std::unique_ptr<DbConn> acquire();

  IDbPool& pool_;
  int timeout_ms_;
};

}  // namespace tnycl::repository
