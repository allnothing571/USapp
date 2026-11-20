# Achievements 0047: inherited B3 ownership-checked disposable harness; no cloud or published ports.
# Phase 4 B3 avatar loadouts (0046_avatar_loadouts, contract 0.44.0). One disposable compiler/test container and one disposable
# PostgreSQL stack. Derived from b2-avatar-verify.ps1 (same ownership, internal network, no published port, rollback-only
# prevalidation on a base of the frozen migrations, apply, checksums, repeat apply with InitRoles, privilege deviations, targeted
# suites, the real API binary on both listeners, full CTest). Fake registry metadata only.
param([string]$Image = 'sha256:83afaf6179c2f2c80c73ee209f907773f33a67deac1610905924ed17e66cbfb1', [switch]$PrevalidateOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$expected0046 = '2b136ef9650201dc1a569e35078a7607107669d3020f8006af92f55b42f2ba69'
$expected0047 = '75d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0047_achievements.sql')).Hash.ToLowerInvariant() -ne $expected0047) { throw 'Frozen 0047 bytes changed.' }
$migrations = @(Get-ChildItem -LiteralPath (Join-Path $repo 'database/migrations') -Filter '*.sql')
if ($migrations.Count -ne 48 -or ($migrations | Sort-Object Name | Select-Object -Last 1).BaseName -ne '0047_achievements') { throw 'Unexpected migration set: this batch adds only 0047.' }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0046_avatar_loadouts.sql')).Hash.ToLowerInvariant() -ne $expected0046) { throw '0046 bytes changed.' }
$project = 'usapp-ach-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
$work = Join-Path $env:TEMP $project
[void](New-Item -ItemType Directory -Path $work)
$utf8 = [Text.UTF8Encoding]::new($false)
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
    $Lines | Where-Object { $_ -match '断言|RESULT|\[FAIL\]|\[CRASH\]|Subprocess aborted|SIGSEGV' } | ForEach-Object { Write-Output ($Label + ': ' + $_) }
}
function Apply([string]$LogName, [string]$MigrationsDir = '') {
    $arguments = @{ Mode = 'Docker'; Database = 'usapp_test'; User = 'postgres'; Password = $env:ISO_SUPERUSER_PASSWORD; ComposeFile = $compose; EnvFile = $envFile; InitRoles = $true }
    if ($MigrationsDir) { $arguments['MigrationsDir'] = $MigrationsDir }
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

    # 1. Rollback-only prevalidation: the 46 frozen migrations, then 0046 and its ledger in both roles inside one transaction that rolls back.
    $base = Join-Path $work 'base-migrations'
    [void](New-Item -ItemType Directory -Path $base)
    Get-ChildItem -LiteralPath (Join-Path $repo 'database/migrations') -Filter '*.sql' | Where-Object { $_.BaseName -lt '0047' } | Copy-Item -Destination $base
    Apply 'base-apply.log' $base
    $migrationFile = Join-Path $repo 'database/migrations/0047_achievements.sql'
    $migration = [IO.File]::ReadAllText($migrationFile)
    $ledger = [IO.File]::ReadAllText((Join-Path $repo 'database/tests/achievement_test.sql'))
    $migration = [regex]::Replace($migration,'(?m)^BEGIN;\r?$','')
    $migration = [regex]::Replace($migration,'(?m)^COMMIT;\r?$','')
    $ledger = [regex]::Replace($ledger,'(?m)^(BEGIN;|ROLLBACK;|\\set[^\r\n]*)\r?$','')
    $pre = Join-Path $work 'prevalidation.sql'
    [IO.File]::WriteAllText($pre, "BEGIN;`n" + $migration + "`n" + $ledger + "`nSET LOCAL ROLE usapp_api;`n" + $ledger + "`nRESET ROLE;`nROLLBACK;`n", $utf8)
    [void](D @('cp',$pre,($pg + ':/tmp/b3-prevalidation.sql')))
    try {
        $preLines = @(D @('exec',$pg,'timeout','60','psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/b3-prevalidation.sql'))
        $preLines | Where-Object { $_ -match 'ASSERTIONS PASSED' } | Write-Output
    } finally {
        $absent = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
            "SELECT (to_regclass('public.achievement_activation') IS NULL AND to_regprocedure('public.usapp_achievement_facts(timestamptz[],timestamptz)') IS NULL)::text"))[-1]
        if ($absent -ne 'true') { throw 'Prevalidation persisted 0046 schema.' }
    }
    Write-Output 'ROLLBACK PREVALIDATION PASSED: owner and application roles; 0047 objects absent afterwards'
    if ($PrevalidateOnly) { Write-Output 'PREVALIDATION_ONLY_COMPLETE'; return }

    # 2. Apply 0046 through the runner, then checksums of all 47, eraser violations and privileges.
    Write-Output ('PINNED APPLY SHA256 0047: ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $migrationFile).Hash.ToLowerInvariant())
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

    # 3. SQL ledgers: the new one, the wallet ledger (untouched by B1) and the latest guard ledgers, both roles; each rolls back.
    foreach ($ledgerName in @('achievement_test.sql','avatar_loadout_test.sql','avatar_purchase_test.sql','avatar_catalog_test.sql','wallet_ledger_test.sql','task_ledger_test.sql',
        'product_ledger_test.sql','redemption_ledger_test.sql','redemption_workflow_test.sql','notification_choice_test.sql')) {
        Assert-Owned $pg
        [void](D @('cp',(Join-Path $repo ('database/tests/' + $ledgerName)),($pg + ':/tmp/n3-ledger.sql')))
        foreach ($role in @('postgres','usapp_api')) {
            $out = @(D @('exec',$pg,'timeout','30','psql','-X','-v','ON_ERROR_STOP=1','-U',$role,'-d','usapp_test','-f','/tmp/n3-ledger.sql'))
            if (-not ($out | Where-Object { $_ -eq 'ROLLBACK' })) { throw ('Ledger did not roll back: ' + $ledgerName + ' as ' + $role) }
        }
        Write-Output ('SQL LEDGER PASSED BOTH ROLES: ' + $ledgerName)
    }
    foreach ($modeLedger in @('timestamp_guard_test.sql','push_dispatch_test.sql')) {
        [void](D @('cp',(Join-Path $repo ('database/tests/' + $modeLedger)),($pg + ':/tmp/n3-mode-ledger.sql')))
        foreach ($mode in @('maintenance','application')) {
            $out = @(D @('exec',$pg,'timeout','60','psql','-X','-v','ON_ERROR_STOP=1','-v',('mode=' + $mode),'-U','postgres','-d','usapp_test','-f','/tmp/n3-mode-ledger.sql'))
            if (-not ($out | Where-Object { $_ -eq 'ROLLBACK' })) { throw ('Ledger did not roll back: ' + $modeLedger + ' ' + $mode) }
        }
        Write-Output ('SQL LEDGER PASSED BOTH MODES: ' + $modeLedger)
    }
    $leftover = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        'SELECT (SELECT count(*) FROM avatar_assets) + (SELECT count(*) FROM avatar_items) + (SELECT count(*) FROM avatar_starter_configs) + (SELECT count(*) FROM avatar_purchases) + (SELECT count(*) FROM avatar_loadout_events) + (SELECT count(*) FROM wallet_operations) + (SELECT count(*) FROM notifications)'))[-1]
    if ($leftover -ne '0') { throw 'A ledger committed rows.' }

    # 4. Repeat migration and InitRoles: idempotent, still no deviations.
    Apply 'repeat-apply.log'
    $again = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c','SELECT count(*) FROM schema_migrations'))[-1]
    if ($again -ne [string]$migrations.Count) { throw 'Repeat apply changed the migration count.' }
    Deviations 'after repeat'

    # 5. Build, then targeted suites, the real API binary and full CTest.
    $compiler = @(D @('run','-d','--pull','never','--name',($project + '-compiler'),'--label',('usapp.j2b=' + $project),
        '--network',$network,'--entrypoint','sleep',$Image,'1800'))[-1]
    Assert-Owned $compiler
    if (@(D @('inspect',$compiler,'--format','{{json .HostConfig.PortBindings}}'))[-1] -notin @('null','{}')) { throw 'No ports allowed.' }
    # Every source the image predates (the capability set), plus this batch's files and what the static checks read.
    foreach ($directory in @('services/api/src','services/api/tests','contracts','database','docs','services/push-worker/src','services/push-worker/tests')) {
        [void](D @('cp',(Join-Path $repo ($directory + '/.')),($compiler + ':/workspace/' + $directory)))
    }
    [void](D @('cp',(Join-Path $repo 'services/api/CMakeLists.txt'),($compiler + ':/workspace/services/api/CMakeLists.txt')))
    [void](D @('exec',$compiler,'timeout','400','cmake','--build','/workspace/services/api/build-test','-j','2'))
    Write-Output 'BUILD PASSED'

    $env:USAPP_TEST_DSN = 'postgresql://usapp_api:' + $env:ISO_API_PASSWORD + '@postgres:5432/usapp_test'
    $env:USAPP_TEST_MAINT_CONNINFO = 'host=postgres port=5432 dbname=usapp_test user=postgres password=' + $env:ISO_SUPERUSER_PASSWORD
    # Targeted first, on a database no suite has used: the new avatar target (its incomplete-configuration case needs that).
    foreach ($suite in 'ACHIEVEMENT','AVATAR_CATALOG','AVATAR_PURCHASE','AVATAR_LOADOUT') {
        Assert-Owned $pg
        $targeted = @(D @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO','-e',('USAPP_TEST_ONLY_' + $suite + '=1'),
            $compiler,'timeout','120','/workspace/services/api/build-test/tests/usapp_test_db_gated'))
        [IO.File]::WriteAllLines((Join-Path $work ('targeted-' + $suite + '.log')), [string[]]$targeted, $utf8)
        Summarize $suite $targeted
        Write-Output ($suite + ' cases run: ' + @($targeted | Where-Object { $_ -match '^\[ RUN  \] (Avatar B[123]|Achievements:)' }).Count)
        if ($suite -eq 'AVATAR_CATALOG' -and -not ($targeted | Where-Object { $_ -eq 'AVATAR_INCOMPLETE_CASE covered' })) { throw 'The incomplete-configuration case did not run on a fresh database.' }
    }

    # The real API binary in the compiler container, on its loopback only; bounded by its own timeout.
    $env:POSTGRES_PASSWORD = $env:ISO_API_PASSWORD
    Assert-Owned $compiler
    $ranApi = $true
    [void](D @('exec','-d','-e','POSTGRES_PASSWORD','-e','SESSION_SECRET','-e','POSTGRES_HOST=postgres','-e','POSTGRES_DB=usapp_test',
        '-e','POSTGRES_USER=usapp_api','-e','API_PUBLIC_PORT=8080','-e','REGISTRATION_MODE=open','-e','REMINDERS_ENABLED=false','-e','SCHEDULER_ENABLED=false',
        $compiler,'sh','-c','timeout 120 /workspace/services/api/build-test/usapp_api > /tmp/ph-api.log 2>&1 & echo $! > /tmp/ph-api.pid; wait'))
    $live = @(D @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO','-e','USAPP_TEST_REMINDER_API_URL=http://127.0.0.1:8080',
        '-e','USAPP_TEST_ADMIN_API_URL=http://127.0.0.1:8081','-e','USAPP_TEST_EXPECT_GENERATION=false',
        $compiler,'timeout','60','/workspace/services/api/build-test/tests/usapp_test_db_gated'))
    [IO.File]::WriteAllLines((Join-Path $work 'live-api.log'), [string[]]$live, $utf8)
    Summarize 'LIVE API' $live
    # By its recorded PID; no double quotes in native arguments (Windows PowerShell 5.1).
    [void](D @('exec',$compiler,'sh','-c','kill -TERM $(cat /tmp/ph-api.pid) 2>/dev/null; true'))
    Remove-Item Env:POSTGRES_PASSWORD

    Write-Output 'TARGETED AND LIVE PASSED; RUNNING FULL CTEST'
    Assert-Owned $pg
    $ranCtest = $true
    # A harness bound over the whole run; every CTest target keeps its own 30 s limit.
    $full = @(D @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO',$compiler,'timeout','300','ctest',
        '--test-dir','/workspace/services/api/build-test','--output-on-failure'))
    [IO.File]::WriteAllLines((Join-Path $work 'ctest.log'), [string[]]$full, $utf8)
    $full | Where-Object { $_ -match 'Test +#\d+|tests passed|tests failed|Total Test time' } | Write-Output
    $invalid = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM memory_purges p JOIN memory_deletions d ON d.id=p.deletion_id WHERE p.purged_at < d.purge_after OR EXISTS (SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id=d.id)"))[-1]
    if ($invalid -ne '0') { throw 'Early or reversed-cycle text erasure.' }
    $wrongRemovals = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM media_late_objects l WHERE l.disposition='removed' AND usapp_media_key_disposition(l.object_key) IN ('keep','live')"))[-1]
    if ($wrongRemovals -ne '0') { throw 'A removal was recorded for a key that is keep or live.' }
    $avatarWallet = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM wallet_operations WHERE source_type LIKE 'avatar%' AND (operation_type <> 'AVATAR_PURCHASE' OR source_type <> 'avatar_purchase')"))[-1]
    if ($avatarWallet -ne '0') { throw 'An avatar wallet operation is not a purchase.' }
    $unaudited = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM avatar_item_revisions r WHERE NOT EXISTS (SELECT 1 FROM audit_logs a WHERE a.actor_type = 'admin' AND a.actor_id = r.admin_user_id AND a.target_id = r.item_id::text AND a.action IN ('avatar.item_created','avatar.item_revised') AND (a.detail->>'version')::int = r.version)"))[-1]
    if ($unaudited -ne '0') { throw 'A catalog revision has no audit row.' }
    $badPurchases = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM avatar_purchases p WHERE (SELECT count(*) FROM wallet_ledger_entries e WHERE e.operation_id = p.operation_id) <> 1 OR NOT EXISTS (SELECT 1 FROM wallet_ledger_entries e WHERE e.operation_id = p.operation_id AND e.asset_type = 'sunshine' AND e.user_id = p.user_id AND e.amount = -p.price_sunshine) OR NOT EXISTS (SELECT 1 FROM audit_logs a WHERE a.action = 'avatar.purchased' AND a.target_id = p.id::text) OR EXISTS (SELECT 1 FROM avatar_items i WHERE i.id = p.item_id AND i.starter)"))[-1]
    if ($badPurchases -ne '0') { throw 'A purchase lacks its exact debit or audit row, or names a starter.' }
    $chain = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM (SELECT user_id, asset_type, seq, balance_after, amount, lag(balance_after) OVER w AS prev, lag(seq) OVER w AS pseq FROM wallet_ledger_entries WINDOW w AS (PARTITION BY user_id, asset_type ORDER BY seq)) x WHERE balance_after <> COALESCE(prev, 0) + amount OR seq <> COALESCE(pseq, 0) + 1 OR balance_after < 0"))[-1]
    if ($chain -ne '0') { throw 'A ledger chain is broken.' }
    Write-Output 'AVATAR PURCHASES WITHOUT EXACT DEBIT/AUDIT=0; LEDGER CHAIN BREAKS=0'
    $badLoadouts = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM avatar_loadout_events e WHERE NOT usapp_avatar_equippable(e.user_id, e.hair_item_id, 'hair') OR NOT usapp_avatar_equippable(e.user_id, e.clothes_item_id, 'clothes') OR NOT usapp_avatar_equippable(e.user_id, e.expression_item_id, 'expression') OR (e.hat_item_id IS NOT NULL AND NOT usapp_avatar_equippable(e.user_id, e.hat_item_id, 'hat')) OR (e.handheld_item_id IS NOT NULL AND NOT usapp_avatar_equippable(e.user_id, e.handheld_item_id, 'handheld')) OR (e.accessory_item_id IS NOT NULL AND NOT usapp_avatar_equippable(e.user_id, e.accessory_item_id, 'accessory')) OR NOT EXISTS (SELECT 1 FROM audit_logs a WHERE a.action = 'avatar.loadout_saved' AND a.target_id = e.id::text) OR EXISTS (SELECT 1 FROM avatar_loadout_events x WHERE x.user_id = e.user_id AND x.version < e.version GROUP BY x.user_id HAVING count(*) <> e.version - 1)"))[-1]
    if ($badLoadouts -ne '0') { throw 'A saved loadout is ineligible, unaudited or out of sequence.' }
    Write-Output 'INELIGIBLE/UNAUDITED/GAPPED LOADOUTS=0'
    $planNotices = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM notification_intents i JOIN shared_plan_memory_links l ON l.memory_id = i.memory_id AND l.mode = 'create'"))[-1]
    if ($planNotices -ne '0') { throw 'A plan-created memory has a notification intent.' }
    $perPhoto = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM (SELECT album_batch_id FROM notifications WHERE album_batch_id IS NOT NULL GROUP BY album_batch_id HAVING count(*) > 1) x"))[-1]
    if ($perPhoto -ne '0') { throw 'A batch has more than one notice.' }
    $remembered = @(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c',
        "SELECT count(*) FROM notification_intents WHERE operation IN ('memory_create','shared_plan_create','album_photos_add') AND preference_seq IS NOT NULL"))[-1]
    if ($remembered -ne '0') { throw 'A D20 choice was remembered.' }
    Write-Output 'EARLY/REVERSED ERASURES=0; KEEP/LIVE REMOVALS=0; PLAN-CONVERSION INTENTS=0; MULTI-NOTICE BATCHES=0; REMEMBERED D20 CHOICES=0'
    Deviations 'after CTest'
    Write-Output ('ACHIEVEMENT_PASSED: ' + $project + '; logs=' + $work)
} finally {
    if ($compiler -and $ranCtest) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & docker cp ($compiler + ':/workspace/services/api/build-test/Testing/Temporary/LastTest.log') (Join-Path $work 'LastTest.log') 2>&1 | Out-Null
        $ErrorActionPreference = $old
    }
    if ($compiler -and $ranApi) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & docker cp ($compiler + ':/tmp/ph-api.log') (Join-Path $work 'api-server.log') 2>&1 | Out-Null
        $ErrorActionPreference = $old
    }
    if ($pg) {
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $serverLog = @(& docker logs $pg 2>&1 | ForEach-Object { [string]$_ })
        [IO.File]::WriteAllLines((Join-Path $work 'postgres-server.log'), [string[]]$serverLog, $utf8)
        $ErrorActionPreference = $old
    }
    foreach ($id in @($compiler,$pg)) {
        if ($id) {
            Assert-Owned $id
            [void](D @('stop','--time','5',$id))
        }
    }
    foreach ($name in @('ISO_SUPERUSER_PASSWORD','ISO_API_PASSWORD','ISO_PUSH_PASSWORD','SESSION_SECRET','POSTGRES_PASSWORD','USAPP_TEST_DSN','USAPP_TEST_MAINT_CONNINFO')) {
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    Write-Output ('Created containers stopped and retained. Work folder: ' + $work)
}
