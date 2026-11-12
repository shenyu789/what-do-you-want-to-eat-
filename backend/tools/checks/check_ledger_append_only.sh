#!/usr/bin/env bash
# T-1.03 检查 6/6：积分账本只允许追加（技术决策 T-08 / 红线 R-3）。
# point_ledger 一旦写入不得 UPDATE/DELETE；纠错只能追加 reversal 事件。
set -u
source "$(dirname "$0")/_common.sh"

if [ "${1:-}" = "--self-test" ]; then
    self_test "ledger_append_only" 'void f() { db.exec("UPDATE point_ledger SET delta = 1"); }'
fi

echo "检查 ledger_append_only"
GREP_FLAGS="-i"
scan 'update[[:space:]]+point_ledger' "出现 UPDATE point_ledger（账本 append-only）"
scan 'delete[[:space:]]+from[[:space:]]+point_ledger' "出现 DELETE FROM point_ledger（账本 append-only）"
GREP_FLAGS=""
scan 'set[[:space:]]+earned[[:space:]]*=' "直接改 user_points.earned（余额必须由账本求和）"
scan 'set[[:space:]]+spent[[:space:]]*=' "直接改 user_points.spent（余额必须由账本求和）"
finish "ledger_append_only"
