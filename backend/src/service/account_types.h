// M3 账号领域类型与纯校验逻辑（不依赖任何 IO，可独立单测）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tnycl::account {

struct UserRecord {
  std::uint64_t user_id{0};
  std::string username;
  std::string pass_hash;
  int status{1};  // 1=正常，其他=禁用（2007）
  int fail_count{0};
  std::optional<std::int64_t> locked_until_ms;
  std::int64_t created_at_ms{0};
};

struct SessionRecord {
  std::uint64_t session_id{0};
  std::uint64_t user_id{0};
  std::string token_hash;
  std::string device;
  std::int64_t created_at_ms{0};
  std::int64_t expires_at_ms{0};
};

// 01-SRS FR-A-03：令牌 7 天有效。
inline constexpr std::int64_t kSessionTtlMs = 7LL * 24 * 60 * 60 * 1000;
// 01-SRS FR-A-07：连续失败 5 次锁定 15 分钟。
inline constexpr int kMaxLoginFailures = 5;
inline constexpr std::int64_t kLockDurationMs = 15LL * 60 * 1000;
// 07 §5：同 IP 登录 10 次/分钟。
inline constexpr int kLoginIpLimit = 10;
inline constexpr std::int64_t kLoginIpWindowMs = 60LL * 1000;

// 返回空 optional 表示合法；否则返回面向用户的错误说明。
std::optional<std::string> validate_username(std::string_view username);
std::optional<std::string> validate_password(std::string_view password);

std::size_t utf8_codepoint_count(std::string_view text);
bool is_valid_utf8(std::string_view text);

// 07 / 01-SRS FR-A-08：令牌比较必须常量时间。
bool constant_time_equal(std::string_view a, std::string_view b);

bool is_locked(const UserRecord& user, std::int64_t now_ms);

}  // namespace tnycl::account
