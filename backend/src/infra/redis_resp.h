// infra/redis_resp.h —— Redis RESP 协议编解码（纯逻辑，无 hiredis 依赖，可单测）
//
// 仅实现 IRedisClient 冻结接口所需的命令与应答子集：
//   命令：GET/SET/DEL/EXPIRE/INCR/ZADD/ZREVRANGE（RESP 数组编码）
//   应答：简单字符串(+)/错误(-)/整数(:)/批量字符串($)/数组(*)/空值($-1)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tnycl {

// 命令 → RESP 数组（*N\r\n$len\r\narg\r\n...）
std::string resp_encode_command(const std::vector<std::string>& args);

struct RespReply {
  enum class Type { kError, kInteger, kBulk, kNil, kArray } type = Type::kNil;
  std::string str;          // kError/kBulk 的文本
  long long integer = 0;    // kInteger
  std::vector<RespReply> array;  // kArray
};

// 解析**单个完整**应答（从 raw 起始处解析一个 reply；不处理流式残留）。
// 解析失败返回 type=kError 且 str 为错误说明。
RespReply resp_decode_reply(std::string_view raw);

}  // namespace tnycl
