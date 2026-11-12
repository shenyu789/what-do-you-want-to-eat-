#include "service/profile_service.h"

#include <string>
#include <utility>

#include "service/account_rate_limiter.h"

namespace tnycl::profile {
namespace {

// 1006：统一限流文案，retry_after_ms 随消息返回（07 §5 暂无 details 字段）。
std::string retry_message(std::int64_t retry_after_ms) {
  return "请求过于频繁，请 " + std::to_string(retry_after_ms) + " 毫秒后重试";
}

}  // namespace

ProfileService::ProfileService(Deps deps) : deps_(deps) {}

tnycl::Result<ProfileView> ProfileService::get(std::uint64_t user_id) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (const auto retry = deps_.limiter.deny_retry_after_ms(
          account::user_quota_key("profile:read", user_id), account::kReadQuotaPerMinute,
          account::kUserQuotaWindowMs, deps_.clock.now_ms())) {
    return Result<ProfileView>::err(ErrorCode::kRateLimited, retry_message(*retry));
  }

  auto found = deps_.profiles.find(user_id);
  if (!found.is_ok()) {
    return Result<ProfileView>::err(found.code(), found.message());
  }
  if (!found.value().has_value()) {
    return Result<ProfileView>::err(ErrorCode::kProfileNotFound);
  }
  const auto& record = *found.value();
  ProfileView view;
  view.profile = record.profile;
  view.bmi = compute_bmi(record.profile.height_cm, record.profile.weight_kg);
  view.updated_at_ms = record.updated_at_ms;
  return Result<ProfileView>::ok(std::move(view));
}

tnycl::Result<PutProfileResult> ProfileService::put(std::uint64_t user_id,
                                                    const ProfileInput& input) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (const auto retry = deps_.limiter.deny_retry_after_ms(
          account::user_quota_key("profile:write", user_id), account::kWriteQuotaPerMinute,
          account::kUserQuotaWindowMs, deps_.clock.now_ms())) {
    return Result<PutProfileResult>::err(ErrorCode::kRateLimited, retry_message(*retry));
  }

  if (const auto invalid = validate_profile(input); invalid.has_value()) {
    return Result<PutProfileResult>::err(ErrorCode::kProfileInvalid,
                                         invalid->field + ": " + invalid->message);
  }

  ProfileRecord record;
  record.profile = input;
  record.updated_at_ms = deps_.clock.now_ms();

  const auto saved = deps_.profiles.upsert(user_id, record);
  if (!saved.is_ok()) {
    return Result<PutProfileResult>::err(saved.code(), saved.message());
  }

  PutProfileResult result;
  result.bmi = compute_bmi(input.height_cm, input.weight_kg);
  result.updated_at_ms = record.updated_at_ms;
  return Result<PutProfileResult>::ok(std::move(result));
}

tnycl::Result<bool> ProfileLookupAdapter::has_profile(std::uint64_t user_id) {
  auto found = repository_.find(user_id);
  if (!found.is_ok()) {
    return tnycl::Result<bool>::err(found.code(), found.message());
  }
  return tnycl::Result<bool>::ok(found.value().has_value());
}

}  // namespace tnycl::profile
