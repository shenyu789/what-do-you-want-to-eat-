#!/usr/bin/env bash
# T-1.03 检查 4/6：禁止 Redis Pub/Sub（硬约束：禁 Pub/Sub，群聊/广播不得靠它）。
set -u
source "$(dirname "$0")/_common.sh"

if [ "${1:-}" = "--self-test" ]; then
    self_test "no_pubsub" 'void f() { redis_cmd("PUBLISH chan msg"); }'
fi

echo "检查 no_pubsub"
GREP_FLAGS="-i"
scan '\b(publish|psubscribe|subscribe|pubsub)\b' \
    "出现 Pub/Sub 调用（硬约束禁止；业务广播只能靠进程内分发）"
finish "no_pubsub"
