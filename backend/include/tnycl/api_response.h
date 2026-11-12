// tnycl/api_response.h —— 统一 HTTP 响应结构（唯一权威：docs/07 §1.1 响应包封）
//
// 成功：{ "code":0, "data":{...}, "trace_id":"..." }
// 失败：{ "code":6001, "message":"...", "details":{...}, "trace_id":"..." }
// 规则：HTTP 状态码与 code 同时给出（http_status(code)）；details 仅调试用、不含敏感信息。
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "tnycl/error_codes.h"

namespace tnycl {

struct ApiResponse {
  int code = 0;
  std::string message;      // 仅失败时返回
  std::string trace_id;
  nlohmann::json data{};    // 仅成功时返回（默认 null）
  nlohmann::json details{}; // 仅失败时可选返回（默认 null）

  nlohmann::json to_json() const {
    nlohmann::json j;
    j["code"] = code;
    j["trace_id"] = trace_id;
    if (code == 0) {
      j["data"] = data.is_null() ? nlohmann::json::object() : data;
    } else {
      j["message"] = message;
      if (!details.is_null()) j["details"] = details;
    }
    return j;
  }

  // 序列化为 HTTP body 字符串。
  std::string dump() const { return to_json().dump(); }
};

// 便捷构造器。
inline ApiResponse ok(nlohmann::json data, std::string trace_id = {}) {
  ApiResponse r;
  r.code = 0;
  r.data = std::move(data);
  r.trace_id = std::move(trace_id);
  return r;
}

inline ApiResponse fail(ErrorCode c, std::string trace_id = {},
                        nlohmann::json details = nullptr,
                        std::string custom_message = {}) {
  ApiResponse r;
  r.code = error_code(c);
  r.message = custom_message.empty() ? std::string(error_message(c))
                                     : std::move(custom_message);
  r.details = std::move(details);
  r.trace_id = std::move(trace_id);
  return r;
}

}  // namespace tnycl
