#!/usr/bin/env bash
# =============================================================================
# mysql_e2e.sh —— 真实 MySQL 8 端到端集成驱动（CI 集成 job 的唯一入口）
#
# 为什么需要本脚本：ctest 里 mysql_integration_test 在无库环境下以 77 退出并显示
# Skipped，这只能证明"没跑"，不能证明"跑过"。真实数据库验收必须满足：
#   建库 → V001（空库）→ V002 ×3（幂等 + 不误伤）→ 同名不同 slug 冲突 → 11 个场景
#   → 清理/隔离断言，且每一步都有明确的断言与失败退出码。
#
# 本脚本**不吞错**：任一步失败即以非 0 退出，并把原因打成 ::error 注释
# （注释免登录可读，日志需要登录）。
#
# 退出码：0=全部真实通过；1=任一断言失败；77=环境缺失（CI 中视为失败）
#
# 环境变量（全部可选，CI 会显式传入）：
#   TNYCL_MYSQL_HOST/PORT/USER/PASSWORD/DB   连接参数（DB 默认 tnycl_it_ci）
#   TEST_EXE                                 集成可执行体路径
#   BUILD_DIR                                构建目录（用于自动定位 TEST_EXE）
#   MIN_CASES                                期望场景数下限（默认 11；实际跑多少以日志为准）
#   LOG_DIR                                  日志输出目录（默认 ./ci-logs）
# =============================================================================
set -uo pipefail

HOST=${TNYCL_MYSQL_HOST:-127.0.0.1}
PORT=${TNYCL_MYSQL_PORT:-3306}
DBUSER=${TNYCL_MYSQL_USER:-root}
DBPASS=${TNYCL_MYSQL_PASSWORD:-}
# 独立库名：与本地开发库（tnycl_it）、生产库都不重合，避免"跑集成把别人的库洗了"。
DBNAME=${TNYCL_MYSQL_DB:-tnycl_it_ci}
BUILD_DIR=${BUILD_DIR:-backend/build}
LOG_DIR=${LOG_DIR:-$PWD/ci-logs}
MIN_CASES=${MIN_CASES:-11}
TEST_EXE=${TEST_EXE:-}

BACKEND_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V001="$BACKEND_ROOT/migrations/V001__init.sql"
V002="$BACKEND_ROOT/sql/V002__seed_foods.sql"
SEED_SLUGS="'lemon-chicken','tomato-beef','mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken'"
SEED_COUNT=17   # V001 建表数（02-ARCH v2.3 §5.2）

mkdir -p "$LOG_DIR"
RUN_LOG="$LOG_DIR/mysql_e2e.log"
: >"$RUN_LOG"

say() { printf '%s\n' "$*" | tee -a "$RUN_LOG"; }
fail() {
  say "[FAIL] $2"
  # $1 = 简短标题（英文，作为注释 title）；$2 = 中文原因
  printf '::error title=%s::%s\n' "$1" "$2"
  exit 1
}
ok() { say "[PASS] $1"; }

# ------------------------------------------------------------------ 前置校验
# 库名直接拼进 SQL，必须先做严格白名单校验（同时避免反引号在 bash 双引号里
# 被当成命令替换执行）。
if ! printf '%s' "$DBNAME" | grep -Eq '^[A-Za-z_][A-Za-z0-9_]*$'; then
  fail bad-db-name "TNYCL_MYSQL_DB 只允许字母数字下划线，实际：$DBNAME"
fi
[ -f "$V001" ] || fail missing-sql "缺少 $V001"
[ -f "$V002" ] || fail missing-sql "缺少 $V002"
command -v mysql >/dev/null 2>&1 || {
  say "[SKIP] 未找到 mysql 客户端，环境不具备（CI 中视为失败）"
  exit 77
}

# 密码走 MYSQL_PWD，不出现在进程参数里（ps 可见）。
mysql_cli() {
  if [ -n "$DBPASS" ]; then
    MYSQL_PWD="$DBPASS" mysql --protocol=TCP -h"$HOST" -P"$PORT" -u"$DBUSER" \
      --default-character-set=utf8mb4 -N -B "$@"
  else
    mysql --protocol=TCP -h"$HOST" -P"$PORT" -u"$DBUSER" \
      --default-character-set=utf8mb4 -N -B "$@"
  fi
}
# 带默认库执行（迁移文件、种子文件都通过它喂进去）
mysql_db() { mysql_cli -D "$DBNAME" "$@"; }
# 单值查询，去掉首尾空白
scalar() {
  local v
  v=$(mysql_db -e "$1") || return 1
  printf '%s' "$(printf '%s' "$v" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
}
scalar_root() {
  local v
  v=$(mysql_cli -e "$1") || return 1
  printf '%s' "$(printf '%s' "$v" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
}
expect_eq() { # 实际 期望 说明
  if [ "$1" != "$2" ]; then
    fail assert-mismatch "$3：期望 [$2]，实际 [$1]"
  fi
}

say "=============================================================="
say "真实 MySQL 集成（E4 证据）  时间：$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
say "客户端：$(mysql --version 2>&1 | head -1)"
say "=============================================================="

# ------------------------------------------------------- 1. 连接 + 版本证据
version=$(scalar_root "SELECT VERSION();") || fail connect "无法连接 $HOST:$PORT（账号/密码/网络）"
case "$version" in
  8.*) ;;
  *) fail mysql-version "本套件要求 MySQL 8.x，实际 [$version]" ;;
esac
ok "已真实连接 MySQL $version（$HOST:$PORT）"

# 打印实际使用的认证插件：caching_sha2_password 走 TLS，若这里显示异常可快速定位认证问题。
say "[INFO] 账号认证插件：$(scalar_root "SELECT CONCAT(user,'@',host,':',plugin) FROM mysql.user WHERE user='$DBUSER' LIMIT 1")"

# ----------------------------------------------- 2. 建独立空库并断言确实为空
mysql_cli -e "DROP DATABASE IF EXISTS $DBNAME;
              CREATE DATABASE $DBNAME DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;" \
  || fail create-db "建库 $DBNAME 失败"
empty=$(scalar_root "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$DBNAME';")
expect_eq "$empty" "0" "建库后表数"
ok "独立空库 $DBNAME 就绪（表数=0，未复用任何既有库）"

# ------------------------------------------------------------- 3. V001 schema
mysql_db <"$V001" >>"$RUN_LOG" 2>&1 || fail run-v001 "V001 在空库上执行失败"
tables=$(scalar_root "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$DBNAME';")
expect_eq "$tables" "$SEED_COUNT" "V001 后表数"
parts=$(scalar_root "SELECT COUNT(*) FROM information_schema.partitions WHERE table_schema='$DBNAME' AND partition_name IS NOT NULL;")
expect_eq "$parts" "0" "分区表数量（02-ARCH 明确禁分区）"
ok "V001 首次执行成功（$tables 张表，分区表 0）"

# ------------------------------------ 4. V002 连续两次：幂等且不重复插入
round=0
for round in 1 2; do
  out=$(mysql_db <"$V002" 2>&1) || {
    printf '%s\n' "$out" >>"$RUN_LOG"
    fail run-v002 "V002 第 $round 次执行失败"
  }
  printf '%s\n' "$out" >>"$RUN_LOG"
  printf '%s' "$out" | grep -q 'seed_ok' || fail v002-assert "V002 第 $round 次未输出 seed_ok"
  seeds=$(scalar "SELECT COUNT(*) FROM foods WHERE slug IN ($SEED_SLUGS);")
  expect_eq "$seeds" "6" "V002 第 $round 次后种子数"
done
dup=$(scalar "SELECT COUNT(*) FROM (SELECT slug FROM foods GROUP BY slug HAVING COUNT(*)>1) t;")
expect_eq "$dup" "0" "重复 slug 数"
collide=$(scalar "SELECT COUNT(*) FROM (SELECT name FROM foods GROUP BY name HAVING COUNT(DISTINCT slug)>1) t;")
expect_eq "$collide" "0" "同名不同 slug 数"
total=$(scalar "SELECT COUNT(*) FROM foods;")
expect_eq "$total" "6" "foods 总行数"
ok "V002 重复执行不重复插入（连续 2 次，种子仍为 6 条）"

# ------------------------- 5. 第三次 V002：只更新自己的 slug，不误伤其他行
mysql_db -e "INSERT INTO foods (slug,name,category,description,calories,protein_g,fiber_g,veggies_g,water_ml,image_url,active)
             VALUES ('it-decoy-probe','集成占位食物(勿动)','占位','占位',1,1,1,1,1,'',0);" \
  || fail decoy-insert "插入对照行失败"
mysql_db <"$V002" >>"$RUN_LOG" 2>&1 || fail run-v002-third "V002 第 3 次执行失败"
decoy_cal=$(scalar "SELECT calories FROM foods WHERE slug='it-decoy-probe';")
decoy_name=$(scalar "SELECT name FROM foods WHERE slug='it-decoy-probe';")
expect_eq "$decoy_cal" "1" "对照行 calories 被误改"
expect_eq "$decoy_name" "集成占位食物(勿动)" "对照行 name 被误改"
seeds=$(scalar "SELECT COUNT(*) FROM foods WHERE slug IN ($SEED_SLUGS);")
expect_eq "$seeds" "6" "第 3 次后种子数"
ok "V002 第 3 次只更新本 slug 行，未误伤其他食物"

# ------------------------- 6. 同名不同 slug 必须明确失败，且不改动任何既有行
before_total=$(scalar "SELECT COUNT(*) FROM foods;")
before_cal=$(scalar "SELECT calories FROM foods WHERE slug='lemon-chicken';")
# 刻意不开 set -e：冲突插入**本应**失败，非零退出码是期望结果，必须显式取 rc 再断言。
conflict_out=$(mysql_db -e "INSERT INTO foods (slug,name,category,description,calories,protein_g,fiber_g,veggies_g,water_ml,image_url,active)
                            VALUES ('it-conflict-probe','柠檬鸡胸能量碗','t','t',1,1,1,1,1,'',1);" 2>&1)
conflict_rc=$?
printf '%s\n' "$conflict_out" >>"$RUN_LOG"
if [ "$conflict_rc" -eq 0 ]; then
  mysql_db -e "DELETE FROM foods WHERE slug='it-conflict-probe';" >/dev/null 2>&1 || true
  fail conflict-not-rejected "同名不同 slug 竟插入成功（uk_name 失效）"
fi
printf '%s' "$conflict_out" | grep -q 'ERROR 1062' \
  || fail conflict-code "冲突未以 1062 拒绝，实际输出：$conflict_out"
after_total=$(scalar "SELECT COUNT(*) FROM foods;")
after_cal=$(scalar "SELECT calories FROM foods WHERE slug='lemon-chicken';")
stray=$(scalar "SELECT COUNT(*) FROM foods WHERE slug='it-conflict-probe';")
expect_eq "$after_total" "$before_total" "冲突后 foods 总行数"
expect_eq "$after_cal" "$before_cal" "冲突后 lemon-chicken.calories（不误更新其他食物）"
expect_eq "$stray" "0" "冲突残留行数"
ok "同名不同 slug 以 ERROR 1062 明确失败，且未误更新其他食物"

# --------------------------------------------------- 7. 清理对照行，回到 6 条
mysql_db -e "DELETE FROM foods WHERE slug='it-decoy-probe';" || fail decoy-clean "清理对照行失败"
total=$(scalar "SELECT COUNT(*) FROM foods;")
expect_eq "$total" "6" "清理后 foods 行数"
ok "对照行清理完毕（foods=6）"

# ------------------------------------------------ 8. 跑 11 个集成场景（真实库）
if [ -z "$TEST_EXE" ]; then
  for candidate in "$BUILD_DIR/mysql_integration_test" "$BUILD_DIR/tests/mysql_integration_test"; do
    if [ -x "$candidate" ]; then TEST_EXE="$candidate"; break; fi
  done
fi
[ -n "$TEST_EXE" ] && [ -x "$TEST_EXE" ] \
  || fail no-test-exe "未找到 mysql_integration_test（BUILD_DIR=$BUILD_DIR）"

TEST_LOG="$LOG_DIR/mysql_integration_test.log"
# 同样显式取退出码：77 与 1 的语义完全不同，必须分别断言。
TNYCL_MYSQL_HOST="$HOST" TNYCL_MYSQL_PORT="$PORT" TNYCL_MYSQL_USER="$DBUSER" \
TNYCL_MYSQL_PASSWORD="$DBPASS" TNYCL_MYSQL_DB="$DBNAME" \
  "$TEST_EXE" >"$TEST_LOG" 2>&1
test_rc=$?
tee -a "$RUN_LOG" <"$TEST_LOG"

# 77 = "跳过"，在集成 job 里绝不允许（否则绿色只代表没跑）。
if [ "$test_rc" -eq 77 ]; then
  fail test-skipped "集成可执行体返回 77（跳过），真实库尚未被验证，集成 job 不允许跳过"
fi
if [ "$test_rc" -ne 0 ]; then
  fail test-failed "集成可执行体退出码 $test_rc（详见 $TEST_LOG）"
fi

# 用日志反证"确实连了库、确实跑了场景"，而不是只看退出码。
grep -q '\[REAL-MYSQL\] target=' "$TEST_LOG" || fail no-real-marker "日志缺少 [REAL-MYSQL] target=，无法证明连的是真实库"
grep -q '\[REAL-MYSQL\] server=8\.' "$TEST_LOG" || fail no-server-marker "日志缺少 server=8.x，服务端版本未确认"
pass_n=$(grep -c '^\[PASS\]' "$TEST_LOG")
fail_n=$(grep -c '^\[FAIL\]' "$TEST_LOG")
expect_eq "$fail_n" "0" "失败场景数"
# 场景数下限而非等值：套件后续可能继续增加真实库用例，硬绑数字会让"多写了测试"变成红灯。
if [ "$pass_n" -lt "$MIN_CASES" ]; then
  fail too-few-cases "通过场景数 $pass_n 少于要求的 $MIN_CASES（是否有用例漏注册或中途 return）"
fi
checks=$(grep -oE 'checks=[0-9]+' "$TEST_LOG" | tail -1)
ok "$pass_n 个场景全部通过（$checks failures=0，要求下限 $MIN_CASES），可执行体退出码 0"

# ------------------------------------------- 9. 清理与隔离：测试不得留脏数据
left_users=$(scalar "SELECT COUNT(*) FROM users WHERE username LIKE 'it\\_%';")
expect_eq "$left_users" "0" "测试残留用户数（隔离/清理）"
left_sessions=$(scalar "SELECT COUNT(*) FROM user_sessions s LEFT JOIN users u ON u.user_id=s.user_id WHERE u.user_id IS NULL;")
expect_eq "$left_sessions" "0" "孤儿会话数"
ok "测试数据已清理、用例间隔离（残留 users=0，孤儿 sessions=0）"

# --------------------------------------------------------------- 10. 汇总
say "=============================================================="
say "E4 证据汇总（全部为真实 MySQL $version 上执行所得）"
say "  库            : $DBNAME @ $HOST:$PORT"
say "  V001          : 17 表 / 0 分区（空库首次执行通过）"
say "  V002          : 连续 3 次执行，种子恒为 6 条，未重复插入、未误伤对照行"
say "  冲突负向      : 同名不同 slug → ERROR 1062，既有行未被更新"
say "  集成场景      : $pass_n 通过 / $fail_n 失败（$checks）"
say "  清理与隔离    : 残留 users=0"
say "=============================================================="
exit 0
