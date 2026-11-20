# Read-only retention inventory for a single database in an operator-controlled archive directory.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z_][A-Za-z0-9_]{0,62}$')][string]$Database,
    [Parameter(Mandatory)][ValidateRange(0,366)][int]$KeepDailyDays,
    [Parameter(Mandatory)][ValidateRange(0,104)][int]$KeepWeeklyWeeks,
    [ValidateRange(1,365)][int]$MinimumArchives=2,
    [DateTimeOffset]$AsOf=[DateTimeOffset]::UtcNow
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Get-Item -LiteralPath $Directory
if(!$root.PSIsContainer -or ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)){
    throw 'Select a regular backup directory.'
}
$now=$AsOf.UtcDateTime
$day=$now.Date
$monday=$day.AddDays(-(([int]$day.DayOfWeek+6)%7))
$firstDay=$day.AddDays(1-$KeepDailyDays)
$firstWeek=$monday.AddDays(7*(1-$KeepWeeklyWeeks))
$maxEnvelope=268435456L+2097152

function Get-Monday([DateTime]$Date){
    return $Date.Date.AddDays(-(([int]$Date.DayOfWeek+6)%7))
}

function Read-Manifest([string]$Path){
    $item=Get-Item -LiteralPath $Path
    if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'Manifest must be a regular file.'
    }
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        if($stream.Length -lt 1 -or $stream.Length -gt 16384){throw 'Manifest size refused.'}
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$false)
        try{$content=$reader.ReadToEnd()}finally{$reader.Dispose()}
        if(!$content.TrimStart().StartsWith('{')){throw 'Manifest object required.'}
        if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')){
            return ConvertFrom-Json -InputObject $content -DateKind String
        }
        return ConvertFrom-Json -InputObject $content
    }finally{
        $stream.Dispose()
    }
}

$archives=@(Get-ChildItem -LiteralPath $root.FullName -Force | Where-Object { $_.Extension -ceq '.usbackup' })
$entries=@(foreach($archive in $archives){
    $entry=[pscustomobject]@{
        archive=$archive.Name
        status='review'
        reasons=@()
        created_utc=$null
        encrypted_sha256=$null
    }
    try{
        if($archive.PSIsContainer -or ($archive.Attributes -band [IO.FileAttributes]::ReparsePoint)){
            throw 'archive-not-regular'
        }
        try{$manifest=Read-Manifest ($archive.FullName+'.json')}catch{throw 'manifest-unreadable'}
        if($manifest -isnot [pscustomobject] -or
           !$manifest.PSObject.Properties['format'] -or $manifest.format -cne 'usapp-db-cms-v1' -or
           !$manifest.PSObject.Properties['database'] -or $manifest.database -isnot [string] -or
           $manifest.database -cnotmatch '^[A-Za-z_][A-Za-z0-9_]{0,62}$'){
            throw 'manifest-invalid'
        }
        if($manifest.database -cne $Database){
            $entry.status='outside-scope'
            $entry.reasons=@('other-database')
        }else{
            $created=[DateTimeOffset]::MinValue
            if(!$manifest.PSObject.Properties['created_utc'] -or $manifest.created_utc -isnot [string] -or
               $manifest.created_utc -cnotmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,7})?(Z|\+00:00)$' -or
               ![DateTimeOffset]::TryParse($manifest.created_utc,[Globalization.CultureInfo]::InvariantCulture,
                   [Globalization.DateTimeStyles]::None,[ref]$created)){
                throw 'timestamp-invalid'
            }
            if($created -gt $AsOf){throw 'timestamp-in-future'}
            if(!$manifest.PSObject.Properties['encrypted_sha256'] -or $manifest.encrypted_sha256 -isnot [string] -or
               $manifest.encrypted_sha256 -cnotmatch '^[0-9a-f]{64}$'){
                throw 'digest-invalid'
            }
            $stream=[IO.File]::Open($archive.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            try{
                if($stream.Length -lt 1 -or $stream.Length -gt $maxEnvelope){throw 'archive-size-refused'}
                $digest=(Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
            }finally{
                $stream.Dispose()
            }
            if($digest -ine $manifest.encrypted_sha256){throw 'archive-digest-mismatch'}
            $entry.created_utc=$created.UtcDateTime.ToString('o',[Globalization.CultureInfo]::InvariantCulture)
            $entry.encrypted_sha256=$manifest.encrypted_sha256
            $entry.status='eligible'
        }
    }catch{
        $reason=$_.Exception.Message
        $entry.reasons=@(if($reason -cin @('archive-not-regular','manifest-unreadable','manifest-invalid',
            'timestamp-invalid','timestamp-in-future','digest-invalid','archive-size-refused','archive-digest-mismatch')){
            $reason
        }else{'archive-unreadable'})
    }
    $entry
})

$eligible=@($entries | Where-Object status -eq 'eligible' |
    Sort-Object @{Expression='created_utc';Descending=$true}, archive)
$days=@{}
$weeks=@{}
$newest=@{}
foreach($entry in $eligible){
    $date=[DateTimeOffset]::ParseExact($entry.created_utc,'o',[Globalization.CultureInfo]::InvariantCulture).UtcDateTime.Date
    $week=Get-Monday $date
    $digest=$entry.encrypted_sha256
    if($newest.ContainsKey($digest) -or $newest.Count -lt $MinimumArchives){
        $newest[$digest]=$true
        $entry.reasons+= 'newest-distinct-archives'
    }
    if($KeepDailyDays -gt 0 -and $date -ge $firstDay -and !$days.ContainsKey($date)){
        $days[$date]=$true
        $entry.reasons+= 'daily'
    }
    if($KeepWeeklyWeeks -gt 0 -and $week -ge $firstWeek -and !$weeks.ContainsKey($week)){
        $weeks[$week]=$true
        $entry.reasons+= 'weekly'
    }
    if($entry.reasons.Count){$entry.status='keep'}else{
        $entry.status='candidate'
        $entry.reasons=@('outside-retention-selection')
    }
}

[pscustomobject]@{
    database=$Database
    as_of_utc=$now.ToString('o',[Globalization.CultureInfo]::InvariantCulture)
    keep_daily_days=$KeepDailyDays
    keep_weekly_weeks=$KeepWeeklyWeeks
    minimum_distinct_archives=$MinimumArchives
    entries=@($entries | Sort-Object archive)
}
