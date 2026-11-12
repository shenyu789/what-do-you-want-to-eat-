// M4 档案应用服务（T-4.01/04.02/04.03）。
// uid 只能由 transport 从会话注入；本服务不接受请求体中的 user_id（AC-M4-07）。
#pragma once

#include <cstdint>

#include "service/account_repository.h"
#include "service/profile_repository.h"
#include "service/profile_types.h"
#include "tnycl/status.h"

namespace tnycl::profile {

struct PutProfileResult {
  double bmi{0.0};
  std::int64_t updated_at_ms{0};
};

class ProfileService {
 public:
  struct Deps {
    ProfileRepository& profiles;
    account::Clock& clock;
    // T-3.06 / 07 §5：读 120/分钟/用户、写 30/分钟/用户（本地降级实现）。
    account::RateLimiter& limiter;
  };

  explicit ProfileService(Deps deps);

  tnycl::Result<ProfileView> get(std::uint64_t user_id);
  tnycl::Result<PutProfileResult> put(std::uint64_t user_id, const ProfileInput& input);

 private:
  Deps deps_;
};

}  // namespace tnycl::profile
