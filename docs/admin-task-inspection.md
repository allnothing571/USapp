# Admin task inspection (read-only)

Claude Code, 2026-11-11/12. Local admin, read-only inspection of the tasks of one selected couple/space: a paginated list and one task's detail with its history. Contract 0.48.0 (additive). No migration, grant, dependency or public-API change; migrations 0000-0047 frozen and unchanged, 0048 unreserved.

Out of scope, and absent from the server and the page: creation, edits, completion, progress posting, approval, closure, recurrence generation, reminders, asset changes, notifications and export.

## 1. Boundary

- `GET /api/admin/v1/couples/{coupleId}/tasks?kind=&limit=1..100&cursor=`
- `GET /api/admin/v1/couples/{coupleId}/tasks/{taskId}`

Both routes use `AdminFilter`, in this order: peer CIDR, the admin surface header (missing or wrong value: 403), then the independent admin session (absent: 401). The controller also requires the admin identity that the filter attached. An ordinary user cookie never authorizes, even with a forged surface header. Both listeners register the routes; the live test drives both.

The public `/api/v1/tasks` reads are unchanged: still caller-scoped, still with the 30-day history window, and still with no couple selector.

**Ids.** Path ids are lowercased, then validated as UUIDs; a malformed id answers 400 `VALIDATION_ERROR`.

**Subject states.**

| Case | Answer |
|---|---|
| Unknown couple (list or detail) | 404 `SPACE_NOT_FOUND` |
| Existing couple with no tasks | 200, `items: []`, `next_cursor: null` |
| Task not in the selected couple (another couple's, or nonexistent) | 404 `TASK_NOT_FOUND`, identical for both, so the route cannot probe other couples |

An archived couple is still readable; its `status` is reported.

## 2. What is listed

Stored `tasks` rows of the couple, of three kinds:

| `kind` | Rows | `schedule_type` reported |
|---|---|---|
| `task` | roots with `schedule_type` `one_off` or `custom` | their own |
| `template` | `daily`/`weekly` roots | their own |
| `period` | stored periods (`recurrence_id` set) | the template's (as the public read does) |

A period in which nobody ever acted has no stored row. The public read synthesizes such a period from its template; this admin read does not invent it. The template row is listed instead. Old expired, completed and closed rows are all included: there is no time window.

**Status.** Status comes from the same SQL expression as the public read. `TaskStateSql.h` holds `USAPP_TASK_ROW_CLOSURE` and `USAPP_TASK_STATUS_CASE`, moved verbatim out of `TaskService.cc`, which now uses the header; the generated SQL text is unchanged. The order of precedence is completed > cancelled/superseded (row closure) > expired > upcoming > open.

- For a template, `open` means its schedule is not closed.
- A period that ended before its template was closed keeps its own state. The detail reports this as `closure.applies_to_this_task: false`.
- The DB test checks that the admin status equals the public `getTask` status for every row the public read still shows.

**Progress count.** For a single task, 1 if completed, otherwise 0. For a count task, the highest counted event.

## 3. Fields (allowlist)

**List item:**
- identity: `id`, `kind`, `schedule_type`, `template_id`, `period_date`, `title`, `creator_id`, `assignee_id`;
- rewards and progress: `reward_flowers`, `reward_sunshine`, `completion_mode`, `target_count`, `progress_count`, `status`;
- dates: `start_date`, `deadline_at`, `created_at`, `completed_at`.

**Detail:**
- `task`: the list fields, plus `description`, `timezone`, `deadline_local_date` and `deadline_local_time`;
- `completion`: `completed_by`, `completed_at`, and the reward amounts actually credited;
- `progress`: `total`, plus the newest 200 events by `progress_count` (each: `id`, `user_id`, `progress_count`, `target_count`, `recorded_at`, `completes_task`);
- `reaction`: one per completion;
- `changes`:
  - `owner_task_id`, the root the proposals belong to; for a period, its template;
  - `total`;
  - the newest 100 proposals, each with its kind, proposer, time, terms (null for a cancellation) and decision (outcome, decider, time, successor);
- `closure` of the root: reason, time, proposal, successor, and `applies_to_this_task`;
- `replaces`: the task whose approved edit produced this root;
- `read_at`.

**Exact amounts.** Rewards are decimal strings (`AdminDecimal`, as in the admin wallet). Counts are JSON integers within the 32-bit column range, so they are exact in JavaScript.

**Excluded:**
- idempotency keys and request or create fingerprints;
- wallet operation ids, internal lock fields and raw request JSON;
- notifications, reminder rules and user profiles. People are shown by id.

**References.** Every referenced task id (template, successor, replaced task) is a row of the same couple: the foreign keys `(…, couple_id)` guarantee it. The page links only those ids, inside the same couple's route. Nothing else is linked.

## 4. Queries, consistency and pagination

Each answer is one SQL statement, so one snapshot.

**List.** Space existence and the page share one snapshot. The page is keyset over `(created_at, id)` descending, fetching `limit + 1` rows; the cursor key is `created_at` in exact integer microseconds (`extract(epoch …)` is numeric on PostgreSQL 17). The cursor format is `t1.<all|task|template|period>.<micros>.<taskId>.<coupleId>`, valid only for the same couple and filter. A malformed or mismatched cursor is 400.

**Pages are separate reads.** There is no cross-page snapshot. A row created after the first page is newer than the cursor, so it never shows up on later pages, and nothing repeats; the DB test checks this.

**Couple filter.** Every task and child query is filtered by the selected couple: completions, progress, reactions, proposals, decisions and closures by `couple_id = $1` plus the task or its root. Task status subqueries go through the same task ids.

**Detail.** A `tk` CTE (`id = $2 AND couple_id = $1`) gates every part. An empty `tk` yields `TASK_NOT_FOUND`.

**Index.** The list uses the existing `tasks_couple_deadline_idx` leading column, then sorts in memory. There is no new index (no migration). This is acceptable for admin-sized couples and is recorded as a limit.

## 5. Admin UI

**Entry and route.**
- `CouplesView` gains a "只读查看" link per couple.
- One protected route, `/couples/:coupleId/tasks/:taskId?` (name `couple-tasks`, `requiresAuth`). It is a single route record, so opening a task keeps the list and its page position mounted.

**Store `couple-tasks` (memory only):**
- **Identities:**
  - list = admin session (`epoch` and admin id) + couple + kind filter;
  - detail = admin session + couple + task;
  - list and detail have separate sequence counters.
- **What a change invalidates:**
  - filter change: list reads only;
  - task selection: detail reads only. Neither strands the other's loading indicator, which avoids the shared-generation bug found in the wallet store;
  - couple, session or unmount change: both.
- **Authorization loss:** a 401/403 from any current read clears the private data, settles both loaders, shows the authorization message and invalidates every read in flight, so a delayed 200 cannot repopulate the page. Recovery is only by an explicit refresh or a new selection.
- **404s:** a 404 space is reported as an unknown space. A 404 `TASK_NOT_FOUND` keeps the list and reports that the task is not in this space.
- **No persistence:** no localStorage, IndexedDB or caching.

**View:**
- labelled read-only, with loading, empty, error and retry states;
- only filter, refresh and pager controls; no mutation button, enabled or disabled;
- the per-page and per-detail snapshot is explained on the page.

## 6. Files

**Backend:**
- `services/api/src/services/TaskStateSql.h` (new: shared status SQL);
- `TaskService.cc`: uses the header; the produced SQL is unchanged;
- `AdminTaskService.{h,cc}` and `controllers/AdminTaskController.cc` (new);
- `services/api/CMakeLists.txt`: one controller line.

**Tests:**
- `services/api/tests/AdminTaskDbTests.h` (new, CTest target `admin_task_db`, `TIMEOUT 30`, outside `db_gated`);
- `AdminTaskLiveApiTest.h` (new, runs in the live step);
- `DbGatedTest.cc`: dispatch and includes;
- `tests/CMakeLists.txt`: the new target;
- `StaticCheckTest.cc`: a new admin-task case, and three version assertions moved to 0.48.0.

**Contract:** `contracts/openapi.yaml` 0.48.0: two paths and the schemas `AdminTaskSpace`, `AdminTaskSummary`, `AdminTaskPage`, `AdminTaskProposal`, `AdminTaskDetail`.

**Admin app:**
- `apps/admin/src/api/tasks.ts`, `stores/couple-tasks.ts` and `views/CoupleTasksView.vue` (all new);
- `router/index.ts` and `views/CouplesView.vue` (the entry link);
- `tests/unit/couple-tasks.spec.ts` (new, 13 tests).

**Harness:** `tools/export-verify.ps1`.
- Adds `Suite 'ADMIN_TASK' 120` after `ADMIN_WALLET`.
- A failure of the known, unrelated `EXPORT_CONNECTION_LOSS` mode is now recorded instead of aborting the run. `EXPORT_LARGE`, `EXPORT_CLOSURE` and the migration-file comparison still run, and the run then ends with "Harness NOT green".
- No CTest timeout changed.

## 7. Verification record (Claude Code, 2026-11-12)

| Check | Result |
|---|---|
| Focused admin tests (`couple-tasks.spec.ts`) | **13/13 passed** on the first run |
| Mutation check, to show the race tests discriminate (store restored byte-for-byte afterwards) | (a) a filter change bumps a shared generation: **1 failed**, the "filter change does not strand a pending detail" test; (b) authorization loss does not invalidate reads in flight: **3 failed**, the list-first, detail-first and data-shown interleavings |
| Full admin suite (run `Claude outputs/at-admin-6008f054c4`) | **94 tests in 6 files** (81 earlier + 13 new) |
| Both admin typechecks (`vue-tsc` app and node) | passed |
| Isolated production build to a temporary directory | passed |
| `apps/admin/dist` and `apps/web/dist` | unchanged: 20 and 45 files, identical hashes and timestamps |
| Strict YAML parse (`Claude outputs/at-yaml-check.py`, Compose loader with a duplicate-key negative control) | passed, structure only (not OpenAPI semantics): version 0.48.0, 109 paths, 126 operations, 200 refs resolved, both new operations and five schemas present, `additionalProperties: false` on each, cursor pattern positive and negative cases, public task reads without a couple selector, and no key, fingerprint or operation-id field in the new schemas |

**Backend NOT RUN.** None of these ran: build, `admin_task_db`, the live HTTP case on both listeners, full CTest, read footprint, privilege deviations or migration hashes.

**Why.** Docker Desktop 4.93.0 crashed at startup. Its secrets engine could not rename a stale Windows AF_UNIX socket, `%LOCALAPPDATA%\docker-secrets-engine\engine.sock` (a 0-byte reparse point from 2026-11-11). The crash dialog offered only Quit and factory reset; factory reset was not used.

**Repair attempts.** With the owner's approval to delete only that file, Claude Code tried:
- .NET `File.Delete`, `cmd del /f /a`, `fsutil reparsepoint delete` and `Remove-Item -Force`: each failed with Windows error 1920 ("The file cannot be accessed by the system");
- renaming the folder aside: access denied;
- `wsl --shutdown`: refused by the session's permission classifier.

No process held the file. Nothing was deleted or renamed, and no container, image or volume was touched. The verification requires the owner to clear that socket (a reboot normally does) and then run `tools/export-verify.ps1`.

**Statically reviewed only (not compiled or run):**
- the backend SQL and C++;
- the DB and live tests, after replacing two macros the project framework does not have (`CAPTURE`, `FAIL_CHECK`) with `expectThat`.

**Not verified:** browser rendering, a real admin login, production. NOT DEPLOYED.

## 8. Backend verification completed (Codex, 2026-11-12)

Codex took over the existing implementation at the owner's direction. Docker was reachable; no Docker repair or project-code change was needed. This record supersedes Section 7's **Backend NOT RUN** status, while preserving its historical failure report.

- Disposable run: `usapp-e1-a5abe99599`, exact container ownership checks, internal network, no published ports, generated credentials; no shared database, cloud or production access.
- Build passed. Targeted `ADMIN_TASK`: **209 assertions / 2 cases**, no failures; `ADMIN_WALLET`: 112; `EXPORT`: 444; achievement: 22; avatar catalog/purchase/loadout: 157/114/85.
- Real API binary: **687 assertions**, including the new admin-task block on both listeners; no failures. This is automated HTTP evidence, not browser or real-login acceptance.
- Full CTest: **22/22 passed in 99.46 s**; `db_gated` 16.08 s, `admin_task_db` 0.21 s. No timeout changed.
- 48 migration checksums matched; all 48 migration files remained unchanged. Eraser violations and privilege deviations after apply, repeat initialization and CTest: 0. Early/reversed erasures and wallet ledger-chain breaks: 0.
- Additional E1 modes: connection loss 8 assertions (release at 1,048 ms, `session_replaced`), large export 6 (8,500 rows accepted; input and row caps refused), closure 23; all passed. This single successful connection-loss run **does not fix or explain** the previous >60 s reconnect stall. Closure mode intentionally left the disposable database CLOSED.
- Codex reran the full admin suite: **94 tests / 6 files**, both typechecks and an isolated Vite production build passed. The build output went to a temporary directory; existing `dist` outputs were not targeted. Strict YAML parsing and 200 reference resolutions passed; this does not validate OpenAPI semantics.
- Generated test-credential matches across the run's logs: 0; CTest crash markers: 0. Both run containers were confirmed `exited`; containers, network and work folder were retained.

Evidence: `Claude outputs/at-verify-a5abe99599/` (CTest, targeted suites, server logs and migration-hash baseline). Original work folder: `%TEMP%/usapp-e1-a5abe99599`. Admin isolated build: sandbox temporary directory `usapp-at-admin-build-28d2f26e71`.

No source correction was necessary in this verification pass. Browser rendering, real admin login, production volumes and deployment remain unverified. Historical clock-guard intermittents and the DbClient teardown crash remain unresolved. NOT DEPLOYED; nothing committed.
