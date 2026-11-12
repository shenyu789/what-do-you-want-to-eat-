#!/usr/bin/env bash
# T-1.03 检查 1/6：禁止用裸 new/delete/malloc/free 管理资源（约束 C-2 / 决策 D-04）。
# 白名单：src/adapters/（第三方 C 库的 RAII 封装层，唯一允许接触裸资源的地方）。
set -u
source "$(dirname "$0")/_common.sh"

if [ "${1:-}" = "--self-test" ]; then
    self_test "raw_ptr" 'void f() { int* p = new int(1); delete p; }'
fi

echo "检查 raw_ptr（白名单：src/adapters/）"
scan '\bnew\s+[A-Za-z_(]'                        "使用了 new（应用 std::make_unique / make_shared）" adapters
scan '\bdelete\s+(\[\])?\s*[A-Za-z_(]'           "使用了 delete（应用 RAII / unique_ptr）" adapters
scan '\bmalloc\s*\('                             "使用了 malloc（应用 RAII 分配器）" adapters
scan '\bfree\s*\('                               "使用了 free（应用 RAII 释放器）" adapters
scan '\bstrdup\s*\(|\bstrndup\s*\('              "使用了 strdup（应用 std::string）" adapters
finish "raw_ptr"
