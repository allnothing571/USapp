# V1 backup implementation plan and checkpoint

Owner-run operational tooling, not a web endpoint that executes arbitrary host/container commands. Implement DB backup,
encrypted/signed archives, retention and independent restore verification; database-only proof does not close photo/journal
restore gates. Source provider/device/production access and deployment remain separately reviewable final steps.

Proposed Windows implementation uses built-in .NET CMS (SignedCms inside EnvelopedCms), SHA-256 signature and AES-256-CBC
encryption. One explicit operator certificate/private key; decryption pins the exact trusted signer certificate and verifies
the inner signature before exposing or restoring a dump. No hand-rolled cipher, dependency installation, certificate-store
mutation or trusted-root installation. Test certificate may be ephemeral/in-memory, never production enrollment.

pg_dump custom format provides a consistent database snapshot. Exact full container id plus expected single ASCII label,
database and user are required; verify actual current_database before dumping. No broad container selection or implicit
production defaults. Do not read or print .env, passwords or all container environment fields. Use local noninteractive
database authentication, fail rather than prompt or weaken authentication.

Stage plaintext only in a per-run private LocalAppData directory with restricted NTFS ACL, not in the requested (possibly
synced/off-site) output folder. Use an explicit archive-size/memory cap for CMS buffering; reject before unbounded reads.
Publish only authenticated encrypted artifact plus nonsecret manifest after a successful local decrypt/hash check. Clean
only exact files created by that run; no recursive deletion or unrelated resource cleanup. A crash may leave private
plaintext staging, which must be reported and safely cleaned by an ownership-scoped recovery command.

The outer manifest is informative, not an independent trust anchor. Cryptographic provenance must come from the pinned
signature, and schema/consistency verification must inspect the restored database, not assume a separately read metadata
query shares the pg_dump snapshot. Backups exclude object bytes and server/global role secrets; restore roles and frozen
migrations explicitly through existing init-roles/deviation checks.

Restore verification must create a new isolated, non-serving database/container, refuse nonempty targets and not attach
an API. Keep application roles disconnected until the recovery/journal proof is established. Verify schema checksums and
ledger/deletion invariants; never mark production open or relax CLOSED. Scheduling, rotation, private-key custody,
encrypted off-site storage and a DB+object/J5 drill need actual operator/environment evidence, not just script existence.

Implementation checkpoint: tools/backup/BackupCrypto.psm1 implements bounded SignedCms inside EnvelopedCms, exact external
signer certificate pinning, SHA-256 and AES-256-CBC. Pure local test-crypto.ps1 passed 9 checks under both Windows PowerShell
5.1 and PowerShell 7: round trip, randomized envelopes, wrong key/signer, altered/malformed input, byte caps and missing
signing key. Ephemeral memory-only certificates; no store mutation or production enrollment. This is a crypto primitive
proof, not a database backup, restore or provider acceptance. Whole CMS operations buffer multiple representations; byte
caps do not promise a fixed process-memory multiplier or forensic erasure of managed copies.

New-DatabaseBackup.ps1 is now drafted (syntax parsed only): exact source ownership/database checks, private ACL staging,
source work/file bounds, retained object owners/ACL, signature+decrypt/hash self-check and CreateNew encrypted publication.
Object owners/ACL must be retained: stripping them on a superuser restore would incorrectly transfer the erasure
SECURITY DEFINER function to postgres. Global role/password setup stays separate, explicit and trusted. Execution,
independent restore verification and retention tooling are still pending. Existing
docs/backup-restore.md correctly says no implemented/verified database backup script at this checkpoint.

Latest checkpoint 2026-11-15: this earlier plan's pending implementation is superseded by
[database-backup-tools.md](database-backup-tools.md). Signed/encrypted create and isolated non-serving restore are now
implemented and passed in usapp-backup-probe-b413dcc15f, including role/ownership, CONNECT denial, actual financial data,
nonempty-target and byte-bound proofs. All limits beyond DB-only tools (real custody, scheduling/retention, off-site and
DB+object/journal recovery) remain open. Historical draft and failed attempts above are retained as dated development
states, not current completion claims.
