#!/usr/bin/env bash
# T-1.03 检查 3/6：禁止 SQLite（硬约束：数据库只能 MySQL，缓存只能 Redis）。
set -u
source "$(dirname "$0")/_common.sh"
GREP_FLAGS="-i"

if [ "${1:-}" = "--self-test" ]; then
    self_test "no_sqlite" 'void f() { sqlite3_open("x.db", nullptr); }'
fi

echo "检查 no_sqlite"
scan 'sqlite' "出现 sqlite 引用（本项目禁止 SQLite）"
scan_extra 'sqlite' "构建脚本引用了 sqlite" "$BACKEND_ROOT/CMakeLists.txt"
for f in "$BACKEND_ROOT"/migrations/*.sql; do
    [ -f "$f" ] || continue
    scan_extra 'sqlite' "迁移脚本引用了 sqlite" "$f"
done
finish "no_sqlite"
