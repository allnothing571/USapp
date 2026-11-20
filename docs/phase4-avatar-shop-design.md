# Phase 4: sunshine shop and character customization

Latest B4 (Codex, 2026-11-10): user shop, wardrobe and home character UI implemented against 0.44.0, with 14 placeholder SVG files and a closed nine-entry asset manifest. Final offline web verification: 920 tests, both typechecks, isolated production build and asset hashes passed; old dist and 47 frozen migrations unchanged. No registry/catalog/default enrollment occurred. Browser checks were halted by the computer-use tool before navigation; visual, real API and device acceptance remain unpassed. Full evidence: [B4 web implementation](phase4-b4-avatar-web.md). B5 and final art remain separate; NOT DEPLOYED.

Latest B5 (Codex, 2026-11-10): local-admin forms implemented, 30 focused / 63 full admin tests, both typechecks and isolated build passed. No backend or contract change and no enrollment. B4 manual follow-up is owner-reported partial evidence; remaining checks stopped at the owner's request. See [B5 implementation and evidence](phase4-b5-avatar-admin.md) and B4 Section 5.

Status:
- **Design:** recorded 2026-11-09 from the read-only design in the conversation.
- **Owner approval:** the B1 rules below, approved the same day.
- **Batches (current 2026-11-10):** B1-B3 backend, B4 user web and B5 local-admin catalog forms are implemented with automated evidence. Registry enrollment, formal art and release acceptance remain pending. See [B5 evidence](phase4-b5-avatar-admin.md).
- **NOT DEPLOYED.** Fake-based automated evidence and limited owner-reported B4 browser checks only; real API/device/release acceptance remains pending.

Sources:
- `app开发方案V1.docx`: sections 5.2, 19-22, 36, 39, 43 and 52.3.
- The existing ledger (`0006`), redemption (`0015`/`0016`) and admin (`0005`, `AdminFilter`) designs.

## 1. Approved B1 rules (owner, 2026-11-09)

1. Six categories (`hair`, `clothes`, `expression`, `hat`, `handheld`, `accessory`), and one equipped item per category.
2. Hair, clothes and expression are required. Hat, handheld item and accessory may be empty.
3. Starter items are free and never create wallet entries.
4. Exactly one deterministic default item for each required slot, even if several free items exist in that category.
5. Paid-item prices are integer sunshine amounts from 1 to 9999.
6. Unlisting prevents future purchases. It preserves existing ownership and equip eligibility.
7. Items are never physically deleted.
8. Assets are bundled and versioned, and selected through known asset keys. Admin asset upload is deferred.
9. Catalog administration is authorized, audited, and restricted to the existing admin listener and authentication boundary.
10. The admin may see per-item ownership counts, not individual inventories.
11. Names may change and asset defects may be corrected. A materially different visual appearance must be a new item, not a silent replacement for existing buyers.

Earlier owner decisions this design keeps:
- **D2 bounds:** a sunshine entry may not exceed ±100,000.
- **D21:** a user sees only their own balances.
- **Posting paths:** the only authorized ones are task settlement, redemption debits and M9 refunds. `AVATAR_PURCHASE` is a new path, still to be built in B2.

## 2. Corrected design

### 2.1 Starter configuration and fallback (correction 1)

- **Starter items:**
  - A starter item is a catalog item whose immutable `starter` flag is true, with no price.
  - It is equip-eligible for every user, and is never "owned" through a purchase or a wallet entry.
  - Creating one is an audited admin act. A paid item can never become a starter, and a starter can never gain a price.
  - So the starter flag can't turn a paid item into a free grant.
- **Starter configuration (`avatar_starter_configs`):**
  - Append-only and globally versioned.
  - Each version names exactly one item for each required slot: hair, clothes and expression. Every one must be a starter item of that category.
  - The current configuration is the highest version.
- **Deterministic defaults:** the default for a required slot is exactly the item the current configuration names. It is never "the latest item" or any other rule over the catalog. Items can't be deleted and their category and starter flag can't change, so a stored configuration can't become invalid later.
- **Listing:** unlisting a configured starter only hides it from the catalog list. It stays the default until a new configuration version names another item.
- **Incomplete configuration:**
  - The only incomplete state is "no configuration version exists", for example on a fresh install.
  - The appearance read then answers 200 with `complete: false`, `starter_version: null` and `null` for every slot. It doesn't invent defaults, and the database doesn't refuse.
  - Optional slots (hat, handheld item, accessory) are always `null` in B1.
- **Fallback in later batches (B3/B4):** an empty required slot, or one whose saved item is unusable, falls back to that slot's configured default. An empty optional slot stays empty.

### 2.2 Layering (correction 2)

Every layer covers one shared transparent canvas, with one anchor. Rendering stacks layers in this fixed order, rear first:

| z | Layer | Source |
|---|---|---|
| 1 | accessory with position `behind_body` (wings, back bow) | accessory `main` |
| 2 | hair `back` (long hair behind the shoulders) | hair `back` |
| 3 | base body | not a catalog category in B1; art decision (V1 §52.3) |
| 4 | clothes | clothes `main` |
| 5 | accessory with position `neck` (scarf, necklace) | accessory `main` |
| 6 | expression (face) | expression `main` |
| 7 | accessory with position `face` (glasses) | accessory `main` |
| 8 | hair `front`, or `front_under_hat` when the equipped hat has `covers_hair_top` | hair |
| 9 | hat | hat `main` |
| 10 | accessory with position `above_hat` | accessory `main` |
| 11 | handheld item, in front of the body and sleeves | handheld `main` |

**Enforced by the registry, the database and guards:**
- each category's exact layer set: hair has `back`, `front` and `front_under_hat`; every other category has `main`;
- a SHA-256 per layer file;
- one format per asset (`svg` or `webp`);
- `covers_hair_top` present for hats only;
- `accessory_position` present for accessories only, from a closed set;
- the asset category matching the item category.

### 2.3 What layering cannot guarantee (correction 3)

The fixed order and metadata make every combination *renderable*: each slot has a defined layer and position. They do not prove that every combination *fits visually*. Examples:
- sleeve shapes against a handheld item;
- a hat brim against a voluminous `front_under_hat`;
- a face accessory against an expression.

Visual fit is checked by human art review when assets are added to the registry. It is not a database property and the API doesn't claim it.

### 2.4 Purchase key claim (correction 4; superseded in detail by Section 6.2, implemented in B2)

The purchase transaction must classify an existing operation **before** any mutable catalog check. Steps:

1. Take a per-(user, key) transaction lock.
2. Look up `wallet_operations` by `(actor, 'AVATAR_PURCHASE', key)`. If found: the same fingerprint replays the original purchase snapshot, and a different one answers `IDEMPOTENCY_KEY_REUSED`. In both cases no catalog state is read.
3. Only then lock the item, read its current revision, and check listed, not starter, expected version and not already owned.
4. Then the account row lock, the balance check and the writes.

So a lost-response retry gets its original answer even after the item was unlisted, repriced or revised in between. A concurrent same-key retry waits on step 1 and then replays.

### 2.5 Asset correction versus a new appearance (correction 5)

- **Families:** every registry asset belongs to an appearance *family* (`<category>.<name>`) and has a revision. Its key is `<family>.v<revision>`, for example `hair.bob.v2`.
- **Files:** a bundled file is named `<asset_key>.<layer>.<format>`. Its bytes are never overwritten; a correction is a new revision with new file names.
- **Database-enforced:**
  - Registry rows are append-only and keyed by revision; the guard requires contiguous revisions.
  - Within a family, the category, format, `covers_hair_top` and `accessory_position` can't change.
  - An item is bound to one family at creation, and each family belongs to at most one item.
  - A revision may keep its asset or move it forward within the item's family. Moving backwards or to another family is refused.
- **Admin review:** whether a new family revision is a *defect correction* (stray pixel, alignment, colour fix) or a *materially different appearance*.
  - The database can't judge pictures, so the registry entry for a corrected revision must be reviewed before it is added.
  - A different appearance gets a new family and therefore a new item, which existing buyers don't own.
- **Renames:** allowed as revisions.

### 2.6 Authorization boundary: database versus application

- **The database enforces independently:**
  - append-only tables (UA001);
  - registry shape and family rules (UA002);
  - item and revision rules (UA003): contiguous versions, an active admin user, matching asset category and family, non-decreasing asset revision, starter↔price rules, the price range, and every item having revision 1 in its own transaction;
  - starter configuration rules (UA004);
  - `usapp_api` may only SELECT the registry; only the maintenance role writes it.
- **Only the application enforces:**
  - that a catalog write came through the admin listener: peer CIDR, the admin-surface header, exact admin Origin, `X-Requested-By`, an admin session and its CSRF token;
  - which admin is acting.
- Both listeners connect as `usapp_api`. A defect on a user route could therefore write catalog rows naming any active admin. This residual risk is mitigated only by route separation and tests.

## 3. B1 scope (implemented)

- **Migration `0044_avatar_catalog`:**
  - registry `avatar_assets`, read-only to the application;
  - `avatar_items`, `avatar_item_revisions` and `avatar_starter_configs`;
  - guards UA001-UA004, and three write functions that classify the key first and audit atomically;
  - new advisory lock classes 33027 (item), 33028 (starter configuration) and 33029 (admin key).
- **User reads (session required):**
  - `GET /api/v1/avatar/catalog`: listed items, current revision.
  - `GET /api/v1/avatar`: the caller's appearance. In B1 this is truthfully the starter defaults, `source: "starter_defaults"`. It has no ownership or purchase fields.
- **Admin (admin boundary):**
  - Reads: `GET /api/admin/v1/avatar/assets`, `GET /api/admin/v1/avatar/items`, `GET /api/admin/v1/avatar/items/{itemId}` (with revisions) and `GET /api/admin/v1/avatar/starter`.
  - Writes, with an `Idempotency-Key` header: `POST /api/admin/v1/avatar/items`, `POST /api/admin/v1/avatar/items/{itemId}/revisions` (also how an item is listed or unlisted) and `POST /api/admin/v1/avatar/starter`.
  - Ownership counts are not reported: B1 has no ownership. B2 adds them.
- **Errors:**
  - New: `AVATAR_ITEM_NOT_FOUND` (404), `AVATAR_VERSION_CONFLICT` (409), `AVATAR_ASSET_INVALID` (400), `AVATAR_STARTER_INVALID` (400).
  - Reused: `VALIDATION_ERROR`, `IDEMPOTENCY_KEY_REUSED`, `RESULT_UNKNOWN`.
- **Out of scope:** no wallet change, purchase, ownership, equip write or web, and no real assets. The production registry is empty until reviewed art is bundled.

## 4. Remaining batches and their blockers

- **B2 (purchase):**
  - Needs owner approval of the `AVATAR_PURCHASE` posting path, plus the remaining purchase decisions: one purchase per item (S2), buying and equipping as separate actions (S7), and no refunds, gifting or trading (S12).
  - Its key claim follows 2.4.
  - Adds per-item ownership counts to the admin list.
- **B3 (equip):** needs the save-whole-outfit semantics and the fallback rules in 2.1 confirmed.
- **B4 (web):** needs the base body and art direction (V1 §52.3), placeholder layers, and the PWA caching choice.
- **B5:** admin catalog forms.

## 5. B1 implementation record (Claude Code, 2026-11-09)

**Migration:**
- `0044_avatar_catalog`, SHA-256 `8f1930eadf26f0a5ebd89f77d1e199d85e6b8802ed25d169d9e6eeab3cef7b1f`.
- Additive: four tables, guards UA001-UA004, write functions `usapp_avatar_item_create`, `usapp_avatar_item_revise` and `usapp_avatar_starter_set`, and the key classifier `usapp_avatar_admin_key`.
- No SECURITY DEFINER and no data. The next migration number is 0045.

**Contract:** 0.41.0 -> 0.42.0, additive. It has two user operations, seven admin operations and four error codes.

**Transactions:** each admin write is one transaction holding one function call. The function:
1. takes the per-(admin, key) lock (33029);
2. classifies an existing use of the key across items, revisions and starter configurations;
3. then takes the item or configuration lock, checks, inserts, and writes the `audit_logs` row.

The service then reads the result in the same transaction and commits. A refused statement answers 500 (a definite failure). A failed commit, begin, or the test-only `FailBeforeCommit` fault answers `RESULT_UNKNOWN`; a retry with the same key and body then replays the original write if it did commit, or writes it once if it did not. Only the second case is tested, through the fault before commit.

**Authorization:**
- User routes are GET only, behind `AuthFilter`. The three POSTs are admin routes behind `AdminFilter`; a static check enforces this.
- The admin's identity is taken only from the admin session.
- What the database independently enforces, and what it cannot, is listed in Section 2.6.

**Files:**
- **New:** `database/migrations/0044_avatar_catalog.sql`, `database/tests/avatar_catalog_test.sql`, `services/api/src/services/AvatarService.{h,cc}`, `services/api/src/controllers/AvatarController.cc`, `services/api/tests/AvatarCatalogDbTests.h`, `Claude outputs/b1-avatar-verify.ps1` and this document.
- **Changed:**
  - `database/init-roles.sql` (section 36);
  - `database/tests/task_privilege_deviation.sql`;
  - `services/api/src/db/TaskPrivilegePolicy.cc`;
  - `services/api/src/core/Errors.{h,cc}`;
  - `services/api/CMakeLists.txt`;
  - `services/api/tests/{CMakeLists.txt,DbGatedTest.cc,UnitTest.cc,StaticCheckTest.cc,ReminderLiveApiTest.h}`;
  - `contracts/openapi.yaml`.
- No web, admin app, push worker or dependency change, so no web tests were run.

### 5.1 Automated verification (fakes and disposable instances only)

Each run used:
- the existing local image;
- an internal network with no published port;
- tmpfs PostgreSQL 17;
- one-time credentials.

Container ownership was checked before each step. The asset registry rows were fake metadata added through the maintenance connection; no asset file exists.

| Run | Result |
|---|---|
| Prevalidation (`usapp-b1-657a6d5d7d`) | 0044 plus its ledger inside one transaction that rolled back: maintenance pass 43 cases, application pass 9 cases. 0044 objects absent afterwards |
| 1 (`usapp-b1-791a952386`) | Prevalidation, apply (45 checksums match, eraser violations 0), privilege deviations 0 after apply and after repeat apply with InitRoles. Nine SQL ledgers passed: `avatar_catalog_test`, `wallet_ledger_test`, `phase3_notification_test`, `notification_choice_test`, `memory_ledger_test`, `album_ledger_test` and `shared_plan_ledger_test` in both roles; `timestamp_guard_test` and `push_dispatch_test` in both modes. Build passed. **The targeted avatar suite failed one assertion** (`AvatarCatalogDbTests.h:302`): a test-fixture error. The fault-injection case for the starter configuration passed a paid accessory item for all three slots, so the function correctly answered `starter_invalid` before reaching the fault. The fixture now creates real starter items for each slot; no product code changed. The run stopped there |
| 2 (`usapp-b1-a81041af61`) | The same database stages passed. Targeted `avatar_catalog_db` on a fresh database: 151 assertions, 8 cases, including the incomplete-configuration case. Live API through the real binary on both listeners: 443 assertions. **Full CTest 16/16 in 94.45 s** (`unit` 2833, `db_gated` 3573 in 13.87 s, `static_check` 1252, `avatar_catalog_db` 139, every other target passed). Post-checks: avatar wallet operations 0, unaudited revisions 0, the earlier deletion/notification consistency checks 0, privilege deviations 0 after CTest |

**What the tests cover:**
- **Starter determinism:** a later starter and an unlisted configured starter change nothing.
- **Invalid configuration:** paid item, wrong category, unknown id, stale version.
- **Incomplete configuration:** the targeted run only. Under CTest the same case reports `skipped` because a configuration exists by then.
- **Immutability:** 42501 for the application role; UA001 for maintenance.
- **Item rules:** one family per item; revision conflicts, including two concurrent revisions with exactly one winner; asset corrections only forward within a family.
- **Idempotency:** replay, changed payload, a key reused across write kinds.
- **Unlisting:** hidden from the catalog, kept in the admin list.
- **Invalid metadata:** URL and path keys, wrong layers, hash, format and hints.
- **Atomicity:** the fault before commit leaves no item, revision, configuration or audit row.
- **Admin state:** a disabled admin writes nothing.
- **Both listeners:**
  - no admin session → 401;
  - a user session → 401;
  - no surface header → 403;
  - foreign Origin → 403;
  - no `X-Requested-By` → 403;
  - wrong CSRF token → 403;
  - no key → 400.
- **Admin listener with a session:** create, replay, key reuse, conflict, unlist and an audit row.
- **Users:**
  - the global catalog is visible to members of different couples;
  - their own appearance has no ownership fields;
  - `/api/v1/avatar/{userId}` → 404;
  - an admin cookie is not a user session;
  - POSTs to user routes write nothing.

**Not verified:** any browser, device, cloud, restore or rollback acceptance, and real art. **NOT DEPLOYED.** Nothing is committed.

**Behaviour to note:**
- The public listener also serves the admin routes when the surface header is present and the peer is in the admin CIDRs. This is the existing design, which relies on the Caddy sites; it was not changed.
- A fresh production registry is empty, so no item can be created until reviewed art is bundled and registered by the maintenance role.

## 6. B2: sunshine purchases and owned items

### 6.1 Owner approval (2026-11-09)

- The new `AVATAR_PURCHASE` wallet posting path is authorized.
- Sunshine only; flowers are never debited.
- One purchase per user per item.
- Buying does not equip the item.
- No refunds, gifting or trading in this batch.
- B1 prices are kept: whole sunshine amounts from 1 to 9999.
- Unlisted items cannot be bought; existing ownership stays valid.
- Starter items are free and never create purchases or wallet entries.

### 6.2 Corrected purchase sequence

The Section 2.4 sketch was ambiguous on two points, corrected here:

1. **Where the key is classified.** It is classified against `avatar_purchases` (unique by user and key), not by scanning `wallet_operations`. The wallet operation carries the same key and fingerprint and is held to them by the purchase guard.
2. **Why no new same-key row can appear after classification.** Every same-key request first takes the per-(user, key) lock, so a classification made after acquiring it can't be overtaken. Each statement of the function runs on a fresh READ COMMITTED snapshot, so the classification sees anything committed while the lock was awaited.

All of the following runs inside one function, `usapp_avatar_purchase`, in the caller's transaction:

1. **Key lock** `33030` on (user, key). A concurrent same-key request waits here.
2. **Classification**, before any catalog read: a purchase with this key and the same server-side fingerprint replays its original facts. Any other request under the key answers `key_reused`. No catalog, ownership or balance state is read on either path, so a retry after a reprice or unlist still replays.
3. **Membership:** `couple_members` row `FOR SHARE`. No couple answers `no_space`, the same rule the wallet read uses (`SPACE_NOT_FOUND`).
4. **Item lock** `33027` (`'item:' || id`), the same lock B1 revisions take. A purchase and a catalog revision of that item are serialized; whichever waits sees the other's commit.
5. **Catalog checks**, in this order:
   - unknown item → `not_found`;
   - starter → `starter`;
   - current revision not listed → `unlisted`;
   - current version differs from the expected one → `conflict` (with today's version and price).
6. **Ownership:** an existing purchase of this item by this user answers `already_owned`. This includes another key's purchase that committed while this request waited on the item lock. The unique constraint backs it up.
7. **Wallet account:** create the user's sunshine account if missing, then `SELECT … FOR UPDATE` on it. This is the existing wallet serialization: task settlement, redemptions and refunds take the same row lock for their own accounts.
8. **Balance** from the latest entry; less than the price answers `insufficient` and nothing is written.
9. **Writes:**
   - the operation, with key and fingerprint;
   - the immutable purchase snapshot (item, version, name, category, asset key, price);
   - one sunshine ledger entry of exactly −price, the next seq and the balance afterwards;
   - an audit row.
10. **The purchase's four deferred checks run immediately, by name, then return to deferred:** `avatar_purchases_debit_valid`, `wallet_operations_avatar_purchase_valid`, `wallet_ledger_entries_avatar_purchase_valid` and `wallet_operations_require_entries`.
    - A violation therefore fails the statement (a definite rollback, HTTP 500) instead of the commit, where it couldn't be told apart from a lost connection.
    - *Correction during implementation:* the first draft used `SET CONSTRAINTS ALL IMMEDIATE`. That changed the caller's whole transaction, and the ledger caught it (prevalidation attempt 1, Section 6.6).

**Lock order:** purchase key (33030) → item (33027) → the buyer's sunshine account row.
- Catalog administration takes admin key (33029) → item (33027) and never a wallet row.
- Other wallet writers take their own domain locks, then account rows; none takes an avatar lock.
- No lock is ever taken in the reverse direction, so there is no cycle.

**Outcomes:**
- **Definite:** any refusal in steps 2-8 is answered from the function's result after an explicit rollback. A database error in the function is a definite 500. Neither becomes `RESULT_UNKNOWN`.
- **Uncertain:** only a failed commit or begin (or the test-only fault before commit) answers `RESULT_UNKNOWN`. The same key and body then replays or writes exactly once.

### 6.3 Ownership model

- **What ownership is:** the purchase snapshot row itself. `avatar_purchases` is append-only and unique per (user, item). There is no separate grant table and no write path other than a paid purchase, so B2 cannot grant items.
- **Starter items** are not owned. They are available to everyone, which is a separate fact: `starter: true` in the catalog.
- **Unlisted items:** owned items stay owned and are listed among owned items with today's catalog data, including `listed: false`.
- **Couples:** a purchase records the buyer's couple at purchase time, as every wallet operation does. Leaving or changing couples isn't supported by the product, and B2 defines no transfer or dissolution behaviour.

### 6.4 Responses

**Purchase (201 new, 200 replay):**
- `purchase`: the immutable original facts:
  - id, item id and version, name, category, asset key and price;
  - `debited_sunshine`;
  - `sunshine_balance_after` (from the debit entry, so a replay repeats the original value);
  - operation id and time.
- `current`: separated from the original facts and read now: `sunshine_balance` today, and today's catalog `item` (which may be repriced or unlisted). It is not the post-purchase state.

**Owned list:** `GET /api/v1/avatar/owned?limit=&cursor=`:
- the caller's own paid items, newest purchase first, by (purchase time, id);
- `limit` 1-100, default 50; `next_cursor` is opaque;
- each entry is the original purchase facts plus today's admin-free item view, with `listed`;
- no partner data and no balance.

**Compatibility:**
- The B1 catalog and appearance responses are unchanged.
- The admin item list gains the additive `owner_count`: the number of paid owners, or `null` for a starter.

**Errors:**
- New: `AVATAR_ALREADY_OWNED` (409) and `AVATAR_ITEM_UNAVAILABLE` (409, unlisted or starter).
- Reused: `AVATAR_ITEM_NOT_FOUND`, `AVATAR_VERSION_CONFLICT`, `INSUFFICIENT_BALANCE`, `SPACE_NOT_FOUND`, `IDEMPOTENCY_KEY_REUSED`, `VALIDATION_ERROR`, `RESULT_UNKNOWN`.

### 6.5 Database enforcement (migration 0045)

- **Operation types:** `wallet_operations_type_valid` is replaced by a superset that adds `AVATAR_PURCHASE`; no other type is added.
- **Purchase guard** `UA005` (before insert, under the item lock): the snapshot must equal the item's current listed revision, of a non-starter item. Its operation must be an `AVATAR_PURCHASE` with source `avatar_purchase`/purchase id, the buyer as actor, the same couple, key and fingerprint.
- **Deferred link** `UA006`, on the purchase, the operation and every ledger entry: each `AVATAR_PURCHASE` operation has exactly one purchase and exactly one entry, the buyer's sunshine debit of exactly the snapshot price.
- **Append-only:** `UA001`, plus the application role's SELECT/INSERT-only grant.

### 6.6 B2 implementation record (Claude Code, 2026-11-09)

**Migration:**
- `0045_avatar_purchases`, SHA-256 `c714684ce8d58d59bd0d5ac4208f1b408211eac39498706cb9aa514419c47205`.
- Additive: the posting-type superset, `avatar_purchases`, guards UA005/UA006, three deferred link triggers, the purchase function, and SELECT/INSERT for `usapp_api`.
- New lock class 33030. The next migration number is 0046.

**Contract:** 0.42.0 -> 0.43.0, additive: `purchaseAvatarItem`, `listOwnedAvatarItems`, the purchase and owned schemas, `owner_count` on admin items, and two error codes. The B1 catalog and appearance responses are unchanged.

**Files:**
- **New:** `database/migrations/0045_avatar_purchases.sql`, `database/tests/avatar_purchase_test.sql`, `services/api/src/services/AvatarPurchaseService.{h,cc}`, `services/api/src/controllers/AvatarPurchaseController.cc`, `services/api/tests/AvatarPurchaseDbTests.h`, `Claude outputs/b2-avatar-verify.ps1` and `Claude outputs/b2-yaml-check.py`.
- **Changed:**
  - `database/init-roles.sql` (section 37);
  - `database/tests/task_privilege_deviation.sql`;
  - `database/tests/wallet_ledger_test.sql` (E9's maintenance TRUNCATE lists `avatar_purchases`);
  - `services/api/src/db/TaskPrivilegePolicy.cc`;
  - `services/api/src/core/Errors.{h,cc}`;
  - `services/api/src/services/AvatarService.cc` (admin `owner_count`);
  - `services/api/CMakeLists.txt`;
  - `services/api/tests/{CMakeLists.txt (target avatar_purchase_db), DbGatedTest.cc, UnitTest.cc, StaticCheckTest.cc, ReminderLiveApiTest.h, AvatarCatalogDbTests.h}`;
  - `contracts/openapi.yaml`.
- No web, admin app or push worker change, so no web tests were run.

**Contract YAML** (`Claude outputs/b2-yaml-check.py`, log `b2-yaml-check.log`):
- Docker Compose v5.5.1's loader, used as a strict YAML parser only.
- The duplicate-key negative control was refused (exit 1). The contract parsed: 0.43.0, 102 paths, 119 operations, no duplicate operationId, every expected avatar operation and schema present, all 180 `$ref`s resolve.
- This is YAML structure only, not OpenAPI semantic validation. It also covers B1's earlier "parsed by eye" limitation.

**Disposable-database runs:**

| Run | Result |
|---|---|
| Prevalidation 1 (`usapp-b2-d837655514`) | The migration applied in the rolled-back transaction; **the new ledger failed**. Section D's B1 item creation hit `USAPP_AVATAR_ITEM_WITHOUT_REVISION` because the purchase function's `SET CONSTRAINTS ALL IMMEDIATE` stayed in force for the rest of the transaction. **Product defect, fixed:** only the four named purchase checks are made immediate, then restored to deferred (6.2 step 10); a static check now forbids `SET CONSTRAINTS ALL` in the function |
| Prevalidation 2 (`usapp-b2-8cd4b8077d`) | Maintenance pass 23 cases (including the six forged-row refusals: wrong amount, wrong asset, extra entry, missing purchase, wrong source, snapshot price, plus a forged starter purchase), application pass 6 cases; 0045 objects absent afterwards |
| 1 (`usapp-b2-e6de84487b`) | Prevalidation, apply, 46 checksums, privileges and the two avatar ledgers passed. **`wallet_ledger_test` E9 failed** (expected UW001, got 0A000): its maintenance TRUNCATE of the wallet tables must list every table referencing `wallet_operations`, and 0045 adds one. Fixed in the test the way 0007/0015/0023 did (one branch listing `avatar_purchases` last, no CASCADE) |
| 2 (`usapp-b2-7c3fdeb87f`) | All ten SQL ledgers passed. **The build failed:** `CHECK_EQ` on a `std::set` in the new suite (no stream operator); test-only fix |
| 3 (`usapp-b2-76e365d85d`) | Build and both targeted suites passed. **The live API failed twice, both test defects:** (a) B1's live assertion "no avatar wallet operation at all" was database-wide, and the purchase suite had legitimately created some (narrowed to the B1 item); (b) my live fixture inserted a hat asset without `covers_hair_top`, which the registry rightly refused |
| 4 (`usapp-b2-f6a4a6238d`) | Targeted 151/114 and live 488 passed. **CTest 16/17:** `avatar_catalog_db` failed the same kind of database-wide B1 assertion (`AvatarCatalogDbTests.h:458`). Replaced with the exact invariant: no purchase ever names a starter item, checked globally and per starter |
| 5 (`usapp-b2-883f3cedf2`) | **All passed.** Prevalidation 23/6; 46 checksums; privilege deviations 0 (after apply, repeat apply with InitRoles, and CTest); ten SQL ledgers (`avatar_purchase_test`, `avatar_catalog_test`, `wallet_ledger_test`, `task_ledger_test`, `product_ledger_test`, `redemption_ledger_test`, `redemption_workflow_test` and `notification_choice_test` in both roles; `timestamp_guard_test` and `push_dispatch_test` in both modes). Targeted `avatar_catalog_db` 156 and `avatar_purchase_db` 114. Live API through the real binary on both listeners: 488. **Full CTest 17/17 in 95.01 s** (`unit` 2833, `db_gated` 3573 in 14.66 s, `static_check` 1288, `avatar_catalog_db` 144, `avatar_purchase_db` 114). Post-checks: purchases without exact debit/audit or naming a starter 0, ledger chain breaks 0, avatar wallet operations other than purchases 0 |

None of the failures was intermittent: each had a deterministic cause, recorded above, and was fixed before the next run. No timeout was raised. The reminder split and every existing case are kept.

**Coverage:**
- **Purchase outcomes:**
  - exact sunshine debit with flowers unchanged, from real task settlement;
  - each refusal with no write (insufficient, stale version, starter, unlisted, unknown, owned, outside a couple), including dropping the sunshine account row a refusal would have created.
- **Idempotency:**
  - replay after a reprice and an unlist, with original facts kept separate from today's state;
  - key reuse with another version or another item.
- **Concurrency:**
  - the same key at once;
  - the same item under two keys;
  - two items competing for the funds of one;
  - a held catalog revision that a waiting purchase must observe as a conflict.
- **Fault before commit** (nothing committed), then a retry that writes exactly once and replays afterwards. Not a real post-commit response loss (see Limitations).
- **Owned items:** owner-only listing, pagination, limits and cursor; unlisted owned items still shown; append-only refusals.
- **Live:**
  - real HTTP funding;
  - the session/Origin/CSRF/key/body boundary;
  - 201/replay/reuse/owned/unavailable/conflict/insufficient;
  - wallet before/after;
  - partner, other-couple and no-couple isolation;
  - the admin `owner_count` without identities.

**Not verified:** browser, device, cloud, restore or rollback acceptance, and real art. **NOT DEPLOYED.** Nothing is committed.

**Limitations:**
- The B1 tests' fresh-database case (incomplete starter configuration) runs only in the targeted pass.
- A purchase records the buyer's couple at purchase time; leaving or changing couples is unsupported product-wide and undefined here.
- `owner_count` counts paid owners only.
- Uncertain-commit recovery is tested with the fault before commit, not with a real lost connection after commit. The same-key replay path that such a retry would take is tested.

## 7. B3: loadout persistence and equip rules

### 7.1 Owner approval (2026-11-09)

1. Six categories, at most one item per category.
2. Hair, clothes and expression are required; hat, handheld item and accessory may be empty.
3. Only starter items or items purchased by the caller may be equipped. Unlisted but owned items stay equippable.
4. Buying and equipping remain separate actions.
5. Clearing a required slot selects its explicitly configured default; clearing an optional slot leaves it empty.
6. The whole loadout is saved atomically with expected-version protection; a same-key retry replays without another event.
7. No UI or final art in this batch.

### 7.2 Request and normalization

`POST /api/v1/avatar/loadout` saves the caller's whole outfit. It requires a user session, Origin and CSRF, and an `Idempotency-Key`. The body is exactly:

```json
{"expected_version": 0, "slots": {"hair": "<uuid>|null", "clothes": "<uuid>|null", "expression": "<uuid>|null",
                                  "hat": "<uuid>|null", "handheld": "<uuid>|null", "accessory": "<uuid>|null"}}
```

- **Shape:**
  - Exactly these two members, and all six slot keys. A missing, unknown or duplicate key is `VALIDATION_ERROR`, as is a non-uuid value or a version below 0.
  - Ids are lowercased before anything else.
  - The caller comes only from the session; a user id in the body is an unknown field.
- **`null` in a required slot** (hair, clothes, expression) means "use the configured default". It is resolved during the write from the current starter configuration and stored as the concrete item id.
- **`null` in an optional slot** (hat, handheld, accessory) means empty.
- **Version:**
  - Each user's saved loadouts are versioned 1, 2, … in one append-only history.
  - Before the first save the version is **0**, matching B1's starter `expected_version` convention.
  - A save must name the current version; anything else is `AVATAR_VERSION_CONFLICT`.
- **Fingerprint:** computed by the server over the canonical *requested* terms (expected version and the six requested values, nulls included), never over the resolved defaults. An identical retry is therefore identical, even if a default changed in between.

### 7.3 Fallback and resolution

- **When it happens:** only when at least one required slot is `null`. The write then takes the starter-configuration lock in shared mode (`33028`) and reads exactly one configuration version, the latest committed one.
- **What it resolves:** every `null` required slot comes from that one version, and the event records the version as `starter_version`.
- **When it fails:** if no configuration exists, the save fails closed with `AVATAR_DEFAULTS_UNAVAILABLE` and nothing is written. A B1 configuration always names all three required defaults, so "incomplete" can only mean "absent".
- **When no slot is `null`:** no configuration is read, and `starter_version` is recorded as null.
- **Replay:** a committed save stores the resolved ids, so its replay returns exactly those, and so does the appearance read, even after the defaults change.
- **Uncertain save** (`RESULT_UNKNOWN`): the caller cannot tell whether the original transaction committed, and the retry's outcome depends on which happened. *(Corrected 2026-11-10: this paragraph first said an unconfirmed commit captured nothing and that the retry is always a new write. That is true only of the second case below.)*
  - **The original committed** (for example, the response was lost after the commit): the same key and body replays the original event, with its original resolved ids and `starter_version`, even if the defaults changed in between.
  - **No original committed:** the retry may create the first event, and it then resolves any defaults against the configuration current in that transaction.
  - **Verification limit:** the test-only fault before commit exercises only the second case. Real response loss after a commit is not verified. The first case rests on the same key-classification path as the ordinary same-key replay of a confirmed save, which is tested, including after a default change.
- **Missing art** stays a rendering concern: a saved loadout is never rewritten to hide it.

### 7.4 Eligibility

Each non-null slot, given or resolved, must name an item that:
- exists (`AVATAR_ITEM_NOT_FOUND` otherwise);
- belongs to that slot's category (`AVATAR_SLOT_INVALID`);
- is either a starter item, or has a purchase by **the caller** (`AVATAR_NOT_OWNED`). Another user's purchase never counts, including the partner's.

Listing doesn't matter: unlisted owned items and unlisted starters stay equippable. Items are never deleted, starter flags never change and purchases are never reversed, so a saved loadout cannot become ineligible later.

### 7.5 Sequence, locking and replay

One function, `usapp_avatar_loadout_save`, in one transaction:

1. **Per-user loadout lock** `33031`. It serializes every save by the user, including same-key retries.
2. **Key classification**, before any membership, version, starter or ownership read:
   - the same key and fingerprint replays the original event;
   - any other request under the key answers `key_reused`.
3. **Membership:** the caller must belong to a couple (`SPACE_NOT_FOUND` otherwise), consistent with B2 purchases. The event records that couple, like every wallet operation and purchase. Leaving or changing couples is not defined here.
4. **Version check** against the caller's latest event.
5. **Fallback** (7.3), only when needed.
6. **Eligibility** (7.4) for all six slots.
7. **Writes:** insert the event (resolved ids, requested values, `starter_version`, key, fingerprint) and an `avatar.loadout_saved` audit row.

**Lock order:** loadout user (33031) → starter configuration, shared (33028), only when a fallback is needed.
- Catalog administration takes admin key (33029) → item (33027) or starter configuration (33028), and never 33031.
- Purchases take purchase key (33030) → item (33027) → the wallet account row, and never 33031.
- Equip takes no item lock and no wallet lock, and writes no wallet row.
- No path takes these locks in reverse order, so there is no cycle.
- A starter write in progress makes a fallback save wait, and the save then resolves the newest committed version.
- A save that names every required slot doesn't wait.

**Outcomes:**
- Refusals are definite and roll back.
- Only a failed commit or begin answers `RESULT_UNKNOWN`. The same key and body then replays the original event if it did commit, or writes it once if it did not (7.3, "Uncertain save").

### 7.6 Responses

**Save** (201 new, 200 replay):
- `loadout`: the immutable mutation facts:
  - event id and version;
  - the six resolved item ids and the six requested values;
  - `starter_version` and time.
- `current.appearance`: read now, in the B1 appearance shape. It is not part of the original facts.

**Appearance** (`GET /api/v1/avatar`), additive:
- `source` is `saved_loadout` once the caller has saved, otherwise `starter_defaults`, exactly as in B1.
- A new `loadout_version` member is the latest saved version, or 0.
- With a saved loadout, `slots` are its items' current catalog views, `complete` is true, and `starter_version` is the version that save resolved against, or null.
- The read still needs only a session, as in B1.

### 7.7 Database (migration 0046)

- `avatar_loadout_events`: append-only, unique per (user, version) and (user, key).
  - It has foreign keys to the buyer membership, the six items and the starter configuration.
  - The required slots are not null.
- Guard **UA007**, under the per-user lock:
  - contiguous versions;
  - categories;
  - eligibility (starter, or the user's own purchase);
  - `starter_version` present when a requested required value was null;
  - and, in that case, the resolved item equals that configuration's item.
- `usapp_api` gets SELECT and INSERT only.
- No wallet, purchase or catalog table changes, and no item grant.

### 7.8 B3 implementation record (Claude Code, 2026-11-09)

**Migration:**
- `0046_avatar_loadouts`, SHA-256 `2b136ef9650201dc1a569e35078a7607107669d3020f8006af92f55b42f2ba69`.
- Additive: `avatar_loadout_events`, the eligibility and request-shape functions, guard UA007, append-only, the save function, and SELECT/INSERT for `usapp_api`.
- New lock class 33031; shared use of 33028. No wallet, purchase or catalog table changed. The next migration number is 0047.

**Contract:** 0.43.0 -> 0.44.0:
- `saveAvatarLoadout`, with the loadout request and response schemas.
- The appearance read gains `loadout_version` and the `saved_loadout` source. The optional slots may now be items, after a save.
- Three new error codes: `AVATAR_SLOT_INVALID`, `AVATAR_NOT_OWNED`, `AVATAR_DEFAULTS_UNAVAILABLE`.

**Files:**
- **New:** `database/migrations/0046_avatar_loadouts.sql`, `database/tests/avatar_loadout_test.sql`, `services/api/src/services/AvatarLoadoutService.{h,cc}`, `services/api/src/controllers/AvatarLoadoutController.cc`, `services/api/tests/AvatarLoadoutDbTests.h`, `Claude outputs/b3-avatar-verify.ps1` and `Claude outputs/b3-yaml-check.py`.
- **Changed:**
  - `services/api/src/services/AvatarService.cc` (appearance prefers the saved loadout);
  - `services/api/src/core/Errors.{h,cc}`;
  - `services/api/src/db/TaskPrivilegePolicy.cc`;
  - `services/api/CMakeLists.txt`;
  - `services/api/tests/{CMakeLists.txt (target avatar_loadout_db), DbGatedTest.cc, UnitTest.cc, StaticCheckTest.cc, ReminderLiveApiTest.h, AvatarCatalogDbTests.h}`;
  - `database/init-roles.sql` (section 38);
  - `database/tests/task_privilege_deviation.sql`;
  - `database/tests/avatar_catalog_test.sql` (A8 TRUNCATE lists `avatar_loadout_events`);
  - `contracts/openapi.yaml`.
- No web change, so no web tests were run.

**Contract YAML** (`Claude outputs/b3-yaml-check.py`, log `b3-yaml-check.log`):
- The duplicate-key negative control was refused.
- The contract parsed: 0.44.0, 103 paths, 120 operations, every expected avatar operation and schema present, all 183 `$ref`s resolve.
- YAML structure only, not OpenAPI semantic validation.

**Disposable-database runs:**

| Run | Result |
|---|---|
| Prevalidation (`usapp-b3-efe25552fc`) | 0046 plus its ledger, rolled back. Maintenance pass 27 cases, including the fail-closed "no starter configuration" case (`defaults_case=covered`) and seven forged-row UA007 refusals. Application pass 7 cases; its defaults case was skipped because the maintenance pass in the same transaction had created a configuration. 0046 objects absent afterwards |
| 1 (`usapp-b3-a7d5113071`) | Apply, 47 checksums, privileges, and the loadout and purchase ledgers passed. **`avatar_catalog_test` A8 failed** (expected UA001, got 0A000): 0046 adds a foreign key from loadouts to starter configurations, so its maintenance TRUNCATE must list the referencing table. Fixed in the test, the same way as B2's wallet E9 |
| 2 (`usapp-b3-6f624fc846`) | All 11 SQL ledgers, the build, targeted catalog 157 / purchase 114 / loadout 85, live API 519 and **CTest 18/18** passed. **The new loadout post-check failed (2 rows):** the B3 suite's "guard accepts a correct forged row" control committed a row without the audit row that every API save writes, once per suite run. The control now runs in a transaction that is rolled back; no product change |
| 3 (`usapp-b3-1cdc420ecb`) | **All passed.** Prevalidation 27/7; 47 checksums; privilege deviations 0 (apply, repeat apply with InitRoles, CTest); 11 SQL ledgers (`avatar_loadout_test`, `avatar_purchase_test`, `avatar_catalog_test`, `wallet_ledger_test`, `task_ledger_test`, `product_ledger_test`, `redemption_ledger_test`, `redemption_workflow_test` and `notification_choice_test` in both roles; `timestamp_guard_test` and `push_dispatch_test` in both modes). Targeted catalog 157, purchase 114, loadout 85; live API 519 through the real binary on both listeners. **Full CTest 18/18 in 98.38 s** (`unit` 2833, `db_gated` 3573 in 15.18 s, `static_check` 1320, `avatar_catalog_db` 144, `avatar_purchase_db` 114, `avatar_loadout_db` 85). Post-checks: purchase debit/audit 0, ledger chain breaks 0, ineligible/unaudited/gapped loadouts 0 |

Neither failure was intermittent; each had a deterministic cause, recorded above. No timeout was raised. The reminder split and every existing case are kept.

**Coverage:**
- **Eligibility:** starters, purchased items and an unlisted owned item; refusals for wrong category, unknown item, unbought item, the partner's purchase and a forged other-user purchase.
- **Request handling:**
  - malformed requests, including incomplete slots, an extra slot and a supplied user id;
  - required-slot default resolution and optional-slot clearing;
  - "no saved loadout" (`starter_defaults`, version 0) versus a saved loadout.
- **Defaults:**
  - a default change after a save (replay and appearance keep the captured ids);
  - a save stopped by the test fault before commit (so nothing committed), whose retry creates the first event and resolves against the configuration current then. This is not the committed-but-response-lost case (7.3);
  - "no configuration" failing closed with no event (the ledger on a fresh database).
- **Idempotency and concurrency:**
  - same-key replay, a changed request under the same key, concurrent duplicates;
  - concurrent saves on the same version;
  - a fallback save waiting for an open starter write and resolving it, while a full save does not wait.
- **Integrity:** atomic audit, append-only refusals, and no wallet operation, entry or balance change.
- **Live HTTP:** authentication, Origin and key; a user-id field refused; replay and reuse; version conflict; partner isolation; no-couple `SPACE_NOT_FOUND`; the read without a couple; the wallet unchanged.

**Not verified:** browser, device, cloud, restore or rollback acceptance, and real art. **NOT DEPLOYED.** Nothing is committed.

**Limitations:**
- The C++ "no starter configuration" case only runs when the suite starts on a fresh database. Under the harness order it reports `skipped`, so the SQL ledger is its deterministic coverage.
- Uncertain-commit recovery is tested only with the fault before commit, which proves the "nothing committed, retry writes once" case. A real response loss after a commit is not verified; in that case the retry must replay the original resolved ids and `starter_version` (7.3). The test case's name and one comment in `AvatarLoadoutDbTests.h` (lines 195 and 217) still carry the earlier wording ("an uncertain save resolves again", "nothing was captured"); they describe the pre-commit fault only and were left unchanged in this documentation-only correction.
- Couple changes after a save are undefined, as they are product-wide.
- The appearance read shows today's catalog view of saved items (names may change); the saved item ids never change.

## 8. B4: user shop, wardrobe and home (Codex, 2026-11-10)

The owner approved placeholder art and delegated implementation to Codex after Claude Code's quota ended. [Implementation and evidence](phase4-b4-avatar-web.md) records the exact scope, changed files, verified asset hashes, enrollment boundary, captured request/replay behavior, fixed regressions and failed attempts.

The final source passed 46 focused avatar tests and the complete 45-file/920-test web suite, vue-tsc, Node-config tsc and an isolated production build. Runtime screenshot/layout/browser cache checks did not run because computer use halted before navigation. This is not real API, browser, device or service-worker lifecycle acceptance. No backend, schema, contract or dependency changes; no new migration. Registry enrollment remains an explicitly pending maintenance step, so empty real catalogs remain empty. B5 administration UI and formal artwork are not included.
