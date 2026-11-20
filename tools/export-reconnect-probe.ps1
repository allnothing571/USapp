param([int]$DelayMs = 20, [int]$Rounds = 12, [switch]$Chain, [ValidateSet('','warm','short','watchdog','teardown','teardown-close')][string]$Variant = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$run = 'usapp-reconnect-' + [guid]::NewGuid().ToString('N').Substring(0,10)
$work = Join-Path $env:TEMP $run
[void](New-Item -ItemType Directory -Path $work)
$utf8 = [Text.UTF8Encoding]::new($false)
$pg = ''; $compiler = ''; $network = $run + '_internal'
$step = 0
function D([string[]]$Arguments) {
    $script:step++
    $old=$ErrorActionPreference; $ErrorActionPreference='Continue'
    try { $lines=@(& docker @Arguments 2>&1 | ForEach-Object {[string]$_}); $code=$LASTEXITCODE }
    finally {$ErrorActionPreference=$old}
    [IO.File]::WriteAllLines((Join-Path $work ($script:step.ToString('000')+'.log')),[string[]]$lines,$utf8)
    if($code -ne 0){throw "Docker command failed ($code); diagnostic output retained in $work"}
    return $lines
}
function Owned([string]$Id) {
    if($Id -notmatch '^[0-9a-f]{64}$'){throw 'Exact ID required'}
    if(@(D @('inspect',$Id,'--format','{{index .Config.Labels `usapp.exportprobe`}}'))[-1] -cne $run){throw 'Owner mismatch'}
}
try {
    $octet=Get-Random -Minimum 1 -Maximum 250
    [void](D @('network','create','--internal','--subnet',("10.253.$octet.0/24"),'--label',('usapp.exportprobe='+$run),$network))
    $env:POSTGRES_PASSWORD=[guid]::NewGuid().ToString('N')+[guid]::NewGuid().ToString('N')
    $pg=@(D @('run','-d','--pull','never','--label',('usapp.exportprobe='+$run),'--network',$network,'--network-alias','postgres',
        '--tmpfs','/var/lib/postgresql/data:rw,size=256m','-e','POSTGRES_PASSWORD','-e','POSTGRES_DB=usapp_test','postgres:17'))[-1]
    Owned $pg
    for($i=0;$i -lt 30;$i++){
        $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try{& docker exec $pg pg_isready -h 127.0.0.1 -U postgres -d usapp_test *> $null;$ready=$LASTEXITCODE -eq 0}finally{$ErrorActionPreference=$old}
        if($ready){break};Start-Sleep -Seconds 1
    }
    if(-not $ready){throw 'Database not ready'}
    if(@(D @('exec',$pg,'psql','-X','-At','-U','postgres','-d','usapp_test','-c','SELECT current_database()'))[-1] -ne 'usapp_test'){throw 'Wrong database'}
    $compiler=@(D @('run','-d','--pull','never','--label',('usapp.exportprobe='+$run),'--network',$network,
        '--entrypoint','sleep',
        'sha256:83afaf6179c2f2c80c73ee209f907773f33a67deac1610905924ed17e66cbfb1','300'))[-1]
    Owned $compiler
    foreach($id in @($pg,$compiler)){if(@(D @('inspect',$id,'--format','{{json .HostConfig.PortBindings}}'))[-1] -notin @('null','{}')){throw 'Ports forbidden'}}
    if(@(D @('network','inspect',$network,'--format','{{.Internal}}'))[-1] -ne 'true'){throw 'Internal network required'}
    [void](D @('cp',(Join-Path $repo 'tools/probes/export-reconnect'),($compiler+':/tmp/probe-src')))
    $build=@(D @('exec',$compiler,'sh','-c','cmake -S /tmp/probe-src -B /tmp/probe-build -DCMAKE_BUILD_TYPE=Debug && cmake --build /tmp/probe-build -j2'))
    [IO.File]::WriteAllLines((Join-Path $work 'build.log'),[string[]]$build,$utf8)
    $env:PROBE_DSN='host=postgres port=5432 dbname=usapp_test user=postgres password='+$env:POSTGRES_PASSWORD
    $shell=@'
#!/bin/sh
timeout 75 /tmp/probe-build/export_reconnect_probe "$1" "$2" "$3" > /tmp/probe.log 2>&1
code=$?
cat /tmp/probe.log
echo PROBE_EXIT=$code
exit 0
'@
    [IO.File]::WriteAllText((Join-Path $work 'run.sh'),$shell.Replace("`r`n","`n"),$utf8)
    [void](D @('cp',(Join-Path $work 'run.sh'),($compiler+':/tmp/run-probe.sh')))
    Write-Output ('PROBE RUN: '+$run+'; work='+$work)
    $mode=if($Chain){'chain'}else{'plain'}
    if($Variant){$mode=$Variant}
    $output=@(D @('exec','-e','PROBE_DSN',$compiler,'sh','/tmp/run-probe.sh',[string]$DelayMs,[string]$Rounds,$mode))
    [IO.File]::WriteAllLines((Join-Path $work 'probe.log'),[string[]]$output,$utf8)
    $output | Where-Object {$_ -match '^PROBE_'} | Select-Object -Last 12 | Write-Output
}finally{
    foreach($id in @($compiler,$pg)){if($id){Owned $id;[void](D @('stop','--time','3',$id))}}
    $env:POSTGRES_PASSWORD=$null;$env:PROBE_DSN=$null
    Write-Output ('Probe resources stopped and retained: '+$work)
}
