$ErrorActionPreference='Stop'
$copy=Join-Path $PSScriptRoot 'Copy-DatabaseBackup.ps1'
$work=Join-Path ([IO.Path]::GetTempPath()) ('usapp-copy-test-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $work)
$source=Join-Path $work 'source.usbackup'
$output=Join-Path $work 'replica.usbackup'
$refused=Join-Path $work 'refused.usbackup'
$large=Join-Path $work 'large.usbackup'
$empty=Join-Path $work 'empty.usbackup'
$checks=0
function Assert-Refused([scriptblock]$Action,[string]$Expected){
    $errorText=''
    try{[void](& $Action)}catch{$errorText=$_.Exception.Message}
    if($errorText -notlike ('*'+$Expected+'*')){throw ('Expected refusal: '+$Expected)}
    if(Test-Path -LiteralPath $refused){throw 'Failed copy published an archive.'}
    if(@(Get-ChildItem -LiteralPath $work -Filter '*.partial' -Force).Count){throw 'Failed copy left staging files.'}
    $script:checks++
}
try{
    [IO.File]::WriteAllBytes($source,[byte[]]::new(150000))
    $expected=(Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
    $result=& $copy -InputPath $source -OutputPath $output -ExpectedSha256 $expected
    if($result.bytes -ne 150000 -or $result.encrypted_sha256 -ine $expected -or
       (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash -cne $expected){throw 'Replica digest or byte count mismatch.'}
    $checks++
    Assert-Refused {& $copy -InputPath $source -OutputPath $output -ExpectedSha256 $expected} 'new output path'
    Assert-Refused {& $copy -InputPath $source -OutputPath $source -ExpectedSha256 $expected} 'new output path'
    Assert-Refused {& $copy -InputPath $source -OutputPath $refused -ExpectedSha256 ('f'*64)} 'digest differs'
    [IO.File]::WriteAllBytes($large,[byte[]]::new(2098177))
    Assert-Refused {& $copy -InputPath $large -OutputPath $refused -ExpectedSha256 $expected -MaximumBytes 1024} 'size is outside'
    [IO.File]::WriteAllBytes($empty,[byte[]]@())
    Assert-Refused {& $copy -InputPath $empty -OutputPath $refused -ExpectedSha256 $expected} 'size is outside'
    Assert-Refused {& $copy -InputPath $source -OutputPath (Join-Path $work 'missing/new.usbackup') -ExpectedSha256 $expected} 'must already exist'
    if((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -cne $expected -or
       (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash -cne $expected){throw 'A refusal modified an existing archive.'}
    $checks++
    Write-Output ('BACKUP_COPY_TESTS_PASSED checks='+$checks)
}finally{
    foreach($file in @($source,$output,$refused,$large,$empty)){
        if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file}
    }
    Remove-Item -LiteralPath $work
}
