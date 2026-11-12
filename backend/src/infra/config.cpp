// config.cpp —— 最小 TOML 子集解析 + 启动自检
#include "tnycl/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "tnycl/status.h"

namespace tnycl {
namespace {

[[noreturn]] void fail(const std::string& what) {
  throw AppError(ErrorCode::kInternalError, "config: " + what);
}

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// 去行内注释（仅当 # / ; 在字符串外时视为注释起始）。
std::string strip_comment(std::string line) {
  bool in_str = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') {
      in_str = !in_str;
    } else if (!in_str && (line[i] == '#' || line[i] == ';')) {
      return line.substr(0, i);
    }
  }
  return line;
}

// 解析后的键：section.key → 原始值字符串（已去引号）。
using Table = std::map<std::string, std::string>;

Table parse_toml(const std::string& content, const std::string& source) {
  Table t;
  std::string section;
  std::istringstream in(content);
  std::string raw;
  int lineno = 0;
  while (std::getline(in, raw)) {
    ++lineno;
    std::string line = trim(strip_comment(raw));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos)
      fail(source + ":" + std::to_string(lineno) + ": 期望 key = value");
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (key.empty()) fail(source + ":" + std::to_string(lineno) + ": 空键");
    if (!val.empty() && val.front() == '"') {
      if (val.size() < 2 || val.back() != '"')
        fail(source + ":" + std::to_string(lineno) + ": 字符串未闭合");
      val = val.substr(1, val.size() - 2);
    }
    std::string full = section.empty() ? key : section + "." + key;
    t[full] = val;
  }
  return t;
}

int get_int(const Table& t, const std::string& key, int def) {
  auto it = t.find(key);
  if (it == t.end()) return def;
  try {
    return std::stoi(it->second);
  } catch (...) {
    fail("键 " + key + " 应为整数，实际为: " + it->second);
  }
}

std::string get_str(const Table& t, const std::string& key,
                    const std::string& def) {
  auto it = t.find(key);
  return it == t.end() ? def : it->second;
}

Config build_config(const Table& t) {
  Config c;
  c.server.bind = get_str(t, "server.bind", c.server.bind);
  c.server.port = get_int(t, "server.port", c.server.port);
  c.server.io_threads = get_int(t, "server.io_threads", c.server.io_threads);
  c.server.max_connections =
      get_int(t, "server.max_connections", c.server.max_connections);
  c.server.keepalive_idle_sec =
      get_int(t, "server.keepalive_idle_sec", c.server.keepalive_idle_sec);
  c.server.read_timeout_sec =
      get_int(t, "server.read_timeout_sec", c.server.read_timeout_sec);
  c.server.max_body_bytes =
      get_int(t, "server.max_body_bytes", c.server.max_body_bytes);

  c.mysql.host = get_str(t, "mysql.host", c.mysql.host);
  c.mysql.port = get_int(t, "mysql.port", c.mysql.port);
  c.mysql.db = get_str(t, "mysql.db", c.mysql.db);
  c.mysql.user = get_str(t, "mysql.user", c.mysql.user);
  c.mysql.password = get_str(t, "mysql.password", c.mysql.password);
  c.mysql.pool_size = get_int(t, "mysql.pool_size", c.mysql.pool_size);
  c.mysql.acquire_timeout_ms =
      get_int(t, "mysql.acquire_timeout_ms", c.mysql.acquire_timeout_ms);

  c.redis.host = get_str(t, "redis.host", c.redis.host);
  c.redis.port = get_int(t, "redis.port", c.redis.port);
  c.redis.db = get_int(t, "redis.db", c.redis.db);
  c.redis.pool_size = get_int(t, "redis.pool_size", c.redis.pool_size);

  c.cpu.threads = get_int(t, "cpu.threads", c.cpu.threads);
  c.cpu.argon2_memory_kb =
      get_int(t, "cpu.argon2_memory_kb", c.cpu.argon2_memory_kb);
  c.cpu.argon2_iterations =
      get_int(t, "cpu.argon2_iterations", c.cpu.argon2_iterations);

  c.business.timezone =
      get_str(t, "business.timezone", c.business.timezone);
  c.business.token_ttl_days =
      get_int(t, "business.token_ttl_days", c.business.token_ttl_days);
  c.business.settle_cron =
      get_str(t, "business.settle_cron", c.business.settle_cron);

  c.auth_cache.local_lru_ttl_sec =
      get_int(t, "auth_cache.local_lru_ttl_sec", c.auth_cache.local_lru_ttl_sec);
  c.auth_cache.local_lru_capacity = get_int(
      t, "auth_cache.local_lru_capacity", c.auth_cache.local_lru_capacity);
  return c;
}

}  // namespace

Config parse_config(std::string_view content, std::string source) {
  Table t = parse_toml(std::string(content), std::move(source));
  Config c = build_config(t);
  validate_config(c);
  return c;
}

Config load_config(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) fail("无法打开文件: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_config(ss.str(), path);
}

void validate_config(const Config& c) {
  if (c.business.timezone != "Asia/Shanghai")
    fail("business.timezone 必须为 Asia/Shanghai，当前: " +
         c.business.timezone);
  if (c.server.port < 1 || c.server.port > 65535)
    fail("server.port 越界: " + std::to_string(c.server.port));
  if (c.server.io_threads < 1) fail("server.io_threads 必须 ≥ 1");
  if (c.mysql.pool_size < c.server.io_threads)
    fail("mysql.pool_size 必须 ≥ server.io_threads");
  if (c.cpu.argon2_memory_kb < 19456)
    fail("cpu.argon2_memory_kb 必须 ≥ 19456");
  if (c.cpu.argon2_iterations < 3) fail("cpu.argon2_iterations 必须 ≥ 3");
  if (c.mysql.acquire_timeout_ms <= 0)
    fail("mysql.acquire_timeout_ms 必须 > 0");
  if (c.server.max_body_bytes < 1) fail("server.max_body_bytes 必须 ≥ 1");
}

std::string example_config_toml() {
  return R"([server]
bind = "0.0.0.0"
port = 8080
io_threads = 4
max_connections = 2000
keepalive_idle_sec = 75
read_timeout_sec = 10
max_body_bytes = 65536

[mysql]
host = "127.0.0.1"
port = 3306
db = "tnycl"
user = "tnycl"
password = ""
pool_size = 8
acquire_timeout_ms = 200

[redis]
host = "127.0.0.1"
port = 6379
db = 0
pool_size = 4

[cpu]
threads = 4
argon2_memory_kb = 65536
argon2_iterations = 3

[business]
timezone = "Asia/Shanghai"
token_ttl_days = 7
settle_cron = "MON 00:05"

[auth_cache]
local_lru_ttl_sec = 30
local_lru_capacity = 10000
)";
}

}  // namespace tnycl
