# Bounded CMS backup envelope. No certificate-store changes or production key enrollment.
Set-StrictMode -Version Latest
try { Add-Type -AssemblyName System.Security.Cryptography.Pkcs -ErrorAction Stop } catch { Add-Type -AssemblyName System.Security -ErrorAction Stop }
function Get-UsappBackupDigest {
 param([Parameter(Mandatory)][byte[]]$Bytes)
 $hash=[Security.Cryptography.SHA256]::Create()
 try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() } finally { $hash.Dispose() }
}
function Assert-UsappBackupCertificate {
 param([Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,[bool]$Private)
 if(!$Certificate -or ($Private -and !$Certificate.HasPrivateKey)){throw 'Explicit certificate/private key required.'}
 $rsa=[Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($Certificate)
 try {if(!$rsa -or $rsa.KeySize -lt 2048){throw 'RSA certificate of at least 2048 bits required.'}} finally {if($rsa){$rsa.Dispose()}}
}
function Protect-UsappBackupBytes {
 [CmdletBinding()]
 param([Parameter(Mandatory)][byte[]]$Bytes,
       [Parameter(Mandatory)][Security.Cryptography.X509Certificates.X509Certificate2]$SigningCertificate,
       [Parameter(Mandatory)][Security.Cryptography.X509Certificates.X509Certificate2]$RecipientCertificate,
       [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864)
 if($Bytes.Length -eq 0 -or $Bytes.Length -gt $MaximumBytes){throw 'Backup input size refused.'}
 Assert-UsappBackupCertificate $SigningCertificate $true
 Assert-UsappBackupCertificate $RecipientCertificate $false
 $signed=[Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($Bytes),$false)
 $signer=[Security.Cryptography.Pkcs.CmsSigner]::new([Security.Cryptography.Pkcs.SubjectIdentifierType]::IssuerAndSerialNumber,$SigningCertificate)
 $signer.IncludeOption=[Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
 $signer.DigestAlgorithm=[Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1') # SHA-256
 $signed.ComputeSignature($signer,$true)
 $signedBytes=$signed.Encode()
 try {
  if($signedBytes.Length -gt $MaximumBytes+1048576){throw 'Signed backup size refused.'}
  $algorithm=[Security.Cryptography.Pkcs.AlgorithmIdentifier]::new([Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.1.42'),256) # AES-256-CBC
  $envelope=[Security.Cryptography.Pkcs.EnvelopedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($signedBytes),$algorithm)
  $envelope.Encrypt([Security.Cryptography.Pkcs.CmsRecipient]::new($RecipientCertificate))
  $encoded=$envelope.Encode()
  if($encoded.Length -gt $MaximumBytes+2097152){throw 'Encrypted backup size refused.'}
  return ,$encoded
 } finally { [Array]::Clear($signedBytes,0,$signedBytes.Length) }
}
function Unprotect-UsappBackupBytes {
 [CmdletBinding()]
 param([Parameter(Mandatory)][byte[]]$Envelope,
       [Parameter(Mandatory)][Security.Cryptography.X509Certificates.X509Certificate2]$RecipientCertificate,
       [Parameter(Mandatory)][Security.Cryptography.X509Certificates.X509Certificate2]$TrustedSignerCertificate,
       [ValidateRange(1024,268435456)][int]$MaximumBytes=67108864)
 if($Envelope.Length -eq 0 -or $Envelope.Length -gt $MaximumBytes+2097152){throw 'Encrypted backup size refused.'}
 Assert-UsappBackupCertificate $RecipientCertificate $true
 Assert-UsappBackupCertificate $TrustedSignerCertificate $false
 $cms=[Security.Cryptography.Pkcs.EnvelopedCms]::new();$cms.Decode($Envelope)
 if($cms.ContentEncryptionAlgorithm.Oid.Value -ne '2.16.840.1.101.3.4.1.42' -or $cms.RecipientInfos.Count -ne 1){throw 'Backup encryption format refused.'}
 $identity=$cms.RecipientInfos[0].RecipientIdentifier
 if($identity.Type -ne [Security.Cryptography.Pkcs.SubjectIdentifierType]::IssuerAndSerialNumber -or
    $identity.Value.IssuerName -cne $RecipientCertificate.Issuer -or
    $identity.Value.SerialNumber.TrimStart('0') -ine $RecipientCertificate.SerialNumber.TrimStart('0')){throw 'Explicit backup recipient refused.'}
 # Decrypt(extraStore) may also consult the Windows store. Check the explicitly supplied identity first; never accept
 # an unrelated supplied recipient just because the OS happens to contain the actual recipient's private key.
 $recipients=[Security.Cryptography.X509Certificates.X509Certificate2Collection]::new();[void]$recipients.Add($RecipientCertificate)
 $cms.Decrypt($recipients)
 $inner=$cms.ContentInfo.Content
 try {
  if($inner.Length -gt $MaximumBytes+1048576){throw 'Signed backup size refused.'}
  $signed=[Security.Cryptography.Pkcs.SignedCms]::new();$signed.Decode($inner)
  if($signed.Detached -or $signed.SignerInfos.Count -ne 1){throw 'Backup signature format refused.'}
  $signer=$signed.SignerInfos[0]
  if(!$signer.Certificate -or (Get-UsappBackupDigest $signer.Certificate.RawData) -cne (Get-UsappBackupDigest $TrustedSignerCertificate.RawData) -or $signer.DigestAlgorithm.Value -ne '2.16.840.1.101.3.4.2.1'){throw 'Backup signer refused.'}
  # Exact external certificate pin is the trust anchor. No implicit OS/root-chain trust or untrusted manifest pin.
  $signed.CheckSignature($true)
  $plain=$signed.ContentInfo.Content
  if($plain.Length -eq 0 -or $plain.Length -gt $MaximumBytes){throw 'Backup plaintext size refused.'}
  return ,([byte[]]$plain.Clone())
 } finally { [Array]::Clear($inner,0,$inner.Length) }
}
Export-ModuleMember -Function Get-UsappBackupDigest,Protect-UsappBackupBytes,Unprotect-UsappBackupBytes
