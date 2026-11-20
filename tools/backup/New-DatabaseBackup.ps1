# Explicit owner-run DB-only backup. Does not back up object bytes, roles, secret env files or grant production approval.
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
 [Parameter(Mandatory)][string]$OutputPath,
 [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864
)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'BackupCrypto.psm1') -Force
$dockerExecutable=(Get-Command docker.exe -CommandType Application -ErrorAction Stop).Source
$stage='';$containerStage='';$ownedFiles=@();$signing=$null;$recipient=$null;$published=$false
function Docker([string[]]$Arguments){
 $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{$result=@(& $dockerExecutable @Arguments 2>&1 | ForEach-Object {[string]$_});$exit=$LASTEXITCODE}finally{$ErrorActionPreference=$old}
 if($exit -ne 0){throw ('Backup Docker operation refused/failed (exit '+$exit+'); no raw database/credential output emitted.')}
 return $result
}
function Assert-Source {
 $full=@(Docker @('inspect',$ContainerId,'--format','{{.Id}}'))
 $label=@(Docker @('inspect',$ContainerId,'--format',('{{index .Config.Labels `'+$OwnerLabelKey+'`}}')))
 $running=@(Docker @('inspect',$ContainerId,'--format','{{.State.Running}}'))
 if($full.Count -ne 1 -or $full[0] -cne $ContainerId -or $label.Count -ne 1 -or $label[0] -cne $ExpectedOwner -or $running[0] -cne 'true'){throw 'Exact source identity/ownership/running state refused.'}
}
try {
 $out=[IO.Path]::GetFullPath($OutputPath)
 if([IO.Path]::GetExtension($out) -cne '.usbackup' -or (Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath ($out+'.json'))){throw 'A new .usbackup output path is required; existing files are never replaced.'}
 $parent=[IO.Path]::GetDirectoryName($out);if(!(Test-Path -LiteralPath $parent -PathType Container)){throw 'Output parent must already exist.'}
 $signing=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.Path]::GetFullPath($SigningPfxPath),$SigningPfxPassword,[Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
 $recipient=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.Path]::GetFullPath($RecipientPfxPath),$RecipientPfxPassword,[Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
 Assert-Source
 $identity=@(Docker @('exec','-u','postgres',$ContainerId,'psql','-X','-w','-At','-U',$DatabaseUser,'-d',$Database,'-c','SELECT current_database()'))
 if($identity.Count -ne 1 -or $identity[0] -cne $Database){throw 'Database identity refused.'}
 $localRoot=[IO.Path]::GetFullPath($env:LOCALAPPDATA)
 if(!$localRoot -or $localRoot.StartsWith('\\')){throw 'Private local staging required.'}
 $tag=[guid]::NewGuid().ToString('N');$stage=[IO.Path]::GetFullPath((Join-Path $localRoot ('usapp-db-backup-'+$tag)))
 if(!$stage.StartsWith($localRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $stage)){throw 'Staging path refused.'}
 [void](New-Item -ItemType Directory -Path $stage)
 $acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false)
 $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
 $acl.SetOwner($sid);$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))
 Set-Acl -LiteralPath $stage -AclObject $acl
 $dump=Join-Path $stage 'database.pgcustom';$ownedFiles+=@($dump)
 $containerStage='/tmp/usapp-db-backup-'+$tag
 [void](Docker @('exec','-u','postgres',$ContainerId,'mkdir','-m','700','--',$containerStage))
 Assert-Source
 # Both work duration and output file size are bounded on the source, before host copy or whole-CMS buffering.
 # Required existing utilities fail closed; no tool installation or shell interpolation.
 # Retain object ownership/ACL: stripping them would turn the erasure SECURITY DEFINER function into a superuser-owned
 # function on a maintenance restore. Global roles/passwords remain outside pg_dump and require explicit trusted setup.
 [void](Docker @('exec','-u','postgres',$ContainerId,'timeout','300','prlimit',('--fsize='+$MaximumBytes+':'+$MaximumBytes),'--','pg_dump','-w','-U',$DatabaseUser,'-d',$Database,'-Fc','-f',($containerStage+'/database.pgcustom')))
 $size=@(Docker @('exec',$ContainerId,'stat','-c','%s',($containerStage+'/database.pgcustom')))
 if($size.Count -ne 1 -or $size[0] -notmatch '^[1-9][0-9]{0,9}$' -or [long]$size[0] -gt $MaximumBytes){throw 'Dump byte bound refused.'}
 Assert-Source
 [void](Docker @('cp',($ContainerId+':'+$containerStage+'/database.pgcustom'),$dump))
 if((Get-Item -LiteralPath $dump).Length -gt $MaximumBytes){throw 'Host dump byte bound refused.'}
 $bytes=[IO.File]::ReadAllBytes($dump)
 if($bytes.Length -lt 5 -or [Text.Encoding]::ASCII.GetString($bytes,0,5) -cne 'PGDMP'){throw 'Custom PostgreSQL archive required.'}
 $encrypted=Protect-UsappBackupBytes $bytes $signing $recipient -MaximumBytes $MaximumBytes
 $check=Unprotect-UsappBackupBytes $encrypted $recipient $signing -MaximumBytes $MaximumBytes
 try{if((Get-UsappBackupDigest $bytes) -cne (Get-UsappBackupDigest $check)){throw 'Local decrypt/signature/hash verification failed.'}}finally{[Array]::Clear($check,0,$check.Length)}
 # Creation proves local cryptographic recovery. Only an independent database restore drill can validate the dump itself.
 $manifest=[ordered]@{format='usapp-db-cms-v1';created_utc=[DateTime]::UtcNow.ToString('o');database=$Database;dump_bytes=$bytes.Length;dump_sha256=Get-UsappBackupDigest $bytes;encrypted_sha256=Get-UsappBackupDigest $encrypted;signer_certificate_sha256=Get-UsappBackupDigest $signing.RawData;object_bytes_included=$false;restore_verified=$false}
 # CreateNew avoids replacement even if another process won after the earlier existence check.
 $stream=[IO.File]::Open($out,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
 try{$stream.Write($encrypted,0,$encrypted.Length)}finally{$stream.Dispose()}
 $published=$true
 $text=$manifest|ConvertTo-Json -Depth 4
 $stream=[IO.File]::Open($out+'.json',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
 try{$encoded=[Text.UTF8Encoding]::new($false).GetBytes($text);$stream.Write($encoded,0,$encoded.Length)}finally{$stream.Dispose()}
 Write-Output ('DATABASE_BACKUP_CREATED encrypted='+$out+'; DB-only; restore_verified=false; external signer certificate pin required')
} finally {
 # Each path was created by this exact invocation. No recursive deletion, globbing or broad container selection.
 if($containerStage -match '^/tmp/usapp-db-backup-[0-9a-f]{32}$'){
  try{Assert-Source;[void](Docker @('exec','-u','postgres',$ContainerId,'rm','-f','--',($containerStage+'/database.pgcustom')));[void](Docker @('exec','-u','postgres',$ContainerId,'rmdir','--',$containerStage))}catch{Write-Warning ('Private container staging may remain: '+$containerStage)}
 }
 foreach($file in $ownedFiles){if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file -Force}}
 if($stage -and (Test-Path -LiteralPath $stage)){Remove-Item -LiteralPath $stage -Force}
 if($signing){$signing.Dispose()};if($recipient){$recipient.Dispose()}
 if($published -and !(Test-Path -LiteralPath ($out+'.json'))){Write-Warning 'Encrypted artifact exists without its informational manifest; it has not been restore-verified.'}
}
