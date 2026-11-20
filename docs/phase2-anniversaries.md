# Phase 2 P2-2: Anniversaries and countdowns

Status (2026-10-29): **design approved by the owner with defaults O1-O10; implemented by
Claude Code; NOT DEPLOYED.** All usapp_test stages passed on 2026-10-29 (Section 15.1):
rollback-only prevalidation, 0018 apply with InitRoles, privilege checks, full CTest with
db_gated block 25, the SQL script in both roles, web suite and typecheck. **0018 is applied
on usapp_test and frozen** (not applied on main). Implementation and verification record:
Sections 14-15. The design text in
Sections 1-13 is kept as approved; where the implementation adds detail, Section 14 says so.

**P2-1 live/device acceptance is DEFERRED, not passed.** The owner paused
live/deployment/device verification on 2026-10-29. P2-1 remains as recorded in
[phase2-messages.md](phase2-messages.md) Section 13: deployed to main and checked by
automated tests, with live message workflows, phone access and PWA installation unverified.
P2-2 does not depend on that acceptance. Its own rollout (Section 11) needs live checks too,
and those are deferred as well. P2-2 remains **NOT DEPLOYED** unless a future, separate
authorization changes that.

Grounding: V1 §19 (top right), §24, §29, §30, §31, §42 (`Anniversary`, `EditRequest`), §44,
§46, §47, §54; frozen migrations 0000-0017; the M7 revision pattern (0014,
[phase1-m7-rewards.md](phase1-m7-rewards.md) Section 3); the M6 proposal/decision pattern
(0013); P2-1 messages (0017, `MessageService`, `stores/home.ts`, `stores/messages.ts`) and
the retry rules in phase2-messages.md Section 12.

## 1. Requirements traced

| V1 | Requirement | P2-2 handling |
|---|---|---|
| §24 | Shared module. Each item has a title, date, type (纪念日 / 倒数日), note, optional image and a "show on Home" choice. If none is chosen, Home shows the nearest date. | Shared items with title, date, kind and note. Images are excluded (Phase 3, §45). "Show on Home" becomes a per-user pin (Section 2.5). Deterministic automatic selection (Section 4). |
| §19 top right | Users can choose which item Home shows; otherwise "自动显示时间最近的一个" | The Home top-right card replaces the M10 placeholder `home-later-top` |
| §44 | Store civil dates as local dates; avoid UTC date shifts | `date` column. Beijing "today" comes from the database clock. Date-only arithmetic is done in pure C++. The web never parses `YYYY-MM-DD` with `new Date()` (Section 3). |
| §29 | The creator edits directly; the partner cannot overwrite the creator's content | Direct edits are creator-only and use `expected_version` |
| §30 | Edit request: original content, proposed content, requester, time, status Pending/Approved/Rejected; the creator accepts or rejects; content changes only on acceptance | Append-only proposals and decisions. The original content is the referenced base revision. Withdrawal is added (Section 5). |
| §31 | Non-content state changes may be direct | A home pin is personal state. Nobody needs approval for it. |
| §18.1 | "新纪念日" is an optional partner notification | Not in P2-2. Same-transaction audit rows and stable ids are kept for P2-3. |
| §24 / P2-6 | Anniversary reminders | Not in P2-2 (P2-6) |
| §47 | "我们" hosts anniversaries | Section on `/us`, plus a detail route |
| §54 | Server-side membership on every access | Every statement joins the caller's `couple_members` row. Another space's id gives 404. |

## 2. Product rules and proposed defaults

### 2.1 Fields

| Field | Rule (proposed) |
|---|---|
| `title` | 1-40 code points, single line, trimmed. Same character rules as task/product titles (0007/0014 CHECKs). |
| `date` | Civil date `YYYY-MM-DD`, 1900-01-01 to 2199-12-31. The range must reach before 1970 so birthdays fit. |
| `kind` | `anniversary` (纪念日) or `countdown` (倒数日) |
| `note` | 0-500 code points, LF allowed. Same rules as task descriptions (CRLF becomes LF, other control characters are rejected, trimmed). Empty is allowed: V1 lists 备注 without making it mandatory. |

Either member may create an item. A space without a partner may create items too, because
an item has no target member. That is unlike messages and products (owner choice O7). The
creator is fixed at creation.

### 2.2 Anniversary vs countdown (computed against Beijing "today" T)

**`anniversary`, with date D.**
- *Elapsed days*: `days_since = T − D`.
  - If D ≤ T, the label is "已经 N 天", with N = T − D. On D itself N is 0. Owner choice O2
    is the inclusive "第 N+1 天" label.
  - If D > T, the item has not started. The label is "还有 N 天" (N = D − T) and
    `days_since` is negative.
- *Next yearly occurrence*: the smallest `add_years(D, k)` ≥ T with k ≥ 0.
  - The response gives that date, `years = k` and `days_until`.
  - When k ≥ 1 the label is "距 k 周年还有 M 天". When `days_until = 0` the label is
    "就是今天" (plus "k 周年" when k ≥ 1).
  - An anniversary always has a next occurrence.

**`countdown`, with target D.**
- `days_until = D − T`.
  - Greater than 0: "还有 N 天".
  - Equal to 0: "就是今天".
  - Less than 0: "已过 N 天".
- A countdown does not recur. A past countdown remains readable and stays in the list.

**Recurrence.** No configurable recurrence is in scope. Yearly repetition is implicit for
`anniversary` (birthdays are entered as anniversaries). There is no recurrence column or
rule. A yearly *countdown* flag is owner choice O3 and is deferred; adding it later is
additive.

### 2.3 Leap day

`add_years(2024-02-29, k)` falls on Feb 28 in non-leap years. This matches PostgreSQL's
`date + interval 'k years'` clamping, so a later SQL mirror cannot disagree. Every year is
computed from the original D, never from the previous occurrence, so the date returns to
Feb 29 in leap years. `years` counts calendar years from D's year. Owner choice O4 is to use
Mar 1 instead.

### 2.4 Edit rights (§29/§30/§31)

- **Creator**: edits directly (title, date, kind and note together) with `expected_version`.
- **Partner**: cannot edit directly (403 `ANNIVERSARY_EDIT_FORBIDDEN`). The partner submits
  an edit proposal instead (Section 5).
- **Kind**: a kind change is an ordinary content edit.
- **Identity**: edits append revisions to the same anniversary id. There is no successor
  entity and no M6-style close-and-replace model.

### 2.5 Home pin: per user (recommended)

- Each member pins at most one item for **their own** Home, or clears the pin. The partner's
  Home is unaffected. The partner's pin is never returned to the caller.
- Why per user:
  - V1 §19 says "用户可以指定"; each member has their own Home.
  - A shared per-item flag would let one member change the other's Home. As an item field it
    would also fall under §29/§30, needing creator edits or proposals just to change Home.
  - Per-user state has no cross-member conflict and needs no approval (§31).
- The V1 §24 per-item "是否在首页展示" is therefore represented by "pinned by me". A shared
  flag is owner choice O1 (not recommended).
- A pin needs no `expected_version`: only the user writes their own pin. Two devices of the
  same user are serialized, and the last writer wins.

### 2.6 Deletion

**Not proposed for P2-2.**
- V1 §24 lists no deletion. Shared-content deletion would itself need §29/§30-style rules.
- A mistaken entry can be corrected by its creator. A pin overrides an unwanted automatic
  selection.
- Owner choice O6 is a creator-only archive. It would be an additive `archive` revision with
  `status`, like M7, and archived items would leave automatic selection.

### 2.7 No other features

There are no notifications, Push, reminders, images, wallet effects, revision-history UI or
read receipts.

## 3. Beijing civil dates

- Storage: `event_date date`. It is never a `timestamptz` and never derived from an instant.
- "Today": every read evaluates `(clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date`
  once in the same statement that reads the rows. This is the existing convention
  (`task_read_model`, 0013). The API returns it as `as_of_date`, and every computed field
  refers to it. `couples.timezone` exists but is fixed to `Asia/Shanghai` (handoff Section 3),
  so the literal is used as elsewhere.
- Arithmetic: `AnniversaryCalendar` is a new pure C++ unit. It does days-from-civil and
  civil-from-days arithmetic, `add_years` and the selection order, with no time-zone or
  `std::tm` use. It is the single source of computed values, and unit tests cover it with no
  database.
- Web:
  - date inputs use `<input type="date">` strings directly;
  - validation compares `YYYY-MM-DD` strings;
  - display uses the server-computed fields;
  - no `new Date('YYYY-MM-DD')` (which is UTC midnight) and no `toISOString().slice(0, 10)`.
- Midnight: Home already refreshes on return or Beijing-date rollover (`beijingDate` in
  `stores/home.ts`). The anniversary card is refetched then, not recomputed on the client.

## 4. Deterministic Home selection

`GET /api/v1/anniversaries/home` returns exactly one of:

1. **Pinned**: the caller's current pin, if set. The item always exists because there is no
   deletion. The response has `source: "pinned"`.
2. **Upcoming** (`source: "auto"`): over all items in the space, `distance` is:
   - for an anniversary, `next.days_until` (always ≥ 0);
   - for a countdown with D ≥ T, `D − T`.

   The item with the smallest `(distance ASC, created_at ASC, id ASC)` wins.
3. **All in the past** (`source: "auto_past"`): this happens only when every item is a
   countdown with D < T. The most recently passed one wins: `(D DESC, created_at ASC, id ASC)`.
   It is shown as "已过 N 天". Owner choice O5 is to show the empty state instead.
4. **None** (`source: "none"`): the space has no items, and the card shows "还没有纪念日".

Ties are broken by immutable creation time, then id, so two readers at the same T always
agree. The selection runs on the server in C++ over the space's current revisions.

## 5. Edit proposals (V1 §30), without a workflow engine

These rules are specific to this domain and live in their own tables. They do not reuse or
generalize the M6 task tables.

- **Propose** (partner only; 403 for the creator): the full proposed term set plus
  `expected_version`, which becomes `base_version`.
  - `base_version` must equal the current version, otherwise 409
    `ANNIVERSARY_VERSION_CONFLICT`.
  - The proposed terms must differ from the base, otherwise 400 `VALIDATION_ERROR`.
  - §30's "原内容" is the immutable base revision, referenced by foreign key.
    The requester, time and status are columns or derived.
- **One pending proposal per anniversary.** A second one gets 409
  `ANNIVERSARY_PROPOSAL_PENDING`.
- **Creator direct edit while a proposal is pending**: 409 `ANNIVERSARY_PROPOSAL_PENDING`
  (recommended, option A). The creator approves or rejects first; the UI offers
  "拒绝并自己修改". Under A the version changes only through the creator's edit or an
  approval, so a pending proposal can never be stale. Option B is owner choice O8: allow
  the edit and close the pending proposal in the same transaction with outcome `superseded`.
- **Decide** (`approve` / `reject`): creator only. Approval appends revision `base_version + 1`
  with exactly the proposal's terms, `action = 'approve'` and a `proposal_id` link, in the same
  transaction as the decision row.
  - A defensive check rejects approval unless `base_version` is still the current version:
    409 `ANNIVERSARY_PROPOSAL_STALE`. The database guard enforces the same rule.
  - Rejection changes nothing else and carries no reason text.
- **Withdraw**: proposer only, while pending.
- **Decided proposals** are final: a second decision gets 409 `ANNIVERSARY_PROPOSAL_DECIDED`.
  After any decision the partner may propose again against the current version.
- **Races**: decisions on one proposal race through the decision primary key and the
  advisory lock, so exactly one of approve, reject or withdraw wins. The loser gets
  `ANNIVERSARY_PROPOSAL_DECIDED`. A replay of the winner's key returns the original result.

## 6. Database (proposed migration `0018_anniversaries.sql`; nothing written yet)

0000-0017 are not touched. The preflight requires `public.message_replies` (0017).
SQLSTATE class `UN` is currently unused (used classes: UW UT UP UD UC UX UR UM UB). The new
advisory lock classes are `33010` (per anniversary) and `33011` (per user pin); the current
two-integer locks use only 33004, 33007 and 33008. All tables are append-only with
`SELECT, INSERT` for `usapp_api`.

| Table | Columns and constraints |
|---|---|
| `anniversaries` (identity) | `id`, `couple_id`, `creator_id`, `created_at`; `UNIQUE (id, couple_id)`; creator FK to `couple_members`; index `(couple_id, created_at DESC, id DESC)` |
| `anniversary_revisions` | PK `(anniversary_id, version)`; `couple_id`, `actor_id`, `action` (`create`/`edit`/`approve`), `proposal_id` (only for `approve`), `title`, `event_date`, `kind`, `note`, `idempotency_key`, `request_fingerprint`, `created_at`. `create` ⇔ version 1. Key and fingerprint are NOT NULL except for `approve`, whose key lives on the decision. `UNIQUE (actor_id, idempotency_key)`. Shape CHECKs as in Section 2.1. |
| `anniversary_edit_proposals` | `id`, `couple_id`, `anniversary_id`, `proposer_id`, `base_version`, proposed `title`/`event_date`/`kind`/`note`, key, fingerprint, `created_at`; FK `(anniversary_id, base_version)` → revisions; `UNIQUE (proposer_id, idempotency_key)` |
| `anniversary_edit_decisions` | PK `proposal_id`; `couple_id`, `decided_by`, `outcome` (`approved`/`rejected`/`withdrawn`), key, fingerprint, `decided_at`; `UNIQUE (decided_by, idempotency_key)` |
| `anniversary_home_pins` | PK `(user_id, seq)`; `couple_id`, `anniversary_id` (NULL = cleared), key, fingerprint, `created_at`; member FK `(couple_id, user_id)`; FK `(anniversary_id, couple_id)` → identity, so a pin cannot reference another space. The current pin is the highest `seq`. |

Guards, all VOLATILE and all taking the lock before reading:

| Code | Guard |
|---|---|
| UN001 | Statement-level append-only on all five tables |
| UN002 | Revision guard, under 33010: actor = creator; `version = previous + 1`; `edit` requires no pending proposal (option A); `approve` requires an `approved` decision for a proposal of this anniversary with `base_version = previous` and identical terms; times are monotonic and not in the future |
| UN003 | Proposal guard: proposer is a member ≠ creator; `base_version` = current version; no pending proposal |
| UN004 | Decision guard: `approved`/`rejected` ⇒ `decided_by` = creator; `withdrawn` ⇒ `decided_by` = proposer; time ≥ proposal time |
| UN005 | Deferred constraint triggers: an identity row has its version-1 `create` revision; an `approved` decision has exactly one `approve` revision |
| UN006 | Pin guard, under 33011: `seq = previous + 1` for that user |

Grants and enforcement:
- The migration and a re-runnable `init-roles.sql` section 16 grant the five tables.
- All five join the startup privilege policy and `task_privilege_deviation.sql`, as 0017
  did, so the new API refuses to start without 0018.

**No speculative columns**: no image key, recurrence, notification, reminder or archive.

## 7. API (proposed contract 0.15.0)

All endpoints require a session and membership and return `Cache-Control: no-store`. Every
write needs the exact Origin, the session CSRF token and an `Idempotency-Key`. Writes return
`{mutation, anniversary}` (or `{mutation, pin}`): `mutation` holds the immutable facts and is
identical on replay; the second object is current state at read time, as in P2-1 and M7.

| Method and path | Who | Result |
|---|---|---|
| `POST /api/v1/anniversaries` `{title, date, kind, note}` | member | 201 / 200 replay |
| `GET /api/v1/anniversaries?limit&cursor` | member | `{as_of_date, anniversaries, next_cursor}` |
| `GET /api/v1/anniversaries/{id}` | member | `{as_of_date, anniversary}` |
| `POST /api/v1/anniversaries/{id}/revisions` `{expected_version, title, date, kind, note}` | creator | 201 / 200 |
| `POST /api/v1/anniversaries/{id}/proposals` `{expected_version, title, date, kind, note}` | partner | 201 / 200 |
| `POST /api/v1/anniversaries/{id}/proposals/{proposalId}/decision` `{outcome: approve\|reject\|withdraw}` | creator (approve/reject), proposer (withdraw) | 200 |
| `GET /api/v1/anniversaries/home` | member | `{as_of_date, source, anniversary\|null}` |
| `POST /api/v1/anniversaries/home-pin` `{anniversary_id: uuid\|null}` | member, own pin | 200 `{mutation, pin}` |

`Anniversary` contains:
- `id`, `creator_id`, `version`, `title`, `date`, `kind`, `note`, `created_at`, `updated_at`;
- `pinned_by_me` (boolean);
- `computed`:
  - `days_since` (anniversary only);
  - `next: {date, years, days_until}` (anniversary only);
  - `days_until` (countdown only);
- `pending_proposal`: null or `{id, proposer_id, base_version, title, date, kind, note,
  created_at}`;
- `last_decision`: the newest decided proposal of this anniversary, or null, so the proposer
  sees the outcome.

**List cursor (stable).**
- The order is the immutable identity `(created_at DESC, id DESC)`. The cursor uses the
  existing strict `<timestamp>_<uuid>` format; limit is 1-100, default 50; an invalid limit
  or cursor gives 400 before any query.
- Edits never move a row, and new items appear only before page 1. Following `next_cursor`
  therefore neither skips nor repeats rows. Pages are not snapshots of content.
- Sorting by date or next occurrence is **rejected** for the cursor: edits and the daily
  rollover would reorder rows between pages. Nearness is handled by the Home endpoint and by
  each row's computed fields.

**Idempotency, replay and an unknown commit (P2-1 rules).**
- Each table has its own `(actor, key)` uniqueness. The fingerprint is SHA-256 of
  `anniversary-<action>:v1:` plus the path ids, `expected_version` and length-prefixed fields.
- Key classification runs **before** state checks. A retry of a committed edit therefore
  replays (200) instead of returning `ANNIVERSARY_VERSION_CONFLICT`. A different fingerprint
  gives 409 `IDEMPOTENCY_KEY_REUSED`.
- Inserts use `ON CONFLICT DO NOTHING`; a skipped insert is classified again.
- Success is reported only after a confirmed commit. An unconfirmed commit returns 503
  `RESULT_UNKNOWN`.

**Transactions** (one `TxScope`, READ COMMITTED):
1. Caller membership `FOR SHARE`.
2. Advisory lock 33010 on the anniversary, or 33011 on the user for a pin.
3. Key classification.
4. Role and state checks.
5. Insert.
6. Audit row in the same transaction. Audit details hold ids, version, kind and outcome,
   never text.
7. Read back, then commit.

Audit actions: `anniversary.created`, `.edited`, `.edit_proposed`, `.edit_approved`,
`.edit_rejected`, `.edit_withdrawn`, `.home_pinned`.

**New errors**:
- `ANNIVERSARY_NOT_FOUND` (404; also for another space's id or pin target);
- `ANNIVERSARY_EDIT_FORBIDDEN` (403);
- `ANNIVERSARY_VERSION_CONFLICT` (409);
- `ANNIVERSARY_PROPOSAL_PENDING` (409);
- `ANNIVERSARY_PROPOSAL_NOT_FOUND` (404);
- `ANNIVERSARY_PROPOSAL_DECIDED` (409);
- `ANNIVERSARY_PROPOSAL_STALE` (409).

Existing errors that also apply: `VALIDATION_ERROR`, `SPACE_NOT_FOUND`,
`IDEMPOTENCY_KEY_REUSED`, `RESULT_UNKNOWN`.

**Backend files (proposed)**:
- `AnniversaryService.h/.cc` and `AnniversaryCalendar.h/.cc` (pure);
- `AnniversaryController.cc`;
- `Errors.*` and the startup privilege policy objects;
- `CMakeLists.txt`.

## 8. Web (proposed)

- **API layer**: `api/anniversaries.ts` and `api/anniversary-rules.ts` (validation only),
  plus types and error messages.
- **`stores/anniversaries.ts`**:
  - list, detail and the create/edit/propose/decide/pin actions;
  - session generation, request sequences and identity reset;
  - Section 12 retry rules: once a result is unknown, the action keeps its original key and
    payload across later errors. Only a valid same-key 2xx resolves it. Nothing is persisted
    and nothing is replayed automatically.
  - A success must match the captured target (anniversary id, `version = expected + 1`,
    terms, proposal id or outcome, pin target); otherwise the result stays unknown.
  - The edit or proposal form captures an immutable `{id, version}` snapshot when it starts
    (M7 pattern). A version conflict keeps the draft and asks the user to refresh.
- **`/us`**: a "纪念日" section above the message board, with its list and "加载更多" and a
  create form.
- **`/us/anniversaries/:id`** (new `AnniversaryDetailView.vue`):
  - the creator sees the edit form and any pending proposal with approve/reject;
  - the partner sees the proposal form or their pending proposal with withdraw;
  - both see the pin toggle "在我的首页展示".
  - The page clears drafts when the id or account changes (P2-1 Section 11.2).
- **Home**:
  - `home.ts` gains an independent `loadAnniversary()` with its own sequence: one request
    to `/anniversaries/home`, never a list scan;
  - `HomeView` replaces `home-later-top`;
  - the loading state shows no content; an error shows retry and never the empty state;
  - tapping opens the detail, or `/us` when the source is `none`;
  - a failure of this card never affects the other cards, and theirs never affect it.
- **Isolation**: `reset()` clears anniversary and Home state on logout, account switch or
  bootstrap failure. Late results from an earlier identity are discarded.

## 9. Acceptance tests (to write during implementation; none written or run)

- **C++ unit (`AnniversaryCalendar`, parsing)**:
  - `days_since`, the next occurrence and `days_until` around D = T, D = T ± 1 and future
    anniversaries;
  - Feb 29 in leap years, non-leap years and across a century (2100);
  - date range edges 1900-01-01 and 2199-12-31;
  - selection order in all four sources and in every tie-break;
  - parse rejections (extra fields, bad date, control characters, lengths);
  - fingerprint separation by action; error mapping.
- **db_gated block 25** (committed `@m3-fixture.test` spaces):
  - create, replay and key reuse;
  - a creator edit with a stale `expected_version` gives a conflict, while the same-key
    retry of a committed edit replays;
  - partner direct edit gives 403; creator propose gives 403;
  - propose → approve applies the exact terms as version + 1;
  - reject and withdraw change nothing;
  - a second pending proposal and a creator edit while pending give 409;
  - concurrent approve versus withdraw: exactly one wins;
  - the decision actor matrix;
  - cross-space ids give 404 for read, edit, propose, decide and pin;
  - pins are per user: A's pin does not change B's Home, and the partner's pin is never
    returned;
  - Home in all four sources against the database's Beijing date;
  - a Beijing date that differs from UTC: an instant at 16:00-23:59 UTC is checked in a
    rollback-only test with a stated "today" through the pure function, without changing the
    server clock.
- **SQL script `anniversary_ledger_test.sql`** (both roles, rollback only): UN001-UN006,
  grants, append-only behaviour, and a foreign key preventing a cross-space pin.
- **Static and policy**: privilege deviation query with 0 rows; the startup policy includes
  the five tables.
- **Web (Vitest)**:
  - the store's unknown-retry matrix (as in messages-repairs) and target mismatch;
  - snapshot versions, account switch and late results;
  - Home card states, independent failure and the rollover refetch;
  - a regression test that no `new Date('YYYY-MM-DD')` path exists in the formatting
    helpers, as a behaviour test for a UTC-negative environment (`TZ=America/Los_Angeles`
    and `TZ=Asia/Shanghai`), not a keyword scan;
  - bottom-nav ownership of `/us/anniversaries/:id`.

## 10. Failure and compatibility risks

- **Startup coupling.** As with 0017, adding the tables to the startup policy means the new
  API cannot start before 0018. Whether the *rollback* API image accepts a database with 0018
  applied (unexpected-privilege reporting) has not been checked; verify it during test
  rollout.
- **Deploy order.** A new web against the old API gets 404 on the anniversary endpoints. The
  Home card shows an error, not an empty state. Deploy the API first, then the bind-mounted
  web.
- **Unbounded Home load.** `/anniversaries/home` reads every current revision in the space,
  which should be tens of items. No cap is proposed (O9). Very large spaces would slow Home.
- **Identity reset.** An unresolved create after an identity reset can still produce a
  duplicate item on manual re-entry. This limit is shared with P2-1 Section 12. Duplicate
  edits are impossible because `expected_version` forbids them, and a duplicate proposal hits
  `ANNIVERSARY_PROPOSAL_PENDING`.
- **Option A blocking.** A partner's pending proposal blocks the creator's direct edits
  until decided. This is an intentional trade-off (O8).
- **Leap-day rule.** The Feb 28 rule is a product decision. Changing it later changes
  computed labels, not stored data.
- **No deletion.** Mistaken items stay visible. A pin and creator edits are the only
  mitigation until O6.
- **Not verified.** No migration, build or test of this design exists. All of the above is
  static design reasoning.

## 11. Rollout prerequisites (after approval and implementation; none authorized now)

1. The owner approves this design and the O-choices below.
2. Implementation on usapp_test:
   - rollback-only pre-validation of 0018 plus the SQL script;
   - DryRun showing only 0018 pending;
   - apply with InitRoles, then the privilege query with 0 rows;
   - repeat InitRoles, then the privilege query with 0 rows;
   - full CTest (db_gated not skipped);
   - the SQL script as both roles;
   - the web suite and typecheck.
3. Main, separately authorized:
   - a fresh backup with a restore check;
   - DryRun, then apply with InitRoles and the privilege checks;
   - API deploy and startup-policy lines;
   - web build only after the API is healthy;
   - every Compose command with `-f .\deploy\docker-compose.lan.yml`.
4. Browser and phone acceptance (**deferred** while the owner's pause lasts):
   - create, edit, propose, approve, reject and withdraw;
   - pin and clear;
   - Home selection, including leap-day and past-countdown fixtures;
   - account switching;
   - a Beijing-midnight rollover.

## 12. Owner decisions needed

| # | Question | Proposed default |
|---|---|---|
| O1 | Home pin per user or shared? | **Per user** (Section 2.5) |
| O2 | Elapsed-day label: N = T − D ("已经 N 天") or inclusive ("第 N+1 天")? | T − D. The API returns the raw value, so the label is a web-only change. |
| O3 | Any configurable recurrence (for example yearly countdowns)? | No; yearly is implicit for `anniversary` only |
| O4 | Feb 29 in non-leap years: Feb 28 or Mar 1? | Feb 28 |
| O5 | All items in the past: show the most recently passed countdown, or the empty state? | Most recently passed |
| O6 | Deletion or archive? | None in P2-2. A creator-only archive could be added later. |
| O7 | Allow creation before the partner joins? | Yes |
| O8 | Creator direct edit while a proposal is pending: block (A) or supersede (B)? | **A, block** |
| O9 | Per-space item cap? | None |
| O10 | Title 40, note 500, date range 1900-2199? | As proposed |

## 13. Codex static design review (2026-10-29)

The delegation call timed out after 300 seconds, but this design file was present and
read in full. This is a static review of the written proposal, not a completion or
verification report from the worker. Implementation, migration and runtime verification
have not been performed for P2-2.

Implementation must make these details explicit:

- `anniversary_home_pins` needs `UNIQUE (user_id, idempotency_key)` as well as the
  `(user_id, seq)` primary key. Its key and fingerprint must be non-null and shape-checked.
  A same-key retry replays the immutable pin mutation even after another pin was selected;
  it must not append a new sequence or restore an older pin.
- The 1900-2199 bound applies to the stored input date, not to a derived next occurrence.
  For example, an anniversary read after its 2199 occurrence may compute a date in 2200.
  Calendar calculations must not overflow, truncate or reject that derived date merely
  because it exceeds the input bound.
- Home selection must inspect the complete current dataset, not the first list page or a
  silently truncated scan. The proposed all-row implementation has an explicit performance
  limitation; assess the query and current-revision lookup during implementation rather
  than assuming every space stays small. No benchmark or load result is claimed here.
- Approval, rejection and withdrawal must target the proposal's anniversary and space,
  and mutation responses must carry enough immutable identifiers and terms for the web
  store to validate an explicit same-key success. A rejected retry after an earlier unknown
  result must not discard that unresolved action.

The creator-edit blocking rule (O8) is a product trade-off requiring owner confirmation,
not a requirement inherited from task editing. The remaining proposed defaults are also
not recorded as approved by this document.

## 14. Owner approval and implementation (2026-10-29)

**Approval.** On 2026-10-29 the owner approved this design with the proposed defaults
O1-O10 (Section 12): per-user pin, N = T − D, no configurable recurrence, Feb 29 → Feb 28,
most recently passed countdown when all are past, no deletion or archive, creation before
the partner joins, option A (creator edit blocked while a proposal is pending), no per-space
cap, title 40 / note 500 / dates 1900-2199. The owner assigned the complete milestone to
Claude Code with verification on usapp_test only. P2-3 is not authorized.

**Earlier Batch A.** A previous delegation of "Batch A" (calendar helpers) had no output in
the repository: no 0018, no AnniversaryService/Calendar, no web files and no `anniversar`
reference anywhere. Implementation therefore started from this design.

### 14.1 Files

Database:
- `database/migrations/0018_anniversaries.sql` (new, 388 lines, SHA-256
  `58137f310a682ff41c969ccb7e3608fa1b44f3a657aa83b14b6613b61b532f9f`). Applied and frozen on
  usapp_test 2026-10-29; not applied on main.
- `database/init-roles.sql` section 16; `database/tests/task_privilege_deviation.sql` (+5 tables);
  `database/tests/anniversary_ledger_test.sql` (new; both roles, `ROLLBACK`).

Backend:
- New: `services/AnniversaryCalendar.{h,cc}` (pure), `services/AnniversaryService.{h,cc}`,
  `controllers/AnniversaryController.cc`.
- Changed: `core/Errors.{h,cc}` (7 codes), `db/TaskPrivilegePolicy.cc` (matrix SQL,
  `taskTables()`, `requiredColumnFor()`), `CMakeLists.txt` (controller).
- Tests: new `tests/AnniversaryUnitTests.h` and `tests/AnniversaryDbTests.h` (db_gated block 25,
  gate `0018_anniversaries`); changed `UnitTest.cc` (registration, policy matrix entries),
  `DbGatedTest.cc`, `StaticCheckTest.cc` (contract 0.15.0, P2-2 case).

Contract: `contracts/openapi.yaml` 0.15.0 (8 operations, schemas, 7 error codes,
`AnniversaryId` parameter, `anniversaries` tag).

Web:
- New: `api/anniversaries.ts`, `api/anniversary-rules.ts`, `stores/anniversaries.ts`,
  `components/AnniversaryForm.vue`, `components/AnniversarySection.vue`,
  `views/AnniversaryDetailView.vue`, `tests/unit/anniversaries.spec.ts`.
- Changed: `api/types.ts`, `api/error-messages.ts`, `stores/home.ts` (`loadAnniversary`),
  `stores/session.ts` (identity reset), `router/index.ts` (`/us/anniversaries/:id`),
  `components/BottomNav.vue` (detail belongs to Us), `views/HomeView.vue` (top-right card
  replaces `home-later-top`), `views/UsView.vue` (section above the board); specs
  `home.spec.ts`, `home-wallet.spec.ts`, `messages.spec.ts` (mock the new API module) and
  `bottom-nav.spec.ts` (detail ownership and guard).

### 14.2 Behaviour as implemented

- **Tables and guards** as in Section 6, plus `UNIQUE (user_id, idempotency_key)` on pins
  (Section 13), `UNIQUE (proposal_id)` on revisions and a revision → proposal foreign key.
  Revision `key_presence`: key and fingerprint are NULL exactly for `approve`. Pin target
  foreign key `(anniversary_id, couple_id)` keeps pins inside the space. UN001 append-only;
  UN002 revision (actor = creator, `version = prev + 1`, `edit` blocked while pending,
  `approve` needs an `approved` decision by the creator for this item with
  `base_version = prev` and identical terms, time monotonic and not future); UN003 proposal
  (partner, base = current, none pending); UN004 decision (creator approves/rejects,
  proposer withdraws, approval only against the current version, time ≥ proposal); UN005
  deferred (identity has its create revision; an approved decision has exactly one approve
  revision); UN006 pin (`seq = prev + 1`). Locks 33010 (per item) and 33011 (per user pin).
- **Beijing date.** Every read statement evaluates
  `(clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date` once (single-row `OFFSET 0`
  subquery) and returns it as `as_of_date`; `AnniversaryCalendar` computes all values from it.
  Derived next occurrences may lie beyond 2199 (unit-tested).
- **Home** reads every current item of the space in one statement with no limit and selects
  in C++ (pinned > auto > auto_past > none, tie-breaks as Section 4). The caller's pin comes
  from their own highest `seq` only.
- **Writes** follow Section 7: membership `FOR SHARE` → lock → key classification → checks →
  `INSERT … ON CONFLICT DO NOTHING` (re-classify when skipped) → audit (ids, version, kind,
  outcome; never text) → read back → commit. An unconfirmed commit is `RESULT_UNKNOWN`.
  A pin replay returns the original pin facts and never appends or restores a pin; the
  response's `pin` object is the caller's current pin.
- **Responses.** Writes return `{mutation, as_of_date, anniversary}` (pins `{mutation, pin}`).
  `computed` always carries all three keys, with null for the other kind. Decision mutations
  include `anniversary_id`, `proposal_id`, `proposer_id`, `outcome`, `decided_by`,
  `base_version` and `version` (approved version, else null), so the web can validate them.
- **Web.** The store follows the P2-1 Section 12 rule: once a result is unknown, every later
  error keeps the original key and payload until a same-key success that matches the
  captured target (item id, `version = expected + 1`, terms, proposal id and outcome, pin
  target). Forms capture `{id, version}` when opened; a version conflict keeps the draft
  and offers "基于最新版本继续". Drafts reset on item or account change. A list page read on a
  later Beijing date than page 1 restarts the list instead of mixing dates. Labels use only
  server values; dates are formatted from the string, never through `Date`.

### 14.3 Decisions and deviations

- **Unchanged creator edit → 400.** Section 5 requires proposals to differ from the base; the
  implementation applies the same rule to creator edits (`内容没有变化`), so no empty
  revisions are written. This was not specified for edits and is reversible.
- **`computed` shape.** All three keys are present (null when not applicable) rather than
  omitted, for simpler typing; the contract states it.
- **Decision response code.** A first decision returns 200 (not 201), matching the contract
  row "200".
- **Route ambiguity.** `GET /api/v1/anniversaries/home` and `/{anniversaryId}` overlap; the
  `{id}` handler also serves the literal `home` defensively, whichever route Drogon tries
  first. Not run against a live router.
- **Shared form component.** `AnniversaryForm.vue` is shared by create, edit and proposal.
- No deviation from O1-O10. No deletion, archive, recurrence, image, reminder, notification,
  Web Push or wallet code.

## 15. Verification (2026-10-29)

Run by Claude Code:
- Web typecheck with the installed binaries (`vue-tsc --noEmit -p tsconfig.json`, then
  `tsc --noEmit -p tsconfig.node.json`): both exit 0, 16 s.
- Full web unit suite (`vitest run`): exit 0, **24 files / 289 tests passed** in 32 s,
  including the new `anniversaries.spec.ts` (23 tests: rules, the time-zone behaviour test
  under `America/Los_Angeles`, `Asia/Shanghai` and `UTC`, store retry matrix and target
  mismatch, account and logout isolation, Home card states and independence, detail roles,
  conflict draft retention, per-target reset) and the extended bottom-nav test.
- Static checks: LF endings, final newline and no trailing whitespace in every changed file;
  0018 SHA-256 as above.

**First attempt blocked (superseded by Section 15.1).** Docker Desktop was not reachable:
`failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine … The
system cannot find the file specified.` (EXIT=1, checked at the start and again after the
web stages). Docker Desktop was not started by Claude Code. Therefore none of these ran:
- the usapp_test migration-state check (0018 absence is unconfirmed on the database);
- rollback-only prevalidation;
- DryRun, apply with InitRoles, privilege queries, repeat InitRoles;
- the test image build — **the C++ code has not been compiled**;
- full CTest (unit, db_gated block 25, static_check);
- `anniversary_ledger_test.sql` in both roles.

The OpenAPI file was not parsed by a YAML validator (none installed); only the
StaticCheck text assertions cover it, and they have not run.

A fail-fast owner-runnable script for these stages is in
`Claude outputs/p22-verify-usapp-test.ps1` (not tracked). It asserts: local set 0000-0018;
usapp_test at exactly 0000-0017 with matching checksums and no anniversary tables;
prevalidation pass markers in both roles (`groups=3 cases=34`) and nothing persisted;
DryRun 18 skipped plus 0018 pending with the local hash; exactly one apply; 0 privilege
deviation rows after apply and after repeat; repeat 19 skipped; 19 matching registrations;
CTest 3/3 with db_gated `Passed` and nothing skipped; the SQL script in both roles; then the
web suite and typecheck with the local binaries. Direct Compose calls use `.env` and the LAN
override; `apply.ps1` is used unmodified (base Compose file, postgres exec/cp only).

### 15.1 usapp_test run (2026-10-29, after Docker Desktop was started by the owner)

Claude Code ran `Claude outputs/p22-verify-usapp-test.ps1` once from Windows PowerShell
(work folder `%TEMP%\usapp-p22-verify-20261029_184419`). Script exit 0; every stage
asserted, fail-fast; no repair was needed.

- State before: local set 0000-0018; usapp_test had exactly 0000-0017 with checksums equal to
  the local files; 0018 unregistered; no anniversary tables.
- Rollback-only prevalidation (0018 body + `anniversary_ledger_test.sql`, ending in
  `ROLLBACK`): `ALL ANNIVERSARY ASSERTIONS PASSED` with `groups=3 cases=34` as
  postgres/maintenance and as usapp_api/application, exit 0 each. Afterwards 0 registrations
  for 0018 and both checked tables absent.
- Fresh test image build: exit 0, 231 s. Compiler warnings come only from the vendored
  Drogon `HttpUtils.cc` (2), none from project code.
- DryRun: 18 skipped, only `0018_anniversaries` pending with the hash above.
- Apply with InitRoles: exactly one `[apply]` and `[ok   ] 0018_anniversaries`; roles
  initialised. Privilege deviation query: 0 rows.
- Repeat InitRoles: 19 skipped, 0 applied. Privilege deviation query: 0 rows.
- All 19 registered checksums equal the local files, including 0018.
- Full CTest: exit 0, `100% tests passed, 0 tests failed out of 3`, nothing skipped.

  | Target | Time |
  |---|---|
  | unit | 0.03 s |
  | db_gated | 10.54 s |
  | static_check | 0.01 s |

- Evidence that block 25 ran: a read-only count on usapp_test afterwards found 24
  anniversaries, 8 proposals, 6 decisions and 3 pins (plus 32 revisions and 41
  `anniversary.%` audit rows). These are exactly the rows block 25 commits once (24 items,
  8 proposals, 6 decisions, 3 pins), and no other P2-2 writer has run.
- `anniversary_ledger_test.sql` on the applied database: usapp_api/application and
  postgres/maintenance each `groups=3 cases=34`, exit 0, `ROLLBACK`.
- Web: `vitest run` 24 files / 289 tests exit 0; `vue-tsc` exit 0; `tsc` (node config) exit 0.

Not verified: the contract was not parsed by a YAML validator; no live HTTP, browser or
phone workflow; no route-precedence check of `/anniversaries/home` against a running API.

Migration 0018 is applied and frozen on usapp_test; it must never be edited.

**Fixture effects.** Block 25 committed isolated `@m3-fixture.test` spaces on usapp_test
(users, couples, members, the anniversary rows counted above and their audit rows), like
earlier blocks. Prevalidation and the SQL-script runs committed nothing. The container
`/tmp` copies and the `%TEMP%` work folder were left in place.

**Untouched:** main database, API/Caddy containers (no replacement or restart), served web
dist, Git, dependencies, permission settings, frozen migrations 0000-0017.

**Remaining (not authorized):** main backup/DryRun/apply with InitRoles, API deploy, web
build and publish, and browser/phone acceptance, all deferred.

## 16. Review repair: pin replay (2026-10-29)

**Finding (Codex review).** After a successful pin, the store set `pinned_by_me` from the
action's own target. A same-key replay returns the *original* immutable mutation together
with the *current* pin. If device A's pin of item A is replayed after device B pinned item B,
the UI wrongly showed A as pinned.

**Repair (web only; `apps/web/src/stores/anniversaries.ts`).**
- The immutable mutation is still validated against the original action: action `pin`,
  `user_id` = caller, `anniversary_id` = original target (or null), and `seq` a positive integer.
- The returned current pin is validated separately (`isCurrentPin`):
  - `anniversary_id` is a non-empty string or null;
  - `seq` is a positive integer not below the mutation's `seq`;
  - `updated_at` is a non-empty string or null.
  A malformed current pin leaves the action unknown, with its original key and payload.
- `pinned_by_me` in the list and the open detail now comes from `response.pin.anniversary_id`
  only. A cleared current pin (null) selects nothing.
- When the current pin differs from the replayed target, the notice says the request had
  already been saved and was changed later.
- The unknown-retry rules, the original keys and payloads, and the session/account guards
  are unchanged.

**Tests (`apps/web/tests/unit/anniversaries.spec.ts`, 12 new; an existing fixture now carries
a real `updated_at`).**
- A replay returning mutation A and current pin B keeps B selected (list and detail), and
  both sends use the same key.
- A replay returning a cleared current pin leaves nothing selected.
- Eight malformed current pins keep the action unknown with its original key and target:
  missing, not an object, numeric or empty target, `seq` below the mutation, fractional or
  missing `seq`, non-string `updated_at`. A later 403 retry stays unknown; only a valid
  same-key answer resolves it.
- A mutation for another target or user stays unknown.
- A late replay after an account switch is discarded.

**Verification (Claude Code, installed local binaries).**
- `vitest run tests/unit/anniversaries.spec.ts`: exit 0, 35 tests.
- `vue-tsc --noEmit -p tsconfig.json`: exit 0. `tsc --noEmit -p tsconfig.node.json`: exit 0.
- Not rerun by instruction: backend tests, SQL scripts and the full web suite.
- Not done: checking that the new tests fail against the previous code.

No backend, contract, schema, grant or migration change. 0018 is unchanged (SHA-256
`58137f310a682ff41c969ccb7e3608fa1b44f3a657aa83b14b6613b61b532f9f`) and frozen on usapp_test.
Nothing was deployed; main, the served dist and Git are untouched.
