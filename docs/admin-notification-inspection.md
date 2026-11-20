# Admin notification / push metadata (read-only)

Codex, 2026-11-13. Contract 0.51.0 adds `GET /api/admin/v1/users/{userId}/notifications`. No send, resend, claim, mark-read, subscription change, preference change, dispatch enablement or other mutation. No schema, grants, dependencies or CTest timeout change; 48 migrations 0000-0047 remain frozen, next 0048 unreserved.

## Authorization and privacy

AdminFilter requires peer CIDR, admin surface header and an independent admin session; the controller also checks its attached identity. Normal user cookies never authorize. Malformed ids/limit/cursor -> 400; unknown user -> 404 NOT_FOUND; existing empty/no-space user -> 200. The path id is lowercased before UUID validation. Disabled users remain inspectable.

Exact recipient selection, not current-couple public inbox semantics: all stored metadata addressed to this user is inspectable by the local admin. Notifications are scoped by recipient; deliveries also match recipient and couple; outcomes, attempts, observations and expiries join on the full notification/generation/attempt keys. No content/target lookup is done. Returned notification fields: id, event_type, created_at, read_at and push metadata. No actor, content target, body, intents/preferences, endpoint, p256dh, auth, payload, worker instance id or session tokens/hashes. The query never reads push_subscriptions or calls a transport.

## Stored facts and bounded display

One SQL statement includes user existence/status, snapshot read_at and limit+1 notifications, ordered by (created_at DESC,id DESC). Limit 1-100, default 50. Cursor n1.<integer-microseconds>.<notificationId>.<userId>, canonical UUIDs and positive signed-64-bit time; bound to selected user. Each page is a separate snapshot. New notifications after page one appear on refresh, not later older pages; receipts and push results can change between reads.

Each notification includes exact decimal-string total delivery count and the newest five delivery generations (created_at DESC,generation_id DESC). Each delivery shows its created_at, recorded final_state/decided_at and only its newest attempt by attempt_no. An attempt shows attempt_no, claimed_at, lease_until, expired_at and its optional observation. Observations expose only fixed class/outcome codes, http_status, retry_after_s and observed_at. Terminal states and observation classes are the existing database/dispatcher vocabulary, not a new state machine.

Accepted means provider acceptance, not device display. A missing delivery does not prove why no push occurred. A missing observation or an expiry does not prove no request was sent. Internal skips are separate from provider outcomes. No current retry eligibility/control/TTL state is inferred; a stored retry_after value is historical advice, not a promise of retry. The newest-attempt view is not a complete attempt history, and the five-delivery view may omit older devices. The UI says all of this explicitly.

The bounded page is materialized by its multiple references; only bounded child JSON is built (at most 101 x 5 latest attempts). Delivery totals and indexed latest-attempt lookups still depend on retained history size. No production-volume EXPLAIN, memory or latency guarantee is claimed.

## Admin UI

UsersView links to /users/:userId/notifications. The memory-only store scopes requests by admin epoch/id and selected user, with generation and sequence guards. User/session/unmount resets drop data and late responses. A current 401/403 invalidates earlier in-flight reads, clears shown data and settles loading; recovery is explicit refresh/navigation. Refresh and paging only; expanding stored details issues no extra request.

## Verification

Final backend verification completed; see below. Focused frontend 9 tests, full admin 121 tests/9 files, both typechecks and strict YAML/ref checks passed (0.51.0, 113 paths, 212 distinct refs; duplicate-key negative control refused; not OpenAPI semantics). Isolated Vite build passed (sandbox temporary directory identifier usapp-an-admin-70c35006f8; existing dist directories were not build targets).

Backend fixtures create notifications, read receipts and subscription parents through real services. Maintenance-only fixture inserts seed the immutable delivery/attempt/result combinations, without enabling control or making provider calls; database CHECK constraints remain active, while row guards are bypassed for these read-model fixtures. This verifies stored-fact projection, not real transport or dispatcher orchestration. The existing push_dispatch suite provides separate fake-transport regression evidence. No real Push, cloud/shared database, browser, real admin login or deployment acceptance is claimed.

NOT DEPLOYED; nothing committed. Historical clock guards, DbClient teardown crash and reconnect stalls remain open.

## Initial verification failure

Disposable run usapp-e1-d2274bc11f: build and existing admin wallet/task/redemption/session targets passed. New notification target stopped at a maintenance fixture: push_deliveries.created_at was NULL, because 0041 dropped its default and the fixture bypassed the timestamp-assignment trigger. The fixture now explicitly supplies clock_timestamp(); product query, schema and assertions were not changed. First run: 24 assertions / 1 failure; full CTest and live API were not reached. Its resources were stopped and retained. The final fresh run below passed.

## Final verification (2026-11-13)

Final disposable run `usapp-e1-2940666e63`, full harness exit 0:

- Build passed. ADMIN_NOTIFICATION **150 assertions / 2 cases**, ADMIN_USER_SESSION 74, ADMIN_REDEMPTION 205, ADMIN_TASK 209, ADMIN_WALLET 112; no failures.
- Real API binary **866 assertions** (53 added for this route), both listeners. Admin boundary, exact-recipient paging, unknown/malformed users and cursor mismatch, no-store, no secret/content fields, empty user and unchanged read/send footprint passed. Rich delivery/attempt combinations were verified at service level; the new HTTP fixture has no delivery rows, so it does not prove rich-record serialization end-to-end.
- Full CTest **25/25 passed in 96.25 s**; db_gated 13.68 s, push_dispatch 19.61 s, admin_notification_db 0.12 s. No timeout raised or assertion weakened. SQL ledgers were not separately rerun.
- 48 checksums matched; all migration files unchanged. Eraser/privilege deviations after apply, repeat initialization and CTest: 0; early/reversed erasures and wallet ledger-chain breaks: 0.
- E1 connection-loss 8 (release 1,039 ms, session_replaced), large export 6 and closure 23 assertions passed. This does not resolve historical reconnect stalls. Closure mode deliberately left the disposable database CLOSED.
- Generated test-credential matches in final-run logs: 0; CTest crash markers: 0. Exact-owned compiler/database containers confirmed exited. Stopped containers/networks from both runs retained; existing application stack untouched.

Evidence: `Claude outputs/an-verify-2940666e63/` (targeted, HTTP, CTest, server and first-failure logs plus migration-hash baseline). Original host work folder `%TEMP%/usapp-e1-2940666e63`. Frontend isolated build directory identifier: `usapp-an-admin-70c35006f8`.

Only the maintenance fixture's explicit created_at changed between backend runs; product SQL and frontend code were unchanged. No real Push/provider, browser/real admin login, production-scale query plan, identical-timestamp notification pagination fixture, no-space/disabled-user fixture for this new route, shared database, production or deployment acceptance. The reused cursor codec is covered with tied timestamps by the preceding session batch, not newly demonstrated here. Full attempt history and older delivery generations remain outside this batch. NOT DEPLOYED; nothing committed.

## Main files

- services/api/src/services/AdminNotificationService.{h,cc}, controllers/AdminNotificationController.cc; explicit controller registration in services/api/CMakeLists.txt.
- apps/admin/src/api/notifications.ts, stores/user-notifications.ts, views/UserNotificationsView.vue; protected route and UsersView entry.
- services/api/tests/AdminNotificationDbTests.h and AdminNotificationLiveApiTest.h; separate admin_notification_db target, static boundary check and harness registration.
- apps/admin/tests/unit/user-notifications.spec.ts; contracts/openapi.yaml 0.51.0; tools/export-verify.ps1 and Claude outputs/an-yaml-check.py.
