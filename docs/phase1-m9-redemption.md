# Phase 1 M9: Request redemption and mutual cancellation

Status (2026-10-26): owner-authorized direct Codex implementation and usapp_test
verification. Implemented and automated-test verified on usapp_test. Main API/web
rollout completed 2026-10-26 by Claude Code on owner authorization (Section 5);
0016 is applied/frozen on usapp_test and main; frozen 0000-0016 must not change.
Live/browser acceptance is not recorded. M10 is not started.

## 1. Rules

The existing redeem endpoint accepts both modes. Direct mode debits immediately;
request mode snapshots the displayed version without reserving funds and returns
pending_confirmation. Creator confirms/rejects; redeemer withdraws. Confirmation
uses the original snapshot even after edit/archive. Insufficient funds leave the
request pending. Rejected/withdrawn requests derive status cancelled and reason
rejected/withdrawn. Fulfilled is final.

Either party can propose cancellation of a paid pending_fulfillment redemption;
only the other can approve/reject, and only the proposer can withdraw. Approval
atomically refunds the snapshot price once. Refund above the flower cap is
rejected as a whole, leaving the proposal pending. One undecided cancellation
per redemption, no expiry. Fulfillment may win while cancellation is pending;
the proposal then ceases to be actionable. No partial refunds or sunshine.

## 2. Architecture

0016 adds one append-only reward_redemption_events table. Existing request
snapshots may have NULL operation_id; confirmed debits live on confirm events.
Existing direct snapshots are unchanged. A security-invoker state view derives
current status, effective debit and active cancellation proposal. No UPDATE grant.
Database guards serialize every event/fulfillment on (33008, hashtext(id)),
validate actor/state/proposal identity and operation identity. Deferred ledger
checks require exactly one matching debit/refund entry for each linked operation.

Lock order: membership FOR SHARE -> product lock (creation only) OR redemption
lock (transitions/fulfillment) -> operation claim -> redeemer flower account row
-> latest balance in a fresh statement -> event/ledger/audit -> confirmed COMMIT.
Never acquire product lock after a redemption/account lock. Request replay and
transition replay return immutable mutation facts plus current redemption state.
Unknown results retain the same action, proposal identity and key for explicit
retry; no automatic queue. Session guards apply before any wallet/list effects.

## 3. Verification

Executed directly by Codex on 2026-10-26, against literal usapp_test only.

| Check | Evidence |
| --- | --- |
| Migration rollback-only syntax prevalidation | First run failed on a PL/pgSQL CASE expression in IF; parenthesized before application. Recheck exit 0, ROLLBACK. |
| Migration + workflow SQL rollback-only prevalidation | Maintenance and SET LOCAL ROLE usapp_api: exit 0 each, 7 groups each, ROLLBACK; no migration registered at this point. |
| DryRun | exit 0; 16 skipped, only 0016 pending. |
| Apply + InitRoles, repeat InitRoles | exit 0 each; privilege query 0 deviation rows each; repeat skipped all 17. |
| Frozen 0016 hash | `909cacce8d8eb19389dbb9c73a37a4a55dc70d8cddd963036fa4915b365d1f33`; file equals schema_migrations. |
| Backend compile | Runtime sources compiled successfully; first test build failed on a nonexistent CHECK_MSG helper and enum streaming in a new assertion. Tests repaired; subsequent builds exit 0, no warnings reported. |
| Full CTest | unit PASS 0.03 s, static_check PASS 0.01 s; db_gated initially failed two new test assertions (see below), 10.10 s. |
| Targeted db_gated repair runs | First repair left one assertion lifetime error; final rebuilt run exit 0, PASS 10.53 s. All db_gated blocks 1-23 run behind the exact database/migration gate; not skipped. No repeated unit/static runs after test-only repairs. |
| Task startup policy on usapp_test | Actual db_gated report: `ok=true checks=972 mismatches=0 missing=0 unexpected=0`; wallet checks=187 clean. This is not a main API startup observation. |
| Web typecheck | exit 0, vue-tsc and tsc. |
| Web regression | First full run: 153/154 passed; obsolete M8-only "no request control" expectation replaced by explicit unfunded confirmation check. M9 targeted suite 13/13 passed. Final full run exit 0, 17 files / 160 tests, 7.35 s. |
| SQL compatibility on applied test DB | 10/10 final passes, application and maintenance roles: task 35, wallet 28, product 8, redemption 6, workflow 7 groups; all ROLLBACK. |

Repairs did not change any frozen migration or weaken a business guard:

- New test expected ProductService::Archived for a successful archive; success
  is Updated, while Archived is the error for an already archived product.
- Initial cap fixture reused an operation across multiple entries for one account;
  this correctly violated wallet_ledger_entries_operation_account_unique. The
  fixture now gives each legal funding entry its own operation.
- CHECK_EQ retained a reference through a temporary optional Status in a new
  test; storing the optional before comparing removed the dangling reference.
- The legacy redemption SQL duplicate-fulfillment test expected 23505. M9's
  terminal-state guard rejects earlier with UM004. The test now expects UM004
  only when M9 is present, retaining 23505 for the M8 schema.
- Maintenance TRUNCATE tests list the new dependent events table explicitly,
  without CASCADE, so they still exercise append-only rejection rather than an
  unrelated foreign-key error. Application permission expectations are unchanged.

Real-service coverage: unfunded request; creator/recipient/other-space roles;
insufficient confirmation leaves no operation/event and retries after funding;
snapshot confirmation after product edit and archive; immutable request and
confirmation replay after fulfillment; rejected/withdrawn requests never debit;
proposal-specific reject/withdraw/approve, stale proposal rejection; exactly one
refund and no fulfillment after cancellation; confirm-vs-withdraw and
fulfill-vs-refund races; fulfillment-first and refund-first orders; concurrent
same-key confirmation; real refund-cap rejection with no refund operation, same
proposal retained, then same-key success after spending. Database SQL checks also
reject incomplete debit, partial refund, self-approved cancellation, premature
fulfillment, mutations and writable-view privileges; a failing audit rolls back
its event, debit and operation together.

Web coverage: request does not load the wallet; definitive insufficient balance
ends the action and a later attempt gets a new key; unknown refund retains the
same key and proposal across refresh; refund success survives wallet-load failure;
late old-account results have no effects; confirmation captures the proposal
identity and session changes clear the local confirmation. Existing M7/M8
version, paging, session and direct-debit tests continue to pass.

Fixture cost: each successful M9 block creates five isolated @m3-fixture.test
spaces (including one foreign-actor space), six products and ten redemptions,
plus real task funding/settlements and transition events. The single real refund
cap case requires 1002 legal funding operations and entries to reach 1,000,000
flowers (per-entry maximum 999); these are committed test data, not reset or
cleaned. The failed duplicate-operation DO block rolled back its funding; later
cap fixtures remain. There is no shared initializer or global-count assertion.

## 4. Files and release limits

- Database: new `0016_redemption_workflow.sql`, new
  `redemption_workflow_test.sql`, init-roles section 14, privilege query and
  compatibility TRUNCATE branches/duplicate-state expectation.
- Backend: RedemptionService request/event/refund paths, RewardController
  authenticated transitions route, REDEMPTION_STATE_CONFLICT, expanded startup
  policy, OpenAPI 0.13.0, db_gated block 23 and pure/static checks.
- Web: typed transition client, captured proposal/action retry, conditional wallet
  reload, request/cancellation confirmations and actor-dependent controls.
- Status references: CLAUDE.md, handoff, plan, root/database README.

The release prerequisites below were completed in the main rollout (Section 5):
fresh backup/restore and rollback artifacts, main DryRun (0016 only), apply+repeat
InitRoles and zero deviations, API build/deploy/health and startup policy (task 972,
wallet 187), and finally the bind-mounted web build.

Live HTTP/controller/browser/PWA behavior and injected lost-COMMIT remain
unverified. Audit failure is checked in rollback SQL, not by a new production
fault hook. Cursor pagination is not snapshot-consistent; large-history view
performance is not benchmarked. Older M8 code does not understand unpaid requests
or M9 cancellation state; image/web rollback after M9 data exists is degraded
and never reverses wallet/audit facts. No Git publication, cleanup, reset, new
dependency, timeout change or M10 work occurred.

## 5. Main rollout (Claude Code, 2026-10-26)

The owner authorized this guarded release (backup/restore verification, migration,
InitRoles, API build/deployment, web build, release documents) and assigned its
execution to Claude Code; the M7/M8 rollout (phase1-m8-redemption.md Section 7) was
the reference. No source, frozen migration, dependency, timeout or deployment
configuration was changed; no Git publication, cleanup, reset, automatic rollback,
main business fixture or debit/refund transaction. M10 was not started. Detailed
release evidence is recorded here only.

- Preflight exit 0, 4.1 s: target `usapp` (current_database). Local set exactly
  0000-0016; 0016 SHA-256
  `909cacce8d8eb19389dbb9c73a37a4a55dc70d8cddd963036fa4915b365d1f33`. Main had 16
  rows (0000-0015), every checksum equal to the local file. DryRun -InitRoles:
  16 skipped, only 0016 pending with the frozen hash.
- Backup/rollback artifacts exit 0, 9.3 s:
  `backups/before-m9-20261026_212028/` (new directory; overwrite guards for path,
  image tag, container files and scratch database).
  - `usapp_before_0016.dump`: 409332 bytes, SHA-256
    `ec1b73c20f97f9a0ee6e7e51b35e544e994381e0afdeaa9e8427b1d38304a482`.
  - The exported host file was copied back and restored into
    `usapp_m9_restore_20261026_212028` (template0,
    `pg_restore --exit-on-error --single-transaction`). All 24 public base tables
    matched before/after/restore under the stability and volatile-table rules;
    this is a count verification only, not row-by-row proof. Baseline: 7 tasks,
    3 completions, 2 progress events, 4 accounts, 3 operations, 5 entries,
    23 audits, 16 migrations; products, revisions, redemptions, fulfillments and
    M6 change tables all zero.
  - Running M7/M8 image
    `sha256:0e61cc6d31585ddc448b953750c46cabc093253904a1b5e18be9fc5c976400d4`
    (container `088ce2982523...f9629`) tagged
    `usapp-api:before-m9-20261026_212028`; inspect confirmed the tag.
  - `web-dist/` copied from `caddy:/srv/web`: 22 files, 286280 bytes; index.html
    `351d50308b2503cfaf6de75df3e6dc115f9ea284478a1a880281471fd9941c7b`, sw.js
    `7d5fd21f36483ebcf75c6a41cdac2814582a6ec40513f220ec2cf61b8459398c` (equal to
    the M7/M8 release hashes).
  - `rollback-info.json` (SHA-256
    `460905ad8c71cc468dbb52e3d0bae3a982c48e0e77e240801196b73110442271`) records
    these artifacts and counts. Scratch DB, exported dump, host work directory and
    container `/tmp` copies are retained.
- Migration: immediate DryRun/hash recheck matched; apply + InitRoles exit 0,
  5.9 s, one `[ok] 0016_redemption_workflow`; privilege deviation query exit 0,
  0 rows. Repeat InitRoles exit 0, 4.3 s, 17 skipped / 0 applied; deviation query
  exit 0, 0 rows. All 17 main schema_migrations checksums match the local files.
- Backend: `docker compose build api` exit 0, 19.17 s (only the existing
  `useradd` UID>SYS_UID_MAX warning). Image
  `sha256:a5e17286c6f82161244924bdf67a665f3fead08eb665909a72f19c47d6b480b9`.
  `up -d --no-deps --no-build --wait --wait-timeout 120 api` exit 0, 8.1 s.
  Container `d4a7b3c8e0baab02ab7255979d8881957e99b660e850dab35a22e6714ec7367f`
  runs that image, healthy, started 13:33:25 UTC. Fresh startup:
  wallet `role=usapp_api ok=true checks=187 mismatches=0 missing=0 unexpected=0`;
  task `role=usapp_api ok=true checks=972 mismatches=0 missing=0 unexpected=0`.
- Web after backend health: `corepack.cmd pnpm run build` exit 0, 10.33 s
  (vue-tsc, tsc, Vite, PWA); precache 21 entries / 213.68 KiB; dist 22 files /
  290686 bytes. Caddy-served hashes equal local dist: index.html
  `9b4238fa0f8b74735afc823c1cd8c4a7895129c3248f140b4836012a7a584427`; sw.js
  `84cfc85bdb037a8304591b3ed7c2807ca9e537dd30665c74bb56b1a196909809`.
- Final status exit 0: api running (healthy), postgres running (healthy), caddy
  running; 17 main migrations; products, redemptions, redemption events and
  fulfillments all zero. Previously passed automated suites were not rerun.

Remaining unverified: live request/confirm/reject/withdraw, cancellation
propose/approve/refund and fulfil HTTP workflows, browser/PWA update acceptance,
rollback exercise and formal acceptance. Code rollback can use the saved image and
web copy only; 0016 stays applied, older M8 code does not understand unpaid
requests or cancellation state, and rollback never reverses debits, refunds or
audits. Restoring the dump over main would discard later data and was not done.
