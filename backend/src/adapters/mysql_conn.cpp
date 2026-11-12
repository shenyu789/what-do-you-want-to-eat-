// adapters/mysql_conn.cpp —— 真实 MySQL DbConn 实现（mysqlclient，T-2.04）
//
// 编译前提：libmysqlclient-dev（Ubuntu）。本文件在 Windows/MinGW 无 mysql.h 环境不可编译，
// 需在 Ubuntu 24.04 + libmysqlclient 下编译并经真实 MySQL 集成测试验证。
#include "adapters/mysql_conn.h"

#include <mysql.h>

#include <array>
#include <cstring>
#include <stdexcept>

#include "adapters/mysql_error.h"
#include "tnycl/status.h"

namespace tnycl {

void MysqlDeleter::operator()(MYSQL* h) const noexcept {
  if (h) mysql_close(h);
}

MysqlHandle MySqlConn::connect(const std::string& host, int port,
                               const std::string& db, const std::string& user,
                               const std::string& password,
                               int connect_timeout_sec) {
  MYSQL* h = mysql_init(nullptr);
  if (!h) throw AppError(ErrorCode::kInternalError, "mysql_init failed");
  MysqlHandle handle(h);

  mysql_options(h, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_sec);
  if (!mysql_real_connect(h, host.c_str(), user.c_str(), password.c_str(),
                          db.c_str(), port, nullptr, 0)) {
    int err = mysql_errno(h);
    std::string msg = mysql_error(h);
    throw AppError(mysql_errno_to_code(err), msg);
  }
  mysql_set_character_set(h, "utf8mb4");
  return handle;
}

MySqlConn::MySqlConn(MysqlHandle handle) : handle_(std::move(handle)) {}
MySqlConn::~MySqlConn() = default;

namespace {
// RAII 关闭 MYSQL_STMT。
struct StmtGuard {
  MYSQL_STMT* s;
  ~StmtGuard() {
    if (s) mysql_stmt_close(s);
  }
};

[[noreturn]] void throw_stmt_error(MYSQL_STMT* stmt, int& err, std::string& msg) {
  err = static_cast<int>(mysql_stmt_errno(stmt));
  msg = mysql_stmt_error(stmt);
  throw AppError(mysql_errno_to_code(err), msg);
}
}  // namespace

DbConn::Result MySqlConn::exec(const std::string& sql,
                               const std::vector<std::string>& params) {
  last_errno_ = 0;
  last_error_.clear();

  MYSQL_STMT* stmt = mysql_stmt_init(handle_.get());
  if (!stmt) {
    last_errno_ = static_cast<int>(mysql_errno(handle_.get()));
    last_error_ = mysql_error(handle_.get());
    throw AppError(mysql_errno_to_code(last_errno_), last_error_);
  }
  StmtGuard guard{stmt};

  if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0)
    throw_stmt_error(stmt, last_errno_, last_error_);

  // 参数绑定：一律按字符串（DbConn 接口约定）。
  std::vector<MYSQL_BIND> binds(params.size());
  std::vector<std::string> bufs = params;
  for (std::size_t i = 0; i < params.size(); ++i) {
    std::memset(&binds[i], 0, sizeof(MYSQL_BIND));
    binds[i].buffer_type = MYSQL_TYPE_STRING;
    binds[i].buffer = const_cast<char*>(bufs[i].data());
    binds[i].buffer_length = bufs[i].size();
  }
  if (!params.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0)
    throw_stmt_error(stmt, last_errno_, last_error_);

  if (mysql_stmt_execute(stmt) != 0)
    throw_stmt_error(stmt, last_errno_, last_error_);

  Result result;
  result.affected = mysql_stmt_affected_rows(stmt);
  result.insert_id = mysql_stmt_insert_id(stmt);

  // 结果集解析（SELECT）。
  MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
  if (meta) {
    unsigned int ncols = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
    result.columns.reserve(ncols);
    for (unsigned int i = 0; i < ncols; ++i)
      result.columns.emplace_back(fields[i].name);

    if (mysql_stmt_store_result(stmt) != 0)
      throw_stmt_error(stmt, last_errno_, last_error_);

    constexpr std::size_t kColBuf = 8192;
    std::vector<std::array<char, kColBuf>> bufs2(ncols);
    std::vector<unsigned long> lengths(ncols);
    // MySQL 8.0 移除了 my_bool（MYSQL_BIND::is_null 已改为 bool*），
    // 这里必须用 bool；又因为 std::vector<bool> 是位压缩特化、取址非法，
    // 故用 unique_ptr<bool[]> 拿到真正的 bool 数组指针。
    auto is_null = std::make_unique<bool[]>(ncols);
    std::vector<MYSQL_BIND> rb(ncols);
    for (unsigned int i = 0; i < ncols; ++i) {
      std::memset(&rb[i], 0, sizeof(MYSQL_BIND));
      rb[i].buffer_type = MYSQL_TYPE_STRING;
      rb[i].buffer = bufs2[i].data();
      rb[i].buffer_length = kColBuf;
      rb[i].length = &lengths[i];
      rb[i].is_null = &is_null[i];
    }
    if (mysql_stmt_bind_result(stmt, rb.data()) != 0)
      throw_stmt_error(stmt, last_errno_, last_error_);

    int rc = 0;
    while ((rc = mysql_stmt_fetch(stmt)) == 0) {
      Row row;
      row.cols.reserve(ncols);
      for (unsigned int i = 0; i < ncols; ++i) {
        if (is_null[i])
          row.cols.push_back(std::nullopt);
        else
          row.cols.push_back(std::string(bufs2[i].data(), lengths[i]));
      }
      result.rows.push_back(std::move(row));
    }
    if (rc != MYSQL_NO_DATA)  // rc == 1 表示取行出错
      throw_stmt_error(stmt, last_errno_, last_error_);
    mysql_free_result(meta);
  }

  return result;
}

void MySqlConn::begin() {
  if (mysql_query(handle_.get(), "BEGIN") != 0) {
    last_errno_ = static_cast<int>(mysql_errno(handle_.get()));
    last_error_ = mysql_error(handle_.get());
    throw AppError(mysql_errno_to_code(last_errno_), last_error_);
  }
}

void MySqlConn::commit() {
  if (mysql_query(handle_.get(), "COMMIT") != 0) {
    last_errno_ = static_cast<int>(mysql_errno(handle_.get()));
    last_error_ = mysql_error(handle_.get());
    throw AppError(mysql_errno_to_code(last_errno_), last_error_);
  }
}

void MySqlConn::rollback() {
  if (mysql_query(handle_.get(), "ROLLBACK") != 0) {
    last_errno_ = static_cast<int>(mysql_errno(handle_.get()));
    last_error_ = mysql_error(handle_.get());
    throw AppError(mysql_errno_to_code(last_errno_), last_error_);
  }
}

bool MySqlConn::valid() const noexcept {
  return mysql_ping(handle_.get()) == 0;
}

int MySqlConn::err_no() const noexcept { return last_errno_; }

std::string MySqlConn::err_msg() const { return last_error_; }

}  // namespace tnycl
