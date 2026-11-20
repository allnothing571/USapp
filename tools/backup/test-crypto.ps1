# Pure local tests: ephemeral in-memory keys, no certificate-store writes, files, Docker or database calls.
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'BackupCrypto.psm1') -Force
function New-TestCertificate {
 $key=[Security.Cryptography.RSA]::Create(2048)
 try {
  $request=[Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=usapp-backup-ephemeral-test',$key,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
  return $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-1),[DateTimeOffset]::UtcNow.AddHours(1))
 } finally {$key.Dispose()}
}
function Refuses([scriptblock]$Action,[string]$Label){$failed=$false;try{[void](& $Action)}catch{$failed=$true};if(!$failed){throw ('Accepted invalid input: '+$Label)};$script:checks++}
$a=New-TestCertificate;$b=New-TestCertificate;$wrong=New-TestCertificate;$checks=0
try {
 $plain=[Text.Encoding]::UTF8.GetBytes('PGDMP disposable synthetic payload '+('abc'*1000))
 $sealed=Protect-UsappBackupBytes $plain $a $b
 $restored=Unprotect-UsappBackupBytes $sealed $b $a
 if((Get-UsappBackupDigest $plain) -cne (Get-UsappBackupDigest $restored)){throw 'Round trip mismatch'};$checks++
 $another=Protect-UsappBackupBytes $plain $a $b
 if((Get-UsappBackupDigest $sealed) -ceq (Get-UsappBackupDigest $another)){throw 'Encryption repeated IV/ciphertext'};$checks++
 Refuses {Unprotect-UsappBackupBytes $sealed $wrong $a} 'wrong decryption key'
 Refuses {Unprotect-UsappBackupBytes $sealed $b $wrong} 'untrusted signer'
 $tampered=[byte[]]$sealed.Clone();$tampered[[int]($tampered.Length/2)]=$tampered[[int]($tampered.Length/2)] -bxor 1
 Refuses {Unprotect-UsappBackupBytes $tampered $b $a} 'tampered ciphertext'
 Refuses {Unprotect-UsappBackupBytes ([byte[]](1,2,3)) $b $a} 'malformed envelope'
 Refuses {Protect-UsappBackupBytes $plain $a $b -MaximumBytes 1024} 'input cap'
 Refuses {Unprotect-UsappBackupBytes $sealed $b $a -MaximumBytes 1024} 'plaintext cap'
 $public=[Security.Cryptography.X509Certificates.X509Certificate2]::new($a.RawData)
 try {Refuses {Protect-UsappBackupBytes $plain $public $b} 'missing signing key'}finally{$public.Dispose()}
 Write-Output ('BACKUP_CRYPTO_PASSED checks='+$checks+'; ephemeral only; no provider or restore acceptance')
} finally {$a.Dispose();$b.Dispose();$wrong.Dispose()}
