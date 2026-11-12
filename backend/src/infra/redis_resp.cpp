// infra/redis_resp.cpp —— RESP 编解码实现（纯逻辑）
#include "infra/redis_resp.h"

#include <cstdlib>

namespace tnycl {

std::string resp_encode_command(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) {
    out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  }
  return out;
}

namespace {

bool parse_reply(std::string_view raw, std::size_t& pos, RespReply& out);

bool read_line(std::string_view raw, std::size_t& pos, std::string_view& line) {
  auto p = raw.find("\r\n", pos);
  if (p == std::string_view::npos) return false;
  line = raw.substr(pos, p - pos);
  pos = p + 2;
  return true;
}

bool parse_reply(std::string_view raw, std::size_t& pos, RespReply& out) {
  if (pos >= raw.size()) return false;
  char prefix = raw[pos++];
  switch (prefix) {
    case '+': {  // 简单字符串
      std::string_view line;
      if (!read_line(raw, pos, line)) return false;
      out.type = RespReply::Type::kBulk;  // 简单字符串按 bulk 文本返回
      out.str.assign(line);
      return true;
    }
    case '-': {  // 错误
      std::string_view line;
      if (!read_line(raw, pos, line)) return false;
      out.type = RespReply::Type::kError;
      out.str.assign(line);
      return true;
    }
    case ':': {  // 整数
      std::string_view line;
      if (!read_line(raw, pos, line)) return false;
      out.type = RespReply::Type::kInteger;
      out.integer = std::strtoll(std::string(line).c_str(), nullptr, 10);
      return true;
    }
    case '$': {  // 批量字符串
      std::string_view line;
      if (!read_line(raw, pos, line)) return false;
      long long len = std::strtoll(std::string(line).c_str(), nullptr, 10);
      if (len < 0) {  // $-1 → nil
        out.type = RespReply::Type::kNil;
        return true;
      }
      if (pos + static_cast<std::size_t>(len) + 2 > raw.size()) return false;
      out.type = RespReply::Type::kBulk;
      out.str.assign(raw.substr(pos, static_cast<std::size_t>(len)));
      pos += static_cast<std::size_t>(len);
      if (raw[pos] != '\r' || raw[pos + 1] != '\n') return false;
      pos += 2;
      return true;
    }
    case '*': {  // 数组
      std::string_view line;
      if (!read_line(raw, pos, line)) return false;
      long long n = std::strtoll(std::string(line).c_str(), nullptr, 10);
      if (n < 0) {  // *-1 → nil
        out.type = RespReply::Type::kNil;
        return true;
      }
      out.type = RespReply::Type::kArray;
      out.array.reserve(static_cast<std::size_t>(n));
      for (long long i = 0; i < n; ++i) {
        RespReply sub;
        if (!parse_reply(raw, pos, sub)) return false;
        out.array.push_back(std::move(sub));
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

RespReply resp_decode_reply(std::string_view raw) {
  RespReply out;
  std::size_t pos = 0;
  if (!parse_reply(raw, pos, out)) {
    out = RespReply{};
    out.type = RespReply::Type::kError;
    out.str = "malformed RESP";
  }
  return out;
}

}  // namespace tnycl
