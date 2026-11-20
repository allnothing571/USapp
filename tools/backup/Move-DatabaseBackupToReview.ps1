# Move one reviewed backup pair into a local subdirectory, keeping the verified replica in place.
[CmdletBinding(SupportsShouldProcess, ConfirmImpact='High')]
param(
    [Parameter(Mandatory)][string]$PrimaryDirectory,
    [Parameter(Mandatory)][string]$ReplicaDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z_][A-Za-z0-9_]{0,62}$')][string]$Database,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,179}\.usbackup$')][string]$ArchiveName,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory)][ValidateRange(0,366)][int]$KeepDailyDays,
    [Parameter(Mandatory)][ValidateRange(0,104)][int]$KeepWeeklyWeeks,
    [ValidateRange(2,365)][int]$MinimumArchives=2
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Get-RegularItem([string]$Path,[bool]$Directory){
    $item=Get-Item -LiteralPath $Path
    if($item.PSIsContainer -ne $Directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'A regular file or directory is required.'
    }
    return $item
}

function Open-VerifiedReplica([string]$Source,[string]$Replica,[string]$Digest){
    [void](Get-RegularItem $Source $false)
    [void](Get-RegularItem $Replica $false)
    # An exclusive primary handle also refuses a replica path that aliases the primary file.
    $sourceHandle=[IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    try{
        $replicaHandle=[IO.File]::Open($Replica,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try{
            if($replicaHandle.Length -ne $sourceHandle.Length -or $replicaHandle.Length -lt 1 -or
               $replicaHandle.Length -gt (268435456L+2097152)){
                throw 'Replica size does not match the reviewed archive.'
            }
            if((Get-FileHash -Algorithm SHA256 -InputStream $sourceHandle).Hash -ine $Digest -or
               (Get-FileHash -Algorithm SHA256 -InputStream $replicaHandle).Hash -ine $Digest){
                throw 'Primary or replica digest does not match the reviewed archive.'
            }
            return $replicaHandle
        }catch{
            $replicaHandle.Dispose()
            throw
        }
    }finally{$sourceHandle.Dispose()}
}

$primary=(Get-RegularItem $PrimaryDirectory $true).FullName
$replicas=(Get-RegularItem $ReplicaDirectory $true).FullName
$review=(Get-RegularItem (Join-Path $primary '.usapp-retention-review') $true).FullName
if($replicas -ieq $primary -or $replicas -ieq $review){
    throw 'Select separate primary, replica and review directories.'
}
if($ArchiveName -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)'){
    throw 'Reserved Windows archive name refused.'
}
$source=Join-Path $primary $ArchiveName
$destination=Join-Path $review $ArchiveName
if(!$PSCmdlet.ShouldProcess($source,('Move archive and manifest to '+$review))){return}

$lockPath=Join-Path $primary ('.usapp-backup-'+$Database+'.lock')
if(Test-Path -LiteralPath $lockPath){[void](Get-RegularItem $lockPath $false)}
try{
    $lease=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
}catch [IO.IOException]{
    throw 'Backup cycle lock could not be acquired. Check for an active backup or review move.'
}
try{
    $plan=& (Join-Path $PSScriptRoot 'Get-DatabaseBackupRetentionPlan.ps1') -Directory $primary -Database $Database `
        -KeepDailyDays $KeepDailyDays -KeepWeeklyWeeks $KeepWeeklyWeeks -MinimumArchives $MinimumArchives
    if(@($plan.entries | Where-Object status -eq 'review').Count){
        throw 'Resolve primary inventory review entries before moving an archive.'
    }
    $candidate=@($plan.entries | Where-Object archive -ceq $ArchiveName)
    if($candidate.Count -ne 1 -or $candidate[0].status -cne 'candidate' -or
       $candidate[0].encrypted_sha256 -ine $ExpectedSha256){
        throw 'Archive is not a current candidate with the reviewed digest.'
    }
    if((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination+'.json'))){
        throw 'Review destination already exists.'
    }
    $verifiedReplica=Open-VerifiedReplica $source (Join-Path $replicas $ArchiveName) $ExpectedSha256
    try{
        [void](Get-RegularItem ($source+'.json') $false)
        [IO.File]::Move(($source+'.json'),($destination+'.json'))
        try{
            [IO.File]::Move($source,$destination)
        }catch{
            try{
                [IO.File]::Move(($destination+'.json'),($source+'.json'))
            }catch{
                throw ('Archive move and manifest rollback failed. Inspect these paths: '+$source+' and '+$destination)
            }
            throw
        }
    }finally{$verifiedReplica.Dispose()}
    [pscustomobject]@{archive_path=$destination; manifest_path=($destination+'.json'); encrypted_sha256=$candidate[0].encrypted_sha256}
}finally{$lease.Dispose()}
