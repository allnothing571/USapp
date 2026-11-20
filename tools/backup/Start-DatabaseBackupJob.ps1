# Windows scheduler entry point. Run using powershell.exe or pwsh.exe -NoProfile -NonInteractive -File.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ConfigPath)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Assert-PrivatePath([string]$Path,[bool]$Directory){
    if($Path -notmatch '^[A-Za-z]:[\\/]'){throw 'A local absolute path is required.'}
    $item=Get-Item -LiteralPath $Path
    if($item.PSIsContainer -ne $Directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'A regular private file or directory is required.'
    }
    $acl=Get-Acl -LiteralPath $item.FullName
    if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $accountSid){
        throw 'The task account must own the private path.'
    }
    foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
        if($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
           $rule.IdentityReference.Value -notin @($accountSid,'S-1-5-18','S-1-5-32-544')){
            throw 'A private path grants access to another principal.'
        }
    }
    return $item.FullName
}

function Read-PrivateText([string]$Path,[int]$Limit){
    $path=Assert-PrivatePath $Path $false
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        if($stream.Length -lt 1 -or $stream.Length -gt $Limit){throw 'Private file size is outside bounds.'}
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$true)
        try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
    }finally{
        $stream.Dispose()
    }
}

function Read-TaskPassword([string]$Path){
    $protected=(Read-PrivateText $Path 32768).Trim()
    # ConvertFrom-SecureString without a key emits a Windows DPAPI blob.
    if($protected -cnotmatch '^01000000d08c9ddf0115d1118c7a00c04fc297eb([0-9a-f]{2})+$'){
        throw 'Expected a Windows DPAPI password blob.'
    }
    $password=ConvertTo-SecureString -String $protected
    if($password.Length -eq 0){
        $password.Dispose()
        throw 'Empty task password refused.'
    }
    return $password
}

$signing=$null
$recipient=$null
try{
    try{
        $accountSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $path=Assert-PrivatePath $ConfigPath $false
        [void](Assert-PrivatePath ([IO.Path]::GetDirectoryName($path)) $true)
        $raw=Read-PrivateText $path 16384
        if(!$raw.TrimStart().StartsWith('{')){throw 'Expected a configuration object.'}
        $config=ConvertFrom-Json -InputObject $raw
        $strings=@('ContainerId','OwnerLabelKey','ExpectedOwner','Database','DatabaseUser','SigningPfxPath',
                   'SigningPasswordPath','RecipientPfxPath','RecipientPasswordPath','PrimaryDirectory','ReplicaDirectory')
        $required=$strings+@('KeepDailyDays','KeepWeeklyWeeks')
        $optional=@('MinimumArchives','MaximumBytes')
        foreach($property in $config.PSObject.Properties){
            if($property.Name -cnotin ($required+$optional)){throw 'Unknown configuration property.'}
        }
        foreach($name in $required){
            if(!$config.PSObject.Properties[$name]){throw 'Missing configuration property.'}
        }
        foreach($name in $strings){
            if($config.$name -isnot [string] -or [string]::IsNullOrWhiteSpace($config.$name)){
                throw 'Invalid string configuration property.'
            }
        }
        foreach($name in (@('KeepDailyDays','KeepWeeklyWeeks')+$optional)){
            if($config.PSObject.Properties[$name] -and $config.$name -isnot [int] -and $config.$name -isnot [long]){
                throw 'Invalid integer configuration property.'
            }
        }
        foreach($name in @('PrimaryDirectory','ReplicaDirectory')){
            if($config.$name -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+)'){
                throw 'An absolute archive directory is required.'
            }
        }
        $cycleArgs=@{}
        foreach($name in ($required+$optional)){
            if($config.PSObject.Properties[$name] -and $name -cnotin @('SigningPasswordPath','RecipientPasswordPath')){
                $cycleArgs[$name]=$config.$name
            }
        }
        foreach($name in @('SigningPfxPath','RecipientPfxPath','SigningPasswordPath','RecipientPasswordPath')){
            $keyPath=Assert-PrivatePath $config.$name $false
            [void](Assert-PrivatePath ([IO.Path]::GetDirectoryName($keyPath)) $true)
        }
        $signing=Read-TaskPassword $config.SigningPasswordPath
        $recipient=Read-TaskPassword $config.RecipientPasswordPath
        $cycleArgs.SigningPfxPassword=$signing
        $cycleArgs.RecipientPfxPassword=$recipient
    }catch{
        [Console]::Error.WriteLine('BACKUP_JOB_CONFIGURATION_REFUSED')
        exit 2
    }
    try{
        $result=& (Join-Path $PSScriptRoot 'Invoke-DatabaseBackupCycle.ps1') @cycleArgs
        ConvertTo-Json -InputObject $result -Depth 8 -Compress
    }catch{
        [Console]::Error.WriteLine('BACKUP_JOB_CYCLE_FAILED')
        exit 1
    }
}finally{
    if($signing){$signing.Dispose()}
    if($recipient){$recipient.Dispose()}
}
