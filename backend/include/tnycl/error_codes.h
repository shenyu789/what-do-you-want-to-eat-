// tnycl/error_codes.h —— 统一错误码（唯一权威：docs/07 接口需求与错误码规范 v2.3）
//
// 语义稳定性约定（见 07 §6）：
//   - 已发布的 code 含义永久固定，不得复用、不得改语义。
//   - 新增错误码不需升版本；客户端遇到未知 code 按「可重试失败」处理。
#pragma once

#include <cstdint>
#include <string_view>

namespace tnycl {

// 错误码段：1xxx 通用 / 2xxx 账号 / 3xxx 档案 / 4xxx 餐次食物 / 5xxx 分析猜餐
//          6xxx 积分商城 / 7xxx 榜单 / 8xxx 历史导出 / 9xxx 服务端依赖
enum class ErrorCode : int {
  kOk = 0,

  // ---- 1xxx 通用 ----
  kBadRequest = 1001,           // JSON 语法错 / 缺必填 / 畸形报文
  kValidationFailed = 1002,     // 字段格式或范围错误
  kRouteNotFound = 1003,        // 路径不存在
  kMethodNotAllowed = 1004,     // 方法错误
  kPayloadTooLarge = 1005,      // > 64KB
  kRateLimited = 1006,          // 超限流
  kIdempotencyKeyMissing = 1007,// POST /meals 缺幂等键
  kIdempotencyConflict = 1008,  // 同键不同请求体
  kUnsupportedMediaType = 1009, // 非 JSON
  kInternalError = 1010,        // 未捕获异常

  // ---- 2xxx 账号 ----
  kUsernameTaken = 2001,
  kInvalidCredentials = 2002,   // 不区分账号是否存在
  kAccountLocked = 2003,
  kTokenInvalid = 2004,
  kTokenExpired = 2005,
  kForbidden = 2006,            // 访问他人资源
  kUserDisabled = 2007,
  kWeakPassword = 2008,

  // ---- 3xxx 档案 ----
  kProfileNotFound = 3001,
  kProfileInvalid = 3002,       // 字段越界 / 枚举非法（goal/activity）

  // ---- 4xxx 餐次与食物 ----
  kMealNotFound = 4001,         // 按会话 uid 过滤后不存在（他人资源也返回 404）
  kMealTypeInvalid = 4002,
  kFoodNotFound = 4003,         // slug 不存在
  kMealTextTooLong = 4004,
  kMealDateInvalid = 4005,      // 未来日期 / 早于 2000-01-01
  kMealDuplicateConflict = 4006,// 幂等键冲突（预留，主用 1008）

  // ---- 5xxx 分析与猜餐 ----
  kGuessNoCandidate = 5002,

  // ---- 6xxx 积分与商城 ----
  kPointsInsufficient = 6001,   // 余额不足
  kThemeNotFound = 6002,
  kPointsOverflow = 6003,
  kRuleVersionUnsupported = 6004,

  // ---- 7xxx 榜单 ----
  kLeaderboardForbidden = 7005,

  // ---- 8xxx 历史与导出 ----
  kCursorInvalid = 8001,
  kExportEmpty = 8002,

  // ---- 9xxx 服务端与依赖 ----
  kDbUnavailable = 9001,
  kDbBusy = 9002,               // 连接池获取超时
  kCpuBusy = 9003,              // CPU 池满
  kMaintenance = 9004,          // 停机/维护中
};

// 错误码 → HTTP 状态码（07 §2 逐条固定，各处必须一致，不得再出现 400/422 混用）。
// 边界规则：400=请求不合法 / 422=业务不可执行 / 404=资源不存在 / 409=状态冲突。
constexpr int http_status(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::kOk: return 200;
    // 1xxx
    case ErrorCode::kBadRequest: return 400;
    case ErrorCode::kValidationFailed: return 400;
    case ErrorCode::kRouteNotFound: return 404;
    case ErrorCode::kMethodNotAllowed: return 405;
    case ErrorCode::kPayloadTooLarge: return 413;
    case ErrorCode::kRateLimited: return 429;
    case ErrorCode::kIdempotencyKeyMissing: return 400;
    case ErrorCode::kIdempotencyConflict: return 409;
    case ErrorCode::kUnsupportedMediaType: return 415;
    case ErrorCode::kInternalError: return 500;
    // 2xxx
    case ErrorCode::kUsernameTaken: return 409;
    case ErrorCode::kInvalidCredentials: return 401;
    case ErrorCode::kAccountLocked: return 423;
    case ErrorCode::kTokenInvalid: return 401;
    case ErrorCode::kTokenExpired: return 401;
    case ErrorCode::kForbidden: return 403;
    case ErrorCode::kUserDisabled: return 403;
    case ErrorCode::kWeakPassword: return 422;
    // 3xxx
    case ErrorCode::kProfileNotFound: return 404;
    case ErrorCode::kProfileInvalid: return 400;
    // 4xxx
    case ErrorCode::kMealNotFound: return 404;
    case ErrorCode::kMealTypeInvalid: return 400;
    case ErrorCode::kFoodNotFound: return 404;
    case ErrorCode::kMealTextTooLong: return 400;
    case ErrorCode::kMealDateInvalid: return 400;
    case ErrorCode::kMealDuplicateConflict: return 409;
    // 5xxx
    case ErrorCode::kGuessNoCandidate: return 500;
    // 6xxx
    case ErrorCode::kPointsInsufficient: return 422;
    case ErrorCode::kThemeNotFound: return 404;
    case ErrorCode::kPointsOverflow: return 400;
    case ErrorCode::kRuleVersionUnsupported: return 400;
    // 7xxx
    case ErrorCode::kLeaderboardForbidden: return 403;
    // 8xxx
    case ErrorCode::kCursorInvalid: return 400;
    case ErrorCode::kExportEmpty: return 422;
    // 9xxx
    case ErrorCode::kDbUnavailable: return 503;
    case ErrorCode::kDbBusy: return 503;
    case ErrorCode::kCpuBusy: return 503;
    case ErrorCode::kMaintenance: return 503;
  }
  return 500;
}

// 符号名（用于日志/metrics 标签，不用于对客户端展示）。
constexpr std::string_view error_name(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::kOk: return "OK";
    case ErrorCode::kBadRequest: return "BAD_REQUEST";
    case ErrorCode::kValidationFailed: return "VALIDATION_FAILED";
    case ErrorCode::kRouteNotFound: return "ROUTE_NOT_FOUND";
    case ErrorCode::kMethodNotAllowed: return "METHOD_NOT_ALLOWED";
    case ErrorCode::kPayloadTooLarge: return "PAYLOAD_TOO_LARGE";
    case ErrorCode::kRateLimited: return "RATE_LIMITED";
    case ErrorCode::kIdempotencyKeyMissing: return "IDEMPOTENCY_KEY_MISSING";
    case ErrorCode::kIdempotencyConflict: return "IDEMPOTENCY_CONFLICT";
    case ErrorCode::kUnsupportedMediaType: return "UNSUPPORTED_MEDIA_TYPE";
    case ErrorCode::kInternalError: return "INTERNAL_ERROR";
    case ErrorCode::kUsernameTaken: return "USERNAME_TAKEN";
    case ErrorCode::kInvalidCredentials: return "INVALID_CREDENTIALS";
    case ErrorCode::kAccountLocked: return "ACCOUNT_LOCKED";
    case ErrorCode::kTokenInvalid: return "TOKEN_INVALID";
    case ErrorCode::kTokenExpired: return "TOKEN_EXPIRED";
    case ErrorCode::kForbidden: return "FORBIDDEN";
    case ErrorCode::kUserDisabled: return "USER_DISABLED";
    case ErrorCode::kWeakPassword: return "WEAK_PASSWORD";
    case ErrorCode::kProfileNotFound: return "PROFILE_NOT_FOUND";
    case ErrorCode::kProfileInvalid: return "PROFILE_INVALID";
    case ErrorCode::kMealNotFound: return "MEAL_NOT_FOUND";
    case ErrorCode::kMealTypeInvalid: return "MEAL_TYPE_INVALID";
    case ErrorCode::kFoodNotFound: return "FOOD_NOT_FOUND";
    case ErrorCode::kMealTextTooLong: return "MEAL_TEXT_TOO_LONG";
    case ErrorCode::kMealDateInvalid: return "MEAL_DATE_INVALID";
    case ErrorCode::kMealDuplicateConflict: return "MEAL_DUPLICATE_CONFLICT";
    case ErrorCode::kGuessNoCandidate: return "GUESS_NO_CANDIDATE";
    case ErrorCode::kPointsInsufficient: return "POINTS_INSUFFICIENT";
    case ErrorCode::kThemeNotFound: return "THEME_NOT_FOUND";
    case ErrorCode::kPointsOverflow: return "POINTS_OVERFLOW";
    case ErrorCode::kRuleVersionUnsupported: return "RULE_VERSION_UNSUPPORTED";
    case ErrorCode::kLeaderboardForbidden: return "LEADERBOARD_FORBIDDEN";
    case ErrorCode::kCursorInvalid: return "CURSOR_INVALID";
    case ErrorCode::kExportEmpty: return "EXPORT_EMPTY";
    case ErrorCode::kDbUnavailable: return "DB_UNAVAILABLE";
    case ErrorCode::kDbBusy: return "DB_BUSY";
    case ErrorCode::kCpuBusy: return "CPU_BUSY";
    case ErrorCode::kMaintenance: return "MAINTENANCE";
  }
  return "UNKNOWN";
}

// 面向用户的可读文案（可随版本调整，客户端不得依据 message 判断）。
inline std::string_view error_message(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::kOk: return "ok";
    case ErrorCode::kBadRequest: return "请求不合法";
    case ErrorCode::kValidationFailed: return "参数校验失败";
    case ErrorCode::kRouteNotFound: return "接口不存在";
    case ErrorCode::kMethodNotAllowed: return "请求方法不允许";
    case ErrorCode::kPayloadTooLarge: return "请求体过大";
    case ErrorCode::kRateLimited: return "请求过于频繁";
    case ErrorCode::kIdempotencyKeyMissing: return "缺少幂等键";
    case ErrorCode::kIdempotencyConflict: return "幂等键冲突";
    case ErrorCode::kUnsupportedMediaType: return "不支持的内容类型";
    case ErrorCode::kInternalError: return "服务内部错误";
    case ErrorCode::kUsernameTaken: return "用户名已被占用";
    case ErrorCode::kInvalidCredentials: return "用户名或密码错误";
    case ErrorCode::kAccountLocked: return "账号已锁定";
    case ErrorCode::kTokenInvalid: return "令牌无效";
    case ErrorCode::kTokenExpired: return "令牌已过期";
    case ErrorCode::kForbidden: return "无权访问";
    case ErrorCode::kUserDisabled: return "账号已禁用";
    case ErrorCode::kWeakPassword: return "密码强度不足";
    case ErrorCode::kProfileNotFound: return "尚未创建健康档案";
    case ErrorCode::kProfileInvalid: return "档案字段不合法";
    case ErrorCode::kMealNotFound: return "餐次不存在";
    case ErrorCode::kMealTypeInvalid: return "餐次类型不合法";
    case ErrorCode::kFoodNotFound: return "食物不存在";
    case ErrorCode::kMealTextTooLong: return "文本过长";
    case ErrorCode::kMealDateInvalid: return "日期不合法";
    case ErrorCode::kMealDuplicateConflict: return "重复的幂等请求";
    case ErrorCode::kGuessNoCandidate: return "暂无可推荐的食物";
    case ErrorCode::kPointsInsufficient: return "本周可用积分不足";
    case ErrorCode::kThemeNotFound: return "主题不存在";
    case ErrorCode::kPointsOverflow: return "积分变动越界";
    case ErrorCode::kRuleVersionUnsupported: return "规则版本不支持";
    case ErrorCode::kLeaderboardForbidden: return "无权查看该榜单";
    case ErrorCode::kCursorInvalid: return "分页游标非法";
    case ErrorCode::kExportEmpty: return "无可导出数据";
    case ErrorCode::kDbUnavailable: return "数据库不可用";
    case ErrorCode::kDbBusy: return "数据库繁忙";
    case ErrorCode::kCpuBusy: return "系统繁忙";
    case ErrorCode::kMaintenance: return "服务维护中";
  }
  return "未知错误";
}

constexpr int error_code(ErrorCode c) noexcept { return static_cast<int>(c); }

}  // namespace tnycl
