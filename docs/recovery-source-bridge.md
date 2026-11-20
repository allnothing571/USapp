# C++ authenticated recovery Source (2026-11-16)

## PostgreSQL integration drill (2026-11-18)

The backend harness runs the PostgreSQL drill after the ordinary tests and the export closure case. It creates
the database at the start of the run, checks ownership and internal network membership, and supplies the
disposable maintenance connection to the test process. The test requires an already closed database and uses
the actual HTTPS Source, Python reader, coordinator, and PostgresStore.

The matched attempt must persist a manifest, the expected quarantine entry, and the same batch head reported
by the external reader. A second attempt with the wrong context must leave a newer generation without a
manifest or verification, and PostgreSQL must reject writes through the earlier generation. The harness then
checks that the application still refuses startup against the closed database.

Run `usapp-e1-8cf11fb637` passed all 17 PostgreSQL drill assertions, all 36 CTest targets (103.88 s),
and 1,465 API assertions. Publication and recovery HTTPS passed 14 and 21 assertions respectively.
Python passed 149 tests in each environment. The final startup probe reported `recovery_closed`
and confirmed that the listeners stayed closed. All 54 migration checksums matched and the
harness exited 0. The compact drill output, regression summary and fixture shutdown report are
in `Claude outputs/recovery-postgres-20261118`.

The journal uses synthetic objects and metadata. The quarantine target is a synthetic identifier. The drill
covers the authenticated staging and fence path. Restoring a database backup and reconciling actual object
contents remain release requirements.

## Implemented scope

`services/api/src/services/RecoverySourceBridge.{h,cc}` implements the existing `deletionrecovery::Source` interface
against the Python read-only recovery protocol. It accepts explicit journal/database/attempt pins, MAC key and bounded
Exchange (normally the existing HTTPS transport). No credential/environment/config loader, production factory, main
wiring, database reopen or serving-release method is added. The API startup refusal remains unchanged.

The caller must independently control the reader endpoint/key and active restore-context pins and establish the
reader's external current-checkpoint/complete-verification contract. An authenticated `verified` reply delegates the
chain/index validation to that configured reader; the C++ adapter verifies the message and projection, **not the full
journal bytes again**. It cannot prove resource ownership, effective IAM, no provider rollback, active incarnation or
future head stability merely from a MAC. Read [reader trust limits](journal-recovery-reader.md).

Contract **0.60.0**, **54** frozen migrations through 0053, next **0054**. No migration, grant, dependency or public route.

## Wire validation and failure behavior

- Separate `usapp-journal-recovery-v1` request/response MAC domains; ASCII 64-hex key bytes as in Python. No publication
  MAC fallback. Default nonce is 16 cryptographically random bytes encoded as 32 lowercase hex characters; injected
  nonce generation is a programmatic test seam, not an environment option or production replay-safe alternative.
- Each call binds the full context, nonce and exact raw request SHA-256 digest. Wrong queried journal/invalid nonce is
  refused before Exchange. Constructor rejects malformed pins/key/missing Exchange. `snapshot` and `current_head` are
  the only emitted methods. No publication/repair/initialization request is available.
- A reply must be 200, <=65536 body bytes, correct lowercase MAC with constant-time comparison and strict JSON object.
  Duplicate/extra keys, trailing documents/commas, comments/BOM, incorrect field types, context/nonce/digest mismatch,
  any state other than verified or any value other than boolean false for `serving_allowed` yields false with empty
  output. Unknown/missing/error is never turned into a zero snapshot or old cached success.
- Exact full v2 checkpoint schema: integer version 2, pinned journal, integer counts <= signed 64-bit maximum, batch
  count <=1024 (current prototype history cap), event count >= batch count, lowercase hashes. Zero is valid only when
  both counts/hashes are zero; nonzero batch requires nonzero hashes. This is structural validation; the configured
  reader supplies the full-chain/current-authority assurance under its separately verified contract.
- Snapshot exact schema and head must match the checkpoint's **batch** seq/hash. Hidden and purge sets and per-subject
  unresolved intent ids must be valid, complete canonical projections: memory then media then id, sorted ids, no
  duplicates. Combined item cap 2048 plus wire cap; invalid/oversized material is refused whole, never truncated.
  Cross-set overlap is allowed because legitimate purge barriers can also be hidden; cleanup barriers need not be hidden.
  A zero head requires all sets empty. Current-head replies must have null snapshot.
- Atomic per-call delivery guard gives at most one callback even after duplicate/late replies, exceptions or owner
  destruction. Pending callbacks retain copied pins/key/binding state; destruction alone does not prove cancellation.
  Exchange must retain its existing confidentiality, serialization, queue/streamed-byte/deadline/cancellation guarantees.
  The Source itself has no timeout thread, unbounded request queue, automatic retry or destructor-driven network abort.

The existing `Source` callback boolean means material accepted under this pinned reader contract. It is not permission
to open a database, and the `serving_allowed=false` protocol field is not a database-state observation.

## Restore-context correction in the coordinator

Previously the coordinator passed only journal id to Source methods; a correctly authenticated Source pinned to a
different restore database/attempt could otherwise supply material to the current request. Source now has a required
`binds(Request) noexcept` contract with no permissive production default. WireSource checks all three pins exactly.
The legacy unit fake explicitly declares its intentionally unpinned model behavior; it is not a production fallback.

Coordinator preserves **close first**: after a confirmed store close it checks binding before snapshot/read/stage.
Mismatch yields Blocked with no Source network call or staged/verified result. Binding is checked again before the
final current-head call, so a Source that reports a revoked pin cannot reach verified recording. Full current head
must still equal the staged batch head; a changed head remains HeadChanged and CLOSED. Even VerifiedClosed is only
preparation evidence: a writer may advance after that read, so separate quiet-window/active-incarnation/release proof
is still mandatory. No auto-open branch was introduced.

## Verification arrangement

- Eight new ordinary unit cases in `RecoverySourceBridgeUnitTests.h`: valid/zero material, 39 malformed/forged/binding
  faults, current-head contradictions, invalid inputs, duplicate/late/exception/lifetime handling, fresh nonce replay
  refusal, coordinator mismatched context/head movement and pin revocation. Registered in the existing unit target.
- Actual C++ Source -> HTTPS -> Python RecoveryBackend/Reader -> validating metadata adapter -> conditional metadata
  facade and immutable object fakes. Separate publication/reader endpoints and keys; reader gets current shared external
  fixture rows on each new read transaction. Reader fixture refuses data writes and records zero commit/PUT counts.
- Compiled coordinator uses the real WireSource and **fake recovery Store** for this network test: it validates full
  snapshot/head, reaches VerifiedClosed only with the same bound context, and rejects foreign attempt/key and a
  mismatched coordinator request. This is not a real PostgreSQL recovery-overlay/fence drill or provider evidence.
- The combined owned harness creates a new tmpfs PostgreSQL instance (never restarts the old discarded data), compiles
  final code, runs publication + recovery HTTPS proofs, Python tests, normal backend suites/live API/CTest and startup
  refusal/export extras. PKI private files are removed by exact filename and cleanup is checked.

Scoped prebuild on the older owned compiler passed unit **3183 assertions**, including the eight new Source cases.
Initial full run `usapp-e1-5d75a4d4e5` then passed CTest 36/36 (104.62 s), HTTP1465, actual recovery HTTPS21 and
publication HTTPS14, Python149 twice and the whole harness. These results **precede the last parser hardening** and
are retained as intermediate history, not final-code proof.

A subsequent new regression sent an otherwise valid, correctly signed JSON reply followed by a raw NUL and `{}`.
JsonCpp accepted it as end-of-input despite `failIfExtra`; unit then failed **3 of 3187** assertions as intended. This
was an authenticated-frame syntax gap in the fixed compiler image's JsonCpp, not an unsigned MAC bypass. Both recovery
and the existing publication consumer now explicitly reject raw NUL before parsing, with regression cases in both.
No authorization/timeout/assertion was relaxed.

Final full run **`usapp-e1-2423c0644f`** passed the complete owned harness (exit 0) on the final hardened source:

- Build; unit **3189 assertions**, including eight new Source cases and both NUL regressions; full **CTest 36/36 in
  106.03 s**; real isolated API **1465 assertions**, zero failures. All **619** prior ordinary cases retained in relative
  order; **8** new Source cases, **627** total, no omissions or duplicate names. No target timeout increased.
- Actual recovery C++ Source/HTTPS/Python reader/coordinator-fake-Store integration **21 assertions**; existing publication
  HTTPS **14**, file transcript **12 requests / 15 assertions**, HTTPS transport **74 assertions / 9 cases** passed.
- Python **3.8.7** and **3.12.16** each **149 tests**, including the reader/TLS/direct lease paths. SDK-only offline
  collection was not rerun: those adapters were unchanged; the previous 188/4-skipped result remains separate history.
- External fixture publication stays **2 operations / 4 batches**; recovery principal **0 commits / 0 PUTs**, one
  preflight; both services' network/backend/resource drains true, all non-serving. These are model/fake-Store results,
  not provider durability or actual PostgreSQL recovery-Source staging proof.
- All **54** migration checksums/files unchanged, eraser/privilege checks zero after apply/repeat/CTest, ledger chain
  and early/reversed erasure checks zero. Existing two-role SQL security/financial/memory/push/clock controls passed.
- Actual media/purge missing-provider and recovery-closed restart refusal/port probes passed; export loss/large/closure
  **8/6/23**, closed album/memory **7/8**, closed startup **9** passed. Exported-content markers zero. Normal observed
  export reconnect (~1008 ms here) does not prove the unrelated half-open/reconnect availability issue fixed.
- Host/runtime/compiled-source bindings verified, synthetic private key files removed and absence checked, all owned
  containers stopped/retained. The closure result refers to the running test's end state; tmpfs PG data is not retained.

The older `35fb10e7d8` compiler was modified for scoped prebuild/NUL probing and is **not** a current verified artifact.
Use the final `2423c0644f` snapshot/proof for this component, not an older stopped compiler's name or prior green label.
No frontend/provider/browser/device/independent backup rerun, live resource access, deployment or commit. The separate
J3b recovery-only real-database mode was not run; the future Source/Postgres closed recovery drill is still required.
Evidence allowlist: `Claude outputs/recovery-source-20261116` (summaries, compact CTest/interops, source/migration bindings,
NUL negative-control conclusion). No keys, environment/credential files, raw API/PostgreSQL logs or dumps archived.

## Remaining gates

### Independent checkpoint in restored-database preparation

`Coordinator::runPinned` accepts a checkpoint supplied outside the restored database and the reader
being checked. The journal identity and checkpoint shape are checked at entry. After PostgreSQL
confirms the close, the complete snapshot must match that checkpoint exactly before quarantine can
be staged. The existing current-head read must still match the staged snapshot before verification
can be recorded. A valid reader response can therefore fail on either an older sequence or a
different hash at the same sequence. An unavailable or unauthenticated reader also leaves the
database closed.

The isolated restore drill supplies checkpoints fixed from the synthetic publication fixture. It
checks a prior checkpoint and an authentication refusal against the real PostgreSQL restore target
before running with the matching checkpoint. The pin identifies the expected data. Its authority
and the quiet window needed for a serving release are separate requirements.

Disposable run `usapp-backup-probe-2a1711f142` passed CTest's unit target, the 149 Python cases in
both environments, and all 16 assertions for HTTPS recovery against the restored PostgreSQL
database. Both refused attempts left no manifest or verification for their generation. The
matching attempt withheld the deleted photo and recovered one retained object, while the
database stayed closed and both application roles lacked CONNECT. No main startup, schema,
provider configuration, or serving release was changed.

### Signed evidence for one restore instance

`authority::prepare` verifies an Ed25519 signature against a separately supplied trusted public
key. The signed fields bind the checkpoint sequence and hash to the journal, database incarnation,
attempt, externally allocated instance token, and SHA-256 of the encrypted archive. The verifier
compares the binding with values supplied by the restore host. It accepts a window of at most
five minutes and checks freshness again before staging, before recording, and when the record
receipt arrives. Wall-clock rollback and passage of the steady-clock deadline block the attempt.
Expiry during a confirmed record can leave preparation evidence in PostgreSQL, but the caller
receives a blocked result and the database stays closed.

The signed representation is ASCII. Each line ends in LF. The first line is
`usapp-recovery-attestation-v1`, followed in order by database UUID, attempt UUID, journal UUID,
instance token, archive digest, checkpoint sequence, checkpoint hash, issued-at, and expires-at.
Integers use unsigned decimal spelling for the allowed nonnegative values and times are Unix
seconds. Digest strings use 64 lowercase hexadecimal digits. An independent Node-generated
known answer fixes the encoding in the C++ tests.

In the disposable drill the owner-checked Docker container ID identifies the instance and the
database session must report that container's internal address. The archive digest is checked on
the exact byte array passed to authenticated decryption. A synthetic signing key issues the
fixture evidence separately from the journal reader. A signature proves who attested to a
checkpoint and target. Deploying the signer that actually knows the latest authoritative
checkpoint, enrolling its key, and enforcing release authority remain separate work.

Final disposable run `usapp-backup-probe-a28b87abfd` passed the CTest unit target, all 149
Python tests in both environments, and all 21 restored-PostgreSQL HTTPS assertions. The restore
tool refused the wrong archive digest and confirmed the digest of the encrypted bytes it
actually restored. Incorrect instance, incorrect archive, expired proof, and invalid signature
created no recovery attempt. The valid proof copied one retained synthetic object, withheld
one deleted object, and kept serving closed. All 54 migration checksums matched.

### Offline issuer and independent process boundary

`tools/recovery-authority-issuer.mjs` uses the same attestation bytes from an independent Node
process. The offline command takes a signer-owned state document, a restore request, and the
path of an Ed25519 private key, in that order. The request contains only the five target fields
`databaseId`, `attemptId`, `journalId`, `instanceToken`, and `archiveSha256`. The state adds the
registered target, independently observed checkpoint, observation time, and time through which
writers are paused. The checkpoint sequence accepts an exact decimal string for the full
signed 64-bit nonnegative range. A request cannot supply or override checkpoint or pause facts.

The command refuses a mismatch, observations older than 30 seconds, future observations, or
an ended pause window. The proof expires at the earlier of five minutes and the end of the
pause window. The response includes only issuance time, expiry time, and signature. Key
enrollment is separate. The restore process still compares the proof against the actual
archive digest, target transport address, and journal reader output before it records
preparation. The synthetic drill enrolls a throwaway public key and keeps its private key in
the separate signing process.

The issuer assumes the checkpoint and pause record are populated by a trusted authority.
It has no production checkpoint collector, writer fence, public-key enrollment, or serving
release adapter. The offline tool does not discover whether a backup is the latest.

For the restored-database drill, the synthetic publisher stops accepting work and drains its
network, backend and owned resources before emitting an observation of its checkpoint. The
read-only recovery listener remains active. The harness supplies that observation to the
separate issuer and passes the resulting public proof to the C++ recovery test. This exercises
an actual pause of the single owned synthetic publisher. Establishing a fence across every
production writer is a separate requirement.

Disposable run `usapp-backup-probe-00f2772ed8` passed four issuer tests, the CTest unit target,
the journal suites, and all 21 restored PostgreSQL assertions. The C++ verifier accepted a
proof from the independent Node process after the publisher drained. The joint restore
copied one retained object, withheld one deleted object, and kept database serving disabled.

### Shared pause across local publisher processes

`tools/journal_writer_fence.py` uses a private, pre-provisioned directory on a local Linux
filesystem outside the database being restored. Each cooperating publisher holds shared
admission and creates a durable marker before it can start. It clears only its own marker
after its drain and external effects are confirmed. An exception or process death releases
the operating-system lock but leaves the marker, which blocks a pause.

The pause owner acquires exclusive admission, refuses unresolved writer markers, and
persists a pause marker before observing the checkpoint. The marker blocks new publishers
after the owner exits or crashes. There is no automatic resume or marker-clearing operation.
The synthetic journal service uses this fencing path for the joint restore and the issuer
requires its latched observation. Linux multiprocess tests cover two concurrent publishers,
abnormal writer and authority exit, and stale drain tickets.

This is a single-host fence for cooperating processes with the same directory and UID.
Production must still account for all writers and their effective permissions, including
writes outside that host, and reconcile uncertain provider effects before clearing a
writer marker. Service shutdown by itself confirms only local drain.

Disposable run `usapp-backup-probe-e75e7e7095` passed all four Linux multiprocess controls,
the CTest unit target, the issuer and journal suites, and all 21 restored PostgreSQL assertions.
The issuer accepted the observation after the local pause was latched. Joint restore copied
one retained object, withheld one deleted object, and left the database closed. All 54
migration checksums matched.

### Shared coordination database for publishers on different hosts

`tools/recovery-fence/schema.sql` provides a PostgreSQL coordinator in a database separate
from the application and any restored copy. It is not an application migration. Each
registered publisher logs in with a distinct role and uses a stable admission UUID for its
incarnation. It can begin provider work after a confirmed commit of `admit`. An admission
survives a session disconnect and has no automatic expiry. The same publisher calls
`confirm_drained` only after it stops and accounts for its external effects. It can replay
that acknowledgement with the same UUID.

An authority role calls `request_pause` with a pause UUID to stop new admissions while existing
publishers drain. `confirm_pause` advances the same UUID to paused only when every admission
is terminal. Both orders of the admission/pause race lock the same journal row. Only
READ COMMITTED transactions with durable commit settings are accepted. Consumers have to
wait for a confirmed commit before treating results as evidence. The schema grants no
direct access to state tables and exposes no resume operation.

The coordinator records statements made by enrolled publishers. Its admission UUIDs cannot
stop a process that writes directly with independent provider credentials. Production use needs
separate credentials for each writer, complete provider coverage, reconciliation of uncertain
effects, and a coordinator that survives any failover without losing acknowledged state.

`python -B tests/fence_postgres_probe.py` passed against an owned disposable PostgreSQL 17
instance. Separate authenticated sessions verified permission boundaries, idempotent
acknowledgements, disconnect persistence, and the durable commit precondition. In both race
orders the second session was observed waiting for the lock, then returned the result
dictated by the first committed transaction. The disposable instance was removed afterward.

### Coordinated issuance

`issueAfterPause` validates the registered restore target first. It requires a committed
`paused` reply for the journal and pause UUID, reads the checkpoint through the authority's
separate observation adapter, and confirms the same pause again before signing. The pause UUID
is the restore attempt UUID, which the existing signed format already covers. Missing, changed,
or incorrectly bound confirmations stop issuance. The offline command accepts separately
obtained offline evidence.

The joint drill admits the synthetic publisher in a separate, network-disabled PostgreSQL
coordinator before starting publication. A pause request initially stays in `draining`.
New admission is refused. The publisher stops, drains, and latches its local pause before its
coordinator admission is marked drained. A separate Node issuer then queries the coordinator,
requests a fresh writer-side checkpoint observation, confirms the same pause again, and signs.
The C++ restore verifier compares the signed checkpoint to the read-only journal response.
The drill leaves the recovery database closed. The observation adapter is synthetic. Production
provider observation, live credential control, and serving release remain open.

Run `usapp-backup-probe-eab62e8631` passed all seven issuer tests, the unit target, the
four Linux writer-fence cases, and the 21 restored PostgreSQL assertions. It recorded
`COORDINATED_PAUSE_CHECKPOINT_ISSUER_INTEROP_PASSED`. The retained object was restored,
the deleted object was withheld, and serving stayed closed. The fixture coordinator was
removed during cleanup.

### Signer-side TableStore and OSS observer

`CheckpointObserver` owns its own restricted TableStore and OSS read clients and accepts a
separately enrolled observation principal. It reuses the full-chain validation and uncached
metadata recheck from the recovery reader. A valid observation returns the journal and exact
batch sequence/hash, with the read start time. Missing rows, preflight refusal, corrupted
history, unavailable clients, and changes across reads leave the observation unconfirmed.
The observer supplies no pause assertion. The issuer still obtains both pause confirmations
from the separate authority coordinator.

The joint drill gives the observer its own synthetic identity and client handles. Between the
two pause confirmations, the issuer requests material that the observer reads through the
TableStore/OSS adapter boundary with injected fixtures. It no longer consumes a checkpoint
copied directly from the publisher's state. The observer report checks that no commit or
object upload was made. Default network blocking and the existing restricted resource
configuration continue to apply. Live credential enrollment, provider approval, and serving
release remain separate.

Run `usapp-backup-probe-87276712c4` passed 153 Python tests in each environment, seven
issuer tests, the unit target and all 21 PostgreSQL restore assertions. The independent
observer passed its preflight, made zero commits and object uploads, and closed its handles.
The retained object was copied, the deleted object was withheld, and serving stayed closed.

### Publisher lifecycle bound to the coordinator

`FencedPublisher` starts the publication listener only after it receives a committed admission
receipt for the exact journal and publisher incarnation. It first closes the listener and
drains the local workers and client handles. It then requires separate evidence that the
provider effects have been reconciled. The publisher reports a completed drain only after
the coordinator confirms the commit of that drain for the same admission. An absent,
misbound, or unconfirmed result preserves the admission for a later explicit recheck.

The joint drill routes committed PostgreSQL receipts through its owned fixture. The fixture
checks the complete read-only journal snapshot before acknowledging provider reconciliation.
This exercises the lifecycle with synthetic provider effects. A live adapter must establish
the outcome of requests that may have reached the provider and enroll every real writer.

Run `usapp-backup-probe-20730081af` passed four publisher lifecycle and four local
writer-fence checks, plus all 21 PostgreSQL recovery assertions and the coordinated
joint restore. The publisher waited for the coordinator's drain acknowledgement.
The retained synthetic photo was restored, the deleted photo was withheld, and
serving stayed closed. A subsequent focused five-case run also verified that an
admission callback exception produces a fixed refusal and does not trigger a retry.

Production endpoint and identity enrollment, resource ownership, independent current reads,
active-incarnation authority, and main startup wiring remain open. The isolated PostgreSQL and
synthetic object drills cover preparation. Release still needs provider-backed joint recovery
and J3c/J4/J5 acceptance with the final application build. No cloud resource calls, real credential
loading, deployment or commit.

Stopped tmpfs database containers preserve no database contents; CLOSED means the earlier test's end state only.
Compiler artifacts/evidence and explicitly exported backup files are distinct from that disposable database data.
