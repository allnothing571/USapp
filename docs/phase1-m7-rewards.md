# Phase 1 M7: Reward Shop Products

Status (2026-10-26): **the owner explicitly assigned and approved M7 reward-shop
implementation and automated verification on usapp_test to Claude Code.** This
supersedes the earlier CLAUDE.md wording that M7 was unapproved. A price range of
1-999 flowers was approved on 2026-10-26. Main rollout, deployment and Git
publication are not part of this assignment.

Progress: **implemented and test-verified on usapp_test (Section 6); 0014 is
applied/frozen on test and main. Joint M7/M8 API/web rollout completed by Codex
on owner authorization, 2026-10-26.** Release evidence and live-check limits:
[phase1-m8-redemption.md](phase1-m8-redemption.md), Section 7. No Git publication.

Inputs: `docs/phase1-plan.md` (Sections 4-6, D2, D13, D14, C5, M7/M8 rows and
the indicative `reward_products` model), the M3-M6 conventions recorded in
`docs/phase1-m3c-design.md` and `docs/phase1-m6-design.md`.

## 1. Approved scope and rules

| Topic | Rule |
|---|---|
| Who | Each member creates products **for the partner**, never for themselves. Creator and owner (the partner who will redeem) are fixed at creation. |
| Operations | Create, list, detail, edit, archive. No hard deletion. |
| Visibility (D14) | The creator sees their products, active and archived. The owner (partner) sees active products only, and detail applies the same rule (an archived product reads as not found). Other spaces: not found. |
| Archive (D13) | One-way. An archived product cannot be edited or archived again. Future redemption snapshots are unaffected. |
| Price | Integer 1-999 flowers inclusive. No free products, no sunshine pricing. |
| Text | Same title/description validation and Unicode behaviour as tasks (title 1-40 code points, description 0-500, the same normalization). |
| Redemption mode | `direct` or `request`, stored for later milestones only. **No redemption, debit, reservation, fulfillment or refund in M7; M7 writes no wallet data.** |
| Images, navigation | No images (C5). No M10 redesign; the home page gets one shop entry and the router one `/rewards` route. |

## 2. Model (migration `0014_reward_products.sql`)

Append-only, like the M6 task model, so no UPDATE grant is needed.

- `reward_products` holds the immutable identity: `id`, `couple_id`,
  `creator_id`, `owner_id` (a member FK each, `creator_id <> owner_id`),
  `created_at`.
- `reward_product_revisions` holds one row per mutation, PK
  `(product_id, version)`:
  - `actor_id`, `action` (`create` | `edit` | `archive`);
  - the full term set after the mutation: `title`, `description`,
    `price_flowers` (1-999), `redemption_mode`, `status` (`active` |
    `archived`);
  - `idempotency_key`, `request_fingerprint`, `created_at`.
- The current state is the highest version. A revision is the immutable fact of
  its mutation, so committed replay rebuilds from it after any later edit or
  archive. A future redemption copies the terms of a specific version as its
  snapshot.
- Database protections (SQLSTATE class UR):
  - UR001: both tables are append-only (UPDATE/DELETE/TRUNCATE).
  - UR002 revision guard, under the per-product advisory lock
    `(33007, hashtext(product))`:
    - the actor is the creator of a product in the same couple;
    - versions are contiguous (1 for `create`, otherwise previous + 1);
    - nothing is accepted after an archived revision;
    - `archive` keeps the previous terms; `created_at` does not go backwards.
  - UR003 deferred: every product has exactly its version-1 `create` revision
    in the same transaction.
  - Shape CHECKs mirror the task title/description CHECKs.
- Grants: SELECT and INSERT for `usapp_api` on both tables; nothing for
  `usapp_push` or PUBLIC.
  - Re-asserted in `init-roles.sql` Section 12.
  - Covered by the existing startup privilege policy, whose object list grows by
    two tables; its log label stays "task privilege policy".
  - Added to `task_privilege_deviation.sql`.

## 3. Service and contract (0.11.0, additive)

Endpoints (all behind AuthFilter; POST requires Origin, CSRF and
`Idempotency-Key`):

- `POST /api/v1/rewards/products`: create. Body `{title, description?,
  price_flowers, redemption_mode}`; the owner is the caller's partner
  (409 `PARTNER_REQUIRED` without one).
- `GET /api/v1/rewards/products?limit=&cursor=`: products visible to the caller,
  cursor-paginated in the immutable `(created_at DESC, id DESC)` order (limit
  1-100, default 50; `next_cursor` is `<created_at>_<id>`, strictly validated;
  invalid limit/cursor -> 400 without a query). Visibility is applied on every
  page. Pages are **not a snapshot**: an edit or archive between requests can
  change which products appear.
- `GET /api/v1/rewards/products/{productId}`: detail (visibility as above).
- `POST /api/v1/rewards/products/{productId}/revisions`: edit, a full term set
  plus `expected_version`.
- `POST /api/v1/rewards/products/{productId}/archive`: `{expected_version}`.

Transactions (one `TxScope`, READ COMMITTED; same lifecycle and failure-label
conventions as the task services):

1. `couple_members` `FOR SHARE` for the caller (joined to the partner for
   create, or to the product for a mutation).
2. Advisory lock `(33007, hashtext(product))` for mutations.
3. Key classification: same actor+key with the same fingerprint replays (200)
   after any later mutation; a different fingerprint is 409
   `IDEMPOTENCY_KEY_REUSED`.
4. State checks:
   - a non-creator mutation: 403 `PRODUCT_FORBIDDEN` (owner) or 404
     `PRODUCT_NOT_FOUND`;
   - archived: 409 `PRODUCT_ARCHIVED`;
   - `expected_version` not equal to the current version: 409
     `PRODUCT_VERSION_CONFLICT`.
5. Revision `INSERT ... ON CONFLICT DO NOTHING`; a skipped insert is
   re-classified.
6. Audit in the same transaction (`reward_product.created|edited|archived`;
   detail holds ids, version, price and mode only, never text).
7. Read back, then commit. Success only from a confirmed commit; an unconfirmed
   commit returns 503 `RESULT_UNKNOWN`.

Responses are `{mutation, product}`: `mutation` holds the immutable facts of
that revision; `product` is the current state at read time. Replays guarantee
identical `mutation` facts, not identical whole bodies.

Fingerprints: SHA-256 of `reward-product-<action>:v1:` plus the product id,
expected version and length-prefixed fields.

## 4. Web

- `api/rewards.ts`, types and error messages.
- `stores/rewards.ts`: one pending mutation per product plus one create action.
  The key is kept on an unknown or unsent result and resent only on an explicit
  retry; a definitive 4xx ends the action; `reset()` on session change.
- Optimistic concurrency (review repair): editing captures an immutable `{id, title,
  version}` snapshot when it starts, and the draft always submits that version.
  The first archive click captures the version that the confirming click
  archives. Refreshes, edits elsewhere and removal from the list never retarget a
  draft; drift is shown, and the server's 409 appears as a notice. The store
  paginates (load more, de-duplication by id, newer loads discard older
  responses).
- `views/RewardsView.vue`, at `/rewards`:
  - the partner's active products ("对方为我准备的");
  - my products with active/archived badges;
  - create form; edit form (prefilled, sends `expected_version`); archive with
    confirmation.
- The home page links to it.

## 5. Tests (lean, existing helpers only)

| Layer | Cases |
|---|---|
| Unit | Request parsing (price bounds, mode, text rules shared with tasks, unknown fields); fingerprints; error mapping; response separation; policy object list. |
| db_gated block 21 | Create/list/detail visibility (creator vs owner vs other space; archived hidden from owner); edit with a stale version; concurrent edits on one version (exactly one wins); archive one-way and edits after archive; replay of create/edit/archive after later mutations; key reuse; exactly one audit per committed mutation and none for rejections; no wallet rows for the space; direct-SQL guards (non-creator revision, version gap, update/delete). |
| SQL | New `database/tests/product_ledger_test.sql` in both roles; task/wallet scripts rerun unchanged. |
| Web | Store create/edit lifecycle (unknown -> same-key retry; a stale version ends with a notice); view visibility and archived badges. |

## 6. Verification (2026-10-26, Claude Code, usapp_test only)

**Implemented and test-verified on usapp_test; not deployed; main not accessed.**
The only pre-existing assertions changed were the contract version (`0.10.0` ->
`0.11.0`, three StaticCheck lines) and the task-policy unit matrix listing the
two new tables. No test was removed or weakened.

- Web: `pnpm run typecheck` exit 0, 13.5 s; `pnpm run test:unit` exit 0, 15 files
  / 140 tests (135 existing + 5 new in `rewards.spec.ts`), 31.1 s. No dist build.
- Rollback-only pre-validation before freezing: the 0014 body plus
  `product_ledger_test.sql`, in one transaction ending in ROLLBACK on usapp_test,
  exit 0 with 8 groups as maintenance and as `SET ROLE usapp_api`. Afterwards
  0014 was not registered and `reward_products` did not exist.
- Compile check before freezing: test image exit 0, 37.4 s, no errors or
  warnings.
- DryRun: exit 0, 2.9 s; 0000-0013 skipped with matching checksums, only
  `0014_reward_products` pending.
- Apply + InitRoles: exit 0, 5.8 s. **0014 is frozen**, SHA-256
  `5ee3ab1e38366308f325ae16d8060a0a78bd55f09f0363f49a076f5d2ab2f33e` (file =
  `schema_migrations`). Table/column privilege deviation query (now including both
  product tables): 0 rows. Repeat InitRoles: exit 0, 4.0 s, 15 skipped; deviation
  query 0 rows.
- Fresh test image: exit 0, 7.1 s (same sources as the compile check). Full
  CTest **exit 0**, 12.2 s: unit 0.03 s, db_gated 9.40 s (not skipped; gate
  requires 0000-0014), static_check 0.01 s; 3/3 on the first run, no repairs.
- Block 21 effects (one run, from zero): 6 products, 11 revisions, 11
  `reward_product` audits. That is exactly one audit per committed mutation; the
  rejected requests (forbidden, stale, archived, key reuse, lost race) left none.
  No wallet operation or account exists for the M7 spaces.
- SQL compatibility on the applied schema, both roles, all exit 0 and rolled
  back: `task_ledger_test.sql` 35 groups; `wallet_ledger_test.sql` 28 groups;
  `product_ledger_test.sql` 8 groups.
- Startup privilege policy: expected **667** checks after 0014 (537 + 2 objects
  x 38 + 3 x 18 columns). **Not observed;** no API runtime started.

Permanent usapp_test fixture growth per successful db_gated run (isolated
`@m3-fixture.test` spaces, no reset): 6 spaces, 6 products, 11 revisions,
11 audits.

Release prerequisites (separate, guarded, owner-delegated as for M6):

1. Main backup with scratch restore and rollback artifacts.
2. DryRun showing only 0014 (hash above), then apply + InitRoles and the privilege
   query, twice.
3. New API whose startup shows task policy 667 ok and wallet 187 ok; only then
   build the web dist and compare served hashes.

Live creator/owner workflows and browser checks are still needed; no
acceptance is recorded.

### 6.1 Codex review repairs (2026-10-26)

Blockers repaired:

1. Edit/archive submitted the latest list version. Fixed with immutable version
   snapshots.
2. The list was silently truncated at 200. Replaced by bounded cursor
   pagination.
3. Drafts were not cleared on account switch. The view now clears create/edit
   drafts, version snapshots, archive confirmation and local errors when the
   session identity changes, and ignores late results through a view
   generation.

No migration change: 0014 bytes and hash unchanged (`5ee3ab1e...f33e`); no new
migration; no usapp_test schema operation. Contract 0.11.0 was amended before
any release: the list gained `limit`/`cursor` parameters, `next_cursor`,
`maxItems: 100` and a 400 response.

- Web: typecheck exit 0, 5.4 s; `test:unit` exit 0, 16 files / 147 tests (7 new
  in `rewards-repairs.spec.ts`), 12.5 s. New cases:
  - v1 draft -> refresh to v2 -> submit sends `expected_version: 1` and shows
    the conflict;
  - a draft survives its product leaving the list;
  - archive confirmed at v1 -> refresh to v2 -> archives v1 and shows the
    conflict;
  - multi-page load-more with de-duplication; a newer load discards an
    in-flight load-more;
  - deferred old-account create success and edit failure after an account
    switch leave no notices, actions or list changes;
  - the view clears drafts, snapshots, confirmation and errors on account
    switch.
- Backend: fresh test image exit 0, 41.4 s. Full CTest exit 0, 12.1 s (unit,
  db_gated 9.53 s not skipped, static_check). That build printed one compiler
  warning from the new unit test (a range-for copy); it was fixed and the image
  rebuilt warning-free (exit 0, 38.4 s), with full CTest again exit 0, 11.1 s
  (unit 0.02 s, db_gated 8.87 s, static_check 0.01 s).
  - Unit: strict cursor encode/decode, including rejection of malformed,
    uppercase and over/under-length cursors.
  - db_gated: four products sharing one `created_at` (one archived) plus a newer
    one. Creator pages of 2 return all five in `(created_at DESC, id DESC)`
    order over 3 pages. Owner pages of 1 return the four active ones over 4
    pages, never the archived one. An exactly-full page has no `next_cursor`;
    limit 0 is rejected.
- Fixture growth per db_gated run is now 11 products, 17 revisions and 12
  audits (observed 6/11/11 -> 17/28/23 on the first repaired run). The
  pagination case uses 5 small products, not hundreds.
- Task/wallet/product SQL scripts were not rerun: no database object, grant or
  migration changed.

## 7. Limits and risks

- Product lists are cursor-paginated but not snapshot-consistent: concurrent edits
  or archives can make a product appear or disappear between pages (documented in
  the contract).
- The startup policy object list now includes the product tables (expected 667
  checks). A runtime older than this policy simply does not check them, and an M7
  runtime refuses to start on a database without 0014 (the tables would be
  missing).
- Products are versioned per mutation; no redemption references them yet. M8
  snapshots must copy the terms of a specific version.
