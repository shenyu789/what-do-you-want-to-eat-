#include "service/account_types.h"

#include <string>
#include <vector>

namespace tnycl::account {
namespace {

// 解码一个 UTF-8 码点；失败返回 false。
bool decode_one(std::string_view text, std::size_t offset, std::uint32_t* cp, std::size_t* length) {
  const auto byte = static_cast<unsigned char>(text[offset]);
  std::size_t len = 0;
  std::uint32_t value = 0;
  if (byte < 0x80) {
    len = 1;
    value = byte;
  } else if ((byte & 0xE0) == 0xC0) {
    len = 2;
    value = byte & 0x1F;
  } else if ((byte & 0xF0) == 0xE0) {
    len = 3;
    value = byte & 0x0F;
  } else if ((byte & 0xF8) == 0xF0) {
    len = 4;
    value = byte & 0x07;
  } else {
    return false;
  }
  if (offset + len > text.size()) {
    return false;
  }
  for (std::size_t i = 1; i < len; ++i) {
    const auto next = static_cast<unsigned char>(text[offset + i]);
    if ((next & 0xC0) != 0x80) {
      return false;
    }
    value = (value << 6) | (next & 0x3F);
  }
  if ((len == 2 && value < 0x80) || (len == 3 && value < 0x800) || (len == 4 && value < 0x10000)) {
    return false;  // 过长编码
  }
  if (value > 0x10FFFF) {
    return false;
  }
  if (value >= 0xD800 && value <= 0xDFFF) {
    return false;  // 代理区
  }
  *cp = value;
  *length = len;
  return true;
}

std::optional<std::vector<std::uint32_t>> to_codepoints(std::string_view text) {
  std::vector<std::uint32_t> out;
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t len = 0;
    if (!decode_one(text, offset, &cp, &len)) {
      return std::nullopt;
    }
    out.push_back(cp);
    offset += len;
  }
  return out;
}

bool is_control(std::uint32_t cp) { return cp < 0x20 || cp == 0x7F; }

}  // namespace

bool is_valid_utf8(std::string_view text) { return to_codepoints(text).has_value(); }

std::size_t utf8_codepoint_count(std::string_view text) {
  const auto points = to_codepoints(text);
  return points.has_value() ? points->size() : 0;
}

std::optional<std::string> validate_username(std::string_view username) {
  const auto points = to_codepoints(username);
  if (!points.has_value()) {
    return std::string("用户名不是合法的 UTF-8 文本");
  }
  if (points->size() < 4 || points->size() > 32) {
    return std::string("用户名长度必须为 4–32 个字符");
  }
  for (const auto cp : *points) {
    if (is_control(cp)) {
      return std::string("用户名不得包含控制字符");
    }
  }
  if (points->front() == ' ' || points->back() == ' ') {
    return std::string("用户名首尾不得为空格");
  }
  return std::nullopt;
}

std::optional<std::string> validate_password(std::string_view password) {
  if (password.size() < 8) {
    return std::string("密码至少 8 个字符");
  }
  if (password.size() > 128) {
    return std::string("密码不得超过 128 个字符");
  }
  if (!is_valid_utf8(password)) {
    return std::string("密码不是合法的 UTF-8 文本");
  }
  bool has_lower = false;
  bool has_upper = false;
  bool has_digit = false;
  bool has_other = false;
  bool all_same = true;
  const char first = password.front();
  for (const char ch : password) {
    if (ch != first) {
      all_same = false;
    }
    const auto byte = static_cast<unsigned char>(ch);
    if (byte >= 'a' && byte <= 'z') {
      has_lower = true;
    } else if (byte >= 'A' && byte <= 'Z') {
      has_upper = true;
    } else if (byte >= '0' && byte <= '9') {
      has_digit = true;
    } else {
      has_other = true;
    }
  }
  const int classes = static_cast<int>(has_lower) + static_cast<int>(has_upper) +
                      static_cast<int>(has_digit) + static_cast<int>(has_other);
  if (classes < 2) {
    return std::string("密码需包含至少两类字符（小写字母、大写字母、数字、符号）");
  }
  if (all_same) {
    return std::string("密码不得由重复的单一字符组成");
  }
  return std::nullopt;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^
                                             static_cast<unsigned char>(b[i])));
  }
  return diff == 0;
}

bool is_locked(const UserRecord& user, std::int64_t now_ms) {
  return user.locked_until_ms.has_value() && *user.locked_until_ms > now_ms;
}

}  // namespace tnycl::account
