# =====================================================================
# database/tests/advisory_lock_test.ps1
#
# 调度锁语义测试（只编写，不执行；由项目负责人手动运行）。
#
# 验证 PostgreSQL 事务级 advisory lock 的 4 条语义，这些语义是
# services/api 调度器实现的前提：
#   1. 同一 key 在锁被持有时 pg_try_advisory_xact_lock 返回 false
#      （调度器据此“立即跳过”，而不是排队等待）
#   2. 持锁事务 COMMIT 后锁自动释放
#   3. 持锁事务 ROLLBACK 后锁自动释放
#   4. 不同 key 之间互不阻塞（跨实例同 job 才互斥）
#
# 用法：
#   $env:PGPASSWORD = '<密码>'
#   pwsh -NoProfile -File database/tests/advisory_lock_test.ps1 `
#        -Host 127.0.0.1 -Port 5432 -Database usapp -User usapp_api
#
# 说明：脚本用文件重定向运行两个 psql 进程，避免依赖管道；
#       除用例 1 需要真正的并发外，其余用例在单连接内即可判定。
# =====================================================================
[CmdletBinding()]
param(
    [string]$HostName = '127.0.0.1',
    [int]   $Port     = 5432,
    [string]$Database = 'usapp',
    [string]$User     = 'usapp_api',
    [string]$Password,
    [long]  $LockKey  = 7370624951540231400,   # 'usapp.scheduler:scheduler.heartbeat'
    [long]  $OtherKey = 7370624951540231401
)

$ErrorActionPreference = 'Stop'

if ($Password) { $env:PGPASSWORD = $Password }
if (-not $env:PGPASSWORD) { throw "未设置 PGPASSWORD。" }
if (-not (Get-Command psql -ErrorAction SilentlyContinue)) { throw "未找到 psql。" }

$psqlBase = @('-h', $HostName, '-p', "$Port", '-U', $User, '-d', $Database,
              '-v', 'ON_ERROR_STOP=1', '-X', '-q', '-t', '-A')

$failures = New-Object System.Collections.Generic.List[string]

function Assert-Eq {
    param([string]$Name, [string]$Expected, [string]$Actual)
    if ($Actual -eq $Expected) {
        Write-Host "  [ok]   $Name = $Actual"
    } else {
        Write-Host "  [FAIL] $Name 期望 '$Expected'，实际 '$Actual'"
        $script:failures.Add($Name)
    }
}

function Invoke-PsqlScalar {
    param([string]$Sql)
    $out = & psql @psqlBase -c $Sql 2>&1
    if ($LASTEXITCODE -ne 0) { throw "psql 失败：`n$out" }
    return ($out | Where-Object { $_ -ne '' } | Select-Object -Last 1).ToString().Trim()
}

function Invoke-PsqlBatch {
    param([string]$Sql, [string]$LogFile)
    $args = $psqlBase + @('-c', $Sql)
    $proc = Start-Process -FilePath 'psql' -ArgumentList $args -NoNewWindow -PassThru `
                          -RedirectStandardOutput $LogFile -RedirectStandardError "$LogFile.err"
    return $proc
}

Write-Host "advisory lock 语义测试（key=$LockKey）"

# ---------------------------------------------------------------
# 用例 1：锁被持有 -> 立即返回 false（不排队）
# ---------------------------------------------------------------
Write-Host "1) 持锁期间并发获取应立即失败"
$logA = Join-Path $env:TEMP "usapp-lock-a.log"
$holder = Invoke-PsqlBatch -LogFile $logA -Sql @"
BEGIN;
SELECT pg_try_advisory_xact_lock($LockKey) AS acquired;
SELECT pg_sleep(4);
COMMIT;
"@

Start-Sleep -Milliseconds 1200
$got = Invoke-PsqlScalar -Sql "SELECT pg_try_advisory_xact_lock($LockKey);"
Assert-Eq -Name '1. 并发抢锁返回 false' -Expected 'f' -Actual $got

$holder.WaitForExit()
$holderOut = (Get-Content -Path $logA -Raw).Trim()
Assert-Eq -Name '1b. 持锁方初次获取返回 true' -Expected 't' -Actual (($holderOut -split "`r?`n")[0].Trim())

# ---------------------------------------------------------------
# 用例 2：COMMIT 后锁释放
# ---------------------------------------------------------------
Write-Host "2) COMMIT 释放锁"
$got = Invoke-PsqlScalar -Sql "SELECT pg_try_advisory_xact_lock($LockKey);"
Assert-Eq -Name '2. COMMIT 后可再次获取' -Expected 't' -Actual $got

# ---------------------------------------------------------------
# 用例 3：ROLLBACK 后锁释放
# ---------------------------------------------------------------
Write-Host "3) ROLLBACK 释放锁"
$got = Invoke-PsqlScalar -Sql @"
BEGIN;
SELECT pg_try_advisory_xact_lock($LockKey);
ROLLBACK;
SELECT pg_try_advisory_xact_lock($LockKey);
"@
Assert-Eq -Name '3. ROLLBACK 后可再次获取' -Expected 't' -Actual $got

# ---------------------------------------------------------------
# 用例 4：不同 key 互不阻塞
# ---------------------------------------------------------------
Write-Host "4) 不同 key 互不阻塞"
$logB = Join-Path $env:TEMP "usapp-lock-b.log"
$holder2 = Invoke-PsqlBatch -LogFile $logB -Sql @"
BEGIN;
SELECT pg_try_advisory_xact_lock($LockKey);
SELECT pg_sleep(3);
COMMIT;
"@

Start-Sleep -Milliseconds 1000
$got = Invoke-PsqlScalar -Sql "SELECT pg_try_advisory_xact_lock($OtherKey);"
Assert-Eq -Name '4. 其他 key 不受影响' -Expected 't' -Actual $got
$holder2.WaitForExit()

# ---------------------------------------------------------------
# 用例 5：同一事务内重复获取同 key 是幂等的（同一连接）
# ---------------------------------------------------------------
Write-Host "5) 同一事务内重复获取同 key 幂等"
$got = Invoke-PsqlScalar -Sql @"
BEGIN;
SELECT pg_try_advisory_xact_lock($LockKey);
SELECT pg_try_advisory_xact_lock($LockKey);
COMMIT;
"@
Assert-Eq -Name '5. 同事务重复获取返回 true' -Expected 't' -Actual $got

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host "advisory lock 测试失败：$($failures -join '; ')"
    exit 1
}
Write-Host "advisory lock 测试全部通过。"
