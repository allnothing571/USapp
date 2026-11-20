# Database backup tools — current local proof, not release clearance

2026-11-15 continuation. Owner-approved minimum V1 operations work; no production key enrollment, scheduling, off-site
upload, live database access or deployment was performed. Object bytes and independent deletion-journal recovery remain
outside this database-only tool. Never open a restored service merely because this probe passed.

## Implemented surface

- `tools/backup/New-DatabaseBackup.ps1`: explicitly selected full container ID, expected ASCII owner label, database and
  database user; checks actual identity before read. No broad container selection, environment/password extraction,
  interactive DB authentication or implicit production defaults. Uses existing postgres OS user, pg_dump, timeout and
  prlimit; missing utilities or authentication fail closed rather than install/weaken anything.
- `tools/backup/BackupCrypto.psm1`: SignedCms inside EnvelopedCms; SHA-256 signature, AES-256-CBC, RSA certificates at least
  2048 bits. Explicit recipient identity and exact external signer certificate pin; no certificate/root-store enrollment.
  The external trusted signer file is the trust anchor, not the archive's adjacent JSON or its embedded certificate.
- `tools/backup/Test-DatabaseBackupRestore.ps1`: explicit new `usapp_restore_*` target, expected owner, internal network,
  no published ports, empty and unused DB, separately provisioned trusted roles. Refuses privileged/inherited application
  roles and application-owned DBs. Confirms signature and byte cap before plaintext is written. No drop, clean, overwrite,
  automatic fallback, release opening or journal-hold removal.
- `tools/backup/test-crypto.ps1`: pure local ephemeral key tests. `tools/backup-verify.ps1`: fresh owned source and independent
  restore PostgreSQL 17 containers, generated credentials, internal-only network and synthetic financial fixture.

Source dump work is limited to 300 seconds and the explicit file-size cap (default 64 MiB; maximum 256 MiB), before host
copy. CMS operations buffer multiple representations; these caps do not promise a fixed process-memory multiplier or
forensic erasure of managed memory. Plaintext staging is a new restricted-ACL LocalAppData folder and a postgres-owned
0700 container folder. Cleanup names only files created by that invocation; it never recursively clears unrelated paths.
A crash can leave private staging; inspect the reported exact owned path before manual cleanup.

Only a new `.usbackup` encrypted artifact plus informational `.usbackup.json` is published after a local decrypt/signature/
hash check. Existing output is never replaced. Manifest `restore_verified=false` is intentional: successful encryption
is not a database restore. A later probe reports separate evidence and does not silently rewrite original facts.

## Ownership, permissions and restore gate

Keep object ownership and ACL in the dump. Stripping ownership on a superuser restore can make the erasure SECURITY
DEFINER function superuser-owned. Global roles/passwords are not in pg_dump: provision trusted fresh application/push
roles and the restricted NOLOGIN eraser role independently before this tool. Missing/unsafe roles are refused.

Target bootstrap default privileges can leave permissions beyond the source's ACL baseline. The reviewed checkout's
`database/init-roles.sql` is reapplied in one transaction, with application/public CONNECT revoked again before commit.
This transaction never publishes its intermediate CONNECT grant. The probe then checks this checkout's exact numbered
migration hashes, eraser role/ownership violations, financial ledger sequence/balance integrity, the complete application
privilege matrix and actual API/push CONNECT denial. It leaves the DB non-serving and does not attach an API.

Do not reapply init-roles later without restoring the connection gate: ordinary init-roles intentionally grants CONNECT.
Even a database-level gate is not the independent deletion journal's replay/verification barrier. DB and object snapshot
reconciliation, replay, provider guarantees and J5 restore clearance require separate actual proof.

## Operator inputs and custody

Create requires `ContainerId`, `OwnerLabelKey`, `ExpectedOwner`, `Database`, `DatabaseUser`, explicit `SigningPfxPath`,
`SigningPfxPassword` (SecureString), `RecipientPfxPath`, `RecipientPfxPassword` (SecureString) and a new `OutputPath`.
Passwords can be collected with Read-Host -AsSecureString in the operator's terminal; do not put literal passwords in
command history or source. PFX files/private keys must not be included with off-site archive copies. Never send private
keys or passwords to this chat. Certificate pin and key recovery need independent custody, not trust in a copied manifest.

Restore requires `InputPath`, a separately trusted `TrustedSignerCertificatePath`, recipient PFX/password and the explicit
isolated target identity. The target must be empty; use a new instance, not a live or shared DB. Expected output is
`DATABASE_RESTORE_PROBE_PASSED`; this does not authorize application access. The caller remains responsible for keeping
the restored instance isolated and recording the full DB+object/journal recovery evidence.

No scheduled task, rotation deletion, certificate installation or off-site transport has been configured. Frequency,
retention, private-key recovery and the real destination require a concrete operational review before live activation.

## Evidence

Pure crypto: 9 checks passed on both PowerShell 7 and Windows PowerShell 5.1, including wrong key/signer, malformed or
changed ciphertext, missing key, size refusals and randomized encryption. Repeated after explicit recipient pinning.

First incomplete b1b0bd699b run had no retained terminal error; it is not a pass. Code inspection found a case-insensitive
PowerShell helper-name collision (`Docker` calling `docker`), corrected by invoking the resolved docker.exe explicitly.
52a5727fb4 failed before resource creation because Docker was stopped. d88900d8f8 successfully encrypted and restored but
failed the final permission matrix; the reviewed init-roles tightening/revoked-CONNECT transaction was added.

Run **usapp-backup-probe-700d0818d1** passed: 52 exact migration hashes, erasure/financial/privilege checks, actual synthetic
1000-flower correction restored, nonempty target and 1024-byte source cap refused. Generated test private keys removed;
containers stopped and retained. No real data/providers/keys. A subsequent application-owner/inheritance guard and actual
CONNECT assertion are under a fresh final verification run and not yet claimed passed here.

Final run **usapp-backup-probe-b413dcc15f** passed the completed tools, including refusal of an application-owned empty
restore DB and explicit final API/push CONNECT denial. Source remains unchanged by the backup; only the owned target is
restored. Synthetic financial fixture restored, schema/eraser/chain/privileges verified; source cap and nonempty-target
refusals passed. Both containers stopped and retained; ephemeral test PFX/private keys removed. No source or dump secrets
are copied into repository evidence. Recorded output: Claude outputs/backup-probe-latest.log. Scheduling, key custody,
off-site copy and database+object/journal recovery remain pending live operations work.

Subsequent personal-security batch: usapp-backup-probe-d555d4355a passed the same checks on all 53 migrations through 0052,
including a restored no-space login notice and NULL-space read receipt exactly once, alongside the financial fixture.
The updated current harness is tools/backup-verify.ps1. This does not backfill notifications on restore or authorize API
access; all release/object/journal/real key-custody/scheduling gates above remain unchanged.


Memory-clock follow-up: `usapp-backup-probe-f02a7deef0` passed the same synthetic independent restore through all **54**
migrations (0053). In addition to prior financial/personal-notice checks, the restored decision column has no default and
the restored function retains the causal clock assignment. Application CONNECT remains denied. Test keys removed;
containers stopped/retained. No new scheduling, off-site, real-key, object/journal or live release acceptance.

## Combined database, journal and object rehearsal

Run `tools/backup-verify.ps1 -JournalDrill` from the repository in PowerShell with the existing disposable
compiler, PostgreSQL and Python images available. The harness backs up a database containing two synthetic
memories with media bindings, saves the matching synthetic object bytes and their SHA-256 digests, and restores
the signed and encrypted database archive to a separate container.

The recovered database initially contains both records. While application and worker connections are denied,
the C++ recovery coordinator reads the later deletion through the Python HTTPS (Hypertext Transfer Protocol
Secure) reader. It closes the restored database, stages the quarantine and records the verified journal head.
The harness then uses the restored media bindings and quarantine to decide which object to copy into a fresh
directory. It checks the copied bytes against the backup digest, confirms that the deleted object's original
bytes are still in the backup and absent from the restore output, and rechecks both database gates.

This rehearsal uses an external journal fixture and local synthetic object copies. Production storage restore,
object version retention, credentials, and permission to serve require separate verification.

The 2026-11-18 run `usapp-backup-probe-ddbb0618a2` exited 0. The restored database passed all 10 dedicated
assertions. The backup contained two objects, the journal withheld one, and the remaining object's digest
matched the restored file. The recovery and publication HTTPS checks passed 21 and 14 assertions respectively.
Both Python environments passed 149 tests. The backup harness verified all 54 migration checksums and
retained the existing independent restore and refusal checks. The first run stopped before creating a backup
because a synthetic media idempotency key was too short. The fixture key was corrected and the final run
used fresh containers. Evidence is in `Claude outputs/joint-restore-20261118`.

## Copy an encrypted archive to a second destination

Run `tools/backup/Copy-DatabaseBackup.ps1` with the source `-InputPath`, a new `-OutputPath` in an existing
destination directory, and `-ExpectedSha256`. Use the `encrypted_sha256` recorded by the trusted backup
producer. If the source has crossed a trust boundary, obtain the expected digest from a separately trusted
record rather than accepting the sidecar supplied with the source.

The command streams the encrypted bytes into a temporary file next to the destination, flushes that file,
checks its SHA-256 digest, and publishes it with a same-directory move that refuses replacement. It copies
only the archive. It needs no signing or decryption key, and the restore tool still checks the archive
against its pinned signer. `-MaximumBytes` uses the backup tool's plaintext cap plus the existing 2 MiB
envelope allowance for the encrypted file.

Use an operator-controlled destination directory. Local Windows filesystem behavior is covered by tests.
The actual off-site destination still needs verification for publication semantics, access control,
retention and a recovery read from that destination.

On 2026-11-18, the copy tests passed all 8 checks on Windows PowerShell 5.1 and PowerShell 7.
Run `usapp-backup-probe-8a7d6f2726` restored from the replica in a fresh PostgreSQL instance,
verified all 54 migrations and checked that application roles could not connect.
The evidence is in `Claude outputs/backup-replica-20261118`.

## Review archive retention

`tools/backup/Get-DatabaseBackupRetentionPlan.ps1` produces a read-only report for one database
in an operator-controlled directory. Set the daily and weekly windows explicitly. For example, this
lists a seven-day window and a four-week window for review.

```powershell
& tools/backup/Get-DatabaseBackupRetentionPlan.ps1 -Directory 'D:\Backups\USapp' -Database 'usapp' -KeepDailyDays 7 -KeepWeeklyWeeks 4 | ConvertTo-Json -Depth 5
```

Each window includes the current UTC period. Weeks start on Monday. Each populated period contributes
its newest archive. A minimum of two distinct encrypted archive digests is kept by default when available,
along with all copies of those digests. Override the floor with `-MinimumArchives` and reproduce a historical
plan with `-AsOf`. Setting a daily or weekly window to zero disables that part of the rule.

The report labels entries `keep`, `candidate`, `review` or `outside-scope`. Missing manifests, digest mismatches
and invalid or future timestamps require review. Scanning is limited to archive files directly in the selected
directory. Each candidate still needs an operator decision based on restore evidence, the required recovery
window and any retention holds. The sidecar timestamp guides selection, and the digest check establishes
consistency with that sidecar. The pinned signer and recoverability checks belong to the independent restore.
The report makes no file changes.

On 2026-11-19, Windows PowerShell 5.1 and PowerShell 7 passed the 12-entry fixture, including
UTC calendar boundaries, digest mismatch, missing metadata, duplicate archives and unchanged input bytes.
Run `usapp-backup-probe-265229b877` inventoried a newly generated encrypted archive and then restored
its replica in a fresh PostgreSQL instance. The harness checked 54 migrations and exited 0.

## Run one backup cycle

`tools/backup/Invoke-DatabaseBackupCycle.ps1` is the entry point for an operator or a scheduler.
Pass the same container, database and certificate parameters as `New-DatabaseBackup.ps1`
except `-OutputPath`. Supply existing `-PrimaryDirectory` and `-ReplicaDirectory` directories, plus
`-KeepDailyDays` and `-KeepWeeklyWeeks`. The default floor retains the two latest distinct archive digests.

The cycle holds an exclusive lock file handle scoped to the primary directory and database. Keep the
lock file in place between runs. All scheduled instances for that database should use the same primary
directory. The cycle generates a unique archive name, verifies the encrypted replica and reports
retention for the primary directory. It returns the two paths, digest, byte count and retention plan.
Run an independent restore to establish recoverability. The cycle always reports `restore_verified=false`.

A replica error fails the cycle and leaves the primary archive in place. Use the producer sidecar's trusted
digest with `Copy-DatabaseBackup.ps1` to retry publication of that archive. The retention report is advisory.
Schedule registration, the machine identity and certificate password retrieval depend on the deployment environment.

On 2026-11-19, run `usapp-backup-probe-1c6b34475c` held the cycle lock and confirmed that a second call
refused before publishing either archive. After releasing the lock, the harness published both archives,
checked the primary retention report and restored the replica into a fresh PostgreSQL container.
All 54 migration checksums and application connection denial passed. Windows PowerShell 5.1 and
PowerShell 7 both refused a primary/replica directory alias in a separate parameter check.

## Run unattended on Windows

Run the scheduler action with an absolute script path and configuration path.

~~~powershell
pwsh.exe -NoProfile -NonInteractive -File "D:\JYJ\USapp\tools\backup\Start-DatabaseBackupJob.ps1" -ConfigPath "C:\Private\USappBackup\job.json"
~~~

The JSON object uses the backup cycle's fields. Replace the two SecureString parameters with
`SigningPasswordPath` and `RecipientPasswordPath`. The following is an example with illustrative paths,
retention windows and a container ID to replace.

~~~json
{
  "ContainerId": "replace-with-the-full-container-id",
  "OwnerLabelKey": "usapp.backup",
  "ExpectedOwner": "replace-with-the-source-owner-label",
  "Database": "usapp",
  "DatabaseUser": "postgres",
  "SigningPfxPath": "C:\\Private\\USappBackup\\signer.pfx",
  "SigningPasswordPath": "C:\\Private\\USappBackup\\signer.dpapi",
  "RecipientPfxPath": "C:\\Private\\USappBackup\\recipient.pfx",
  "RecipientPasswordPath": "C:\\Private\\USappBackup\\recipient.dpapi",
  "PrimaryDirectory": "D:\\Backups\\USapp",
  "ReplicaDirectory": "E:\\Replica\\USapp",
  "KeepDailyDays": 7,
  "KeepWeeklyWeeks": 4
}
~~~

Create each password file as the Windows account that will run the task, on the intended backup machine.
For example, prompt for the signer password and write its encrypted Windows Data Protection API (DPAPI)
representation to a file. Repeat for the recipient certificate. [Microsoft PowerShell reference](https://learn.microsoft.com/powershell/module/microsoft.powershell.security/convertfrom-securestring)

~~~powershell
Read-Host 'Signer certificate password' -AsSecureString | ConvertFrom-SecureString | Set-Content -LiteralPath 'C:\Private\USappBackup\signer.dpapi'
~~~

The task account must own the configuration, password and certificate files and their immediate parent
directories. Allowed access-control principals are the task account, SYSTEM and local Administrators.
Select a trusted local parent path and preserve the task account's Windows profile. Test the action
with the actual task identity before enabling its trigger. After a container recreation, update the full
container ID after independently verifying the replacement identity.

Exit code 0 carries a JSON backup and retention report on standard output. Exit code 2 indicates that
configuration or credential loading was refused. Exit code 1 indicates that the cycle failed.
Failure output uses fixed markers and omits credential and configuration values.

On 2026-11-19, run `usapp-backup-probe-e9bea557a7` exercised the entry point as a separate
non-interactive PowerShell process. A plaintext password fixture returned 2 without echoing the
fixture. A held cycle lock returned 1 and published zero archives. The valid configuration returned
0, published both archives and produced the retention report. Independent PostgreSQL restore from
the replica passed all 54 migration checksum checks and application connection denial.

## Check backup freshness and replica integrity

Run the health check independently of the backup task. Choose a maximum age based on the intended
backup frequency and the time needed to finish a run.

~~~powershell
pwsh.exe -NoProfile -NonInteractive -File "D:\JYJ\USapp\tools\backup\Test-DatabaseBackupHealth.ps1" -PrimaryDirectory "D:\Backups\USapp" -ReplicaDirectory "E:\Replica\USapp" -Database usapp -MaximumAgeHours 30
~~~

Exit code 0 means the primary inventory has no review entries, its newest valid archive is within
the configured age, and the replica with the same filename matches its encrypted digest.
Exit code 1 returns a JSON report that identifies the issue, such as a stale archive, a missing replica
or a digest mismatch. Exit code 2 indicates the probe could not read the selected directories or
produce a report. Run outside the expected backup publication window to avoid transient warnings
while the primary, sidecar and replica are being published.

The probe reads the archive inventory and hashes files. Schedule it with that storage cost in mind.
Freshness uses the producer sidecar's UTC timestamp, and the report keeps `restore_verified=false`.
Use a pinned signer and an independent database restore to check recoverability.

On 2026-11-19, seven health checks passed on both Windows PowerShell 5.1 and PowerShell 7.
Run `usapp-backup-probe-367158c52a` passed health inspection of a newly generated archive pair and
then restored that replica into a fresh PostgreSQL instance. The harness checked 54 migration hashes
and verified that application roles could not connect.

## Move an approved retention candidate to review

After reviewing the retention plan, create a regular directory named `.usapp-retention-review`
inside the primary backup directory. Pass one candidate's exact archive filename and digest to
`tools/backup/Move-DatabaseBackupToReview.ps1`, along with the same database, replica directory and
retention windows. Use `-WhatIf` to preview the destination, then `-Confirm` for the reviewed move.
The preview shows the intended action. Eligibility is checked again under the backup cycle lock
when executing the move.

The move requires a matching replica and keeps at least two distinct primary archive digests.
Outstanding primary inventory issues, changed retention eligibility, digest mismatch and existing
review files cause refusal. The review archive and sidecar retain their filenames and bytes.
The replica stays in place. The review directory is on the same storage as the primary and this
step does not reclaim space. Keep it under the same access controls as the primary directory.

The archive and sidecar use two filesystem moves. If the archive move fails, the command attempts
to put the sidecar back. After an interruption, inspect the two directories and reunite a split
pair before continuing. To undo a completed move, stop competing backup jobs and return both files
only when their original paths are free. Permanent deletion requires a separate operator decision.

On 2026-11-19, PowerShell 7 and Windows PowerShell 5.1 passed nine synthetic checks each. Coverage
included preview, retention floor, wrong digest, wrong replica, destination collision, unresolved
inventory entries, competing backup lock, file preservation and the remaining archive count.

## Rehearse a database upgrade from migration 0024

Run `tools/backup-verify.ps1 -UpgradeFrom0024` in PowerShell with the existing isolated PostgreSQL
image available. The harness installs the first 25 frozen migrations in a fresh database using the
standard migration runner. It seeds a couple, session, message and reply, and a wallet operation
with ledger data. Before upgrade, it records a fingerprint of their common fields.

The same runner then applies migrations 0025 through 0053 and the target role setup. After a repeat
apply, the harness checks the fixture's fingerprint and runs the target privilege, ledger and backup
checks. It restores a fresh encrypted replica into a second PostgreSQL instance and checks the legacy
fixture again with application connections denied.

This rehearsal establishes schema migration, preservation of its synthetic fixture, and recovery of
the upgraded backup. To exercise the actual API build pairing on the disposable database, also pass
`-RollbackImage usapp-api:release-0024`. This mode builds the current API and uses the selected
local image by immutable ID. It probes health, anonymous wallet refusal, and the legacy fixture's
wallet and messages in the candidate, rollback, and restored-candidate stages. All probes run on
container loopback, with scheduling, media, and purge disabled.

On 2026-11-19, a read-only check of the running local LAN stack matched all 25 installed migration
names and checksums through `0024_push_dispatch_repairs`. The running API used image
`sha256:3ffd2142b565ccf1040ba96deb07e3b56e7206b2bfee73ea23ce13ac566a9da6`
and answered its health endpoint. Mail was in log mode and push was off. This check covered
the local LAN stack only.

Run `usapp-backup-probe-8adc4daf48` passed all three API stages on the same upgraded database.
The current API binary's SHA-256 was
`7432e625acdcf58b93c5b580961bf35b36347a5a38e32d6312527e3f2b1f9090`.
The synthetic legacy fixture fingerprint stayed stable across the three stages. Earlier runs
stopped on harness configuration, a missing container HTTP client, and PowerShell argument
grouping. The probe now uses a separate local Python container in the target's loopback namespace.
This covers the API pairing with media and scheduled work off. Album rollback, application write
compatibility, and any rollback after real production writes still require separate evidence.

On 2026-11-19, `usapp-backup-probe-698d8d2d8e` verified all 25 baseline checksums and all
54 target checksums. The synthetic fixture digest matched after the upgrade, repeat apply and
replica restore. Privilege deviations were zero. The fresh restore continued to deny application
connections. The harness exited 0. A prior invocation stopped before allocating any test
container because the local Docker engine was down. Evidence is in `Claude outputs/upgrade-0024-20261119`.
