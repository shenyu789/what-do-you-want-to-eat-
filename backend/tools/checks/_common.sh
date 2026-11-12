#!/usr/bin/env bash
# 静态检查公共库（T-1.03）。
# 设计要点：每个检查都可 --self-test，用"故意违规文件"证明它真的能抓到问题，
# 避免出现"检查脚本永远返回 0"这种假门禁。
set -u

BACKEND_ROOT="${BACKEND_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
GREP_FLAGS="${GREP_FLAGS:-}"     # 各检查可设为 -i
violations=0

# strip_comments PATTERN GREP_OUTPUT
# 去掉"只出现在注释里"的命中，避免把
#     // 严禁 SUBSCRIBE/PUBLISH
# 这类说明文字判成违规（I-02：v2.3 W1 的真实误报）。
# 规则：① 截断行尾 // 注释后重新匹配；② 跳过整行块注释内的行（以 * 或 /* 开头）。
strip_comments() {
    local pattern="$1"; local out="$2"
    [ -n "$out" ] || return 0
    local line content code trimmed
    while IFS= read -r line; do
        content=${line#*:*:}                 # 去掉 path:line: 前缀
        trimmed=$(printf '%s' "$content" | sed 's/^[[:space:]]*//')
        # 只跳过块注释内部行。**绝不能跳过以 # 开头的行**——
        # 预处理指令是代码，而 third_party_includes / no_sqlite 要抓的违规
        # 恰恰就是 `#include <mysql.h>` 这类行（I-05：跳过它会让自检变成假门禁）。
        case "$trimmed" in
            '/*'*|'*'*) continue ;;
        esac
        code=${content%%//*}                 # 截断行尾注释
        if printf '%s' "$code" | grep -qE ${GREP_FLAGS} -e "$pattern"; then
            printf '%s\n' "$line"
        fi
    done <<< "$out"
}

# scan PATTERN DESC [EXCLUDE_DIR ...]
scan() {
    local pattern="$1"; shift
    local desc="$1"; shift
    local args=(-rnE ${GREP_FLAGS}
                --include='*.cpp' --include='*.hpp' --include='*.h'
                --include='*.cc' --include='*.cxx' --include='*.inl')
    local e
    for e in ${@+"$@"}; do
        args+=(--exclude-dir="$e")
    done
    local out
    out=$(grep "${args[@]}" -e "$pattern" "$BACKEND_ROOT/src" "$BACKEND_ROOT/include" 2>/dev/null || true)
    out=$(strip_comments "$pattern" "$out")
    if [ -n "$out" ]; then
        echo "  ✗ $desc"
        echo "$out" | sed 's/^/      /'
        violations=$((violations + 1))
    fi
}

# scan_extra PATTERN DESC PATH — 扫描 src/include 之外的单个文件（如 CMakeLists.txt）
scan_extra() {
    local pattern="$1"; local desc="$2"; local path="$3"
    [ -f "$path" ] || return 0
    local out
    out=$(grep -nE ${GREP_FLAGS} -e "$pattern" "$path" 2>/dev/null || true)
    if [ -n "$out" ]; then
        echo "  ✗ $desc"
        echo "$out" | sed "s|^|      ${path}:|"
        violations=$((violations + 1))
    fi
}

finish() {
    local name="$1"
    if [ "$violations" -eq 0 ]; then
        echo "[PASS] $name"
        exit 0
    fi
    echo "[FAIL] $name：$violations 项违规"
    exit 1
}

# self_test NAME 'VIOLATION_SNIPPET' [EXTRA_FILE_NAME]
# 建临时工程写入违规片段，断言检查以非 0 退出；然后再用空工程跑一次，断言以 0 退出。
self_test() {
    local name="$1"; local snippet="$2"; local extra="${3:-}"
    local tmp; tmp=$(mktemp -d)
    mkdir -p "$tmp/src" "$tmp/include"
    printf '%s\n' "$snippet" > "$tmp/src/violation.cpp"
    local rc_bad=0
    BACKEND_ROOT="$tmp" bash "$0" >/dev/null 2>&1 || rc_bad=$?
    if [ "$rc_bad" -eq 0 ]; then
        echo "[SELFTEST FAIL] $name：违规文件未被抓到（检查是假门禁）"
        rm -rf "$tmp"; exit 1
    fi

    local tmp2; tmp2=$(mktemp -d)
    mkdir -p "$tmp2/src" "$tmp2/include"
    printf '%s\n' 'int ok() { return 0; }' > "$tmp2/src/clean.cpp"
    [ -n "$extra" ] && printf '%s\n' 'cmake_minimum_required(VERSION 3.28)' > "$tmp2/$extra"
    local rc_ok=0
    BACKEND_ROOT="$tmp2" bash "$0" >/dev/null 2>&1 || rc_ok=$?
    rm -rf "$tmp" "$tmp2"
    if [ "$rc_ok" -ne 0 ]; then
        echo "[SELFTEST FAIL] $name：干净工程被误判（误报）"
        exit 1
    fi
    echo "[SELFTEST PASS] $name：能抓违规、不误报"
    exit 0
}
