$ErrorActionPreference='Stop'
$health=Join-Path $PSScriptRoot 'Test-DatabaseBackupHealth.ps1'
$hostExecutable=(Get-Process -Id $PID).Path
$work=Join-Path ([IO.Path]::GetTempPath()) ('usapp-health-test-'+[guid]::NewGuid().ToString('N'))
$primary=Join-Path $work 'primary'
$replica=Join-Path $work 'replica'
[void](New-Item -ItemType Directory -Path $work,$primary,$replica)
$archive=Join-Path $primary 'newest.usbackup'
$sidecar=$archive+'.json'
$copy=Join-Path $replica 'newest.usbackup'
$unpaired=Join-Path $primary 'unpaired.usbackup'
$checks=0

function RunHealth {
    & $hostExecutable -NoProfile -NonInteractive -File $health -PrimaryDirectory $primary -ReplicaDirectory $replica `
        -Database usapp -MaximumAgeHours 24 -AsOf '2026-11-19T12:00:00+00:00'
}

function Check([int]$Code,[string]$Issue,[string]$ReplicaStatus){
    $line=RunHealth
    if($LASTEXITCODE -ne $Code){throw ('Health check exit code mismatch: '+$LASTEXITCODE)}
    $result=ConvertFrom-Json -InputObject $line
    if($result.healthy -ne ($Code -eq 0) -or $result.restore_verified -ne $false -or $result.replica_status -cne $ReplicaStatus){
        throw 'Health status mismatch.'
    }
    if(($Issue -and $Issue -notin $result.issues) -or (!$Issue -and @($result.issues).Count)){
        throw ('Unexpected health issues: '+($result.issues -join ','))
    }
    $script:checks++
}

function Manifest([string]$Time){
    $digest=(Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    $json=@{format='usapp-db-cms-v1';database='usapp';created_utc=$Time;encrypted_sha256=$digest} | ConvertTo-Json
    [IO.File]::WriteAllText($sidecar,$json)
}

try{
    Check 1 'no-valid-primary-archive' 'not-checked'
    [IO.File]::WriteAllText($archive,'synthetic encrypted bytes')
    Manifest '2026-11-18T12:00:00Z'
    Check 1 'latest-replica-missing' 'missing'
    [IO.File]::WriteAllText($copy,'different bytes')
    Check 1 'latest-replica-digest-mismatch' 'digest-mismatch'
    [IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($archive))
    Check 0 '' 'matched'
    Manifest '2026-11-18T11:59:59Z'
    Check 1 'latest-archive-stale' 'matched'
    Manifest '2026-11-19T10:00:00Z'
    [IO.File]::WriteAllText($unpaired,'unpaired archive')
    Check 1 'primary-inventory-needs-review' 'matched'
    $conflict=Join-Path $work 'missing'
    $oldPreference=$ErrorActionPreference
    $ErrorActionPreference='Continue'
    try{
        $failure=@(& $hostExecutable -NoProfile -NonInteractive -File $health -PrimaryDirectory $primary -ReplicaDirectory $conflict `
            -Database usapp -MaximumAgeHours 24 2>&1 | ForEach-Object {[string]$_})
        if($LASTEXITCODE -ne 2 -or ($failure -join '') -cne 'BACKUP_HEALTH_CHECK_FAILED'){throw 'Expected probe failure marker.'}
    }finally{$ErrorActionPreference=$oldPreference}
    $checks++
    Write-Output ('BACKUP_HEALTH_TESTS_PASSED checks='+$checks)
}finally{
    foreach($file in @($archive,$sidecar,$copy,$unpaired)){
        if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file}
    }
    foreach($directory in @($primary,$replica,$work)){Remove-Item -LiteralPath $directory}
}
