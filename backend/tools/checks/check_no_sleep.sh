#!/usr/bin/env bash
# T-1.03 检查 5/6：禁止 sleep 轮询（硬约束 C-5 / 技术决策 T-05）。
# 周期任务必须由单 Timer 线程（timerfd + 最小堆）驱动，
# 禁止"起个线程 while(true){ sleep }"这类独立线程轮询。
#
# 注意：本检查**不**禁止 while(true) —— 事件循环 while(true){ epoll_wait(...) }
# 是合法且必要的。真正的违规特征是"用 sleep 制造周期"，故只抓 sleep 系列。
set -u
source "$(dirname "$0")/_common.sh"

if [ "${1:-}" = "--self-test" ]; then
    self_test "no_sleep" '#include <thread>
void f() { std::this_thread::sleep_for(std::chrono::seconds(1)); }'
fi

echo "检查 no_sleep"
scan 'sleep_for|std::this_thread::sleep|usleep\s*\(|nanosleep\s*\(|\bsleep\s*\(' \
    "出现阻塞式 sleep（周期任务必须走单 Timer 线程 + timerfd）"
finish "no_sleep"
