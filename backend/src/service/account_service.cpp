#include "service/account_service.h"

#include <string>
#include <utility>

namespace tnycl::account {
namespace {

constexpr int kTokenCacheTtlSeconds = 30;

// 账号不存在时也执行一次校验，避免用响应时间区分"账号是否存在"（AC-M3-07）。
constexpr char kDummyHash[] =
    "$argon2id$v=19$m=65536,t=3,p=4$ZHVtbXlzYWx0ZHVtbXk$ZHVtbXloYXNodmFsdWU";

}  // namespace

AuthService::AuthService(Deps deps) : deps_(deps) {}

tnycl::Result<RegisterResponse> AuthService::register_user(const RegisterRequest& request) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (request.username.empty()) {
    return Result<RegisterResponse>::err(ErrorCode::kBadRequest, "用户名不能为空");
  }
  if (request.password.empty()) {
    return Result<RegisterResponse>::err(ErrorCode::kBadRequest, "密码不能为空");
  }
  if (const auto invalid = validate_username(request.username); invalid.has_value()) {
    return Result<RegisterResponse>::err(ErrorCode::kValidationFailed, "username: " + *invalid);
  }
  if (const auto weak = validate_password(request.password); weak.has_value()) {
    return Result<RegisterResponse>::err(ErrorCode::kWeakPassword, "password: " + *weak);
  }

  const std::string pass_hash = deps_.hasher.hash(request.password);
  if (pass_hash.empty()) {
    return Result<RegisterResponse>::err(ErrorCode::kInternalError, "密码哈希失败");
  }

  const std::int64_t now = deps_.clock.now_ms();
  auto inserted = deps_.users.insert(request.username, pass_hash, now);
  if (!inserted.is_ok()) {
    return Result<RegisterResponse>::err(inserted.code(), inserted.message());
  }

  RegisterResponse response;
  response.user_id = inserted.value();
  response.username = request.username;
  response.created_at_ms = now;
  return Result<RegisterResponse>::ok(std::move(response));
}

tnycl::Result<LoginResponse> AuthService::login(const LoginRequest& request) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (request.username.empty()) {
    return Result<LoginResponse>::err(ErrorCode::kBadRequest, "用户名不能为空");
  }
  if (request.password.empty()) {
    return Result<LoginResponse>::err(ErrorCode::kBadRequest, "密码不能为空");
  }

  const std::int64_t now = deps_.clock.now_ms();

  // 07 §5：同 IP 登录 10/分钟。无 client_ip（内部调用/单测）时跳过。
  if (!request.client_ip.empty()) {
    const auto retry = deps_.limiter.deny_retry_after_ms("login:ip:" + request.client_ip,
                                                         kLoginIpLimit, kLoginIpWindowMs, now);
    if (retry.has_value()) {
      return Result<LoginResponse>::err(
          ErrorCode::kRateLimited,
          "登录过于频繁，请 " + std::to_string(*retry) + " 毫秒后重试");
    }
  }

  const auto user = deps_.users.find_by_username(request.username);
  if (!user.has_value()) {
    deps_.hasher.verify(request.password, kDummyHash);  // 等时化，响应仍为 2002
    return Result<LoginResponse>::err(ErrorCode::kInvalidCredentials);
  }

  if (is_locked(*user, now)) {
    return Result<LoginResponse>::err(
        ErrorCode::kAccountLocked,
        "账号已锁定至 " + std::to_string(user->locked_until_ms.value_or(0)) + "（epoch ms）");
  }

  if (!deps_.hasher.verify(request.password, user->pass_hash)) {
    const int next_fail_count = user->fail_count + 1;
    std::optional<std::int64_t> locked_until;
    if (next_fail_count >= kMaxLoginFailures) {
      locked_until = now + kLockDurationMs;
    }
    const auto updated =
        deps_.users.update_login_state(user->user_id, next_fail_count, locked_until);
    if (!updated.is_ok()) {
      return Result<LoginResponse>::err(updated.code(), updated.message());
    }
    return Result<LoginResponse>::err(ErrorCode::kInvalidCredentials);
  }

  if (user->status != 1) {
    return Result<LoginResponse>::err(ErrorCode::kUserDisabled);
  }

  if (user->fail_count != 0 || user->locked_until_ms.has_value()) {
    const auto reset = deps_.users.update_login_state(user->user_id, 0, std::nullopt);
    if (!reset.is_ok()) {
      return Result<LoginResponse>::err(reset.code(), reset.message());
    }
  }

  const std::string token = deps_.tokens.generate();
  if (token.empty()) {
    return Result<LoginResponse>::err(ErrorCode::kInternalError, "令牌生成失败");
  }
  const std::string token_hash = deps_.tokens.sha256_hex(token);
  const std::int64_t expires_at = now + kSessionTtlMs;
  const auto session_id =
      deps_.sessions.insert(user->user_id, token_hash, request.device, now, expires_at);
  if (!session_id.is_ok()) {
    return Result<LoginResponse>::err(session_id.code(), session_id.message());
  }

  const auto has_profile = deps_.profiles.has_profile(user->user_id);
  if (!has_profile.is_ok()) {
    return Result<LoginResponse>::err(has_profile.code(), has_profile.message());
  }

  if (deps_.cache != nullptr) {
    Principal principal;
    principal.user_id = user->user_id;
    principal.session_id = session_id.value();
    principal.username = user->username;
    principal.status = user->status;
    principal.expires_at_ms = expires_at;
    deps_.cache->put(token_hash, principal, kTokenCacheTtlSeconds);
  }

  LoginResponse response;
  response.token = token;
  response.expires_at_ms = expires_at;
  response.user_id = user->user_id;
  response.has_profile = has_profile.value();
  return Result<LoginResponse>::ok(std::move(response));
}

tnycl::Result<AuthContext> AuthService::load_context(const std::string& token_hash) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  const auto session = deps_.sessions.find_by_token_hash(token_hash);
  if (!session.has_value()) {  // 不存在或已登出（登出即删除会话行）
    return Result<AuthContext>::err(ErrorCode::kTokenInvalid);
  }
  if (session->expires_at_ms <= deps_.clock.now_ms()) {
    return Result<AuthContext>::err(ErrorCode::kTokenExpired);
  }
  const auto user = deps_.users.find_by_id(session->user_id);
  if (!user.has_value()) {
    return Result<AuthContext>::err(ErrorCode::kTokenInvalid, "令牌对应的账号不存在");
  }
  if (user->status != 1) {
    return Result<AuthContext>::err(ErrorCode::kUserDisabled);
  }

  AuthContext context;
  context.user_id = user->user_id;
  context.session_id = session->session_id;
  context.username = user->username;
  context.status = user->status;
  context.expires_at_ms = session->expires_at_ms;
  return Result<AuthContext>::ok(std::move(context));
}

tnycl::Result<AuthContext> AuthService::authenticate(std::string_view token) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (token.empty()) {
    return Result<AuthContext>::err(ErrorCode::kTokenInvalid, "缺少访问令牌");
  }
  const std::string token_hash = deps_.tokens.sha256_hex(token);

  if (deps_.cache != nullptr) {
    const auto cached = deps_.cache->get(token_hash);
    if (cached.has_value()) {
      if (cached->expires_at_ms <= deps_.clock.now_ms()) {
        deps_.cache->erase(token_hash);
        return Result<AuthContext>::err(ErrorCode::kTokenExpired);
      }
      AuthContext context;
      context.user_id = cached->user_id;
      context.session_id = cached->session_id;
      context.username = cached->username;
      context.status = cached->status;
      context.expires_at_ms = cached->expires_at_ms;
      return Result<AuthContext>::ok(std::move(context));
    }
  }

  auto loaded = load_context(token_hash);
  if (!loaded.is_ok()) {
    return Result<AuthContext>::err(loaded.code(), loaded.message());
  }
  if (deps_.cache != nullptr) {
    const auto& context = loaded.value();
    Principal principal;
    principal.user_id = context.user_id;
    principal.session_id = context.session_id;
    principal.username = context.username;
    principal.status = context.status;
    principal.expires_at_ms = context.expires_at_ms;
    deps_.cache->put(token_hash, principal, kTokenCacheTtlSeconds);
  }
  return loaded;
}

tnycl::Result<void> AuthService::logout(std::string_view token) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  if (token.empty()) {
    return Result<void>::err(ErrorCode::kTokenInvalid, "缺少访问令牌");
  }
  const std::string token_hash = deps_.tokens.sha256_hex(token);
  const auto revoked = deps_.sessions.revoke(token_hash, deps_.clock.now_ms());
  if (!revoked.is_ok()) {
    return Result<void>::err(revoked.code(), revoked.message());
  }
  if (deps_.cache != nullptr) {
    deps_.cache->erase(token_hash);
  }
  return Result<void>::ok();
}

tnycl::Result<MeResponse> AuthService::me(std::string_view token) {
  using tnycl::ErrorCode;
  using tnycl::Result;

  auto context = authenticate(token);
  if (!context.is_ok()) {
    return Result<MeResponse>::err(context.code(), context.message());
  }
  const auto has_profile = deps_.profiles.has_profile(context.value().user_id);
  if (!has_profile.is_ok()) {
    return Result<MeResponse>::err(has_profile.code(), has_profile.message());
  }
  const auto user = deps_.users.find_by_id(context.value().user_id);
  if (!user.has_value()) {
    return Result<MeResponse>::err(ErrorCode::kTokenInvalid, "令牌对应的账号不存在");
  }

  MeResponse response;
  response.user_id = user->user_id;
  response.username = user->username;
  response.status = user->status;
  response.has_profile = has_profile.value();
  response.created_at_ms = user->created_at_ms;
  return Result<MeResponse>::ok(std::move(response));
}

}  // namespace tnycl::account
