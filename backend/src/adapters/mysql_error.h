// adapters/mysql_error.h —— MySQL errno → 项目统一错误码映射（T-2.04）
//
// 连接/锁类错误映射到 9xxx（依赖故障），其余默认 1010（由 repository 层按具体
// 唯一键冲突细化为 2001/4006 等）。此映射在适配层做粗分类，repository 做细分类。
#pragma once

#include "tnycl/error_codes.h"

namespace tnycl {

ErrorCode mysql_errno_to_code(int mysql_errno) noexcept;

}  // namespace tnycl
