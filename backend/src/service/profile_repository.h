// M4 档案持久化端口 + 供 M3 使用的 ProfileLookup 适配器。
// SQL 实现见 src/repository/profile_repository_mysql.cpp（T-4.01）。
#pragma once

#include <cstdint>
#include <optional>

#include "service/account_repository.h"
#include "service/profile_types.h"
#include "tnycl/status.h"

namespace tnycl::profile {

class ProfileRepository {
 public:
  virtual ~ProfileRepository() = default;
  virtual tnycl::Result<std::optional<ProfileRecord>> find(std::uint64_t user_id) = 0;
  // 单行覆盖写（PK=user_id），重复 PUT 幂等（AC-M4-02）。
  virtual tnycl::Result<void> upsert(std::uint64_t user_id, const ProfileRecord& record) = 0;
};

// 把 M4 的档案存在性转换成 M3 的只读契约，避免 M3 直接依赖 profiles 表。
class ProfileLookupAdapter final : public account::ProfileLookup {
 public:
  explicit ProfileLookupAdapter(ProfileRepository& repository) : repository_(repository) {}

  tnycl::Result<bool> has_profile(std::uint64_t user_id) override;

 private:
  ProfileRepository& repository_;
};

}  // namespace tnycl::profile
