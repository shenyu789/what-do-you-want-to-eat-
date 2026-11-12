// M4 测试替身（仅测试目录，视为仓库层的假实现，不复制生产基础设施）。
#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "service/profile_repository.h"
#include "tnycl/status.h"

namespace tnycl::test {

class FakeProfileRepository final : public tnycl::profile::ProfileRepository {
 public:
  tnycl::Result<std::optional<tnycl::profile::ProfileRecord>> find(
      std::uint64_t user_id) override {
    if (fail_with.has_value()) {
      return tnycl::Result<std::optional<tnycl::profile::ProfileRecord>>::err(*fail_with);
    }
    const auto it = rows.find(user_id);
    if (it == rows.end()) {
      return tnycl::Result<std::optional<tnycl::profile::ProfileRecord>>::ok(std::nullopt);
    }
    return tnycl::Result<std::optional<tnycl::profile::ProfileRecord>>::ok(
        std::optional<tnycl::profile::ProfileRecord>{it->second});
  }

  tnycl::Result<void> upsert(std::uint64_t user_id,
                             const tnycl::profile::ProfileRecord& record) override {
    if (fail_with.has_value()) {
      return tnycl::Result<void>::err(*fail_with);
    }
    rows[user_id] = record;
    ++upsert_calls;
    return tnycl::Result<void>::ok();
  }

  // 错误注入（数据库异常时服务层行为）；默认不注入。
  std::optional<tnycl::ErrorCode> fail_with;
  std::map<std::uint64_t, tnycl::profile::ProfileRecord> rows;
  int upsert_calls{0};
};

}  // namespace tnycl::test
