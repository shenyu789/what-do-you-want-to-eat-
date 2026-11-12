// tnycl/db_pool.h —— 数据库连接池接口（W2 冻结契约）
//
// 语义约定：
//  - 所有 SQL 一律参数化（位置占位符 '?'），禁止字符串拼接。
//  - acquire 超时返回 nullopt，调用方映射为 9002 DB_BUSY，绝不无限等待。
//  - DbConn 为 RAII 句柄：析构自动归还连接；事务由 begin/commit/rollback 显式管理，
//    析构时未提交事务自动回滚。
//  - 连接池线程安全；DbConn 不跨线程使用。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tnycl {

class DbConn {
 public:
  struct Row {
    // 每列以字符串返回；NULL 表示为 nullopt。
    std::vector<std::optional<std::string>> cols;
  };
  struct Result {
    std::vector<std::string> columns;  // 列名（SELECT 时有意义）
    std::vector<Row> rows;             // 结果集
    uint64_t affected = 0;             // INSERT/UPDATE/DELETE 影响行数
    uint64_t insert_id = 0;            // 最近 AUTO_INCREMENT
  };

  virtual ~DbConn() = default;

  // 执行一条参数化语句。params 对应 SQL 中的 '?' 占位符（按顺序）。
  // 失败时抛 AppError（SQL 错误 / 连接失效）。
  virtual Result exec(const std::string& sql,
                      const std::vector<std::string>& params = {}) = 0;

  // 事务。begin 之后可多次 exec；commit/rollback 结束。
  virtual void begin() = 0;
  virtual void commit() = 0;
  virtual void rollback() = 0;

  virtual bool valid() const noexcept = 0;
  virtual int err_no() const noexcept = 0;
  virtual std::string err_msg() const = 0;
};

class IDbPool {
 public:
  virtual ~IDbPool() = default;

  // 在 timeout_ms 内获取连接；超时/池耗尽返回 nullopt（→ 9002）。
  virtual std::unique_ptr<DbConn> acquire(int timeout_ms) = 0;

  virtual std::size_t idle() const noexcept = 0;
  virtual std::size_t in_use() const noexcept = 0;

  // 探活：MySQL 可达返回 true；否则 false（用于 /readyz）。
  virtual bool healthy() = 0;
};

}  // namespace tnycl
