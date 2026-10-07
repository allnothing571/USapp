# USapp Phase 1 Plan: Couple Core Loop

Status (updated 2026-10-06, Asia/Shanghai):

- **M1 (nickname): approved, implemented, and owner-run acceptance checks completed successfully on 2026-10-06.** Implementation status is in Section 7.8; the owner's verification evidence and its limits are in Section 7.9. The approval covered M1 only, including the additive `PATCH /api/v1/me` operation and OpenAPI `info.version` 0.2.0.
- **M2 (wallet/ledger): design in progress (Section 9), documentation only.** The owner approved design directions D1, D2 (balance and per-entry bounds only), D18, D19, and D21 on 2026-10-06. M2 implementation, migrations, grant changes, and database execution are **not approved**.
- **M3-M10: not approved.** Their proposed defaults in Section 4 are not approved either. Contracts, tables, and files proposed for M2-M10 are design proposals and do not exist in the repository.
- Claude Code has not built, tested, or verified anything in this document; all verification results are owner-run and recorded as such.

Prerequisite: Phase 0 was accepted by the owner on 2026-10-06 as a local development baseline, within the scope and limitations in [docs/phase0-acceptance.md](phase0-acceptance.md). That acceptance is not production-readiness approval.

Rules for using this plan:

- Implementation starts only after the owner approves this plan and assigns one milestone. Each milestone is implemented, reviewed by Codex, and accepted by the owner before the next one starts.
- Every contract, migration, role/grant, dependency, or deployment change named here still needs explicit owner approval when its milestone is assigned.
- Open product decisions (Section 4) must be answered before the milestone that depends on them. Proposed defaults are suggestions, not decisions.

## 1. Sources Read

- Original design document `app开发方案V1.docx` (referred to below as "V1"; section numbers are V1's own). Read only; it is not modified and must not be added to Git.
- `docs/claude-code-handoff.md`, `docs/phase0-acceptance.md`, `README.md`, `docs/architecture.md` (historical snapshot; its Section 5 records owner design constraints reserved for later phases).
- `contracts/openapi.yaml` (Phase 0 contract, version 0.1.0, described as frozen).
- `database/migrations/0000`-`0005`, `database/init-roles.sql`.
- `services/api/src/services/SpaceService.h`, `src/db/Db.h`, `src/core/Errors.h`, `src/controllers/SpaceController.cc`, the controller route list, `services/api/CMakeLists.txt`, the `DbGatedTest.cc` block list.
- `apps/web/src/router/index.ts`, `apps/web/src/views/HomeView.vue`, `apps/web/src/api/client.ts`, `apps/web/tests/unit/` file list.

## 2. Phase 1 Scope

### 2.1 What V1 assigns to Phase 1

V1 Sec 53 ("recommended development phases", explicitly a recommended engineering split rather than a hard requirement) defines Phase 1, "couple core loop", as: users, couple binding, home page, tasks, red flowers, sunshine points, reward shop, redemption. V1 states that after this phase the two users can start real use.

Users and couple binding already exist from Phase 0 (email-code login, space creation, invite acceptance). The only user-facing gap observed is that there is no way to set a nickname: `users.display_name` defaults to an empty string, no endpoint writes it, and the home page shows a "Nickname not set" fallback.

### 2.2 In scope for Phase 1 (proposed)

| Area | V1 source | Phase 1 content |
| --- | --- | --- |
| Profile | Sec 3, 53 | Set own nickname |
| Assets | Sec 5, 43 | Red flower and sunshine balances with an append-only ledger; server-side atomic settlement only |
| Tasks | Sec 6-10, 12, 32 | Publish to partner; one-off, daily, weekly, custom-range periods; single and counting completion; self-reported completion without proof; silent expiry without penalty; view partner's completions; basic edit/cancel rules |
| Reward shop | Sec 14, 33 | Each member maintains the shop the partner buys from; name, description, flower price, redemption mode |
| Redemption | Sec 15, 16 | Direct and request modes; statuses; immutable snapshot of name, description, price; history |
| Home | Sec 19, 46 | Own balances and today's tasks; other quadrants depend on later phases (decision D15) |
| Audit | Sec 39, 43 | Every asset change and redemption writes `audit_logs` |

### 2.3 Explicitly not in Phase 1

Per V1 Sec 53 these belong to later phases, even where V1 describes them in detail:

- Phase 2: messages, task reactions (Sec 13), notifications and the "notify partner" toggle (Sec 18), Web Push, task reminders (Sec 11, which need push), anniversaries.
- Phase 3: photo memories, shared albums, Want/Plan/Finished, edit requests for shared content (Sec 29-31), and any image upload (including optional reward images, Sec 14).
- Phase 4: avatar, sunshine shop, dress-up, achievements, streaks.
- Phase 5: data export, admin asset correction and other admin write features (Sec 36), performance and polish.
- V1 Sec 48 exclusions remain excluded (chat, payments, flower cash-out, penalties, leaderboards, E2EE, and so on).

Sunshine points can be earned in Phase 1 but have nothing to spend on until Phase 4. This follows V1's phase split and is accepted rather than a defect.

## 3. Constraints Carried Forward (Must Not Change)

From the handoff Section 3 and the Phase 0 security baseline in `README.md`:

- Architecture: Vue 3 PWA, separate admin frontend, C++17 / Drogon v1.9.11 modular monolith, PostgreSQL 17, Caddy, Docker Compose. No new services, no Redis, no ORM change. V1 Sec 52.1 left the stack open; the established Phase 0 architecture takes precedence.
- Every couple-data endpoint derives the user from the server session and verifies membership on the server. Non-members get the same 404 as "not found". Frontend hiding is never authorization (V1 Sec 39, 54).
- Every state-changing request keeps exact Origin plus session-bound CSRF checks through the existing `AuthFilter`. No new exemptions.
- Every asset change happens in one backend transaction: lock, check, ledger insert, balance update, business row update, audit row. No client-side balance arithmetic (V1 Sec 54). Post-commit side effects only after confirmed commit (`TxScope::commit`).
- Numeric parameters use explicit SQL casts that match the C++ width (handoff Section 5, binary binding note).
- Natural-day logic uses `couples.timezone` (fixed `Asia/Shanghai` in the contract), never a raw UTC date cut (V1 Sec 44, migration 0003 comment).
- Offline: business writes are never queued; the Service Worker never caches `/api/` (no change).
- Reserved owner constraints from `docs/architecture.md` Section 5: no explicit "start task" button; after the current period is published, key task edits need recipient confirmation; cancelling an already-debited redemption needs both members' consent; ordinary notifications default to off; shared content uses independent tables, no `shared_contents` parent table.
- No hard-coded gender or role columns; use `creator_id`, `assignee_id`, `owner_id`, `couple_id` (V1 Sec 41).

## 4. Conflicts and Open Product Decisions

Each item needs an owner answer before the milestone listed. "Proposed" is a suggestion only.

### 4.1 Conflicts between sources

| ID | Conflict | Proposed resolution | Needed by |
| --- | --- | --- | --- |
| C1 | V1 Sec 19 fixes a four-quadrant home page, but two quadrants depend on later phases (anniversary: Phase 2; partner message: Phase 2) and the avatar is Phase 4. | See D15. | M10 |
| C2 | V1 Sec 17 makes the redemption notification mandatory, but notifications and push are Phase 2. | Phase 1 shows an in-app pending indicator derived from data (no notification table, no push). Real notifications arrive in Phase 2. | M8 |
| C3 | V1 Sec 32 lets the publisher edit a task "if the recipient has not started", but `docs/architecture.md` Section 5 removes the explicit start action and requires recipient confirmation for key edits once the current period is published. | The established requirement prevails (owner correction, 2026-10-06): once the current period is published, key edits require recipient confirmation. V1's "not started" wording and the absence of progress are not exemptions. The detailed workflow is undecided (D3). | M6 |
| C4 | V1 Sec 16 lists statuses pending confirmation, pending fulfillment, fulfilled, cancelled; request mode also needs a rejected or withdrawn outcome. | Use `cancelled` with a `cancel_reason` (`withdrawn`, `rejected`, `mutual`). | M9 |
| C5 | V1 Sec 14 allows an optional reward image, but there is no private media storage until Phase 3. | No images in Phase 1. | M7 |
| C6 | V1 Sec 6 says reward amounts are unlimited, but storage needs bounds. | See D2. | M2 |
| C7 | `contracts/openapi.yaml` describes itself as the frozen Phase 0 contract. | **Approved 2026-10-06 for M1:** keep all Phase 0 operations unchanged; add Phase 1 operations additively; `info.version` is now 0.2.0 and the description says Phase 0 operations stay frozen. Later additions still need approval per milestone. | M1 |

### 4.2 Open product decisions

| ID | Question | Proposed default | Needed by |
| --- | --- | --- | --- |
| D1 | Are task rewards newly created (minted) on completion, or transferred out of the publisher's balance? | **Approved direction (2026-10-06):** completion creates new flowers and sunshine for the recipient; the publisher is not debited. Section 9.9. | M2 |
| D2 | Bounds for reward amounts, prices, and balances. | **Approved in part (2026-10-06):** maximum balance 1,000,000 flowers and 1,000,000,000 sunshine; maximum absolute per-entry amount 999 flowers and 100,000 sunshine; reject, never clamp. Task reward ranges and product price ranges remain **unresolved** (M3, M7). Section 9.9. | M2 |
| D3 | How does recipient confirmation of key edits (reward, completion condition, deadline, period, target count) work after the current period is published: proposal and approval flow, what happens while a proposal is pending, which period the change applies to, and whether cancel-and-republish is also offered? | Undecided; no default is proposed. Fixed constraint: after publication of the current period, key edits are never silent, regardless of progress. | M6 |
| D4 | Week boundaries for weekly tasks. | Monday 00:00 to Sunday 24:00 in the couple's timezone. | M5 |
| D5 | Deadline semantics. | One-off: optional local deadline date and time. Daily/weekly: the period end, with an optional local time-of-day cutoff. Custom range: start and end dates, inclusive. | M3 |
| D6 | Can the recipient undo a completion or a +1? | No undo in Phase 1; extra +1 beyond the target is ignored. Undo would need a reversing ledger entry and is deferred. | M3 |
| D7 | Can a user publish a task to themselves? | No; tasks are published to the partner only (V1 Sec 6.1). | M3 |
| D8 | Visibility of tasks inside the couple. | Both members see all tasks in their couple, including the partner's completions (V1 Sec 12). | M3 |
| D9 | Phase 1 handling of the "notify partner" toggle (V1 Sec 18). | Not shown in Phase 1; added with notifications in Phase 2. | M3 |
| D10 | Phase 1 handling of recipient-set reminders (V1 Sec 11). | Deferred to Phase 2 (requires push). | M3 |
| D11 | Redemption cancellation rules. | Pending confirmation: redeemer may withdraw, creator may reject; no debit happened. Pending fulfillment (debited): cancellation needs one member to propose and the other to approve; on approval a refund ledger entry is written. Fulfilled: final. | M9 |
| D12 | Request mode when the redeemer's balance is too low at confirmation time. | Confirmation fails; nothing is reserved at request time. | M9 |
| D13 | Reward product removal. | Archive (hidden from the shop); never hard-delete; redemption snapshots are unaffected. | M7 |
| D14 | Can the shop owner see archived products or the creator's draft edits? | Owner sees only active products; creator sees active and archived. | M7 |
| D15 | Phase 1 home layout. | Keep the four-quadrant frame: top-left shows nickname and both balances (no avatar yet); bottom-left shows today's tasks; top-right and bottom-right show neutral "coming later" cards, not fake data. | M10 |
| D16 | Bottom navigation (V1 Sec 47 is a recommendation; Sec 52.2 says not final). | Phase 1: Home, Tasks, Rewards, Me. "Us" is added in Phase 2. | M10 |
| D17 | Nickname rules. | **Decided 2026-10-06 (owner):** after trimming surrounding whitespace, 1-20 Unicode code points; invalid UTF-8, control characters, and line breaks rejected; duplicates allowed; existing empty nicknames remain valid stored state, with no mandatory onboarding step. | M1 |
| D18 | Ledger immutability enforcement. `init-roles.sql` grants `usapp_api` UPDATE and DELETE on all tables, and `apply.ps1 -InitRoles` re-runs it. | **Approved direction (2026-10-06):** narrowed grants plus database triggers; restrictions must survive re-running the role-initialization script. Exact grant changes: Section 9.5 (implementation not approved). | M2 |
| D19 | Admin visibility of Phase 1 asset data. | **Approved direction (2026-10-06):** no new administrator asset endpoints in M2. | M2 |
| D20 | Duplicate-submission protection for completion and redemption. | Client sends an `Idempotency-Key` header; the server stores it with a unique constraint so a retried request cannot settle twice. Scope refined in Section 9.6. | M3 |
| D21 | Can a member see the partner's balances? V1 Sec 19 shows only one's own assets on the home page. | **Approved direction (2026-10-06):** M2 displays only the caller's own balances; revisit with M10 if needed. | M2 |

## 5. Proposed Data Model (Sketch)

Indicative only. Final names, columns, and constraints are settled in the milestone that introduces them, through a new numbered migration (`0006_...` onward). Existing migrations are never edited.

- Wallet and ledger tables: superseded by the detailed M2 proposal in Section 9 (`wallet_accounts`, `wallet_operations`, `wallet_ledger_entries`).
- `tasks`: `couple_id`, `creator_id`, `assignee_id` (creator differs from assignee; both members of the couple), title, description, `schedule_type` (`one_off`, `daily`, `weekly`, `custom`), `completion_mode` (`single`, `count`), `target_count`, reward amounts, local start/end dates, optional local deadline time, `status` (`active`, `cancelled`), `version`, timestamps.
- `task_periods`: one row per task period actually touched, keyed by (task, local period start). `progress_count`, `completed_at`, links to settlement transactions. Created lazily on first progress; no scheduler job is needed because expiry has no side effects (V1 Sec 10).
- `task_progress_events`: each +1 or completion, with idempotency key, for history and auditing.
- `reward_products`: `couple_id`, `creator_id`, `owner_id` (the partner who buys), title, description, `price_flowers`, `mode` (`direct`, `request`), `status` (`active`, `archived`), `version`.
- `reward_redemptions`: product reference plus snapshot columns (title, description, price, mode), `redeemer_id`, `creator_id`, `status`, `cancel_reason`, cancellation proposal fields, debit and refund transaction references, timestamps.

Membership integrity: services verify that every referenced user is a member of the same couple inside the transaction. A composite foreign key to `couple_members (couple_id, user_id)` is a possible schema-level backstop; it needs an additional unique constraint and is left to the M2/M3 design review.

## 6. Milestones

Each milestone is small enough to review on its own, leaves the app usable, and ends with owner-run acceptance.

| # | Milestone | User-visible result | Contract | Schema | Depends on |
| --- | --- | --- | --- | --- | --- |
| M1 | Nickname | Set and see own nickname; partner sees it | `PATCH /api/v1/me` | None | C7, D17 |
| M2 | Wallet ledger foundation (design: Section 9) | Home shows own flower and sunshine balances (initially 0) | `GET /api/v1/wallet` | Migration `0006_wallet_ledger`, grant changes | D1, D2, D18, D19, D21 |
| M3 | One-off single tasks | Publish to partner, complete, earn rewards, view both lists | Task create/list/complete | `tasks`, `task_periods`, progress events | M2, D5-D10, D20 |
| M4 | Counting completion | "+1" progress with 0/N display; reward on reaching N | Progress endpoint | Uses M3 tables | M3 |
| M5 | Recurring periods | Daily, weekly, custom-range tasks; silent expiry | Schedule fields | Period columns | M4, D4 |
| M6 | Task edit and cancel | Key edits after publication of the current period need recipient confirmation (workflow per D3); cancel | Update/cancel endpoints | `version` checks | M5, D3 |
| M7 | Reward shop | Create, edit, archive products for the partner | Product endpoints | `reward_products` | M2, D13, D14 |
| M8 | Direct redemption | Redeem, debit, pending fulfillment, creator marks fulfilled; history with snapshot | Redemption endpoints | `reward_redemptions` | M7, C2 |
| M9 | Request redemption and cancellation | Request, confirm/reject, withdraw; mutual cancellation with refund | Status transitions | Cancellation fields | M8, D11, D12 |
| M10 | Phase 1 home and navigation | Four-quadrant home with balances and today's tasks; bottom navigation | None or read aggregation | None | M3, M8, D15, D16 |

M2 to M9 involve asset changes. Each of those milestones requires real-database regression tests in `DbGatedTest.cc` covering concurrency (two parallel completions or redemptions), idempotent retry, non-member access, and rollback on failure, in addition to unit tests.

## 7. First Milestone: M1 Nickname (Approved and Owner-Verified 2026-10-06)

### 7.1 Why M1 first

- It closes the only visible user-profile gap left by Phase 0 (members currently show "Nickname not set").
- It is the smallest possible Phase 1 change that exercises the full new-endpoint path: contract change, controller route behind `AuthFilter`, service-layer validation, audit log, frontend store update, and owner-run live checks including Origin/CSRF rejection on a new write endpoint.
- It needs no migration, no role change, and no asset logic, so the review can focus on process and on the security baseline being preserved for new endpoints.
- M2 (ledger) is the highest-risk design; it should start only after D1, D2, D18, and D19 are answered.

### 7.2 User-visible behavior

- On the home page, the current-user card offers "edit nickname". The user enters a nickname and saves.
- On success the new nickname appears immediately in the current-user card and in the user's member slot.
- The partner sees the new nickname in the member slots after a refresh.
- Invalid input (empty after trimming, too long, control characters) shows a Chinese validation message and changes nothing.
- Offline, the save button is blocked by the existing offline mutation guard; nothing is queued.

### 7.3 Contract change (`contracts/openapi.yaml`; approved 2026-10-06)

- New operation `updateMe`: `PATCH /api/v1/me`, cookie session required.
- Request body: `{ "display_name": string }`, required.
- Responses: `200` with the existing `MeResponse`; `400 VALIDATION_ERROR`; `401 UNAUTHENTICATED`; `403` `ORIGIN_NOT_ALLOWED` or `CSRF_TOKEN_INVALID`. All with `Cache-Control: no-store`.
- `info.version` 0.1.0 to 0.2.0; description notes that Phase 0 operations are unchanged and frozen.
- No change to any existing operation, schema, or error code.

### 7.4 Proposed schema change

None. `users.display_name` already exists (`text NOT NULL DEFAULT ''`). Validation is enforced in the service. A database CHECK constraint on length and trimming is possible defense in depth but would need a migration; it is not proposed for M1.

### 7.5 Affected files (expected)

Backend (`services/api/`):

- `src/services/ProfileService.h`, `ProfileService.cc` (new): pure validation function (trim, UTF-8 validity, code-point length, control-character rejection) plus `updateDisplayName(userId, name, cb)` running in one `TxScope`: update the user's own row by session user id, write an `audit_logs` row (`actor_type='user'`, action such as `user.display_name_updated`, no secrets), commit, then return the refreshed user and space. New files are picked up by the existing `GLOB_RECURSE` for `usapp_core`; no CMake change expected (to be confirmed during implementation).
- `src/controllers/SessionController.cc`: add the `PATCH /api/v1/me` route with `AuthFilter`, parse JSON, map outcomes to the existing error envelope.
- Confirm during implementation that `AuthFilter` already applies Origin and CSRF checks to PATCH (expected, since it checks every state-changing method). No filter change is planned; if a change turns out to be needed, it is reported before editing.

Frontend (`apps/web/`):

- `src/api/auth.ts` or a new `src/api/profile.ts`: `updateMe()` via the existing `send()` helper (CSRF header and offline guard come from the client).
- `src/stores/session.ts`: replace the cached user after success; refresh the space store so the member slot updates.
- `src/views/HomeView.vue`: edit control in the current-user card.
- `src/api/error-messages.ts`: Chinese message for validation failure if not already covered.

No changes to admin frontend, migrations, `init-roles.sql`, deployment files, dependencies, or `.claude/settings.json`.

### 7.6 Required tests

- `services/api/tests/UnitTest.cc`: validation boundaries: 1 and 20 code points accepted (ASCII, Chinese, emoji); 0 and 21 rejected; leading/trailing whitespace trimmed; whitespace-only rejected; control characters and line breaks rejected; invalid UTF-8 rejected.
- `services/api/tests/DbGatedTest.cc`, new block 9: the real `ProfileService` updates only the caller's row; a second independent test user is unchanged; an audit row is written without the nickname value or any secret; invalid input changes nothing; independent fixtures are cleaned up by identifier in dependency order, failing the test on cleanup errors.
- `apps/web/tests/unit/`: the API call uses PATCH with the CSRF header through the shared client; the session store updates the user on success and leaves it unchanged on failure.

### 7.7 Owner-run acceptance checks (one step at a time, guided by Codex)

1. Codex reviews the actual diff (contract, backend, frontend, tests).
2. Build the backend test image and run all three CTest targets; confirm `db_gated` ran (not skipped), the new block 9 passed, and the exit code is 0.
3. Build the runtime image and update only the `api` container (handoff Section 9 commands). No migration is needed.
4. In `apps/web`: `corepack.cmd pnpm run test:unit`, then `corepack.cmd pnpm run build`; deploy the web build the same way as the earlier HomeView fix.
5. Browser, account A: edit the nickname on the home page (for example a Chinese name with an emoji, with surrounding spaces). The current-user card and A's member slot show the trimmed value immediately; after a page refresh they still show it (persistence).
6. Browser, account B: refresh; A's slot shows the new nickname; B's own nickname is unchanged; membership remains 2/2.
7. Validation in the browser: whitespace-only and a 21-character nickname show a Chinese error and change nothing.
8. Security, live, with the API directly:
   - wrong Origin: 403 `ORIGIN_NOT_ALLOWED`;
   - authenticated session, missing `X-CSRF-Token`: 403 `CSRF_TOKEN_INVALID`;
   - authenticated session, incorrect `X-CSRF-Token`: 403 `CSRF_TOKEN_INVALID`;
   - no session cookie: 401 `UNAUTHENTICATED`;
   - pre-authentication (logged-out) session with its valid CSRF token: 401 `UNAUTHENTICATED`;
   - body with an extra field such as `user_id`: 400 `VALIDATION_ERROR`;
   - after each rejection, `GET /api/v1/me` shows the nickname unchanged.
9. Offline: with the browser offline, saving shows the offline message; after going back online and refreshing, the nickname is unchanged (nothing was queued).
10. Admin UI: the audit-log list shows `user.display_name_updated` events for the successful saves only; the user list shows the new nickname.

### 7.8 Implementation status (2026-10-06)

Implemented by Claude Code under the owner's M1 approval. Claude Code did not build, test, or exercise anything against a database or browser; all verification was owner-run and is recorded separately in Section 7.9. (When this section was first written, verification had not been run.)

- Backend: new `services/api/src/services/ProfileService.h/.cc`; `PATCH /api/v1/me` route in `services/api/src/controllers/SessionController.cc` behind `AuthFilter`. `GET /api/v1/me` now shares the same response helper; its behavior is intended to be unchanged.
- The request body must be a JSON object with exactly one string field `display_name`; any other field is rejected with 400 instead of being ignored.
- Trimming removes only Unicode space separators (Zs). Tabs and line breaks are not trimmed; they are rejected wherever they occur.
- Audit: `user.display_name_updated`, `target_type='user'`, `couple_id` from the caller's membership (NULL without a space), `detail` is `{"field":"display_name"}` only. Written in the same transaction as the update; success is reported only after the commit callback confirms success.
- `AuthFilter` was confirmed by reading the code to treat PATCH as state-changing (Origin and CSRF checks); no filter change was made.
- Contract: `PATCH /api/v1/me` and `UpdateMeRequest` added; `info.version` 0.2.0; no existing operation changed.
- Frontend: `apps/web/src/api/display-name.ts` (rule mirror for immediate feedback; the server is authoritative), `updateMe()` in `src/api/auth.ts`, `UpdateMeRequest` in `src/api/types.ts`, `updateDisplayName()` in `src/stores/session.ts`, edit control in `src/views/HomeView.vue`.
- Tests: `UnitTest.cc` validation case; `DbGatedTest.cc` block 9; `StaticCheckTest.cc` contract and route checks; `apps/web/tests/unit/update-me.spec.ts` (new) and `session-store.spec.ts` (extended).
- No migration, dependency, deployment, role-grant, or `.claude/settings.json` change.
- Review repair (2026-10-06, after Codex review): the first implementation had a strong ownership cycle (`UpdateCtx` -> `TxScope` -> stored `onError_` -> `UpdateCtx`) that kept the context and the request callback alive after a successful commit or a NotFound rollback. Fixed in `ProfileService.cc` only: the error handler stored by `TxScope` now captures the context weakly; asynchronous SQL and commit callbacks still hold it strongly, so it stays alive while work is pending; all completions go through one exactly-once `complete()` guard; success is still reported only from the confirmed commit callback. `Db.h`/`Db.cc` and other services were not changed. New `DbGatedTest.cc` block 10 checks that a callback-owned sentinel is released within a bounded wait after success, NotFound, and an SQL failure, and that the callback fires exactly once. Transaction-creation failure and commit failure were reviewed in code only. When written, this repair had not been built or tested; it was included in the owner-run checks of Section 7.9.

### 7.9 Owner-run verification results (2026-10-06)

Run by the owner under Codex's guidance after the review repair. These are M1 results only; Phase 0 evidence is recorded separately in `docs/phase0-acceptance.md`.

Builds and automated tests:

- Backend test-image build: exit code 0.
- CTest: `unit`, `db_gated`, and `static_check` all Passed; `db_gated` ran and was not skipped. Timings: `unit` 0.01 s, `db_gated` 0.35 s, `static_check` 0.00 s; total 0.37 s; exit code 0. `db_gated` includes blocks 9 (nickname/audit) and 10 (lifetime sentinels); per-assertion counts were not recorded.
- Runtime image build and `api` container update: exit code 0; container Healthy.
- Web unit tests: 4 test files, 35 tests passed; exit code 0.
- Web build: exit code 0; Service Worker generated.

Live behavior (browser and direct API requests):

- Saving a nickname updated the current-user card and the user's own member slot; the value survived a page refresh.
- The partner saw the new nickname after refreshing; the partner's own nickname was unchanged; membership remained 2/2.
- Whitespace-only input, 21 emoji code points, and a request with an extra body field each returned 400 `VALIDATION_ERROR`; the nickname was unchanged.
- Wrong Origin: 403 `ORIGIN_NOT_ALLOWED`.
- Correct Origin without a session cookie: 401 `UNAUTHENTICATED`.
- Authenticated requests with a missing or an incorrect `X-CSRF-Token`: 403 `CSRF_TOKEN_INVALID`; the nickname was unchanged.
- Pre-authentication session with its valid CSRF token: 401 `UNAUTHENTICATED`.
- Offline: the save was blocked; no PATCH was sent, and none was replayed automatically after reconnecting; the nickname was unchanged after refresh.
- Admin UI: the user list showed the new nickname; the audit list showed the update event. The admin UI and admin API do not expose audit `detail`.
- Owner SQL query: audit row id 10, action `user.display_name_updated`, `detail` `{"field":"display_name"}`; exit code 0.

Limits of this evidence:

- These were targeted checks of one endpoint and its UI. They are not exhaustive security verification.
- Audit `detail` content was confirmed by a single owner SQL query, not through the admin UI/API (which do not expose it).
- Absence of nickname values from application logs was not separately inspected.
- Concurrent nickname updates and the transaction-creation and commit-failure paths were not exercised live; the latter two remain code-review only.
- HEAD/OPTIONS behavior on `/api/v1/me` was not checked.

## 8. Phase 0 Items That Remain Open

Phase 0 acceptance does not close these. They are tracked, not re-verified, by this plan (details in `docs/phase0-acceptance.md` and handoff Section 7):

- Invalid, expired, and revoked session cookies are covered by tests only, not by live checks.
- HEAD/OPTIONS handling on administrator routes is checked only at filter level.
- The cause of the earlier `Transaction roll back error` is unverified. Phase 1 asset transactions must not rely on any assumption about it; M2+ tests must cover rollback paths explicitly.
- The installed app's first-launch blank screen has no confirmed cause.
- Real email delivery and real push delivery are unverified; the push helper is disabled.
- Live multi-instance scheduler concurrency is unverified (Phase 1 as proposed adds no scheduler job).
- Backup evidence lacks row-by-row equality, off-site or encrypted copies, and recovery timing. New Phase 1 tables will need to be included in the next backup/restore check.
- Live user-side Origin/CSRF negative checks covered only `POST /api/v1/auth/email-codes`.
- Older guides (`docs/phase0-handoff.md`, `deploy/README.md`, `services/api/tests/README.md`) contain outdated statements; `docs/architecture.md` is a historical snapshot.
- Container temporary dump files from the backup check were not cleaned up.

## 9. M2 Wallet and Ledger: Detailed Design Proposal

Status (revised 2026-10-06):

- **Approved design directions (owner, 2026-10-06):** D1, D2 (balance and per-entry bounds only), D18, D19, D21; see Section 9.9.
- **Not approved:** M2 implementation, migrations, grant changes, `init-roles.sql` changes, contract changes, and any database execution. Nothing in this section exists in the repository.
- This revision corrects four points raised in review: the ledger-chain invariant (Section 9.4), the rollback-only test strategy (Section 9.10), role assumptions (Section 9.6), and system-actor idempotency (Section 9.7).
- Second review revision (2026-10-07): the M2 concurrency fixture is account-creation contention only, with existing-account locking and post-commit visibility deferred to M3 (Sections 9.10, 9.11); exact permission outcomes (Section 9.11); an effective table and column privilege matrix for API and test preflights (Section 9.6); and the limits of rollback evidence with bounded teardown checks (Section 9.10).

### 9.1 Scope

In M2:

- Schema for wallet accounts, wallet operations, and the append-only ledger (one new migration), with database-level invariants and immutability enforcement.
- Narrowed privileges for the application role on the new tables, kept stable when `init-roles.sql` is re-run.
- One read-only endpoint, `GET /api/v1/wallet`, returning the caller's own flower and sunshine balances.
- Home page shows the caller's own balances. Because nothing in M2 writes to the wallet, both are 0.
- A non-secret role preflight for the API and for `db_gated` (Section 9.6).

Not in M2: task settlement or any other posting path, redemption, refunds, administrative asset correction, admin asset endpoints, partner balances, idempotency handling in request code (schema only; first used in M3), notifications.

### 9.2 Source findings that shape the design

- `database/init-roles.sql` grants `SELECT, INSERT, UPDATE, DELETE ON ALL TABLES` to `usapp_api` and sets default privileges giving the same on future tables. `database/apply.ps1 -InitRoles` re-runs it after migrations, so a revoke placed only in a migration would be undone.
- `deploy/docker-entrypoint-initdb.d/01-init-roles.sh` runs once on an empty data volume, before any migration. It creates the runtime roles from configurable names (`${API_USER}`, `${PUSH_USER}`), whereas `init-roles.sql` hard-codes `usapp_api` and `usapp_push`.
- Migrations run as the maintenance superuser, one transaction each; applied migrations are checksum-protected and are never edited.
- PostgreSQL requires UPDATE privilege on at least one column for `SELECT ... FOR UPDATE` (PostgreSQL GRANT documentation, UPDATE privilege). Taking a row lock on `wallet_accounts` therefore needs some UPDATE privilege even if updates themselves are forbidden.
- Drogon v1.9.11 `TransactionImpl::execSqlInLoop` wraps every exception callback so that it calls `rollback()` before invoking the caller's callback, and every later command on that `Transaction` is rejected with "The transaction has been rolled back" ([source](https://github.com/drogonframework/drogon/blob/v1.9.11/orm_lib/src/TransactionImpl.cc)). A Drogon transaction cannot continue after an expected SQL failure, so `ROLLBACK TO SAVEPOINT` through it is not a usable test technique.
- Drogon binds numeric parameters in binary format; C++ `int64_t` must pair with `::bigint` casts (handoff Section 5).

### 9.3 Proposed tables (migration `0006_wallet_ledger.sql`)

Indicative DDL shape; final SQL is written only after approval.

`wallet_accounts`: lock anchor and membership binding for one (user, asset). It deliberately stores **no balance and no sequence**, so there is no mutable copy of ledger state for the application role to forge.

| Column | Type | Rule |
| --- | --- | --- |
| `user_id` | uuid | part of primary key; FK to `users` ON DELETE RESTRICT |
| `asset_type` | text | part of primary key; CHECK in (`flower`, `sunshine`) |
| `couple_id` | uuid | composite FK `(couple_id, user_id)` to `couple_members` ON DELETE RESTRICT |
| `lock_token` | smallint | NOT NULL DEFAULT 0, CHECK `lock_token = 0`; exists only so the application role can hold column-level UPDATE privilege for `FOR UPDATE` row locks |
| `created_at` | timestamptz | |

`wallet_operations`: one row per user-visible action, for example "settle this task period".

| Column | Type | Rule |
| --- | --- | --- |
| `id` | uuid | primary key; plus UNIQUE `(id, couple_id)` for composite FKs |
| `couple_id` | uuid | FK to `couples` ON DELETE RESTRICT |
| `actor_user_id` | uuid | **NOT NULL**; composite FK `(couple_id, actor_user_id)` to `couple_members` |
| `operation_type` | text | CHECK in (`TASK_REWARD`, `REWARD_REDEMPTION`, `REDEMPTION_REFUND`); new types need a new migration |
| `source_type`, `source_id` | text | the business row that caused the operation |
| `idempotency_key` | text NULL | CHECK length 16-128 when present |
| `request_fingerprint` | text NULL | SHA-256 hex; CHECK present if and only if `idempotency_key` is present |
| `created_at` | timestamptz | |

There is no `actor_type` column and no system actor in Phase 1 (Section 9.7). Unique indexes: `(actor_user_id, operation_type, idempotency_key) WHERE idempotency_key IS NOT NULL`; `(operation_type, source_type, source_id)`.

`wallet_ledger_entries`: append-only.

| Column | Type | Rule |
| --- | --- | --- |
| `id` | uuid | primary key |
| `operation_id` | uuid | composite FK `(operation_id, couple_id)` to `wallet_operations (id, couple_id)` |
| `couple_id`, `user_id` | uuid | composite FK to `couple_members (couple_id, user_id)` |
| `asset_type` | text | FK `(user_id, asset_type)` to `wallet_accounts`; CHECK in (`flower`, `sunshine`) |
| `seq` | bigint | per-account sequence, starting at 1 |
| `amount` | bigint | CHECK `amount <> 0`; `abs(amount) <= 999` for flower, `<= 100000` for sunshine |
| `balance_after` | bigint | CHECK `0 <= balance_after`; `<= 1000000` for flower, `<= 1000000000` for sunshine |
| `created_at` | timestamptz | |

Unique: `(user_id, asset_type, seq)` (also the index used to find the latest entry); `(operation_id, user_id, asset_type)`.

Also in 0006: `ALTER TABLE couple_members ADD CONSTRAINT couple_members_couple_user_unique UNIQUE (couple_id, user_id)`, a new constraint in a new migration (0003 is not edited).

A balance is read as the `balance_after` of the account's highest-`seq` entry, or 0 when there is none. This is one index lookup per asset.

### 9.4 Invariants and the ledger chain

| ID | Invariant | Enforced by |
| --- | --- | --- |
| W1 | Every `balance_after` is within `[0, cap(asset)]` | CHECK |
| W2 | Every `amount` is non-zero and within `op_cap(asset)` | CHECK |
| W3 | Per account, `seq` is 1, 2, 3, ... with no gaps or repeats, and `balance_after` equals the previous entry's `balance_after` plus `amount`, where "previous" for `seq = 1` is the opening state (0) | BEFORE INSERT chain trigger (below) plus UNIQUE `(user_id, asset_type, seq)` |
| W4 | One ledger row per INSERT statement | AFTER INSERT statement-level trigger with a transition table: raise if more than one row |
| W5 | Ledger entries, operations, and accounts are never updated, deleted, or truncated (the column-level UPDATE grant on `lock_token` exists only to permit row locks; the trigger rejects every UPDATE); mistakes are fixed only by new compensating operations (Phase 5) | narrowed privileges plus triggers (Section 9.5) |
| W6 | An entry's user is a member of the entry's couple; the entry's couple equals its operation's couple; the operation's actor is a member of that couple | composite FKs |
| W7 | At most one entry per (operation, account); one operation may have one entry per asset | UNIQUE `(operation_id, user_id, asset_type)` |
| W8 | Every operation has at least one entry | DEFERRABLE INITIALLY DEFERRED constraint trigger on `wallet_operations`, checked at commit |
| W9 | An operation is unique per idempotency scope and per business source | partial UNIQUE and UNIQUE indexes |

Chain trigger (`BEFORE INSERT FOR EACH ROW` on `wallet_ledger_entries`). It derives everything from the previous **immutable** ledger entry and never reads a mutable copy:

1. Lock the account row: `SELECT 1 FROM wallet_accounts WHERE user_id = NEW.user_id AND asset_type = NEW.asset_type FOR UPDATE`; raise if the account does not exist.
2. Read the previous entry: `SELECT seq, balance_after FROM wallet_ledger_entries WHERE user_id = NEW.user_id AND asset_type = NEW.asset_type ORDER BY seq DESC LIMIT 1`. If there is none, the opening state is `seq = 0`, `balance_after = 0`. There is no opening-balance entry type and no way to start an account at a non-zero balance.
3. Require `NEW.seq = previous seq + 1` and `NEW.balance_after = previous balance_after + NEW.amount`; otherwise raise. W1 then bounds the result.

Why this closes the reported gap: the earlier proposal compared against `wallet_accounts.balance/last_seq`, which the application role could UPDATE. A forged intermediate value followed by a matching insert could pass both the insert check and the final deferred check while breaking arithmetic against the previous entry. Now accounts hold no balance state, the application role cannot update or delete ledger rows, and every insert is checked against the last immutable entry.

Concurrency:

- Two transactions posting to the same account serialize on the account row lock taken in step 1. (The service takes the same lock earlier, in sorted order; re-locking in the trigger is a no-op.)
- After the waiter acquires the lock, step 2 runs as a new statement. Under READ COMMITTED, statements inside a volatile PL/pgSQL function take a fresh snapshot, so it sees the entry the other transaction just committed. It then requires the next `seq`; a request that was computed before the wait fails cleanly instead of forking the chain.
- The UNIQUE `(user_id, asset_type, seq)` index is an independent backstop: even if a future change dropped the lock, two entries with the same `seq` could not both commit.
- These concurrency properties are design reasoning. M2 tests cover only account-creation contention; existing-account lock serialization and post-commit visibility are tested in M3 (Section 9.10 item 4).
- Postings must run at READ COMMITTED (the Drogon default). Under REPEATABLE READ the chain query could see a stale snapshot; the unique index would still prevent corruption, but the transaction would fail instead of proceeding.

Multiple entries in one transaction:

- Rows inserted by earlier statements of the same transaction are visible to the chain trigger's query, so several entries for one account in one transaction (for example from different operations) chain correctly: `seq` 1, 2, 3 with running balances.
- One INSERT statement may insert only one ledger row (W4). This avoids relying on intra-statement visibility rules for row-level BEFORE triggers.
- One operation that rewards both assets writes two rows to two different accounts (W7), each chained independently.

### 9.5 Immutability enforcement and exact grant changes (D18, approved direction)

In `0006_wallet_ledger.sql`, after the tables are created:

```sql
REVOKE ALL ON wallet_ledger_entries, wallet_operations, wallet_accounts FROM usapp_api;
REVOKE ALL ON wallet_ledger_entries, wallet_operations, wallet_accounts FROM usapp_push;
GRANT SELECT, INSERT ON wallet_ledger_entries, wallet_operations, wallet_accounts TO usapp_api;
GRANT UPDATE (lock_token) ON wallet_accounts TO usapp_api;
```

Resulting `usapp_api` privileges:

| Table | SELECT | INSERT | UPDATE | DELETE | TRUNCATE |
| --- | --- | --- | --- | --- | --- |
| `wallet_ledger_entries` | yes | yes | no | no | no |
| `wallet_operations` | yes | yes | no | no | no |
| `wallet_accounts` | yes | yes (via `ON CONFLICT DO NOTHING`) | `lock_token` column only, for `FOR UPDATE`; every actual UPDATE is rejected by trigger | no | no |

`usapp_push` has no privileges on any wallet table.

Triggers, which also bind the table owner and the maintenance role:

- `BEFORE UPDATE OR DELETE FOR EACH ROW` on all three tables: raise ("append-only"). `SELECT ... FOR UPDATE` does not fire UPDATE triggers, so row locking still works.
- `BEFORE TRUNCATE FOR EACH STATEMENT` on all three tables: raise.
- The chain trigger and statement trigger in Section 9.4.

A superuser can still deliberately bypass triggers (`ALTER TABLE ... DISABLE TRIGGER` or `session_replication_role`). This is accepted as an explicit, owner-only recovery path outside the application's reach.

Alternative considered for row locking: a transaction-level advisory lock keyed by account instead of `FOR UPDATE`. It needs no table privilege, but locking happens outside the table, hash collisions over-serialize, and the key space must not collide with the scheduler and migration locks. Not recommended.

`database/init-roles.sql` change (re-runnable, not a migration): append a final section that, for each wallet table where `to_regclass('public.<table>')` is not NULL, re-applies exactly the REVOKE/GRANT statements above. Without it, `apply.ps1 -InitRoles` would restore UPDATE and DELETE through the ALL TABLES grant. Before 0006 is applied the section does nothing.

### 9.6 Supported roles and non-secret preflight

Supported configuration for M2: the API runtime role and the `db_gated` test role are both exactly `usapp_api`; the push role is exactly `usapp_push`; both are non-superuser and own no wallet table. Other role names are **unsupported** for M2. A migration that grants to `usapp_api` does not fail when the API actually connects as a different role that also exists. It would simply leave that other role with whatever default privileges it has. Configuration alone therefore cannot be trusted; preflights check the live identity.

Preflights (no secrets, no connection strings, no `.env` reads):

- Migration 0006 begins with a DO block that raises if `usapp_api` or `usapp_push` does not exist, or if either has `rolsuper`.
- API startup (M2 code change, to be approved with M2) and `db_gated` start both run the same **effective privilege matrix check**, implemented once in `usapp_core` so the API and the tests cannot drift apart. The API refuses to start on any mismatch. The `db_gated` wallet blocks fail (not skip). If the wallet tables do not exist yet (0006 not applied), both stop with a clear message. Output is only the role name, table/column/privilege names, and booleans.

Effective privilege matrix (checked for `current_user`, which must be `usapp_api`):

| Check | Required result |
| --- | --- |
| `current_user` | `usapp_api` |
| `rolsuper` | false |
| Owner of each wallet table (`pg_class.relowner`) | not `current_user` (owners hold every privilege implicitly) |
| Role memberships of `current_user` (`pg_auth_members`) | none, so no privilege can be gained by `SET ROLE` to another role, whether or not that role is inherited |
| `has_table_privilege(current_user, t, 'SELECT')` and `'INSERT'`, for each wallet table | true (required) |
| `has_table_privilege(current_user, t, p)` for p in `UPDATE`, `DELETE`, `TRUNCATE`, `REFERENCES`, `TRIGGER`, `MAINTAIN` (PostgreSQL 17), for each wallet table | false (forbidden) |
| `has_column_privilege(current_user, 'wallet_accounts', 'lock_token', 'UPDATE')` | true (required, only for row locking) |
| `has_column_privilege(current_user, t, c, 'UPDATE')` for every other column c of every wallet table, enumerated from the catalog at check time so future columns are covered | false (forbidden) |
| The same table and column privileges for the pseudo-role `public` | all false |

`has_table_privilege` and `has_column_privilege` report effective privileges: direct grants, grants to `PUBLIC`, and grants inherited through role membership. A role that is not inherited can still be reached with `SET ROLE`, and the `has_*` functions do not count it; the "no role memberships" check closes that path. `db_gated` additionally checks `usapp_push` against an all-false matrix. The owner-run query of Section 9.12 checks both roles as the maintenance role.
- Owner-run, as the maintenance role: `SELECT rolname, rolsuper FROM pg_roles WHERE rolname IN ('usapp_api', 'usapp_push')` and the privilege query of Section 9.12. The owner confirms that the configured runtime role name is `usapp_api` using a method that does not display passwords. Claude Code never reads `.env` and never prints connection strings.

### 9.7 Idempotency scope (used from M3; schema in M2)

- An operation is one user-visible action. One task-period settlement that rewards 1 flower and 40 sunshine is one `wallet_operations` row with two ledger entries (one per asset account), written in the same transaction: both exist or neither does.
- The key identifies the action, so one key covers both entries. Scope: `(actor_user_id, operation_type, idempotency_key)`, where `actor_user_id` is NOT NULL, so the unique index gives a real guarantee.
- **No system actor in Phase 1.** Every Phase 1 operation is triggered by a signed-in member (task completion by the recipient, redemption and refund by members). `actor_type` and a nullable actor are removed from the proposal. A nullable actor would let unique indexes treat NULLs as distinct and silently lose the guarantee.
- If a later phase adds system operations (for example scheduler-driven or admin adjustments), a new migration must either give them a non-null scope (for example a NOT NULL `system_actor` text such as the job key, with its own unique index) or forbid idempotency keys for them and rely only on business-source uniqueness. That is not part of M2.
- The client generates the key (random UUID) once per user action, keeps it only in memory, and reuses it only for an immediate retry of the same request; it is never persisted or queued.
- Replay: same scope and same `request_fingerprint` returns the original result with no new rows. Same scope with a different fingerprint returns 409 with a new error code (contract change in M3).
- Concurrent duplicates: the second insert waits on the unique index and fails with a unique violation after the first commits. The service then reads the committed operation in a new transaction and returns it as a replay.
- Business uniqueness `(operation_type, source_type, source_id)` holds without a key, so two different keys cannot settle the same source twice.

### 9.8 Transaction and locking rules (binding for every posting path from M3)

1. One business action is one `TxScope` transaction: lock the business row, check membership, insert the operation, lock accounts, insert ledger entries (one per statement), update the business row, insert the audit row, commit. The response is sent only from the confirmed commit callback; no network side effects inside the transaction.
2. Lock order, always: business row -> `couple_members` row (`FOR SHARE`) -> operation insert -> wallet accounts in ascending `(user_id, asset_type)` order.
3. Account creation: `INSERT ... ON CONFLICT (user_id, asset_type) DO NOTHING`, then `SELECT ... FOR UPDATE`.
4. A debit reads the latest entry only after the account lock is held, checks sufficiency in the service, and returns a domain error before any insert. The chain trigger and CHECK constraints are backstops.
5. READ COMMITTED isolation; no decision is based on an unlocked read.
6. All numeric parameters are `int64_t` with explicit `::bigint` casts.
7. Every posting writes one `audit_logs` row in the same transaction (no secrets).
8. The M2 read path is a plain read-committed SELECT without locks; it never creates rows.

### 9.9 Decisions

Approved design directions (owner, 2026-10-06):

- **D1:** task completion creates new flowers and sunshine for the recipient; it does not debit the publisher. In ledger terms, a `TASK_REWARD` operation has only credit entries.
- **D2 (bounds only):** maximum balance 1,000,000 flowers and 1,000,000,000 sunshine; maximum absolute amount per ledger entry 999 flowers and 100,000 sunshine; operations exceeding bounds are rejected, never clamped. Storage is `bigint` with these CHECKs; values stay below 2^53 for JavaScript. **Not approved by this decision:** task reward ranges (for example "not both zero"), product price ranges, and every other product or task rule. Those remain open for M3 and M7.
- **D18:** narrowed grants plus database triggers, and the restrictions must survive re-running the role-initialization script (Section 9.5).
- **D19:** no new administrator asset endpoints in M2.
- **D21:** M2 displays only the caller's own balances.

Alternatives recorded for context (not chosen): transferred rewards with escrow (D1); no caps beyond type range (D2); triggers only, or a `SECURITY DEFINER` posting function as the only write path (D18); read-only admin wallet views (D19); showing partner balances (D21).

These directions do not approve implementation. Writing the migration, changing `init-roles.sql`, changing the contract, writing code, and running anything against a database still require the owner to approve and assign M2.

### 9.10 Test strategy under immutability

Constraints: the application role cannot update or delete wallet rows; triggers block it for the maintenance role too; FK `ON DELETE RESTRICT` prevents deleting users, couples, or memberships referenced by committed wallet rows. A Drogon transaction is rolled back and unusable after any SQL error (Section 9.2).

Recommended approach, with no committed wallet rows and no weakened permissions or triggers:

1. **`db_gated`, one independent transaction per case.** Each case opens its own transaction through the existing transaction helper. Unless stated otherwise, it creates its own fixtures inside that transaction (user, couple, membership, account, operation, prior entries), then runs exactly one statement expected to fail, or one sequence expected to succeed. The account-creation contention case (Section 9.11) is the exception: it uses committed non-wallet fixtures shared by two transactions.
   - Expected failure: the case waits (bounded) for the error callback and asserts the SQLSTATE or constraint/trigger name. Drogon has already requested rollback, so nothing is committed and the fixtures disappear with it. No `ROLLBACK TO SAVEPOINT` is used.
   - Expected success: the case finishes with `SET CONSTRAINTS ALL IMMEDIATE`, which forces the deferred checks (W8) to run inside the transaction, and then requests rollback instead of committing.
   - Deferred-failure cases: one case ends with `SET CONSTRAINTS ALL IMMEDIATE` and expects the error there. One case attempts a real commit and expects the commit to fail (operation without entries), proving the trigger fires at commit. A failed commit leaves nothing behind.
   - Committed-row check: after each case, a query on another connection confirms that no wallet row references the case's fixture identifiers. This proves only that **no committed rows are visible**. It does not prove that the rollback has finished, that the transaction's locks are released, or that the connection is back in the pool, because uncommitted rows are invisible to other connections either way.
   - Bounded teardown check: the first statement of every case transaction records `pg_backend_pid()`. After the case's rollback is requested (explicitly or by Drogon on error), another connection polls, with a bound (for example 5 s total, 20 ms steps), until `pg_stat_activity` shows that pid with `state = 'idle'` and `backend_xid IS NULL`, and `pg_locks` shows no relation, tuple, or transactionid locks held by that pid. A non-superuser can see these columns for sessions of its own role. Passing shows that the server-side transaction ended and released its locks within the bound. It does not show when the Drogon client callback ran or that Drogon returned the connection to its pool; the next case acquiring a transaction within its own bounded wait is the practical check for that. On timeout the case fails and reports only the pid and observed state, and it does not block on the stuck transaction.
   - Case fixtures use fresh random identifiers, so even a slow rollback cannot make one case's rows or locks collide with the next case.
   - Whether the Drogon `SqlError` exposes the SQLSTATE in v1.9.11 must be confirmed during implementation. If it does not, the case asserts the constraint or trigger name in the error text.
2. **Owner-run `psql` script** (new `database/tests/wallet_ledger_test.sql`, written with M2), in the style of `constraints_test.sql`. It runs in one `BEGIN ... ROLLBACK` and uses savepoints or `DO` blocks with `EXCEPTION` handlers for expected failures; `psql` sessions can recover with `ROLLBACK TO SAVEPOINT`, unlike a Drogon transaction. It is run twice: as `usapp_api` (privilege behavior) and as the maintenance role (trigger behavior when privileges are not the barrier).
3. **Committed wallet rows are never created in M2.** M2 may commit only non-wallet fixtures (users, couple, membership), which are removed with the existing delete-by-id cleanup because no committed wallet row references them.
4. **Deferred to M3, with its own approval:** lock serialization on an *existing* (committed) account, and the post-commit fresh-snapshot behavior of the chain trigger (a waiter seeing an entry committed by another transaction). Both need committed wallet rows, which can never be deleted. Proposed M3 strategy:
   - an isolated fixture set per test run: its own users, couple, and memberships with random identifiers and a reserved test email domain, committed once and never reused by other cases;
   - committed wallet rows and the non-wallet fixtures they reference stay in `usapp_test` permanently (immutability and `ON DELETE RESTRICT`); tests filter strictly by their own identifiers and never assert global counts or empty tables;
   - a guard that the connected database name is the test database (in addition to the existing DSN guard), so such tests can never commit wallet rows to `usapp`;
   - `usapp_test` is treated as disposable; the owner may drop and recreate it with `apply.ps1 -CreateDatabase -InitRoles` as maintenance, outside the test run.
5. No test-only bypass: no DELETE grant, no "test mode" in triggers, no `SECURITY DEFINER` purge function. Tests never run against the main `usapp` database.

### 9.11 Proposed regression tests

`db_gated` (as `usapp_api`, one independent transaction per case):

- Role preflight and migration gate: `current_user = 'usapp_api'`, not superuser; `0006_wallet_ledger` applied.
- Effective privilege matrix (Section 9.6), required and forbidden entries, for `usapp_api`, `usapp_push` (all false), and `public` (all false); `usapp_api` owns no wallet table and is a member of no role.
- Trigger catalog: every trigger in Sections 9.4 and 9.5 exists and is enabled.
- Chain and tampering cases:
  - first entry with `seq = 1`, `balance_after = amount` accepted;
  - **fabricated opening balance:** first entry with `balance_after` not equal to `amount` (for example amount 1, balance_after 1000) rejected;
  - first entry with `seq` other than 1 rejected;
  - **tampered sequence:** next entry skipping a number or repeating one rejected;
  - **tampered intermediate balance:** next entry whose `balance_after` is based on a value other than the previous entry's `balance_after` rejected;
  - **forged account state:** no UPDATE of `wallet_accounts` can succeed (exact outcomes in the permission cases below), so no mutable state can feed the chain;
  - two entries for the same account in one transaction (different operations, separate statements) chain correctly; a third entry computed from the first instead of the second is rejected;
  - multi-row INSERT into the ledger rejected (W4);
  - debit that would make `balance_after` negative rejected; credit beyond the balance cap rejected; amount beyond the per-entry cap rejected; zero amount rejected; unknown asset rejected.
- **Account-creation contention** (no committed wallet rows): committed shared non-wallet fixtures (one user, couple, and membership, removed afterwards by delete-by-id) are created first. Then:
  1. Transaction A runs `INSERT INTO wallet_accounts ... ON CONFLICT (user_id, asset_type) DO NOTHING` for that user and asset and stays open.
  2. Transaction B runs the same statement. It must not complete within a bounded wait (for example 300 ms). As corroboration, another connection may observe B's pid in `pg_stat_activity` with `wait_event_type = 'Lock'`.
  3. A is rolled back. B then completes within a bounded wait, reporting one inserted row.
  4. B is rolled back. The teardown check (Section 9.10) runs for both pids, and the committed-row check finds no wallet rows for the fixture user.

  This shows only that concurrent creation of the same account is resolved by the unique-key wait and that B proceeds after A aborts. It is **not** evidence of `FOR UPDATE` serialization on an existing account, and not evidence that a waiter sees a committed ledger entry. An account created inside A is invisible to B, so B cannot lock it. Those two behaviors are deferred to M3 (Section 9.10 item 4).
- Structure: second entry for the same (operation, account) rejected; entry for a non-member rejected; entry whose couple differs from its operation's couple rejected; operation by a non-member actor rejected; operation without entries rejected at `SET CONSTRAINTS ALL IMMEDIATE` and, separately, at commit.
- Idempotency schema: duplicate key in the same scope rejected; same key for a different actor accepted; duplicate business source rejected; key without fingerprint rejected.
- Permission cases as `usapp_api` (each in its own transaction):
  - `UPDATE wallet_accounts SET lock_token = 0 ...`: passes the privilege check (column-level grant) and is rejected by the BEFORE UPDATE trigger. The case asserts the trigger's exception, not 42501.
  - UPDATE of any other `wallet_accounts` column (`user_id`, `asset_type`, `couple_id`, `created_at`), alone or together with `lock_token`: insufficient privilege (SQLSTATE 42501).
  - UPDATE of any column of `wallet_ledger_entries` or `wallet_operations`: 42501.
  - DELETE on each of the three tables: 42501.
  - TRUNCATE on each of the three tables: 42501.
  - Positive: `SELECT ... FOR UPDATE` on a `wallet_accounts` row created in the same transaction succeeds (it relies on the `lock_token` column grant).
- `WalletService` read (committed non-wallet fixtures only, existing delete-by-id cleanup): member without entries gets 0/0; user without a space gets `SpaceNotFound`.
- Not in M2: service-level posting, idempotent replay, rollback of a failed posting (no posting path yet), lock serialization on an existing account, and post-commit snapshot visibility (need committed wallet rows; Section 9.10 item 4). These are required in M3.

Owner-run `psql` script (Section 9.10 item 2): the same chain, tampering, structure, and `usapp_api` permission cases. Separately, as the maintenance role, where privileges are not the barrier: UPDATE (any column), DELETE, and TRUNCATE on each table each raise the trigger exception.

`unit`: response shaping; cap constants match the migration values. `static_check`: 0006 contains the REVOKE/GRANT statements, triggers, and role preflight; `init-roles.sql` contains the re-narrowing section; the contract contains `getWallet`. Web: wallet API uses GET through the shared client; store reset on logout; home shows 0/0 and never shows numbers after a failed load.

### 9.12 Read endpoint, expected files, and owner-run acceptance steps

Read endpoint: `GET /api/v1/wallet` behind `AuthFilter`; authenticated session required (pre-authentication 401); `Cache-Control: no-store`; response `{ "wallet": { "flower": 0, "sunshine": 0 } }` with JSON integers; 404 `SPACE_NOT_FOUND` without a couple; only the caller's own balances; no write methods on this path.

Expected files: `database/migrations/0006_wallet_ledger.sql`; `database/init-roles.sql`; `database/tests/wallet_ledger_test.sql`; `database/README.md`; new `services/api/src/services/WalletService.h/.cc` (read only) and the startup role preflight; new `services/api/src/controllers/WalletController.cc` plus its entry in the explicit `add_executable(usapp_api ...)` list in `services/api/CMakeLists.txt`; `contracts/openapi.yaml` (0.3.0, additive); `apps/web/src/api/wallet.ts`, a wallet store registered in `resetBusinessState()`, `HomeView.vue`; tests above.

Owner-run acceptance (after M2 is approved and implemented):

1. Codex reviews the migration SQL, the `init-roles.sql` diff, the psql test script, the contract, code, and tests.
2. Back up the main database with the existing procedure.
3. As the maintenance role, confirm `usapp_api` and `usapp_push` exist and are not superusers; confirm the configured runtime role name is `usapp_api` without displaying passwords.
4. `apply.ps1 -DryRun` against `usapp_test`: only `0006_wallet_ledger` is pending.
5. Apply to `usapp_test` with `-InitRoles`. As the maintenance role, run the effective privilege matrix query (Section 9.6) for `usapp_api`, `usapp_push`, and `public`, plus the ownership and role-membership checks; every required entry is true and every forbidden entry is false.
6. Run `apply.ps1 -InitRoles` again (no pending migrations) and repeat step 5: the grants are still narrowed.
7. Run `database/tests/wallet_ledger_test.sql` against `usapp_test` as `usapp_api` and as the maintenance role; both end with ROLLBACK and report no failures.
8. Build the backend test image and run all CTest targets; `db_gated` not skipped; role preflight reports `usapp_api`; new wallet blocks pass. Afterwards, the wallet tables in `usapp_test` contain no rows.
9. Apply to the main database with `-InitRoles`; repeat step 5 there.
10. Build the runtime image and update the `api` container; it starts, its preflight logs `usapp_api` and passes, and the container is Healthy. Run web unit tests, build, and deploy.
11. Browser, both accounts: home shows 0 flowers and 0 sunshine and keeps showing them after refresh; neither account sees the other's balances.
12. API: `GET /api/v1/wallet` without a cookie and with a pre-authentication session -> 401; authenticated -> 200 with 0/0 and `Cache-Control: no-store`; write methods are not routed.
13. Database: the three wallet tables in the main database contain no rows; no wallet audit events exist.
14. Recommended before M3: include the new tables in a backup/restore check.
