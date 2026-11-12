// tnycl/config.h —— 配置加载与启动自检（对应 02 §12）
//
// 启动自检（validate 失败抛 AppError，进程拒绝启动）：
//  - business.timezone 必须为 "Asia/Shanghai"（强制，禁止依赖系统时区）
//  - mysql.pool_size >= server.io_threads
//  - cpu.argon2_memory_kb >= 19456
//  - 端口/线程数/池大小等基本范围校验
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tnycl {

struct ServerConfig {
  std::string bind = "0.0.0.0";
  int port = 8080;
  int io_threads = 4;
  int max_connections = 2000;
  int keepalive_idle_sec = 75;
  int read_timeout_sec = 10;
  int max_body_bytes = 65536;
};

struct MySqlConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string db = "tnycl";
  std::string user;
  std::string password;   // 来自环境变量或未跟踪文件，不入库/入日志
  int pool_size = 8;
  int acquire_timeout_ms = 200;
};

struct RedisConfig {
  std::string host = "127.0.0.1";
  int port = 6379;
  int db = 0;
  int pool_size = 4;
};

struct CpuConfig {
  int threads = 4;
  int argon2_memory_kb = 65536;
  int argon2_iterations = 3;
};

struct BusinessConfig {
  std::string timezone = "Asia/Shanghai";
  int token_ttl_days = 7;
  std::string settle_cron = "MON 00:05";
};

struct AuthCacheConfig {
  int local_lru_ttl_sec = 30;
  int local_lru_capacity = 10000;
};

struct Config {
  ServerConfig server;
  MySqlConfig mysql;
  RedisConfig redis;
  CpuConfig cpu;
  BusinessConfig business;
  AuthCacheConfig auth_cache;
};

// 从 TOML 内容解析配置（不含文件 IO；load_config 读文件后调用本函数）。解析/自检失败抛 AppError。
// 支持的最小 TOML 子集：`[section]`、`key = value`、`key = "string"`、`key = 123`、`key = true`，
// 以及 `;`/`#` 行内注释。嵌套表不支持（当前配置无嵌套）。
Config parse_config(std::string_view content, std::string source = "<string>");

// 从 TOML 文件加载配置；文件不存在/解析失败抛 AppError。
Config load_config(const std::string& path);

// 启动自检；失败抛 AppError 并给出字段名。
void validate_config(const Config& cfg);

// 生成默认示例配置文本（用于 server.example.toml）。
std::string example_config_toml();

}  // namespace tnycl
