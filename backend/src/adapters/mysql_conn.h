// adapters/mysql_conn.h —— 真实 MySQL DbConn（mysqlclient，T-2.04）
//
// 实现冻结的 DbConn 接口：mysql_stmt_* 参数化执行、结果解析、事务、错误映射。
// 本头文件不 include <mysql.h>（前向声明 MYSQL，避免第三方 C 头泄漏到 include 路径）。
// 生产编译需 libmysqlclient-dev；Ubuntu CI 上链接。错误经 mysql_errno_to_code 映射到统一错误码。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tnycl/db_pool.h"

struct MYSQL;

namespace tnycl {

struct MysqlDeleter {
  void operator()(MYSQL* h) const noexcept;  // mysql_close
};
using MysqlHandle = std::unique_ptr<MYSQL, MysqlDeleter>;

class MySqlConn final : public DbConn {
 public:
  explicit MySqlConn(MysqlHandle handle);
  ~MySqlConn() override;

  Result exec(const std::string& sql,
              const std::vector<std::string>& params) override;
  void begin() override;
  void commit() override;
  void rollback() override;
  bool valid() const noexcept override;  // mysql_ping
  int err_no() const noexcept override;  // 最近一次 MySQL errno
  std::string err_msg() const override;

  // 建立连接（mysql_real_connect + utf8mb4）。失败抛 AppError（经 errno 映射）。
  static MysqlHandle connect(const std::string& host, int port,
                             const std::string& db, const std::string& user,
                             const std::string& password, int connect_timeout_sec);

 private:
  MysqlHandle handle_;
  mutable int last_errno_ = 0;
  mutable std::string last_error_;
};

}  // namespace tnycl
