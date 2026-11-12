// M3 出站端口（repository / 哈希 / 令牌 / 时钟 / 缓存 / 限流）。
// 错误码与 Result 一律复用 Agent A 冻结的 tnycl/status.h + tnycl/error_codes.h（07 为唯一权威）。
// 基础设施实现由 Agent A 在 adapters/infra 提供；本模块只定义端口，不复制实现。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/account_types.h"
#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace tnycl::account {

class UserRepository {
 public:
  virtual ~UserRepository() = default;
  virtual std::optional<UserRecord> find_by_username(std::string_view username) = 0;
  virtual std::optional<UserRecord> find_by_id(std::uint64_t user_id) = 0;
  // 唯一键冲突必须返回 kUsernameTaken（2001），而不是 1010。
  virtual tnycl::Result<std::uint64_t> insert(std::string_view username,
                                              std::string_view pass_hash,
                                              std::int64_t now_ms) = 0;
  virtual tnycl::Result<void> update_login_state(std::uint64_t user_id, int fail_count,
                                                 std::optional<std::int64_t> locked_until_ms) = 0;
};

class SessionRepository {
 public:
  virtual ~SessionRepository() = default;
  virtual tnycl::Result<std::uint64_t> insert(std::uint64_t user_id,
                                              std::string_view token_hash,
                                              std::string_view device,
                                              std::int64_t created_at_ms,
                                              std::int64_t expires_at_ms) = 0;
  virtual std::optional<SessionRecord> find_by_token_hash(std::string_view token_hash) = 0;
  // 吊销 = 删除会话行（V001 user_sessions 无 revoked_at 列）；重复吊销幂等。
  virtual tnycl::Result<void> revoke(std::string_view token_hash, std::int64_t now_ms) = 0;
};

// 跨模块只读契约：由 M4 的 ProfileRepository 适配（T-4.01）。
// M3 只询问"是否已建档"，不接触 profiles 表结构。
class ProfileLookup {
 public:
  virtual ~ProfileLookup() = default;
  virtual tnycl::Result<bool> has_profile(std::uint64_t user_id) = 0;
};

// Argon2id（01-SRS NFR-S-02，m≥64MB, t≥3）。实现方必须投递到 CPU 池，禁止在 IO 线程调用。
class PasswordHasher {
 public:
  virtual ~PasswordHasher() = default;
  virtual std::string hash(std::string_view password) = 0;
  virtual bool verify(std::string_view password, std::string_view pass_hash) = 0;
};

// 32B 随机令牌 + SHA-256 摘要（01-SRS NFR-S-01）。
class TokenSource {
 public:
  virtual ~TokenSource() = default;
  virtual std::string generate() = 0;
  virtual std::string sha256_hex(std::string_view token) = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual std::int64_t now_ms() const = 0;
};

struct Principal {
  std::uint64_t user_id{0};
  std::uint64_t session_id{0};
  std::string username;
  int status{1};
  std::int64_t expires_at_ms{0};
};

// T-3.05 本地 LRU 兜底：Redis 全挂时鉴权仍可用（AC-M3-10）。
class TokenCache {
 public:
  virtual ~TokenCache() = default;
  virtual std::optional<Principal> get(std::string_view token_hash) = 0;
  virtual void put(const std::string& token_hash, const Principal& principal, int ttl_seconds) = 0;
  virtual void erase(std::string_view token_hash) = 0;
};

// T-3.06 限流端口：返回非空表示拒绝，值为建议退避毫秒数（1006 的 retry_after_ms）。
class RateLimiter {
 public:
  virtual ~RateLimiter() = default;
  virtual std::optional<std::int64_t> deny_retry_after_ms(std::string_view key, int limit,
                                                          std::int64_t window_ms,
                                                          std::int64_t now_ms) = 0;
};

}  // namespace tnycl::account
