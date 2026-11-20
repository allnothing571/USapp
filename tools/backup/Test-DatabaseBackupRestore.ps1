# Explicit independent, non-serving restore probe. Never opens service, relaxes a journal hold or changes a source DB.
[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$InputPath,
 [Parameter(Mandatory)][string]$TrustedSignerCertificatePath,
 [Parameter(Mandatory)][string]$RecipientPfxPath,
 [Parameter(Mandatory)][Security.SecureString]$RecipientPfxPassword,
 [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{64}$')][string]$ContainerId,
 [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]{1,64}$')][string]$OwnerLabelKey,
 [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')][string]$ExpectedOwner,
 [Parameter(Mandatory)][ValidatePattern('^usapp_restore_[a-z0-9_]{1,40}$')][string]$Database,
 [ValidatePattern('^[0-9a-f]{64}$')][string]$ExpectedArchiveSha256,
 [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864
)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'BackupCrypto.psm1') -Force
$dockerExecutable=(Get-Command docker.exe -CommandType Application -ErrorAction Stop).Source
$stage='';$containerStage='';$recipient=$null;$signer=$null;$dump=''
function Docker([string[]]$Arguments){$old=$ErrorActionPreference;$ErrorActionPreference='Continue';try{$result=@(& $dockerExecutable @Arguments 2>&1|ForEach-Object {[string]$_});$exit=$LASTEXITCODE}finally{$ErrorActionPreference=$old};if($exit){throw ('Restore operation refused/failed (exit '+$exit+'); raw database/key output withheld.')};return $result}
function Query([string]$Sql){return @(Docker @('exec','-u','postgres',$ContainerId,'psql','-X','-w','-At','-v','ON_ERROR_STOP=1','-U','postgres','-d',$Database,'-c',$Sql))}
function Assert-Target {
 $id=@(Docker @('inspect',$ContainerId,'--format','{{.Id}}'));$label=@(Docker @('inspect',$ContainerId,'--format',('{{index .Config.Labels `'+$OwnerLabelKey+'`}}')))
 if($id.Count -ne 1 -or $id[0] -cne $ContainerId -or $label.Count -ne 1 -or $label[0] -cne $ExpectedOwner){throw 'Restore target ownership refused.'}
 $ports=@(Docker @('inspect',$ContainerId,'--format','{{json .HostConfig.PortBindings}}'))
 if($ports.Count -ne 1 -or $ports[0] -notin @('null','{}')){throw 'Restore target must have no published ports.'}
 $net=@(Docker @('inspect',$ContainerId,'--format','{{json .NetworkSettings.Networks}}'))
 $networks=($net -join '')|ConvertFrom-Json;$properties=@($networks.PSObject.Properties)
 if($properties.Count -eq 0){throw 'An explicitly isolated target network is required.'}
 foreach($property in $properties){$networkId=$property.Value.NetworkID;if($networkId -notmatch '^[0-9a-f]{64}$' -or @(Docker @('network','inspect',$networkId,'--format','{{.Internal}}'))[0] -cne 'true'){throw 'Restore target network is not internal.'}}
 if(@(Query 'SELECT current_database()')[0] -cne $Database){throw 'Restore database identity refused.'}
}
try {
 Assert-Target
 $empty=@(Query "SELECT (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname !~ '^pg_' AND n.nspname<>'information_schema')+(SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname !~ '^pg_' AND n.nspname<>'information_schema')+(SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid())")
 if($empty.Count -ne 1 -or $empty[0] -cne '0'){throw 'Restore target must be empty and unused; no overwrite/drop/clean fallback.'}
 # These roles must be provisioned separately with trusted fresh credentials. Never import source role passwords.
 if(@(Query "SELECT count(*) FROM pg_roles WHERE rolname IN('usapp_api','usapp_push','usapp_memory_eraser')")[0] -cne '3'){throw 'Explicit trusted target roles required.'}
 $unsafe=@(Query "SELECT (SELECT count(*) FROM pg_roles r WHERE r.rolname IN('usapp_api','usapp_push') AND (r.rolsuper OR r.rolcreatedb OR r.rolcreaterole OR r.rolbypassrls OR EXISTS(SELECT 1 FROM pg_auth_members m WHERE m.member=r.oid)))+(SELECT count(*) FROM pg_database WHERE datname=current_database() AND pg_get_userbyid(datdba)<>'postgres')")
 if($unsafe[0] -cne '0'){throw 'Application privilege inheritance or target database ownership refused.'}
 $path=[IO.Path]::GetFullPath($InputPath)
 if((Get-Item -LiteralPath $path).Length -gt $MaximumBytes+2097152){throw 'Encrypted archive size refused.'}
 $archiveBytes=[IO.File]::ReadAllBytes($path)
 if($archiveBytes.Length -gt $MaximumBytes+2097152){throw 'Encrypted archive size refused.'}
 if($ExpectedArchiveSha256){
  $sha=[Security.Cryptography.SHA256]::Create()
  try{$actualDigest=[BitConverter]::ToString($sha.ComputeHash($archiveBytes)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
  if($actualDigest -cne $ExpectedArchiveSha256){throw 'Encrypted archive digest mismatch.'}
 }
 [void](Query ('REVOKE CONNECT ON DATABASE '+$Database+' FROM PUBLIC,usapp_api,usapp_push'))
 $recipient=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.Path]::GetFullPath($RecipientPfxPath),$RecipientPfxPassword,[Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
 $signer=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.Path]::GetFullPath($TrustedSignerCertificatePath))
 $plain=Unprotect-UsappBackupBytes $archiveBytes $recipient $signer -MaximumBytes $MaximumBytes
 $archiveBytes=$null
 if($plain.Length -lt 5 -or [Text.Encoding]::ASCII.GetString($plain,0,5) -cne 'PGDMP'){throw 'Authenticated payload is not a custom PostgreSQL archive.'}
 $root=[IO.Path]::GetFullPath($env:LOCALAPPDATA);$tag=[guid]::NewGuid().ToString('N');$stage=[IO.Path]::GetFullPath((Join-Path $root ('usapp-db-restore-'+$tag)))
 if($root.StartsWith('\\') -or !$stage.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $stage)){throw 'Private restore staging refused.'}
 [void](New-Item -ItemType Directory -Path $stage)
 $acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false);$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User;$acl.SetOwner($sid);$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'));Set-Acl -LiteralPath $stage -AclObject $acl
 $dump=Join-Path $stage 'database.pgcustom';[IO.File]::WriteAllBytes($dump,$plain);[Array]::Clear($plain,0,$plain.Length)
 $containerStage='/tmp/usapp-db-restore-'+$tag
 [void](Docker @('exec','-u','postgres',$ContainerId,'mkdir','-m','700','--',$containerStage))
 [void](Docker @('cp',$dump,($ContainerId+':'+$containerStage+'/database.pgcustom')))
 [void](Docker @('exec',$ContainerId,'chown','postgres:postgres',($containerStage+'/database.pgcustom')))
 Assert-Target
 [void](Docker @('exec','-u','postgres',$ContainerId,'timeout','300','pg_restore','-w','-U','postgres','-d',$Database,'--exit-on-error','--single-transaction',($containerStage+'/database.pgcustom')))
 # Retain the restored object owners and ACL. Leave DB connection disabled for application/public roles.
 [void](Query ('REVOKE CONNECT ON DATABASE '+$Database+' FROM PUBLIC,usapp_api,usapp_push'))
 $repo=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
 # Target bootstrap default privileges may differ from object ACL baselines assumed by pg_restore. Reapply the reviewed
 # current tightening script atomically, with CONNECT revoked again before the same transaction becomes visible.
 $roles=Join-Path $repo 'database/init-roles.sql';[void](Docker @('cp',$roles,($ContainerId+':'+$containerStage+'/init-roles.sql')))
 [void](Docker @('exec','-u','postgres',$ContainerId,'psql','-X','-w','-v','ON_ERROR_STOP=1','-U','postgres','-d',$Database,'--single-transaction','-f',($containerStage+'/init-roles.sql'),'-c',('REVOKE CONNECT ON DATABASE '+$Database+' FROM PUBLIC,usapp_api,usapp_push')))
 $migrations=@(Get-ChildItem -LiteralPath (Join-Path $repo 'database/migrations') -Filter '*.sql')
 $rows=@(Query "SELECT version||'|'||checksum FROM schema_migrations ORDER BY version")
 if($rows.Count -ne $migrations.Count){throw 'Restored migration count differs from this reviewed checkout.'}
 foreach($row in $rows){$parts=$row.Split('|');if($parts.Count -ne 2 -or $parts[0] -notmatch '^[0-9]{4}_[a-z0-9_]+$'){throw 'Restored migration identity refused.'};$file=Join-Path $repo ('database/migrations/'+$parts[0]+'.sql');if(!(Test-Path -LiteralPath $file) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -cne $parts[1]){throw 'Restored schema checksum mismatch.'}}
 if(@(Query 'SELECT count(*) FROM usapp_memory_eraser_violations()')[0] -cne '0'){throw 'Restored erasure role/ownership deviations.'}
 $ledger="SELECT count(*) FROM (SELECT seq,amount,balance_after,lag(seq) OVER w AS pseq,lag(balance_after) OVER w AS prev FROM wallet_ledger_entries WINDOW w AS(PARTITION BY user_id,asset_type ORDER BY seq)) x WHERE seq<>COALESCE(pseq,0)+1 OR balance_after<>COALESCE(prev,0)+amount OR balance_after<0"
 if(@(Query $ledger)[0] -cne '0'){throw 'Restored financial chain inconsistent.'}
 $matrix=Join-Path $repo 'database/tests/task_privilege_deviation.sql';[void](Docker @('cp',$matrix,($ContainerId+':'+$containerStage+'/matrix.sql')))
 $deviations=@(Docker @('exec','-u','postgres',$ContainerId,'psql','-X','-w','-At','-v','ON_ERROR_STOP=1','-U','postgres','-d',$Database,'-f',($containerStage+'/matrix.sql')))
 if($deviations.Count){throw 'Restored application privilege deviations.'}
 if(@(Query "SELECT has_database_privilege('usapp_api',current_database(),'CONNECT') OR has_database_privilege('usapp_push',current_database(),'CONNECT')")[0] -cne 'f'){throw 'Application connection gate is not closed.'}
 Write-Output ('DATABASE_RESTORE_PROBE_PASSED migrations='+$rows.Count+'; no application connection; DB-only; journal/object replay and release acceptance still required')
 if($ExpectedArchiveSha256){Write-Output ('DATABASE_RESTORE_ARCHIVE_BOUND '+$actualDigest)}
} finally {
 if($containerStage -match '^/tmp/usapp-db-restore-[0-9a-f]{32}$'){
  try{Assert-Target;foreach($name in @('database.pgcustom','matrix.sql','init-roles.sql')){[void](Docker @('exec',$ContainerId,'rm','-f','--',($containerStage+'/'+$name)))};[void](Docker @('exec','-u','postgres',$ContainerId,'rmdir','--',$containerStage))}catch{Write-Warning ('Private restore container staging may remain: '+$containerStage)}
 }
 if($dump -and (Test-Path -LiteralPath $dump)){Remove-Item -LiteralPath $dump -Force};if($stage -and (Test-Path -LiteralPath $stage)){Remove-Item -LiteralPath $stage -Force}
 if($recipient){$recipient.Dispose()};if($signer){$signer.Dispose()}
}
