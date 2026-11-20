# USapp Phase 1 Plan: Couple Core Loop

Status (updated 2026-10-25, Asia/Shanghai): M4 deployment and representative live evidence are in Section 10.8; the owner waived the remaining M4 live checks and started M5-A daily counting. M5-A is deployed on main with its remaining live checks waived: [phase1-m5-daily.md](phase1-m5-daily.md), Verification. M5-B weekly counting passed Codex-run automated and SQL compatibility checks; 0010 is applied/frozen on test/main, main API/web rollout completed, live weekly workflows remain unverified: [phase1-m5-weekly.md](phase1-m5-weekly.md), Verification results. Earlier milestone evidence remains historical.

- **M1 (nickname): approved, implemented, and owner-run acceptance checks completed successfully on 2026-10-06.** Implementation status is in Section 7.8; the owner's verification evidence and its limits are in Section 7.9. The approval covered M1 only, including the additive `PATCH /api/v1/me` operation and OpenAPI `info.version` 0.2.0.
- **M2 (wallet/ledger):** design in Section 9, with owner-approved directions D1, D2 (balance and per-entry bounds only), D18, D19, and D21 (2026-10-06). M2 is split:
  - **M2-A (database foundation): approved and implemented 2026-10-07; owner-run verification on `usapp_test` passed 2026-10-07 (Section 9.14).** The owner also applied frozen 0006 to the main database during M2-B rollout (Section 9.19). Claude Code itself built, tested, or executed none of it.
  - **M2-B (read endpoint, API startup privilege check, home display): owner-authorized for direct Codex implementation on 2026-10-07; implemented and passed owner-run verification (Section 9.19, updated 2026-10-09).** A separate formal M2 acceptance decision has not been recorded. The owner committed and pushed M2-A/M2-B on 2026-10-09 (Section 9.20, dated observation).
- **M3 (one-off tasks with single completion and reward settlement): design in [docs/phase1-m3-design.md](phase1-m3-design.md) (revision 3). On 2026-10-09 the owner approved M3-A (task database foundation) and confirmed the product decisions listed in that document's Section 2.3 (D5, D5a, D5b, D5c, D6, D7, D8, D20, D22, D22a, D23, D24, D24a, D25, D26, D27, D28, D31, D34, D35). M3-A is implemented by Claude Code and passed the recorded owner-run verification on `usapp_test` on 2026-10-09 (0007 applied there with checksum `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`; CTest, task SQL script and repaired wallet SQL script passed in the recorded modes; final fixture query passed; design Section 11.3, with limits). No separate formal M3-A acceptance decision is recorded. The owner committed and pushed M3-A as `6a9da4b` on 2026-10-09 (design Section 12.1; dated observation). M3-B (publication and read APIs, task list/publication UI, task startup enforcement, per-run isolated committed task fixtures) was approved by the owner on 2026-10-09, implemented by Claude Code, and on 2026-10-10 passed the recorded owner-run checks (CTest targets, web tests/type check/build, fresh main backup with a count-level restore check, main 0007 application, deployment with both startup policies `ok=true`, targeted live checks; design Section 12.9, with limits). 0007 is now applied to the main database and M3-B is deployed. On 2026-10-10 the owner accepted M3-B as a local-development milestone within the scope and limitations of those checks (not production readiness) and reported the M3-B commit `380696b` pushed to `origin/main` (design Section 12.10; dated observation). M3-C implementation and owner-run evidence are recorded below and in its design Sections 16-18; D29 remains unauthorized and D33 deferred.**
- **M3-C:** M3-C1/M3-C2 were approved and implemented on 2026-10-14; the owner ran the recorded backend/web automated checks, deployed them and reported representative live checks successful. Evidence and limits live once in [phase1-m3c-design.md](phase1-m3c-design.md) Section 18. No separate formal acceptance decision or later Git publication is recorded.
- **M4 (counting tasks): implemented and deployed; owner-run automated, SQL compatibility and representative live results are recorded in Section 10.8.** Remaining live checks were waived, not passed.
- **M5-A (daily counting): owner-authorized direct Codex implementation on 2026-10-16; implemented and passed recorded owner-run backend/web automated checks.** Test 0009 is applied/frozen; repeat-InitRoles/privilege/SQL compatibility checks passed on usapp_test on 2026-10-21; main 0009 was applied and the runtime/web deployed on 2026-10-22; remaining live checks were waived, not passed. See [phase1-m5-daily.md](phase1-m5-daily.md), Verification.
- **M5-C (custom date-range counting, D5 inclusive Beijing dates): owner-authorized 2026-10-25; implemented by Claude Code; 0011 + repair 0012 applied on usapp_test and main; automated checks passed; main API/web rollout completed 2026-10-25; live workflows unverified.** See [phase1-m5-custom.md](phase1-m5-custom.md).
- **M6 (task edit/cancel, D3 resolved by the owner's approved rules 2026-10-25): implemented by Claude Code; 0013 applied on usapp_test and main; automated checks passed; main API/web rollout completed 2026-10-25; live workflows unverified.** See [phase1-m6-design.md](phase1-m6-design.md).
- **M7 (reward-shop products, D13/D14; price 1-999 flowers): implemented/test-verified by Claude Code; 0014 frozen on test/main; joint M7/M8 main API/web rollout completed by Codex on owner authorization 2026-10-26.** See [phase1-m8-redemption.md](phase1-m8-redemption.md), Section 7, for release evidence and live-check limits.
- **M8 (direct redemption): implemented/test-verified by Claude Code; 0015 frozen on test/main; main API/web deployed by Codex 2026-10-26. Live product/redeem/fulfill and browser/PWA acceptance unverified.** See [phase1-m8-redemption.md](phase1-m8-redemption.md), Section 7.
- **M9 (request redemption and mutual cancellation/refund): owner-assigned direct Codex implementation 2026-10-26; implemented and automated-test verified on usapp_test; 0016 applied/frozen on test and main; main API/web rollout 2026-10-26 (Section 5 of the M9 document); live/browser acceptance unverified. M10 not started.** See [phase1-m9-redemption.md](phase1-m9-redemption.md) for evidence and release limits.
- **M10 (home and navigation, D15/D16/C1/C2): owner-authorized 2026-10-26; implemented by Claude Code in the web app only; web unit tests and typecheck passed; frontend web published 2026-10-26 (Section 8 of the M10 document); browser/device acceptance pending; Phase 1 not formally accepted.** See [phase1-m10-home.md](phase1-m10-home.md).
- **M5-B (weekly counting, D4 Monday-to-Monday weeks in Asia/Shanghai): implemented; Codex-run automated/SQL checks passed and main API/web rollout completed 2026-10-25. Frozen 0010 is applied on test/main; live weekly workflows remain unverified.** See [phase1-m5-weekly.md](phase1-m5-weekly.md), Verification results and Main rollout. Custom periods and M6-M10 remain outside this approval.
- Claude Code has not built, tested, or verified anything in this document; verification results are attributed to the owner or Codex as recorded for each milestone.

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

M3-specific refinements of D5 and D20, and new proposals D5c and D22-D35 (reward ranges, zero rewards, title/description limits, completion authority and repeat behaviour, publish idempotency, task grants, cap rejection, paginated listing, test-database reset, completion lock order, completion response content, partial-failure test seam, cap fixture, completion-to-settlement integrity, unknown commit outcomes), are in [docs/phase1-m3-design.md](phase1-m3-design.md) Section 2.2; the owner's 2026-10-09 confirmations are recorded in its Section 2.3, and the rest stay proposals. D30 is a proposed M3 refinement of Section 9.8's lock order (no task-row lock; the business-source claim serializes completions); Section 9.8 stays binding unless the owner approves D30.

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
| M4 | Counting completion | "+1" progress with 0/N display; reward on reaching N | Progress endpoint, contract 0.6.0 | 0008: mode/target and append-only progress | M3 |
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

Historical design status (revised 2026-10-06; later approvals and verification are in Sections 9.13-9.19):

- **Approved design directions (owner, 2026-10-06):** D1, D2 (balance and per-entry bounds only), D18, D19, D21; see Section 9.9.
- **At that design revision:** M2 implementation, migrations, grant changes, `init-roles.sql` changes, contract changes, and database execution were not approved. The owner subsequently assigned M2-A and M2-B; their implementation and verification supersede this historical status.
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

At the design stage, these directions alone did not approve implementation. The owner subsequently assigned M2-A and M2-B (Sections 9.13-9.19); any further migration, grant, contract or code change requires its own assigned scope. M3-M10 remain unapproved.

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

### 9.13 M2-A implementation status (2026-10-07): implemented, verification pending

Approved by the owner on 2026-10-07: database wallet/ledger foundation, reusable privilege checking, tests, and documentation. Excluded and not implemented: `WalletService`, `WalletController`, contract/version changes, frontend changes, API startup integration, task posting, redemption, M2-B, M3+. **Nothing below has been built, tested, or executed against a database by Claude Code.** M2-A is not accepted until the owner completes the steps at the end of this section.

Files:

- New `database/migrations/0006_wallet_ledger.sql`: role preflight; `couple_members (couple_id, user_id)` unique constraint; the three tables of Section 9.3 (accounts hold no balance or sequence); approved D2 bounds as CHECKs; membership/couple composite FKs; source and partial idempotency unique indexes; triggers; narrowed grants. Writes no wallet rows; no existing migration edited.
- `database/init-roles.sql`: new section 7, after the ALL TABLES grant, re-applies the wallet privilege matrix for each wallet table that exists (re-runnable; no-op before 0006).
- New `database/tests/wallet_ledger_test.sql`: owner-run psql script (Section 9.10 item 2).
- New `services/api/src/db/WalletPrivilegePolicy.h/.cc`: the Section 9.6 effective privilege matrix (identity, superuser, ownership, role memberships, table and any-column privileges, catalog-enumerated column UPDATE, `usapp_push`, `public`). Expected values live only in C++; output is names and booleans only. **Not wired into API startup** (M2-B).
- Tests: new `services/api/tests/WalletLedgerProbe.h` and `WalletLedgerDbTests.h`, called as block 11 of `DbGatedTest.cc`; the `db_gated` migration gate now also requires `0006_wallet_ledger`; `UnitTest.cc` policy case; `StaticCheckTest.cc` 0006/init-roles case.
- `database/README.md`: 0006 row, wallet notes, psql script instructions.

Implementation details that refine Section 9 (review these):

- Append-only triggers are **statement-level** `BEFORE UPDATE OR DELETE OR TRUNCATE` on all three tables, so even 0-row UPDATE/DELETE statements are rejected. `SELECT ... FOR UPDATE` and `INSERT ... ON CONFLICT DO NOTHING` do not fire them.
- Triggers raise custom SQLSTATEs: `UW001` append-only, `UW002` account missing, `UW003` sequence, `UW004` balance arithmetic, `UW005` multi-row insert, `UW006` operation without entries at commit or `SET CONSTRAINTS ... IMMEDIATE`.
- The chain trigger skips its checks when a key column is NULL so that the NOT NULL constraint reports 23502 instead of a chain error.
- Drogon v1.9.11 (checked in source): `PgConnection` reports query failures as `Failure(PQerrorMessage(...))` with **no SQLSTATE**, and the server message text may be localized. The `db_gated` wallet cases therefore wrap each expected-failure statement in a `DO` block that catches the error and re-raises `USAPP_TEST_SQLSTATE=<code>;CONSTRAINT=<name>;`. The DO block still ends in an error, so Drogon still rolls back the whole transaction; this only exposes the SQLSTATE and is not savepoint recovery. Statements inside the DO block contain only literals (test-generated, shape-checked UUIDs and integers).
- Each case uses its own transaction acquired with `newTransactionAsync` under a bounded wait (late arrivals are rolled back). Every exec, commit, and teardown wait is bounded. A separate single-connection `DbClient` (outside the working pool) is the observer; no transaction is observed on its own connection.
- Teardown evidence: the case records `pg_backend_pid()`; the observer polls `pg_stat_activity` / `pg_locks` until the backend is idle with no xid and no relation/tuple/transactionid locks, and checks that no fixture or wallet row is visible as committed. This shows that the server-side transaction ended and released its locks; it does not show Drogon callback timing or pool return.
- The only real commit is the "operation without entries" case; the commit must fail. If it ever succeeded, the case reports the committed rows that would remain in `usapp_test`.
- Sunshine balance cap (10,000 entries) is tested only in the psql script, to stay within the 30 s `db_gated` CTest timeout; the flower balance cap (1,002 entries in one DO statement, each INSERT still a single-row statement) is tested in both.
- Maintenance-role TRUNCATE in the psql script truncates all three tables together (and the ledger alone), because truncating accounts or operations alone fails first on foreign-key references and would not prove the trigger.
- Concurrency: only account-creation contention with committed non-wallet fixtures and both wallet transactions rolled back. Existing-account lock serialization and post-commit snapshot visibility were **not** tested; they remain M3 requirements.
- Review repair (2026-10-07, before any owner-run database check): target-database guards.
  - `DbGatedTest.cc` queries `current_database()` right after the connectivity probe, before the migration gate, every fixture write, and every test case (existing and new). It requires exactly `usapp_test` and exits with failure (1, not Skipped) on a mismatch or query failure. It prints only a sanitized database name and the reason, never the connection string. It does not rely on DSN substrings; the older `prod` substring check remains as an extra.
  - `wallet_ledger_test.sql` raises an exception if `current_database()` is not `usapp_test`, as the first statement inside its DO block, before any fixture DML. `ON_ERROR_STOP` and the BEGIN ... ROLLBACK structure are unchanged.
  - Coverage: the decision is a pure function in new `services/api/tests/TestDatabaseGuard.h` with a `unit` case (exact match only; near-misses such as `usapp`, `usapp_test2`, `USAPP_TEST`, padded or embedded-NUL names rejected; safe output). A `static_check` case asserts that both guards precede every migration query, test case, and write, that the guard block returns 1 without skip, and that the script never commits and ends with `ROLLBACK;`. The rejection path was not exercised against any real database.
  - The DO-based SQLSTATE extraction, whole-transaction rollback, and the independent observer connection are unchanged. Not built or tested.

Unverified assumptions (all to be confirmed by the owner-run steps):

- PostgreSQL 17 accepts every statement in 0006 (custom SQLSTATEs, transition-table statement trigger, combined statement-level `UPDATE OR DELETE OR TRUNCATE` trigger, `has_*_privilege` with `'public'` and `MAINTAIN`).
- `apply.ps1`'s statement scanner accepts the new migration (dollar-quoted bodies with distinct tags, no bare `$`).
- `SET CONSTRAINTS ALL IMMEDIATE` inside a PL/pgSQL `DO` block fires deferred triggers queued earlier in the same transaction.
- `INSERT ... ON CONFLICT DO NOTHING` waits on another transaction's uncommitted insert of the same key, and the waiting backend shows `wait_event_type = 'Lock'`.
- A non-superuser can read `state`, `backend_xid`, and `wait_event_type` in `pg_stat_activity` for sessions of its own role, and `pg_locks`.
- TRUNCATE privilege and FK checks run before BEFORE TRUNCATE triggers (affects which code the psql script expects).
- `db_gated` really connects as `usapp_api` (the policy case fails otherwise; the actual `USAPP_TEST_DSN` role is not recorded).
- The new code compiles (`WalletLedgerProbe.h`, `WalletLedgerDbTests.h`, `WalletPrivilegePolicy.cc`) and the `db_gated` run stays within its 30 s timeout.

Owner-run acceptance steps (one at a time, guided by Codex):

1. Codex reviews the migration, the `init-roles.sql` diff, the psql script, the policy module, and the tests.
2. Back up the main database with the existing procedure (before any later apply to `usapp`).
3. As the maintenance role: `usapp_api` and `usapp_push` exist and are not superusers.
4. `apply.ps1 -DryRun` against `usapp_test`: only `0006_wallet_ledger` is pending, and the transaction-boundary precheck passes.
5. Apply to `usapp_test` with `-InitRoles`.
6. As the maintenance role on `usapp_test`, check privileges for `usapp_api`: `has_table_privilege` is true only for SELECT and INSERT on each wallet table; `has_column_privilege('usapp_api', 'wallet_accounts', 'lock_token', 'UPDATE')` is true; UPDATE on every other wallet column, and every privilege for `usapp_push` and `public`, is false.
7. Run `apply.ps1 -InitRoles` again (no pending migrations) and repeat step 6: unchanged.
8. Run `database/tests/wallet_ledger_test.sql` on `usapp_test` twice, as `usapp_api` and as the maintenance role; each prints `ALL WALLET LEDGER ASSERTIONS PASSED` with the expected mode and exits 0.
9. Build the backend test image and run all CTest targets: `unit`, `db_gated` (not skipped), and `static_check` pass; the `db_gated` log shows the target-database guard passing for `usapp_test` and the privilege summary with `role=usapp_api ok=true`.
10. On `usapp_test`, the three wallet tables contain zero rows.
11. Only when the owner decides (not required until M2-B): apply 0006 to the main database with `-InitRoles` after the backup, repeat step 6 there, and confirm the wallet tables are empty and the existing app still works (login, home, nickname).

### 9.14 M2-A owner-run verification results (2026-10-07)

Run by the owner under Codex's guidance against `usapp_test`. These are M2-A results only; Phase 0 and M1 evidence are recorded separately.

Migration and roles:

- `apply.ps1 -DryRun`: the six existing migration checksums matched; only `0006_wallet_ledger` was pending; exit code 0.
- `usapp_api` and `usapp_push` existed and were not superusers.
- 0006 applied to `usapp_test` and `init-roles.sql` completed; exit code 0. Applied checksum of `0006_wallet_ledger`: `2633e17ea6ff6e664832df84023d1fab8aa9513b4cfe48786c4ca093bc7d543d`. **This migration is applied and must never be edited.**
- Repeated `init-roles` run: exit code 0.

Automated tests:

- Backend test image build: exit code 0.
- CTest `unit`, `db_gated`, `static_check` all Passed; `db_gated` ran and was not skipped; timings 0.01 s, 0.88 s, 0.00 s, total 0.90 s; exit code 0. Per-assertion counts were not recorded.
- A later database-only CTest run (after the repeated `init-roles`): one test, no failures, total 1.07 s, exit code 0.
- The full CTest run came before the script-only repairs below; those repairs changed only `database/tests/wallet_ledger_test.sql`, which CTest does not execute.

psql script `database/tests/wallet_ledger_test.sql`, with failed runs and retries:

- Application role (`usapp_api`), first run: failed with exit code 3, "syntax error at end of input" at the internal `THEN` of an unparenthesized `CASE` in an `IF` condition.
- Codex, under the owner's authorization, repaired the standalone script directly: the seven `IF SQLSTATE <> CASE ... END THEN` conditions in E2-E8 were parenthesized. In the maintenance branch, E9 now runs `SET CONSTRAINTS ALL IMMEDIATE` before testing `TRUNCATE`.
- The first maintenance-role run failed with exit code 3: E9 expected UW001 but received 55006 (pending trigger events prevented TRUNCATE before the append-only trigger could run). After the E9 repair, the retry passed as recorded below.
- Application-role retry after the repair: exit code 0. Its success banner and group count were not supplied and are not recorded.
- Maintenance-role retry after the repair: 28 assertion groups passed, ended with `ROLLBACK`, exit code 0.

Final state of `usapp_test` (owner query, exit code 0): `wallet_accounts` 0, `wallet_operations` 0, `wallet_ledger_entries` 0, wallet test users 0.

Limits of this evidence:

- 0006 has not been applied to the main database.
- No API startup integration, wallet endpoint, or frontend exists yet; no service posting path exists.
- Existing-account lock serialization and post-commit snapshot visibility were not tested (M3 requirements).
- The privilege matrix was verified on `usapp_test` only.
- These are targeted checks, not exhaustive security verification.

### 9.15 M2-B scope and rollout: wallet read endpoint, startup privilege check, home display

Status: originally a design proposal, written 2026-10-07. The owner subsequently authorized direct Codex implementation of M2-B; implementation is recorded in Section 9.16 and owner-run verification in Section 9.19. The scope follows D18, D19, D21 and the frozen M2-A schema. M2-B added no migration, permission, dependency, or deployment configuration change; the owner applied the existing frozen M2-A migration during rollout.

Scope: one read endpoint, the shared privilege policy enforced at API startup, and own-balance display on the home page. No wallet writes, posting, redemption, admin endpoints, or partner balances.

API (`GET /api/v1/wallet`):

- Behind `AuthFilter`; route registered for GET only. The caller is taken only from the server session (`ctrl::requireUser`); there is no query or body input.
- Pre-authentication or anonymous session: 401 `UNAUTHENTICATED`. Authenticated user without a space: 404 `SPACE_NOT_FOUND` (same as Phase 0 space reads). Success: 200 `{ "wallet": { "flower": <int>, "sunshine": <int> } }`. Every response is `Cache-Control: no-store` through the existing response helpers.
- One SQL statement reads membership and both balances, so all values come from one snapshot:

  ```sql
  SELECT m.couple_id::text AS couple_id,
         COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e
                    WHERE e.user_id = m.user_id AND e.asset_type = 'flower'
                    ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS flower,
         COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e
                    WHERE e.user_id = m.user_id AND e.asset_type = 'sunshine'
                    ORDER BY e.seq DESC LIMIT 1), 0)::bigint AS sunshine
    FROM couple_members m
   WHERE m.user_id = $1::uuid
  ```

  No row means no space (404). Absent ledger entries mean 0. The statement takes no locks and creates no rows. Values are read as `int64_t` and written as JSON integers (the D2 caps keep them below 2^53).
- No wallet writes and no audit rows on reads.
- Contract: `contracts/openapi.yaml` `info.version` 0.3.0 (additive). New operation `getWallet` and schema `WalletResponse` (`flower`, `sunshine`: integer, minimum 0, maximum per D2 caps). Responses 200 / 401 / 404. No existing operation or schema changes.

Startup privilege check (`services/api/src/main.cc`):

- New step between "2) 数据库" (`waitForDatabase`) and "3) Drogon 运行参数". It runs `WalletPrivilegePolicy::check` on the pool client with a bounded wait (for example 10 s). It completes before filters, listeners, the scheduler (`startScheduler`), and `drogon::app().run()`, so the process neither serves requests nor starts scheduler jobs with the wrong identity or privileges.
- On a mismatch, missing wallet tables, query error, or timeout: log `report.summary()` (names and booleans only), log a short reason, and exit with status 1. Never log the connection string. On success: log one line with `role=usapp_api ok=true`.
- A small pure function decides start/refuse from a report so it can be unit-tested. The policy module itself is unchanged.
- Consequence: an M2-B runtime refuses to start on a database without 0006. Hence the rollout order below.

Frontend (`apps/web`):

- `src/api/types.ts`: `Wallet`, `WalletResponse`. `src/api/wallet.ts`: `getWallet()` via the shared client (`GET`, so no CSRF header; offline GET is still attempted and fails normally; nothing is cached because the Service Worker never caches `/api/`).
- New `src/stores/wallet.ts` (memory only): `balances: { flower, sunshine } | null`, `status: 'idle' | 'loading' | 'ready' | 'error'`, `errorCode`, and a private `generation` counter.
  - `load()` increments and captures the generation before the request and discards successes and failures if the generation changed when they return. Thus newer loads also invalidate older loads.
  - `reset()` clears balances and error and increments the generation.
  - On error, balances are cleared, so stale numbers are never shown next to an error.
- `src/stores/session.ts`: `resetBusinessState()` also calls `useWalletStore().reset()`. `applyBootstrap()` resets the wallet store whenever the snapshot is anonymous or the user id changes. The bootstrap error handler uses the common reset path; refreshMe and nickname snapshots also reset on user-id change. A wallet response started before such a reset is discarded.
- `src/views/HomeView.vue`: a "我的资产" card. It shows "加载中…" while loading; the two own balances (小红花, 阳光值) and a refresh button when ready; a Chinese error with a retry button on error, without numbers. A watcher loads for the authenticated user on mount and user-id changes; refresh/retry reissues the read. Partner balances are never requested or shown.

Expected files: `contracts/openapi.yaml`; new `services/api/src/services/WalletService.h/.cc` (read only; exposes its SQL text as a constant for tests); new `services/api/src/db/WalletStartupCheck.h/.cc` (bounded startup wrapper and pure decision); new `services/api/src/controllers/WalletController.cc`; `services/api/CMakeLists.txt` (add the controller to the explicit `add_executable(usapp_api ...)` list); `services/api/src/main.cc` (startup check); `services/api/tests/UnitTest.cc`, `DbGatedTest.cc` and new `WalletReadDbTests.h`, `StaticCheckTest.cc`; `apps/web/src/api/types.ts`, new `apps/web/src/api/wallet.ts`, new `apps/web/src/stores/wallet.ts`, `apps/web/src/stores/session.ts`, `apps/web/src/views/HomeView.vue`, new or extended specs under `apps/web/tests/unit/`; docs. No migration, `init-roles.sql`, grant, dependency, or deployment change.

Tests:

- `unit`: the startup decision function (ok report -> start; any mismatch, missing, unexpected, or error -> refuse); wallet JSON shaping (int64 values, both keys present).
- `db_gated` (guarded to `usapp_test`, as `usapp_api`):
  - Real `WalletService` read for a member with no entries -> 0/0, and for a user without a space -> `SpaceNotFound`. These use committed non-wallet fixtures, deleted by id afterwards.
  - The balance SQL constant executed inside a rollback-only transaction with in-transaction fixtures and ledger entries. It checks: the latest entry wins over earlier ones; both assets are returned independently; a partner's entries are not returned for the caller; a user without membership returns no row. These cases use the M2-A independent-transaction pattern and leave no committed wallet rows (checked by the observer).
  - The existing privilege-matrix case remains.
- `static_check`: the contract contains `getWallet`, `WalletResponse`, and version 0.3.0. `WalletController.cc` registers `/api/v1/wallet` with `drogon::Get` and `AuthFilter` only. `CMakeLists.txt` lists the controller. In `main.cc`, the privilege check comes before `startScheduler()` and `drogon::app().run()`.
- Web unit tests:
  - `getWallet` issues a GET through the shared client without a CSRF header.
  - The store sets `ready` with balances on success; on error it clears balances and sets the error.
  - `reset()` clears state. A response that resolves after `reset()` (deferred promise) does not restore balances. A response for user A that resolves after switching to user B is discarded.
  - Session logout resets the wallet store.

Rollout order (owner-run, one step at a time):

1. Codex reviews the M2-B diff.
2. On the test side first: build the backend test image and run all CTest targets against `usapp_test` (0006 already applied there); run web unit tests. Build the web only when ready to deploy: apps/web/dist is bind-mounted into Caddy, so a web build changes the served files. A newer wallet UI against the older API would show a load error.
3. Take a **fresh** main-database backup (new, after M2-A verification) with the existing procedure.
4. `apply.ps1 -DryRun` against the main database: the six existing checksums match and only `0006_wallet_ledger` is pending.
5. The owner applies 0006 to the main database with `-InitRoles`. As the maintenance role, check the privilege matrix for `usapp_api` there and confirm the three wallet tables are empty. The current (pre-M2-B) runtime does not use the wallet tables, so login, home, and nickname must still work.
6. Before building the new runtime, record the running api image ID and give it a unique local rollback tag; verify that the tag resolves to the recorded ID. Preserve that tag. Only then build the runtime image and update the `api` container. The startup log shows the privilege check passing with `role=usapp_api ok=true`; the container is Healthy. If startup is refused, retag the preserved old image to the Compose api image name and recreate api (0006 does not affect it) and report the logged summary.
7. After backend startup passes, run the web build (including type checks) and refresh through the Service Worker update flow.
8. Browser, both accounts: the home page shows 0 flowers and 0 sunshine and keeps showing them after refresh. Each account sees only its own balances. Log out and log in as the other account: no previous numbers appear at any point. While still on the authenticated home page, switch offline and click the wallet card's refresh button: it shows an error without numbers. Returning online does not replay a write; retry the read explicitly. A network toggle alone does not reload. A full page refresh offline may show the login page because API/session data is not persisted; that existing behavior is preserved.
9. API: `GET /api/v1/wallet` without a cookie -> 401; with a pre-authentication session -> 401; authenticated -> 200 `{ "wallet": { "flower": 0, "sunshine": 0 } }` with `Cache-Control: no-store`; POST/PATCH/DELETE on `/api/v1/wallet` are rejected (405 or 404, whichever Drogon v1.9.11 returns for an unregistered method; record the observed code) and never change data.
10. Main database: the wallet tables still contain no rows; no wallet audit events exist.

Out of scope for M2-B and still open: posting and settlement (M3), existing-account serialization and post-commit snapshot tests (M3), admin wallet views (D19: none in M2), partner balances (D21: none).

### 9.16 M2-B implementation by Codex (2026-10-07; verification pending)

Authorization: the owner assigned M2-B directly to Codex instead of Claude Code.
This covers the additive GET endpoint, OpenAPI 0.3.0, startup enforcement, own-wallet
home card, regression tests, and documentation. It does not authorize M3+, wallet
posting, changes to 0006 or grants, dependency changes, or deployment configuration
changes. The owner continues to execute builds/tests/database/container/Git actions.

Implemented:

- WalletService.h/.cc: a single membership/latest-ledger query; own balances only,
  zero for missing entries, SpaceNotFound without membership, generic Error on a
  query failure. Json::Int64 envelope. No transaction, account creation, ledger
  write, or audit event. WalletController.cc: GET + AuthFilter + requireUser;
  200/401/404/500 through the existing no-store response helpers. Controller added
  to the explicit CMake executable list; service discovered by the existing GLOB.
- WalletStartupCheck.h/.cc: shared WalletPrivilegePolicy query, 10-second bounded
  wait, heap-owned promise callbacks, late replies safe; missing/error/timeout or
  failed report refuses startup. Logs policy summary or a fixed error reason,
  never exception text. main.cc invokes it after DB readiness and before filter
  registration, listener registration, scheduler start and app().run().
- Web: wallet types/API wrapper/store and HomeView card; idle/loading/ready/error
  states, refresh/error retry, memory only, no CSRF header on GET. Each load/reset advances
  a generation; older successes AND failures are ignored. Session reset, anonymous
  or changed-user bootstrap, failed bootstrap, and changed-user me snapshots clear
  the wallet. Home loads when the authenticated user appears/changes, and on refresh/retry.
- Tests written: unit startup decisions and integer JSON; db_gated block 12 real
  service reads 0/0, no-space and query-error outcomes without wallet writes, plus
  actual startup wrapper. Rollback-only production-SQL checks cover latest seq,
  both assets, partner/other-space exclusion and no membership. Non-wallet committed
  fixtures are deleted by UUID; rollback-only cases use the independent observer.
  Static checks cover the new contract/route/CMake/startup ordering inside main.
  Web wallet-client, wallet-store, wallet-session and home-wallet specs cover GET,
  missing CSRF, offline failure, rendering/retry, concurrency and session boundaries.

Files changed for M2-B (excluding earlier M2-A work):

- contracts/openapi.yaml
- services/api/CMakeLists.txt; services/api/src/main.cc
- services/api/src/services/WalletService.h and WalletService.cc (new)
- services/api/src/controllers/WalletController.cc (new)
- services/api/src/db/WalletStartupCheck.h and WalletStartupCheck.cc (new)
- services/api/tests/UnitTest.cc, StaticCheckTest.cc, DbGatedTest.cc
- services/api/tests/WalletReadDbTests.h (new)
- apps/web/src/api/types.ts; apps/web/src/api/wallet.ts (new)
- apps/web/src/stores/session.ts; apps/web/src/stores/wallet.ts (new)
- apps/web/src/views/HomeView.vue
- apps/web/tests/unit/wallet-client.spec.ts, wallet-store.spec.ts,
  wallet-session.spec.ts, home-wallet.spec.ts (new)
- docs/phase1-plan.md; docs/claude-code-handoff.md; CLAUDE.md

Status: files modified and source reviewed only. VERIFICATION HAS NOT BEEN RUN.
No build, type check, test, database, container, install or Git command was run.
No real .env or backup content was read. No applied migration was modified.
M2-B has not been accepted. Section 9.15 is the owner-run rollout sequence.

Unverified: compilation and new routing; actual startup success/refusal/timeout;
added DB tests and their combined 30-second budget; web type checks, unit/component
specs and visual layout. Live reads with nonzero committed balances are deferred
until posting has its own approved fixture strategy. SQL tests read nonzero values
only inside rollback-only transactions; they do not prove existing-account lock
serialization or post-commit snapshot visibility. The main database still needs
0006 before the new runtime can start. A wallet read may update the session's
last_seen_at through the existing AuthFilter; it performs no wallet/audit writes.

### 9.17 M2-B first owner-run backend test and audit-query repair (2026-10-07)

- Test image build: owner reported exit 0.
- First full CTest: unit Passed (0.02 s), db_gated Failed (1.13 s), static_check
  Passed (0.00 s), total 1.15 s, exit 8. The target-database guard passed for
  usapp_test. Database output reported 591 assertions and 2 failures.
- Both failed assertions originated from the new WalletReadDbTests.h audit
  queries: audit_logs has actor_type and actor_id (0005), not actor_user_id.
  Codex corrected both queries to actor_type='user' AND actor_id IN (...).
  The no-audit assertions remain; neither assertion was removed or weakened.
- Repair scope: test header and status documentation only. Wallet service,
  startup enforcement, migration 0006, permissions and frontend unchanged.
- The failure remains part of the record. Codex did not execute tests or database
  commands. The owner subsequently rebuilt and reran full CTest; see Section 9.18.
  Frontend checks, main migration, runtime rollout and live acceptance remain
  pending. The initial not-run status in Section 9.16 describes delivery time.

### 9.18 M2-B backend rerun and Vue test-configuration repair (2026-10-07)

- Owner rebuilt the repaired test image, exit 0. Full CTest rerun: unit Passed
  (0.01 s), db_gated Passed (1.05 s, not skipped), static_check Passed (0.00 s),
  total 1.06 s, zero failed targets, exit 0. Per-assertion counts were not supplied
  for this passing run. This supersedes the pending backend rerun in Section 9.17.
- First web unit-test run: seven files passed, 52 tests passed; one suite failed
  to load (home-wallet.spec.ts), duration 16.09 s, exit 1. HomeView.vue was sent
  to JavaScript import analysis without Vue transformation; its component tests
  therefore did not execute. This does not establish a HomeView syntax defect.
- Source-confirmed cause: the standalone vitest.config.ts did not register the
  Vue plugin. @vitejs/plugin-vue already exists in package.json and the regular
  Vite build uses it. Codex added its import and plugins: [vue()] to the standalone
  test config. No dependency or lockfile changed; PWA plugin remains excluded.
- Repair statically reviewed only. Codex did not run any test, build, database,
  container or Git command. Web rerun, type checks/build, main migration, startup
  and live acceptance remain pending; M2-B is not accepted.
- Add apps/web/vitest.config.ts to the Section 9.16 M2-B changed-file list.

### 9.19 M2-B owner-run verification and rollout (updated 2026-10-09)

The owner performed the commands and browser checks below under Codex guidance.
These results supersede delivery-time pending statements in Sections 9.16-9.18.
Owner-run verification passed within the limits below. A separate formal M2
acceptance decision has not been recorded; Git review/commit/push is pending.
Neither Codex nor Claude Code ran build/test/database/container/Git commands.

Automated checks and rollout (2026-10-07):

- Repaired backend test image rebuilt, exit 0. All three CTest targets Passed:
  unit 0.01 s, db_gated 1.05 s (not skipped), static_check 0.00 s; total 1.06 s,
  exit 0. Passing-run per-assertion counts were not supplied. Earlier failed runs
  and both repairs remain recorded in Sections 9.17-9.18.
- Web unit-test rerun exit 0. Successful-rerun file/test counts were not supplied;
  the earlier seven files/52 tests are not used as counts for the successful run.
  Separate web type check exit 0. Web production build, including type checks and
  service worker generation, exit 0; 15 precache entries, 140.91 KiB.
- Fresh pre-migration main backup: pg_dump custom format exit 0; host export exit 0,
  backups/usapp_before_m2b_0006_20261007_175748.dump, 104193 bytes. This particular
  backup was not restore-tested; Phase 0 restoration is separate evidence.
- Main DryRun exit 0: six existing checksums matched, only 0006 pending. Main
  apply plus -InitRoles exit 0. Frozen 0006 checksum:
  2633e17ea6ff6e664832df84023d1fab8aa9513b4cfe48786c4ca093bc7d543d.
  Do not edit the applied migration.
- Main counts after migration: users/spaces/members 2/1/2; all three wallet tables
  empty, query exit 0. Both roles non-superuser with zero memberships; API SELECT
  and INSERT on the three tables, UPDATE only on accounts.lock_token, forbidden
  table privileges false, push/public privileges false; query exit 0. The owner
  also reported the old runtime compatibility check successful.
- Preserved old runtime image:
  sha256:f576443a538d44fdccff1c462f9e07d89360aba38b5c868bb8300b476e1ddf37,
  rollback tag usapp-api:before-m2b-20261007_180639. Owner checked matching IDs.
  The rollback itself was not exercised.
- New runtime build and api update exit 0; api Healthy. Startup log at
  2026-10-07 10:08:01.861009 UTC: role=usapp_api ok=true checks=187 mismatches=0
  missing=0 unexpected=0. The complete shared policy passed on the main database.
  Web was built after backend startup and served via the existing dist bind mount.

Owner browser/API checks (completed by 2026-10-09):

- Screenshot: own-wallet card displayed flower 0 and sunshine 0; membership 2/2.
  Owner reported offline card refresh showed an error without numbers and online
  retry restored 0/0. Owner reported logout/login to the other member, correct
  account identity, 2/2 membership and wallet refresh successful.
- Authenticated GET /api/v1/wallet: 200, wallet {flower: 0, sunshine: 0},
  Cache-Control: no-store, no-cache, must-revalidate, private. This was a plain
  browser GET without an X-CSRF-Token header.
- No-cookie curl: 401 UNAUTHENTICATED, no wallet data.
- Private-window bootstrap: session 200, authenticated=false, nonempty pre-auth
  token (only its presence printed); subsequent wallet GET 401 UNAUTHENTICATED.
- Authenticated POST/PATCH/DELETE with the current session-bound CSRF header:
  all returned 405 Method Not Allowed. Browser console error lines were expected.
- Final main counts: database usapp, users/spaces/members 2/1/2, wallet accounts/
  operations/entries 0/0/0; query exit 0. Audit action prefix wallet.% count 0,
  query exit 0. This audit query checks that action prefix only.

Restart incident (2026-10-09): the login page reported an expired security token,
but GET /api/v1/session failed at the network layer. Compose could not connect to
dockerDesktopLinuxEngine because Docker Desktop had not been opened. After the
owner started Docker Desktop, api/postgres were Healthy and Caddy was running;
the owner subsequently obtained an authenticated wallet 200 response. No cache
clear or code change was needed. The generic CSRF error text can also mask a
bootstrap network failure; clearer wording is a possible future fix, not included
in this milestone. This observation does not explain the older initial blank screen.

Remaining limits:

- Targeted local checks, not production readiness or exhaustive security review.
- Both live balances were zero; manual account switching alone cannot prove
  isolation of different balances. Automated rollback-only SQL tests cover nonzero
  latest entries, both assets and caller/partner/other-space separation.
- No separately reported full-page balance-persistence check; API calls and card
  refresh/account switching were observed or owner-reported as described above.
- No live authenticated no-space 404 check; service-level automated coverage only.
  No live startup refusal/timeout or rollback-image exercise; successful startup
  and automated decision/policy checks are recorded, not failure-path deployment.
- No committed nonzero-wallet/posting path, existing-account serialization or
  post-commit visibility check; these remain M3 requirements. HEAD/OPTIONS behavior
  was not separately checked. A read can touch session last_seen_at via AuthFilter.
- Fresh backup not restore-tested; no offsite copy claim. No current Git state or
  commit/push is inferred from runtime checks. M3-M10 remain unapproved.

### 9.20 M2 Git publication (owner-reported, 2026-10-09; dated observation)

- The owner committed and pushed M2-A and M2-B as
  `594a77bcadb0d2742c0704f93aea20cb09a1e726` with the message
  `feat: add wallet ledger foundation and own balance display`.
- Local `HEAD` and remote `main` matched; tracked files had no outstanding changes.
- `.claude/`, `.workbuddy/`, `Claude outputs/` and the original V1 design document
  remained untracked.
- A limited credential-pattern check over the staged files returned no matches
  (exit code 1, meaning no match). As before, this is not a full-history secret scan.
- This is a dated observation, not a claim about later Git state; Claude Code and
  Codex ran no Git commands. A commit or push is not an acceptance decision: M2
  passed the recorded owner-run verification, and no separate formal M2 acceptance
  decision has been recorded.
- This supersedes the "Git review/commit/push pending" statements in Sections 9.19
  and the status header, which describe the state before 2026-10-09.

## 10. M4 Counting Tasks: Implementation (2026-10-14; later evidence in Section 10.8)

### 10.1 Authorization and scope

The owner requested the next feature immediately after reporting the M3-C live
checks successful, then explicitly assigned this implementation to Codex. M4
covers one-off counting tasks, the additive schema/grants needed for them, service,
contract, frontend and focused regression tests. This assignment does not invent
a separate M3-C acceptance, final database check or Git publication. M5-M10,
task editing, undo, recurring periods, database reset, dependency/deployment
changes and a timeout increase remain outside scope. D33 stays deferred.

The publisher selects single completion or counting, with a positive target N
(PostgreSQL integer storage range 1-2147483647; no additional product cap).
The recipient sees 0/N and presses +1. Below N, only progress is recorded.
At N, the full reward is credited once. Extra new requests return an ignored
success without a new event, audit or reward. Deadline rules are unchanged.

### 10.2 Schema, locking and settlement

- `0008_task_counting.sql` adds immutable mode/target columns; existing tasks
  default to single/1. It creates append-only `task_progress_events` with unique
  `(task_id, progress_count)` and `(user_id, idempotency_key)` constraints.
- Before-insert guards require the same couple, assignee, target and next
  contiguous count, with a valid decision timestamp. Final events link to the
  correct task-sourced wallet operation. Completion insertion requires that
  final event; deferred checks require the matching completion in the same
  transaction. Existing wallet/settlement constraints remain intact.
- Membership share lock -> reserved per-task advisory transaction lock
  `(33004, hashtext(canonical task UUID))` -> key classification -> current count.
  Hash collisions only serialize unrelated tasks; scheduler bigint keys are separate.
- Non-final actions insert an event and `task.progressed` audit, then commit.
  Final actions claim the operation, lock positive assets in flower/sunshine order,
  read balances and check caps, then insert the final event at one clock decision,
  insert completion using that timestamp, entries, `task.completed` and
  `task.progressed` audits. Confirmed commit alone reports new progress/settlement.
- New advisory serialization refines D30 for count tasks; single tasks retain
  their existing operation-claim serialization. No task-row UPDATE grant is added.
- Progress keys belong to the actor/action domain, independently of publication
  and single completion. Final wallet operations use NULL wallet key/fingerprint;
  their business-source uniqueness and the progress event protect settlement.
  Same-task event replay is immutable even after later progress or expiry;
  an existing key for another counting task returns 409. Concurrent conflicts
  use `ON CONFLICT` followed by reads, without SQLSTATE classification.
  Ignored excess requests do not reserve their unused keys.
- Progress SELECT/INSERT grants are narrowed in migration and init-roles section 9.
  The startup policy now checks all three task tables; new runtime needs 0008.

### 10.3 Contract and frontend

OpenAPI 0.6.0 adds mode/target to publication and
`POST /api/v1/tasks/{taskId}/progress` behind AuthFilter, with an empty body and
Idempotency-Key. Responses contain task/event identifiers, this event's count/target,
recorded time, completed/ignored flags and nullable settlement facts, never balances.
Wrong-mode use returns 409 `TASK_COMPLETION_MODE_INVALID`.
Legacy single-task publication fingerprints and JSON response shapes are retained;
only counting task responses include the new mode/target/progress fields.

The form adds completion mode and target; assignees get +1 on count tasks and
complete on single tasks. Lists, history and detail show progress. An action is
registered before sending, so duplicate clicks share one pending action. Lost,
unreadable or unusable responses preserve its key for explicit retry only;
logout/account switches discard late responses. No offline queue or auto-retry.
Confirmed responses trigger fresh list reads (and an open detail refresh), never
assign an old replay count into current state. Only settled responses reload the
wallet; response amounts remain feedback, not balances. Ignored excess never
appears as another reward. Existing view-generation guards remain in use.

### 10.4 Files and focused tests

- Database: new 0008; init-roles section 9; task/wallet SQL scripts' maintenance
  TRUNCATE preparations include the new referencing table so FK failures do not
  mask the original append-only guards. Expected errors and ROLLBACK are preserved.
- Backend: `TaskService.*`, `TaskCompletionService.*`, `TaskController.cc`,
  `Errors.*`, `TaskPrivilegePolicy.*`; existing service/ledger unit helpers,
  `UnitTest.cc`, `DbGatedTest.cc`, `StaticCheckTest.cc` and new `TaskProgressDbTests.h`.
- Web: task API/types/rules/errors, tasks store/view; existing client/view specs
  and new `tasks-progress.spec.ts`. No wallet/session-store change.
- New real-service tests reuse existing helpers: non-final no settlement,
  immutable replay, final shared transaction, ignored excess, key domains,
  same/different-key concurrency, final D32 fault rollback with observed teardown
  and same-key retry, authorization/expiry, SQL gap and premature/missing settlement
  guards. Only isolated committed fixtures on literal `usapp_test`; no reset/purge.
- Parameterized unit checks cover N bounds, fingerprint/shape compatibility and
  new outcomes. Frontend cases cover +1 routing, final/non-final wallet refresh,
  duplicate clicks, unknown retry, old snapshot replay, logout/switch and offline.
  Existing M3 regressions remain. No new test framework, dependency or timeout.

### 10.5 Verification status and owner sequence

Codex changed source and performed read-only static review/hygiene/hash checks.
At delivery, Codex ran no compiler, test, package, Git, container or database
command; no SQL was executed, no fixtures created and no web dist built by Codex.
Subsequent owner-run results are recorded in Sections 10.6-10.7. Migration 0000-0007,
Db.h/Db.cc, wallet implementation, dependencies and deployment are untouched.
Frozen 0006/0007 hashes match the recorded values. M4 is not accepted or deployed.

1. On `usapp_test`, owner DryRun: 0000-0007 checksums match, only 0008 pending.
2. Apply 0008 with InitRoles to test DB; repeat InitRoles and inspect narrowed
   task/progress and wallet privileges. Rebuild test image and run full CTest;
   db_gated must pass, not skip, inside its existing 30 s budget. Rerun both
   task/wallet SQL scripts in application and maintenance modes after copying
   their current source into the container. Web unit suite and typecheck, no build.
3. Only after checks: fresh main backup and scratch restore verification,
   preserve current API image and served dist, main DryRun/application and grants,
   then deploy backend and confirm both startup policies before building web dist.
4. Representative live acceptance: N=2 below/final steps, rewards once, publisher
   no control, same-key replay and ignored excess, deadline/security/offline checks.
   Record actual outcomes here once; Git publication remains owner-controlled.

The owner-run passes in Sections 10.6-10.7 verify compilation, type checking,
the implemented database cases and their measured timing. They do not verify
every possible cross-task key-conflict interleaving. Existing-account final fault
rollback ran in db_gated;
count-specific expiry while waiting across a deadline is not separately covered
(M3-C shared lock/deadline tests remain). No real-service near-cap test or forced
lost-COMMIT test. Committed fixture growth is permanent; one row per accepted +1.
Old runtimes cannot serve counting and their completion path cannot bypass its
target: runtime rollback preserves settlements and supports legacy single tasks,
but interrupts count/retry functionality. Do not attempt schema/data rollback
by deleting immutable rows or editing an applied migration.

### 10.6 Owner-run backend verification and narrow repair (2026-10-14)

- Owner DryRun on `usapp_test`: 0000-0007 checksums matched; only 0008 pending.
  Owner applied 0008 with InitRoles, exit 0, SHA-256
  `36fe053917c86bcd7ed1931b085f59effd778205d93a1ac871e53b3336413065`.
  0008 is now applied to the test database and must not be edited. No main
  application is reported. Repeated InitRoles output was not supplied separately.
- Test image built successfully in 33.5 s. Full CTest: unit failed (0.02 s,
  895 assertions, one failure); db_gated passed (7.85 s, not skipped);
  static_check passed (0.00 s); total 7.88 s, exit 8.
- Unit failure: the missing-column fixture removed `task_id` checks from both
  task_completions and the new task_progress_events, producing six missing
  keys while the assertion expected three. Codex repaired UnitTest.cc only:
  remove checks for one named table at a time, test both tables, and require
  exactly the three subject-specific missing keys. Production policy, service,
  migrations and grants are unchanged. No assertion or permission guard weakened.
- Codex ran no build/test/Git/container/database command. The repaired unit target
  needed an owner rebuild and targeted rerun when this entry was written (completed
  in Section 10.7). Earlier database/static passes are separate evidence, not a
  later full-suite pass. SQL-script role compatibility reruns remain pending.
  M4 is not accepted or deployed.

### 10.7 Owner-run targeted unit rerun and frontend checks (2026-10-14)

- Rebuilt test image successfully in 39.2 s. Targeted CTest `unit` passed in
  0.02 s (total 0.02 s), exit 0. All three backend targets have now passed across
  the recorded runs; no full CTest rerun after the test-only repair is claimed.
- Web full unit suite: 13 files, 120 tests passed, 7.04 s, exit 0. Typecheck
  (vue-tsc and tsc) passed, exit 0. Codex ran neither command.
- Pending: role-mode SQL-script compatibility reruns, separately reported repeated
  InitRoles evidence, main 0008 rollout, M4 runtime/web build, live counting checks
  and an acceptance decision. No Git commit/push or M4 web build is reported.
- 0008 remains frozen on usapp_test with the checksum in Section 10.6; no main
  application is inferred. These automated checks do not constitute deployment
  or exhaustive security verification. D33 and forced lost-COMMIT tests remain gaps.

### 10.8 Owner-run M4 deployment and scope close (2026-10-16)

This supersedes the pending statements above; they describe the state when written.
All results below were supplied by the owner, not executed by Codex.

- Repeated InitRoles on usapp_test skipped all nine applied migrations with
  matching checksums and exited 0. The current task and wallet SQL scripts passed
  as both usapp_api and postgres: respectively 27 and 28 groups in each mode,
  with SET CONSTRAINTS, ROLLBACK and exit 0. No permanent script fixtures claimed.
- Fresh main backup: backups/usapp_before_m4_20261016_170559.dump, 218770 bytes.
  Single-transaction restore into usapp_m4_restore_20261016_170559 succeeded.
  Basic counts matched: accounts=3, audits=19, completions=2, entries=3, members=2,
  migrations=8, operations=2, spaces=1, tasks=6, users=2. This is a count-level
  restore check, not a row-by-row comparison.
- Main DryRun exited 0: only 0008 was pending, checksum
  36fe053917c86bcd7ed1931b085f59effd778205d93a1ac871e53b3336413065.
  The main apply/repeated-InitRoles command outputs were not supplied; do not
  invent their exit codes. Subsequent main permissions and the healthy M4 runtime
  establish that the new schema is operational. The migration is frozen.
- Main permission evidence covered tasks, completions, progress and the three
  wallet tables. API SELECT/INSERT were true; forbidden table, push and PUBLIC
  privileges false. Only wallet_accounts.lock_token had UPDATE. API/push roles
  were nonsuperusers with zero memberships.
- Saved API image sha256:5c1bef4e72350f2d287013665b1033ca76bd8fd060a3bf6cd579ea509c90d5d9
  as usapp-api:before-m4-20261016_170559. Served web backup:
  backups/before-m4-20261016_170559/web-dist, 19 files including index.html/sw.js.
  The owner saved rollback-info.json. Runtime rollback was not exercised.
- API build 13.8 s, exit 0; updated container healthy in 6.6 s, exit 0. Startup
  wallet policy had 187 checks, task policy 217; both ok=true, zero mismatches,
  missing and unexpected checks. Web build exited 0; SW precache 18 entries,
  169.39 KiB. Automated results remain Section 10.7: targets passed across runs,
  not a single post-repair full backend rerun.
- Owner reported success for the representative N=2 task (flower 1, sunshine 3):
  publisher had no +1 control; first recipient +1 showed 1/2 with unchanged
  balances after refresh; second showed history 2/2 and credited 1/3 once;
  another refresh did not award again. Exact before/after balances were not pasted.
- The owner explicitly waived the remaining M4 live tests and asked to start
  the next feature. They were not executed or passed. No final main-data query,
  exhaustive security claim or later Git publication is recorded. M5-A daily
  counting is now assigned: details in phase1-m5-daily.md.
