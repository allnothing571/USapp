# Run one backup cycle for a scheduler or an operator using explicit credentials and directories.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$ContainerId,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]{1,64}$')][string]$OwnerLabelKey,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')][string]$ExpectedOwner,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z_][A-Za-z0-9_]{0,62}$')][string]$Database,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z_][A-Za-z0-9_]{0,62}$')][string]$DatabaseUser,
    [Parameter(Mandatory)][string]$SigningPfxPath,
    [Parameter(Mandatory)][Security.SecureString]$SigningPfxPassword,
    [Parameter(Mandatory)][string]$RecipientPfxPath,
    [Parameter(Mandatory)][Security.SecureString]$RecipientPfxPassword,
    [Parameter(Mandatory)][string]$PrimaryDirectory,
    [Parameter(Mandatory)][string]$ReplicaDirectory,
    [Parameter(Mandatory)][ValidateRange(0,366)][int]$KeepDailyDays,
    [Parameter(Mandatory)][ValidateRange(0,104)][int]$KeepWeeklyWeeks,
    [ValidateRange(1,365)][int]$MinimumArchives=2,
    [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Resolve-ArchiveDirectory([string]$Path){
    $item=Get-Item -LiteralPath $Path
    if(!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'Select an existing regular archive directory.'
    }
    return $item.FullName
}

$primary=Resolve-ArchiveDirectory $PrimaryDirectory
$replicas=Resolve-ArchiveDirectory $ReplicaDirectory
if($primary.TrimEnd('\','/').Equals($replicas.TrimEnd('\','/'),[StringComparison]::OrdinalIgnoreCase)){
    throw 'The primary and replica directories must differ.'
}

$lockPath=Join-Path $primary ('.usapp-backup-'+$Database+'.lock')
if(Test-Path -LiteralPath $lockPath){
    $item=Get-Item -LiteralPath $lockPath
    if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'Backup cycle lock must be a regular file.'
    }
}
$cycleLock=$null
try{
    $cycleLock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
}catch [IO.IOException]{
    throw 'Backup cycle lock could not be acquired. Check for an active cycle or an inaccessible lock.'
}

try{
    $stamp=[DateTime]::UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'",[Globalization.CultureInfo]::InvariantCulture)
    $name=$Database+'-'+$stamp+'-'+[guid]::NewGuid().ToString('N')+'.usbackup'
    $archive=Join-Path $primary $name
    $replica=Join-Path $replicas $name
    $backupArgs=@{
        ContainerId=$ContainerId; OwnerLabelKey=$OwnerLabelKey; ExpectedOwner=$ExpectedOwner
        Database=$Database; DatabaseUser=$DatabaseUser; SigningPfxPath=$SigningPfxPath
        SigningPfxPassword=$SigningPfxPassword; RecipientPfxPath=$RecipientPfxPath
        RecipientPfxPassword=$RecipientPfxPassword; OutputPath=$archive; MaximumBytes=$MaximumBytes
    }
    [void](& (Join-Path $PSScriptRoot 'New-DatabaseBackup.ps1') @backupArgs)
    try{
        $manifest=Get-Content -LiteralPath ($archive+'.json') -Raw | ConvertFrom-Json
        if($manifest.database -cne $Database -or $manifest.format -cne 'usapp-db-cms-v1'){
            throw 'Unexpected generated backup manifest.'
        }
        $copy=& (Join-Path $PSScriptRoot 'Copy-DatabaseBackup.ps1') -InputPath $archive -OutputPath $replica `
            -ExpectedSha256 $manifest.encrypted_sha256 -MaximumBytes $MaximumBytes
    }catch{
        throw ('The primary archive was created but replica publication failed. Primary archive: '+$archive+' Reason: '+$_.Exception.Message)
    }
    $plan=& (Join-Path $PSScriptRoot 'Get-DatabaseBackupRetentionPlan.ps1') -Directory $primary -Database $Database `
        -KeepDailyDays $KeepDailyDays -KeepWeeklyWeeks $KeepWeeklyWeeks -MinimumArchives $MinimumArchives
    $created=@($plan.entries | Where-Object archive -ceq $name)
    if($created.Count -ne 1 -or $created[0].status -cne 'keep' -or
       $created[0].encrypted_sha256 -cne $copy.encrypted_sha256){
        throw ('The published archive pair could not be confirmed in the retention report. Primary archive: '+$archive)
    }
    [pscustomobject]@{
        primary_path=$archive
        replica_path=$copy.path
        encrypted_sha256=$copy.encrypted_sha256
        encrypted_bytes=$copy.bytes
        restore_verified=$false
        retention_plan=$plan
    }
}finally{
    # Keep one stable lock path. The open handle, rather than the file's existence, is the lease.
    $cycleLock.Dispose()
}
