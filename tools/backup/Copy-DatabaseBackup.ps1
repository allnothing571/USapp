# Copy one encrypted archive using an independently supplied digest.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256,
    [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$inputFile=[IO.Path]::GetFullPath($InputPath)
$outputFile=[IO.Path]::GetFullPath($OutputPath)
if([IO.Path]::GetExtension($inputFile) -cne '.usbackup' -or
   [IO.Path]::GetExtension($outputFile) -cne '.usbackup'){
    throw 'Both paths must use the .usbackup extension.'
}
if($inputFile.Equals($outputFile,[StringComparison]::OrdinalIgnoreCase) -or
   (Test-Path -LiteralPath $outputFile)){
    throw 'The replica requires a new output path.'
}
$item=Get-Item -LiteralPath $inputFile
if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){
    throw 'The source must be a regular archive file.'
}
$parent=[IO.Path]::GetDirectoryName($outputFile)
if(!(Test-Path -LiteralPath $parent -PathType Container)){
    throw 'The replica directory must already exist.'
}

$maximumEnvelope=[long]$MaximumBytes+2097152
$temporary=Join-Path $parent ('.usapp-replica-'+[guid]::NewGuid().ToString('N')+'.partial')
$source=$null
$destination=$null
$owned=$false
$sha=[Security.Cryptography.SHA256]::Create()
try{
    $source=[IO.File]::Open($inputFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    if($source.Length -eq 0 -or $source.Length -gt $maximumEnvelope){
        throw 'The encrypted archive size is outside the configured bounds.'
    }
    $expectedLength=$source.Length
    $destination=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $owned=$true
    $buffer=[byte[]]::new(65536)
    $copied=0L
    while(($length=$source.Read($buffer,0,$buffer.Length)) -gt 0){
        if($copied+$length -gt $expectedLength){
            throw 'The archive size changed while copying.'
        }
        $destination.Write($buffer,0,$length)
        $copied+=$length
    }
    if($copied -ne $expectedLength){throw 'The archive ended before the expected size.'}
    $destination.Flush($true)
    $destination.Position=0
    $digest=([BitConverter]::ToString($sha.ComputeHash($destination))).Replace('-','').ToLowerInvariant()
    if($digest -ine $ExpectedSha256){throw 'The replica digest differs from the expected archive digest.'}
    $destination.Dispose()
    $destination=$null
    # The same-directory move is the publication point. The two-argument overload refuses overwrite.
    [IO.File]::Move($temporary,$outputFile)
    $owned=$false
    [pscustomobject]@{path=$outputFile; bytes=$copied; encrypted_sha256=$digest}
}finally{
    if($destination){$destination.Dispose()}
    if($source){$source.Dispose()}
    $sha.Dispose()
    if($owned){[IO.File]::Delete($temporary)}
}
