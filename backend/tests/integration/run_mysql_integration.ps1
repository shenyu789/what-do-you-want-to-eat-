# 真实 MySQL 集成测试驱动脚本（B 交付，P0）。
# 用途：建库 → 执行 V001 → 连续执行 V002 两次（验证幂等）→ 校验种子 → 跑集成测试可执行体。
# 依赖：mysql.exe（8.0+）在 PATH，或通过 -MysqlExe 指定；集成可执行体由 CMake 构建。
# 退出码：0=通过，1=失败，77=环境未就绪（跳过，不计通过）。
param(
  [string]$MysqlExe = "mysql",
  [string]$HostName = $(if ($env:TNYCL_MYSQL_HOST) { $env:TNYCL_MYSQL_HOST } else { "127.0.0.1" }),
  [string]$Port = $(if ($env:TNYCL_MYSQL_PORT) { $env:TNYCL_MYSQL_PORT } else { "3306" }),
  [string]$User = $(if ($env:TNYCL_MYSQL_USER) { $env:TNYCL_MYSQL_USER } else { "root" }),
  [string]$Password = $env:TNYCL_MYSQL_PASSWORD,
  [string]$Database = $(if ($env:TNYCL_MYSQL_DB) { $env:TNYCL_MYSQL_DB } else { "tnycl_it" }),
  [string]$TestExe = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$v001 = Join-Path $repoRoot "migrations\V001__init.sql"
$v002 = Join-Path $repoRoot "sql\V002__seed_foods.sql"

if (-not (Get-Command $MysqlExe -ErrorAction SilentlyContinue)) {
  Write-Output "[SKIP] 未找到 mysql 客户端（$MysqlExe），跳过真实库集成测试（不计为通过）"
  exit 77
}
foreach ($file in @($v001, $v002)) {
  if (-not (Test-Path $file)) {
    Write-Output "[FAIL] 缺少 SQL 文件：$file"
    exit 1
  }
}

function Invoke-Mysql {
  param([string]$Sql, [switch]$IgnoreErrors)
  $args = @("--protocol=TCP", "-h", $HostName, "-P", $Port, "-u", $User, "--default-character-set=utf8mb4", "-N", "-B")
  if ($Password) { $args += "-p$Password" }
  if ($Sql) { $args += @("-e", $Sql) }
  $output = & $MysqlExe @args 2>&1
  $code = $LASTEXITCODE
  if ($code -ne 0 -and -not $IgnoreErrors) {
    Write-Output "[FAIL] mysql 执行失败（exit=$code）：$Sql"
    Write-Output $output
    exit 1
  }
  return @{ Code = $code; Output = $output }
}

Write-Output "[INFO] 目标库：$HostName`:$Port/$Database"
Invoke-Mysql -Sql "DROP DATABASE IF EXISTS ``$Database``; CREATE DATABASE ``$Database`` DEFAULT CHARACTER SET utf8mb4;" | Out-Null

# 1) V001 schema
$args = @("--protocol=TCP", "-h", $HostName, "-P", $Port, "-u", $User, "--default-character-set=utf8mb4", $Database)
if ($Password) { $args += "-p$Password" }
Get-Content -Raw -Encoding UTF8 $v001 | & $MysqlExe @args
if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL] V001 迁移失败"; exit 1 }
Write-Output "[PASS] V001 首次执行"

# 2) V002 第一次 + 第二次（幂等）
foreach ($round in 1..2) {
  Get-Content -Raw -Encoding UTF8 $v002 | & $MysqlExe @args
  if ($LASTEXITCODE -ne 0) { Write-Output "[FAIL] V002 第 $round 次执行失败"; exit 1 }
  $count = (Invoke-Mysql -Sql "SELECT COUNT(*) FROM ``$Database``.foods WHERE slug IN ('lemon-chicken','tomato-beef','mushroom-soup','shrimp-pasta','vegetable-rice','pepper-chicken');").Output
  if ("$count".Trim() -ne "6") {
    Write-Output "[FAIL] V002 第 $round 次执行后种子数应为 6，实际：$count"
    exit 1
  }
  Write-Output "[PASS] V002 第 $round 次执行（种子数=6）"
}

# 3) 唯一键冲突必须明确失败（同名不同 slug）
$conflict = Invoke-Mysql -IgnoreErrors -Sql "INSERT INTO ``$Database``.foods (slug, name, category, description, calories, protein_g, fiber_g, veggies_g, water_ml, image_url, active) VALUES ('it-conflict-probe','柠檬鸡胸能量碗','t','t',1,1,1,1,1,'',1);"
if ($conflict.Code -eq 0) {
  Write-Output "[FAIL] 同名不同 slug 未被唯一键拒绝（uk_name 失效）"
  Invoke-Mysql -IgnoreErrors -Sql "DELETE FROM ``$Database``.foods WHERE slug='it-conflict-probe';" | Out-Null
  exit 1
}
Write-Output "[PASS] 同名不同 slug 被 uk_name 拒绝（exit=$($conflict.Code)）"

# 4) 集成测试可执行体
if (-not $TestExe) {
  $candidates = @(
    (Join-Path $repoRoot "build\tests\mysql_integration_test.exe"),
    (Join-Path $repoRoot "build\mysql_integration_test.exe")
  )
  $TestExe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $TestExe) {
  Write-Output "[SKIP] 未找到 mysql_integration_test 可执行体，SQL 部分已通过；可执行体部分未执行"
  exit 77
}

$env:TNYCL_MYSQL_HOST = $HostName
$env:TNYCL_MYSQL_PORT = $Port
$env:TNYCL_MYSQL_USER = $User
$env:TNYCL_MYSQL_PASSWORD = $Password
$env:TNYCL_MYSQL_DB = $Database
& $TestExe
exit $LASTEXITCODE
