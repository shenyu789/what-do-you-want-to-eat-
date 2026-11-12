// adapters/mysql_error.cpp —— MySQL errno → 错误码映射（T-2.04）
#include "adapters/mysql_error.h"

namespace tnycl {

ErrorCode mysql_errno_to_code(int e) noexcept {
  switch (e) {
    case 1040:  // ER_CON_COUNT_ERROR 连接数满
    case 1203:  // ER_TOO_MANY_USER_CONNECTIONS
    case 1205:  // ER_LOCK_WAIT_TIMEOUT
    case 1213:  // ER_LOCK_DEADLOCK
      return ErrorCode::kDbBusy;
    case 2002:  // CR_CONNECTION_ERROR
    case 2003:  // CR_CONN_HOST_ERROR
    case 2006:  // CR_SERVER_GONE_ERROR
    case 2013:  // CR_SERVER_LOST
      return ErrorCode::kDbUnavailable;
    default:    // 含 1062（唯一键冲突，由 repository 细化为 2001/4006）
      return ErrorCode::kInternalError;
  }
}

}  // namespace tnycl
