# =====================================================================
# database/apply.ps1
# USapp 阶段 0 数据库迁移应用脚本（供项目负责人手动执行；Agent 不执行任何命令）。
#
# 特性与设计原则：
#   1. 严格事务边界校验：
#      - 每个待应用迁移文件必须有且仅有一个最外层 BEGIN; 与对应的 COMMIT;
#      - 顺序必须正确，最外层事务边界之外禁止有任何可执行语句（只允许空白与注释）
#      - 事务内部禁止包含顶层 BEGIN/COMMIT/ROLLBACK 控制语句（多段事务或格式异常直接报错）
#      - 自动识别并跳过单引号字面量与 $$...$$ / $tag$...$tag$ 美元引用块，避免误判 PL/pgSQL 内部语句
#      - 在执行任何迁移前，先预检所有待应用文件的事务边界，结构不支持则立即失败，严禁自动包装
#      - 原始迁移文件在磁盘上保持只读不变
#   2. 统一版本格式与完整性：
#      - 迁移版本以原始 SQL 文件的完整 BaseName 为准（如 0000_schema_migrations）
#      - checksum 基于原始 SQL 文件二进制字节计算 SHA-256
#   3. 原子单事务提交与并发锁：
#      - 将 DDL 与 INSERT INTO schema_migrations 记录注入在同一个物理事务内原子提交
#      - 事务开头注入 pg_try_advisory_xact_lock(7492000) 互斥锁，杜绝并发冲突
#   4. 完备只读 DryRun（含不存在数据库处理）：
#      - 若目标数据库不存在且带 -DryRun：仅输出计划创建与待应用迁移列表，不建库、不连接不存在库、不执行赋权
#      - 若目标库存在：探测 to_regclass('public.schema_migrations')，未建表时不提前建表，保持 100% 只读
#   5. 双环境运行支持（Windows PowerShell 5.1 与 PowerShell 7+）：
#      - 脚本自身以 UTF-8 with BOM 保存，兼容系统自带 Windows PowerShell 5.1
#      - 传入 PostgreSQL 的临时文件使用无 BOM UTF-8，杜绝 Linux psql BOM 乱码
#      - 支持 Docker 模式（默认，无需宿主 psql 与 5432 端口）与 Host 模式
#   6. 健壮的外部进程标准流分离与退出码处理：
#      - 通过标准流重定向分离 stdout 与 stderr，避免进度输出与提示混入查询结果
#      - 执行外部命令期间仅临时调整错误策略，并在 finally 中严格恢复，杜绝 NativeCommandError 误报
#      - 执行后立即保存 $LASTEXITCODE，严格以自身退出码判定成功或失败
# =====================================================================
[CmdletBinding()]
param(
    [ValidateSet('Docker', 'Host')]
    [string]$Mode = 'Docker',

    [Alias('Host')]
    [string]$HostName = '127.0.0.1',

    [int]   $Port = 5432,

    [string]$Database = 'usapp',

    [string]$User,

    [string]$Password,

    [string]$MigrationsDir,

    [string]$ComposeFile,

    [string]$EnvFile,

    [string]$Service = 'postgres',

    [switch]$CreateDatabase,

    [switch]$InitRoles,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# 1. 路径解析与默认值设置
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not $MigrationsDir) {
    $MigrationsDir = Join-Path $PSScriptRoot 'migrations'
}
if (-not (Test-Path $MigrationsDir)) {
    throw "迁移目录不存在: $MigrationsDir"
}

if (-not $ComposeFile) {
    $ComposeFile = Join-Path $RepoRoot 'deploy/docker-compose.yml'
}

if (-not $EnvFile) {
    $defaultEnv = Join-Path $RepoRoot '.env'
    if (Test-Path $defaultEnv) {
        $EnvFile = $defaultEnv
    }
}

# 默认用户：迁移与 DDL 必须由超级用户执行
if (-not $User) {
    if ($env:POSTGRES_SUPERUSER) {
        $User = $env:POSTGRES_SUPERUSER
    } else {
        $User = 'postgres'
    }
}

if (-not $Password) {
    if ($env:POSTGRES_SUPERUSER_PASSWORD) {
        $Password = $env:POSTGRES_SUPERUSER_PASSWORD
    } elseif ($env:PGPASSWORD) {
        $Password = $env:PGPASSWORD
    }
}

# Compose 基础命令行参数
$composeArgs = @('compose', '-f', $ComposeFile)
if ($EnvFile -and (Test-Path $EnvFile)) {
    $composeArgs += @('--env-file', $EnvFile)
}

# 2. 执行模式检查
if ($Mode -eq 'Docker') {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw "未找到 docker 命令。请确保 Docker Desktop 已启动并加入 PATH，或使用 -Mode Host。"
    }
} else {
    if (-not (Get-Command psql -ErrorAction SilentlyContinue)) {
        throw "未找到 psql。请把 PostgreSQL 17 的 bin 目录加入 PATH 后重试，或使用默认的 -Mode Docker。"
    }
    if ($Password) { $env:PGPASSWORD = $Password }
    if (-not $env:PGPASSWORD -and -not $DryRun) {
        Write-Warning "Host 模式未设置 PGPASSWORD。若数据库需要密码认证，请提供 -Password 或先设置 `$env:PGPASSWORD。"
    }
}

# 3. 底层进程调用适配器与标准流分离
function Invoke-NativeCommand {
    param(
        [string]$Command,
        [string[]]$CommandArgs
    )

    $tempErr = [System.IO.Path]::GetTempFileName()
    $prevEap = $ErrorActionPreference

    try {
        # 临时将错误动作首选项设为 Continue，避免 Windows PowerShell 5.1
        # 因外部命令向 stderr 输出常规进度/提示信息时误抛出 NativeCommandError 异常
        $ErrorActionPreference = 'Continue'

        $stdout = @(& $Command @CommandArgs 2>$tempErr)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }

    $stderrText = ''
    try {
        if (Test-Path $tempErr) {
            $stderrText = [System.IO.File]::ReadAllText($tempErr, [System.Text.Encoding]::UTF8)
        }
    } catch {
        # 忽略读取临时 stderr 文件的非关键异常
    } finally {
        if (Test-Path $tempErr) {
            Remove-Item -Path $tempErr -Force -ErrorAction SilentlyContinue
        }
    }

    return [PSCustomObject]@{
        ExitCode = $exitCode
        Stdout   = $stdout
        Stderr   = $stderrText.Trim()
    }
}

function Invoke-PsqlQuery {
    param(
        [string]$Sql,
        [string]$TargetDb = $Database
    )

    if ($Mode -eq 'Docker') {
        $execArgs = @()
        $execArgs += $composeArgs
        $execArgs += @('exec')
        if ($Password) {
            $execArgs += @('-e', "PGPASSWORD=$Password")
        }
        $execArgs += @('-T', $Service, 'psql', '-U', $User, '-d', $TargetDb,
                       '-v', 'ON_ERROR_STOP=1', '-X', '-q', '-t', '-A', '-c', $Sql)

        $res = Invoke-NativeCommand -Command 'docker' -CommandArgs $execArgs
        if ($res.ExitCode -ne 0) {
            $errMsg = "Docker psql 查询执行失败 (exit $($res.ExitCode))"
            if (-not [string]::IsNullOrWhiteSpace($res.Stderr)) { $errMsg += ":`n$($res.Stderr)" }
            elseif (-not [string]::IsNullOrWhiteSpace($res.Stdout)) { $errMsg += ":`n$($res.Stdout -join "`n")" }
            throw $errMsg
        }
        return $res.Stdout
    } else {
        $args = @('-h', $HostName, '-p', "$Port", '-U', $User, '-d', $TargetDb,
                  '-v', 'ON_ERROR_STOP=1', '-X', '-q', '-t', '-A', '-c', $Sql)
        $res = Invoke-NativeCommand -Command 'psql' -CommandArgs $args
        if ($res.ExitCode -ne 0) {
            $errMsg = "宿主 psql 查询执行失败 (exit $($res.ExitCode))"
            if (-not [string]::IsNullOrWhiteSpace($res.Stderr)) { $errMsg += ":`n$($res.Stderr)" }
            elseif (-not [string]::IsNullOrWhiteSpace($res.Stdout)) { $errMsg += ":`n$($res.Stdout -join "`n")" }
            throw $errMsg
        }
        return $res.Stdout
    }
}

function Invoke-PsqlFile {
    param(
        [string]$FilePath,
        [string]$TargetDb = $Database
    )

    if ($Mode -eq 'Docker') {
        $containerTmp = "/tmp/usapp_mig_$([guid]::NewGuid().ToString('N')).sql"

        # 1. 复制文件进容器（由命令自身退出码判断结果，忽略 stderr 中的正常复制进度输出）
        $cpArgs = @()
        $cpArgs += $composeArgs
        $cpArgs += @('cp', $FilePath, "${Service}:${containerTmp}")
        $cpRes = Invoke-NativeCommand -Command 'docker' -CommandArgs $cpArgs
        if ($cpRes.ExitCode -ne 0) {
            $errMsg = "将文件复制进容器失败 (exit $($cpRes.ExitCode))"
            if (-not [string]::IsNullOrWhiteSpace($cpRes.Stderr)) { $errMsg += ":`n$($cpRes.Stderr)" }
            elseif (-not [string]::IsNullOrWhiteSpace($cpRes.Stdout)) { $errMsg += ":`n$($cpRes.Stdout -join "`n")" }
            throw $errMsg
        }

        $execError = $null
        try {
            # 2. 容器内执行 SQL 文件
            $execArgs = @()
            $execArgs += $composeArgs
            $execArgs += @('exec')
            if ($Password) {
                $execArgs += @('-e', "PGPASSWORD=$Password")
            }
            $execArgs += @('-T', $Service, 'psql', '-U', $User, '-d', $TargetDb,
                           '-v', 'ON_ERROR_STOP=1', '-X', '-q', '-f', $containerTmp)

            $execRes = Invoke-NativeCommand -Command 'docker' -CommandArgs $execArgs
            if ($execRes.ExitCode -ne 0) {
                $errMsg = "容器内执行 SQL 失败 (exit $($execRes.ExitCode))"
                if (-not [string]::IsNullOrWhiteSpace($execRes.Stderr)) { $errMsg += ":`n$($execRes.Stderr)" }
                elseif (-not [string]::IsNullOrWhiteSpace($execRes.Stdout)) { $errMsg += ":`n$($execRes.Stdout -join "`n")" }
                $execError = $errMsg
                throw $errMsg
            }
            return $execRes.Stdout
        } finally {
            # 3. 及时清理容器内临时 SQL 文件（杜绝清理逻辑覆盖原始执行错误）
            try {
                $rmArgs = @()
                $rmArgs += $composeArgs
                $rmArgs += @('exec', '-T', $Service, 'rm', '-f', $containerTmp)
                $null = Invoke-NativeCommand -Command 'docker' -CommandArgs $rmArgs
            } catch {
                if (-not $execError) {
                    Write-Warning "清理容器内临时文件失败: $($_.Exception.Message)"
                }
            }
        }
    } else {
        # Host 模式
        $args = @('-h', $HostName, '-p', "$Port", '-U', $User, '-d', $TargetDb,
                  '-v', 'ON_ERROR_STOP=1', '-X', '-q', '-f', $FilePath)
        $res = Invoke-NativeCommand -Command 'psql' -CommandArgs $args
        if ($res.ExitCode -ne 0) {
            $errMsg = "宿主 psql 执行 SQL 文件失败 (exit $($res.ExitCode))"
            if (-not [string]::IsNullOrWhiteSpace($res.Stderr)) { $errMsg += ":`n$($res.Stderr)" }
            elseif (-not [string]::IsNullOrWhiteSpace($res.Stdout)) { $errMsg += ":`n$($res.Stdout -join "`n")" }
            throw $errMsg
        }
        return $res.Stdout
    }
}

# 4. 严格词法扫描与顶层语句解析
function Parse-SqlTopLevelStatements {
    param(
        [string]$Sql,
        [string]$FileName
    )

    $len = $Sql.Length
    $i = 0

    $statements = [System.Collections.Generic.List[PSCustomObject]]::new()
    $currentTokens = [System.Collections.Generic.List[string]]::new()
    $stmtStartIndex = -1

    while ($i -lt $len) {
        $c = $Sql[$i]

        # 1. 忽略空白字符
        if ([char]::IsWhiteSpace($c)) {
            $i++
            continue
        }

        # 2. 注释与 '-' 符号（负数或减号）
        if ($c -eq '-') {
            if ($i + 1 -lt $len -and $Sql[$i + 1] -eq '-') {
                # 单行注释: --
                $i += 2
                while ($i -lt $len -and $Sql[$i] -ne "`n") {
                    $i++
                }
                if ($i -lt $len -and $Sql[$i] -eq "`n") { $i++ }
                continue
            } else {
                # 单独的 '-' 符号（支持当前迁移中的负数与操作符，记录为 token 并明确推进游标）
                if ($stmtStartIndex -lt 0) { $stmtStartIndex = $i }
                $currentTokens.Add('-')
                $i++
                continue
            }
        }

        # 3. 块注释与 '/' 符号（除号或路径）
        if ($c -eq '/') {
            if ($i + 1 -lt $len -and $Sql[$i + 1] -eq '*') {
                # 块注释: /* ... */
                $commentStart = $i
                $i += 2
                $closed = $false
                while ($i + 1 -lt $len) {
                    if ($Sql[$i] -eq '*' -and $Sql[$i + 1] -eq '/') {
                        $i += 2
                        $closed = $true
                        break
                    }
                    $i++
                }
                if (-not $closed) {
                    throw "迁移文件 $FileName 包含未闭合的块注释 (/* 开始于字符位置 $commentStart)。"
                }
                continue
            } else {
                # 单独的 '/' 符号（除号或路径符号，记录为 token 并明确推进游标）
                if ($stmtStartIndex -lt 0) { $stmtStartIndex = $i }
                $currentTokens.Add('/')
                $i++
                continue
            }
        }

        # 4. 单引号字符串字面量: '...' (支持 '' 转义)
        if ($c -eq "'") {
            if ($stmtStartIndex -lt 0) { $stmtStartIndex = $i }
            $strStart = $i
            $i++
            $closed = $false
            while ($i -lt $len) {
                if ($Sql[$i] -eq "'") {
                    if ($i + 1 -lt $len -and $Sql[$i + 1] -eq "'") {
                        $i += 2
                        continue
                    } else {
                        $i++
                        $closed = $true
                        break
                    }
                }
                $i++
            }
            if (-not $closed) {
                throw "迁移文件 $FileName 包含未闭合的单引号字符串 (开始于字符位置 $strStart)。"
            }
            continue
        }

        # 5. 美元引用块 (PL/pgSQL 函数体或 DO 块): $tag$...$tag$ 或 $$...$$
        if ($c -eq '$') {
            $sub = $Sql.Substring($i)
            $tagMatch = [regex]::Match($sub, '^\$([a-zA-Z0-9_]*)\$')
            if ($tagMatch.Success) {
                if ($stmtStartIndex -lt 0) { $stmtStartIndex = $i }
                $tag = $tagMatch.Value
                $tagLen = $tag.Length
                $strStart = $i
                $i += $tagLen
                $closeIdx = $Sql.IndexOf($tag, $i)
                if ($closeIdx -lt 0) {
                    throw "迁移文件 $FileName 包含未闭合的美元符号引用块 (开始于字符位置 $strStart, 标签: '$tag')。"
                }
                $i = $closeIdx + $tagLen
                continue
            } else {
                # 不支持的美元符号用法，明确报错输出文件名与位置，禁止停在原位循环
                throw "迁移文件 $FileName 在字符位置 $i 处检测到不支持的美元符号 '$'（仅支持标准美元引用块如 `$`$`$ 或 `$tag$`）。"
            }
        }

        # 6. 分号: 顶层语句结束标志
        if ($c -eq ';') {
            if ($stmtStartIndex -ge 0) {
                $stmtEndIndex = $i + 1
                $statements.Add([PSCustomObject]@{
                    Tokens     = @($currentTokens)
                    StartIndex = $stmtStartIndex
                    EndIndex   = $stmtEndIndex
                    Text       = $Sql.Substring($stmtStartIndex, $stmtEndIndex - $stmtStartIndex).Trim()
                })
                $currentTokens.Clear()
                $stmtStartIndex = -1
            }
            $i++
            continue
        }

        # 7. 普通标识符 / 关键字
        if ($stmtStartIndex -lt 0) { $stmtStartIndex = $i }
        $tokStart = $i
        while ($i -lt $len -and -not [char]::IsWhiteSpace($Sql[$i]) -and $Sql[$i] -ne ';' -and $Sql[$i] -ne '-' -and $Sql[$i] -ne '/' -and $Sql[$i] -ne "'" -and $Sql[$i] -ne '$') {
            $i++
        }

        # 严格防御性校验：必须确保确实消费了至少一个字符，未消费时立即报错，杜绝添加空 token
        if ($i -le $tokStart) {
            $badChar = $Sql[$tokStart]
            throw "迁移文件 $FileName 词法扫描异常：字符位置 $tokStart 处的未识别字符 '$badChar' (ASCII $([int]$badChar)) 未被消费，禁止添加空 token。"
        }

        $token = $Sql.Substring($tokStart, $i - $tokStart)
        $currentTokens.Add($token)
    }

    if ($currentTokens.Count -gt 0) {
        $stmtEndIndex = $len
        $statements.Add([PSCustomObject]@{
            Tokens     = @($currentTokens)
            StartIndex = $stmtStartIndex
            EndIndex   = $stmtEndIndex
            Text       = $Sql.Substring($stmtStartIndex, $stmtEndIndex - $stmtStartIndex).Trim()
        })
    }

    return $statements
}

# 5. 严格验证事务边界（不符合要求立即抛出异常，杜绝自动包装）
function Validate-And-Parse-Migration {
    param(
        [string]$Sql,
        [string]$FileName
    )

    $statements = Parse-SqlTopLevelStatements -Sql $Sql -FileName $FileName

    if ($statements.Count -lt 2) {
        throw "迁移文件 $FileName 事务边界验证失败：未找到完整的顶层 BEGIN; 与 COMMIT; 语句（顶层语句总数: $($statements.Count)）。"
    }

    # 1. 验证首条语句必须且只能是最外层 BEGIN;
    $firstStmt = $statements[0]
    $firstTok = if ($firstStmt.Tokens.Count -gt 0) { $firstStmt.Tokens[0].ToUpperInvariant() } else { '' }
    if ($firstTok -ne 'BEGIN') {
        throw "迁移文件 $FileName 事务边界验证失败：文件首条可执行语句必须是 BEGIN;，实际为 '$($firstStmt.Text)'。最外层事务边界之前不允许有可执行语句。"
    }
    if ($firstStmt.Tokens.Count -gt 2 -or ($firstStmt.Tokens.Count -eq 2 -and ($firstStmt.Tokens[1].ToUpperInvariant() -notin @('TRANSACTION', 'WORK')))) {
        throw "迁移文件 $FileName 事务边界验证失败：首条 BEGIN 语句语法不合规 ('$($firstStmt.Text)')。"
    }
    if (-not $firstStmt.Text.EndsWith(';')) {
        throw "迁移文件 $FileName 事务边界验证失败：首条 BEGIN 语句缺少分号终止符。"
    }

    # 2. 验证末尾语句必须且只能是最外层 COMMIT;
    $lastStmt = $statements[$statements.Count - 1]
    $lastTok = if ($lastStmt.Tokens.Count -gt 0) { $lastStmt.Tokens[0].ToUpperInvariant() } else { '' }
    if ($lastTok -ne 'COMMIT') {
        throw "迁移文件 $FileName 事务边界验证失败：文件末尾可执行语句必须是 COMMIT;，实际为 '$($lastStmt.Text)'。最外层事务边界之后不允许有可执行语句。"
    }
    if ($lastStmt.Tokens.Count -gt 2 -or ($lastStmt.Tokens.Count -eq 2 -and ($lastStmt.Tokens[1].ToUpperInvariant() -notin @('TRANSACTION', 'WORK')))) {
        throw "迁移文件 $FileName 事务边界验证失败：末尾 COMMIT 语句语法不合规 ('$($lastStmt.Text)')。"
    }
    if (-not $lastStmt.Text.EndsWith(';')) {
        throw "迁移文件 $FileName 事务边界验证失败：末尾 COMMIT 语句缺少分号终止符。"
    }

    # 3. 验证事务内部（第 2 条至倒数第 2 条语句）绝对不能包含顶层 BEGIN、COMMIT 或 ROLLBACK 控制语句
    for ($idx = 1; $idx -lt ($statements.Count - 1); $idx++) {
        $midStmt = $statements[$idx]
        $midTok = if ($midStmt.Tokens.Count -gt 0) { $midStmt.Tokens[0].ToUpperInvariant() } else { '' }
        if ($midTok -in @('BEGIN', 'COMMIT', 'ROLLBACK', 'START')) {
            throw "迁移文件 $FileName 事务边界验证失败：在事务内部（第 $($idx + 1) 条顶层语句）检测到非法事务控制语句 '$($midStmt.Text)'。本项目仅支持单段单一最外层事务，禁止多段事务或顶层嵌套事务。"
        }
    }

    return [PSCustomObject]@{
        BeginEndIndex    = $firstStmt.EndIndex
        CommitStartIndex = $lastStmt.StartIndex
    }
}

# 6. 单事务注入函数（在首条 BEGIN; 后注入排他锁，在末尾 COMMIT; 前注入版本登记）
function Build-AtomicMigrationContent {
    param(
        [string]$OriginalSql,
        [string]$Version,
        [string]$Checksum,
        [string]$FileName
    )

    $boundary = Validate-And-Parse-Migration -Sql $OriginalSql -FileName $FileName

    $escapedVersion = $Version.Replace("'", "''")
    $escapedChecksum = $Checksum.Replace("'", "''")

    $lockSql = @"

-- [apply.ps1 注入: 事务级迁移并发锁 (7492000)]
DO `$usapp_mig_lock`$
BEGIN
    IF NOT pg_try_advisory_xact_lock(7492000) THEN
        RAISE EXCEPTION '获取迁移锁失败：另一迁移进程正在执行中 (lock_key=7492000)';
    END IF;
END `$usapp_mig_lock`$;

"@

    $insertSql = @"

-- [apply.ps1 注入: 迁移版本记录（与 DDL 原子提交）]
INSERT INTO schema_migrations (version, checksum, applied_by, applied_at)
VALUES ('$escapedVersion', '$escapedChecksum', current_user, now());

"@

    $beforeBegin = $OriginalSql.Substring(0, $boundary.BeginEndIndex)
    $middleDdl = $OriginalSql.Substring($boundary.BeginEndIndex, $boundary.CommitStartIndex - $boundary.BeginEndIndex)
    $afterCommit = $OriginalSql.Substring($boundary.CommitStartIndex)

    return ($beforeBegin + "`n" + $lockSql + "`n" + $middleDdl + "`n" + $insertSql + "`n" + $afterCommit)
}

# =====================================================================
# 主流程
# =====================================================================

Write-Host "=========================================================="
Write-Host "USapp 数据库迁移应用流程"
Write-Host "  运行模式:     $Mode"
Write-Host "  目标数据库:   $Database"
Write-Host "  执行角色:     $User (超级用户/DDL所有者)"
Write-Host "  迁移文件目录: $MigrationsDir"
Write-Host "  DryRun:       $($DryRun.IsPresent)"
Write-Host "=========================================================="

# A. 扫描所有待应用的迁移文件
$files = Get-ChildItem -Path $MigrationsDir -Filter '*.sql' | Sort-Object Name
if (-not $files) {
    throw "在 $MigrationsDir 下没有找到迁移文件。"
}

# B. 预检所有迁移文件的事务边界（执行前统一校验，任一文件不支持立即停止，杜绝执行途中失败）
Write-Host "[check] 预检全部 $($files.Count) 个迁移文件的事务边界格式..."
foreach ($file in $files) {
    $rawContent = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    $null = Validate-And-Parse-Migration -Sql $rawContent -FileName $file.Name
}
Write-Host "[check] 事务边界严格预检通过 (各文件均含唯一顶层 BEGIN; 与 COMMIT;，无事务外可执行语句)。"

# C. 检查目标数据库是否存在
Write-Host "[db   ] 检查目标数据库 '$Database' 是否存在..."
$dbCheck = Invoke-PsqlQuery -TargetDb 'postgres' `
    -Sql "SELECT 1 FROM pg_database WHERE datname = '$Database';"
$dbCheckStr = if ($dbCheck -is [array]) { ($dbCheck -join '').Trim() } else { "$dbCheck".Trim() }
$dbExists = ($dbCheckStr -eq '1')

# 处理数据库不存在的情况
if (-not $dbExists) {
    if ($DryRun) {
        Write-Host "[dry  ] 目标数据库 '$Database' 当前不存在。"
        if ($CreateDatabase) {
            Write-Host "[dry  ] 计划创建目标数据库 '$Database' (DryRun: 不实际创建)。"
        } else {
            Write-Warning "目标数据库 '$Database' 不存在，且未指定 -CreateDatabase。实际运行时将无法连接目标库。"
        }
        Write-Host "[dry  ] 由于数据库尚未创建，所有迁移均为待应用状态："
        foreach ($file in $files) {
            $checksum = (Get-FileHash -Path $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            Write-Host "[dry  ] $($file.BaseName) (待应用, sha256: $checksum)"
        }
        if ($InitRoles) {
            Write-Host "[dry  ] 计划在迁移完成后执行角色赋权 database/init-roles.sql (DryRun: 不实际执行)。"
        }
        Write-Host "=========================================================="
        Write-Host "DryRun 预览完成。待应用: $($files.Count) 项。数据库未做任何更改。"
        Write-Host "=========================================================="
        return
    }

    # 实际执行模式（非 DryRun）
    if ($CreateDatabase) {
        Write-Host "[db   ] 创建目标数据库 '$Database' ..."
        Invoke-PsqlQuery -TargetDb 'postgres' -Sql "CREATE DATABASE `"$Database`";" | Out-Null
        Write-Host "[db   ] 数据库 '$Database' 创建成功。"
    } else {
        throw "目标数据库 '$Database' 不存在！请先指定 -CreateDatabase 参数以自动创建，或手动在 PostgreSQL 中创建。"
    }
} else {
    Write-Host "[db   ] 目标数据库 '$Database' 已存在。"
}

# D. 目标库存在：检查迁移表 schema_migrations 是否已存在（只读探测，杜绝在 DryRun 或迁移前私自建表）
$regClass = Invoke-PsqlQuery -Sql "SELECT COALESCE(to_regclass('public.schema_migrations')::text, '');"
$regClassStr = if ($regClass -is [array]) { ($regClass -join '').Trim() } else { "$regClass".Trim() }
$tableExists = (-not [string]::IsNullOrWhiteSpace($regClassStr))

$applied = @{}
if ($tableExists) {
    $rows = Invoke-PsqlQuery -Sql "SELECT version || '|' || checksum FROM schema_migrations ORDER BY version;"
    foreach ($row in $rows) {
        if ([string]::IsNullOrWhiteSpace($row)) { continue }
        $trimmedRow = "$row".Trim()
        if (-not $trimmedRow.Contains('|')) { continue }
        $parts = $trimmedRow.Split('|', 2)
        $applied[$parts[0]] = $parts[1]
    }
} else {
    Write-Host "[info ] 表 public.schema_migrations 尚未建立 (将由 0000_schema_migrations.sql 创建)。"
}

# E. 遍历迁移文件，对比已应用记录与 checksum
$pendingCount = 0
$skipCount = 0

foreach ($file in $files) {
    $version = $file.BaseName
    $checksum = (Get-FileHash -Path $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()

    if ($applied.ContainsKey($version)) {
        if ($applied[$version] -ne $checksum) {
            throw "迁移 $version 已应用但内容已被修改 (checksum 不一致: 库中为 $($applied[$version]), 本地为 $checksum)。禁止修改已应用的迁移！"
        }
        Write-Host "[skip ] $version (已应用，checksum 校验一致)"
        $skipCount++
        continue
    }

    $pendingCount++
    if ($DryRun) {
        Write-Host "[dry  ] $version (待应用, sha256: $checksum)"
        continue
    }

    Write-Host "[apply] $version (sha256: $checksum) ..."

    # 读取原始 SQL 并在内存构建单事务原子文件
    $rawContent = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    $atomicContent = Build-AtomicMigrationContent -OriginalSql $rawContent -Version $version -Checksum $checksum -FileName $file.Name

    # 写入本地无 BOM 的 UTF-8 临时文件（供 Linux psql 安全解析）
    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) "usapp_mig_${version}_$([guid]::NewGuid().ToString('N')).sql"
    try {
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($tempFile, $atomicContent, $utf8NoBom)

        # 单事务执行迁移与记录
        Invoke-PsqlFile -FilePath $tempFile | Out-Null
        Write-Host "[ok   ] $version 应用并登记成功。"
    } finally {
        if (Test-Path $tempFile) {
            Remove-Item -Path $tempFile -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($DryRun) {
    if ($InitRoles) {
        Write-Host "[dry  ] 计划在迁移完成后执行角色赋权 database/init-roles.sql (DryRun: 不实际执行)。"
    }
    Write-Host "=========================================================="
    Write-Host "DryRun 预览完成。已应用: $skipCount 项，待应用: $pendingCount 项。数据库未做任何更改。"
    Write-Host "=========================================================="
} else {
    Write-Host "=========================================================="
    Write-Host "迁移完成。本次已跳过: $skipCount 项，本次新应用: $pendingCount 项。"
    Write-Host "=========================================================="

    # F. 执行角色赋权（若指定 -InitRoles）
    if ($InitRoles) {
        $initRolesScript = Join-Path $PSScriptRoot 'init-roles.sql'
        if (-not (Test-Path $initRolesScript)) {
            throw "角色赋权脚本不存在: $initRolesScript"
        }
        Write-Host "[roles] 执行 database/init-roles.sql 对应用及推送账户赋权..."
        Invoke-PsqlFile -FilePath $initRolesScript | Out-Null
        Write-Host "[roles] 角色与权限初始化完成。"
    }
}
