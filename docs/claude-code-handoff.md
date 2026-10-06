# USapp: Claude Code Implementation / Codex Acceptance Handoff

Updated: 2026-10-06, Asia/Shanghai. Later the same day: recorded owner-run acceptance results for the AdminFilter authentication fix (Section 2) and the HomeView member-label fix (Section 7, item 3). Subsequently: recorded the owner-made initial local Git baseline commit `b9cd0ea` (Section 6, "Git Baseline"). This documentation update itself is not yet committed.
Project root: `D:\JYJ\USapp`. Environment: Windows PowerShell.

## 1. Responsibilities, Authorization, and Current Objective

- Claude Code implements subsequent code changes, adds necessary regression tests, and revises changes based on acceptance feedback.
- Codex checks assumptions, reviews actual changes, designs acceptance checks, and guides the owner through validation one step at a time.
- **The owner manually runs builds, tests, database operations, container operations, installations, browser checks, and Git operations.** Neither Claude Code nor Codex should execute these actions unless the owner explicitly grants additional authorization.
- Commands in this document are references for the owner. Their presence does not authorize Claude Code to execute them.
- The owner has delegated subsequent implementation to Claude Code. Do not ask again for permission to implement an explicitly assigned fix. Explain and obtain authorization before making out-of-scope architectural changes, database changes, dependency upgrades, or feature additions.
- Preserve existing work. Do not reset, overwrite, or revert changes unrelated to the assigned fix. The owner initialized a local repository on `main` and made the initial baseline commit `b9cd0ea` (Section 6, "Git Baseline"). Changes made after that commit, including later updates to this document, are uncommitted unless the owner reports a new commit; do not claim otherwise.
- Do not read the real `.env`. Do not disclose secrets, passwords, session cookies, plaintext verification codes, or invitation codes. Do not inspect database backups to obtain secrets.
- Deliver actual file differences, the reasoning behind the fix, and its verification status. Distinguish "files modified," "statically reviewed," "build passed," "tests passed," and "real workflow verified." Another tool's completion report is not test evidence.
- Give the owner one acceptance step at a time and wait for its result before proceeding. Retain essential errors rather than pasting lengthy logs.
- All content handed to Claude Code must be written in English. Codex continues to communicate with the owner in Chinese.

**Current objective: complete Phase 0 acceptance. The reproduced administrator-list authentication defect has been fixed and passed the owner-run checks recorded in Section 2, and the Phase 0 baseline is committed locally as `b9cd0ea`, but Phase 0 is still not accepted: the open items and verification limits in Section 7 remain, and the owner has not explicitly accepted Phase 0. A baseline commit is not Phase 0 acceptance. Do not proceed to Phase 1 until the owner explicitly accepts Phase 0.**

## 2. Resolved: Unauthenticated Access to Administrator Lists

Status: fixed by Claude Code and checked by the owner under Codex's guidance (results under "Acceptance Results" below). The evidence, root cause, and requirements are retained as history and as the behavior later changes must preserve. The checks were targeted, not an exhaustive security verification.

### Original Evidence (Before the Fix)

The owner ran the following request without a login cookie:

```powershell
curl.exe -q -i --ssl-revoke-best-effort --cacert .\deploy\caddy\certs\root.crt https://localhost:8444/api/admin/v1/users
```

Actual response before the fix: `HTTP/1.1 200 OK`, including both users and their email addresses. Personal information is intentionally omitted here.
Expected response: `401 UNAUTHENTICATED`, without returning the user list. After the fix, this is the observed response (see "Acceptance Results").

The reproduction used the local administrator entry point. There is currently no evidence that these records are accessible through the public entry point. A loopback restriction does not replace administrator authentication.

### Root Cause in the Source (Before the Fix)

- In `services/api/src/http/AdminFilter.cc`, approximately lines 126-140, `continueWithoutSession()` rejects only state-changing requests other than login. It allows all read requests to continue.
- Both the missing-cookie path and the missing/invalid-session path call this function, approximately at lines 190 and 200.
- In `services/api/src/controllers/AdminController.cc`, `listUsers()`, `listCouples()`, and `listAuditLogs()`, approximately at lines 218, 265, and 309, have no additional administrator authentication guard.
- All three lists register `AdminFilter`. The defect is the filter's authentication policy, not a missing filter registration.
- `contracts/openapi.yaml` already specifies authentication and unauthenticated responses for these lists. Do not change the contract to justify the defective behavior.

Line numbers are navigation aids from the pre-fix snapshot. Locate the actual functions before editing.

### Requirements (Implemented; Must Remain True)

1. Use an exact method-and-path allowlist for unauthenticated administrator requests. Allow only:

   | Method | Path | Purpose |
   | --- | --- | --- |
   | GET | `/api/admin/v1/health` | Health probe |
   | GET | `/api/admin/v1/session` | Unauthenticated session bootstrap |
   | POST | `/api/admin/v1/sessions` | Login |

2. Every other administrator request requires a valid administrator session. Missing cookies and invalid, expired, or revoked sessions must produce `401 UNAUTHENTICATED`, terminate processing immediately, and never reach the controller. Deny by default; do not use prefix matching.
3. The allowlist must not bypass existing peer-address checks, the administrator-entry marker, or login Origin and custom-header validation.
4. Preserve login protection and session-bound CSRF (Cross-Site Request Forgery) token checks for authenticated write requests. Logout remains a protected write request.
5. Unauthenticated administrator session bootstrap must still return `200`, `authenticated=false`, and an empty `csrf_token`. Do not introduce an administrator pre-authentication session.
6. Update filter comments to explain that protected administrator reads also require authentication. Preserve login, bootstrap, all three lists, and logout.

The scope limits of the original task remain in force for any follow-up: do not use administrator-authentication work to refactor transactions or change migrations, frontend code, deployment configuration, or the interface contract.

### Implemented Fix

- `services/api/src/http/AdminFilter.cc`: added `kAnonymousAdminRoutes`, an exact method-and-path allowlist containing only the three entries above, checked by `allowsAnonymous()` with exact string equality. `continueWithoutSession()` (used for both the missing-cookie path and the invalid/expired/revoked/load-failure path) now rejects every non-allowlisted request with `401 UNAUTHENTICATED` and does not call the filter-chain continuation. The allowlist is evaluated only at the session step, after the peer-address, administrator-entry marker, and state-changing Origin / `X-Requested-By` checks. The valid-session branch and session-bound CSRF checks are unchanged.
- `services/api/src/http/AdminFilter.h`: comment-only update describing default-deny authentication and the allowlist.
- No changes to `AdminController.cc`, `AdminAuthService.*`, `contracts/openapi.yaml`, migrations, deployment files, frontends, or CMake files.

### Required Regression Coverage (Implemented)

- Call the real `AdminFilter::doFilter()`. Verify that requests to all three lists without cookies return 401 and do not invoke the filter-chain continuation callback. Source-string assertions are not a substitute for behavior tests.
- Verify that unauthenticated health and bootstrap requests can pass the filter, while login still requires the exact Origin and custom request header.
- Similar path prefixes or different methods must not bypass the exact allowlist.
- In database-gated tests, use independent test administrators and sessions to verify rejection of invalid, expired, and revoked sessions and continuation for valid sessions. Do not merely simulate an authenticated boolean.
- Use test configuration, do not read the real `.env`, and do not weaken production address restrictions. Restore configuration changed by tests.
- Capture shared promises by value and use bounded waits. On timeout, fail without subsequently blocking on `get()`. Do not capture references to stack objects that may be destroyed before callbacks run.
- Clean up independent fixture records by their identifiers and in dependency order. Report cleanup failures; do not swallow them and claim cleanup succeeded.

Implemented test files (all under `services/api/tests/`):

- `AdminFilterProbe.h` (new, test-only): calls the real `AdminFilter::doFilter()` and records whether the reject callback or the continuation callback fired; callbacks capture shared state by value; waits are bounded and never call `get()` after a timeout. `ScopedAdminConfig` saves, overrides, and restores the administrator CIDR list, administrator Origin, and administrator cookie name; `AppConfig::load()` is never called, so no `.env` is read. Synthetic requests use Drogon's default peer address, and only that exact address is allowlisted during the test.
- `UnitTest.cc`: four new AdminFilter cases without a database: the three lists without a cookie; the allowlist plus login Origin / `X-Requested-By`, missing administrator-entry marker, and non-allowlisted peer checks; similar-path and other-method bypass probes; and fail-closed behavior when a cookie is present but sessions cannot be loaded.
- `DbGatedTest.cc`: new test block 8 using an independent test administrator and directly inserted valid, expired, and revoked sessions plus a never-stored token, evaluated by the real `loadByRawToken`. It covers rejection of invalid/expired/revoked sessions, continuation for the valid session with correct attributes, session-bound CSRF and Origin checks on logout, and rejection of the same token after revocation. Cleanup deletes by identifier in dependency order and fails the test on any cleanup error.

### Acceptance Results (Owner-Run)

The owner ran these checks under Codex's guidance after the fix:

- Test image and runtime image builds succeeded.
- All three CTest targets (`unit`, `db_gated`, `static_check`) passed; `db_gated` ran and was not skipped; exit code 0. Per-assertion counts and timings for this run were not recorded here; do not invent them.
- Anonymous requests to `/api/admin/v1/users`, `/api/admin/v1/couples`, and `/api/admin/v1/audit-logs` returned `401 UNAUTHENTICATED` without list data.
- Anonymous `GET /api/admin/v1/health` and `GET /api/admin/v1/session` returned 200; bootstrap returned `authenticated=false` and an empty `csrf_token`.
- An authenticated administrator could load all three lists.
- After logout, the browser's user-list request returned `UNAUTHENTICATED`.
- The public entry point returned 404 for the administrator user list.
- Login with a wrong Origin returned `403 ORIGIN_NOT_ALLOWED`.
- Login with a missing `X-Requested-By` header returned `403 CSRF_TOKEN_INVALID`.

### Limits of This Evidence

- Invalid, expired, and revoked sessions were exercised by the `db_gated` test against the test database, not by separate live requests against the running service.
- A wrong or missing `X-CSRF-Token` on an authenticated administrator write was covered by tests only; no separate live check was recorded.
- HEAD and OPTIONS requests to administrator routes were checked only at filter level in unit tests. Whether Drogon rewrites HEAD to GET or answers OPTIONS before filters run has not been verified.
- Local address isolation and these targeted checks are not proof of complete authentication enforcement. No exhaustive security review or penetration test has been performed.

## 3. Established Architecture: Do Not Redesign

- User frontend: Vue 3, TypeScript, Vite, and a PWA (Progressive Web App). The administrator frontend is separate.
- Backend: a C++17 / Drogon modular monolith, pinned to Drogon `v1.9.11`; PostgreSQL 17; Caddy reverse proxy; Docker Compose orchestration.
- Scheduling runs inside the C++ process with PostgreSQL transaction-level advisory locks. Locks, scheduler records, and associated business writes must share a transaction connection. Post-commit side effects wait for confirmed commit success. Do not introduce Redis or a separate scheduler.
- The Node.js 24 push helper handles delivery, retries, and invalidation only. It is disabled by default; real push delivery has not been verified in Phase 0.
- Sessions are server-side. Cookies use the `__Host-` prefix, Secure, HttpOnly, Path=/, and no Domain attribute. State-changing requests require exact Origin validation and server-session-bound tokens.
- Ordinary registration uses a runtime-configured email allowlist after canonicalization. It controls creation of new accounts, not business permissions, and must not ban existing accounts removed from the allowlist. Administrator accounts and sessions use separate tables.
- A space holds at most two members, enforced by database slots 1/2, unique space/slot membership, and unique user membership. Both members have equal business permissions.
- The current space timezone is fixed to `Asia/Shanghai`.
- Offline caching stores only the application shell and static resources, not business API responses, private media, or business data. Business write requests must not be queued offline.
- Wallets with immutable ledgers, atomic backend settlement, task versions and periods, mutual approval of reward cancellation, private object-storage uploads, and independent media backups belong to later phases. Do not implement them in this task.

## 4. Environment, Entry Points, and Data State

| Item | Configuration or Previously Verified State |
| --- | --- |
| Working directory | `D:\JYJ\USapp` |
| Node.js | 24.21.0, installed at `D:\NodeJS` |
| Package-manager invocation | `corepack.cmd pnpm`; previously observed version 12.9.1; no directly usable pnpm command |
| Docker | Desktop with Linux containers; services were started by the owner |
| User frontend | `https://localhost:8443/` |
| Administrator frontend | **`https://localhost:8444/admin/`**, including `/admin/`, with hash routing |
| Administrator root URL | `https://localhost:8444/` returns 404 under the current routing |
| Administrator ports | Caddy host binding `127.0.0.1:8444`; backend administrator host binding `127.0.0.1:8081` |
| User entry point | Caddy host port 8443; backend public port 8080 is not directly published to the host |
| Databases | Main `usapp`, dedicated test `usapp_test`, recovery verification `usapp_recovery` |
| Database roles | Maintenance superuser `postgres`; restricted login roles `usapp_api` and `usapp_push` |
| Mail | Runtime development / log mode was confirmed; verification codes are read locally from logs, not delivered to real inboxes |
| Local certificate | `deploy/caddy/certs/root.crt` was exported and imported into the current user's trust store |

The owner has populated `.env` with both real email addresses, database passwords, the session secret, and administrator bootstrap username/password hash. Do not overwrite it or copy the template over an existing `.env`.

Windows curl previously reported an unknown revocation status for the local certificate. Successful checks used `--ssl-revoke-best-effort --cacert`, retaining certificate validation rather than disabling it.

Creating global pnpm shims through Corepack failed because of directory permissions. `corepack.cmd pnpm` works; reinstalling it or elevating privileges is unnecessary. The owner has approved esbuild installation scripts separately for both frontend applications.

## 5. Implemented Fixes and Relevant Files

The following fixes are already present in the working directory. Do not reintroduce the previous errors. They are part of the initial local baseline commit `b9cd0ea` (Section 6, "Git Baseline"); a commit records the files but is not additional runtime verification.

| File / Module | Key Change and Boundary |
| --- | --- |
| `deploy/Dockerfile.api` | Initialize pinned submodules when cloning the fixed Drogon tag; use `BUILD_SHARED_LIBS=ON`; preserve builder / tester / runtime stages and PostgreSQL dependencies |
| `src/db/Db.h`, `src/scheduler/Scheduler.h` | Use Drogon's actual `DbClient.h` for Transaction declarations; do not include the nonexistent Transaction.h |
| `src/db/Db.cc` | Commit through a commit callback and release of the transaction reference; success responses wait for callback success; rollback requests do not confirm completion; destruction of an unsettled scope requests rollback to prevent accidental automatic commit |
| `src/http/AuthFilter.*`, `AdminFilter.*` | Use actual public request-Attributes interfaces; disable automatic creation for manually registered filters. AdminFilter now denies unauthenticated administrator requests by default except an exact three-route allowlist (see Section 2) |
| `src/mailer/Mailer.cc` | SendResult is a namespace-scope type, not a nested Mailer type |
| `src/services/SessionService.cc` | Capture the previous-session identifier; use SQL NULL when no previous session exists instead of casting an empty string to uuid; preserve revocation and creation within the same transaction |
| `src/services/AdminAuthService.cc`, `SessionService.cc`, `AuthService.cc`, `SpaceService.cc` | Add explicit `::integer` to seven time parameters, matching the incoming C++ int width before conversion for make_interval |
| `src/controllers/AdminController.cc` | Use `::integer` for LIMIT and `::bigint` for OFFSET, matching int / long long |
| `src/controllers/AuthController.cc` | Call Mailer after verification-code creation commits successfully; preserve the 202 Accepted response and do not expose plaintext codes in the response |
| `src/main.cc` | Configure RegistrationPolicy after loading configuration and before accepting requests; set stdout line buffering before logging and check for failure. Previously, missing initialization left an empty allowlist, and buffering delayed code logs until restart |
| `src/services/SpaceService.cc` | Use `$4::integer` for invitation max_uses and `$3::integer` for membership slot, matching the incoming int before conversion to smallint |
| `services/api/tests/DbGatedTest.cc` | Add real-service regressions for administrator sessions, time binding, pagination, and invitation creation/acceptance; repeated acceptance must return InviteInvalidOrExpired, not merely any failure that could be an internal error. Test block 8 adds real AdminFilter session checks (Section 2) |
| `services/api/tests/UnitTest.cc`, `AdminFilterProbe.h` | Behavioral AdminFilter regressions through the real `doFilter()` without a database; `AdminFilterProbe.h` is a new test-only helper (Section 2) |
| `apps/web/src/views/HomeView.vue` | Member-slot title shows the vacancy label only when `item.member` is absent; an existing member shows a trimmed `display_name` or a Chinese fallback meaning "Nickname not set". Badges, slot mapping, and membership data unchanged |
| `apps/web/src/api/error-messages.ts`, `space.ts` | Fix type imports and unused imports while preserving public type re-exports and strict type checking |
| `apps/web/vite.config.ts` | Point the custom Service Worker entry to `src/pwa`; build output includes `dist/sw.js` |
| `apps/admin/src/api/client.ts`, `admin.ts`, and tests | Add X-Requested-By to all administrator writes; exempt only exact POST /sessions login from the missing-token block; logout and other writes still require a token; unauthenticated bootstrap uses the actual empty token |
| `database/apply.ps1` | Container-based migration execution, original-file hashes, full version names, strict transaction-boundary scanning, transactional version records and advisory locks, read-only preview, Windows PowerShell 5.1 encoding compatibility, and native stderr handling |
| `database/init-roles.sql` and deployment role initialization | Maintenance user owns schema objects, application roles have runtime privileges only; fail if required roles are absent, without weak-password fallbacks; isolate the push role from core business tables |

In this table, `src/` means `services/api/src/`.

Drogon binds numeric parameters in binary format. Without explicit parameter types, PostgreSQL may infer smallint / bigint / double widths that differ from a C++ int. Previous failures included `incorrect binary data format in bind parameter N` and `insufficient data left in message`. Match the incoming type; do not declare an int-bound parameter directly as smallint.

## 6. Actual Acceptance Evidence and Its Limits

### Checks Run by the Owner

- Main and test databases each received all six migrations and role grants; role flags matched restricted-account settings.
- Backend test and runtime images built successfully; updated backend containers passed health checks.
- Earlier complete backend test result (before the AdminFilter fix): `unit` 0.01 seconds, `db_gated` 0.21 seconds, `static_check` 0.00 seconds, total 0.23 seconds, exit code 0. The database test ran and was not skipped.
- These are **three CTest targets**, not only three internal test cases. Do not repeat the outdated fixed "12 cases" claim. DbGatedTest now has eight test blocks, including the administrator-filter regression; UnitTest also has AdminFilter cases.
- After the AdminFilter fix: test and runtime images built successfully; all three CTest targets passed, `db_gated` ran and was not skipped, exit code 0. Timings and assertion counts for this run were not recorded. Runtime results are in Section 2.
- The user frontend and custom Service Worker built successfully; three test files with 24 unit tests passed.
- The administrator frontend built successfully. An earlier run passed two files with 29 unit tests. Tests passed again with exit code 0 after the login-request fix, but that run's total count was not recorded; do not invent an updated count.
- User health on 8443 returned 200, administrator health on 8444 returned 200, and administrator health through the user entry point returned 404.
- Real administrator login, all three lists, logout, and refresh after logout were verified. Before the AdminFilter fix, this did not establish rejection of unauthenticated access; that check failed then. After the fix, anonymous list requests returned 401 (Section 2).
- Both ordinary accounts registered and logged in; space creation, invitation generation, and acceptance by the second account succeeded with 2/2 members. Refresh preserved both sessions and membership. Logging out the second account did not affect the first account or space membership.
- The installed desktop application displayed the authenticated home page and 2/2 members, and reopening worked. The first launch was blank and recovered after refreshing; it has not recurred, and its cause is unknown. Do not report it as a fixed blank-screen defect.
- Offline refresh displayed the shell and offline banner. Failed session bootstrap showed the login page. Restoring the network and refreshing returned automatically to the authenticated home page with 2/2 members. The offline login page did not establish server-side expiration or data loss.
- Both caches were inspected: 15 precache entries and 14 shell-cache entries, containing shell/static resources, with no `/api/`, `/media/`, `/private/`, or `/uploads/` entries. This was a snapshot check, not proof that every future request is safe.
- A main-database backup was exported to the host, copied back from the host file, and successfully restored into an independent empty database using a single transaction with immediate failure on errors.

### Backup and Recovery Evidence

- Retained file: `D:\JYJ\USapp\backups\usapp_stage0_20261006_144542.dump`, 70,721 bytes.
- Recovery database: `usapp_recovery`, retained for inspection, not used by the application.
- Container temporary files: `/tmp/usapp_stage0.dump` and `/tmp/usapp_restore.dump`; cleanup was not performed during this verification.
- Main and recovery databases each contained two users, one space, and two members, with one member in each slot. All six migration versions and checksums matched exactly.
- This establishes that the archive restored successfully and passed those basic checks. **It does not establish full row-by-row equality, off-site backup, encrypted backup, or disaster-recovery timing.**
- `.gitignore` already excludes `backups/`, `*.dump`, real environment files, and certificates. Do not commit backups, secrets, or verification-code logs.

Migration versions use the complete original filename BaseName:

```text
0000_schema_migrations
0001_common
0002_identity
0003_couple_binding
0004_scheduler
0005_admin_audit
```

### Git Baseline (Owner-Run)

- The owner initialized a new local Git repository on branch `main`.
- The owner reviewed the staged scope and made the initial baseline commit `b9cd0ea` with the message `chore: establish phase 0 baseline`.
- After the commit, no outstanding changes to tracked files were shown.
- `.workbuddy/` and the original V1 design document remain untracked. They were intentionally excluded from the initial commit; do not add them without an explicit owner instruction.
- No remote publication was reported. Do not assume a remote, a push, or tags exist.
- Limits: Claude Code and Codex did not run Git commands; these results are owner-reported. The commit records the working tree at that point; it does not add runtime verification beyond what Sections 2, 6, and 7 record. Changes made after `b9cd0ea`, including this documentation update, are not committed.

## 7. Known Issues and Outstanding Acceptance Work

1. **Resolved: unauthenticated administrator reads.** Fixed, rebuilt, tested, and checked at runtime; see Section 2 for results and the limits of that evidence.
2. Administrator negative checks, partially done. Live results: wrong login Origin returned `403 ORIGIN_NOT_ALLOWED`; missing `X-Requested-By` returned `403 CSRF_TOKEN_INVALID`; anonymous lists returned 401; the list request after logout returned `UNAUTHENTICATED`. Still covered only by automated tests, with no separate live check: invalid, expired, and revoked session cookies, and a wrong or missing `X-CSRF-Token` on authenticated writes. Passing tests does not prove complete access-control correctness.
3. **Resolved: HomeView member label.** Previously, `apps/web/src/views/HomeView.vue` used `item.member?.display_name || ...`, so a member with an empty nickname was labeled "Vacant" while the badge said "Joined." The vacancy label now renders only when `item.member` is absent; an existing member shows their nickname or a Chinese fallback meaning "Nickname not set." Owner results: the user-frontend build passed; after the app's update prompt and a refresh, both members displayed the "Nickname not set" label with "Joined" badges, and membership remained 2/2. Limits: user-frontend unit tests were not reported as rerun after this change; a member with a non-empty nickname was not separately shown after the change; membership data was not modified.
4. `Transaction roll back error` occurred alongside the invitation-acceptance parameter-binding error. After the type fix, the real invitation workflow and regression passed. The explanation that malformed binary binding necessarily prevents subsequent rollback is unverified. Do not refactor transactions on that assumption or claim all rollback failures have been excluded.
5. Line buffering was built and deployed. The owner later obtained codes and logged in successfully but did not separately confirm that every log appeared immediately. Do not report complete log-timing verification.
6. The installed application's initial blank screen has no confirmed cause. Reopening currently works. If it recurs, collect console/request evidence before clearing caches.
7. Real email delivery, real push delivery, and later task/reward/asset/media features remain unverified or outside Phase 0.
8. **Resolved: initial Git baseline.** Repository initialization, staged-scope review, and the initial commit `b9cd0ea` were completed by the owner (Section 6, "Git Baseline"). This later documentation update is not yet committed; any further commit or remote publication is the owner's decision. Older handoff documents (including `services/api/tests/README.md` test counts) still need a final status update.

## 8. Document Reliability and Reading Order

- `contracts/openapi.yaml`: interface contract. Do not change it to allow unauthenticated list access.
- `services/api/src/http/AdminFilter.cc`, `AdminFilter.h`, `controllers/AdminController.cc`, and `services/AdminAuthService.cc`: direct entry points for the administrator authentication fix (resolved; see Section 2); controller/service paths are relative to `services/api/src/`.
- `services/api/tests/UnitTest.cc`, `DbGatedTest.cc`, `TestSupport.h`, `AdminFilterProbe.h`, and `CMakeLists.txt`: active tests and supporting mechanisms, all under `services/api/tests/`.
- `deploy/docker-compose.yml`, `deploy/Caddyfile`, and `deploy/Dockerfile.api`: current deployment/build sources of truth.
- `docs/phase0-handoff.md`, `deploy/README.md`, and `services/api/tests/README.md`: older execution guides. Some "not run" statements and test counts are outdated.
- **`docs/architecture.md` is an outdated static snapshot.** It still says controllers are absent and the administrator entry uses only 8443. Those claims are no longer true. Do not delete existing controllers, merge administrator entry points, or redesign the architecture based on that snapshot. Use current code/configuration, runtime evidence recorded here, and subsequent owner instructions.
- This document records the handoff snapshot. Changes made afterward do not automatically inherit these verification results.

## 9. Reference Commands for Owner-Run Acceptance

Codex supplies these commands to the owner step by step. Claude Code must not run them autonomously. Always work from the repository root and explicitly specify the environment and Compose files.

```powershell
Set-Location -LiteralPath 'D:\JYJ\USapp'

# Build the test image; wait for success before proceeding.
docker compose --progress plain --env-file .\.env -f .\deploy\docker-compose.yml --profile test build test-runner
Write-Host "Test image build exit code: $LASTEXITCODE"

# Run all tests; verify that db_gated actually Passed, not just the summary.
docker compose --env-file .\.env -f .\deploy\docker-compose.yml --profile test run --rm test-runner
Write-Host "Full test suite exit code: $LASTEXITCODE"

# After tests pass, build the runtime image.
docker compose --progress plain --env-file .\.env -f .\deploy\docker-compose.yml build api
Write-Host "Backend runtime image build exit code: $LASTEXITCODE"

# Update only the backend container without reinitializing the database.
docker compose --env-file .\.env -f .\deploy\docker-compose.yml up -d --no-deps --wait --wait-timeout 120 api
Write-Host "Backend container update exit code: $LASTEXITCODE"

# No-cookie administrator user-list request: expect HTTP 401 (observed after the fix).
curl.exe -q -i --ssl-revoke-best-effort --cacert .\deploy\caddy\certs\root.crt https://localhost:8444/api/admin/v1/users
```

A curl exit code of 0 indicates successful transport, not correct authorization. Inspect the response status and content. The reproduced defect returned 200 and sensitive list data despite transport success.

Only when frontend changes are necessary, the owner runs these commands from the corresponding frontend directory:

```powershell
corepack.cmd pnpm run test:unit
corepack.cmd pnpm run build
```

Main, test, and recovery databases already exist and were prepared successfully. This authentication fix does not require rerunning migrations, creating or deleting databases, removing volumes, or restoring backups.

## 10. Initial Message to Claude Code

```text
Read D:\JYJ\USapp\docs\claude-code-handoff.md.

You implement changes. Codex reviews them and guides me through acceptance
one step at a time. I manually run all builds, tests, database operations,
container operations, installations, and Git operations. Do not execute
those actions unless I explicitly authorize them separately.

The Section 2 administrator authentication fix and the HomeView label fix
have been implemented and checked; do not redo them. The Phase 0 baseline
is committed locally as b9cd0ea; later changes stay uncommitted until I
report a new commit. Phase 0 is still not accepted. Wait for my next assigned
task, then use the existing implementation authorization for that task
only. Do not read the real .env, and preserve unrelated changes.

Briefly confirm the root cause before editing. Do not expand into Phase 1
or redesign the architecture. Deliver actual file differences, regression
coverage, owner-run acceptance steps, and remaining unverified risks.
Explicitly state when verification has not been run; do not claim tests passed.
Write all handoff and implementation reports for Claude Code in English.
After each task, also provide a copy-paste-ready report for Codex.
```
