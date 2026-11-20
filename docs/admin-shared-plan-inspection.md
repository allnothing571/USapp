# Admin shared-plan inspection (read-only)

Codex, 2026-11-13. Contract 0.52.0 adds GET /api/admin/v1/couples/{coupleId}/shared-plans and /shared-plans/{planId}. Read-only local-admin list/detail; no editing, proposal decisions, advancement, conversion, memory attachment, notifications or other writes. No schema/grant/dependency/timeout change; migrations 0000-0047 frozen, next 0048 unreserved.

## Boundary and snapshots

AdminFilter checks peer CIDR, the admin surface header and independent admin session. The controller also requires the attached admin identity. Normal user cookies never authorize. Malformed ids, limits or cursor: 400. Unknown couple: SPACE_NOT_FOUND; foreign or unknown plan: identical NOT_FOUND. Existing empty/archived couples remain readable. Each response is one SQL statement/snapshot, not a cross-page or list/detail snapshot.

List: newest (created_at,id), limit 1-100/default 50, lookahead row. p1.<positive signed-64-bit microseconds>.<planId>.<coupleId> cursor, canonical lowercase UUIDs, bound to couple. The bounded page is materialized before usapp_shared_plan_read runs; authoritative JSON is materialized once per selected root. No status filter or total-plan count in this batch.

## Fields and privacy

Current status/body version project the existing usapp_shared_plan_read, not a new state machine. List fields: id, creator_id, created_at, title, body_version, status, status_version. Detail adds the five-field current body (title/date/time/place/note) and newest 50 events by seq descending, plus exact decimal-string total. Events expose id, seq, actor_id, operation, body_version, status_version, status, proposal_id, outcome, created_at and a nullable five-field body. Every child query matches both plan and couple.

The function's complete JSON is never forwarded. Body objects are rebuilt from the five named keys even if maintenance introduced extras. No request_key, raw request, fingerprints, audit command data, pending-request JSON, memory link/id, memory text or photo/media reference is returned. The query does not read shared_plan_memory_links, memories or media. Therefore it does not bypass deletion/RLS to recover hidden linked content. O12 remains: text independently stored in a plan survives deletion of an associated memory. This batch neither erases it nor claims it is memory text.

Historical proposals/decisions are facts, not a new current-pending-work queue. A returned event's body/status versions may be null. The UI states the cap and snapshot boundary. Older events remain outside the bounded detail response; no separate full-history endpoint is added. Counts and the underlying authoritative function still depend on history size; production-scale plans, latency and memory are unmeasured.

## Admin UI

CouplesView links to /couples/:coupleId/shared-plans/:planId?. Memory-only store has independent list/detail sequences and selected-plan generation within the admin session/couple scope. Changing selection, session or unmount invalidates older reads. A current 401/403 clears both scopes and invalidates every pending read; no automatic retry. The page offers refresh, paging, selection and local expanders only, no mutation control. Plan text is rendered as escaped Vue text with preserved line breaks.

## Verification

Scoped shared-plan verification completed; the overall harness failed in the existing export connection-loss mode (details below). New frontend 9 tests, both typechecks and strict YAML/ref checks passed: contract 0.52.0, 115 paths, 218 distinct refs; duplicate-key negative control refused, not OpenAPI semantics. Full admin 130 tests/10 files and isolated Vite build passed (temporary directory identifier usapp-sp-admin-4c31159c64; existing dist directories were not build targets).

Backend fixtures call the real shared-plan SQL write function inside explicitly committed transactions. They create/approve/reject/withdraw edits, advance status, compare current versions with the authoritative read, exercise a 55-event history with a 50-event cap, page roots and check couple isolation, field allowlists and unchanged write counts. Maintenance-only extra body properties test projection boundaries; this is not real deleted-memory/restore acceptance. Separate admin_shared_plan_db target retains the 30-second timeout outside db_gated.

No browser/real admin login, cloud, shared database, real storage, production-scale, rollback or deployment acceptance. Historical clock-guard, DbClient teardown and reconnect issues remain open. NOT DEPLOYED; nothing committed.

## First build failure

Run usapp-e1-69e1b3a73d applied the 48 unchanged migrations with zero privilege/eraser deviations, then stopped during compilation. The new list method passed a conditional const string temporary directly into Drogon 1.9.11's parameter binder, selecting its numeric template overload. The code now binds a named std::string and a named integer, as the preceding admin services do. Query semantics, assertions and timeouts were unchanged. No backend suite or HTTP test ran in that failed attempt; containers stopped and retained. The fresh run below compiled and passed the shared-plan checks.

## Final scoped verification and harness failure (2026-11-13)

Run `usapp-e1-7c5c8605ea`:

- Build passed after binding named parameters. ADMIN_SHARED_PLAN **220 assertions / 2 cases**; admin notification 150, session 74, redemption 205, task 209, wallet 112; no failures.
- Real API binary **942 assertions** (76 added for shared plans): both listeners enforce admin authentication/surface boundaries, user-cookie refusal, malformed/unknown/foreign subjects, cursor validation and scope, body/version/no-store response, unchanged write footprint and public caller scope. This HTTP fixture covers create/read only; edit/decision/history cap and extra-field projection are service-level checks.
- Full CTest **26/26 passed in 106.00 s**, db_gated 16.03 s, admin_shared_plan_db 0.21 s, push_dispatch 21.40 s. No timeout increased or assertion weakened.
- 48 checksums matched and migration files remained unchanged. Eraser/privilege deviations after apply, repeat initialization and CTest: 0; early/reversed erasures and ledger-chain breaks: 0. Generated test-credential hits in retained run logs and CTest crash markers: 0.
- E1 large export 6 and closure 23 assertions passed. Closure intentionally left the disposable database CLOSED.
- **Overall harness exit 1, NOT green:** EXPORT_CONNECTION_LOSS failed **5 of 9 assertions**. It deliberately terminated an export backend, answered deadline at about 19,985 ms, and after 80,000 ms had no cleanup callback and busySlots=1; the next export was refused by the global slot gate. Expected session_replaced/release assertions failed. This resembles earlier reconnect-stall observations; the cause remains unproven. No export source, limits, slot policy or its test was changed. No blind rerun was performed. The preceding normal export target still passed 444 assertions.
- Frontend: focused 9 / full 130 tests in 10 files, both typechecks and isolated Vite build passed. YAML/ref checks passed: version 0.52.0, 115 paths, 218 distinct refs, duplicate-key negative control refused; not OpenAPI semantic validation.

Evidence: `Claude outputs/sp-verify-7c5c8605ea/`, including successful targeted/HTTP/CTest, failed connection-loss mode and the first compile error. Original host folder `%TEMP%/usapp-e1-7c5c8605ea`. Both final-run containers confirmed exited; stopped resources from both attempts were retained. Existing app stack unchanged.

No real linked-memory deletion/closure/restore case, identical-time root paging fixture, archived-couple fixture, production-scale query plan, SQL-ledger rerun, browser/real admin login, real providers, shared/production DB, rollback or deployment acceptance. Memory links/ids and media references are intentionally absent from this admin surface, not an implementation of admin memory inspection. Historical clock-guard and DbClient issues remain open along with the export stall. NOT DEPLOYED; nothing committed.

## Main files

services/api/src/services/AdminSharedPlanService.{h,cc}; controllers/AdminSharedPlanController.cc; API CMake controller registration. apps/admin src/api/shared-plans.ts, stores/couple-shared-plans.ts, views/CoupleSharedPlansView.vue; protected router and CouplesView entry. AdminSharedPlanDbTests.h / AdminSharedPlanLiveApiTest.h; separate admin_shared_plan_db target and static/harness registration. apps/admin/tests/unit/couple-shared-plans.spec.ts, contract 0.52.0 and Claude outputs/sp-yaml-check.py.
