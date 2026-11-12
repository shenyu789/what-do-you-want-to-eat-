// 适配层纯逻辑测试：MySQL errno 映射 + Redis RESP 编解码（无外部库依赖）
#include <string>
#include <vector>

#include "adapters/mysql_error.h"
#include "infra/redis_resp.h"
#include "support/test_harness.h"

using namespace tnycl;

// ---- MySQL errno 映射 ----
TNYCL_TEST(mysql_errno_maps_db_busy) {
  CHECK(mysql_errno_to_code(1205) == ErrorCode::kDbBusy);  // 锁等待
  CHECK(mysql_errno_to_code(1213) == ErrorCode::kDbBusy);  // 死锁
  CHECK(mysql_errno_to_code(1040) == ErrorCode::kDbBusy);  // 连接数满
}

TNYCL_TEST(mysql_errno_maps_db_unavailable) {
  CHECK(mysql_errno_to_code(2006) == ErrorCode::kDbUnavailable);
  CHECK(mysql_errno_to_code(2013) == ErrorCode::kDbUnavailable);
  CHECK(mysql_errno_to_code(2002) == ErrorCode::kDbUnavailable);
}

TNYCL_TEST(mysql_errno_defaults_internal) {
  CHECK(mysql_errno_to_code(1062) == ErrorCode::kInternalError);  // 由 repository 细化
  CHECK(mysql_errno_to_code(9999) == ErrorCode::kInternalError);
}

// ---- Redis RESP ----
TNYCL_TEST(resp_encode_command) {
  std::string cmd = resp_encode_command({"GET", "k"});
  CHECK(cmd == "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
}

TNYCL_TEST(resp_decode_status) {
  auto r = resp_decode_reply("+OK\r\n");
  CHECK(r.type == RespReply::Type::kBulk);
  CHECK(r.str == "OK");
}

TNYCL_TEST(resp_decode_integer) {
  auto r = resp_decode_reply(":42\r\n");
  CHECK(r.type == RespReply::Type::kInteger);
  CHECK_EQ(r.integer, 42);
}

TNYCL_TEST(resp_decode_nil) {
  auto r = resp_decode_reply("$-1\r\n");
  CHECK(r.type == RespReply::Type::kNil);
}

TNYCL_TEST(resp_decode_error) {
  auto r = resp_decode_reply("-ERR nope\r\n");
  CHECK(r.type == RespReply::Type::kError);
  CHECK(r.str == "ERR nope");
}

TNYCL_TEST(resp_decode_bulk) {
  auto r = resp_decode_reply("$5\r\nhello\r\n");
  CHECK(r.type == RespReply::Type::kBulk);
  CHECK(r.str == "hello");
}

TNYCL_TEST(resp_decode_array) {
  auto r = resp_decode_reply("*2\r\n$1\r\na\r\n$1\r\nb\r\n");
  CHECK(r.type == RespReply::Type::kArray);
  CHECK_EQ(r.array.size(), 2);
  CHECK(r.array[0].str == "a");
  CHECK(r.array[1].str == "b");
}

TNYCL_TEST(resp_decode_malformed) {
  auto r = resp_decode_reply("!!!");
  CHECK(r.type == RespReply::Type::kError);
}

int main() { return tnycl::test::run_all(); }
