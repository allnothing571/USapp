# Phase 1 M8: Direct Redemption

Status (2026-10-26): **the owner authorized M8 only (database, backend, contract,
web) with automated verification on usapp_test, implemented by Claude Code.**
This replaces the earlier combined M8-M10 assignment. **M9 and M10 are not started.**
The owner subsequently authorized the joint M7/M8 main release, executed by
Codex on 2026-10-26: 0014/0015 are frozen on test/main and API/web are deployed.
Automated evidence: Section 6. Release evidence and remaining limits: Section 7.

Inputs: `docs/phase1-plan.md` (M8 row, C2, D11/D12 for later), M7
(`docs/phase1-m7-rewards.md`), the wallet ledger (0006) and the task settlement
conventions (`docs/phase1-m3c-design.md`).

## 1. Scope and rules

- Only the product **owner** (the partner it was created for) may redeem, and
  only an **active, direct-mode** product. Request-mode products return 409
  `REDEMPTION_MODE_UNSUPPORTED` until M9. The creator redeeming their own
  product gets 403 `REDEMPTION_FORBIDDEN`; another space gets 404.
- The client submits the **product version it displayed** (`expected_version`).
  A newer version returns 409 `PRODUCT_VERSION_CONFLICT`; an archived product
  returns 409 `PRODUCT_ARCHIVED`. A changed price is never silently charged.
- One transaction does all of the following:
  - checks the balance (409 `INSUFFICIENT_BALANCE`, nothing reserved);
  - debits `price_flowers` flowers;
  - creates the redemption, with snapshots of product id, version, title,
    description, price, mode, creator and redeemer;
  - writes the audit.

  Never a double debit, never a negative balance, never sunshine.
- Only the creator may mark a pending redemption **fulfilled**. That is final and
  charges nothing.
- Both parties can read their redemptions, with snapshots unaffected by later
  edits/archive. Lists use cursor pagination in the immutable
  `(created_at DESC, id DESC)` order, the same contract shape as M7.
- Not in M8: cancellation, refund, reservation, request mode, sunshine spending,
  notifications (C2 later), the M10 home redesign.

## 2. State machine and actors

```
(none) --redeem [owner, active, direct, expected_version current, balance >= price]-->
        pending_fulfillment   (one REWARD_REDEMPTION operation: one flower entry = -price)
pending_fulfillment --fulfill [creator]--> fulfilled   (no wallet write; final)
```

The status is derived: `fulfilled` when a fulfillment row exists, otherwise
`pending_fulfillment`. No other transitions exist in M8.

## 3. Database (migration `0015_reward_redemptions.sql`)

- `reward_redemptions` (append-only):
  - `id`, `couple_id`, `product_id`, `product_version` (FK to that exact
    revision), `creator_id`, `redeemer_id`;
  - snapshots `title`, `description`, `price_flowers` (1-999),
    `redemption_mode` (CHECK `direct` in M8);
  - `operation_id` (unique, FK to `wallet_operations`), `idempotency_key`,
    `request_fingerprint`, `created_at`; unique `(redeemer_id,
    idempotency_key)`.
- `reward_redemption_fulfillments` (append-only): `redemption_id` PK,
  `couple_id`, `fulfilled_by`, `idempotency_key`, `request_fingerprint`,
  `fulfilled_at`; unique `(fulfilled_by, idempotency_key)`.
- Backstops (SQLSTATE class UM):
  - UM001: append-only (UPDATE/DELETE/TRUNCATE) on both tables.
  - UM002 redemption guard, under the M7 product lock `(33007,
    hashtext(product))`:
    - the snapshot equals the latest revision, which is `active` and `direct`;
    - redeemer = product owner; creator = product creator; same couple;
    - the operation is `REWARD_REDEMPTION` / `reward_redemption` /
      `<redemption id>`, with actor = redeemer and the same couple.
  - UM003 deferred, from redemption, operation (`REWARD_REDEMPTION`) and entry
    inserts: every redemption operation belongs to exactly one redemption and
    has exactly one entry, a flower entry for the redeemer of `-price_flowers`.
    The 0006 chain/bounds triggers keep `balance_after >= 0`.
  - UM004 fulfillment guard: `fulfilled_by` = the redemption's creator, same
    couple, not before the redemption; the PK prevents a second fulfillment.
- Grants: SELECT and INSERT for `usapp_api` only (migration and `init-roles.sql`
  Section 13). The startup task/reward policy object list gains both tables
  (expected 667 + 2 x 38 + 3 x columns). The privilege deviation query covers
  them. Wallet grants are unchanged.

## 4. Service, lock order, idempotency

Redeem (`POST /api/v1/rewards/products/{id}/redemptions`,
`{expected_version}`):

1. `couple_members` `FOR SHARE` (caller), joined to the product.
2. Advisory `(33007, hashtext(product))`: the same lock as M7 edit/archive, so
   a redemption and an edit/archive serialize.
3. Key classification on `(redeemer, key)`: the same fingerprint replays
   (also after a later edit/archive/fulfillment); a different one is 409.
4. State checks: actor, archived, mode, version.
5. Operation claim (NULL wallet key; the source key is the redemption id).
6. Flower account `INSERT ... ON CONFLICT DO NOTHING`, then `FOR UPDATE`.
7. Read the latest entry; if balance < price, 409 `INSUFFICIENT_BALANCE`
   (rollback, nothing written).
8. Redemption `INSERT ... ON CONFLICT DO NOTHING`; a skipped insert is
   re-classified.
9. Debit entry (`-price`), audit `reward_redemption.redeemed`, read back,
   commit.

The global order stays membership -> one advisory business lock -> claim ->
`wallet_accounts FOR UPDATE` -> decision, as for task settlement. Redemptions
and task settlements on the same account serialize on the account row; neither
takes the other's advisory lock after an account lock, so there is no cycle.
Fulfillment (`POST /api/v1/rewards/redemptions/{id}/fulfillment`):
membership -> advisory `(33008, hashtext(redemption))` -> key classification ->
creator check -> already fulfilled (409 `REDEMPTION_ALREADY_FULFILLED`) ->
insert -> audit `reward_redemption.fulfilled` -> commit. No wallet access.

Responses are `{mutation, redemption}`: the immutable facts of that redeem or
fulfil, plus the current redemption. Same-key replays repeat `mutation`
exactly. Success is reported only after a confirmed commit; an unconfirmed
commit is 503 `RESULT_UNKNOWN`, retried explicitly with the same key. Responses
never include balances.

Reads: `GET /api/v1/rewards/redemptions?limit=&cursor=` (redemptions where the
caller is redeemer or creator) and `GET /api/v1/rewards/redemptions/{id}`
(404 for anyone else).

## 5. Web

- The partner's shop lists active products. Direct ones get a 兑换 button that
  opens a confirmation with the price and the **captured `{id, title, price,
  version}` snapshot**. Confirming submits that version.
- Request-mode products are shown as not yet redeemable.
- A redemption history section shows status and lets the creator mark a
  redemption fulfilled.
- Store actions keep their key on unknown/unsent results and resend only on an
  explicit retry; there is no offline queue. Session-generation guards apply.
- The wallet store reloads only after a confirmed debit; a reload failure never
  changes the confirmed result.

## 6. Verification

All runs below were executed by Claude Code on 2026-10-26 under the owner's M8
authorization, against usapp_test only. No main access, deployment, Git
publication, reset or cleanup. No served web dist was built.

| Step | Result |
|---|---|
| Web `pnpm run typecheck` | exit 0 |
| Web `pnpm run test:unit` | exit 0; 17 files, 154 tests (27 s), incl. new `rewards-redemption.spec.ts` (7) |
| Rollback-only pre-validation: 0015 body + `redemption_ledger_test.sql`, `product_ledger_test.sql`, `wallet_ledger_test.sql`, each as maintenance and as `SET ROLE usapp_api` | 6/6 exit 0; afterwards `0015` not registered and `reward_redemptions` absent |
| Test image build 1 | exit 1: two `is()` overload errors in `RedemptionDbTests.h` (test code); fixed with `pr::is` |
| Test image build 2 (compile check) | exit 0, 38 s, no warnings |
| `apply.ps1 -DryRun` | exit 0; 15 skips, only `0015_reward_redemptions` pending, sha256 matched |
| Apply 0015 + `-InitRoles` | exit 0 (5.7 s); privilege deviation query exit 0, 0 rows |
| Repeat `-InitRoles` | exit 0 (4.1 s), 16 skips; privilege deviation query exit 0, 0 rows |
| `schema_migrations` | `0015_reward_redemptions` checksum `306bdbac068b3fce3df03e86b30134c809b91ab3b88ceea92fca50efc626985e` (applied and frozen on usapp_test; not on main) |
| Full CTest run 1 | exit 8: unit and static_check passed; db_gated failed 6 assertions (see repairs); its printed task privilege policy line: ok, 803 checks, 0 mismatches/missing/unexpected |
| Full CTest run 2 (fresh image, build exit 0, 39 s) | exit 0, 12 s: unit, db_gated, static_check passed (passing output is not printed, so no per-assertion counts were captured) |
| SQL scripts on applied usapp_test, as `usapp_api` and as maintenance | 8/8 exit 0: task 35 groups, wallet 28, product 8, redemption 6 |

Repairs between CTest runs (test code only; 0015 unchanged):

- `RedemptionDbTests.h` referenced `audit_logs.actor_user_id`; the column is
  `actor_id` (0005). The failing-audit rollback case now inserts a blank
  action with the required `actor_type`.
- Two existing `WalletLedgerDbTests.h` cases used `REWARD_REDEMPTION` only as a
  generic fixture type (a debit-to-zero chain step; a "same key, other type"
  idempotency case). 0015 intentionally requires every `REWARD_REDEMPTION`
  operation to have exactly one linked redemption and one matching debit
  (deferred `UM003`), so those fixtures now use a plain operation and
  `REDEMPTION_REFUND` (+1) respectively; each case still tests the same wallet
  rule. The same change was made to the top-level fixture of
  `wallet_ledger_test.sql`. `product_ledger_test.sql` E3 and
  `wallet_ledger_test.sql` E9 now list the redemption tables in their
  maintenance TRUNCATE (without CASCADE) so the FK check passes and the
  append-only trigger is still what rejects.

Coverage of the M8 risks (db_gated block 22, real service on usapp_test):
creator redeem 403, other space 404, request mode 409, stale version 409 with no
effects; one debit/snapshot/audit; replay returns identical facts after
fulfilment, edit and archive; key reuse 409; fulfilment by redeemer 403, other
space 404, creator once, same-key replay, new key 409, no wallet change;
edit/archive queued ahead of a redeem on the product lock make it fail with no
operation, entry, row or audit; two products redeemed concurrently against an
insufficient balance give exactly one debit and no negative balance; same-key
concurrent redeems give one row and one replay; two-party cursor pages; SQL
guards UM002 (actor, stale, tampered snapshot), UM004, 42501, deferred UM003;
a failing audit rolls back the debit, row and operation. Sunshine entries and
`REDEMPTION_REFUND` operations stay at zero for redeeming spaces.

Committed fixture effect on usapp_test (isolated `@m3-fixture.test` spaces,
both CTest runs): 15 redemptions, 1 fulfilment, 15 `REWARD_REDEMPTION`
operations, 0 unlinked operations.

At the end of the test-only stage, not verified: live API/web workflows, main database, deployment, browser
behaviour, the HTTP controller layer end to end (covered by static checks and
service tests only), and an injected commit-unknown fault for redeem (the
ResultUnknown path follows the shared TxScope pattern; web retry is unit-tested).

## 7. Joint M7/M8 main rollout (Codex, 2026-10-26)

The owner authorized this guarded release and then assigned its execution to
Codex. No source, frozen migration, dependency or deployment configuration was
changed; no Git publication, cleanup, reset, or main business fixture was made.
M9/M10 were not started. Detailed joint release evidence is recorded here only.

- Preflight exit 0: running API used `usapp`; main contained exactly 0000-0013,
  whose checksums matched the local files. DryRun showed 14 skips and only
  0014/0015 pending. Local migration set contained exactly 16 files. Frozen hashes:
  - 0014: `5ee3ab1e38366308f325ae16d8060a0a78bd55f09f0363f49a076f5d2ab2f33e`.
  - 0015: `306bdbac068b3fce3df03e86b30134c809b91ab3b88ceea92fca50efc626985e`.
- Backup/rollback artifacts exit 0, 10.30 s:
  `backups/before-m7m8-20261026_193215/` (new directory; overwrite guards).
  - `usapp_before_0014_0015.dump`: 370966 bytes, SHA-256
    `d41a6603ef8a53865cbb757238f4a75b9c3fd06328e117d6b3d308f198c25360`.
  - The exported host dump was copied back and restored into
    `usapp_m7m8_restore_20261026_193215` using template0 and
    `pg_restore --exit-on-error --single-transaction`. All 20 public base tables
    matched before/after/restore; this is a count check, not row-by-row proof.
    Baseline: 7 tasks, 3 completions, 2 progress events, 4 accounts, 3 operations,
    5 entries, 23 audits, 14 migrations; M6 proposals/decisions/closures all zero.
  - Running M6 image
    `sha256:812506c9e9a9bdef1e00d7c58d370e435128eac2325a7778e35223d0604c66be`
    tagged `usapp-api:before-m7m8-20261026_193215`; inspect confirmed the tag.
  - `web-dist/` copied from `caddy:/srv/web`: 19 files, 264264 bytes;
    index.html `40f91c247069b01400f388c33dd3520a8bbe5822ba03da99b2fbecaaf1404aa1`,
    sw.js `a87f560e34904fc356c4ee8bc63bf634794a9bb88c83d1ad4e3f308b6743ccdc`.
  - `rollback-info.json` records these artifacts and table counts. Scratch DB,
    exported dump, count-query file and container `/tmp` copies are retained.
    Additional privilege query: `/tmp/usapp_m7m8_privileges_20261026_193215.sql`.
- Main migration: immediate DryRun recheck matched; apply + InitRoles exit 0,
  7.66 s including privilege query, 14 skipped / 2 applied. Table/column privilege
  deviations: 0 rows. Repeat InitRoles exit 0, 4.50 s including query, 16 skipped /
  0 applied; deviations: 0 rows. All 16 schema_migrations checksums match files.
- Backend build exit 0, 8.77 s (current-source builder layers reused).
  Image `sha256:0e61cc6d31585ddc448b953750c46cabc093253904a1b5e18be9fc5c976400d4`.
  Deployment with `up -d --no-deps --no-build --wait --wait-timeout 120 api`
  exit 0, 8.49 s. Container
  `088ce2982523333ac6ec23b3c2ca6e6724cd20efc98501914d8e91fc655f9629`
  uses that image and is healthy. Fresh startup at 11:33:28 UTC:
  wallet `role=usapp_api ok=true checks=187 mismatches=0 missing=0 unexpected=0`;
  task `role=usapp_api ok=true checks=803 mismatches=0 missing=0 unexpected=0`.
- Web build after backend health: `corepack.cmd pnpm run build` exit 0, 31.99 s
  (vue-tsc, tsc, Vite, PWA injectManifest); precache 21 entries / 209.38 KiB;
  dist 22 files / 286280 bytes. Caddy-served hashes match local dist:
  index.html `351d50308b2503cfaf6de75df3e6dc115f9ea284478a1a880281471fd9941c7b`;
  sw.js `7d5fd21f36483ebcf75c6a41cdac2814582a6ec40513f220ec2cf61b8459398c`.
- Final status exit 0: API and PostgreSQL healthy, Caddy up, 16 main migrations;
  products/redemptions/fulfillments all zero. No real debit or fulfillment was
  performed. Previously passed automated suites were not unnecessarily rerun.
- Execution limitation: the first sandboxed Docker read failed with a named-pipe
  permission denial. The authorized escalation succeeded; no OS ACL or project
  permission settings were changed. No release stage failed afterwards.

Remaining: live product/redeem/fulfill HTTP workflows, browser/PWA update
acceptance, rollback exercise, and formal live acceptance remain unverified.
Code rollback uses the saved image/web only; 0014/0015 remain applied and M6
cannot display new products or redemptions. Rollback never reverses debits,
fulfillments or audits. Restoring the old dump over main would discard later
data and was not performed. No commit or push was made.
