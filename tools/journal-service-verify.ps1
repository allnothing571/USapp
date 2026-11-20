# Owned, loopback-only C++ HTTPS -> Python dispatcher -> synthetic publication stores.
param(
 [Parameter(Mandatory=$true)][string]$Compiler,
 [Parameter(Mandatory=$true)][string]$Project,
 [Parameter(Mandatory=$true)][string]$Work,
 [string]$ClosedPostgres='',
 [switch]$Restored,
 [string]$ArchiveSha256='',
 [string]$Attempt='',
 [string]$PythonImage='python@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df'
)
$ErrorActionPreference='Stop'
if($Project -notmatch '^usapp-(e1|backup-probe)-[a-f0-9]{10}$' -or
 [IO.Path]::GetFullPath($Work).TrimEnd('\') -ine [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) $Project)).TrimEnd('\')){
 throw 'Owned project and its exact temporary work directory required.'
}
if($Attempt -and $Attempt -notmatch '^[a-f0-9]{10}$'){throw 'Invalid fixture attempt identifier.'}
if($Attempt){
 $capture=Join-Path $Work ('journal-service-'+$Attempt)
 if(Test-Path -LiteralPath $capture){throw 'Fixture attempt directory already exists.'}
 [void](New-Item -ItemType Directory -Path $capture)
 $Work=$capture
}
$repo=Split-Path -Parent $PSScriptRoot
$utf8=[Text.UTF8Encoding]::new($false)
$runtime=''
$coordinator=''
$fixtureJournal='00000000-0000-0000-0000-00000000002c'
$fixturePause='00000000-0000-0000-0000-00000000002e'
$fixtureAdmission='00000000-0000-0000-0000-00000000002f'
$counter=0
$pki='/tmp/usapp-journal-service-'+$Project
if($Attempt){$pki+='-'+$Attempt}
$hostPki=Join-Path $Work 'journal-service-pki'
$oldTls=$env:USAPP_JOURNAL_TLS_TEST_DIR
function J([string[]]$Arguments){
 $script:counter++
 $log=Join-Path $Work ('journal-service-'+$script:counter.ToString('000')+'.log')
 $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{$lines=@(& docker @Arguments 2>&1 | ForEach-Object{[string]$_});$code=$LASTEXITCODE}finally{$ErrorActionPreference=$old}
 [IO.File]::WriteAllLines($log,[string[]]$lines,$utf8)
 [IO.File]::WriteAllText(($log+'.exit'),[string]$code,$utf8)
 if($code -ne 0){$lines | Select-Object -Last 12 | Write-Output;throw ('Journal service Docker step failed: '+$log)}
 return $lines
}
function Owned([string]$Id){
 if($Id -notmatch '^[0-9a-f]{64}$'){throw 'Exact journal fixture container ID required.'}
 $owner=@(J @('inspect',$Id,'--format','{{index .Config.Labels `usapp.j2b`}}'))
 if($owner.Count -ne 1 -or $owner[0] -cne $Project){throw 'Journal fixture owner mismatch.'}
}
function SourceMatches([string]$Id,[string]$Relative){
 $expected=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo $Relative)).Hash.ToLowerInvariant()
 $actual=@(J @('exec',$Id,'sha256sum',('/workspace/'+$Relative)))
 if($actual.Count -ne 1 -or ($actual[0] -split '\s+')[0] -cne $expected){throw 'Journal fixture source mismatch.'}
}
function FixtureJson([string]$Name){
 # Missing files are represented by null, never by a retry of a journal operation.
 $code="from pathlib import Path; p=Path('$pki/$Name'); print(p.read_text() if p.exists() else 'null')"
 return (@(J @('exec',$runtime,'python3','-B','-c',$code)) -join "`n" | ConvertFrom-Json)
}
function FenceSql([string]$Actor,[string]$Statement){
 return @(J @('exec',$coordinator,'psql','-XqAt','-v','ON_ERROR_STOP=1','-U',$Actor,'-d','usapp_recovery_fence','-c',$Statement))
}
function ExpectFence([string]$Actor,[string]$Method,[string]$Key,[string]$Expected){
 $result=@(FenceSql $Actor "SELECT recovery_fence.$Method('$fixtureJournal','$Key');")
 if($result.Count -ne 1 -or $result[0] -cne $Expected){throw ('Unexpected recovery fence result: '+$Method)}
}
try{
 if($Restored -and (!$ClosedPostgres -or $ArchiveSha256 -cnotmatch '^[a-f0-9]{64}$')){throw 'Restore target and archive digest required.'}
 Owned $Compiler
 SourceMatches $Compiler 'services/api/tests/JournalServiceInteropTest.cc'
 SourceMatches $Compiler 'services/api/src/services/JournalHttpsTransport.cc'
 SourceMatches $Compiler 'services/api/src/services/RecoverySourceBridge.cc'
 SourceMatches $Compiler 'services/api/src/services/RecoverySourceBridge.h'
 SourceMatches $Compiler 'services/api/src/services/DeletionRecovery.h'
 SourceMatches $Compiler 'services/api/src/services/RecoveryAuthority.cc'
 SourceMatches $Compiler 'services/api/src/services/RecoveryAuthority.h'
 SourceMatches $Compiler 'services/api/tests/RecoveryAuthorityFixture.h'
 SourceMatches $Compiler 'services/api/tests/DeletionRecoveryUnitTests.h'
 SourceMatches $Compiler 'services/api/tests/RecoverySourceBridgeUnitTests.h'
 [void](J @('exec',$Compiler,'timeout','15','/workspace/services/api/build-test/tests/usapp_test_journal_service_interop','pki',$pki))
 [void](J @('cp',($Compiler+':'+$pki),$hostPki))
 $env:USAPP_JOURNAL_TLS_TEST_DIR=$hostPki
 $pylog=Join-Path $Work 'journal-service-host-tests.log'
 & python -B -m unittest tests.test_journal_https_server tests.test_journal_backend tests.test_journal_backend_tls tests.test_journal_recovery_reader tests.test_journal_recovery_tls tests.test_j4c_metadata tests.test_deletion_journal_bridge tests.test_deletion_journal_publication tests.test_deletion_journal_batches tests.test_deletion_journal *> $pylog
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $pylog -Tail 60 | Write-Output;throw 'Host journal service tests failed.'}
 Get-Content -LiteralPath $pylog -Tail 4 | Write-Output
 # Shares only the owned compiler's network namespace. No host port, real service or external resource.
 $runtimeName=$Project+'-journal-python'
 if($Attempt){$runtimeName+='-'+$Attempt}
 $runtime=@(J @('run','-d','--pull','never','--network',('container:'+$Compiler),'--name',$runtimeName,'--label',('usapp.j2b='+$Project),'--entrypoint','sleep',$PythonImage,'2400'))[-1]
 Owned $runtime
 if(@(J @('inspect',$runtime,'--format','{{json .HostConfig.PortBindings}}'))[-1] -notin @('null','{}')){throw 'Journal fixture ports forbidden.'}
 [void](J @('exec',$runtime,'mkdir','-p','/workspace/tools','/workspace/tests'))
 foreach($directory in @('tools','tests')){[void](J @('cp',(Join-Path $repo ($directory+'/.')),($runtime+':/workspace/'+$directory)))}
 foreach($file in @('tools/journal_https_server.py','tools/deletion_journal_bridge.py','tools/journal_service_fixture.py','tests/test_journal_https_server.py','tools/journal_backend.py','tools/j4c_metadata.py','tools/j4c_tablestore_sdk.py','tools/j4c_oss_objects.py','tests/test_journal_backend.py','tests/test_journal_backend_tls.py','tests/test_j4c_metadata.py')){SourceMatches $runtime $file}
 foreach($file in @('tools/journal_recovery_reader.py','tests/test_journal_recovery_reader.py','tests/test_journal_recovery_tls.py')){SourceMatches $runtime $file}
 foreach($file in @('tools/journal_writer_fence.py','tests/test_journal_writer_fence.py')){SourceMatches $runtime $file}
 SourceMatches $runtime 'tools/journal_fenced_publisher.py'
 [void](J @('cp',$hostPki,($runtime+':'+$pki)))
 [void](J @('exec',$runtime,'chmod','700',$pki))
 [void](J @('exec',$runtime,'chmod','600',($pki+'/server.key')))
 @(J @('exec',$runtime,'python3','--version')) | Write-Output
 $tests=@(J @('exec','-w','/workspace','-e',('USAPP_JOURNAL_TLS_TEST_DIR='+$pki),$runtime,'python3','-B','-m','unittest','tests.test_journal_https_server','tests.test_journal_backend','tests.test_journal_backend_tls','tests.test_journal_recovery_reader','tests.test_journal_recovery_tls','tests.test_j4c_metadata','tests.test_deletion_journal_bridge','tests.test_deletion_journal_publication','tests.test_deletion_journal_batches','tests.test_deletion_journal'))
 $tests | Select-Object -Last 4 | Write-Output
 if($Restored){
  # This independent authority fixture is reachable only by its own container's Unix socket.
  $coordinator=@(J @('run','-d','--pull','never','--network','none','--tmpfs','/var/lib/postgresql/data',
   '--label',('usapp.j2b='+$Project),'-e','POSTGRES_DB=usapp_recovery_fence','-e','POSTGRES_HOST_AUTH_METHOD=trust','postgres:17'))[-1]
  Owned $coordinator
  if(@(J @('inspect',$coordinator,'--format','{{.HostConfig.NetworkMode}}'))[-1] -cne 'none'){throw 'Fence fixture must be offline.'}
  [void](J @('exec',$coordinator,'timeout','25','sh','-c','until pg_isready -h 127.0.0.1 -U postgres -d usapp_recovery_fence -q; do sleep 1; done'))
  [void](FenceSql 'postgres' 'CREATE ROLE usapp_fence_writer NOLOGIN; CREATE ROLE usapp_fence_authority NOLOGIN; CREATE ROLE fence_publisher LOGIN IN ROLE usapp_fence_writer; CREATE ROLE fence_controller LOGIN IN ROLE usapp_fence_authority;')
  [void](J @('cp',(Join-Path $repo 'tools/recovery-fence/schema.sql'),($coordinator+':/tmp/recovery-fence.sql')))
  [void](J @('exec',$coordinator,'psql','-XqAt','-v','ON_ERROR_STOP=1','-U','postgres','-d','usapp_recovery_fence','-f','/tmp/recovery-fence.sql'))
  [void](FenceSql 'postgres' "INSERT INTO recovery_fence.journals(journal_id) VALUES ('$fixtureJournal'); INSERT INTO recovery_fence.writer_roster VALUES ('$fixtureJournal','fence_publisher');")
  ExpectFence 'fence_publisher' 'admit' $fixtureAdmission 'admitted'
  [void](J @('exec',$runtime,'touch',($pki+'/admission-committed')))
 }
 $fixtureArguments=@('exec','-d','-w','/workspace',$runtime,'python3','-B','-m','tools.journal_service_fixture',$pki)
 if($Restored){$fixtureArguments+=@($fixtureAdmission)}
 [void](J $fixtureArguments)
 $deadline=[DateTime]::UtcNow.AddSeconds(15)
 do{$ready=FixtureJson 'ready.json';if($null -ne $ready){break};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $deadline)
 if($null -eq $ready -or $ready.synthetic_only -ne $true -or $ready.port -lt 1 -or $ready.port -gt 65535){throw 'Journal TLS fixture not ready.'}
 $proof=@(J @('exec',$Compiler,'timeout','30','/workspace/services/api/build-test/tests/usapp_test_journal_service_interop','run',[string]$ready.port,($pki+'/ca.crt')))
 $proof | Where-Object{$_ -match '断言|RESULT|\[FAIL\]'} | Write-Output
 if($ready.recovery_port -lt 1 -or $ready.recovery_port -gt 65535){throw 'Recovery TLS fixture not ready.'}
 $recoveryProof=@(J @('exec',$Compiler,'timeout','30','/workspace/services/api/build-test/tests/usapp_test_journal_service_interop','recover',[string]$ready.recovery_port,($pki+'/ca.crt')))
 $recoveryProof | Where-Object{$_ -match '断言|RESULT|\[FAIL\]'} | Write-Output
 if($ClosedPostgres){
  Owned $ClosedPostgres
  $network=$Project+'_isolated'
  if(@(J @('network','inspect',$network,'--format','{{.Internal}}'))[-1] -cne 'true'){throw 'Recovery drill requires an internal network.'}
  foreach($id in @($Compiler,$ClosedPostgres)){
   $networks=@(J @('inspect',$id,'--format','{{json .NetworkSettings.Networks}}'))[-1] | ConvertFrom-Json
   if(@($networks.PSObject.Properties.Name).Count -ne 1 -or $networks.PSObject.Properties.Name -cne $network){throw 'Recovery drill network mismatch.'}
  }
  if(-not $env:USAPP_TEST_MAINT_CONNINFO){throw 'Disposable maintenance connection missing.'}
  $mode=if($Restored){'recover-restored'}else{'recover-db'}
  $proofArguments=@('exec','-e','USAPP_TEST_MAINT_CONNINFO',$Compiler,'timeout','30','/workspace/services/api/build-test/tests/usapp_test_journal_service_interop',$mode,[string]$ready.recovery_port,($pki+'/ca.crt'))
  if($Restored){
   $fenceTests=@(J @('exec','-w','/workspace',$runtime,'python3','-B','-m','unittest','tests.test_journal_writer_fence','tests.test_journal_fenced_publisher'))
   $fenceTests | Select-Object -Last 4 | Write-Output
   $targetAddress=$networks.PSObject.Properties[$network].Value.IPAddress
   $address=[Net.IPAddress]::None
   if(![Net.IPAddress]::TryParse($targetAddress,[ref]$address) -or
      $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork){throw 'Restore target IPv4 address required.'}
   $issuerTests=Join-Path $Work 'recovery-authority-issuer-tests.log'
   & node --test (Join-Path $repo 'tests/test_recovery_authority_issuer.mjs') *> $issuerTests
   if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $issuerTests -Tail 30 | Write-Output;throw 'Recovery issuer tests failed.'}
   ExpectFence 'fence_controller' 'request_pause' $fixturePause 'draining'
   ExpectFence 'fence_controller' 'confirm_pause' $fixturePause 'draining'
   ExpectFence 'fence_publisher' 'admit' $fixtureAdmission 'refused'
   [void](J @('exec',$runtime,'python3','-B','-c',("from pathlib import Path; Path('$pki/pause-publication').touch()")))
   $drainDeadline=[DateTime]::UtcNow.AddSeconds(8)
   do{
    $drain=FixtureJson 'drain-ready.json'
    if($drain -ceq $true){break}
    Start-Sleep -Milliseconds 100
   }while([DateTime]::UtcNow -lt $drainDeadline)
   if($drain -cne $true){throw 'Publisher did not reconcile its provider effects.'}
   ExpectFence 'fence_controller' 'confirm_pause' $fixturePause 'draining'
   ExpectFence 'fence_publisher' 'confirm_drained' $fixtureAdmission 'drained'
   [void](J @('exec',$runtime,'touch',($pki+'/drain-committed')))
   $pauseDeadline=[DateTime]::UtcNow.AddSeconds(8)
   do{
    $pause=FixtureJson 'pause-latched.json'
    if($null -ne $pause){break}
    Start-Sleep -Milliseconds 100
   }while([DateTime]::UtcNow -lt $pauseDeadline)
   if($pause -cne $true){throw 'Latched publication pause required before confirming drain.'}
   Owned $coordinator
   $externalProof=& node (Join-Path $repo 'tests/recovery-authority-issuer-fixture.mjs') $ClosedPostgres $ArchiveSha256 `
    $coordinator $runtime $pki
   if($LASTEXITCODE -ne 0){throw 'External recovery proof issuance failed.'}
   $externalProof=$externalProof | ConvertFrom-Json
   if($externalProof.enrolledKey -cnotmatch '^[a-f0-9]{64}$' -or $externalProof.signature -cnotmatch '^[a-f0-9]{128}$'){
    throw 'External proof encoding refused.'
   }
   $proofArguments+=@($ClosedPostgres,$ArchiveSha256,$targetAddress,$externalProof.enrolledKey,
    $externalProof.signature,[string]$externalProof.issuedAt,[string]$externalProof.expiresAt)
  }
  $databaseProof=@(J $proofArguments)
  $databaseProof | Where-Object{$_ -match '断言|RESULT|\[FAIL\]'} | Write-Output
  Write-Output 'RECOVERY_SOURCE_POSTGRES_DRILL_PASSED'
  if($Restored){Write-Output 'COORDINATED_PAUSE_CHECKPOINT_ISSUER_INTEROP_PASSED'}
 }
 [void](J @('exec',$runtime,'python3','-B','-c',("from pathlib import Path; Path('$pki/stop').touch()")))
 $deadline=[DateTime]::UtcNow.AddSeconds(10)
 do{$report=FixtureJson 'report.json';if($null -ne $report){break};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $deadline)
 if($null -eq $report -or $report.synthetic_only -ne $true -or $report.serving_allowed -ne $false -or $report.operations -ne 2 -or $report.batches -ne 4 -or $report.closed.network_drained -ne $true -or $report.closed.backend_drained -ne $true){throw 'Journal TLS fixture final proof failed.'}
 if($report.backend -cne 'conditional_metadata_fake' -or $report.closed.resources_drained -ne $true -or $report.preflights -ne 1 -or $report.validator_puts -ne 0){throw 'Journal backend composition final proof failed.'}
 if($report.recovery_closed.network_drained -ne $true -or $report.recovery_closed.backend_drained -ne $true -or $report.recovery_closed.resources_drained -ne $true -or $report.recovery_preflights -ne 1 -or $report.recovery_commits -ne 0 -or $report.recovery_puts -ne 0){throw 'Recovery Source read-only/drain proof failed.'}
 $observations=if($Restored){1}else{0}
 if($report.observer_closed -ne $true -or $report.observer_preflights -ne $observations -or
    $report.observer_commits -ne 0 -or $report.observer_puts -ne 0){throw 'Checkpoint observer read-only/drain proof failed.'}
 if($Restored -and $report.coordinator_drained -ne $true){throw 'Publisher did not consume a committed drain receipt.'}
 [void](J @('cp',($runtime+':'+$pki+'/report.json'),(Join-Path $Work 'journal-service-report.json')))
 Write-Output 'JOURNAL_SERVICE_HTTPS_INTEROP_PASSED; operations=2; batches=4; serving_allowed=false'
 Write-Output 'RECOVERY_SOURCE_HTTPS_INTEROP_PASSED; commits=0; puts=0; serving_allowed=false'
 Write-Output ('JOURNAL_SERVICE_EVIDENCE_DIRECTORY: '+$Work)
}finally{
 $env:USAPP_JOURNAL_TLS_TEST_DIR=$oldTls
 # Delete only this run's synthetic private key, before stopping either owned container. Keep public evidence, never keys.
 $cleanupFailed=$false
 try{if(Test-Path -LiteralPath (Join-Path $hostPki 'server.key')){Remove-Item -LiteralPath (Join-Path $hostPki 'server.key') -Force}}catch{$cleanupFailed=$true}
 foreach($id in @($runtime,$Compiler)){
  if($id){try{Owned $id;[void](J @('exec',$id,'rm','-f','--',($pki+'/server.key')));[void](J @('exec',$id,'test','!','-e',($pki+'/server.key')))}catch{$cleanupFailed=$true}}
 }
 if($runtime){try{Owned $runtime;[void](J @('stop','--time','5',$runtime))}catch{$cleanupFailed=$true}}
 if($coordinator){try{Owned $coordinator;[void](J @('rm','-f',$coordinator))}catch{$cleanupFailed=$true}}
 if($cleanupFailed){throw 'Journal fixture key cleanup or owned-runtime stop failed; inspect this run before retention.'}
}
