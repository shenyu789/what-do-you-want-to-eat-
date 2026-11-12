// log.cpp —— 结构化 JSON 行日志 + 敏感字段脱敏
#include "tnycl/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace tnycl {
namespace {

std::mutex g_log_mu;

// 最小 JSON 字符串转义（仅处理 " \ 与控制字符）。
std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

const char* level_name(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "info";
}

bool is_sensitive_key(std::string_view key) {
  return key == "password" || key == "pass_hash" || key == "token" ||
         key == "secret" || key == "authorization" || key == "pwd";
}

}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

std::string Logger::redact(std::string_view input) {
  // 对形如 "password":"xxx" / "token":"xxx" 的片段做值替换。
  static const char* keys[] = {"password", "pass_hash", "token", "secret"};
  std::string s(input);
  for (const char* k : keys) {
    std::string pat = std::string("\"") + k + "\":\"";
    std::size_t pos = 0;
    while ((pos = s.find(pat, pos)) != std::string::npos) {
      std::size_t val_start = pos + pat.size();
      std::size_t val_end = s.find('"', val_start);
      if (val_end == std::string::npos) break;
      s.replace(val_start, val_end - val_start, "***");
      pos = val_end;
    }
  }
  return s;
}

void Logger::log(LogLevel level, std::string_view msg, std::string_view trace_id,
                 std::initializer_list<std::string_view> kv) {
  if (static_cast<int>(level) < static_cast<int>(level_)) return;

  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);

  std::string line;
  line.reserve(128 + msg.size());
  line += "{\"ts\":\"";
  line += ts;
  line += ".";
  char ms_buf[8];
  std::snprintf(ms_buf, sizeof(ms_buf), "%03lld", ms % 1000);
  line += ms_buf;
  line += "\",\"level\":\"";
  line += level_name(level);
  line += "\",\"msg\":\"";
  line += json_escape(msg);
  line += '"';
  if (!trace_id.empty()) {
    line += ",\"trace_id\":\"";
    line += json_escape(trace_id);
    line += '"';
  }

  bool key_turn = true;
  std::string cur_key;
  for (std::string_view v : kv) {
    if (key_turn) {
      cur_key.assign(v);
    } else {
      std::string value(v);
      if (is_sensitive_key(cur_key)) value = "***";
      line += ",\"";
      line += json_escape(cur_key);
      line += "\":\"";
      line += json_escape(value);
      line += '"';
    }
    key_turn = !key_turn;
  }
  line += "}\n";

  std::lock_guard<std::mutex> lk(g_log_mu);
  std::fputs(line.c_str(), stdout);
  std::fflush(stdout);
}

}  // namespace tnycl
