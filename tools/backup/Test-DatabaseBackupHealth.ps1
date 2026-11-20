# Read-only health check for a primary backup directory and its named replica files.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PrimaryDirectory,
    [Parameter(Mandatory)][string]$ReplicaDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z_][A-Za-z0-9_]{0,62}$')][string]$Database,
    [Parameter(Mandatory)][ValidateRange(1,8760)][int]$MaximumAgeHours,
    [DateTimeOffset]$AsOf=[DateTimeOffset]::UtcNow
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Resolve-ArchiveDirectory([string]$Path){
    $item=Get-Item -LiteralPath $Path
    if(!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'Select a regular backup directory.'
    }
    return $item.FullName
}

function Get-ReplicaStatus([string]$Path,[string]$ExpectedDigest){
    try{
        $item=Get-Item -LiteralPath $Path
        if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
            return 'not-regular'
        }
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try{
            if($stream.Length -lt 1 -or $stream.Length -gt (268435456L+2097152)){
                return 'size-refused'
            }
            if((Get-FileHash -Algorithm SHA256 -InputStream $stream).Hash -ine $ExpectedDigest){
                return 'digest-mismatch'
            }
            return 'matched'
        }finally{
            $stream.Dispose()
        }
    }catch [System.Management.Automation.ItemNotFoundException]{
        return 'missing'
    }catch{
        return 'unreadable'
    }
}

try{
    $primary=Resolve-ArchiveDirectory $PrimaryDirectory
    $replicas=Resolve-ArchiveDirectory $ReplicaDirectory
    if($primary.TrimEnd('\','/').Equals($replicas.TrimEnd('\','/'),[StringComparison]::OrdinalIgnoreCase)){
        throw 'The primary and replica directories must differ.'
    }
    $plan=& (Join-Path $PSScriptRoot 'Get-DatabaseBackupRetentionPlan.ps1') -Directory $primary -Database $Database `
        -KeepDailyDays 0 -KeepWeeklyWeeks 0 -MinimumArchives 1 -AsOf $AsOf
    $latest=$plan.entries | Where-Object { $_.status -in @('keep','candidate') } |
        Sort-Object @{Expression='created_utc';Descending=$true}, archive | Select-Object -First 1
    $review=@($plan.entries | Where-Object status -eq 'review' | Select-Object archive,reasons)
    $issues=[Collections.Generic.List[string]]::new()
    if($review.Count){$issues.Add('primary-inventory-needs-review')}
    $replicaStatus='not-checked'
    if($latest){
        $created=[DateTimeOffset]::ParseExact($latest.created_utc,'o',[Globalization.CultureInfo]::InvariantCulture)
        if(($AsOf-$created).TotalHours -gt $MaximumAgeHours){$issues.Add('latest-archive-stale')}
        $replicaStatus=Get-ReplicaStatus (Join-Path $replicas $latest.archive) $latest.encrypted_sha256
        if($replicaStatus -cne 'matched'){$issues.Add('latest-replica-'+$replicaStatus)}
    }else{
        $issues.Add('no-valid-primary-archive')
    }
    $report=[pscustomobject]@{
        database=$Database
        as_of_utc=$plan.as_of_utc
        maximum_age_hours=$MaximumAgeHours
        healthy=($issues.Count -eq 0)
        latest_archive=$latest
        replica_status=$replicaStatus
        primary_review=$review
        issues=$issues.ToArray()
        restore_verified=$false
    }
    ConvertTo-Json -InputObject $report -Depth 6 -Compress
    if(!$report.healthy){exit 1}
}catch{
    [Console]::Error.WriteLine('BACKUP_HEALTH_CHECK_FAILED')
    exit 2
}
