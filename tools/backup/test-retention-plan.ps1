$ErrorActionPreference='Stop'
$command=Join-Path $PSScriptRoot 'Get-DatabaseBackupRetentionPlan.ps1'
$work=Join-Path ([IO.Path]::GetTempPath()) ('usapp-retention-test-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $work)
$files=[Collections.Generic.List[string]]::new()
function Fixture([string]$Name,[string]$Timestamp,[string]$Payload,[string]$Database='usapp',[switch]$WrongDigest,[switch]$NoManifest){
    $file=Join-Path $work ($Name+'.usbackup')
    [IO.File]::WriteAllText($file,$Payload)
    $files.Add($file)
    if(!$NoManifest){
        $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if($WrongDigest){$hash='0'*64}
        $json=@{format='usapp-db-cms-v1';database=$Database;created_utc=$Timestamp;encrypted_sha256=$hash} | ConvertTo-Json
        [IO.File]::WriteAllText(($file+'.json'),$json)
        $files.Add($file+'.json')
    }
}
try{
    Fixture 'latest' '2026-11-19T03:00:00.0000000+00:00' 'latest'
    Fixture 'same-day' '2026-11-19T01:00:00Z' 'second'
    Fixture 'yesterday' '2026-11-18T05:00:00Z' 'daily'
    Fixture 'last-sunday' '2026-11-15T23:59:59Z' 'weekly'
    Fixture 'old' '2026-11-08T23:59:59Z' 'old'
    Fixture 'duplicate-old' '2026-11-01T00:00:00Z' 'latest'
    Fixture 'future' '2026-11-19T05:00:01Z' 'future'
    Fixture 'wrong' '2026-11-17T00:00:00Z' 'wrong' -WrongDigest
    Fixture 'missing' '2026-11-17T00:00:00Z' 'missing' -NoManifest
    Fixture 'other' '2026-11-19T04:00:00Z' 'other' 'elsewhere'
    Fixture 'bad-date' '2026-11-31T00:00:00Z' 'bad-date'
    Fixture 'empty' '2026-11-17T00:00:00Z' ''
    $before=@($files | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash })
    $cutoff=[DateTimeOffset]::Parse('2026-11-19T13:00:00+08:00')
    $plan=& $command -Directory $work -Database usapp -KeepDailyDays 2 -KeepWeeklyWeeks 2 -AsOf $cutoff
    $index=@{}
    foreach($entry in $plan.entries){$index[$entry.archive]=$entry}
    $expected=@{
        latest='keep'; 'same-day'='keep'; yesterday='keep'; 'last-sunday'='keep'
        old='candidate'; 'duplicate-old'='keep'; future='review'; wrong='review'; missing='review'
        other='outside-scope'; 'bad-date'='review'; empty='review'
    }
    foreach($name in $expected.Keys){
        if($index[$name+'.usbackup'].status -cne $expected[$name]){throw ('Unexpected status for '+$name)}
    }
    if('daily' -notin $index['yesterday.usbackup'].reasons -or 'weekly' -notin $index['last-sunday.usbackup'].reasons -or
       'archive-digest-mismatch' -notin $index['wrong.usbackup'].reasons){throw 'Retention reasons mismatch.'}
    $minimum=& $command -Directory $work -Database usapp -KeepDailyDays 0 -KeepWeeklyWeeks 0 -MinimumArchives 1 -AsOf $cutoff
    if(@($minimum.entries | Where-Object status -eq 'keep').Count -ne 2){throw 'Minimum must keep copies of the newest digest.'}
    $after=@($files | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash })
    if(($before -join ',') -cne ($after -join ',') -or @(Get-ChildItem -LiteralPath $work -Force).Count -ne $files.Count){
        throw 'Planning changed the input directory.'
    }
    Write-Output 'BACKUP_RETENTION_PLAN_TESTS_PASSED entries=12'
}finally{
    foreach($file in $files){Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue}
    Remove-Item -LiteralPath $work
}
