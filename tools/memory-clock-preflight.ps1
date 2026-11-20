# Controlled-clock baseline and fixed guard proof on fresh owned schema through 0053.
# Restores the old frozen guard/default inside rollback-only tests; never changes the host clock.
param([string]$Image = 'sha256:83afaf6179c2f2c80c73ee209f907773f33a67deac1610905924ed17e66cbfb1')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$expected0047 = '75d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0047_achievements.sql')).Hash.ToLowerInvariant() -ne $expected0047) { throw 'Frozen 0047 bytes changed.' }
$migrations = @(Get-ChildItem -LiteralPath (Join-Path $repo 'database/migrations') -Filter '*.sql')
if ($migrations.Count -ne 54 -or ($migrations | Sort-Object Name | Select-Object -Last 1).BaseName -ne '0053_memory_decision_timestamp') { throw 'Unexpected migration set.' }
foreach($frozenLine in [IO.File]::ReadAllLines((Join-Path $repo 'database/frozen-through-0048.sha256'))) {
 $parts=$frozenLine.Split(' ');$frozenPath=Join-Path $repo ('database/migrations/'+$parts[0]);
 if((Get-FileHash -Algorithm SHA256 -LiteralPath $frozenPath).Hash.ToLowerInvariant() -ne $parts[1]){throw 'Frozen pre-0049 migration changed.'}
}
$expected0049='24924f637330465867b96373d619d43be765ee424b13346b03d50b8e3a462e96'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0049_admin_wallet_adjustments.sql')).Hash.ToLowerInvariant() -ne $expected0049){throw 'Frozen 0049 bytes changed.'}
$expected0050='450c99df43abc2822c6e047fac7796f86813fbe9e44a2afda74acd6c1aec81ba'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0050_wallet_member_actor_compat.sql')).Hash.ToLowerInvariant() -ne $expected0050){throw 'Frozen 0050 bytes changed.'}
$expected0051='df846c323f905cf5465a91306a0436aabd90495338ad24c7e2c45ddb72878b4c'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0051_admin_redemption_cancellations.sql')).Hash.ToLowerInvariant() -ne $expected0051){throw 'Frozen 0051 bytes changed.'}
$expected0052='b8af60e34fa6652ce39961ed319d4786894fdf5ea4d714bd0e10f1cd8cff4dea'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0052_personal_security_notifications.sql')).Hash.ToLowerInvariant() -ne $expected0052){throw 'Frozen 0052 bytes changed.'}
$expected0053='74024a0bf259b619da08f00c1d9ff527d49564cc6e2f1a2fba499c29360b8d3e'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0053_memory_decision_timestamp.sql')).Hash.ToLowerInvariant() -ne $expected0053){throw 'Frozen 0053 bytes changed.'}
$project = 'usapp-memory-clock-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
$work = Join-Path $env:TEMP $project
[void](New-Item -ItemType Directory -Path $work)
$utf8 = [Text.UTF8Encoding]::new($false)
$migrationHashes = @($migrations | Sort-Object Name | ForEach-Object { $_.Name + ' ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant() })
[IO.File]::WriteAllLines((Join-Path $work 'migration-hashes-before.txt'), [string[]]$migrationHashes, $utf8)
$step = 0
$compiler = ''
$pg = ''
$ranCtest = $false
$ranApi = $false
$network = $project + '_isolated'
function D([string[]]$Arguments) {
    $script:step++
    $log = Join-Path $work ($script:step.ToString('000') + '.log')
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& docker @Arguments 2>&1 | ForEach-Object { [string]$_ }); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $old }
    [IO.File]::WriteAllLines($log, [string[]]$lines, $utf8)
    [IO.File]::WriteAllText(($log + '.exit'), [string]$code, $utf8)
    if ($code -ne 0) { $lines | Where-Object { $_ -match '\[FAIL\]|CHECK|断言|RESULT|error|Error|失败' } | Select-Object -Last 40 | Write-Output; throw "Docker step $script:step failed ($code); $log" }
    return $lines
}
function Assert-Owned([string]$Id) {
    if ($Id -notmatch '^[0-9a-f]{64}$') { throw 'Exact container ID required.' }
    # Go raw-string quotes survive Windows PowerShell 5.1 native argument passing; only the ASCII owner label is read.
    $owners = @(D @('inspect', $Id, '--format', '{{index .Config.Labels `usapp.j2b`}}'))
    if ($owners.Count -ne 1 -or $owners[0] -cne $project) { throw 'Container owner mismatch.' }
}
function Summarize([string]$Label, [string[]]$Lines) {
    $Lines | Where-Object { $_ -match '断言|RESULT|\[FAIL\]|\[CRASH\]|Subprocess aborted|SIGSEGV|^E1_LARGE|^E1_LOSS|slot held' } | ForEach-Object { Write-Output ($Label + ': ' + $_) }
}
function Apply([string]$LogName) {
    $arguments = @{ Mode = 'Docker'; Database = 'usapp_test'; User = 'postgres'; Password = $env:ISO_SUPERUSER_PASSWORD; ComposeFile = $compose; EnvFile = $envFile; InitRoles = $true }
    & (Join-Path $repo 'database/apply.ps1') @arguments *> (Join-Path $work $LogName)
    if ($LASTEXITCODE -ne 0) { throw ('Migration runner failed: ' + $LogName) }
    Assert-Owned $pg
}
function Deviations([string]$Label) {
    [void](D @('cp',(Join-Path $repo 'database/tests/task_privilege_deviation.sql'),($pg + ':/tmp/task_privilege_deviation.sql')))
    $rows = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-f','/tmp/task_privilege_deviation.sql'))
    if ($rows.Count -gt 0) { throw ('Privilege deviation rows: ' + $Label) }
    Write-Output ('PRIVILEGE DEVIATIONS (' + $Label + '): 0')
}
function Suite([string]$Name, [int]$Seconds) {
    Assert-Owned $pg
    $lines = @(D @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO','-e',('USAPP_TEST_ONLY_' + $Name + '=1'),
        $compiler,'timeout',[string]$Seconds,'/workspace/services/api/build-test/tests/usapp_test_db_gated'))
    [IO.File]::WriteAllLines((Join-Path $work ('targeted-' + $Name + '.log')), [string[]]$lines, $utf8)
    Summarize $Name $lines
    Write-Output ($Name + ' cases run: ' + @($lines | Where-Object { $_ -match '^\[ RUN  \] ' }).Count)
}
try {
    $used = (@(& docker network ls -q) | ForEach-Object { & docker network inspect $_ --format '{{range .IPAM.Config}}{{.Subnet}} {{end}}' }) -join ' '
    $octet = 20..220 | Where-Object { $used -notmatch ("(^|\s)10\.252\.$_\.0/") -and $used -notmatch '(^|\s)10\.0\.0\.0/8' -and $used -notmatch '(^|\s)10\.252\.0\.0/16' } | Select-Object -First 1
    if (-not $octet) { throw 'No free 10.252.x.0/24 subnet.' }
    [void](D @('network','create','--internal','--subnet',("10.252.$octet.0/24"),'--label',('usapp.j2b=' + $project),$network))

    $env:ISO_SUPERUSER_PASSWORD = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
    $env:ISO_API_PASSWORD = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
    $env:ISO_PUSH_PASSWORD = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
    $env:SESSION_SECRET = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
    $envFile = Join-Path $work 'isolated.env'
    [IO.File]::WriteAllText($envFile, "ISO_SUPERUSER_PASSWORD=$env:ISO_SUPERUSER_PASSWORD`nISO_API_PASSWORD=$env:ISO_API_PASSWORD`nISO_PUSH_PASSWORD=$env:ISO_PUSH_PASSWORD`n", $utf8)
    $compose = Join-Path $work 'compose.yml'
    $init = (Join-Path $repo 'deploy/docker-entrypoint-initdb.d').Replace('\','/')
    $yaml = @'
name: PROJECT
services:
  postgres:
    image: postgres:17
    pull_policy: never
    labels:
      usapp.j2b: PROJECT
    environment:
      POSTGRES_DB: usapp_test
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: ${ISO_SUPERUSER_PASSWORD:?}
      POSTGRES_API_USER: usapp_api
      POSTGRES_API_PASSWORD: ${ISO_API_PASSWORD:?}
      POSTGRES_PUSH_USER: usapp_push
      POSTGRES_PUSH_PASSWORD: ${ISO_PUSH_PASSWORD:?}
    tmpfs:
      - /var/lib/postgresql/data:rw,size=1g
    volumes:
      - type: bind
        source: 'INIT'
        target: /docker-entrypoint-initdb.d
        read_only: true
    networks: [isolated]
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres -d usapp_test"]
      interval: 1s
      timeout: 3s
      retries: 30
networks:
  isolated:
    external: true
    name: PROJECT_isolated
'@
    [IO.File]::WriteAllText($compose, $yaml.Replace('PROJECT',$project).Replace('INIT',$init), $utf8)
    [void](D @('compose','--env-file',$envFile,'-f',$compose,'up','-d','--wait','postgres'))
    $pg = @(D @('compose','--env-file',$envFile,'-f',$compose,'ps','-q','postgres'))[-1]
    $pg = @(D @('inspect',$pg,'--format','{{.Id}}'))[-1]
    Assert-Owned $pg
    if (@(D @('network','inspect',$network,'--format','{{.Internal}}'))[-1] -ne 'true') { throw 'Network must be internal.' }
    if (@(D @('inspect',$pg,'--format','{{json .HostConfig.PortBindings}}'))[-1] -notin @('null','{}')) { throw 'No ports allowed.' }
    $identity = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c','SELECT current_database()'))[-1]
    if ($identity -ne 'usapp_test') { throw 'Wrong database.' }
    Write-Output ("DISPOSABLE DATABASE: container $pg, database $identity, internal network $network")

    # 1. Apply frozen 0000-0047 plus 0048, checksums, eraser violations, privileges, repeat apply.
    Apply 'apply.log'
    $versions = @(D @('exec',$pg,'psql','-X','-At','-F','|','-U','postgres','-d','usapp_test','-c','SELECT version,checksum FROM schema_migrations ORDER BY version'))
    if ($versions.Count -ne $migrations.Count) { throw 'Migration count mismatch.' }
    foreach ($row in $versions) {
        $parts = $row.Split('|')
        $file = Join-Path $repo ('database/migrations/' + $parts[0] + '.sql')
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -ne $parts[1]) { throw 'Migration checksum mismatch.' }
    }
    $violations = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c','SELECT count(*) FROM usapp_memory_eraser_violations()'))[-1]
    if ($violations -ne '0') { throw 'Eraser-role violations.' }
    Write-Output ('CHECKSUMS MATCH: ' + $versions.Count + '; ERASER VIOLATIONS: 0')
    Deviations 'after apply'
    Apply 'repeat-apply.log'
    Deviations 'after repeat'

    # Rollback-only causal controls and the existing subscription permission/identity ledger, under both roles.
    [void](D @('cp',(Join-Path $repo 'database/tests/push_command_timestamp_test.sql'),($pg + ':/tmp/push-command-clock.sql')))
    foreach($role in @('application','maintenance')) {
        [void](D @('exec',$pg,'psql','-X','-v','expected=fixed','-v',('mode='+$role),'-U','postgres','-d','usapp_test','-f','/tmp/push-command-clock.sql'))
        Write-Output ('PUSH COMMAND CLOCK: '+$role+' passed')
    }
    [void](D @('cp',(Join-Path $repo 'database/tests/push_subscription_test.sql'),($pg + ':/tmp/push-subscription-ledger.sql')))
    foreach($ledgerRole in @('usapp_api','postgres')) {
        [void](D @('exec',$pg,'psql','-X','-U',$ledgerRole,'-d','usapp_test','-f','/tmp/push-subscription-ledger.sql'))
        Write-Output ('PUSH SUBSCRIPTION LEDGER: '+$ledgerRole+' passed')
    }

    [void](D @('cp',(Join-Path $repo 'database/tests/admin_wallet_adjustment_test.sql'),($pg+':/tmp/admin-wallet-adjustment-test.sql')))
    foreach($adjustRole in @('postgres','usapp_api')) {
     $proof=@(D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U',$adjustRole,'-d','usapp_test','-f','/tmp/admin-wallet-adjustment-test.sql'))
     $proof | Where-Object {$_ -match 'ADMIN_ADJUSTMENT_LEDGER_PASSED'} | Write-Output
    }

    foreach($ledger in @('security_notifications_test.sql','admin_redemption_cancel_test.sql','wallet_ledger_test.sql','product_ledger_test.sql','redemption_ledger_test.sql','redemption_workflow_test.sql')) {
     [void](D @('cp',(Join-Path $repo ('database/tests/'+$ledger)),($pg+':/tmp/admin-cancel-ledger.sql')))
     foreach($ledgerRole in @('postgres','usapp_api')) {
      [void](D @('exec',$pg,'timeout','120','psql','-X','-v','ON_ERROR_STOP=1','-U',$ledgerRole,'-d','usapp_test','-f','/tmp/admin-cancel-ledger.sql'))
      Write-Output ('CANCEL_COMPAT_LEDGER '+$ledger+' role='+$ledgerRole+' passed')
     }
    }

    $clock=[IO.File]::ReadAllText((Join-Path $repo 'database/tests/memory_decision_timestamp_test.sql'))
    $old=[IO.File]::ReadAllText((Join-Path $repo 'database/migrations/0036_memory_deletion.sql'))
    $start=$old.IndexOf('CREATE OR REPLACE FUNCTION usapp_memory_decision_guard()')
    $end=$old.IndexOf('$decision_guard$;',$start)+'$decision_guard$;'.Length
    if($start -lt 0 -or $end -le $start){throw 'Frozen baseline guard missing'}
    $old=$old.Substring($start,$end-$start)
    $identitySql="SELECT a.atthasdef::text||md5(pg_get_functiondef('usapp_memory_decision_guard()'::regprocedure)) FROM pg_attribute a WHERE a.attrelid='memory_edit_decisions'::regclass AND a.attname='decided_at'"
    $before=@(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',$identitySql))[-1]
    foreach($expected in @('baseline','fixed')) {
      $body=$clock
      if($expected -eq 'baseline') {
        $change=$old+"`nALTER TABLE memory_edit_decisions ALTER COLUMN decided_at SET DEFAULT pg_catalog.clock_timestamp();"
        $body=$clock.Replace("BEGIN;`nSELECT",("BEGIN;`n"+$change+"`nSELECT"))
      }
      $file=Join-Path $work ('clock-'+$expected+'.sql');[IO.File]::WriteAllText($file,$body,$utf8)
      [void](D @('cp',$file,($pg+':/tmp/memory-clock.sql')))
      foreach($role in @('application','maintenance')) {
        $proof=@(D @('exec',$pg,'psql','-X','-v',('expected='+$expected),'-v',('mode='+$role),'-U','postgres','-d','usapp_test','-f','/tmp/memory-clock.sql'))
        $proof | Where-Object {$_ -match 'MEMORY_DECISION_CLOCK'} | Write-Output
      }
    }
    $after=@(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',$identitySql))[-1]
    if($before -cne $after){throw 'Clock proof did not roll back guard/default'}
    Deviations 'after clock rollback'
    Write-Output 'MEMORY_CLOCK_ROLLBACK_PREFLIGHT_PASSED'
} finally {
 if($pg){Assert-Owned $pg;[void](D @('stop','--time','5',$pg))}
 foreach($name in @('ISO_SUPERUSER_PASSWORD','ISO_API_PASSWORD','ISO_PUSH_PASSWORD','SESSION_SECRET')){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
 Write-Output ('Owned preflight stopped and retained: '+$work)
}
