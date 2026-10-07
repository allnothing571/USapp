# USapp: Claude Code Implementation / Codex Acceptance Handoff

Updated: 2026-10-06, Asia/Shanghai. Later the same day: recorded owner-run acceptance results for the AdminFilter authentication fix (Section 2) and the HomeView member-label fix (Section 7, item 3). Subsequently: recorded the owner-made initial local Git baseline commit `b9cd0ea` (Section 6, "Git Baseline"). Subsequently: recorded documentation-only pre-push corrections to `README.md` and this file (Section 6, "Pre-Push Documentation Corrections"). Subsequently: recorded the owner-reported first push to a private GitHub repository, live administrator CSRF rejection checks, and scheduler heartbeat evidence (Section 6), and added the Phase 0 acceptance matrix `docs/phase0-acceptance.md`. Git hashes and repository status in this document are dated observations, not permanent current state; this document is not updated to record its own commits. Subsequently: recorded the owner's post-fix backend test timings and the pre-commit credential-pattern check (Section 6). Subsequently: recorded owner-run live user-side email-code Origin/CSRF negative checks and administrator session persistence after rejected logout requests (Section 6). Subsequently: recorded the owner's explicit acceptance of Phase 0 on 2026-10-06 as a local development baseline (Sections 1 and 6) and added the Phase 1 plan `docs/phase1-plan.md` (planning only). Statements elsewhere in this document that Phase 0 was "not accepted" or acceptance was "pending" are historical and describe the state when they were written. Subsequently: recorded the owner's approval of Phase 1 milestone M1 only, the C3/D3 correction, and Claude Code's M1 implementation, which has not been verified (Sections 1 and 6). Subsequently: recorded the owner-run M1 verification results (Section 6) and started the documentation-only M2 wallet/ledger design (`docs/phase1-plan.md` Section 9). Subsequently: recorded the owner's approved M2 design directions (D1, D2 bounds, D18, D19, D21) and revised the M2 design after review; M2 implementation remains unapproved (Section 1). Subsequently (2026-10-07): a second documentation-only M2 design revision (concurrency fixture scope, exact permission outcomes, effective privilege preflight, rollback-evidence limits); no approval status changed.
Project root: `D:\JYJ\USapp`. Environment: Windows PowerShell.

## 1. Responsibilities, Authorization, and Current Objective

- Claude Code implements subsequent code changes, adds necessary regression tests, and revises changes based on acceptance feedback.
- Codex checks assumptions, reviews actual changes, designs acceptance checks, and guides the owner through validation one step at a time.
- **The owner manually runs builds, tests, database operations, container operations, installations, browser checks, and Git operations.** Neither Claude Code nor Codex should execute these actions unless the owner explicitly grants additional authorization.
- Commands in this document are references for the owner. Their presence does not authorize Claude Code to execute them.
- The owner has delegated subsequent implementation to Claude Code. Do not ask again for permission to implement an explicitly assigned fix. Explain and obtain authorization before making out-of-scope architectural changes, database changes, dependency upgrades, or feature additions.
- Preserve existing work. Do not reset, overwrite, or revert changes unrelated to the assigned fix. Git state is recorded as dated, owner-reported observations (Section 6): the initial baseline commit `b9cd0ea`, and later a push to a private GitHub repository at `9e00049a6f4b669897ef1172047c4eb8bbdd7a57`. Treat any hash as the state at the time it was reported. Changes made after the last owner-reported state are uncommitted unless the owner reports a new commit; do not claim otherwise.
- Do not read the real `.env`. Do not disclose secrets, passwords, session cookies, plaintext verification codes, or invitation codes. Do not inspect database backups to obtain secrets.
- Deliver actual file differences, the reasoning behind the fix, and its verification status. Distinguish "files modified," "statically reviewed," "build passed," "tests passed," and "real workflow verified." Another tool's completion report is not test evidence.
- Give the owner one acceptance step at a time and wait for its result before proceeding. Retain essential errors rather than pasting lengthy logs.
- All content handed to Claude Code must be written in English. Codex continues to communicate with the owner in Chinese.

**Current objective: documentation-only design of Phase 1 milestone M2 (wallet and ledger), recorded in `docs/phase1-plan.md` Section 9. On 2026-10-06 the owner approved design directions only: D1 (task completion creates new flowers and sunshine for the recipient; the publisher is not debited), D2 bounds (maximum balance 1,000,000 flowers and 1,000,000,000 sunshine; maximum per-entry amount 999 flowers and 100,000 sunshine; reject, never clamp), D18 (narrowed grants plus triggers that survive re-running the role-initialization script), D19 (no new administrator asset endpoints in M2), and D21 (own balances only). No other product or task rule is approved. M2 implementation, migrations, grant and `init-roles.sql` changes, contract changes, and any database execution are not approved. As of 2026-10-07 the design defers existing-account lock serialization and post-commit visibility tests to M3; M2's planned concurrency test covers account-creation contention only. The owner explicitly accepted Phase 0 on 2026-10-06 as a local development baseline, within the verified scope and limitations in `docs/phase0-acceptance.md`; this is not production-readiness approval, and the open items in Section 7 remain open. Phase 1 milestone M1 (nickname editing) was approved, implemented, and passed the owner-run acceptance checks on 2026-10-06 (Section 6 and `docs/phase1-plan.md` Section 7.9). M2-M10 implementation must not start until the owner approves and assigns that specific milestone.**

Historical note: before 2026-10-06 the objective was to complete Phase 0 acceptance; statements in Sections 2, 6, and 7 that Phase 0 was not yet accepted describe that earlier state.

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
- All three CTest targets (`unit`, `db_gated`, `static_check`) passed; `db_gated` ran and was not skipped; exit code 0. Recorded timings: `unit` 0.01 s, `db_gated` 0.20 s, `static_check` 0.00 s, total 0.21 s. Per-assertion counts were not recorded; do not invent them.
- Anonymous requests to `/api/admin/v1/users`, `/api/admin/v1/couples`, and `/api/admin/v1/audit-logs` returned `401 UNAUTHENTICATED` without list data.
- Anonymous `GET /api/admin/v1/health` and `GET /api/admin/v1/session` returned 200; bootstrap returned `authenticated=false` and an empty `csrf_token`.
- An authenticated administrator could load all three lists.
- After logout, the browser's user-list request returned `UNAUTHENTICATED`.
- The public entry point returned 404 for the administrator user list.
- Login with a wrong Origin returned `403 ORIGIN_NOT_ALLOWED`.
- Login with a missing `X-Requested-By` header returned `403 CSRF_TOKEN_INVALID`.

### Limits of This Evidence

- Invalid, expired, and revoked sessions were exercised by the `db_gated` test against the test database, not by separate live requests against the running service.
- A wrong or missing `X-CSRF-Token` on an authenticated administrator write was covered by tests only at the time of these results. A later live check on logout returned `403 CSRF_TOKEN_INVALID` for both cases, and the administrator session remained authenticated after each rejection (Section 6, "Administrator CSRF Live Checks").
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
- After the AdminFilter fix: test and runtime images built successfully; all three CTest targets passed, `db_gated` ran and was not skipped, exit code 0. Timings: `unit` 0.01 s, `db_gated` 0.20 s, `static_check` 0.00 s, total 0.21 s. Per-assertion counts for this run were not recorded. Runtime results are in Section 2.
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
- At the time of the baseline commit, no remote publication was reported. A later push is recorded under "Private GitHub Publication" below. Do not assume tags exist.
- Limits: Claude Code and Codex did not run Git commands; these results are owner-reported. The commit records the tracked files at that point; it does not add runtime verification beyond what Sections 2, 6, and 7 record. At the time, changes made after `b9cd0ea` were not committed.

### Pre-Push Documentation Corrections (Documentation Only)

Documentation-only changes made after `b9cd0ea` to prepare the first GitHub push. No code, configuration, contract, test, or migration file changed. No build, test, database, container, or Git command was run by Claude Code. When written, these corrections were pending Codex review and owner-run Git checks and commit; the later push (below) was reported with no other changes outstanding besides the two intentionally untracked items. They add no runtime verification and do not constitute Phase 0 acceptance.

- `README.md` (kept in Chinese, same structure):
  - The broken link to the nonexistent `docs/phase0-delivery.md` was replaced with `docs/claude-code-handoff.md`, identified as the current handoff and acceptance-status record. `docs/phase0-handoff.md` remains linked as the older step-by-step checklist with partly outdated "not run" notes and test counts.
  - Origin statement: exact Origin validation applies to state-changing requests only; user-side state-changing requests use `PUBLIC_ORIGIN`, and administrator state-changing requests use `ADMIN_ORIGIN`. This matches `AuthFilter` and `AdminFilter`, which check Origin only for state-changing methods.
  - `docs/architecture.md` is labeled a historical snapshot with outdated deployment descriptions.
  - A status note says Phase 0 has not passed final acceptance, the executed checks were targeted, and not every contract or end-to-end check has been executed.
  - Security baseline item 2 now describes the administrator login exception: exact `POST /api/admin/v1/sessions` requires exact `ADMIN_ORIGIN` and `X-Requested-By` but no session-bound CSRF token before login; other protected administrator writes, including logout, still require that token. Ordinary-user pre-authentication protection (CSRF + Origin for sending codes and login) is preserved.
  - The `tests/` row now describes cross-layer acceptance checklists and test documentation executed selectively by the owner; it no longer says the checks are never executed, and it does not claim all checks have run.
- This file: the "Git Baseline" limits wording now says the commit records the tracked files (not the whole working tree). A previous delivery of that wording did not reach the file on disk; it is applied here.
- Known residual: the root `tests/` documents themselves were not re-reviewed for outdated "not executed" statements.

### Pre-Commit Credential-Pattern Check (Owner-Run)

- A basic credential-pattern check was run before committing: `git grep` over the staged files for private-key headers, AWS-style access-key identifiers, and selected GitHub token patterns produced no matches (exit code 1, meaning no match). The owner also verified that ignore rules cover `.env`, the backup archive, certificates, `node_modules`, and the frontend `dist` directories.
- Limits: this was a limited pattern check of staged files only. It covered a small set of credential formats, not every possible secret, and it was not a scan of the full pushed history; no complete history scan was recorded. It does not prove that all secrets are excluded from the repository.

### Private GitHub Publication (Owner-Run; Dated Observation, 2026-10-06)

- The owner pushed the repository for the first time to the owner-confirmed private repository `https://github.com/allnothing571/USapp`.
- Local `HEAD` and remote `main` both matched `9e00049a6f4b669897ef1172047c4eb8bbdd7a57`.
- Local `main` tracks `origin/main`.
- Only `.workbuddy/` and the original V1 design document remained untracked, intentionally.
- Limits: owner-reported; Claude Code and Codex did not run Git commands. This is the state at the time of reporting, not a permanent current state. Repository visibility was confirmed by the owner, not independently checked. Before committing, the owner ran a limited staged-file credential-pattern check with no matches and verified key ignore rules (see "Pre-Commit Credential-Pattern Check" above); no complete scan of the pushed history was recorded, so this is not proof that history contains no secrets. A push is not Phase 0 acceptance.

### Administrator CSRF Live Checks (Owner-Run)

- An authenticated administrator `DELETE /api/admin/v1/sessions` (logout) with a missing `X-CSRF-Token` returned `403 CSRF_TOKEN_INVALID`.
- The same request with an incorrect `X-CSRF-Token` also returned `403 CSRF_TOKEN_INVALID`.
- Immediately after each rejection (2026-10-06), `GET /api/admin/v1/session` returned HTTP 200 with `authenticated=true`, so the rejected logout requests did not end the administrator session.
- Limits: this covers the logout write only, with missing and incorrect tokens. Invalid, expired, and revoked session cookies remain test-only (Section 2).

### User-Side Email-Code Origin/CSRF Live Checks (Owner-Run, 2026-10-06)

Endpoint: `POST /api/v1/auth/email-codes` on the user entry point.

- Wrong Origin: HTTP 403 `ORIGIN_NOT_ALLOWED`; curl exit code 0 (transport success, not an authorization result).
- Existing session, missing `X-CSRF-Token`: HTTP 403 `CSRF_TOKEN_INVALID`.
- Existing session, incorrect `X-CSRF-Token`: HTTP 403 `CSRF_TOKEN_INVALID`.
- The two token checks were sent with browser `fetch` and an empty JSON object body.
- Limits: targeted negative checks on one endpoint. They are not exhaustive user-side security verification; other user-side state-changing endpoints were not checked live in this way.

### Owner Acceptance of Phase 0 (2026-10-06)

- The owner explicitly accepted Phase 0 on 2026-10-06 as a local development baseline, within the verified scope and limitations recorded in `docs/phase0-acceptance.md`.
- Limits: this is not production-readiness approval and adds no new runtime evidence. Test-only coverage remains test-only, and the open items in Section 7 remain open.
- Phase 1 planning was authorized at the same time; implementation awaits owner approval of `docs/phase1-plan.md`.

### Phase 1 M1 Approval and Implementation (2026-10-06; Not Yet Verified)

- The owner approved M1 (nickname editing) on 2026-10-06, including the additive `PATCH /api/v1/me` operation and OpenAPI `info.version` 0.2.0. M2-M10 and their proposed defaults were not approved.
- The owner corrected Phase 1 plan items C3/D3: the established requirement stands that key task edits after publication of the current period require recipient confirmation; the earlier "no progress means silent editing" default was removed. The detailed workflow remains undecided.
- Claude Code implemented M1: `ProfileService` (validation, transactional update plus audit), the `PATCH /api/v1/me` route behind `AuthFilter`, the contract addition, the home-page edit control with session/space store updates, and regression tests (unit, `db_gated` block 9, static check, frontend). Details: `docs/phase1-plan.md` Section 7.8.
- Limits: files modified only. No build, test, database, container, browser, or Git command was run by Claude Code. Verification has not been run, and M1 is not accepted until the owner completes `docs/phase1-plan.md` Section 7.7.
- Review repair (same day): Codex found a strong ownership cycle in `ProfileService` (context -> `TxScope` -> stored error handler -> context) that retained the request callback after success or NotFound. Claude Code changed the stored error handler to a weak capture, added an exactly-once completion guard, and added `DbGatedTest.cc` block 10 (callback-owned sentinel released within a bounded wait after success, NotFound, and SQL failure). Shared `Db.h`/`Db.cc` and other services were not changed. When written, not built or tested; details in `docs/phase1-plan.md` Section 7.8.

### Phase 1 M1 Owner-Run Verification (2026-10-06)

Recorded separately from the implementation status above. M1 evidence only; Phase 0 evidence is unchanged.

- Backend test-image build exit 0. CTest `unit`, `db_gated`, `static_check` all Passed; `db_gated` not skipped; timings 0.01 s, 0.35 s, 0.00 s, total 0.37 s; exit 0. Runtime image build and `api` container update exit 0; container Healthy.
- Web unit tests: 4 files, 35 tests passed, exit 0. Web build exit 0; Service Worker generated.
- Live: nickname updates the current-user card and own member slot and survives refresh; the partner sees it after refresh; the partner's nickname is unchanged; members 2/2.
- Live rejections, nickname unchanged in each case: whitespace-only, 21 emoji code points, and an extra request field -> 400 `VALIDATION_ERROR`; wrong Origin -> 403 `ORIGIN_NOT_ALLOWED`; correct Origin without a cookie -> 401 `UNAUTHENTICATED`; authenticated with missing or incorrect `X-CSRF-Token` -> 403 `CSRF_TOKEN_INVALID`; pre-authentication session with valid CSRF -> 401 `UNAUTHENTICATED`.
- Offline: save blocked; no PATCH sent or automatically replayed after reconnecting; nickname unchanged after refresh.
- Admin UI: user list shows the nickname; audit list shows the update event. The admin UI/API does not expose audit `detail`; an owner SQL query showed audit id 10, `user.display_name_updated`, `detail` `{"field":"display_name"}`, exit 0.
- Limits: targeted checks of one endpoint and its UI, not exhaustive security verification. Application logs were not separately inspected for nickname values; concurrent updates, HEAD/OPTIONS on `/api/v1/me`, and the transaction-creation and commit-failure paths were not exercised live. Per-assertion counts were not recorded.

### Scheduler Heartbeat (Owner-Run)

- Runtime configuration: scheduler enabled (`enabled=true`), tick interval 15000 ms.
- Main-database heartbeat query, excluding the migration placeholder: 1954 completed records; latest timestamp `2026-10-06 08:41:54.591309+00`; age 13.137962 seconds at query time.
- This confirms that heartbeat jobs were executing and completing in the running service at that time.
- Limits: a single point-in-time query. It is not live multi-instance concurrency verification; advisory-lock mutual exclusion and rollback release are covered by the `db_gated` test only.

### Session Migration to Claude Code (2026-10-06; Documentation and Configuration Only)

- Work moved from a Claude (Cowork) session to Claude Code. Two files were added: `CLAUDE.md` (standing instructions for Claude Code) and `.claude/settings.json` (project permission rules denying Git, container, package-manager/install, build/test, and database commands, and reads of real environment files and `backups/`).
- Limits: documentation and configuration only. No code, test, contract, migration, or deployment file changed, and no build, test, database, container, or Git command was run. This adds no runtime verification and is not Phase 0 acceptance. Whether the installed Claude Code version enforces every rule in `.claude/settings.json` has not been verified.

## 7. Known Issues and Outstanding Acceptance Work

1. **Resolved: unauthenticated administrator reads.** Fixed, rebuilt, tested, and checked at runtime; see Section 2 for results and the limits of that evidence.
2. Administrator negative checks, partially done. Live results: wrong login Origin returned `403 ORIGIN_NOT_ALLOWED`; missing `X-Requested-By` returned `403 CSRF_TOKEN_INVALID`; anonymous lists returned 401; the list request after logout returned `UNAUTHENTICATED`. An authenticated administrator logout with a missing or incorrect `X-CSRF-Token` also returned `403 CSRF_TOKEN_INVALID` live, and `GET /api/admin/v1/session` immediately afterward still returned `authenticated=true` (Section 6, "Administrator CSRF Live Checks"). User-side `POST /api/v1/auth/email-codes` rejected a wrong Origin and a missing or incorrect `X-CSRF-Token` with 403 live (Section 6, "User-Side Email-Code Origin/CSRF Live Checks"). Still covered only by automated tests, with no separate live check: invalid, expired, and revoked session cookies. Passing tests does not prove complete access-control correctness.
3. **Resolved: HomeView member label.** Previously, `apps/web/src/views/HomeView.vue` used `item.member?.display_name || ...`, so a member with an empty nickname was labeled "Vacant" while the badge said "Joined." The vacancy label now renders only when `item.member` is absent; an existing member shows their nickname or a Chinese fallback meaning "Nickname not set." Owner results: the user-frontend build passed; after the app's update prompt and a refresh, both members displayed the "Nickname not set" label with "Joined" badges, and membership remained 2/2. Limits: user-frontend unit tests were not reported as rerun after this change; a member with a non-empty nickname was not separately shown after the change; membership data was not modified.
4. `Transaction roll back error` occurred alongside the invitation-acceptance parameter-binding error. After the type fix, the real invitation workflow and regression passed. The explanation that malformed binary binding necessarily prevents subsequent rollback is unverified. Do not refactor transactions on that assumption or claim all rollback failures have been excluded.
5. Line buffering was built and deployed. The owner later obtained codes and logged in successfully but did not separately confirm that every log appeared immediately. Do not report complete log-timing verification.
6. The installed application's initial blank screen has no confirmed cause. Reopening currently works. If it recurs, collect console/request evidence before clearing caches.
7. Real email delivery, real push delivery, and later task/reward/asset/media features remain unverified or outside Phase 0. Scheduler heartbeat execution was observed (Section 6), but live multi-instance scheduler concurrency has not been verified.
8. **Resolved: initial Git baseline.** Repository initialization, staged-scope review, and the initial commit `b9cd0ea` were completed by the owner (Section 6, "Git Baseline"). A later owner-reported push to a private GitHub repository is recorded in Section 6 as a dated observation. Documentation changes made after that push, including the update that records it and `docs/phase0-acceptance.md`, are uncommitted until the owner reports otherwise; any further commit or push is the owner's decision. Older handoff documents (including `services/api/tests/README.md` test counts) still need a final status update.

## 8. Document Reliability and Reading Order

- `CLAUDE.md` and `.claude/settings.json`: standing instructions and project permission rules for Claude Code. Read `CLAUDE.md` first; the permission rules deny commands the owner runs (Section 1) and reads of real environment files and `backups/`.
- `docs/phase0-acceptance.md`: concise Phase 0 acceptance matrix derived from the evidence in this document; the owner accepted Phase 0 on 2026-10-06 as a local development baseline within its recorded scope and limitations.
- `docs/phase1-plan.md`: Phase 1 scope, open product decisions, and proposed milestones. M1 is approved, implemented, and owner-verified (Sections 7.8 and 7.9); Section 9 is the documentation-only M2 wallet/ledger design with approved directions D1, D2 bounds, D18, D19, and D21; M2-M10 implementation is not approved.
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

Also read CLAUDE.md, docs/phase0-acceptance.md, and docs/phase1-plan.md.

The Section 2 administrator authentication fix and the HomeView label fix
have been implemented and checked; do not redo them. Git state in the
handoff is a dated observation (private GitHub push at 9e00049); later
changes stay uncommitted until I report a new commit. Phase 0 was accepted
on 2026-10-06 as a local development baseline, not as production-ready;
its recorded limitations remain open. Phase 1 M1 (nickname editing) is
implemented and passed my acceptance checks. M2 is in documentation-only
design (docs/phase1-plan.md Section 9); its approved design directions do
not authorize implementation. M2-M10 are not approved: do not implement
any of them until I approve and assign that specific milestone. Then use the existing implementation
authorization for that milestone only. Do not read the real .env, and
preserve unrelated changes.

Briefly confirm the root cause or design before editing. Do not expand
beyond the assigned milestone or redesign the architecture. Deliver actual file differences, regression
coverage, owner-run acceptance steps, and remaining unverified risks.
Explicitly state when verification has not been run; do not claim tests passed.
Write all handoff and implementation reports for Claude Code in English.
After each task, also provide a copy-paste-ready report for Codex.
```
