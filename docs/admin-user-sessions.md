# Admin user-session metadata (read-only)

Codex, 2026-11-13. Contract 0.50.0 adds `GET /api/admin/v1/users/{userId}/sessions`. No session mutation, revocation, renewal, user-status change, logout, export or notification is implemented. No migration, grant, dependency or timeout change; 0000-0047 frozen, next 0048 unreserved.

## Boundary and data

AdminFilter checks peer CIDR, admin surface header and the independent admin session; the controller also requires the attached admin identity. A normal user cookie cannot authorize. Malformed id, limit or cursor: 400; unknown user: 404 NOT_FOUND. Existing users without a space or sessions return 200 with an empty page. Paths are canonicalized to lowercase UUIDs.

A session row exposes exactly id, created_at, last_seen_at, expires_at, revoked_at and state. The response also includes user id/status and read_at. No tokens, token/CSRF hashes, raw user agents, IPs, revocation-reason text or rotation links. Only rows with the selected user_id are read; the existing consistency constraint means pre-authentication rows (user_id NULL) cannot appear.

State precedence: revoked_at non-null -> revoked; otherwise expires_at <= statement_timestamp() -> expired; otherwise valid. The same statement timestamp is returned as read_at. Valid describes time/revocation only: disabled users and other authentication checks may still refuse access. Disabled accounts remain inspectable and their account status is displayed separately. This query never touches last_seen_at or any user session. AdminFilter's existing admin-session bookkeeping is unchanged.

## Pagination and resources

One SQL statement includes user existence/status and a limit+1 page. Limit 1-100, default 50. Newest (created_at DESC,id DESC), exact integer-microsecond key, cursor s1.<micros>.<sessionId>.<userId>. Cursor validates canonical UUIDs, positive signed-64-bit microseconds and selected-user binding. The bounded page is reused for item JSON and cursor metadata; keys used for pagination are not returned as private session credentials.

Each page is a separate snapshot. A session created after page one appears on refresh, not later older pages. Expiry, revocation and last-seen data can change between reads; the page is not polled. There is no new history index: sessions_user_active_idx covers only unrevoked rows; production-scale query plans/cost remain unmeasured. Server and browser limits follow existing admin list conventions; no global memory or latency guarantee is claimed.

## Admin UI

UsersView links to /users/:userId/sessions. Memory-only store scoped by admin epoch/id and selected user with generation/sequence checks. User/session/unmount changes invalidate all reads. Current 401/403 clears private data, invalidates earlier in-flight responses and settles the loader; recovery is an explicit refresh or navigation. No persistence or automatic retry. Only refresh and paging controls exist.

## Verification

Final isolated run completed on 2026-11-13. Frontend focused 8 tests, full 112 tests/8 files, both typechecks and isolated Vite build passed on the unchanged frontend tree. Strict YAML/ref check passed: version 0.50.0, 112 paths, 207 distinct refs, duplicate-key negative control refused (not OpenAPI semantics).

First disposable run usapp-e1-99dd33cb47 built successfully; existing ADMIN_WALLET 112, ADMIN_TASK 209 and ADMIN_REDEMPTION 205 passed. New ADMIN_USER_SESSION stopped with 73 assertions / 1 failure: a disabled-account fixture passed UPDATE text without a trailing semicolon into the maintenance helper's DO body. PostgreSQL reported unexpected end of function definition. The helper retried it, but syntax errors cannot recover. Only the fixture was corrected to terminate the statement; product SQL was unchanged. Full CTest and live HTTP were not reached in that run. Containers stopped and retained.

Not verified: browser/real admin login, production data/scale, shared database, cloud, real providers, deployment. Historical reconnect stalls, clock-guard intermittents and DbClient teardown crash remain open. NOT DEPLOYED; nothing committed.

## Final verification (2026-11-13)

Run `usapp-e1-1fc360b797`, full harness exit 0:

- Build passed. ADMIN_USER_SESSION **74 assertions / 2 cases**, ADMIN_WALLET 112, ADMIN_TASK 209, ADMIN_REDEMPTION 205; no failures.
- Real API binary **813 assertions** (46 added for the new route): both listeners enforce the admin boundary, ordinary/foreign user cookies do not authorize, no-store is present, tokens/hashes/raw user-agent fields absent, malformed/unknown ids and invalid/mismatched cursors refused, empty-user page and pagination correct. User-session fingerprint/count footprint unchanged across reads; admin-session bookkeeping excluded intentionally.
- Full CTest **24/24 passed in 96.75 s**, including admin_user_session_db 0.05 s. No timeout raised or assertion weakened.
- 48 migration checksums matched and all migration files remained unchanged. Eraser violations and privilege deviations after apply, repeat initialization and CTest: 0. Early/reversed erasures and ledger chain breaks: 0.
- E1 connection-loss 8 (release 1,034 ms, session_replaced), large export 6 and closure 23 assertions passed. A successful retry is not a fix for historical reconnect stalls. Closure mode deliberately left this disposable database CLOSED.
- Test-credential matches in this run's logs: 0; CTest crash markers: 0. Exact-owned compiler/database containers confirmed exited. Both runs' stopped resources were retained; no existing application stack modified.

Evidence: `Claude outputs/us-verify-1fc360b797/`, including the first failed fixture log and final targeted/HTTP/CTest logs. Original host work folder `%TEMP%/usapp-e1-1fc360b797`. Isolated admin build directory identifier: `usapp-us-admin-a5a5678f7c`; existing dist directories were not build targets.

The only change between the failed and passing backend runs was the semicolon in the disabled-account test fixture. Product query code was unchanged. Full SQL ledgers, production-scale plans, browser and real admin login were not run. Historical clock-guard, DbClient and reconnect issues remain unresolved. NOT DEPLOYED; nothing committed.
