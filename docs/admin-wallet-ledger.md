# Admin wallet and ledger inspection (read-only, contract 0.47.0)

Status (Claude Code, 2026-11-11):
- Implemented, with automated verification recorded in Section 6.
- Not deployed; no browser, device or production acceptance.
- Read-only: no correction, grant, refund, transfer, purchase, export or dashboard.
- No migration, grant or dependency. Migrations 0000–0047 are unchanged, and 0048 is not reserved.

## 1. Boundary

**Routes:** two admin-only routes behind the existing `AdminFilter`:
- `GET /api/admin/v1/users/{userId}/wallet`
- `GET /api/admin/v1/users/{userId}/wallet/ledger?asset=&limit=&cursor=`

**AdminFilter checks, in order:**
1. peer CIDR;
2. the admin surface header;
3. the admin session cookie.

Both are GET, so no CSRF is needed, as for the other admin reads. The controller also requires an admin identity from that session. An ordinary user cookie (`__Host-usapp-session`) is a different cookie and is never consulted: it gets 401, and a request without the surface header gets 403.

**Unchanged:**
- `GET /api/v1/wallet` stays caller-only (D21), with no user selector, and its serialization does not change.
- `WalletService` is not touched.

The admin app uses its own `AdminHttpClient` (`apps/admin`), not the user client.

## 2. Subject states

| Subject | Summary | Ledger |
|---|---|---|
| Malformed user id | 400 `VALIDATION_ERROR` | 400 |
| No such user | 404 `NOT_FOUND` ("用户不存在") | 404 |
| User, no space | 200, `space: null`; both assets `account_exists: false`, balance `"0"`, `latest_seq: "0"` | 200, empty `items` |
| User in a space, no postings yet | 200 with the `space`; accounts as stored (usually `false`), balance `"0"` | 200, empty `items` |
| User with postings | 200; balances from the ledger | 200 |

**One snapshot per response:**
- **Existence, membership and the balances** come from one statement, so one snapshot. A user is never reported as nonexistent while their entries are read, or the reverse.
- **No account versus no user:** "no wallet account" (`account_exists: false`) is distinguished from a missing user (404).

## 3. Fields (allowlist)

**Summary:**
- `user`: `id`, `status`;
- `space`: `couple_id`, `slot`, or null;
- `balances[]`, one per asset in the fixed order flower then sunshine: `asset`, `account_exists`, `balance`, `latest_seq`;
- `read_at`, the statement timestamp.

The balance is the newest entry's `balance_after`, by the per-user, per-asset `seq`, or `"0"` with no entry. `wallet_accounts` has no balance column, and none is invented.

**Ledger:**
- `user_id`, `asset`, `account_exists`, `balance`, `latest_seq`, `read_at`;
- `items[]`: `seq`, `amount`, `balance_after`, `recorded_at`, and `operation` with `id`, `type`, `source_type` and `source_id`;
- `next_cursor`.

`source_id` is shown as text. The admin app has no source-detail pages, so it renders no links.

**Excluded:** idempotency keys, request fingerprints, `lock_token`, the operation's actor, entry ids and couple ids on entries, credentials, and every other user's entries. Every ledger statement filters by `user_id = $1 AND asset_type = $2`.

**Exact numbers:** `seq`, `amount`, `balance_after`, `balance` and `latest_seq` are canonical decimal strings matching `^(0|-?[1-9][0-9]{0,18})$` (no `-0`), produced by PostgreSQL `bigint::text`. The admin client validates them and never converts them to `Number`. Existing public wallet serialization is unchanged.

## 4. Pagination and consistency

**Order:** newest first by `seq DESC`, the per-user, per-asset sequence, served by the unique index `(user_id, asset_type, seq)`. Wall-clock time is not used for order.

**Limit:** `limit` is 1–100, default 50, using the existing `TaskService::parseLimit`. Anything else gets 400.

**Cursor format:** `w1.<asset>.<seq>.<userId>`, at most 128 bytes. A cursor is valid only for the same user and the same asset, with `seq` a positive integer of at most 19 digits. Malformed, mismatched-user and mismatched-asset cursors get 400. A page continues with `seq < cursor seq`.

**Snapshots:**
- **Each response is one statement:** existence, account flag, balance, `latest_seq` and the page all share one snapshot.
- **Separate reads are separate snapshots:** the summary and each page are not one atomic snapshot. Entries appended after the summary appear above an older page's position; the page's own `latest_seq` and `balance` show the state it was read at.
- Entries are append-only, so a page never changes after it is read.

**Writes:** the reads write nothing — no wallet row and no business audit row. AdminFilter may refresh the admin session's own bookkeeping, as for every admin request.

## 5. Admin UI

**Entry:** `UsersView` gains a "钱包" link per user, to the protected route `/users/:userId/wallet`.

**`UserWalletView`:**
- labelled read-only;
- shows both balances, an asset switch, the ledger table with prev/next paging, an empty state, errors and explicit refresh and retry buttons.

**Store `user-wallet` (memory only; corrected 2026-11-11, Section 6.1):**
- **Summary identity:** admin session (`epoch` and admin id) and the selected user.
- **Ledger identity:** additionally, the selected asset.
- **What a change drops:**
  - logout, an account change, a route change (unmount) or a new user drops both kinds of late answer;
  - an asset change drops only ledger answers, so a pending same-user summary still lands.
- **Authorization loss:** a 401/403 from any current read clears the private data, settles both loaders, shows the authorization message and invalidates every read in flight.
- **No persistence:** no localStorage, IndexedDB or caching.

## 6. Verification record (Claude Code, 2026-11-11)

**Files:**
- **New:**
  - `services/api/src/services/AdminWalletService.{h,cc}`;
  - `services/api/src/controllers/AdminWalletController.cc`;
  - `services/api/tests/AdminWalletDbTests.h` (CTest target `admin_wallet_db`, TIMEOUT 30, outside `db_gated`);
  - `services/api/tests/AdminWalletLiveApiTest.h`;
  - `apps/admin/src/api/wallet.ts`;
  - `apps/admin/src/stores/user-wallet.ts`;
  - `apps/admin/src/views/UserWalletView.vue`;
  - `apps/admin/tests/unit/user-wallet.spec.ts`;
  - `Claude outputs/aw-yaml-check.py`.
- **Changed:**
  - `services/api/CMakeLists.txt` (the controller) and `services/api/tests/CMakeLists.txt` (the target);
  - `DbGatedTest.cc` (the `ADMIN_WALLET` mode and the live call);
  - `StaticCheckTest.cc`: three `0.46.0` version assertions become `0.47.0`, and one new case;
  - `contracts/openapi.yaml` (0.47.0: two paths and five schemas);
  - `apps/admin/src/router/index.ts`;
  - `apps/admin/src/views/UsersView.vue` (the "查看（只读）" link);
  - `tools/export-verify.ps1` (runs `ADMIN_WALLET` first on the fresh database; counts all cases).
- **Unchanged:** `WalletService`, `WalletController`, `GET /api/v1/wallet`, every migration, grants, dependencies, the export/E1 code, and `apps/web`.

**Admin and contract checks [executed]:**

| Check | Result |
|---|---|
| Focused `user-wallet.spec.ts` | 13 tests passed. The first run failed 2, both real product bugs, both fixed (below) |
| Full admin suite (`Claude outputs/aw-admin-f98749d6f7`) | **76 tests in 5 files** passed |
| Both admin typechecks | Passed |
| Isolated production build | Passed. `apps/admin/dist` stayed 20 files and `apps/web/dist` 45 files, with identical hashes and timestamps |
| `Claude outputs/aw-yaml-check.py` | Duplicate-key control refused. 0.47.0, 107 paths, 124 operations, all 195 `$ref`s resolve; the `AdminDecimal` pattern accepts `9007199254740993` and refuses `-0`; the public wallet read has no parameter. YAML structure only, **not OpenAPI semantics** |

**The two product bugs:**
1. **`-0` accepted:** the decimal pattern allowed `-0`. The client and contract pattern are now `^(0|-?[1-9][0-9]{0,18})$`.
2. **Lost authorization message:** when both reads lost authorization, the second read cleared the first read's message. An authorization loss now clears only the shown data and keeps the message.

**Backend runs (ownership-checked disposable stack via `tools/export-verify.ps1`; all retained):**

| Run | Result and cause | Change |
|---|---|---|
| 1 `usapp-e1-40f83c6bdb` | Build passed; `ADMIN_WALLET` 100/101. **Fixture defect:** the synthetic large-value account gave two entries one operation, and the frozen schema has `UNIQUE (operation_id, user_id, asset_type)` | One operation per entry |
| 2 `usapp-e1-ff353e83de` | `ADMIN_WALLET` 110/110, live 607/607. CTest 20/21: `admin_wallet_db` failed. **Fixture defect:** fixed synthetic `source_id` values collided with `UNIQUE (operation_type, source_type, source_id)` when CTest re-ran the suite on the same database | Unique source ids per run, in the live test too |
| 3 `usapp-e1-a3104d2306` | All suites, live 607, **CTest 21/21**. The harness's **ledger-chain post-check failed:** the synthetic entries at `seq` 2^53 and 2^53+1 had no chain below them | The fixture is now a chain-consistent account: 10 000 × (+100000) up to the sunshine maximum 1 000 000 000, then −100000. A `seq` above 2^53 cannot be reached through real postings; it stays covered by `decodeCursor` (up to 2^63−1) and the admin client tests |
| 4 `usapp-e1-06d5f0ba61` | **All batch checks passed** (below). The harness then **stopped** in the later, pre-existing E1 harness-only `EXPORT_CONNECTION_LOSS` mode (see the note after the evidence list) | None in this batch |

No product code changed after run 1. No assertion was weakened and no timeout raised.

**Run 4 evidence:**
- **Setup:** 48 checksums, eraser violations 0, privilege deviations 0 after apply and after repeat apply with InitRoles; build passed.
- **`ADMIN_WALLET`:** 112 assertions in 2 cases. Covered:
  - real postings: two task rewards, a direct redemption debit (−4) and its mutual-cancel refund (+4), and an avatar purchase (−120 sunshine);
  - exact balances equal to the caller-only wallet read (flower `"10"`, sunshine `"380"`), as JSON strings;
  - newest-first order by `seq` and the operation types and sources;
  - partner isolation, the field allowlist, and an unchanged write footprint;
  - nonexistent user (404) for both reads, a user without a space, and a member without postings;
  - pages of 2 over 5 entries (5,4 | 3,2 | 1);
  - boundaries below the first entry and above the newest;
  - cursor user/asset binding and 9 malformed cursors;
  - exact `1000000000` and `-100000` values, and a last page ending at seq 1.
- **Other targeted suites:** `EXPORT` 444, `ACHIEVEMENT` 22, `AVATAR_CATALOG` 157, `AVATAR_PURCHASE` 114, `AVATAR_LOADOUT` 85.
- **Live API, real binary, 607 assertions** (560 before this batch). On **both** listeners: no session gives 401; an ordinary user's cookie gives 401; no admin surface header gives 403; a valid admin read gives 200 with `no-store` and exact decimal strings (`"777"`), with no idempotency or fingerprint text. On the admin listener: an unknown user gives 404; a malformed id, missing or unknown asset, limit 0 or 101, a nonsense cursor, and another asset's or another user's cursor each give 400; a valid cursor gives 200. `GET /api/v1/wallet?user_id=<partner>` still returns only the caller's own balances. The admin reads wrote no wallet or audit row.
- **Full CTest: 21/21 in 99.67 s** (`admin_wallet_db` 0.33 s, `db_gated` 14.88 s).
- **Post-checks:** privilege deviations 0 after CTest; early or reversed erasures 0; **ledger chain breaks 0**.
- **Migrations:** the frozen migration hashes were compared directly against the snapshot the harness took at its start: 48 = 48, identical (0047 `75d67669…`), because the harness's own final comparison was not reached.

**Not reached in run 4:** the E1 harness-only `EXPORT_CONNECTION_LOSS` step failed. Drogon's reconnect stall after a server-side termination recurred and was longer than before: the deadline answered at 20 s and the slot stayed held (fail-closed), with no release within the test's 60 s wait (80 s total). This is the E1 availability limit recorded in `docs/user-data-export-design.md` Section 7.1, not this batch, which changes no export code. Because the harness stops at its first failure, `EXPORT_LARGE`, `EXPORT_CLOSURE` and the harness's final migration comparison did not run in run 4. They last passed in run `usapp-e1-babdaf6cdb`. Not rerun until green.

**Not verified:** browser rendering of the admin page, real admin login through the browser, production or deployment, and any real data.

### 6.1 Frontend race correction (Claude Code, 2026-11-11)

**Scope:** `apps/admin/src/stores/user-wallet.ts` and `apps/admin/tests/unit/user-wallet.spec.ts` only. No backend, contract, migration, grant, dependency, monetary validation or financial behaviour changed, so the backend result above stands as recorded (run `usapp-e1-06d5f0ba61`, CTest 21/21), as does the separate E1 connection-loss failure. No Docker or backend run.

**Race 1, authorization loss.**
- **Before:** a 401/403 cleared the shown data but left every other in-flight read valid. So a delayed summary or ledger 200 could repopulate the page after the authorization was lost.
- **Now:** a 401/403 from any *current* read:
  - invalidates every read in flight: summary and ledger, any asset, older pages included;
  - clears the private data and settles both loading indicators;
  - shows the authorization message for both parts.
- **Recovery:** only an explicit refresh or a new selection. There is no automatic retry, and the session store is not touched.

**Race 2, asset switching.**
- **Before:** `selectAsset()` bumped the one shared generation. A pending same-user summary was discarded while `summaryLoading` stayed true, which kept the refresh button disabled.
- **Now:** the two reads have separate identities:
  - a summary belongs to *admin session + selected user*;
  - a ledger page to *admin session + selected user + asset*.
- **What each change invalidates:**
  - an asset change invalidates ledger reads only;
  - a user, session or unmount change invalidates both;
  - summary and ledger keep their independent sequence ordering.

**Regressions (written first; 5 new tests):**
1. Summary 401 and 403, then a delayed ledger 200: nothing repopulates, both loaders settle, and there is no retry.
2. Ledger 401 and 403, then a delayed summary 200: the same.
3. Data shown, then an older page request plus a refresh whose summary gets 401: the stale page and ledger answers cannot bring the data back. An explicit refresh then recovers.
4. View: a slow summary and a fast first ledger; switch the asset while the summary is pending; finish the summary. The summary appears, `summaryLoading` ends, the refresh button is enabled, and old-asset ledger answers stay ignored.
5. A user change and a reset still invalidate a pending summary.

**Runs:**

| Run | Result |
|---|---|
| Against the original store (`Claude outputs/awr-before-fix.log`) | **4 failed, 14 passed.** 1–3 failed because a delayed 200 repopulated the page; 4 failed because the pending summary was discarded after the asset switch. Test 5 already passed: those invalidations were correct before |
| After the fix (`Claude outputs/awr-after-fix.log`) | First 17/18. Test 4's last segment clicked an asset button that the view deliberately disables while a ledger read is pending. That was a test mistake: it now asserts the disabled state and drives the store directly. Then **18/18** |
| Full admin suite (`Claude outputs/awr-admin-a8ead4b3d1`) | **81 tests in 5 files**; both admin typechecks and an isolated production build passed. `apps/admin/dist` stayed 20 files and `apps/web/dist` 45 files, with identical hashes and timestamps |

No existing assertion was weakened.

**Not verified:** browser rendering, a real admin login, production. The reads themselves are unchanged and still rely on the backend's authorization; this correction only governs what the page may show.
