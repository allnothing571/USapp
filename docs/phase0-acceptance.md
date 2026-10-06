# USapp Phase 0 Acceptance Matrix

Evidence date: 2026-10-06, Asia/Shanghai.
Source of record: [docs/claude-code-handoff.md](claude-code-handoff.md). This matrix summarizes it and adds no new evidence.

**Final owner acceptance of Phase 0: PENDING.** Phase 1 must not start until the owner explicitly accepts Phase 0.

This matrix does not claim production readiness or exhaustive security verification. Not every contract or end-to-end check has been executed. Git hashes and repository status are dated observations, not permanent current state. This document is not updated to record its own commits.

## Evidence categories

- **Verified:** run or observed by the owner, through builds, test runs, live requests, the browser, the installed app, or database queries.
- **Test-only:** covered by automated tests (`unit`, `db_gated`, `static_check`) but not separately checked against the running service.
- **Limitation:** not verified, partially verified, or outside Phase 0.

## Summary

| Area | Verified | Test-only coverage | Key limitations |
| --- | --- | --- | --- |
| Builds and tests | Yes | n/a | Post-fix per-assertion counts not recorded |
| Ordinary-user authentication | Yes | Session lifecycle in `db_gated` | Real email delivery unverified |
| Two-member binding | Yes | Slot, invite, and repeat-acceptance rules | Rollback-error explanation unverified |
| Administrator access controls | Yes | Invalid/expired/revoked sessions, bypass probes | Not exhaustive; session persistence checked only after rejected logout |
| Installed / offline app | Yes | n/a | First-launch blank screen unexplained; cache check was a snapshot |
| Backup / recovery | Yes | n/a | No row-by-row, off-site, encrypted, or timing proof |
| Scheduler | Yes (heartbeat) | Advisory-lock exclusion | No live multi-instance concurrency check |
| Private GitHub publication | Yes (owner-reported) | n/a | Dated observation; limited staged-file credential check only, no full-history scan recorded |

## 1. Builds and tests

- **Verified:**
  - After the AdminFilter fix, backend test and runtime images built.
  - All three CTest targets (`unit`, `db_gated`, `static_check`) passed; `db_gated` ran and was not skipped; exit code 0. Timings: `unit` 0.01 s, `db_gated` 0.20 s, `static_check` 0.00 s, total 0.21 s.
  - The user frontend and custom Service Worker built, with 3 test files and 24 unit tests passing in an earlier run. The user-frontend build passed again after the HomeView change.
  - The administrator frontend built; an earlier run passed 2 files and 29 unit tests, and a later run passed with exit code 0.
- **Test-only:** n/a.
- **Limitations:**
  - Per-assertion counts for the post-fix backend run were not recorded.
  - User-frontend unit tests were not reported as rerun after the HomeView change.
  - The administrator frontend's later test count was not recorded.
  - Root `tests/` checklists are executed selectively.

## 2. Ordinary-user authentication

- **Verified:**
  - Both accounts registered and logged in with email codes; mail ran in development/log mode.
  - Refresh preserved both sessions.
  - Logging out the second account did not affect the first.
  - Offline failure of session bootstrap showed the login page; after the network returned, a refresh restored the authenticated home page.
  - `POST /api/v1/auth/email-codes` with a wrong Origin returned `403 ORIGIN_NOT_ALLOWED` (curl exit code 0).
  - The same endpoint with an existing session and a missing or an incorrect `X-CSRF-Token` returned `403 CSRF_TOKEN_INVALID` (browser `fetch`, empty JSON object body).
- **Test-only:** pre-authentication and authenticated session lifecycle, session-fixation protection, and revocation at the database level (`db_gated`).
- **Limitations:**
  - Real email delivery is unverified.
  - Immediate appearance of every code log line was not separately confirmed.
  - Live user-side Origin/CSRF negative checks cover only `POST /api/v1/auth/email-codes`; they are not exhaustive user-side security verification.

## 3. Two-member binding

- **Verified:**
  - Space creation, invitation generation, and acceptance by the second account produced 2/2 members.
  - Membership survived refresh and logout of the second account.
  - After the HomeView label fix, both members showed the "Nickname not set" fallback with "Joined" badges, at 2/2.
- **Test-only:**
  - Slot values limited to 1/2, unique slot per space, and one space per user.
  - Single-use invite consumption.
  - Real `SpaceService` invite creation and acceptance; repeated acceptance returns `InviteInvalidOrExpired` (`db_gated`).
- **Limitations:**
  - The explanation for the earlier `Transaction roll back error` is unverified.
  - Display of a non-empty nickname after the label fix was not separately shown.

## 4. Administrator access controls

- **Verified:**
  - Anonymous `users`, `couples`, and `audit-logs` requests returned `401 UNAUTHENTICATED` without data.
  - Anonymous health and session bootstrap returned 200; bootstrap returned `authenticated=false` and an empty `csrf_token`.
  - An authenticated administrator loaded all three lists.
  - After logout, the user-list request returned `UNAUTHENTICATED`.
  - The public entry point returned 404 for the administrator user list.
  - Login with a wrong Origin returned `403 ORIGIN_NOT_ALLOWED`.
  - Login with a missing `X-Requested-By` returned `403 CSRF_TOKEN_INVALID`.
  - An authenticated logout (`DELETE /api/admin/v1/sessions`) with a missing or an incorrect `X-CSRF-Token` returned `403 CSRF_TOKEN_INVALID`.
  - Immediately after each rejected logout, `GET /api/admin/v1/session` returned 200 with `authenticated=true`.
- **Test-only:**
  - Invalid, expired, and revoked session cookies.
  - Another session's CSRF token.
  - Similar-path and other-method bypass probes.
  - The non-allowlisted peer address and the missing administrator-entry marker.
  - Fail-closed behavior when sessions cannot be loaded.
  - Rejection of the same token after revocation.
- **Limitations:**
  - Session persistence after a rejected write was checked only for logout with missing and incorrect tokens.
  - HEAD/OPTIONS handling is verified only at filter level.
  - Loopback isolation and these targeted checks are not an exhaustive security review.

## 5. Installed and offline app

- **Verified:**
  - The installed desktop app showed the authenticated home page with 2/2 members, and reopening worked.
  - Offline refresh showed the shell and the offline banner.
  - A cache snapshot found 15 precache and 14 shell-cache entries, with no `/api/`, `/media/`, `/private/`, or `/uploads/` entries.
- **Test-only:** n/a.
- **Limitations:**
  - The first launch was blank and recovered after a refresh; the cause is unknown.
  - The cache check was a snapshot, not proof about every future request.
  - The offline login page does not establish server-side expiration or data loss.

## 6. Backup and recovery

- **Verified:**
  - A main-database backup was exported to the host, copied back, and restored into an independent empty database in a single transaction that stops on the first error.
  - The main and recovery databases each had 2 users, 1 space, and 2 members (one per slot).
  - All six migration versions and checksums matched.
- **Test-only:** n/a.
- **Limitations:**
  - No full row-by-row equality, off-site backup, encrypted backup, or disaster-recovery timing.
  - Container temporary dump files were not cleaned up.

## 7. Scheduler

- **Verified:**
  - Runtime configuration: enabled, with a 15000 ms interval.
  - A main-database query, excluding the migration placeholder, found 1954 completed heartbeat records. The latest was at `2026-10-06 08:41:54.591309+00`, 13.137962 seconds old at query time.
- **Test-only:** transaction-level advisory-lock mutual exclusion between two connections, and release on rollback (`db_gated`).
- **Limitations:**
  - The query was a single point-in-time observation.
  - It confirms active heartbeat execution, not live multi-instance concurrency.

## 8. Private GitHub publication

- **Verified (owner-reported, 2026-10-06):**
  - First push to the owner-confirmed private repository `https://github.com/allnothing571/USapp`.
  - Local `HEAD` and remote `main` both matched `9e00049a6f4b669897ef1172047c4eb8bbdd7a57`.
  - `main` tracks `origin/main`.
  - Only `.workbuddy/` and the original V1 design document were untracked, intentionally.
  - Before committing, a `git grep` over staged files for private-key headers, AWS-style access-key identifiers, and selected GitHub token patterns produced no matches (exit code 1).
  - Ignore rules were verified to cover `.env`, the backup archive, certificates, `node_modules`, and frontend `dist` directories.
- **Test-only:** n/a.
- **Limitations:**
  - This is a dated observation; Claude Code and Codex ran no Git commands.
  - Repository visibility was owner-confirmed, not independently checked.
  - The credential check was a limited pattern check of staged files, not a scan of the full pushed history; no complete history scan is recorded, and it does not prove that all secrets are excluded.
  - A push is not acceptance.

## Outside Phase 0 or still open

- Real email delivery and real push delivery are unverified. The push helper is disabled by default.
- Tasks, rewards, assets, media, and other Phase 1+ features are not implemented in Phase 0.
- Older guides still contain outdated statements: `docs/phase0-handoff.md`, `deploy/README.md`, and the `services/api/tests/README.md` test counts. `docs/architecture.md` is a historical snapshot.

## Owner acceptance

- Status: **pending**.
- Decision and date: not recorded.
