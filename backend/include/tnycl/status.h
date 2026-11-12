// tnycl/status.h —— 内部错误基类与结果类型（服务层到传输层的异常契约）
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "tnycl/error_codes.h"

namespace tnycl {

// 携带业务错误码的异常。Service/Repository 抛出，transport 层统一捕获后转 ApiResponse。
// 约定：业务错误用 AppError 抛；未捕获的 std::exception 归为 1010 INTERNAL_ERROR。
class AppError : public std::runtime_error {
 public:
  explicit AppError(ErrorCode code, std::string message = {})
      : std::runtime_error(message.empty() ? std::string(error_message(code))
                                           : std::move(message)),
        code_(code) {}

  ErrorCode code() const noexcept { return code_; }
  int code_int() const noexcept { return error_code(code_); }
  int http() const noexcept { return http_status(code_); }

 private:
  ErrorCode code_;
};

// 内部流控结果：成功带值，失败带错误码。避免异常用于正常分支。
template <typename T>
class Result {
 public:
  static Result ok(T value) { return Result(std::move(value), ErrorCode::kOk); }
  static Result err(ErrorCode c, std::string msg = {}) {
    return Result(std::nullopt, c, std::move(msg));
  }

  bool is_ok() const noexcept { return code_ == ErrorCode::kOk; }
  const T& value() const { return *value_; }
  T& value() { return *value_; }
  const T& operator*() const { return *value_; }
  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  explicit operator bool() const noexcept { return is_ok(); }

 private:
  Result(T value, ErrorCode c) : value_(std::move(value)), code_(c) {}
  Result(std::nullopt_t, ErrorCode c, std::string msg)
      : code_(c), message_(std::move(msg)) {}

  std::optional<T> value_;
  ErrorCode code_;
  std::string message_;
};

template <>
class Result<void> {
 public:
  static Result ok() { return Result(ErrorCode::kOk); }
  static Result err(ErrorCode c, std::string msg = {}) {
    return Result(c, std::move(msg));
  }
  bool is_ok() const noexcept { return code_ == ErrorCode::kOk; }
  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  explicit operator bool() const noexcept { return is_ok(); }

 private:
  explicit Result(ErrorCode c) : code_(c) {}
  Result(ErrorCode c, std::string msg) : code_(c), message_(std::move(msg)) {}
  ErrorCode code_;
  std::string message_;
};

}  // namespace tnycl
