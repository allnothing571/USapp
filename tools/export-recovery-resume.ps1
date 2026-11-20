# Focused continuation after an unrelated CTest abort. Only a stopped, exact-owned disposable E1 run is accepted.
param([Parameter(Mandatory=$true)][ValidatePattern('^usapp-e1-[0-9a-f]{10}$')][string]$Run,[switch]$PushDiagnostics,
      [ValidateSet('','baseline','fixed')][string]$PushClockControls='')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$prior=Join-Path $env:TEMP $Run
$work=Join-Path $prior ((@('recovery-','push-diagnostics-')[[int][bool]$PushDiagnostics])+[guid]::NewGuid().ToString('N').Substring(0,8))
$utf8=[Text.UTF8Encoding]::new($false)
$compiler='';$pg=''
function Owned([string]$Id){
    if($Id -notmatch '^[0-9a-f]{64}$'){throw 'Exact id required'}
    $owner=& docker inspect $Id --format '{{index .Config.Labels `usapp.j2b`}}'
    if($LASTEXITCODE -ne 0 -or $owner -cne $Run){throw 'Owner mismatch'}
    $ports=& docker inspect $Id --format '{{json .HostConfig.PortBindings}}'
    if($ports -notin @('null','{}')){throw 'Published ports forbidden'}
}
function SaveRun([string]$Name,[string[]]$Arguments){
    $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{$out=@(& docker @Arguments 2>&1 | ForEach-Object{[string]$_});$exit=$LASTEXITCODE}finally{$ErrorActionPreference=$old}
    [IO.File]::WriteAllLines((Join-Path $work ($Name+'.log')),[string[]]$out,$utf8)
    $out | Where-Object {$_ -match '^E1_|^DUAL_|RESULT|断言|\[FAIL\]|PUSH_COMMAND_CLOCK|PUSH_GUARD_SAMPLE|DETAIL:.*(recorded_|mismatch|invalid|missing_)'} | Write-Output
    if($exit -ne 0){throw ($Name+' failed; exit='+$exit)}
}
function BaselinePushGuard {
    $migration=[IO.File]::ReadAllText((Join-Path $repo 'database/migrations/0022_push_subscriptions.sql'))
    $start=$migration.IndexOf('CREATE FUNCTION usapp_push_command_guard()')
    $end=$migration.IndexOf('$command_guard$;',$start)+('$command_guard$;').Length
    if($start -lt 0 -or $end -le $start){throw 'Guard source not found'}
    return $migration.Substring($start,$end-$start).Replace('CREATE FUNCTION','CREATE OR REPLACE FUNCTION')
}
try{
    $compiler=& docker inspect ($Run+'-compiler') --format '{{.Id}}'
    if($LASTEXITCODE -ne 0){throw 'Missing retained compiler'}
    Owned $compiler
    $pg=& docker compose --env-file (Join-Path $prior 'isolated.env') -f (Join-Path $prior 'compose.yml') ps -a -q postgres
    $pg=& docker inspect $pg --format '{{.Id}}'
    Owned $pg
    foreach($id in @($compiler,$pg)){
        if((& docker inspect $id --format '{{.State.Running}}') -ne 'false'){throw 'Both retained containers must be stopped'}
    }
    if((& docker network inspect ($Run+'_isolated') --format '{{.Internal}}') -ne 'true'){throw 'Internal network required'}
    [void](New-Item -ItemType Directory -Path $work)
    foreach($line in Get-Content -LiteralPath (Join-Path $prior 'isolated.env')){
        if($line -match '^(ISO_SUPERUSER_PASSWORD|ISO_API_PASSWORD|ISO_PUSH_PASSWORD)=([0-9a-f]{64})$'){
            [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
        }else{throw 'Unexpected generated credential file'}
    }
    if(-not $env:ISO_SUPERUSER_PASSWORD -or -not $env:ISO_API_PASSWORD){throw 'Missing generated credentials'}
    SaveRun 'start-db' @('start',$pg)
    $ready=$false
    for($i=0;$i -lt 30;$i++){
        & docker exec $pg pg_isready -h 127.0.0.1 -U postgres -d usapp_test *> $null
        if($LASTEXITCODE -eq 0){$ready=$true;break}
        Start-Sleep -Milliseconds 500
    }
    if(-not $ready){throw 'Database not ready'}
    # Stopping a tmpfs database discards its contents; this continuation recreates only that disposable database.
    $identity=& docker exec $pg psql -X -At -U postgres -d usapp_test -c 'SELECT current_database()'
    if($identity -ne 'usapp_test'){throw 'Wrong database'}
    & (Join-Path $repo 'database/apply.ps1') -Mode Docker -Database usapp_test -User postgres -Password $env:ISO_SUPERUSER_PASSWORD -ComposeFile (Join-Path $prior 'compose.yml') -EnvFile (Join-Path $prior 'isolated.env') -InitRoles *> (Join-Path $work 'apply.log')
    if($LASTEXITCODE -ne 0){throw 'Migration apply failed'}
    SaveRun 'start-compiler' @('start',$compiler)
    $snapshot=if($PushDiagnostics -or $PushClockControls){@('services/api/src/services/PushService.cc','services/api/tests/PushDispatchDbTest.cc','services/api/tests/WalletLedgerProbe.h')}else{@('services/api/src/services/ExportService.cc','services/api/src/services/ExportService.h','services/api/tests/ExportDbTests.h')}
    foreach($relative in $snapshot){
        $remote=& docker exec $compiler sha256sum ('/workspace/'+$relative)
        if($LASTEXITCODE -ne 0 -or $remote.Split(' ')[0] -cne (Get-FileHash (Join-Path $repo $relative) -Algorithm SHA256).Hash.ToLowerInvariant()){throw 'Source snapshot mismatch'}
    }
    $env:USAPP_TEST_DSN='postgresql://usapp_api:'+$env:ISO_API_PASSWORD+'@postgres:5432/usapp_test'
    $env:USAPP_TEST_MAINT_CONNINFO='host=postgres port=5432 dbname=usapp_test user=postgres password='+$env:ISO_SUPERUSER_PASSWORD
    if($PushClockControls){
        $controlFile=Join-Path $repo 'database/tests/push_command_timestamp_test.sql'
        if($PushClockControls -eq 'baseline'){
            # Negative control on the final schema too: restore the frozen guard/default ONLY inside this file's rollback.
            $controlSql=[IO.File]::ReadAllText($controlFile)
            $prefix="BEGIN;`nDO `$`$ BEGIN IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'test database required'; END IF; END `$`$;`n"+
                    (BaselinePushGuard)+"`nALTER TABLE public.push_registration_commands ALTER COLUMN recorded_at SET DEFAULT pg_catalog.clock_timestamp();"
            if([regex]::Matches($controlSql,'(?m)^BEGIN;').Count -ne 1){throw 'Unexpected clock test transaction boundary'}
            $controlSql=[regex]::Replace($controlSql,'(?m)^BEGIN;', [System.Text.RegularExpressions.MatchEvaluator]{param($m) $prefix})
            $controlFile=Join-Path $work 'baseline-clock-controls.sql'
            [IO.File]::WriteAllText($controlFile,$controlSql,$utf8)
        }
        SaveRun 'copy-clock-controls' @('cp',$controlFile,($pg+':/tmp/push-clock-controls.sql'))
        foreach($role in @('application','maintenance')){
            SaveRun ('clock-control-'+$role) @('exec',$pg,'psql','-X','-v',('expected='+$PushClockControls),'-v',('mode='+$role),'-U','postgres','-d','usapp_test','-f','/tmp/push-clock-controls.sql')
        }
        if($PushClockControls -eq 'baseline'){
            # On a post-0048 schema, prove that each baseline control rolled its catalog changes back.
            $hasDefault=& docker exec $pg psql -X -At -U postgres -d usapp_test -c "SELECT atthasdef FROM pg_attribute WHERE attrelid='public.push_registration_commands'::regclass AND attname='recorded_at'"
            if($LASTEXITCODE -ne 0){throw 'Catalog verification failed'}
            if($hasDefault -eq 'f'){
                SaveRun 'copy-fixed-controls' @('cp',(Join-Path $repo 'database/tests/push_command_timestamp_test.sql'),($pg+':/tmp/push-clock-controls.sql'))
                foreach($role in @('application','maintenance')){
                    SaveRun ('fixed-after-baseline-'+$role) @('exec',$pg,'psql','-X','-v','expected=fixed','-v',('mode='+$role),'-U','postgres','-d','usapp_test','-f','/tmp/push-clock-controls.sql')
                }
            }
        }
        'PUSH_CLOCK_CONTROLS_COMPLETE; expected='+$PushClockControls+'; logs='+$work
        return
    }
    if($PushDiagnostics){
        SaveRun 'copy-clock' @('cp',(Join-Path $repo 'Claude outputs/clock-dual-probe.c'),($compiler+':/tmp/push-clock-probe.c'))
        SaveRun 'build-clock' @('exec',$compiler,'cc','-O2','/tmp/push-clock-probe.c','-o','/tmp/push-clock-probe')
        SaveRun 'clock' @('exec',$compiler,'timeout','40','/tmp/push-clock-probe','35','-1')
        $original=BaselinePushGuard
        $pattern="(?s)    IF NOT FOUND OR g\.user_id.*?MESSAGE = 'USAPP_PUSH_COMMAND_TARGET';\s+END IF;"
        $labeled=@'
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='missing_generation';
    ELSIF g.user_id IS DISTINCT FROM NEW.user_id THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='user_mismatch';
    ELSIF g.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='couple_mismatch';
    ELSIF g.session_id IS DISTINCT FROM NEW.session_id THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='session_mismatch';
    ELSIF NOT usapp_push_session_valid(NEW.session_id,NEW.user_id) THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='session_invalid';
    ELSIF NEW.recorded_at < g.activated_at THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='recorded_before_activation';
    ELSIF NEW.recorded_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE='UY004',MESSAGE='USAPP_PUSH_COMMAND_TARGET',DETAIL='recorded_after_clock';
    END IF;
'@
        if([regex]::Matches($original,$pattern).Count -ne 1){throw 'Unexpected guard source'}
        # Intentional baseline mechanism diagnostic in this fresh disposable database, never production acceptance.
        $diagnostic="BEGIN;`nDO `$`$ BEGIN IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'test database required'; END IF; END `$`$;`n"+
                    "ALTER TABLE public.push_registration_commands ALTER COLUMN recorded_at SET DEFAULT pg_catalog.clock_timestamp();`n"+
                    [regex]::Replace($original,$pattern,$labeled)+"`nCOMMIT;"
        [IO.File]::WriteAllText((Join-Path $work 'guard-diagnostic.sql'),$diagnostic,$utf8)
        SaveRun 'copy-guard' @('cp',(Join-Path $work 'guard-diagnostic.sql'),($pg+':/tmp/guard-diagnostic.sql'))
        SaveRun 'install-guard' @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_test','-f','/tmp/guard-diagnostic.sql')
        SaveRun 'copy-sample' @('cp',(Join-Path $repo 'tools/probes/push-registration/diagnose.sql'),($pg+':/tmp/push-sample.sql'))
        SaveRun 'registration-sample' @('exec',$pg,'psql','-X','-v','ON_ERROR_STOP=1','-U','usapp_api','-d','usapp_test','-f','/tmp/push-sample.sql')
        SaveRun 'push-dispatch-instrumented' @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO',$compiler,'timeout','120','/workspace/services/api/build-test/tests/usapp_test_push_dispatch')
        'PUSH_DIAGNOSTICS_COMPLETE; not a production acceptance; logs='+$work
        return
    }
    foreach($mode in @('EXPORT','EXPORT_CONNECTION_LOSS','EXPORT_LARGE','EXPORT_CLOSURE')){
        Owned $compiler;Owned $pg
        SaveRun $mode @('exec','-e','USAPP_ROOT=/workspace','-e','USAPP_TEST_DSN','-e','USAPP_TEST_MAINT_CONNINFO','-e',('USAPP_TEST_ONLY_'+$mode+'=1'),$compiler,'timeout','180','/workspace/services/api/build-test/tests/usapp_test_db_gated')
    }
    'EXPORT_RECOVERY_FOCUSED_COMPLETE; earlier CTest failure remains; logs='+$work
}finally{
    foreach($id in @($compiler,$pg)){if($id){Owned $id; & docker stop --timeout 3 $id *> $null}}
    foreach($name in @('ISO_SUPERUSER_PASSWORD','ISO_API_PASSWORD','ISO_PUSH_PASSWORD','USAPP_TEST_DSN','USAPP_TEST_MAINT_CONNINFO')){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
    'Stopped and retained: '+$work
}
