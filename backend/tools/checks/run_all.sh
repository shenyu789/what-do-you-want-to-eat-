#!/usr/bin/env bash
# 一次跑完六项静态检查（T-1.03）。
# 用法：
#   bash backend/tools/checks/run_all.sh            # 检查真实代码
#   bash backend/tools/checks/run_all.sh --self-test # 只验证六项检查本身是否有效
#
# --self-test 是必须的：它证明每个检查都能抓到"故意违规"，不是永远返回 0 的假门禁。
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
CHECKS=(
    check_raw_ptr.sh
    check_third_party_includes.sh
    check_no_sqlite.sh
    check_no_pubsub.sh
    check_no_sleep.sh
    check_ledger_append_only.sh
)

MODE="${1:-}"
failed=0

if [ "$MODE" = "--self-test" ]; then
    echo "=== 六项检查有效性自检（用故意违规文件验证）==="
    for c in "${CHECKS[@]}"; do
        out=$(bash "$DIR/$c" --self-test 2>&1) || true
        if echo "$out" | grep -q 'SELFTEST PASS'; then
            echo "  ✓ $c"
        else
            echo "  ✗ $c"
            echo "$out" | sed 's/^/      /'
            failed=$((failed + 1))
        fi
    done
else
    echo "=== 六项静态检查（T-1.03）==="
    for c in "${CHECKS[@]}"; do
        if bash "$DIR/$c"; then :; else failed=$((failed + 1)); fi
    done
fi

echo
if [ "$failed" -eq 0 ]; then
    echo "RESULT: PASS（6/6）"
    exit 0
fi
echo "RESULT: FAIL（$failed 项未通过）"
exit 1
