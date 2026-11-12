// tnycl/log.h —— 结构化日志（对应 02 §11 / 08 §1）
//
// 约定：
//  - 每行合法 JSON：{ts, level, msg, trace_id, uid, route, code, dur_ms, ...}
//  - 敏感字段（password/token/pass_hash 等）永不输出（redact 白名单过滤）。
//  - trace_id 由请求入口生成，贯穿该请求的所有日志。
#pragma once

#include <string>
#include <string_view>

namespace tnycl {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
 public:
  static Logger& instance();

  void set_level(LogLevel level) noexcept { level_ = level; }
  LogLevel level() const noexcept { return level_; }

  // 核心写接口。kv 为交替的 key/value（额外结构化字段，已脱敏）。
  void log(LogLevel level, std::string_view msg, std::string_view trace_id = {},
           std::initializer_list<std::string_view> kv = {});

  void debug(std::string_view msg, std::string_view trace_id = {}) {
    log(LogLevel::kDebug, msg, trace_id);
  }
  void info(std::string_view msg, std::string_view trace_id = {}) {
    log(LogLevel::kInfo, msg, trace_id);
  }
  void warn(std::string_view msg, std::string_view trace_id = {}) {
    log(LogLevel::kWarn, msg, trace_id);
  }
  void error(std::string_view msg, std::string_view trace_id = {}) {
    log(LogLevel::kError, msg, trace_id);
  }

  // 对 JSON 序列化前的字符串做敏感值脱敏（把 password/token 等值替换为 "***"）。
  // 供日志层调用；返回已脱敏后的字符串。
  static std::string redact(std::string_view input);

 private:
  Logger() = default;
  LogLevel level_ = LogLevel::kInfo;
};

}  // namespace tnycl

// 便捷宏（避免每次传 Logger::instance()）。
#define TNYCL_LOG_DEBUG(...) ::tnycl::Logger::instance().debug(__VA_ARGS__)
#define TNYCL_LOG_INFO(...) ::tnycl::Logger::instance().info(__VA_ARGS__)
#define TNYCL_LOG_WARN(...) ::tnycl::Logger::instance().warn(__VA_ARGS__)
#define TNYCL_LOG_ERROR(...) ::tnycl::Logger::instance().error(__VA_ARGS__)
