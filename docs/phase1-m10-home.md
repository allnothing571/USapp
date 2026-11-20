# Phase 1 M10: Home and bottom navigation

Status (2026-10-26): owner-authorized implementation, tests and documentation by
Claude Code; review blockers repaired (Sections 6 and 7). Implemented in the web app
only; web unit tests and typecheck pass. Frontend web published 2026-10-26 by Claude
Code on owner authorization (Section 8). Browser/device/PWA acceptance has not been
performed; Phase 1 is not formally accepted. No backend, contract, migration (0000-0016 frozen) or
grant change was needed.

## 1. Approved scope (D15, D16, C1, C2)

- Four-quadrant home: top-left own nickname and own flower/sunshine balances;
  bottom-left today's actionable tasks with a link to the task page; top-right and
  bottom-right neutral "later" cards. No fake data, partner balances, avatars,
  anniversary or messaging.
- In-app pending indicators derived from existing data (C2; not notifications),
  split into "needs me" and "waiting for the partner", linking to Tasks or Rewards.
- Bottom navigation Home / Tasks / Rewards / Me (D16).
- Me page (`/me`): the former home's account, nickname editing, space, member slots,
  available invitations and logout, unchanged in behaviour.

## 2. Behaviour and decisions

**Data sources.** Wallet balances only from `GET /api/v1/wallet` (existing wallet
store). Today's tasks from `GET /api/v1/tasks?view=open` only; task-change
proposals from `view=open` and `view=history` (undecided changes on completed or
expired tasks stay rejectable/withdrawable); redemptions from
`GET /api/v1/rewards/redemptions`. No detail requests per item.

**Today's tasks** (`isTodayTask`, Beijing date of the read time `asOf`): assigned to
me, server status `open`, and applicable today:
- one-off: no deadline, or deadline instant still in the future;
- daily: `period_date` equals today;
- weekly: `period_date <= today <= deadline.local_date`;
- custom: `start_date <= today <= deadline.local_date`.
Upcoming, expired, completed, cancelled and superseded rows are excluded by status;
stale periods are excluded by date even if a cached row is still open.

**Rollover and return.** Home re-reads wallet, tasks and redemptions at the next
Beijing midnight (`nextDailyRefreshDelay`, rescheduled), on `visibilitychange`
(when visible), `focus` and `online`, on mount, and on a user change. Reads only.

**Pending roles** (`pendingSummary`):

| Category | Needs me | Waiting |
|---|---|---|
| Task change (`pending_change`) | proposer is the partner | proposer is me |
| Redemption request (`pending_confirmation`) | I am the creator | I am the redeemer |
| Cancellation (`pending_cancellation` on `pending_fulfillment`) | proposed by the partner | proposed by me |
| Fulfilment (`pending_fulfillment`) | I am the creator | I am the redeemer |

A paid redemption with a pending cancellation counts in both its fulfilment and its
cancellation row (fulfilment may still win, M9). Task-change proposals are counted
once per proposal id, across open and history rows and across recurring periods
that share one public root id (rows are kept per `id:period_date`).

**Bounded pagination.** Each source (open, history, redemptions) is a separate
scan with its own completeness and error state, and follows cursors up to 5 pages of 100
(`HOME_MAX_PAGES`, `HOME_PAGE_LIMIT`), de-duplicating ids. If a next cursor remains,
the scan is incomplete: counts read "至少 N", an explicit incomplete note is shown,
and neither "no pending items" nor "no tasks today" is shown. Any failed page makes
that source an error (partial items discarded); a failed source never shows zero.
Cursor exhaustion is not source coverage: the server's history view covers only the
last 30 days. Task-change counts therefore read "已找到 N" (both scans exhausted) or
"至少 N" (a scan hit the page bound) and are always accompanied by a visible scope note
(open tasks plus the last 30 days of history). They are never presented as exact global
totals. A failed history scan hides task-change counts and shows its own error.
Redemption counts are exact once their own list is exhausted (it covers all of the
caller's redemptions). An empty result is only ever the scoped "在可查看的范围内没有找到待处理事项
（任务变更只含进行中与最近 30 天的任务）", shown when all three scans are exhausted.
There is no unqualified "没有待处理事项".

**Session isolation and separation.** The new `home` store holds home read state
separately from the tasks and rewards stores, so home refreshes never touch their
lists, pagination, unknown actions or idempotency keys. Every scan checks its
request sequence (newer wins), the session generation and the starting user before
applying success or failure. The session store resets the home store at the same
four boundaries as wallet/tasks/rewards (bootstrap user change or anonymous,
`resetBusinessState`, `refreshMe` and nickname-update user change). No browser
persistence, writes, automatic retries or offline queue.

**Navigation.** `BottomNav` renders only for an authenticated member with a space on
`home`, `tasks`, `rewards` or `me`; RouterLink marks the exact current page with
`aria-current="page"`. It is the last element of the app shell and `position:
sticky; bottom: 0`, so it never covers the end of page content; it adds the
bottom safe-area inset. Offline banner, update notice and install prompt are
unchanged. `/me` uses the existing global authentication and space guards.

## 3. Files

- New: `apps/web/src/stores/home.ts`, `apps/web/src/views/MeView.vue`,
  `apps/web/src/components/BottomNav.vue`, `apps/web/tests/unit/home.spec.ts`,
  `apps/web/tests/unit/me-view.spec.ts`, `apps/web/tests/unit/bottom-nav.spec.ts`,
  this document.
- Modified: `apps/web/src/views/HomeView.vue` (rewritten as the four-quadrant home;
  account/space/invite/logout moved to MeView), `apps/web/src/App.vue` (BottomNav),
  `apps/web/src/router/index.ts` (`/me`), `apps/web/src/stores/session.ts` (home
  reset at the four boundaries), `apps/web/src/styles/main.css` (nav, grid),
  `apps/web/tests/unit/home-wallet.spec.ts` (same three wallet protections on the
  new layout; refresh is now the page-level button).
- Status links: `CLAUDE.md`, `docs/claude-code-handoff.md`, `docs/phase1-plan.md`,
  `README.md`.

## 4. Verification (Claude Code, 2026-10-26)

| Command (apps/web) | Result |
|---|---|
| `pnpm run typecheck` after source changes | exit 0, 4.2 s |
| `vitest run` home, home-wallet, me-view, bottom-nav | exit 0, 29 tests, 7 s |
| `pnpm run test:unit` | exit 0, 12.2 s: 20 files / 186 tests (previously 17 / 160) |
| `pnpm run typecheck` | exit 0, 4.4 s |
| `vitest run` bottom-nav after a test-only fix | exit 0, 8 tests, no stderr |
| Final `pnpm run test:unit` | exit 0, 11.6 s: 20 files / 186 tests, no stderr |
| Final `pnpm run typecheck` | exit 0, 4.4 s |

The only issue: the `/me` guard test using the real router logged jsdom's
"window.scrollTo not implemented" (router scrollBehavior) on stderr while passing;
`scrollTo` is now stubbed for that test until navigation settles.

Covered: own balances with independent loading/error (no numbers on failure,
retry, refresh-while-offline clears old numbers); today's inclusion/exclusion table,
Beijing date and midnight re-read showing only the new daily period; foreground
re-read; role split for all four categories; full cursor follow with exact counts;
5-page bound with "至少" and incomplete notes (no "none"); failed source shows an
error not zero while other cards render; failing page discards partial items; late
success/failure discarded after account switch and logout; newer read wins;
nickname shown and home data cleared on account switch; unknown completion and
redemption actions keep their exact keys/state across home refreshes and task views
are untouched; Me nickname edit, local rejection, invitations, logout; nav links,
single `aria-current`, hidden for anonymous / no-space / login / space-create and
after logout; `/me` guards; app shell keeps update notice, offline banner and nav
order.

Not run: `pnpm build` (dist is bind-mounted), backend tests (no backend change),
browser, device or PWA checks, any database/container/main operation.

## 5. Limitations and release prerequisites

- The server's history view covers the last 30 days; an undecided proposal on a task
  that completed or expired earlier than that is not returned by any list API and is
  therefore not counted (no backend change was made).
- Cursor pages are not a snapshot; items can move between pages during a scan.
  Counts above 500 records per source are reported as lower bounds.
- Indicators link to the Tasks/Rewards pages, not to individual items.
- Sticky navigation, safe-area spacing, focus outline and the install prompt above
  the navigation were not checked in a real browser or device.
- Release: frontend only. Back up the currently served web (`caddy:/srv/web`), run
  `corepack.cmd pnpm run build` in `apps/web` (bind-mounted dist), verify Caddy
  index.html/sw.js hashes, then browser/PWA acceptance (navigation, safe areas,
  update prompt, today's tasks across a Beijing midnight, pending links).

## 6. Review repairs (Claude Code, 2026-10-26)

**Blocker 1: task-change coverage.** The home store gained a separate bounded
`history` scan (`listTasks('history')`, 5 x 100) with its own sequence, error and
completeness. Scans key task rows by `id:period_date`. `pendingSummary` de-duplicates
proposals by `pending_change.id`. HomeView combines open + history for task-change
counts (error if either failed, exact only if both complete) and keeps today's tasks
on `view=open`. A new "历史任务读取失败" error and the incomplete note cover history.

**Blocker 2: identity isolation.**
- `stores/stale.ts`: `StaleSessionError`, thrown instead of applying an old result.
- Session store: an identity generation, bumped by a shared `resetIdentityScoped()`
  (space, wallet, tasks, rewards, home) on every identity change. That covers
  bootstrap to anonymous or another user, `resetBusinessState`, and a user change in
  `refreshMe`/`updateDisplayName`, so old invitations are cleared on account changes.
  `updateDisplayName` and `refreshMe` capture identity first, re-check it after the
  awaited CSRF bootstrap (before sending) and before applying success or failure.
  `logout()` returns whether it cleared the session it started from; a completion
  after the identity changed does not reset the newer session.
- Space store: a generation bumped by `reset()`. `run()` discards late success and
  failure (no state, `errorCode` or `loading` change) by throwing `StaleSessionError`,
  so `loadCurrent`, `refreshInvites` and other reads cannot repopulate cleared or
  new-account state.
- MeView: an epoch bumped on identity change and unmount. An identity change clears
  the draft, errors and in-flight flags and re-reads space/invitations. Post-await
  updates run only for the current epoch, `StaleSessionError` is ignored, and the
  page navigates to login only when `logout()` reports it cleared the session.

Files: `apps/web/src/stores/home.ts`, `src/views/HomeView.vue`,
`src/stores/session.ts`, `src/stores/space.ts`, `src/stores/stale.ts` (new),
`src/views/MeView.vue`, `tests/unit/home.spec.ts` (per-view task mocks, 4 new cases),
`tests/unit/me-session.spec.ts` (new, 19 cases), `tests/unit/session-store.spec.ts`
(logout now resolves `true` instead of `undefined`; the never-rejects and
clears-locally protection is unchanged).

| Command (apps/web) | Result |
|---|---|
| `pnpm run typecheck` after source repairs | exit 0 |
| `vitest run` home, me-session, me-view, wallet-session, session-store | exit 1: 55/56. The only failure was the session-store logout assertion `resolves.toBeUndefined()` against the new boolean result; assertion updated. |
| `pnpm run test:unit` | exit 0, 12 s: 21 files / 209 tests (was 20 / 186), no stderr |
| `pnpm run typecheck` | exit 0, 4.2 s |

New coverage: an empty open list plus a history task with an undecided change
(counted exactly; a history row never appears in today's tasks); one proposal
across open and two history periods counted once; an incomplete history scan
gives "至少" and no "none"; a failed history scan gives an error, no
task-change count and no "none". Nickname request late success and failure across
an account switch and a logout reject with `StaleSessionError` and do not restore
the old user. An identity change or bootstrap failure while waiting for CSRF never
sends the old draft, and the same-user CSRF path still updates. Late space and
invitation success and failure after a switch or logout leave invitations empty,
`errorCode` null and `loading` false. An account switch clears old invitations.
Me drafts and errors clear on an identity change. A late nickname success or failure
shows nothing on the new account. An old space-read failure is not shown. An old
logout success or failure neither clears nor navigates a newer session, and a
current logout still clears and navigates.

Not run: `pnpm build`, backend tests (no backend change), browser/device/PWA checks.

## 7. Final review repairs (Claude Code, 2026-10-26)

**Coverage vs. pagination.** HomeView now labels task-change counts "已找到 N" when
both task scans were exhausted ("至少 N" otherwise). It always shows
`pending-task-scope` ("任务变更只检查进行中的任务与最近 30 天的历史任务…"). The only
empty-state text is the scoped "在可查看的范围内没有找到待处理事项（…最近 30 天…）".
Redemption counts stay exact when their own scan is exhausted. The bounded
pagination, "至少", incomplete note and per-source errors are unchanged. No backend
endpoint was added. Comments in `stores/home.ts` and `HomeView.vue` state that
`complete` means cursor exhaustion only.

**Bootstrap guard.** In `stores/session.ts`, `bootstrap()` records the in-flight
request with the identity that started it and a request sequence.
- A caller reuses the in-flight request only while that identity is still current.
  Otherwise a fresh bootstrap starts.
- `runBootstrap(seq, token)` checks the sequence and identity before every side
  effect: applying the snapshot, the fail-closed `resetBusinessState()`, and setting
  `bootstrapped`.
- The `finally` clears `inFlight` only if it still holds this request.

A stale bootstrap's success or failure therefore cannot restore A, clear a newer
session, change `bootstrapped` or drop a newer in-flight bootstrap. Normal bootstrap,
concurrent callers sharing one request, fail-closed failure for the current identity
and the same-user CSRF refresh are unchanged.

Files: `apps/web/src/stores/session.ts`, `src/views/HomeView.vue`, `src/stores/home.ts`
(comment), `tests/unit/home.spec.ts`, `tests/unit/me-session.spec.ts`.

New and updated tests:
- Bootstrap race (4 cases). A starts `updateDisplayName` with no CSRF token. Before
  the old bootstrap settles, B is applied independently and B's invitations load, or
  logout completes. Then the old bootstrap resolves with A's snapshot or rejects.
  Status, user, session and space-store space, invitations, the session and client
  CSRF token and `bootstrapped` all equal their values before settlement.
  `updateMe` is never called.
- A stale bootstrap is not reused after logout. Its completion does not clear the
  newer in-flight bootstrap, which a later caller joins without a third request.
- Normal concurrent bootstrap still makes one request.
- Home: exhausted task scans give the "已找到 1" badge with the scope note and no
  "none". Scoped empty text and scope note assertions were added. The exact
  redemption count has no "已找到" qualifier. Historical/periodic cases now expect
  "已找到 1".

| Command (apps/web) | Result |
|---|---|
| `pnpm run typecheck` after source changes | exit 0 |
| `vitest run` home, me-session, session-store, wallet-session | exit 1, 57/59. The 2 new bootstrap tests compared promises by identity; Pinia wraps action promises. They now assert request counts and settlement order instead. |
| same, rerun | exit 0, 59/59 |
| `pnpm run test:unit` | exit 0, 12.5 s: 21 files / 216 tests (was 209), no stderr |
| `pnpm run typecheck` | exit 0, 4.5 s |

Not run: `pnpm build`, backend tests, browser/device/PWA checks.

## 8. Frontend rollout (Claude Code, 2026-10-26)

Owner-authorized frontend-only release: a wording correction, verification, a
served-web backup, the production web build and release documents. There was no
database access, migration, InitRoles, backend build or restart, contract or grant
change, Git publication, cleanup or business transaction.

- Wording correction. The scope note in `apps/web/src/views/HomeView.vue` no longer
  says "请到任务页查看" for changes older than the 30-day history window, because the
  Tasks page cannot list them either. It now says they "无法通过现有列表查到，不会在这里显示",
  keeping the limitation explicit. `apps/web/tests/unit/home.spec.ts` asserts the new
  wording and the absence of the redirect. Affected home tests (home, home-wallet):
  exit 0, 22 tests, 5.3 s. `pnpm run typecheck`: exit 0, 4.5 s. The full suite was not
  repeated because the change is text only (last full run: 216 tests, Section 7).
- Read-only preflight, exit 0. api `running (healthy)`, caddy `running`, postgres
  `running (healthy)`. The API container
  `d4a7b3c8e0baab02ab7255979d8881957e99b660e850dab35a22e6714ec7367f` and image
  `sha256:a5e17286c6f82161244924bdf67a665f3fead08eb665909a72f19c47d6b480b9` were
  unchanged since the M9 rollout. The served index.html/sw.js equalled the M9 web build.
  No unexpected changes.
- Served-web backup, exit 0, 1.6 s: `backups/before-m10-web-20261026_222350/`, a new
  directory with an overwrite guard.
  - `web-dist/` copied with `docker compose cp caddy:/srv/web`: 22 files, 290686
    bytes. index.html
    `9b4238fa0f8b74735afc823c1cd8c4a7895129c3248f140b4836012a7a584427`, sw.js
    `84cfc85bdb037a8304591b3ed7c2807ca9e537dd30665c74bb56b1a196909809`; both
    present and nonempty.
  - `rollback-info.json` (SHA-256
    `4e0b0a8409fbfbac621573588a72e81f88e44144a9de222ffb4a10abd4f11cca`) records the
    copy, hashes and API identity.
- Build and publication. `corepack.cmd pnpm run build` in `apps/web` exit 0, 9.29 s
  (vue-tsc, tsc, Vite, PWA). Precache 22 entries / 226.28 KiB. dist 23 files /
  303642 bytes. The bind-mounted dist is served by Caddy, and the served hashes equal
  the local dist:
  - index.html `515cf8445fa8fbb68498976592ee689f3f254403048736cbf03e3f457d12521e`
  - sw.js `453bbbcdcae4c0e24178b6ffa7c4127ee4a3f0489420b279733539ef0399ce6d`
- Final status, exit 0. api `running (healthy)`, same container and image; caddy
  `running`; postgres `running (healthy)`. Nothing was restarted.

Distinction: implementation and automated verification are complete (Sections 4, 6
and 7), and web publication succeeded (this section). Browser/device acceptance was
not performed. The owner will check navigation and safe areas, nickname, logout and
invitation flows, pending links and scope labels, home refresh and date rollover, and
installed-app update behaviour. Rollback, if needed, is to copy the saved `web-dist/`
back into the bind-mounted `apps/web/dist`; this was not performed or exercised. No
database or API change exists to undo. All artifacts are retained.
