$ErrorActionPreference='Stop'
$move=Join-Path $PSScriptRoot 'Move-DatabaseBackupToReview.ps1'
$work=Join-Path ([IO.Path]::GetTempPath()) ('usapp-review-test-'+[guid]::NewGuid().ToString('N'))
$primary=Join-Path $work 'primary'
$replicas=Join-Path $work 'replicas'
$review=Join-Path $primary '.usapp-retention-review'
[void](New-Item -ItemType Directory -Path $work,$primary,$replicas,$review)
$older=Join-Path $primary 'older.usbackup'
$replica=Join-Path $replicas 'older.usbackup'
$destination=Join-Path $review 'older.usbackup'
$lockPath=Join-Path $primary '.usapp-backup-usapp.lock'
$broken=Join-Path $primary 'broken.usbackup'
$names=@('latest','previous','older')
$checks=0

function Fixture([string]$Name,[int]$AgeDays){
    $path=Join-Path $primary ($Name+'.usbackup')
    [IO.File]::WriteAllText($path,$Name)
    $manifest=@{
        format='usapp-db-cms-v1'; database='usapp'; created_utc=[DateTimeOffset]::UtcNow.AddDays(-$AgeDays).ToString('o')
        encrypted_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    }
    [IO.File]::WriteAllText(($path+'.json'),($manifest | ConvertTo-Json))
}

function Refused([hashtable]$Parameters,[string]$Message){
    $actual=''
    try{[void](& $move @Parameters -Confirm:$false)}catch{$actual=$_.Exception.Message}
    if($actual -notlike ('*'+$Message+'*')){throw ('Expected refusal: '+$Message+' Actual: '+$actual)}
    if(!(Test-Path -LiteralPath $older) -or !(Test-Path -LiteralPath ($older+'.json')) -or
       (Test-Path -LiteralPath ($destination+'.json'))){throw 'Refused move changed the archive pair.'}
    $script:checks++
}

try{
    Fixture latest 1
    Fixture previous 2
    Fixture older 10
    $digest=(Get-FileHash -Algorithm SHA256 -LiteralPath $older).Hash
    Copy-Item -LiteralPath $older -Destination $replica
    $options=@{
        PrimaryDirectory=$primary; ReplicaDirectory=$replicas; Database='usapp'; ArchiveName='older.usbackup'
        ExpectedSha256=$digest; KeepDailyDays=0; KeepWeeklyWeeks=0
    }
    [void](& $move @options -WhatIf)
    if((Test-Path -LiteralPath $lockPath) -or (Test-Path -LiteralPath $destination) -or !(Test-Path -LiteralPath $older)){
        throw 'Preview changed the filesystem.'
    }
    $checks++
    $other=$options.Clone()
    $other.ArchiveName='previous.usbackup'
    $other.ExpectedSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $primary 'previous.usbackup')).Hash
    Refused $other 'not a current candidate'
    $other=$options.Clone()
    $other.ExpectedSha256='f'*64
    Refused $other 'not a current candidate'
    [IO.File]::WriteAllText($replica,'wrong')
    Refused $options 'digest does not match'
    Copy-Item -LiteralPath $older -Destination $replica -Force
    [IO.File]::WriteAllText($destination,'existing')
    Refused $options 'destination already exists'
    if([IO.File]::ReadAllText($destination) -cne 'existing'){throw 'Destination was replaced.'}
    Remove-Item -LiteralPath $destination
    [IO.File]::WriteAllText($broken,'missing manifest')
    Refused $options 'Resolve primary inventory review entries'
    Remove-Item -LiteralPath $broken
    $lease=[IO.File]::Open($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{Refused $options 'lock could not be acquired'}finally{$lease.Dispose()}
    $manifestHash=(Get-FileHash -LiteralPath ($older+'.json')).Hash
    $result=& $move @options -Confirm:$false
    if($result.archive_path -cne $destination -or
       (Get-FileHash -LiteralPath $destination).Hash -cne $digest -or
       (Get-FileHash -LiteralPath ($destination+'.json')).Hash -cne $manifestHash -or
       (Get-FileHash -LiteralPath $replica).Hash -cne $digest -or
       (Test-Path -LiteralPath $older) -or (Test-Path -LiteralPath ($older+'.json'))){
        throw 'Reviewed files or replica differ from expected state.'
    }
    $checks++
    $remaining=& (Join-Path $PSScriptRoot 'Get-DatabaseBackupRetentionPlan.ps1') -Directory $primary -Database usapp `
        -KeepDailyDays 0 -KeepWeeklyWeeks 0
    if(@($remaining.entries | Where-Object status -eq 'keep').Count -ne 2 -or @($remaining.entries).Count -ne 2){
        throw 'The minimum retention floor did not survive the move.'
    }
    $checks++
    Write-Output ('BACKUP_REVIEW_MOVE_TESTS_PASSED checks='+$checks)
}finally{
    $files=@($replica,$destination,($destination+'.json'),$lockPath,$broken)
    foreach($name in $names){
        $file=Join-Path $primary ($name+'.usbackup')
        $files+=@($file,($file+'.json'))
    }
    foreach($file in $files){if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file}}
    foreach($directory in @($review,$primary,$replicas,$work)){[IO.Directory]::Delete($directory,$false)}
}
