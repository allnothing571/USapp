# Read-only external-journal recovery material (2026-11-16)

Subsequent C++ Source consumer is now locally verified with real recovery HTTPS and coordinator context guards. Dated
reader-only scope below is historical; production main/active-authority/release and actual Source/Postgres drill remain
open. See [current consumer proof](recovery-source-bridge.md).

## Scope and trust boundary

`tools/journal_recovery_reader.py` adds a dedicated recovery reader, read-only restricted SDK facades and authenticated
recovery-frame dispatcher, hosted through the existing private TLS service/lifecycle owner. It reads the external
TableStore journal partition and immutable objects, **not the restored PostgreSQL database**. No writer credential,
data initialization, repair, publication, database access or serving-release method exists in this component.

This is **not yet a C++ `deletionrecovery::Source` adapter** or production startup/release wiring. The external resource,
identity, TLS/SDK authentication and fresh/authoritative metadata-read contracts still require real evidence. The reader
does not create a separate checkpoint authority, prove a provider cannot roll back history, or attest that an operator's
supplied restore incarnation is current. Context pins must come from an independently controlled restore procedure,
never solely a database name, restored backup row or arbitrary submitted plan. No fallback accepts those as authority.

The existing startup refusal remains unchanged. Contract **0.60.0**, **54** frozen migrations through 0053, next **0054**.
No general production configuration: existing Shanghai/trial-resource restrictions and network-blocking defaults remain.

## Read-only path and consistency

The programmatic `RecoveryBackend` accepts one explicit recovery-reader credential; it does not accept publication or
checkpoint-writer credentials. Identity syntax/configuration binding is checked, not effective IAM ownership/permissions.
Real least-privilege policy and independent key custody remain required; code-level restrictions are not an IAM audit.

The restricted table transport permits only **DescribeTable, StartLocalTransaction, AbortTransaction, GetRow, GetRange**.
Commit, PutRow and UpdateRow are excluded even at the facade's underlying transport. The facade also rejects commit=true,
insert and both advance methods before I/O. Object adapter permits GET only; put and private PUT requests are refused
before signing/network calls. Metadata commit is refused. Start/Abort are read-transaction administration, not claims
that every network request is HTTP GET or that an unknown remote transaction end was proven.

First valid request uses the existing worker-owned schema/transaction preflight and failure latch. No listener start
performs adapter I/O. A missing existing head stays unknown; no initialization or empty-journal inference is attempted.

Each snapshot/current-head call:

1. Uses a fresh metadata read, whose existing adapter brackets all index scans with full current checkpoint reads.
2. Reads all referenced immutable objects and validates v2 batch envelopes, unchanged J1 logical chain/cycle semantics,
   complete operation/cycle indexes and checkpoint binding using the existing publication validator's read-only path.
3. Detaches the validated snapshot and reads the **entire current metadata again** after object validation. Any change
   to the complete checkpoint, key list or indexes refuses the result, even if a reported head alone stayed equal.
4. Returns verified material only inside the configured 0.05–10 s reader budget (default 2 s). Checks before/after each
   adapter operation prevent starting the next operation once a preceding call consumed the budget. They cannot interrupt
   a synchronous call or bound each internal SDK RPC/DNS/CPU pause. HTTP and reader budgets are separate; timed-out work
   retains the existing worker/resources until it returns. There is no hard process/remote-execution deadline claim.

Both methods validate the complete chain; `current_head` does not merely trust an unvalidated head row. Limits remain
the prototype Publisher/Metadata safeguards (bounded history/rows/bytes), not a production scalability design. Observed
head consistency is not a forever-fresh lease: writers may advance after the last read. The later C++ coordinator must
recheck the current head after staging, and a separate quiet-window/incarnation/release protocol remains mandatory.

## Projection and authenticated wire

Dedicated MAC domain: HMAC-SHA256 over ASCII key bytes and
`usapp-journal-recovery-v1:request\n` or `usapp-journal-recovery-v1:response\n` plus the exact body bytes.
This is separate from publication's MAC domain, including when a test deliberately reuses a literal key. Operational
reader/publication keys should still be independently controlled. TLS confidentiality and the existing frame/connection
limits apply. No new public application API or OpenAPI route is introduced.

Request exact fields: `version=1`, `journal_id`, `database_id`, `attempt_id`, `nonce`, `method` (`snapshot`/`current_head`).
The three ids must match the configured context, nonce is 32 lowercase hex characters, and JSON duplicates/extra fields
are refused. Wrong scope/method/shape or MAC fails **before preflight/adapter reads**. Consumers must generate a fresh
nonce and verify the response MAC, raw request digest, nonce, context, shape and full checkpoint; the server is not a
nonce replay cache. The C++ consuming adapter is not implemented in this batch.

Response exact fields: `version`, all three context ids, `nonce`, `request_sha256`, `state`, `checkpoint`, `snapshot`,
`serving_allowed=false`. `verified` includes a full v2 checkpoint; unknown includes null checkpoint/snapshot, never an
empty success or missing/not-found outcome. Every response remains non-serving: that flag is a protocol prohibition on
release, **not an observation that a database is currently CLOSED**. Reader never queries a database state.

Snapshot has the existing C++/Postgres staging shape: `head`, `hidden`, `purge_barriers`, `unresolved`:

- `head.seq/hash` use the **v2 batch sequence/hash**, not just the logical event head; the separately included full
  checkpoint binds both. This preserves operation-envelope changes even if logical events looked the same.
- Targets are complete derived sets, ordered like C++ Kind (memory, then media) and id; no allowlist or content fields.
- `unresolved` contains **per-subject J1 intent ids**, matching existing staging semantics, not J2 operation ids.
- Uncertain deletion and cleanup/purge barriers retain the original conservative validator semantics. Current-head
  replies omit snapshot material. An over-65536-byte reply refuses the **whole** snapshot with empty 503; no truncation,
  dropped barrier, pagination or partially trusted result.

MAC/context binding cannot prove an operator chose the active incarnation. No statement is signed or interpreted as
permission to reopen PostgreSQL, enable media/purge or override the existing startup gate.

## Ownership and failure behavior

Recovery backend reuses one-listener ownership and bounded network/publication/resource shutdown. It additionally
leases the Reader's direct programmatic calls: close returns undrained without clearing clients while a read is active,
then closes admission and marks Reader closed before resource teardown. Later direct reads return unknown. This avoids
protecting only HTTP calls while silently tearing down a direct validation flight. Existing startup/cleanup limits from
[backend composition](journal-backend-composition.md) remain; local cleanup never proves a remote transaction rollback.

Shared TLS hosting now accepts an explicit abstract bounded-frame interface. Publication and recovery dispatchers
implement it independently; publication protocol/MAC/methods are unchanged. There is no duck-typed environment factory
or switch accepting arbitrary submitted trusted recovery plans.

## Final verification

Final scoped attempt **`0d87082007`**, fresh PKI/capture/runtime on the previously owned compiler:

- Host Python **3.8.7** and pinned Python **3.12.16** each passed **149 tests**: all previous 125 plus **18 recovery-core**
  and **6 real loopback recovery-TLS** cases. Coverage includes populated/zero/missing snapshots, delete/aborted/cleanup
  semantics, subject-intent mapping/batch head, object/index faults, head/index movement, whole-frame refusal, wrong
  context/MAC domain, deadline behavior, serialization and both HTTP/direct-read resource leases.
- Real pinned SDK offline collection **188 reported, 4 skipped**, exit 0 (**184 executed**). Four new reader tests
  exercised actual request/response codecs with in-memory responses, read-only API/transport denial, reader principal,
  missing head and failed signature. All socket connect/create_connection/DNS are blocked in these new SDK tests.
  Four skipped cases are the existing opt-in vendor-client local-TLS tests whose separate fixture was not supplied.
- Existing actual **C++ publication HTTPS** interop still passed **14 assertions**, two operations/four batches, one
  preflight, validator PUT 0 and three drains true. **This is not C++ recovery-Source interop**; recovery TLS cases are
  Python clients/server with conditional metadata fakes. The C++ client helper/binary and app code were unchanged/reused
  after source-binding checks. Prior full CTest 36/36 and HTTP1465 are historical, not rerun/reclaimed this batch.
- Two added negative controls each failed as intended before correction: an expired first read still started a second
  dependency (2 vs 1 calls), and a direct read lease allowed backend close to claim success. Final guards pass both
  without weakening assertions. Initial scoped `bf97436c73` passed 148 before the direct-read lease correction and is
  retained as intermediate evidence only. The final 149 run and SDK collection used the final source.
- Final host/runtime source bindings matched; synthetic keys removed and absence checked, created runtime/compiler
  stopped again. Database container stayed stopped; no database observation/write this batch. Its data mount is tmpfs:
  the previous test's CLOSED result is historical evidence, **not retained database contents or a restartable restore
  fixture**. Docker removes tmpfs data when the container stops ([official storage semantics](https://docs.docker.com/engine/storage/tmpfs/)).
  Use a new owned database for the next drill. All 54 migration bytes rechecked unchanged. No C++/frontend/schema/grant/
  application-contract change.

Evidence allowlist: `Claude outputs/journal-recovery-reader-20261116`: summaries/report, source bindings/hashes and
negative-control conclusions. No credentials, private keys, environments, raw API/PostgreSQL logs or backups. Existing
temporary pinned SDK site reused; no dependency download/global install. No cloud resource access or old trial resume.

## Next required work

Implement/verify the C++ Source consumer, pin independently controlled active restore context and current external
checkpoint with real identity/ownership/read guarantees, then exercise staging/current-head/fence behavior against
isolated PostgreSQL while CLOSED. Production transport/credentials/hosting, full J3c/J4/J5 object+journal restore drill,
release proof and consolidated browser/device/provider acceptance remain open. No commit or deployment.
