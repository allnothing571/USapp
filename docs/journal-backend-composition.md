# Restricted journal backend composition (2026-11-16)

Subsequent components add read-only external journal material and a locally verified C++ Source consumer with separate
MAC/context guards; production main/release remain absent. See [reader](journal-recovery-reader.md) and
[consumer proof](recovery-source-bridge.md). Previous CLOSED references below are
end-of-test history only: the disposable PG used tmpfs, so stopped containers do not retain that database data.

## Delivered scope

`tools/journal_backend.py` composes the existing restricted TableStore SDK facade, immutable object adapter, validating
Metadata adapter, Publisher, authenticated Dispatcher and HTTPS listener. Default transport still **blocks network**;
the caller must inject explicitly authorized pools. There is no environment/credential-file reader, daemon/CLI,
resource creation/initialization, production main wiring or restored-database serving permission. Existing Shanghai
and trial-resource naming restrictions are preserved, **not expanded into general production configuration**.

The API's refusal gate remains unchanged. Contract **0.60.0**, **54 frozen migrations through 0053**, next **0054**.
This is a locally verified composition component, not completed cloud integration, J3c/J4/J5 or V1 acceptance.

## Configuration, principals and admission

- Exact table/object configuration types, matching journal, region and resource suffix; existing exact endpoints only.
  Invalid configuration is refused before constructing clients. This is syntax/binding validation, not resource ownership.
- Explicit object-publisher and checkpoint-writer credentials with different access-key ids. Their repr excludes both
  id and secret; the MAC key is excluded from configuration repr. No recovery-reader credential is accepted/needed by
  this writer composition. Distinct ids do not prove distinct IAM policies or effective permissions.
- Two independent object clients: publication and put readback use the publishing principal; Metadata validates full
  referenced/candidate bytes through the checkpoint writer's object client. It never trusts another principal's receipt
  as a substitute for reading those bytes. The tests assert the validator issues GET only; actual IAM is not verified.
- Construction/listener start performs no adapter I/O. Strict authenticated request validation precedes publication.
  The first valid operation performs the existing DescribeTable + Start/Abort preflight on the serialized publisher
  worker. A failed/unconfirmed preflight is latched for this owner; subsequent requests do not automatically probe again.
- Successful schema/transaction preflight is cached locally once. Every operation still loads/validates current head,
  chain and indexes through the existing adapters. A missing head is unavailable and **never automatically initialized**.
  Cached preflight is not a continuing table-policy attestation, latest independent checkpoint or recovery authority.
- All existing Publisher size/history/round limits remain prototype safeguards. Full-history reads are not a production
  scalability solution. Per-SDK socket timeouts do not bound DNS/whole publication; the server retains its single active
  slot after network deadline/disconnect until local work returns. No automatic publication replay or fabricated abort.

First-time preflight may consume the caller's response deadline. A late job may still complete; an unknown response
must be reconciled by operation identity. Preflight failure requires an explicitly new owner after diagnosis, not an
unbounded self-retry. Changing resource policy/credentials or scaling this prototype needs separate review and evidence.

## Resource lifecycle

The SDK/object facades now have idempotent `close`: require a callable pool clear/close method and reject later I/O.
The default blocked transport has a no-op clear because it never owns sockets. An injected wrapper without a cleanup
interface reports unconfirmed cleanup and stays locally unclosed; it cannot be treated as resource-drained. A local
active TableStore transaction refuses close **without sending Abort**; the owning metadata operation must finish first.
An unknown remote commit/session is not a proven rollback merely because the local pool was closed.

Managed Dispatcher serializes direct use too; after close begins, no new call enters the publisher. Backend close
refuses resource teardown while a call is active, reports cleanup failure, and allows an explicit later cleanup attempt.
It does not terminate a worker or retry a business operation. Constructor failure tries to close already-created clients
and emits a fixed error; resource reclamation cannot be guaranteed if an injected constructor/cleanup itself fails.

`Service` gives a backend one listener owner. Invalid TLS construction cleans constructed clients; closed backends cannot
start a listener. Duplicate start closes the original listener before propagating the error. Shutdown first drains
network/publication, then clears pools on at most one pending cleanup thread. That cleanup shares the original close
budget; slow cleanup returns `resources_drained=false` and retains its worker instead of blocking the caller indefinitely
or spawning replacements. An explicit later close can observe completion/retry a completed failed cleanup.

Construction/start-failure cleanup is synchronous outside request/event-loop execution; no hard startup CPU/DNS deadline
is promised. Daemon thread/process exit is never a proof of remote cancellation, durability or safe recovery release.

## Verification and evidence

No C++ application/contract/schema/frontend code changed in this batch. The C++ test helper/client binary was reused
from the previous owned full build `usapp-e1-35fb10e7d8`; its source bindings were checked before use. **The previous
CTest 36/36 and HTTP 1465 result was not rerun or relabeled as a new full backend pass.**

Final scoped attempt **`f7a3bf8752`** on that owned compiler, with a fresh Python container/PKI/capture directory:

- Host Python **3.8.7** and pinned container Python **3.12.16**, **125 tests each**, all passed: 74 journal foundation,
  18 existing HTTPS server, 14 metadata-adapter, 10 composition and 9 new composed TLS/lifecycle cases.
- Actual C++ HTTPS Exchange/WireJournal -> Python Service -> validating Metadata -> conditional metadata facade fake
  and immutable object fakes: **14 assertions**, **2 operations / 4 batches**. Preflight count **1**, validator PUT count
  **0**, network/backend/resources drains all true, `serving_allowed=false`. This is stronger adapter-path coverage than
  the previous pure publication-model fake, but still not vendor atomicity, TLS to a vendor or durability proof.
- Real pinned **TableStore 6.4.8** codecs/signing/response validation plus in-memory HTTP responses: initial focused
  **40 tests** passed before the final cleanup-interface regression was added; final full J4c offline run reports
  **184 tests, 4 skipped**, exit 0 (180 executed). The four skipped cases are
  the prior opt-in vendor-client local-TLS fixture tests; their required fixture was not supplied. New SDK tests ran.
- New real-SDK composition tests block socket connect/create_connection and DNS; synthetic credentials only. The full set includes bad
  signatures/checksums, transaction/row/conditional encoding, response caps, existing loss handling and new close guards.
  Response signatures are generated by the same SDK fake, not independent cryptographic/provider acceptance.
- Initial focused failure was the old HTTP response fixture's hardcoded `offline-access-id`, which rejected a correctly
  configured different principal. It now derives the response id from the actual synthetic credentials; signature and
  checksum validation remain enabled. The old bad-signature case still uses a bad signature for the correct id.
- Earlier scoped attempt `6622735dd9` passed 122 tests each and 14 interop assertions before the last three startup/
  invalid-TLS regressions were added. It is intermediate evidence, not the final 125-test result.
- Scoped `86aacf97a1` then passed 125 each, before the last pool-cleanup-interface correction. A new negative control
  removed the pool's clear method and exposed a false successful-drain result in that intermediate code (one intended
  failure). The facades now require cleanup support; the regression passes in the final 184-test SDK run. No assertion
  was weakened and unsupported pools are not silently accepted as drained. Final scoped `f7a3bf8752` rechecked current
  source bindings and all 125/TLS cases after this correction.
- Final source hashes matched both host and runtime. Temporary synthetic private keys removed and absence checked;
  runtime/compiler stopped again. The previously CLOSED disposable database was not started or modified this batch.

SDK packages were restored to one new temporary `site`/`wheels` directory using the **existing Windows CPython 3.8
hash lock**, binary wheels only, `--require-hashes` and `--no-index` installation. No global Python/pip/application/
production dependency change. That historical test lock is not a current production dependency/security recommendation.
The selected directory is recorded in evidence; if deleted, do not silently substitute a global installation.

Evidence allowlist: `Claude outputs/journal-backend-20261116` (test summaries, final non-serving report, source hashes,
runtime/source bindings and SDK location). No keys, credentials, environments, raw API/PostgreSQL logs or backups.
The existing combined harness now runs the composition tests/fixture for future full runs; it also supports separate
owned attempt directories without overwriting earlier proof. No frontend/CTest/database/independent backup rerun here.

## Still required before production

Explicit production resource/identity/IAM ownership and bucket-hardening proof; authorized transport/credential loading
and process hosting; real SDK/vendor fault/durability/budget evidence; independent recovery Source/latest checkpoint/
incarnation/restore binding; reviewed main/startup integration, full J3c/J4/J5 and combined deployment/device/provider
acceptance. No old cloud trial/expired credentials resumed, no cloud-resource call/creation, no commit or deployment.
