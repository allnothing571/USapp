# Owned disposable signed/encrypted database backup and independent restore probe; V1 or explicit V2 rehearsal.
# Synthetic source and fresh restore target only; application CONNECT remains denied.
param(
 [string]$Image = 'sha256:83afaf6179c2f2c80c73ee209f907773f33a67deac1610905924ed17e66cbfb1',
 [switch]$JournalDrill,
 [switch]$UpgradeFrom0024,
 [string]$RollbackImage,
 [switch]$SharedRoomUpgrade
)
$ErrorActionPreference = 'Stop'
if ($SharedRoomUpgrade -and ($UpgradeFrom0024 -or $RollbackImage -or $JournalDrill)) {
    throw 'Run -SharedRoomUpgrade separately.'
}
if ($RollbackImage -and !$UpgradeFrom0024) { throw '-RollbackImage requires -UpgradeFrom0024.' }
if ($RollbackImage -and $JournalDrill) { throw 'Run release pairing and journal recovery separately.' }
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$expected0047 = '75d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo 'database/migrations/0047_achievements.sql')).Hash.ToLowerInvariant() -ne $expected0047) { throw 'Frozen 0047 bytes changed.' }
$migrations = @(Get-ChildItem -LiteralPath (Join-Path $repo 'database/migrations') -Filter '*.sql')
$latestJournalDrill = $JournalDrill -and $migrations.Count -eq 63
$expectedCount = if ($SharedRoomUpgrade -or $latestJournalDrill) { 63 } else { 54 }
$expectedLast = if ($SharedRoomUpgrade -or $latestJournalDrill) { '0062_avatar_layer_refresh' } else { '0053_memory_decision_timestamp' }
if ($migrations.Count -ne $expectedCount -or ($migrations | Sort-Object Name | Select-Object -Last 1).BaseName -cne $expectedLast) { throw 'Unexpected migration set.' }
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
$project = 'usapp-backup-probe-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
$work = Join-Path $env:TEMP $project
[void](New-Item -ItemType Directory -Path $work)
$utf8 = [Text.UTF8Encoding]::new($false)
$migrationHashes = @($migrations | Sort-Object Name | ForEach-Object { $_.Name + ' ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant() })
[IO.File]::WriteAllLines((Join-Path $work 'migration-hashes-before.txt'), [string[]]$migrationHashes, $utf8)
$step = 0
$compiler = ''
$releaseApis = @()
$restorePg = ''
$keyFiles=@();$keys=''
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
function Apply([string]$LogName,[string]$MigrationDirectory='', [string]$DatabaseRoot=(Join-Path $repo 'database')) {
    $arguments = @{ Mode = 'Docker'; Database = 'usapp_test'; User = 'postgres'; Password = $env:ISO_SUPERUSER_PASSWORD; ComposeFile = $compose; EnvFile = $envFile }
    if($MigrationDirectory){$arguments.MigrationsDir=$MigrationDirectory}else{$arguments.InitRoles=$true}
    & (Join-Path $DatabaseRoot 'apply.ps1') @arguments *> (Join-Path $work $LogName)
    if ($LASTEXITCODE -ne 0) { throw ('Migration runner failed: ' + $LogName) }
    Assert-Owned $pg
}
function Test-SharedRoomUpgrade {
    $commit = '15d94a9032d2bd4965399b26c2119ae29219d8bf'
    $ref = & git rev-parse --verify 'v1-candidate-20261120^{commit}'
    if ($LASTEXITCODE -ne 0 -or $ref -cne $commit) { throw 'Frozen V1 tag is missing or has moved.' }
    $archive = Join-Path $work 'v1.zip'
    & git archive --format=zip ("--output="+$archive) $commit database services/api
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the frozen V1 source.' }
    $v1 = Join-Path $work 'v1'
    Expand-Archive -LiteralPath $archive -DestinationPath $v1
    $v1Database = Join-Path $v1 'database'
    $v1Files = @(Get-ChildItem -LiteralPath (Join-Path $v1Database 'migrations') -Filter '*.sql' | Sort-Object Name)
    if ($v1Files.Count -ne 54) { throw 'Expected exactly 54 frozen V1 migrations.' }
    $v1Hashes = @($v1Files | ForEach-Object { $_.Name + ' ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant() })
    if (($v1Hashes -join ',') -cne (($migrationHashes | Select-Object -First 54) -join ',')) { throw 'V2 changed frozen V1 migrations.' }

    function Check-MigrationLedger([string[]]$Hashes) {
        Assert-Owned $pg
        $rows = @(D @('exec',$pg,'psql','-X','-qAt','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test',
            '-c','SELECT version,checksum FROM schema_migrations ORDER BY version'))
        $expected = @($Hashes | ForEach-Object { $_.Replace('.sql ','|') })
        if (($rows -join ',') -cne ($expected -join ',')) { throw 'Installed migration checksums differ.' }
    }
    function Build-ReleaseApi([string]$Name,[string]$SourceRoot) {
        $source = '/tmp/' + $Name
        $build = $source + '-build'
        [void](D @('exec',$compiler,'mkdir','-p',$source))
        [void](D @('cp',(Join-Path $SourceRoot 'services/api/src'),($compiler+':'+$source+'/src')))
        [void](D @('cp',(Join-Path $SourceRoot 'services/api/CMakeLists.txt'),($compiler+':'+$source+'/CMakeLists.txt')))
        [void](D @('exec',$compiler,'cmake','-S',$source,'-B',$build,'-DUSAPP_BUILD_TESTS=OFF'))
        [void](D @('exec',$compiler,'timeout','900','cmake','--build',$build,'--target','usapp_api','-j','2'))
        $binary = Join-Path $work ($Name + '-usapp_api')
        [void](D @('cp',($compiler+':'+$build+'/usapp_api'),$binary))
        Write-Host ("BUILD_$Name SHA256 " + (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant())
        return $binary
    }
    $script:compiler = @(D @('run','-d','--pull','never','--label',('usapp.j2b='+$project),
        '--network','none','--entrypoint','sleep',$Image,'2400'))[-1]
    Assert-Owned $compiler
    $v1Binary = Build-ReleaseApi 'V1' $v1
    $v2Binary = Build-ReleaseApi 'V2' $repo

    Apply 'v1-apply.log' -DatabaseRoot $v1Database
    Check-MigrationLedger $v1Hashes
    [void](D @('cp',(Join-Path $repo 'database/tests/upgrade_0024_fixture.sql'),($pg+':/tmp/v1-fixture.sql')))
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/v1-fixture.sql'))
    $fingerprint = LegacyFingerprint $pg 'usapp_test'
    [void](D @('cp',(Join-Path $repo 'database/tests/shared_room_upgrade_sessions.sql'),($pg+':/tmp/v1-sessions.sql')))
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/v1-sessions.sql'))

    function Test-ReleaseStage([string]$Stage,[string]$Binary) {
        $settings = @('-e',('POSTGRES_PASSWORD='+$env:ISO_API_PASSWORD),'-e','SESSION_SECRET','-e','POSTGRES_HOST=postgres',
            '-e','POSTGRES_DB=usapp_test','-e','POSTGRES_USER=usapp_api','-e','API_PUBLIC_PORT=8080','-e','API_PUBLIC_BIND=127.0.0.1',
            '-e','PUBLIC_ORIGIN=https://localhost:8443','-e','REGISTRATION_MODE=open','-e','MAIL_MODE=log','-e','MEDIA_ENABLED=false',
            '-e','MEMORY_PURGE_ENABLED=false','-e','SCHEDULER_ENABLED=false','-e','REMINDERS_ENABLED=false')
        $id = @(D (@('run','-d','--pull','never','--name',($project+'-'+$Stage),
            '--label',('usapp.j2b='+$project),'--network',$network) + $settings +
            @('--mount',('type=bind,source='+$Binary+',target=/release-api,readonly'),
                '--entrypoint','sh',$Image,'-c','cp /release-api /tmp/release-api && chmod 500 /tmp/release-api && exec /tmp/release-api')))[-1]
        $script:releaseApis += $id
        Assert-Owned $id
        try {
            $probeDir = Join-Path $repo 'tools/probes'
            D @('run','--rm','--pull','never','--label',('usapp.j2b='+$project),'--network',('container:'+$id),
                '--mount',('type=bind,source='+$probeDir+',target=/probes,readonly'),
                'python:3.12-slim-bookworm','python','-I','/probes/shared-room-upgrade-http.py',$Stage)
        } catch {
            D @('logs','--tail','20',$id) | Write-Host
            throw
        } finally { [void](D @('stop','--time','5',$id)) }
    }
    Test-ReleaseStage 'v1-baseline' $v1Binary
    Apply 'v2-upgrade.log'
    Check-MigrationLedger $migrationHashes
    Apply 'v2-repeat.log'
    Check-MigrationLedger $migrationHashes
    Deviations 'after V2 upgrade and repeat'
    [void](D @('cp',(Join-Path $repo 'database/tests/room_character_positions_test.sql'),($pg+':/tmp/position-tests.sql')))
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/position-tests.sql'))
    Test-ReleaseStage 'v2-upgraded' $v2Binary
    Test-ReleaseStage 'v1-rollback' $v1Binary
    Check-MigrationLedger $migrationHashes
    Test-ReleaseStage 'v2-resumed' $v2Binary
    Check-MigrationLedger $migrationHashes
    if ((LegacyFingerprint $pg 'usapp_test') -cne $fingerprint) { throw 'V1 fixture was changed by the upgrade or application rollback.' }
    Write-Output "SHARED_ROOM_RELEASE_PAIR_PASSED migrations=54->$expectedCount->$expectedCount legacy_fixture=preserved"
}
function Test-ReleasePair {
    $oldImageId = @(D @('image','inspect',$RollbackImage,'--format','{{.Id}}'))[-1]
    if ($oldImageId -notmatch '^sha256:[0-9a-f]{64}$') { throw 'Invalid rollback image identity.' }
    Write-Output ('ROLLBACK_IMAGE '+$oldImageId)
    $script:compiler = @(D @('run','-d','--pull','never','--name',($project+'-compiler'),
        '--label',('usapp.j2b='+$project),'--network',$network,'--entrypoint','sleep',$Image,'2400'))[-1]
    Assert-Owned $compiler
    [void](D @('cp',(Join-Path $repo 'services/api/src/.'),($compiler+':/workspace/services/api/src')))
    [void](D @('cp',(Join-Path $repo 'services/api/CMakeLists.txt'),($compiler+':/workspace/services/api/CMakeLists.txt')))
    [void](D @('exec',$compiler,'timeout','900','cmake','--build','/workspace/services/api/build-test','--target','usapp_api','-j','2'))
    $candidate = '/workspace/services/api/build-test/usapp_api'
    D @('exec',$compiler,'sha256sum',$candidate) | ForEach-Object { Write-Output ('CANDIDATE '+$_) }
    $candidateFile = Join-Path $work 'usapp_api'
    [void](D @('cp',($compiler+':'+$candidate),$candidateFile))
    $token = '98' * 32
    $sql = "INSERT INTO sessions(token_hash,csrf_token_hash,user_id,is_authenticated,expires_at) SELECT encode(sha256(convert_to('$token','UTF8')),'hex'),repeat('3',64),id,true,now()+interval '1 day' FROM users WHERE email_canonical='upgrade0024-b@example.com'"
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-c',$sql))
    $settings = @('-e',('POSTGRES_PASSWORD='+$env:ISO_API_PASSWORD),'-e','SESSION_SECRET','-e','POSTGRES_HOST=postgres',
        '-e','POSTGRES_DB=usapp_test','-e','POSTGRES_USER=usapp_api','-e','API_PUBLIC_PORT=8080','-e','API_PUBLIC_BIND=127.0.0.1',
        '-e','REGISTRATION_MODE=open','-e','MAIL_MODE=log',
        '-e','MEDIA_ENABLED=false','-e','MEMORY_PURGE_ENABLED=false','-e','SCHEDULER_ENABLED=false',
        '-e','REMINDERS_ENABLED=false')
    function CheckApi([string]$Id,[string]$Stage) {
        Assert-Owned $Id
        $probe = Join-Path $repo 'tools/probes/upgrade-0024-http.py'
        [void](D @('run','--rm','--pull','never','--label',('usapp.j2b='+$project),'--network',('container:'+$Id),
            '--mount',('type=bind,source='+$probe+',target=/probe.py,readonly'),
            'python:3.12-slim-bookworm','python','-I','/probe.py',$token))
        Write-Output ('RELEASE_PAIR_PASSED '+$Stage)
    }
    function CheckStage([string]$Stage,[string[]]$Runtime) {
        $id = @(D (@('run','-d','--pull','never','--name',($project+'-'+$Stage),
            '--label',('usapp.j2b='+$project),'--network',$network)+$settings+$Runtime))[-1]
        $script:releaseApis += $id
        Assert-Owned $id
        try { CheckApi $id $Stage }
        catch {
            D @('logs','--tail','20',$id) | Write-Host
            throw
        }
        finally { [void](D @('stop','--time','5',$id)) }
    }
    $candidateRuntime = @('--mount',('type=bind,source='+$candidateFile+',target=/release-api,readonly'),
        '--entrypoint','sh',$Image,'-c','cp /release-api /tmp/release-api && chmod 500 /tmp/release-api && exec /tmp/release-api')
    $beforePair = LegacyFingerprint $pg 'usapp_test'
    CheckStage 'candidate' $candidateRuntime
    CheckStage 'rollback' @($oldImageId)
    CheckStage 'candidate-again' $candidateRuntime
    if ((LegacyFingerprint $pg 'usapp_test') -cne $beforePair) { throw 'Legacy fixture changed during the release pairing.' }
}
function LegacyFingerprint([string]$Id,[string]$Database) {
    Assert-Owned $Id
    [void](D @('cp',(Join-Path $repo 'database/tests/upgrade_0024_proof.sql'),($Id+':/tmp/upgrade-0024-proof.sql')))
    $rows=@(D @('exec',$Id,'psql','-X','-qAt','-v','ON_ERROR_STOP=1','-U','postgres','-d',$Database,'-f','/tmp/upgrade-0024-proof.sql'))
    if($rows.Count -ne 1 -or $rows[0] -cnotmatch '^[a-f0-9]{32}$'){throw 'Expected exactly one complete legacy fixture.'}
    return $rows[0]
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
function JointInventory([string]$Id,[string]$Database) {
    Assert-Owned $Id
    [void](D @('cp',(Join-Path $repo 'database/tests/joint_restore_inventory.sql'),($Id+':/tmp/joint-restore-inventory.sql')))
    return D @('exec',$Id,'psql','-X','-At','-F','|','-v','ON_ERROR_STOP=1','-U','postgres','-d',$Database,'-f','/tmp/joint-restore-inventory.sql')
}
function NewJointObjectBackup {
    [void](D @('cp',(Join-Path $repo 'database/tests/joint_restore_fixture.sql'),($pg+':/tmp/joint-restore-fixture.sql')))
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/joint-restore-fixture.sql'))
    $source=Join-Path $work 'synthetic-object-backup'
    [void](New-Item -ItemType Directory -Path $source)
    $rows=@(JointInventory $pg 'usapp_test')
    if($rows.Count -ne 2){throw 'Expected two source media records.'}
    $manifest=@(for($i=0;$i -lt $rows.Count;$i++){
        $fields=$rows[$i].Split('|')
        $id='00000000-0000-0000-0000-00000000001'+($i+1)
        if($fields.Count -ne 5 -or $fields[0] -cne $id -or $fields[3] -cne '4' -or $fields[4] -cne 'f'){throw 'Unexpected source inventory.'}
        $path=Join-Path $source ($id+'.bin')
        [IO.File]::WriteAllBytes($path,[byte[]]@(($i+1),2,3,4))
        [pscustomobject]@{inventory=$rows[$i].Substring(0,$rows[$i].Length-2); sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash}
    })
    [IO.File]::WriteAllText((Join-Path $source 'manifest.json'),(ConvertTo-Json -InputObject $manifest -Compress),$utf8)
}
function ConfirmJointObjectRestorePreparation {
    Assert-Owned $restorePg
    $query = @'
WITH latest AS (
    SELECT generation, journal_id FROM public.deletion_recovery_attempts ORDER BY generation DESC LIMIT 1
)
SELECT public.usapp_recovery_closed()
   AND NOT has_database_privilege('usapp_api',current_database(),'CONNECT')
   AND NOT has_database_privilege('usapp_push',current_database(),'CONNECT')
   AND EXISTS (
       SELECT 1 FROM latest a
       JOIN public.deletion_recovery_manifests m USING (generation)
       JOIN public.deletion_recovery_verifications v USING (generation)
       WHERE m.payload->'head'->>'journal_id' = a.journal_id::text
         AND m.payload->'head'->>'hash' = v.head_hash
         AND m.payload->'head'->>'seq' = v.head_seq::text
   )
'@
    $observed = @(D @('exec',$restorePg,'psql','-X','-At','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_restore_probe','-c',$query))
    return $observed.Count -eq 1 -and $observed[0] -ceq 't'
}

function InvokeJointRestore([string]$ArchiveSha256) {
    if (ConfirmJointObjectRestorePreparation) { throw 'Object restore should require a verified journal checkpoint.' }
    Write-Output 'JOINT_RESTORE_UNVERIFIED_CHECKPOINT_REFUSED'
    $source=Join-Path $work 'synthetic-object-backup'
    $manifest=@(Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json)
    $prior=@(JointInventory $restorePg 'usapp_restore_probe')
    if($manifest.Count -ne 2 -or $prior.Count -ne 2){throw 'Restored pre-deletion inventory missing.'}
    for($i=0;$i -lt 2;$i++){
        if($prior[$i] -cne ($manifest[$i].inventory+'|f')){throw 'Restored pre-deletion inventory differs from backup.'}
    }
    $script:compiler=@(D @('run','-d','--pull','never','--name',($project+'-compiler'),'--label',('usapp.j2b='+$project),
        '--network',$network,'--entrypoint','sleep',$Image,'2400'))[-1]
    Assert-Owned $compiler
    foreach($directory in @('services/api/src','services/api/tests')){
        [void](D @('cp',(Join-Path $repo ($directory+'/.')),($compiler+':/workspace/'+$directory)))
    }
    [void](D @('cp',(Join-Path $repo 'services/api/CMakeLists.txt'),($compiler+':/workspace/services/api/CMakeLists.txt')))
    [void](D @('exec',$compiler,'timeout','600','cmake','--build','/workspace/services/api/build-test','--target',
        'usapp_test_journal_service_interop','usapp_test_unit','-j','2'))
    $unit = @(D @('exec',$compiler,'ctest','--test-dir','/workspace/services/api/build-test','-R','^unit$','--output-on-failure'))
    $unit | Where-Object { $_ -match 'Tests passed|tests passed' } | Write-Output
    $env:USAPP_TEST_MAINT_CONNINFO='host=restore port=5432 dbname=usapp_restore_probe user=postgres password='+$env:ISO_SUPERUSER_PASSWORD
    try{
        & (Join-Path $repo 'tools/journal-service-verify.ps1') -Compiler $compiler -Project $project -Work $work -ClosedPostgres $restorePg -Restored -ArchiveSha256 $ArchiveSha256
    }finally{
        Remove-Item Env:USAPP_TEST_MAINT_CONNINFO
    }
    if (!(ConfirmJointObjectRestorePreparation)) { throw 'Object restore requires a verified closed target.' }
    $after=@(JointInventory $restorePg 'usapp_restore_probe')
    if($after.Count -ne 2 -or $after[0] -cne ($manifest[0].inventory+'|t') -or
       $after[1] -cne ($manifest[1].inventory+'|f')){throw 'Journal quarantine must exclude exactly the deleted memory object.'}
    $retained='00000000-0000-0000-0000-000000000012.bin'
    $withheld='00000000-0000-0000-0000-000000000011.bin'
    foreach($i in 0..1){
        $name=@($withheld,$retained)[$i]
        if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $source $name)).Hash -cne $manifest[$i].sha256){throw 'Synthetic object backup integrity mismatch.'}
    }
    $output=Join-Path $work 'synthetic-restored-objects'
    [void](New-Item -ItemType Directory -Path $output)
    foreach($row in $after){
        $fields=$row.Split('|')
        if($fields[4] -ceq 'f'){
            $name=$fields[0]+'.bin'
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $output $name)
        }
    }
    if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $output $retained)).Hash -cne $manifest[1].sha256 -or
       @(Get-ChildItem -LiteralPath $output -File).Count -ne 1 -or (Test-Path -LiteralPath (Join-Path $output $withheld))){throw 'Object restore output mismatch.'}
    if (!(ConfirmJointObjectRestorePreparation)) { throw 'Restore target preparation or serving gate changed.' }
    Write-Output 'JOINT_RESTORE_PASSED; pre-deletion media=2; copied=1; withheld=1; serving_allowed=false'
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

    if ($SharedRoomUpgrade) {
        Test-SharedRoomUpgrade
        return
    }

    if($UpgradeFrom0024){
        $baseline=Join-Path $work 'baseline-migrations'
        [void](New-Item -ItemType Directory -Path $baseline)
        $prefix=@($migrations | Where-Object {[int]$_.Name.Substring(0,4) -le 24} | Sort-Object Name)
        if($prefix.Count -ne 25 -or $prefix[-1].BaseName -cne '0024_push_dispatch_repairs'){throw 'Unexpected upgrade baseline.'}
        foreach($file in $prefix){Copy-Item -LiteralPath $file.FullName -Destination $baseline}
        # Target-specific role hardening runs after the complete upgrade.
        Apply 'baseline-0024.log' $baseline
        $before=@(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c','SELECT version,checksum FROM schema_migrations ORDER BY version'))
        $expected=@($migrationHashes | Select-Object -First 25 | ForEach-Object {$_.Replace('.sql ','|')})
        if(($before -join ',') -cne ($expected -join ',')){throw 'Baseline migration checksums differ.'}
        [void](D @('cp',(Join-Path $repo 'database/tests/upgrade_0024_fixture.sql'),($pg+':/tmp/upgrade-0024-fixture.sql')))
        [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/upgrade-0024-fixture.sql'))
        $legacy=LegacyFingerprint $pg 'usapp_test'
        Write-Output 'UPGRADE_BASELINE_0024_PASSED migrations=25'
    }
    # Apply the current migration set and verify the installed checksums and permissions.
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
    if($UpgradeFrom0024){
        if((LegacyFingerprint $pg 'usapp_test') -cne $legacy){throw 'Legacy fixture changed after upgrade or repeat apply.'}
        Write-Output 'UPGRADE_0024_TO_0053_LEGACY_DATA_PRESERVED'
        if ($RollbackImage) { Test-ReleasePair }
    }

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
    $fixture=@'
DO $fixture$
DECLARE a uuid:=gen_random_uuid();b uuid:=gen_random_uuid();c uuid:=gen_random_uuid();adm uuid:=gen_random_uuid();r jsonb;
BEGIN
 INSERT INTO users(id,email_canonical,email_display) VALUES(a,'backup_probe_a@example.com','backup_probe_a@example.com'),(b,'backup_probe_b@example.com','backup_probe_b@example.com');
 INSERT INTO sessions(token_hash,csrf_token_hash,user_id,is_authenticated,expires_at)
  VALUES(md5(a::text),md5(b::text),a,true,now()+interval '1 hour');
 INSERT INTO notification_reads(notification_id,couple_id,recipient_id)
  SELECT id,couple_id,recipient_id FROM notifications WHERE recipient_id=a AND login_event_id IS NOT NULL;
 INSERT INTO couples(id,name,created_by_user_id) VALUES(c,'Backup restore marker',a);
 INSERT INTO couple_members(user_id,couple_id,slot) VALUES(a,c,1),(b,c,2);
 INSERT INTO admin_users(id,username,password_hash) VALUES(adm,'backup_probe_admin','synthetic-only-no-real-login');
 r:=usapp_admin_wallet_adjust(adm,b,'flower',1000,0,'Synthetic backup restore fixture','backup-probe-key-0000001');
 IF r ? 'error' THEN RAISE EXCEPTION 'backup fixture refused';END IF;
END $fixture$;
'@
    [void](D @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-c',$fixture))
    if($JournalDrill){NewJointObjectBackup}
    # Add a separate empty PG instance with fresh trusted roles; never attach an API or publish a port.
    $definition=[IO.File]::ReadAllText($compose)
    $service=@'
  restore:
    image: postgres:17
    pull_policy: never
    labels:
      usapp.j2b: PROJECT
    environment:
      POSTGRES_DB: usapp_restore_probe
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
      test: ["CMD-SHELL", "pg_isready -U postgres -d usapp_restore_probe"]
      interval: 1s
      timeout: 3s
      retries: 30
'@
    $service=$service.Replace('PROJECT',$project).Replace('INIT',$init)
    $definition=[regex]::Replace($definition,'(?m)^networks:\r?$',($service+"`nnetworks:"))
    [IO.File]::WriteAllText($compose,$definition,$utf8)
    [void](D @('compose','--env-file',$envFile,'-f',$compose,'up','-d','--wait','restore'))
    $restorePg=@(D @('compose','--env-file',$envFile,'-f',$compose,'ps','-q','restore'))[-1]
    $restorePg=@(D @('inspect',$restorePg,'--format','{{.Id}}'))[-1];Assert-Owned $restorePg
    [void](D @('exec',$restorePg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_restore_probe','-c','CREATE ROLE usapp_memory_eraser NOLOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS NOINHERIT'))
    $keys=Join-Path $work 'ephemeral-keys';[void](New-Item -ItemType Directory -Path $keys)
    $acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false);$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User;$acl.SetOwner($sid);$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'));Set-Acl -LiteralPath $keys -AclObject $acl
    $key=[Security.Cryptography.RSA]::Create(2048)
    $certificate=$null
    try {
     $request=[Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=usapp-backup-disposable-test',$key,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
     $certificate=$request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-1),[DateTimeOffset]::UtcNow.AddHours(1))
     $pass=[guid]::NewGuid().ToString('N');$secure=ConvertTo-SecureString -String $pass -AsPlainText -Force
     $pfx=Join-Path $keys 'ephemeral.pfx';$cer=Join-Path $keys 'signer.cer';$keyFiles=@($pfx,$cer)
     [IO.File]::WriteAllBytes($pfx,$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx,$pass));[IO.File]::WriteAllBytes($cer,$certificate.RawData)
    } finally {if($certificate){$certificate.Dispose()};$key.Dispose()}
    $replicaDirectory=Join-Path $work 'replica'
    [void](New-Item -ItemType Directory -Path $replicaDirectory)
    $jobScript=Join-Path $repo 'tools/backup/Start-DatabaseBackupJob.ps1'
    $jobConfig=Join-Path $keys 'job.json'
    $passwordFile=Join-Path $keys 'password.dpapi'
    $keyFiles+=@($jobConfig,$passwordFile)
    $taskConfig=@{
        ContainerId=$pg; OwnerLabelKey='usapp.j2b'; ExpectedOwner=$project; Database='usapp_test'; DatabaseUser='postgres'
        SigningPfxPath=$pfx; SigningPasswordPath=$passwordFile; RecipientPfxPath=$pfx; RecipientPasswordPath=$passwordFile
        PrimaryDirectory=$work; ReplicaDirectory=$replicaDirectory; KeepDailyDays=7; KeepWeeklyWeeks=4
    }
    [IO.File]::WriteAllText($jobConfig,(ConvertTo-Json -InputObject $taskConfig),$utf8)
    $hostExecutable=(Get-Process -Id $PID).Path
    function RunBackupJob {
        $oldPreference=$ErrorActionPreference
        $ErrorActionPreference='Continue'
        try{
            $lines=@(& $hostExecutable -NoProfile -NonInteractive -File $jobScript -ConfigPath $jobConfig 2>&1 |
                ForEach-Object {[string]$_})
            return [pscustomobject]@{code=$LASTEXITCODE; output=($lines -join "`n")}
        }finally{$ErrorActionPreference=$oldPreference}
    }
    [IO.File]::WriteAllText($passwordFile,'synthetic-plaintext-refusal-fixture',$utf8)
    $badSecret=RunBackupJob
    if($badSecret.code -ne 2 -or $badSecret.output.Trim() -cne 'BACKUP_JOB_CONFIGURATION_REFUSED'){
        throw 'Scheduled backup must refuse a plaintext password without echoing it.'
    }
    Write-Output 'BACKUP_JOB_PLAINTEXT_REFUSED'
    [IO.File]::WriteAllText($passwordFile,(ConvertFrom-SecureString $secure),$utf8)
    $lease=[IO.File]::Open((Join-Path $work '.usapp-backup-usapp_test.lock'),[IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{
        $overlap=RunBackupJob
    }finally{$lease.Dispose()}
    if($overlap.code -ne 1 -or $overlap.output.Trim() -cne 'BACKUP_JOB_CYCLE_FAILED' -or
       @(Get-ChildItem -LiteralPath $work -Filter '*.usbackup' -File).Count -or
       @(Get-ChildItem -LiteralPath $replicaDirectory -Filter '*.usbackup' -File).Count){
        throw ('Overlapping scheduled cycle did not report a cycle failure. Exit: '+$overlap.code)
    }
    Write-Output 'BACKUP_CYCLE_OVERLAP_REFUSED'
    $job=RunBackupJob
    if($job.code -ne 0){throw ('Scheduled backup failed. Exit: '+$job.code+' '+$job.output)}
    $cycle=ConvertFrom-Json -InputObject $job.output
    Write-Output 'BACKUP_JOB_NONINTERACTIVE_PASSED'
    $artifact=$cycle.primary_path
    $replica=$cycle.replica_path
    $planned=@($cycle.retention_plan.entries | Where-Object archive -ceq ([IO.Path]::GetFileName($artifact)))
    if($cycle.restore_verified -ne $false -or $planned.Count -ne 1 -or $planned[0].status -cne 'keep' -or
       $planned[0].encrypted_sha256 -cne $cycle.encrypted_sha256){throw 'Generated archive retention inventory mismatch.'}
    Write-Output 'BACKUP_CYCLE_RETENTION_PASSED'
    $healthOutput=& $hostExecutable -NoProfile -NonInteractive -File (Join-Path $repo 'tools/backup/Test-DatabaseBackupHealth.ps1') `
        -PrimaryDirectory $work -ReplicaDirectory $replicaDirectory -Database usapp_test -MaximumAgeHours 24
    if($LASTEXITCODE -ne 0){throw 'Generated backup failed its health check.'}
    $health=ConvertFrom-Json -InputObject $healthOutput
    if(!$health.healthy -or $health.restore_verified -or $health.latest_archive.encrypted_sha256 -cne $cycle.encrypted_sha256){
        throw 'Backup health report does not match the generated archive.'
    }
    Write-Output 'BACKUP_HEALTH_GENERATED_ARCHIVE_PASSED'
    $restoreTool=Join-Path $repo 'tools/backup/Test-DatabaseBackupRestore.ps1'
    $restoreArgs=@{ContainerId=$restorePg;OwnerLabelKey='usapp.j2b';ExpectedOwner=$project;Database='usapp_restore_probe'
        InputPath=$replica;TrustedSignerCertificatePath=$cer;RecipientPfxPath=$pfx;RecipientPfxPassword=$secure}
    if($JournalDrill){
        $wrongDigest='0'*64
        if($wrongDigest -ceq $cycle.encrypted_sha256){$wrongDigest='1'*64}
        $digestRefused=$false
        try{& $restoreTool @restoreArgs -ExpectedArchiveSha256 $wrongDigest}
        catch{if($_.Exception.Message -cne 'Encrypted archive digest mismatch.'){throw};$digestRefused=$true}
        if(!$digestRefused){throw 'Restore accepted an incorrect archive digest.'}
        Write-Output 'DATABASE_RESTORE_WRONG_ARCHIVE_REFUSED'
    }
    $restoreProof=@(& $restoreTool @restoreArgs -ExpectedArchiveSha256 $cycle.encrypted_sha256)
    $restoreProof | Write-Output
    if(@($restoreProof | Where-Object {$_ -ceq ('DATABASE_RESTORE_ARCHIVE_BOUND '+$cycle.encrypted_sha256)}).Count -ne 1){
        throw 'Restored archive binding unconfirmed.'
    }
    Write-Output 'BACKUP_REPLICA_RESTORE_PASSED'
    if($UpgradeFrom0024){
        if((LegacyFingerprint $restorePg 'usapp_restore_probe') -cne $legacy){throw 'Restored legacy fixture differs from the baseline.'}
        Write-Output 'UPGRADED_DATABASE_REPLICA_RESTORE_PASSED'
    }
    $proof=@(D @('exec',$restorePg,'psql','-X','-At','-U','postgres','-d','usapp_restore_probe','-c',"SELECT count(*) FROM admin_wallet_adjustments a JOIN users u ON u.id=a.user_id WHERE u.email_canonical='backup_probe_b@example.com' AND a.delta=1000 AND a.balance_after=1000"))[-1]
    if($proof -cne '1'){throw 'Restored actual financial fixture absent.'}
    $loginProof=@(D @('exec',$restorePg,'psql','-X','-At','-U','postgres','-d','usapp_restore_probe','-c',"SELECT count(*) FROM notifications n JOIN users u ON u.id=n.recipient_id JOIN notification_reads r ON r.notification_id=n.id WHERE u.email_canonical='backup_probe_a@example.com' AND n.event_type='security.session_created' AND n.couple_id IS NULL AND r.couple_id IS NULL"))[-1]
    if($loginProof -cne '1'){throw 'Restored personal login/NULL-space receipt absent or duplicated.'}
    $clockProof=@(D @('exec',$restorePg,'psql','-X','-At','-U','postgres','-d','usapp_restore_probe','-c',"SELECT (NOT a.atthasdef AND position('GREATEST(clock_timestamp(), p.created_at)' in pg_get_functiondef('usapp_memory_decision_guard()'::regprocedure))>0)::text FROM pg_attribute a WHERE a.attrelid='memory_edit_decisions'::regclass AND a.attname='decided_at'"))[-1]
    if($clockProof -cne 'true'){throw 'Restored memory decision automatic clock guard/default mismatch.'}
    Write-Output 'RESTORED_MEMORY_CLOCK_GUARD_AND_DEFAULT_PASSED'
    $refused=$false
    try{& (Join-Path $repo 'tools/backup/Test-DatabaseBackupRestore.ps1') -ContainerId $restorePg -OwnerLabelKey 'usapp.j2b' -ExpectedOwner $project -Database usapp_restore_probe -InputPath $artifact -TrustedSignerCertificatePath $cer -RecipientPfxPath $pfx -RecipientPfxPassword $secure}catch{$refused=$true}
    if(!$refused){throw 'Nonempty restore target accepted.'}
    [void](D @('exec',$restorePg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','postgres','-c','CREATE DATABASE usapp_restore_unsafe OWNER usapp_api'))
    $refused=$false
    try{& (Join-Path $repo 'tools/backup/Test-DatabaseBackupRestore.ps1') -ContainerId $restorePg -OwnerLabelKey 'usapp.j2b' -ExpectedOwner $project -Database usapp_restore_unsafe -InputPath $artifact -TrustedSignerCertificatePath $cer -RecipientPfxPath $pfx -RecipientPfxPassword $secure}catch{$refused=$true}
    if(!$refused){throw 'Application-owned restore database accepted.'}
    $small=Join-Path $work 'refused.usbackup';$refused=$false
    try{& (Join-Path $repo 'tools/backup/New-DatabaseBackup.ps1') -ContainerId $pg -OwnerLabelKey 'usapp.j2b' -ExpectedOwner $project -Database usapp_test -DatabaseUser postgres -SigningPfxPath $pfx -SigningPfxPassword $secure -RecipientPfxPath $pfx -RecipientPfxPassword $secure -OutputPath $small -MaximumBytes 1024}catch{$refused=$true}
    if(!$refused -or (Test-Path -LiteralPath $small)){throw 'Source file-size refusal did not fail closed.'}
    Write-Output 'DATABASE_BACKUP_RESTORE_HARNESS_PASSED; actual fixture restored; nonempty target and source file cap refused; DB only, no release/journal/object acceptance'
    if($JournalDrill){InvokeJointRestore $cycle.encrypted_sha256}
} finally {
 foreach($id in ($releaseApis+@($compiler,$restorePg,$pg))){if($id){Assert-Owned $id;[void](D @('stop','--time','5',$id))}}
 foreach($file in $keyFiles){if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file -Force}}
 if($keys -and (Test-Path -LiteralPath $keys)){Remove-Item -LiteralPath $keys -Force}
 foreach($name in @('ISO_SUPERUSER_PASSWORD','ISO_API_PASSWORD','ISO_PUSH_PASSWORD','SESSION_SECRET')){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
 Write-Output ('Backup test containers stopped and retained. Work folder: '+$work)
}
