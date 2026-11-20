# Admin wallet adjustment implementation plan (2026-11-14)

Owner-approved correction: asset flower or sunshine, signed total delta -9999..9999 excluding zero, mandatory reason and
second confirmation, no negative result, immutable audit and key classification. Ordinary financial paths stay unchanged.

## Posting model

- New immutable admin_wallet_adjustments parent: admin, target member/couple, asset, delta, expected account sequence,
  before/after balances, reason, original key/fingerprint, audit reference and ordered child operation ids.
- Preserve the legacy flower-entry bound 999 and operation/account uniqueness. One atomic correction contains ceil(abs(delta)
  /999) child operations for flowers, or one for sunshine; every child has one ledger entry for the same account. Total
  remains <=9999. Rollback of any child/audit/link check rolls back the whole correction.
- Add a nullable actor_admin_id to wallet_operations. ADMIN_ADJUSTMENT requires admin actor and null member actor; all
  existing operation types continue requiring member actor and null admin actor. Do not impersonate the affected member.
- New deferred guards prove parent/children/audit equality, exact target, source, ordered contiguous sequences, signed
  chunk amounts, sum and before/after balances. No extra operation or entry may be attached to a correction.
- The existing account-row chain lock continues serializing against task awards, redemption/refund and avatar purchase.
  Expected sequence binds confirmation to the balance the admin reviewed; conflict requires a fresh explicit review.

## Command protocol

Authenticated active admin -> per-admin/key lock -> classify key/fingerprint before target/current-state checks -> member
read lock -> account row lock -> expected-sequence and bounded resulting-balance checks -> append audit/parent/child
operations/entries -> named deferred checks immediate -> receipt -> confirmed commit. Replayed commands return original
facts, not a new adjustment or a claim about current balance. Session/Origin/CSRF are enforced by AdminFilter.

Request: exact asset, delta decimal string, expected_seq decimal string, reason. Target member is in the path; actor is
from admin context. Idempotency key comes from the existing validated header. An unconfirmed response retains exact key
and body; neither a read nor a later refusal proves the original did not commit. No automatic retry or direct SQL editor.

The SQL function uses the same application-role/admin-identity boundary as existing local admin catalog functions: SQL
cannot distinguish API listeners; AdminFilter is the actual HTTP boundary. No SECURITY DEFINER grant bypass is introduced.

## Required verification before completion

Both assets and signs; 1/999/1000/9999 flower chunk boundaries; zero/over-limit/negative result/maximum balance refusal;
unknown/inactive admin and target; empty member account; stale expected sequence; same-key replay and changed-body reuse;
audit/parent/operation/entry forgery and append-only refusals; rollback of any failure; concurrent correction and ordinary
posting; deliberately discarded confirmed-commit answer followed by replay; both-listener session/surface/CSRF/Origin
boundaries; UI capture/confirmation/unknown/retry/session race tests; old ledger and full regression checks. No production
correction is authorized merely by this design file.

## Implementation progress (2026-11-14)

0049_admin_wallet_adjustments.sql is numbered after successful rollback-only validation under postgres and usapp_api,
including nine independent forged-group refusals. Frozen hash:
24924f637330465867b96373d619d43be765ee424b13346b03d50b8e3a462e96. Prior 49 checksums are saved in
`database/frozen-through-0048.sha256`; the ordinary harness now checks them plus the frozen 0049 before applying 50 files.
No SECURITY DEFINER or relaxed legacy entry bound. Permission narrowing is repeated by init-roles section 40 and matrix.

POST /api/admin/v1/users/{userId}/wallet/adjustments returns 200 original receipt for both a new command and replay.
Admin identity is session-only; body is exactly asset/delta/expected_seq/reason, numeric values canonical strings. Conflict
uses ADMIN_ADJUSTMENT_CONFLICT; active-admin refusal uses ADMIN_ADJUSTMENT_FORBIDDEN. Contract 0.57.0.

The wallet page retains read-only balance/ledger queries and adds a separate two-phase correction panel. Capture freezes
key, target, reviewed sequence/balance and command; one unresolved correction spans route changes. No new command or
uncertain dismissal. Network/malformed success/5xx stay unknown; a later 4xx cannot erase that uncertainty. First definite
refusal requires acknowledgement before a fresh capture. Original receipts never overwrite the displayed current balance;
a confirmed command triggers a fresh read instead. Session change clears private state and ignores old replies. Captures
are memory-only; reload/logout loses retry material and cannot cancel an already-sent command, stated on the page.

Prevalidation failures: b7134af464 depended on absent pgcrypto digest; switched to PostgreSQL's built-in SHA-256 without a
new extension. 502df3ab73 exposed a CASE-in-IF parsing ambiguity; parenthesized before successful draft validation. Draft
codes were changed from occupied UZ codes to unused UV101/UV102/UV103 before numbering. Successful preflights:
7c9914188c (basic), 30148be373 (nine forged groups), 715e937560 (tightened grants), all rolled back objects and fixtures.

First full run usapp-e1-e65be05bb1: 50 checksums and zero role deviations after apply/repeat; both-role SQL ledger and build
passed; service target 43 assertions/3 cases passed (including concurrent commands/task reward and discarded confirmed
commit reply). HTTP failed 32/1347: the new test used AppConfig in the test process, where it was not loaded, so Origin
was empty and AdminFilter correctly refused every request. No application financial error was observed; original migration
is unchanged. Fixture now uses the isolated server's explicit https://localhost:8444 origin. Full rerun in progress.

Frontend focused 34/34 (16 new plus 18 existing), both typechecks and isolated Vite build passed; original 65 dist files
unchanged. One new component test initially used a RouterLink stub that swallowed its slot; a rendering stub corrected that
fixture without weakening the asserted return link. Full frontend verification in progress. Build work folder:
%TEMP%/usapp-admin-adjustment-web-35d45ce081. Strict YAML/ref check passed (0.57.0; 127 paths; 247 refs; duplicate control
refused), not OpenAPI semantic validation. No browser/real-admin/production acceptance or deployment.

The post-commit-loss test deliberately discards the service answer after the actual commit callback confirmed success;
it is stronger than a pre-commit fault but still not a real half-open network failure. Existing export transport availability
limits and older timestamp guards remain separate. Corrections do not yet create anomaly notifications; that is a pending
V1 ledger item, not a claim of completed anomaly handling.



### Combined regression and compatibility follow-up

Run usapp-e1-d264f6f238 passed build, both-role adjustment SQL ledger, service target 43 and real HTTP 1348. Full CTest was
29/32, not green: the synthetic permission matrix omitted the new table's id column; the init-roles host rewrite changed
LF to CRLF against a multiline static needle; the existing wallet null-member probe required SQLSTATE 23502, while the
new conditional actor check returned 23514. No failed assertion was removed. The synthetic map and source line endings
were corrected. Frozen 0049 was preserved; new 0050_wallet_member_actor_compat restores the original 23502 for non-admin
postings with missing member actor. New admin postings retain their distinct conditional-actor validation. Hash of 0050:
450c99df43abc2822c6e047fac7796f86813fbe9e44a2afda74acd6c1aec81ba.

Final combined run usapp-e1-5e706f9f3d: 51 migration hashes; role deviations zero after apply/repeat/CTest; both-role
adjustment SQL ledger passed; build, target 43/3 cases, live HTTP 1348; **CTest 32/32 in 98.20 s**, default db_gated 13.37 s,
new adjustment target 0.09 s. Financial/erasure postchecks zero. Export loss/large/closure 8/6/23 and closed album/memory
7/8 passed. Full harness exit 0; containers stopped and retained. All migration bytes unchanged during the run.

Frontend final: focused 34, full admin **219 tests / 15 files**, both typechecks and isolated build passed. Original admin
20 and web 45 dist files unchanged. Additional compatibility run usapp-wallet-compat-4dc7bb46cc passed all eight old SQL ledgers under both postgres and usapp_api (16 passes): wallet, task, product, redemption, redemption workflow, avatar catalog, purchase and loadout. All fixtures rolled back; 51 checksums and privilege deviations zero. Containers stopped and retained.
A subsequent log-only edit removes raw PostgreSQL exception text from this new service's error handler because database
error DETAIL can contain row contents, including a correction reason. Its final compilation will be recorded separately;
it does not change posting logic. Selected evidence: Claude outputs/admin-adjustment-20261114 (no generated credentials).
No browser/real-admin/production transaction, deployment, activation or commit was performed.

The later cancellation run usapp-e1-5f6305059c compiled and exercised the final diagnostic-only service edit: adjustment
target 43, live combined 1393, CTest 33/33 and both-role ledger checks passed. The raw PostgreSQL DETAIL logging removal
is therefore compiled/verified, not an outstanding uncompiled source change. Financial semantics remain unchanged.
