// M3 账号与会话应用服务（T-3.03/03.04/03.05/03.06）。
// 只做业务编排与校验，不含 SQL、不含 Argon2、不含网络；依赖全部通过 Deps 注入。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "service/account_repository.h"
#include "tnycl/status.h"

namespace tnycl::account {

struct RegisterRequest {
  std::string username;
  std::string password;
};

struct RegisterResponse {
  std::uint64_t user_id{0};
  std::string username;
  std::int64_t created_at_ms{0};
};

struct LoginRequest {
  std::string username;
  std::string password;
  std::string device;
  std::string client_ip;
};

struct LoginResponse {
  std::string token;
  std::int64_t expires_at_ms{0};
  std::uint64_t user_id{0};
  bool has_profile{false};
};

struct MeResponse {
  std::uint64_t user_id{0};
  std::string username;
  int status{1};
  bool has_profile{false};
  std::int64_t created_at_ms{0};
};

struct AuthContext {
  std::uint64_t user_id{0};
  std::uint64_t session_id{0};
  std::string username;
  int status{1};
  std::int64_t expires_at_ms{0};
};

class AuthService {
 public:
  struct Deps {
    UserRepository& users;
    SessionRepository& sessions;
    ProfileLookup& profiles;
    PasswordHasher& hasher;
    TokenSource& tokens;
    Clock& clock;
    RateLimiter& limiter;
    TokenCache* cache = nullptr;  // 可为空：等价于不使用本地缓存
  };

  explicit AuthService(Deps deps);

  tnycl::Result<RegisterResponse> register_user(const RegisterRequest& request);
  tnycl::Result<LoginResponse> login(const LoginRequest& request);
  tnycl::Result<void> logout(std::string_view token);
  tnycl::Result<MeResponse> me(std::string_view token);
  // 供 transport 调用：uid 只来自会话（NFR-S-03）。
  tnycl::Result<AuthContext> authenticate(std::string_view token);

 private:
  tnycl::Result<AuthContext> load_context(const std::string& token_hash);

  Deps deps_;
};

}  // namespace tnycl::account
