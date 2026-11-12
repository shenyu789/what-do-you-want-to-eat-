#!/usr/bin/env bash
# T-1.03 检查 2/6：第三方 C 头文件不得越过 adapters 层（决策 D-04 / 约束 C-2）。
# 业务层（service/domain/repository/transport）不得 include 任何第三方 C 头。
set -u
source "$(dirname "$0")/_common.sh"

if [ "${1:-}" = "--self-test" ]; then
    self_test "third_party_includes" '#include <mysql.h>
void f() {}'
fi

echo "检查 third_party_includes（白名单：src/adapters/）"
scan '#include\s*[<"](mysql\.h|mysql/|hiredis|hiredis\.h|llhttp|sqlite3)' \
     "越界 include 第三方 C 头（只能出现在 src/adapters/）" adapters
scan '#include\s*[<"](curl/|libpq|zlib\.h)' \
     "越界 include 第三方 C 头（未批准的依赖）" adapters
finish "third_party_includes"
