// 测试替身：可脚本化响应的 DbConn / IDbPool，用于校验 SQL 适配层的参数与行解析。
// 仅存在于 tests/support，不进入生产构建。
#pragma once

#include <cstddef>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tnycl/db_pool.h"
#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace tnycl::test {

struct FakeDbState {
  std::deque<DbConn::Result> responses;
  std::deque<int> error_numbers;  // 非 0：该次 exec 抛 AppError（模拟 MySQL 错误号）
  std::vector<std::string> sqls;
  std::vector<std::vector<std::string>> params;
  bool acquire_ok{true};
  int last_timeout_ms{0};
};

class FakeDbConn final : public DbConn {
 public:
  explicit FakeDbConn(std::shared_ptr<FakeDbState> state) : state_(std::move(state)) {}

  Result exec(const std::string& sql, const std::vector<std::string>& values) override {
    state_->sqls.push_back(sql);
    state_->params.push_back(values);
    if (!state_->error_numbers.empty()) {
      const int code = state_->error_numbers.front();
      state_->error_numbers.pop_front();
      if (code != 0) {
        err_no_ = code;
        throw tnycl::AppError(tnycl::ErrorCode::kInternalError,
                              code == 1062 ? "Duplicate entry for key 'uk_username'" : "sql error");
      }
    }
    if (state_->responses.empty()) {
      return Result{};
    }
    auto result = state_->responses.front();
    state_->responses.pop_front();
    return result;
  }

  void begin() override {}
  void commit() override {}
  void rollback() override {}
  bool valid() const noexcept override { return true; }
  int err_no() const noexcept override { return err_no_; }
  std::string err_msg() const override { return {}; }

 private:
  std::shared_ptr<FakeDbState> state_;
  int err_no_{0};
};

class FakeDbPool final : public IDbPool {
 public:
  std::shared_ptr<FakeDbState> state = std::make_shared<FakeDbState>();

  std::unique_ptr<DbConn> acquire(int timeout_ms) override {
    state->last_timeout_ms = timeout_ms;
    if (!state->acquire_ok) {
      return nullptr;
    }
    return std::make_unique<FakeDbConn>(state);
  }
  std::size_t idle() const noexcept override { return 1; }
  std::size_t in_use() const noexcept override { return 0; }
  bool healthy() override { return true; }
};

inline std::optional<std::string> cell(const std::string& value) {
  return std::optional<std::string>{value};
}

inline DbConn::Row make_row(std::initializer_list<std::optional<std::string>> cols) {
  DbConn::Row row;
  row.cols.assign(cols.begin(), cols.end());
  return row;
}

inline DbConn::Result make_result(std::initializer_list<DbConn::Row> rows,
                                 std::uint64_t insert_id = 0) {
  DbConn::Result result;
  result.rows.assign(rows.begin(), rows.end());
  result.insert_id = insert_id;
  result.affected = rows.size();
  return result;
}

}  // namespace tnycl::test
