# USapp: Claude Code Implementation / Codex Acceptance Handoff

Latest C++ recovery Source (Codex, 2026-11-16): explicit pinned WireSource for the Python read-only protocol, separate MAC domain, fresh nonce/raw digest/context/full-checkpoint/canonical projection checks, at-most-once copied callback state. Required Source.binds(Request) guards coordinator after confirmed close and again before final head; mismatch/revocation never stages/records or opens. No main/provider/release factory. Final 2423c0644f passed unit3189 (8 new Source cases), CTest36/36 (106.03 s), HTTP1465, actual recovery HTTPS21 (coordinator Store FAKE), publication14, Python149 twice, whole harness and zero postchecks; 619 old cases retained +8=627, 54 migrations unchanged/0.60.0, next0054. Initial 5d75a4d4e5 green predates last hardening: a new signed raw-NUL regression exposed 3/3187 unit failures; both Source and publication parsers now reject NUL, final regressions passed. Older compiler35fb was mutated for probes; latest verified compiler is2423. Source/context/key custody/current authority and real Source/Postgres CLOSED drill, J3c/J4/J5 and release gates remain open. No SDK/frontend/provider/browser/backup rerun, cloud/real credential loading, commit or deployment. Keys removed, containers stopped; tmpfs DB data not retained. See docs/recovery-source-bridge.md and Claude outputs/recovery-source-20261116.

Latest read-only recovery material component (Codex, 2026-11-16): dedicated recovery principal/SDK transports forbid data writes; validates full external chain/indexes then rereads complete current metadata, binds configured external journal/restore/attempt ids and uses a separate recovery MAC domain. Missing/corrupt/moving/expired reads remain unknown, no initialization/partial snapshot/release. Recovery Head maps v2 batch seq/hash; unresolved contains per-subject intent ids. Direct-read and HTTP leases both retain resources. Final scoped 0d87082007 passed Python 149 in both runtimes (18 recovery core +6 TLS +125 previous), pinned SDK 188 reported/4 explicit old local-TLS skips, existing publication C++ interop14; source bindings/keys/stops checked. Two intended negative controls exposed next-call-after-deadline and direct-read teardown, fixed without weakening tests. No C++ Source adapter yet: C++ recovery consumption, independently active incarnation/fresh-head guarantees, main/release and J3c/J4/J5 remain open. No C++/DB/frontend/contract/migration/grant change; previous CTest36/36/HTTP1465 NOT rerun. 0.60.0/54, next0054. IMPORTANT: prior CLOSED means end-of-test only; PG data was tmpfs and is not retained by stopped containers. New owned PG required for a drill. No cloud resource/credential read, commit or deployment. See docs/journal-recovery-reader.md and Claude outputs/journal-recovery-reader-20261116.

Latest restricted journal backend composition (Codex, 2026-11-16): programmatic two-principal SDK/object/validating-metadata/Publisher/Dispatcher/TLS assembly, lazy worker-owned preflight with failure latch, no missing-head initialization, active-call resource retention, bounded shutdown cleanup and one listener owner. Existing resource restrictions/default network blocking preserved; no credential reader/daemon/main/recovery release. Final scoped f7a3bf8752 passed host/container Python 125 each, actual C++ HTTPS 14 assertions against conditional metadata fakes (2 operations/4 batches, one preflight, validator PUT 0, all three drains true, non-serving). Final pinned SDK offline 184 reported/4 explicit old local-TLS skips, exit 0; new six SDK tests ran. Initial response fixture hardcoded an id; generalized to actual synthetic credentials without disabling signatures. C++/DB/frontend unchanged: previous 35fb10e7d8 CTest 36/36 and HTTP 1465 are historical, NOT rerun here. Source bindings checked, keys removed, containers stopped; DB stayed stopped/CLOSED. 0.60.0/54 migrations unchanged, next 0054. No cloud resource/commit/deployment; production credentials/ownership, Source/head/restore binding and J3c/J4/J5 remain open. See docs/journal-backend-composition.md and Claude outputs/journal-backend-20261116.

Latest completed injected journal HTTPS service (Codex, 2026-11-16): TLS-only private literal address listener, authenticated bounded frames/connections, one serialized publication slot retained after timeout/disconnect until real local completion, pre-admission deadline check and separate network/backend shutdown reporting. No backend/credential factory, production main wiring or recovery permission; startup refusal unchanged. Final 35fb10e7d8 passed host Python 3.8.7/container Python 3.12.16 92 tests each, actual C++/Python HTTPS 14 assertions (2 operations/4 batches, non-serving), prior client 74, CTest 36/36 (105.64 s), HTTP 1465 and whole harness. All 619 ordinary cases preserved; source bindings checked, keys removed, owned containers stopped. First attempt omitted a test include; scoped 91-test continuation predates final deadline hardening and is historical only. Contract 0.60.0/54 frozen migrations unchanged, next 0054. No frontend/deployment/commit/cloud resource or restore release. Real backend, independent Source/head/incarnation and full J3c/J4/J5 remain open. See docs/journal-https-service.md and Claude outputs/journal-service-20261116.

Latest completed journal HTTPS client (Codex, 2026-11-15): optional programmatic libcurl-multi Exchange, mandatory TLS peer/name verification, fixed POST path, no proxy/redirect/retry, absolute I/O deadlines, bounded admission/streamed bodies/headers and cancellation-safe owner lifetime; SIGPIPE blocked only on its I/O thread. No Python HTTP service/backend/env factory or main wiring; startup refusal remains. Final 07bc76b458 passed client 74 assertions/9 cases, CTest 36/36 (112.04 s), HTTP 1465, synthetic wire transcript 12/15, whole harness and zero postconditions; all prior 610 cases retained plus nine. Earlier 47799fc2f6 passed before final hardening and is historical. Source hashes recorded; contract 0.60.0/54 frozen migrations unchanged, next 0054. No real DNS/provider/durability/recovery-release/browser/deployment acceptance or commit. See docs/journal-https-transport.md and Claude outputs/journal-https-20261115.

Latest completed injected journal wire bridge (Codex, 2026-11-15): optional C++ Journal adapter + Python injected Publisher dispatcher, strict raw-byte MAC/request-hash/nonce/journal/operation binding and conservative unknown/terminal mapping. No HTTP/socket/SDK/env factory or main provider wiring; startup refusal remains. New validated Publisher.lookup is operation binding only, not independent recovery Source/fresh-head authority. Python 74 passed; real C++->Python->C++ synthetic 12-request/15-assertion transcript passed (2 operations/4 batches). Full 169cf84da7 passed CTest 35/35 (108.95 s), HTTP 1465, whole harness and zero postconditions; all 605 old cases retained plus five unit cases. Contract 0.60.0/54 frozen migrations unchanged, next 0054. No transport liveness/durability/cloud/recovery-release proof, deployment or commit; complete real integration and J3c/J4/J5 remain open. See docs/deletion-journal-bridge.md and Claude outputs/journal-bridge-20261115.

Latest completed deletion startup refusal layer (Codex, 2026-11-15): partial J3c only, NOT a production journal/Source bridge or restored-data release proof. Before store/listeners/jobs/bootstrap, actual recovery-closed/error/null/timeout refuses the whole API; real media or purge requests also refuse while the durable provider is missing. No readiness override. Contract 0.60.0 and 54 frozen migrations unchanged, next 0054. Final ab46c00368 passed CTest 35/35 (112.82 s), HTTP 1465, open/closed DB 9/9, three actual process refusals with closed ports and whole harness; all 601 prior cases retained plus four new. Independent 41bd74214f verified real blocked-query timeout and missing-function refusal, restoration and zero privileges. No frontend/contract/backup rerun, cloud, release/restore/provider/browser acceptance, deployment or commit. Old backups can lose CLOSED itself, so the new gate alone never authorizes restoring/opening them. See docs/deletion-startup-refusal.md and Claude outputs/deletion-startup-20261115.

Latest completed admin operations snapshot (Codex, 2026-11-15): GET /api/admin/v1/system/status and overview implemented; contract 0.60.0, no migration (54 frozen through 0053, next 0054). Authenticated read-only explicit field allowlist, migration count/latest record and process config/capability, with no secret/raw-log/worker-liveness/readiness claims. Epoch/identity/sequence-cleared memory-only frontend. Final usapp-e1-671835cce9 passed CTest 34/34 (105.23 s), HTTP 1465, whole harness and zero postconditions; all prior 600 cases retained plus one static boundary case. Admin 243/17, both typechecks, isolated build and strict YAML/ref checks passed; original dist 20 unchanged. No online flag changes, remote backup/export/shell execution, real browser/login/provider acceptance, deployment or commit. V1 remaining workflows and release gates stay open. See docs/admin-system-status.md and Claude outputs/admin-system-20261115.

Latest completed memory-clock correction (Codex, 2026-11-15): frozen 0053 (74024a0bf259b619da08f00c1d9ff527d49564cc6e2f1a2fba499c29360b8d3e); 54 migrations, next 0054; contract unchanged 0.59.0. Both-role baseline/fixed controlled rollback proofs passed 13 cases each, including source-ahead and pre-trigger-default rewinds. Automatic decision time now follows the existing lock/deletion/actor/stale checks; supplied strict tests preserved. Final usapp-e1-2cb34d2d2d passed CTest 34/34 (106.71 s), HTTP 1417, memory deletion 1048, whole harness and zero postconditions. All 599 prior cases retained plus one static boundary case. Independent 54-migration backup/restore f02a7deef0 passed, with restored guard/default checks and app CONNECT denied. No frontend changes or browser/provider/device acceptance, deployment or commit; other clock/transport and V1 gates remain open. See docs/v1-clock-hardening-followup.md and Claude outputs/memory-clock-20261115.

Latest completed personal-security notices (Codex, 2026-11-15): owner-approved self-only wallet correction, admin cancellation/refund and new formal-session reminders implemented. Frozen 0052 (b8af60e34fa6652ce39961ed319d4786894fdf5ea4d714bd0e10f1cd8cff4dea); 53 files, next 0053; contract 0.59.0. Same-transaction mandatory source/link guards, original-key dedupe, minimal immutable login facts, recipient-only no-space inbox/receipts, fixed account push target and private payload rules. Final usapp-e1-84f38c2569 passed target 32/2, live HTTP 1417, CTest 34/34 (112.32 s), role/erasure/ledger zero and export/closed-domain extras. Web 968/48, fake worker 67/12, types and final isolated build passed, existing dist unchanged. Updated 53-migration backup/independent restore including NULL-space login receipt passed in d555d4355a. Earlier fixture/config/static failures and an old memory decision clock-guard failure remain recorded; clock issue not fixed or waived. No real provider/browser/device acceptance, deployment or commit. V1 still in progress; notices business-rule question is resolved. See docs/personal-security-notifications.md, docs/v1-completion-plan.md and docs/v1-clock-hardening-followup.md. Evidence: Claude outputs/security-notices-20261115.


Latest mandatory personal-security notice work (Codex, 2026-11-15): owner approved self-only admin wallet correction/cancellation and new authenticated-session reminders; no private reason, risk scoring, freeze or debit. Frozen 0052, 53 migrations, next 0053, contract 0.59.0. Same-transaction mandatory emit/link guards, immutable minimal login facts, private no-space inbox/receipts, fixed account push target and source-based dedupe implemented. Draft both-role proof and final standalone promotion/rollback/forgery cases passed; frontend 967 and fake worker 67 passed. Combined backend still in progress; old push/admin notification fixture expectations are being updated to include legitimate login notices while preserving their original assertions. Do not claim full green, browser/provider acceptance, deployment or V1 completion yet. Design/evidence: docs/personal-security-notifications.md.


Latest bounded admin exception and DB-backup checkpoint (Codex, 2026-11-15): contract 0.58.0; 52 frozen migrations through 0051, next 0052. Approved admin cancel/refund of unfulfilled redemptions implemented with original immutable price, independent true-admin fact/audit, same-key receipt replay and two-step UI; member history and legacy bounds preserved. Full disposable usapp-e1-5f6305059c passed CTest 33/33 (105.48 s), target 42/3, HTTP 1393, privileges/erasure/ledger zero and export/closed-domain extras. Admin 233, web 958, types and isolated builds passed; dist unchanged. Earlier new-test compile/static defects and journal upload fixture intermittency remain recorded. New signed/encrypted owner-run DB-backup tools passed separate fresh-instance synthetic restore in usapp-backup-probe-b413dcc15f, with exact schema/eraser/chain/privilege proof and application CONNECT disabled. No real source, production keys, scheduling, off-site upload, DB+object/journal clearance, browser/provider acceptance, deployment or commit. V1 still in progress. Evidence: docs/admin-redemption-exception-design.md, docs/database-backup-tools.md and docs/v1-completion-plan.md. Mandatory notice business-rule question is pending; do not invent anomaly scores or claim notices implemented.


V1 closeout ACTIVE (Codex, 2026-11-14): owner asked to finish all minimum V1, not stop after one read-only batch. Owner approved bounded audited asset corrections (+/-9999), admin cancel/refund of unfulfilled redemptions only, existing five badges/current avatar assets, and exclusion of optional pictures/photo-binary export/extra achievements. Track remaining work in docs/v1-completion-plan.md. Asset correction now implemented through frozen 0049 (24924f637330465867b96373d619d43be765ee424b13346b03d50b8e3a462e96) and 0050 (450c99df43abc2822c6e047fac7796f86813fbe9e44a2afda74acd6c1aec81ba); true admin actor, atomic bounded child operations, unchanged legacy entry bounds and restored old missing-member SQLSTATE 23502. Contract 0.57.0; next migration 0051, 51 files frozen. Two-phase admin wallet UI preserves exact unknown commands; no original-receipt overwrite of current balances. Passing usapp-e1-5e706f9f3d: target 43, HTTP 1348, CTest 32/32 (98.20 s), role/financial/erasure checks zero and export extras passed. Admin 219/15, types/build passed, dist unchanged. Prior Origin-fixture and unit/static/legacy-error failures recorded, not erased. Compatibility SQL ledgers and a final log-only compilation are pending. Continue exception handling and operational closeout; do not claim V1 complete or deployed. Goal remains active. See docs/admin-wallet-adjustment-design.md and Claude outputs/admin-adjustment-20261114.

Latest admin reward-product inspection (Codex, 2026-11-14): two AdminFilter/independent identity-protected read-only GETs per couple/product. Highest revision gives current terms, price as 1-999 flower decimal string, direct/request mode, active/archived state, creator/owner and times; detail adds description. Immutable creation-time/id rp1 keyset with exact couple/resource cursor binding. Admin can inspect archived metadata; ordinary D14 visibility is unchanged. No purchase/receipt history, refund, corrections, mutations, keys or new hold policy. Protected admin route/link, strict validators and independent list/detail auth-loss/404/logout/unmount invalidation. Contract 0.56.0; no migration/grant/dependency/timeout change, 49 frozen, next 0049. Disposable usapp-e1-377da79553 passed: target 91, real HTTP 1300, CTest 31/31 in 98.73 s, postconditions zero, export extras and closed-media-domain checks passed. Admin focused 16, full 203/14, both typechecks, isolated build and strict YAML/ref check passed; original dist unchanged. Containers stopped/retained. NOT DEPLOYED, not committed; no browser/real-login/production acceptance. Existing operational release gates remain open. Evidence: docs/admin-product-inspection.md and Claude outputs/admin-product-20261114.

Latest admin anniversary inspection (Codex, 2026-11-14): two independent AdminFilter/identity-protected read-only GETs per couple/anniversary. Immutable created_at/id keyset an1 cursor; explicit current terms, pending full replacement and latest decision summary with approved_version. No complete history, computed day counts, pins, notification facts, keys, receipts, writes or new hold/deletion semantics. Frontend protected route and couple link, independent list/detail identities, auth-loss/404/logout/unmount clearing and strict validators. Contract 0.55.0; no migration/grant/dependency/timeout change (49 frozen, next 0049). Disposable usapp-e1-a770a904fa passed: target 93, real HTTP 1228, CTest 30/30 in 97.98 s, privileges/erasure/ledger zero, export extras and closed album/memory checks passed. Admin focused 16, full 187/13, both typechecks, isolated build and strict YAML/ref check passed; original dist unchanged. Containers stopped and retained. Not real-browser/login/production accepted; NOT DEPLOYED, not committed. Existing transport/clock/journal/restore release limitations unchanged. Evidence: docs/admin-anniversary-inspection.md and Claude outputs/admin-anniversary-20261114.

Latest admin memory inspection (Codex, 2026-11-14): independent AdminFilter/identity-protected four read-only GETs for per-couple memories, current body/photo metadata, paginated proposal history and original/proposed comparison. Exact couple/memory/proposal scope; immutable created_at/id keysets with context-bound cursors; deleted/purged parents hide all text history, recovery hold hides children through existing application-role RLS. All three photo snapshots withhold deleted media identifiers (position-only unavailable markers). Historical approval labels use their own response current_version. Admin UI has four independent request identities, global auth-loss invalidation, parent 404/hold clearing, no persistence or write controls. Contract 0.54.0; no migration/grant/dependency/timeout changes (49 frozen, next 0049). Disposable usapp-e1-3d71f582b6 passed: target 193, HTTP 1156, CTest 29/29 in 97.21 s, postconditions zero, export loss/large/closure 8/6/23 and closed-memory 8 passed. Admin 171 tests/12 files, typechecks, isolated build and strict YAML/ref check passed; original dist unchanged. A CMake path-registration mistake was fixed before the passing run; final diagnostic-only label correction rebuilt/static 1539 passed. Existing transport/clock/journal/restore limitations remain open. Not browser/real-login/production accepted, NOT DEPLOYED, not committed. Evidence: docs/admin-memory-inspection.md; Claude outputs/admin-memory-20261114.

Latest admin album inspection (Codex, 2026-11-14): independent AdminFilter/identity-protected read-only GET list/detail/items per couple and album. Current items and removal history use seq paging; cursor binds couple/album/view; album list uses microsecond/id keyset. Current counts and chosen/fallback cover match AlbumService. Deleted/purged media and recovery-held albums are hidden; a hold gives empty unavailable list and detail/items 404. Explicit metadata projection only: no keys, receipts, fingerprints, signed URLs or binary grants. Admin removal metadata does not restore partner media access. Admin page has separate list/detail/item identities, strict shape/context checks, global current-auth-loss invalidation and 404/hold clearing; memory only. Couple-table album link plus previously missing plan link. Contract 0.53.0; no schema/grant/dependency/timeout change; 49 frozen migrations unchanged, next 0049. Full disposable usapp-e1-81c4e573b9: build, album 181, HTTP 1045, CTest 28/28 (96.95 s), role/financial/erasure checks zero; export loss/large/closure 8/6/23 and post-closure album RLS 7 passed; harness exit 0. Admin 149 tests/11 files, focused 19, both typechecks, isolated build; admin dist 20/web dist 45 unchanged. YAML/ref parse passed (118 paths/223 refs, not semantic validation). Initial backend failures were new upload-owner/cover-result fixture errors; strengthened auth tests caught six retained-space failures, fixed without weakening tests or permissions. No real browser/login/provider acceptance, deployment or commit. Containers stopped/retained. Evidence: docs/admin-album-inspection.md; Claude outputs/admin-album-20261114.

Latest push command timestamp correction (Codex, 2026-11-14): new 0048_push_command_timestamp (sha256 3854c3f1f73a6ea9710de0ec14d5409c1add3183c6464125f8b19f7c7a2edec9), old 48 migrations unchanged. Only command recorded_at's default is dropped; after original lock/identity/session checks, omitted/DEFAULT/NULL is assigned max(clock, generation activation). Supplied invalid times stay strict UY004; keys, replay facts, grants, activation/revocation/dispatch cutover/TTL unchanged. Native probe observed an 886 us backward wall-clock step; 2000 instrumented samples and a push run did NOT reproduce the historical failure. Controlled clocks proved both omitted-time rejection paths on the original guard, then exact assignment on the fix under both roles; historical failed predicate remains unproven. Cutover test now checks registration and nonempty id before use. Full disposable usapp-e1-e990d56b9b PASSED: 49 checksums, zero eraser/privilege deviations (apply/repeat/CTest), two-role clock controls 6 each, subscription ledger 22 each, build, HTTP 942, CTest 27/27 (98.66 s), financial/erasure postchecks zero, export loss/large/closure 8/6/23, harness exit 0. Final rollback-only old-guard negative controls plus corrected rechecks passed on a separate fresh-tmpfs continuation recovery-f78a7f47. No blind rerun, timeout change, product API/web/contract change, deployment or commit. Contract 0.52.0; next migration 0049. Generation/revocation clock exposures and export transport-stall root cause remain open; no real acceptance claimed. Evidence: docs/push-command-timestamp.md; Claude outputs/push-command-time-20261114.

Latest test-observer lifecycle correction (Codex, 2026-11-13): callback-tail probe reproduced Resource deadlock avoided (exit 134). A shared test-owner anchor now holds Observer clients through closeAll before final release; copies share the anchor, replacement closes the old client, and no leak/reaper or production-client change is introduced. Safe-close controls passed 100 rounds twice with weak-reference expiry; new observer_lifetime_db passed 78 checks. Run usapp-e1-f6a24ffdab: build, targeted suites and real HTTP 942 passed; FULL CTEST FAILED 26/27 in 102.67 s, previous db_gated crash absent (passed 14.68 s), new target passed 0.20 s. Remaining push_dispatch cutover/device re-enable registration failed USAPP_PUSH_COMMAND_TARGET then empty UUID; exact predicate/cause unproven, no push change or blind rerun. After-CTest checks/modes did not execute in that database. Separate fresh-tmpfs recovery-89c13487 passed EXPORT 449, LOSS 8, LARGE 6, CLOSURE 23; loss released 1009 ms through ordinary error handling, not proof of a transport-stall fix. 48 migration hashes/files unchanged; apply/repeat privileges zero; contract 0.52.0, timeouts unchanged. No web change, deployment or commit. Containers stopped/retained. Evidence: docs/test-observer-lifecycle.md; Claude outputs/observer-lifecycle-20261113.

Latest export callback-loss cleanup (Codex, 2026-11-13): deadline now also requests ordered ROLLBACK/token verification once final generation submission is published; publication occurs after submission and checks an already-answered deadline. No direct deadline release; earlier unknown BEGIN/settings/lock stages remain fail-closed. Added generation-reply-loss lifecycle regression (five checks). Contract 0.52.0, 48 migrations, limits, connection count and web unchanged. Run usapp-e1-91797e0762: build, EXPORT 449, live 942 passed; FULL CTEST FAILED 25/26 (97.02 s), default db_gated aborted with Resource deadlock avoided in DbClient/EventLoopThread destruction; export_db passed. Separate fresh-tmpfs focused continuation recovery-8ed9a2ad passed EXPORT 449, LOSS 8, LARGE 6, CLOSURE 23; loss released after 1009 ms through an ordinary error callback, so the intermittent driver stall is NOT proven fixed. Probe evidence includes failed repeated recovery; root cause remains unproven. No full green claim, deployment or commit. Containers stopped and retained. Evidence: docs/user-data-export-design.md Section 11.7; Claude outputs/export-recovery-20261113.

Latest admin shared-plan inspection (Codex, 2026-11-13): read-only per-couple list/detail, projected usapp_shared_plan_read status/versions, explicit five-field bodies, newest 50 events with decimal total. AdminFilter plus attached identity, exact-couple cursor/children, no raw requests/keys, memory links/ids/media, writes or decisions. O12 independent plan text retained. Protected admin page with independent list/detail identities and auth-loss invalidation. Contract 0.52.0; no migration/grant/dependency/timeout change. Scoped run usapp-e1-7c5c8605ea: build, ADMIN_SHARED_PLAN 220, live API 942, CTest 26/26 in 106.00 s; 48 checksums/files unchanged, privileges/ledger checks 0; admin 130 tests/10 files, typechecks, isolated build and YAML/218 refs passed. OVERALL HARNESS EXIT 1, NOT GREEN: existing EXPORT_CONNECTION_LOSS failed 5/9 after 80 s, cleanup absent and slot still held; no export changes or blind rerun, cause remains unproven. Large/closure modes passed afterwards. Initial compile failure was the new conditional const-string binding; fixed with named parameters before the scoped pass. Containers stopped and retained. No browser/real login/production acceptance, NOT DEPLOYED or committed. Evidence: docs/admin-shared-plan-inspection.md and Claude outputs/sp-verify-7c5c8605ea.

Latest admin notification inspection (Codex, 2026-11-13): read-only GET /api/admin/v1/users/{userId}/notifications, AdminFilter plus attached identity, exact-recipient keyset paging, read receipts and stored push facts only; newest five delivery generations and their latest attempt, decimal total. Fixed outcome/class/status codes only; no content/target ids, endpoints, keys, payloads or worker ids. No mark-read, send/retry/claim, preference or control changes. Provider acceptance is not device display; missing records do not prove no send, and retry eligibility is not inferred. Protected admin page, identity-scoped memory-only state and auth-loss invalidation. Contract 0.51.0; no migration/grant/dependency/timeout change. Final disposable run usapp-e1-2940666e63: build; ADMIN_NOTIFICATION 150, live API 866, CTest 25/25 in 96.25 s; 48 checksums/files unchanged, privilege/financial checks 0; E1 loss/large/closure passed (not a fix for earlier stalls). Admin 121 tests/9 files, both typechecks, isolated build, YAML/212 distinct refs passed. First run stopped because a triggers-off fixture omitted guard-assigned created_at; fixed fixture only, product unchanged. Rich push facts tested at service level, HTTP boundary fixture has no deliveries. Containers stopped and retained; no browser/real-provider/production acceptance. NOT DEPLOYED, not committed. Evidence: docs/admin-notification-inspection.md and Claude outputs/an-verify-2940666e63.

Latest admin user-session inspection (Codex, 2026-11-13): read-only GET /api/admin/v1/users/{userId}/sessions, AdminFilter plus attached admin identity, exact-user keyset paging and timestamp/revocation metadata only; no tokens/hashes/raw user agents/IPs/rotation links, pre-auth rows excluded, no touch/renew/revoke. Revoked > expired > valid (valid describes time/revocation, not login permission); account status separate. Admin UsersView link and session/user-scoped memory-only page with stale-answer/auth-loss invalidation. Contract 0.50.0; no migration/grant/dependency/timeout change. Final disposable run usapp-e1-1fc360b797: build; ADMIN_USER_SESSION 74, live API 813, CTest 24/24 in 96.75 s; 48 checksums/files unchanged, privileges/financial checks 0; E1 loss/large/closure modes passed (not a fix for earlier reconnect stalls). Admin 112 tests/8 files, both typechecks, isolated build and strict YAML/207 distinct refs passed. First run 99dd33cb47 stopped on a missing semicolon in the disabled-user test fixture; fixture corrected, product query unchanged. Both final containers stopped and retained. No browser/real-login/production acceptance; NOT DEPLOYED, not committed. Evidence: docs/admin-user-sessions.md and Claude outputs/us-verify-1fc360b797.

Latest admin redemption inspection (Codex, 2026-11-12): implemented read-only GET /api/admin/v1/couples/{coupleId}/redemptions and /redemptions/{redemptionId}, AdminFilter plus attached admin identity, status-filtered keyset paging scoped to couple/filter, authoritative reward_redemption_state, immutable product snapshot and decimal-string price, newest 100 events with total and fulfillment/cancellation facts; no keys/fingerprints/operation ids, writes, refunds or asset corrections. Admin CouplesView link and session/space/filter/detail-scoped memory-only page; authorization loss invalidates all pending reads. Contract 0.49.0; no migration/grant/dependency/timeout change. Final disposable run usapp-e1-4dff189ce9: build; ADMIN_REDEMPTION 205; live API 767; CTest 23/23 in 96.86 s; 48 checksums and migration files unchanged, privilege/ledger checks 0. E1 connection-loss/large/closure passed this run, not a fix for historical reconnect stalls. Admin 104 tests/7 files, both typechecks, isolated build; strict YAML/205 distinct refs passed (not OpenAPI semantics). First run stopped at an introduced SQL-macro quote typo; corrected before the passing fresh run. Test containers stopped and retained. No browser/real-login/production acceptance; NOT DEPLOYED, not committed. Evidence: docs/admin-redemption-inspection.md and Claude outputs/ar-verify-4dff189ce9.

Latest admin task inspection (Claude Code, 2026-11-12): admin-only, read-only GET /api/admin/v1/couples/{coupleId}/tasks (?kind=task|template|period&limit&cursor; stored rows only, no time window, newest first by (created_at,id); cursor t1.<filter>.<micros>.<taskId>.<coupleId> bound to couple and filter; one statement per page, no cross-page snapshot) and /tasks/{taskId} (one statement: row, completion, newest 200 progress events + total, reaction, root proposals/decisions newest 100 + total, closure with successor and applies_to_this_task, replaces). Unknown couple 404 SPACE_NOT_FOUND, empty couple 200 with no items, foreign or unknown task 404 TASK_NOT_FOUND. Status from the shared TaskStateSql.h expression (moved verbatim out of TaskService.cc). Decimal-string rewards; no keys, fingerprints, wallet operation ids, notifications or reminder rules. AdminFilter plus admin identity. Contract 0.48.0; no migration, grant or dependency (48 frozen, 0048 unreserved). Admin app: CouplesView link, route /couples/:coupleId/tasks/:taskId?, memory-only store with separate list (couple+filter) and detail (couple+task) identities. Verified: admin 94 tests/6 files (13 new; a mutation check failed 1 and 3 race tests as intended), both typechecks, isolated build, admin dist 20 and web dist 45 unchanged; strict YAML check. Codex completed backend verification on 2026-11-12, run usapp-e1-a5abe99599: build passed; ADMIN_TASK 209 assertions, live HTTP 687, CTest 22/22 in 99.46 s; 48 checksums and migration files unchanged; eraser/privilege/ledger checks 0; E1 connection-loss (8), large (6) and closure (23) modes passed. The earlier reconnect stall remains unexplained, not fixed by this pass. Codex also reran admin 94 tests, both typechecks, isolated build and strict YAML/ref checking. Test containers stopped and retained. No source correction was needed; browser/real-login acceptance remains pending. Evidence: docs/admin-task-inspection.md Section 8 and Claude outputs/at-verify-a5abe99599. Harness: ADMIN_TASK suite added; if E1 EXPORT_CONNECTION_LOSS fails, that failure is retained and the overall run ends NOT green. This run passed. NOT DEPLOYED. Evidence: docs/admin-task-inspection.md Sections 7-8.

Latest admin wallet race correction (Claude Code, 2026-11-11): apps/admin stores/user-wallet.ts only. (1) A 401/403 from any current wallet read now clears private data, settles both loaders, keeps the authorization message and invalidates every read in flight (no delayed 200 can repopulate; no auto-retry; session store untouched). (2) Summary identity = admin session + user, ledger identity = + asset: an asset change no longer strands a pending same-user summary (summaryLoading ends, refresh usable); old-asset ledger answers stay ignored; user/session/unmount still invalidate both. Regressions written first: 4 of 5 failed on the original store for the intended reasons; after the fix 18/18 focused, full admin 81 tests/5 files, both typechecks, isolated build, admin dist 20 and web dist 45 unchanged. One new test segment was corrected (it clicked a deliberately disabled button). No backend/contract/financial change; backend run usapp-e1-06d5f0ba61 (CTest 21/21) and the separate E1 connection-loss failure stand. No browser/production acceptance. NOT DEPLOYED. Evidence: docs/admin-wallet-ledger.md Section 6.1.

Latest admin wallet inspection (Claude Code, 2026-11-11): admin-only, read-only GET /api/admin/v1/users/{userId}/wallet (both balances; existence, membership and balances from one statement) and /wallet/ledger?asset=&limit=1..100&cursor= (newest first by the per-user/per-asset seq; cursor w1.<asset>.<seq>.<userId> bound to user and asset; one statement per page; summary and pages are separate snapshots). AdminFilter (peer CIDR, admin surface header, admin session) plus controller admin check; user cookies never authorize. Malformed id 400, unknown user 404, no space 200 with space null and no accounts. Exact decimal strings (no -0), allowlisted fields, no keys/fingerprints/lock tokens/actors/other users; no writes. WalletService and GET /api/v1/wallet unchanged (D21). Admin app: Users link, read-only wallet page, memory-only store scoped by session epoch/admin and user/asset. Contract 0.47.0; no migration, grant or dependency (48 frozen, next 0048 unreserved). Verified with fakes only, run usapp-e1-06d5f0ba61: ADMIN_WALLET 112, live 607 (both listeners), CTest 21/21 in 99.67 s (new admin_wallet_db), privilege deviations 0, ledger chain breaks 0, migration hashes unchanged; admin 76 tests/5 files, both typechecks, isolated build, admin and web dist unchanged; YAML/ref check. Runs 1-3 failed on test fixtures (unique constraints, chain post-check), fixed without weakening checks; 2 web-store bugs fixed (-0, lost auth message). Run 4 then stopped in the pre-existing E1 connection-loss mode (reconnect stall >60 s; slot held fail-closed). No browser/production acceptance. NOT DEPLOYED. Evidence: docs/admin-wallet-ledger.md Section 6.

Latest E1 resource-control correction (Claude Code, 2026-11-11), owner-approved: (1) admission lifetime: each of the 2 slots is a long-lived dedicated single-connection session (2 extra PostgreSQL connections per process, clients never destroyed); each export tags its session with a fresh token (application_name) before BEGIN, checks the tag on every later statement, ends with its own ROLLBACK and releases the slot exactly once only after a token-based check confirms no tagged backend is open or holds 33026; otherwise the slot stays held (unconfirmed, retried on a timer); no backend is ever terminated; callbacks outside the mutex. Drogon 1.9.11 Transaction::rollback has no completion signal, so TxScope is no longer used here. (2) browser: requestDownload reads the body as a stream, counts actual bytes, cancels and refuses at the cap (declared oversize refused unread), no unbounded fallback (DOWNLOAD_STREAM_UNSUPPORTED); request() unchanged. Withdrawn: the 2 x 16 MiB API memory claim. Final run usapp-e1-babdaf6cdb: 48 checksums, privilege deviations 0, export 444 (5 cases incl. end requested/pending/ended, lost BEGIN reply with original open, unrelated pid), live 560, CTest 20/20 in 96.91 s (export_db 3.91 s), harness-only connection loss released session_replaced ~1 s after termination, large and closure passed; web 957 tests/47 files, both typechecks, isolated build, dist unchanged. Runs 8-9 failed (unreliable transaction check; connection-loss case exceeding export_db 30 s, moved to harness-only, no timeout raised). Reconnect after a server-side loss took >20 s once: availability limit, slot held meanwhile. Contract 0.46.0 and privacy scope unchanged. NOT DEPLOYED. Evidence: docs/user-data-export-design.md Sections 7.1, 8.2, 11.6.

Latest E1 user-data export (Claude Code, 2026-11-11), owner-approved Q1-Q6: authenticated POST /api/v1/me/export (exact Origin/CSRF, body absent or exactly {}, no idempotency key) returns one versioned JSON attachment (schema_version 1, no-store/nosniff) of the caller's own account data plus currently authorized shared-space records; partner-private wallet/purchases/loadouts/achievements/reminders/notifications/email excluded; own wallet ledger and expired tasks beyond the 30-day UI window are deliberate new disclosures. Currently deleted memories/photos (undoable included) and all their history excluded; every photo reference uses the media visibleTo predicate, invisible ones are withheld markers without ids (partner private uploads in rejected proposals never exposed); J3-closed memory/album sections unavailable, plan links withheld, O12 plan text kept. READ ONLY READ COMMITTED transaction: shared lock (33026,0) in its own statement, then one generation statement; capped row sets measured before aggregation; document built once in a MATERIALIZED CASE-gated CTE; limits section 20000 rows, total 60000, input 15 MiB, exact output 16 MiB, lock 5 s, statement 15 s, end-to-end deadline 20 s, process-local admission 1/user, 2 global, 1 per 60 s (not cross-process). No audit/notification/wallet/journal/achievement write; no photo binaries, jobs or artifacts. Contract 0.46.0 (EXPORT_TOO_LARGE 413, EXPORT_UNAVAILABLE 503); no migration, grant or dependency (48 frozen, next 0048 not reserved). Web: requestDownload (request() unchanged), memory-only identity-scoped export store, MeView card. Verified with fakes only, final run usapp-e1-10f23629da: 48 checksums, privilege deviations 0, export 406, live API 560 (E1 live incl. Content-Length equals received bytes), CTest 20/20 in 134.24 s (new target export_db 2.78 s), large 8,500 rows/14.7 MB accepted in 583 ms and refusals, closure both lock orders; web 951 tests/47 files, both typechecks, isolated build, dist unchanged; YAML/ref check (not OpenAPI semantics). Runs 1-6 failed on labels, plan-gate placement, a duplicated document expression (both product fixes), harness order and test/harness text (recorded). No browser/device/real-backend acceptance. NOT DEPLOYED. Evidence: docs/user-data-export-design.md Section 11.

Latest achievements (Codex, 2026-11-10): five approved badge-only rules implemented end-to-end. New0047 frozen sha25675d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132; old47 unchanged; next0048. No reward/ledger/completion-path changes. Read-only derived badges, session/current-couple scope; maintenance-only immutable activation, never automatic. Contract0.45.0 GET /api/v1/achievements and web /achievements/Us entry. Final usapp-ach-240cb4530c:19prevalidation/role,48checksums,zero role/consistency deviations,target22,real local HTTP528,CTest19/19(97.65s). Web20focused/940full,two types,isolated build pass. Previous18/19unit failure was an incomplete new-table fixture, fixed without changing privileges; final copy includes counted-progress/replay tests. Exact owned stop logs exit0; Docker later already off. No shared/production activation, deployment, commit, cloud, browser/device or vault action. Review docs/phase4-achievements.md for projection-vs-persisted-award semantics, cutover/clock/source-retention limits, retained logs and failures. Long-history performance and exact commit-order activation remain unverified.

Latest B5 (Codex, 2026-11-10): /admin/#/avatar implements registry/catalog/history reads plus confirmed create/revise/list/unlist and starter configuration using existing contract 0.44.0 and backend audit functions. Unknown sends retain their cloned request/key; a later 4xx does not settle them; first definite refusal requires dismissal and new confirmation. Session epochs clear action/data/form state, late answers ignored. No asset enrollment, preview URL construction, grants or balance writes. Offline evidence Claude outputs/b5-admin-3ed9ec5caf: focused 30; full admin 4 files/63 tests; two typechecks and isolated production build pass. Existing admin/web dist, contracts and migrations unchanged. No backend/schema/dependency/deployment, cloud, DB, CTest, commit or vault action. B4 follow-up owner reported purchase and wardrobe/save/home success after supplying a 375px shop screenshot; remaining browser checks explicitly stopped, not marked passed. Fake server stopped. See docs/phase4-b5-avatar-admin.md; pending real admin/browser/audit acceptance, registry enrollment, final artwork and release gates.

Latest B4 (Codex, 2026-11-10): user-facing avatar shop, wardrobe and home with developer SVG placeholders. Final offline verification is green: 46 focused, 920 full web tests (45 files), both typechecks, isolated production build; 14 built art hashes verified, no art precache entry, dist 45 files and migrations 47 unchanged. No backend, schema, contract (still 0.44.0), dependencies or deployment changes. Registry data is review-only; no catalog/default seed or grants. Runtime browser attempt halted before navigation because the Windows tool could not determine the URL; no visual/device/live acceptance. The local fake server is stopped; the temporary incognito window may need owner closure. No CTest/cloud/provider/database/production action or vault update. B5 and formal art remain separate. Detailed files, state semantics, failures and enrollment prerequisites: docs/phase4-b4-avatar-web.md.

Latest Phase 4 B3 avatar loadouts (2026-11-09), owner-approved: additive 0046_avatar_loadouts (sha256 2b136ef9650201dc1a569e35078a7607107669d3020f8006af92f55b42f2ba69) adds append-only avatar_loadout_events (whole six-slot loadout per save, versioned per user from 1; 0 before the first save; resolved ids stored), guard UA007 (contiguous versions, category, starter-or-own-purchase eligibility, requested/resolved agreement, defaults equal to the recorded configuration), lock 33031 and usapp_avatar_loadout_save: per-user lock, key classification first, membership (SPACE_NOT_FOUND outside a couple, as purchases), version, a null required slot takes the configured default from one configuration under shared 33028 (none configured = AVATAR_DEFAULTS_UNAVAILABLE, nothing saved), null optional = empty, eligibility, event and audit. Fingerprint over requested terms, so replays keep captured defaults after a default change. No wallet, item or catalog lock or row. POST /api/v1/avatar/loadout (session caller only; loadout = original facts, current.appearance = today). GET /api/v1/avatar gains loadout_version and source saved_loadout (additive). Contract 0.44.0 (strict YAML parse passed with a duplicate-key negative control; not OpenAPI semantics); errors AVATAR_SLOT_INVALID, AVATAR_NOT_OWNED, AVATAR_DEFAULTS_UNAVAILABLE; next migration 0047. New CTest target avatar_loadout_db. Verified with fakes only, run usapp-b3-1cdc420ecb: prevalidation 27/7, 47 checksums, privilege deviations 0, 11 SQL ledgers, targeted 157/114/85, live API 519, CTest 18/18 in 98.38 s, post-checks 0. Earlier runs failed on the B1 ledger A8 TRUNCATE list and a committed test control row (both deterministic, test-only fixes). Real acceptance deferred. NOT DEPLOYED. Evidence: docs/phase4-avatar-shop-design.md Section 7.

Latest Phase 4 B2 avatar purchases (2026-11-09), owner-approved: additive 0045_avatar_purchases (sha256 c714684ce8d58d59bd0d5ac4208f1b408211eac39498706cb9aa514419c47205) adds the AVATAR_PURCHASE posting type (superset, no other), append-only avatar_purchases (snapshot = ownership; unique per user+item and user+key), guards UA005 (snapshot equals the current listed revision of a paid item; operation matches buyer, couple, key, fingerprint) and UA006 (deferred: exactly one purchase and one buyer sunshine debit of the snapshot price), lock 33030, and usapp_avatar_purchase: key lock, classification before any catalog read (replay keeps original facts after reprice/unlist), membership, item lock 33027 (shared with catalog revisions), catalog checks, ownership, the buyer sunshine account row lock, balance, writes and audit, then its four named deferred checks immediately (not SET CONSTRAINTS ALL). Sunshine only; never equips, refunds, gifts or grants. Routes: POST /api/v1/avatar/purchases (Idempotency-Key, server fingerprint; purchase = original facts, current = today, separate) and GET /api/v1/avatar/owned (own paid items incl. unlisted, paginated; starters are not owned). Outside a couple: SPACE_NOT_FOUND. Admin items gain owner_count. Contract 0.43.0 (strict YAML parse passed with a duplicate-key negative control; not OpenAPI semantics); errors AVATAR_ALREADY_OWNED and AVATAR_ITEM_UNAVAILABLE; next migration 0046. New CTest target avatar_purchase_db. Verified with fakes only, run usapp-b2-883f3cedf2: prevalidation 23/6, 46 checksums, privilege deviations 0, ten SQL ledgers incl. wallet/task/product/redemption, targeted 156/114, live API 488, CTest 17/17 in 95.01 s. Earlier runs failed on a SET CONSTRAINTS ALL leak (product defect, fixed), the wallet ledger E9 TRUNCATE list, a test compile error and database-wide B1 test assertions (all deterministic, fixed). Real acceptance deferred. NOT DEPLOYED. Evidence: docs/phase4-avatar-shop-design.md Section 6.

Latest Phase 4 B1 avatar catalog (2026-11-09), owner-approved B1 rules: additive 0044_avatar_catalog (sha256 8f1930eadf26f0a5ebd89f77d1e199d85e6b8802ed25d169d9e6eeab3cef7b1f) adds a maintenance-only registry of bundled versioned asset keys (avatar_assets, read-only to usapp_api), append-only avatar_items / avatar_item_revisions / avatar_starter_configs, guards UA001-UA004, locks 33027-33029, and three write functions that classify the admin's idempotency key before any catalog check and audit in the same transaction. Category, starter flag and appearance family are immutable; one item per family; prices 1-9999 for paid items, null for starters; items never deleted; corrections only move forward within a family. Users: GET /api/v1/avatar/catalog (global, listed only) and GET /api/v1/avatar (B1 truthfully source starter_defaults; complete=false with null slots when no configuration exists; deterministic configured defaults, never the latest item). Admin (AdminFilter boundary): assets/items/item/starter reads and create/revise(list-unlist)/starter writes with Idempotency-Key, expected versions and atomic audit; no ownership counts until B2. No wallet, purchase, ownership, equip, web or art. Contract 0.42.0; four AVATAR_* errors; next migration 0045. New CTest target avatar_catalog_db (TIMEOUT 30, outside db_gated, needs USAPP_TEST_MAINT_CONNINFO). The database cannot tell listeners apart: routing and admin session/Origin/CSRF are application-enforced. Verified with fakes only: rollback prevalidation 43/9 cases; 45 checksums, privilege deviations 0 after apply, repeat apply/InitRoles and CTest; 9 SQL ledgers both roles/modes; run usapp-b1-a81041af61 targeted avatar 151 assertions, live API 443 on both listeners, CTest 16/16 in 94.45 s. Run usapp-b1-791a952386 failed one avatar assertion from a test-fixture error (fixed, no product change). Real browser/device/cloud/restore acceptance deferred. NOT DEPLOYED. Design and evidence: docs/phase4-avatar-shop-design.md.

Latest D20 verification follow-up (Codex, 2026-11-09): full CTest passed 15/15 in one run (141.05 s). The first follow-up reproduced only db_gated's 30 s timeout; the four existing reminder suites were then moved unchanged to reminder_services_db, using their existing focused mode and the same timeout. db_gated now 25.41 s, reminders 8.36 s; 179 old default cases = 150 + 29, no omissions/duplicates; one new static case. 44 frozen migrations unchanged, contract still 0.41.0, no product or web changes. Finalization recovered a PowerShell warning-handling error after the green CTest run, confirmed six consistency counts and privilege deviations zero, stopped owned containers. The general slowdown and historical intermittent defects are NOT fixed. Web/worker checks were not repeated; live acceptance remains deferred. Details: phase3-plan.md Section 30.6.

Latest D20 opt-in partner notifications (2026-11-09), owner-approved D20 rules 1-8: createMemory, createSharedPlan and addAlbumPhotos accept optional notify_partner (default off, omitted = false, never remembered: no preference_seq, not in the choices read); at most one generic notice to the other member (by slot) in the write's own transaction, one per successful album batch (source album_photo_batches row), never per photo; no partner = success with no_partner. Responses carry original facts; same key never adds a notice; changed choice = IDEMPOTENCY_KEY_REUSED (fingerprint for memory/album, intent comparison for plans). Plan conversion never notifies (service skips; resolver refuses plan-created memories). Edits, requests/decisions, renames, covers, removals, plan states, deletion and undo stay silent. Push worker closed kinds + memory/shared_plan/album; payload unchanged. Migration 0043_phase3_notifications (sha256 48a0cbb8...44952; additive, 0041 guard bodies kept, no new timestamp correction elsewhere) FROZEN in disposable instances only, not shared usapp_test or production; a build needs 0043 at startup; next migration 0044. Contract 0.41.0. New CTest target phase3_notification_db (TIMEOUT 30) outside db_gated. Web: new PartnerNoticeToggle (no remembered value) on new memory, new plan and album batch only; captured with payload/key, locked while pending/unknown; facts must match the captured choice. Verified with fakes/stub push only: rollback prevalidation 20 refusals per role; 44 checksums, privilege deviations 0, repeat apply/InitRoles, 10 SQL ledgers both roles/modes; targeted D20 114 / memory 260 / album 726 / memory deletion 1048, live API 394. CTest: run usapp-n3-f71cd70496 13/14 (static_check: 3 superseded pre-D20 assertions, then updated); run usapp-n3-bebaf6bf51 12/14: db_gated TIMEOUT 30.01 s after 175/179 cases with no failed assertion (28.08 s in run 2; thin margin, D20 intent writes in its memory/album suites may contribute) and media_db USAPP_MEDIA_TIME (known clock-step intermittent, unproven). No single run passed all 14 targets; push worker 66/66; web 873 tests, both typechecks, isolated build, dist not written. Three superseded pre-D20 static assertions updated (review point). Next: owner decides on a rerun or a db_gated split. Real Push/browser/device/cloud/restore acceptance deferred. NOT DEPLOYED. Evidence: docs/phase3-plan.md Section 30.

Latest upload capability (2026-11-09): GET /api/v1/media/capabilities returns {uploads_enabled} (no-store) for a space member, the same MediaService::uploadsEnabled() predicate as the timeline/album-list fields (kept); no store call, grant or write; SPACE_NOT_FOUND outside a space. A snapshot: upload routes still answer MEDIA_DISABLED. Web: identity-scoped media-capability store with epoch tokens (own read, list-page observe, MEDIA_DISABLED outranks in-flight enabled answers); pickers and direct memory/album pages use it instead of loading unrelated lists; unknown/loading/failed means no new upload, with explicit retry; READY/unconfirmed uploads and previously uploaded photo choice stay available, and the new-memory page and plan convert editor keep their form when uploads are off. Correction (same day): plan convert now ensures capability without loading the memory list (link still loads it); new spec 3 tests, web 851, both typechecks; no backend change. Contract 0.40.0; no migration, next 0043. New CTest target media_capability_db. Verified with fakes only: web 848 tests, both typechecks, isolated build, dist unchanged; backend run usapp-cap-f5a5666dbf failed one unattached_db assertion (USAPP_MEDIA_TIME, 0029 guard, consistent with the known backward clock step, unproven, log retained), owner-approved rerun usapp-cap-7ffc2a0c6f CTest 13/13 in 124.77 s, live API 360. db_gated took 27.61/27.81 s in both runs (15.44 s previously, same assertions; all DB targets slower after Docker restart, unexplained). 20 existing web tests updated to the new mechanism. Real acceptance deferred. NOT DEPLOYED. Evidence: docs/phase3-plan.md Section 29.

Latest unattached-upload recovery (2026-11-08): GET /api/v1/media/unattached lists the caller's own READY photos in the current couple that may take their first home (shared USAPP_MEDIA_UNHOMED in MediaEligibility.h, also used by the album photo check: no everything plan, no binding ever, no pending reservation, not deleted), newest upload first by (uploaded_at, id), items media_id/uploaded_at/width/height only. A snapshot, not a reservation: listing or choosing renews no grant and moves no cleanup deadline; attachments still decide under their locks. Not the retained list or a deletion recovery. Memory editor and album adder offer 'choose previously uploaded photos' with combined order, limits and dedup; pending retries keep captured ids. Contract 0.39.0; no migration, next migration 0043. New CTest target unattached_db (TIMEOUT 30), outside db_gated. Verified with fakes only: run usapp-ua-ed5e2a4829 CTest 12/12 in 91.49 s (db_gated 15.44 s, unattached_db 58 assertions), live API 347, 43 checksums; web 839 tests, both typechecks, isolated build with dist unchanged. Earlier run usapp-ua-e7226a053c failed one live assertion from an invalid interval in the new fixture (fixed). Real browser/device/cloud/restore acceptance deferred, not passed. NOT DEPLOYED. Evidence: docs/phase3-plan.md Section 28.

Latest album rename-request history (2026-11-08): GET /api/v1/albums/{albumId}/proposals lists one album's rename requests as summaries (status, base/approved name version, decider; no names, keys or fingerprints), newest first by (created_at, id), with current_version from the same statement; names stay in the existing detail read. Both members only; another couple, no space, absent or malformed ids answer ALBUM_NOT_FOUND (as listAlbumPhotos); photos and covers move no name version; the read writes nothing. Web album detail shows the history, separates historical approvals from today's name, opens original/proposed names, and leaves deciding to the existing controls. Contract 0.38.0; no migration (0032 index), next migration 0043. New CTest target album_history_db (TIMEOUT 30), outside db_gated. Fixed a watcher in both detail views that re-read history on equal-version object replacement. Verified with fakes only: run usapp-ah-6250d956c3 CTest 11/11 in 89.56 s (db_gated 15.70 s, album_history_db 97 assertions), live API 334, 43 checksums; web 827 tests, both typechecks, isolated build with dist unchanged. Real browser/device/cloud/restore acceptance deferred, not passed. D20 untouched. NOT DEPLOYED. Evidence: docs/phase3-p32b-album-rename-design.md Section 8.

Latest db_gated split (2026-11-08), test infrastructure only: journalmedia/journalmemory/journalpurge moved from the default db_gated tail to new CTest target journal_services_db (same binary, USAPP_TEST_ONLY_JOURNAL_SERVICES=1, TIMEOUT 30); db_gated sets USAPP_TEST_SEPARATE_JOURNAL (direct full runs still include them); single-suite focused modes unchanged; one static check. Case lists from LastTest.log: 506 before, 507 after (only the new static check added), 24 moved cases run once in journal_services_db, none duplicated or missing. Run usapp-dbsplit-d828577d99: CTest 10/10 in 92.35 s, db_gated 15.34 s, journal_services_db 6.42 s; repeats 17.19/7.13, 17.78/7.84, 19.63/10.55 s. Earlier run usapp-dbsplit-c3ba24e4c6 passed CTest 10/10 but repeat 2 failed db_gated once (ReminderDbTests.h:347, USAPP_REMINDER_TIME created_at guard outside 0041; consistent with the known clock step, unproven; log retained). Both targets slow as one database accumulates rows. No product, contract, migration, dependency or timeout change. Evidence: docs/phase3-plan.md Section 26.

Latest memory edit-request history (2026-11-08): GET /api/v1/memories/{memoryId}/proposals lists one memory's proposals as summaries (status, base/approved version, decider; no text, photos or keys), newest first by (created_at, id), with current_version from the same statement. Both members only; another couple, absent, deleted or purged memories answer MEMORY_NOT_FOUND; the read writes and reserves nothing. Web memory detail shows the history, separates historical approvals from the current version, opens decided requests in the existing comparison (moved to MemoryProposalCompare.vue), and leaves deciding to the existing controls. Contract 0.37.0; no migration (0030 index serves it), next migration 0043. Verified with fakes only: run usapp-ph-87023dece9 build, targeted memory_deletion 1048 / memory 260 / live API 304 assertions, CTest 9/9 in 103.90 s (db_gated 29.31 s of 30 s, pre-existing thin margin), 43 checksums match; web 810 tests, both typechecks, isolated build with dist unchanged. Real browser/device/cloud and restore acceptance deferred, not passed. Notifications/D20 untouched. NOT DEPLOYED. Evidence: docs/phase3-plan.md Section 25.

C11a terminal-only entry ready (2026-11-08): owner-provided post-timeout head #3 / operation intent observed. Default-dry-run terminal finalizer either verifies #4 read-only or reuses exact uploaded object for one original independent Metadata.commit from #3; no new object PUT, initialization, fault injection or cleanup. 257 offline cases passed (244 root, 9 unit, 3 terminal workers, 1 original loss worker), other groups not rerun. No actual credential/cloud access this coding batch. Scenario digest e4d793887c8706d9d3f209c96b50ae141cf84d280420d310fa415fbe5ed05902; docs/phase3-j4c-terminal-finalize.md records scope and known limits. Do not mark original timed-out C11a run completed or repeat old scripts. Actual terminal confirmation pending owner-run command.

C11a ready for review (2026-11-07): default-dry-run restricted reply-loss trial plus separate scenario artifact implemented; 261 offline cases passed in five bounded groups, no skipped deadlines. SDK-success marker + UNKNOWN + reaped/no timeout is the only injected-unknown continuation; recovery/replay is read-only and mutation attempts audited. Local hot SQLite journal recovery only after child reaped, no quota reset. No real credentials/cloud, no new dependencies/app/schema/deploy changes. Actual extension from 2 to 4 batches and two objects awaits explicit approval; old exact-two-batch tools must not run afterward. Plan, evidence and intermediate failures: docs/phase3-j4c-reply-loss-trial.md. C11b and J3 remain deferred.

J4c11 plan/offline protocol preparation (2026-11-07): new design and five test-only cases, no live runtime/CLI or cloud access. Root 244 passed; final five separately passed after stronger read-only replay assertions. Proposed first C11a SDK-success-then-discard, then separate C11b shared-predecessor competition; no real packet-loss/parallel-vendor proof. Preserve fixed deadlines, independent writer validation, exact candidate evidence, no auto retries/cleanup/initialization. Current journal extension invalidates old exact-two-batch checkers by design. Details docs/phase3-j4c-concurrency-fault-plan.md. No service/schema/contract/config/dependency change; J3 deferred.

J4c10 actual v3 result, owner-provided (2026-11-07): three exact negative actions passed; 15 stages ready, all reaped, no timeout; before/after chain/bootstrap/guard validation passed. Cumulative 91 requests already includes 51/30 prior consumption, one denied object PUT, one synthetic guard row; no new log batch or serving/cleanup. Original v2 guard-create cause remains unknown. Evidence recorded in docs/phase3-j4c-permission-trial.md. No cloud call or credential access by agent in this documentation update. Next prepare separately bounded concurrent/unknown-commit tests; no implicit cleanup, repeat initialization or J3 resume.

J4c10 guard-resolution preparation (2026-11-07): owner v2 guard-create result cannot distinguish original write outcome; generic UNKNOWN was incorrectly mapped to conflict, corrected with fixed error categories. Console query returned no row. Explicit guard-failure evidence + read-first acknowledgment now gates a fresh manual attempt, carrying cumulative 51 request counters; matching row means facts-only reuse/no rewrite, absent allows one conditional synthetic insert, mismatched/unknown stops. Final root 239 offline cases passed, no real credential/cloud access or old worker-group rerun. See docs/phase3-j4c-permission-trial.md. Do not delete prior evidence or claim original guard write succeeded.

J4c10 read-only timeout follow-up (2026-11-07): duplicate complete-chain read removed, bootstrap/post-guard checks separately bounded, 5s unchanged. Final offline 233 root cases + 1 new split-verification worker case passed; other groups not rerun. Explicit new manual attempt can validate exact old pre-write timeout evidence and carry its consumed counters, leaving old artifacts intact. No automatic write resume, credentials or actual cloud calls. Owner previous run reached no guard/probe writes; remote read-lease expiry unknown. Details docs/phase3-j4c-permission-trial.md.

Latest J4c10 (2026-11-07): restricted conditional permission-test runner added. Offline 228 root + 1 owned-TLS/spawn test passed. No real credential/cloud execution in this batch. One pure synthetic guard row, two negative PutRow requests and one reader PutObject request; strict permission-denial codes only, no generic auth failure/409 as success. Positive controls before, full chain and guard checks after, fixed 5s stages, unknown stops, evidence retained. User J4c9 real positive run and exact two-object/88-request evidence recorded separately. See docs/phase3-j4c-permission-trial.md; do not claim all operations/resources/permissions or failure recovery verified. J3 deferred.

Latest J4c9 (2026-11-07): restricted fresh-only synthetic cloud-trial CLI implemented, dry-run default. Final offline 216 root + 2 new spawned/owned-TLS cases passed. No real credentials/cloud writes; actual initialization/write permission remains separate. Trial precharges existing quotas, records candidate keys before PUT, fixed synthetic IDs, unknown stops without retry/cleanup; no resume or service wiring. Full details and failures: docs/phase3-j4c-live-trial.md. User terminal outputs separately confirm real bucket and table configuration/start-abort probes, not full provider/restore acceptance. J3 remains deferred.

Latest J4c8 (2026-11-07): default-dry-run resource preflight implemented and offline tested, 19 new / 195 root cases passed. Bucket checks require a separate exact-bucket two-action read policy; not yet bound. Table probe describes, starts and aborts only, never commits or writes; lost replies may leave a lease, report unknown. User-provided actual identity/resource setup evidence recorded separately. No real credential or cloud call in this coding batch, no deployment. See docs/phase3-j4c-resource-preflight.md for logs, sandbox failure, limits and next manual step. Do not treat identity or configuration success as authority/checkpoint/live-storage verification. J3 remains deferred.

Latest J4c7 (2026-11-07): default-dry-run read-only identity preflight, external owner-protected Windows credential reader, ACS3 known-answer signing and three-principal STS check implemented. New 24 cases; 183 cases passed this batch in four bounded groups; other 19 worker cases not rerun. No real credentials/cloud, IAM creation/binding, resource/initialization/publish, Docker build or deployment. Explicit Run is ONLY up to three identity reads and is not user authorization by itself. Root/role/wrong account/name, unsafe ACL/path/link, expired/unbound/duplicate keys and unconfirmed workers refused. Default 2 s object-test premise failed once; separate 5 s test retains all outcome checks, no production-fix claim. Next needs separate approval for three no-data-policy RAM users and read-only verification; billable resource and data permissions remain later. See docs/phase3-j4c-identity-preflight.md.

Latest J4c6 (2026-11-07): offline restricted initialization and parent-owned SQLite trial quotas implemented; whole-partition probe, atomic bound bootstrap receipt, replay without resetting heads, confirmed initialization before publish, conservative reservations and fail-closed budgets. 178 distinct cases passed across eight bounded processes; see docs/phase3-j4c-initialize-trial.md for exact logs and intermediate failures. Real SDK/local TLS fakes only, no live/cloud/resource initialization/credentials/deployment or J3 action. Version-1 review bundle unchanged and verifiable; new generation v2 reflects offline initializer progress. Live trial/authority/fresh-lineage, real storage and release gates remain open.

Owner-authorized Docker maintenance completed: build cache removed, offline VHD compaction succeeded, original three services restored; all 701 container IDs, 51 unique image IDs and 3 volume names retained. C free observed 60.51 GiB. No Docker build during resumed Python verification. See docs/local-docker-space-maintenance-20261107.md; no permission to delete retained history was granted.

Latest J4c5 (2026-11-07): account declaration confirmed, local concrete review bundle docs/reviews/j4c-e3784866 generated and verified. Bucket usapp-j4c-log-e3784866, instance uj4ce3784866, journal 0afe377a-3e06-4134-997f-4f1a437b8492; not created or availability-checked. Recovery-reader policy now includes Start/Abort needed by Metadata.read, no data-write/Commit privilege. Twelve new package tests plus prior baseline: 138 passed this run (acda70080a); prior 12 worker tests not rerun. No cloud permissions applied, real credentials, resource/initialization/deployment action, migration or contract change. Package integrity does not verify ownership/authorization; zero seed is not a trusted latest head. Next: offline restricted initializer and enforceable live-test bounds, then separate concrete creation/live approval. See docs/phase3-j4c-resource-confirmation.md.

Latest J4c4 (2026-11-07): bounded full-publication worker plus offline manifest/preflight implemented. Synthetic keys only, default networking blocked; real SDK tested against owned loopback HTTPS stateful fakes. Five independently bounded groups passed: original 126 + preflight 6 + publication 2 + post-commit deadline recovery 1 + refusal 3 = 138. Logs and timing in docs/phase3-j4c-worker-preflight.md. No cloud ownership/permission/durability proof, live mode, real credential/resource action, real initialization, deployment or J3 work. Early combined target timed out and one partial E lacks a captured cause; group split did not increase the 20 s per-process limit. Next: concrete account/resource confirmation, restricted initialization and a separately approved live trial. Do not interpret configuration_valid as ownership_verified or creation_authorized.

Latest J4c3 (2026-11-07): restricted OSS adapter plus real-SDK full publication codec tests, loopback TLS and owned-process deadline tests implemented. Combined 126 passed, exit 0, log Claude outputs/j4c-sdk-offline-719aec5649.log. No real cloud, credentials, resource creation, global trust/dependency change, migration, contract or deployment. Fake server atomicity is not vendor evidence; process watchdog covers injected object calls, not a wired real SDK publisher. No-overwrite requires independently verified never-enabled bucket versioning. Next: controlled full-publication worker, SDK local TLS/deadline and offline ownership/setup preflight; creation approval still separate. J3 deferred. See docs/phase3-j4c-objects-offline.md.

Latest J4c2 (2026-11-07): bounded metadata publication adapter implemented; missing head fails closed, complete chain validated before write transaction, conditional head plus all indexes committed together, lost commit receipt remains UNKNOWN without automatic abort. Combined 103 offline tests passed (14 new stateful fake-client cases and 3 real-SDK codec/condition cases); log Claude outputs/j4c-sdk-offline-e854a0d06f.log. Full publication remains fake-client/store evidence; real SDK covers empty-head reads and conditional RPC encoding. No live cloud, credentials, resource creation, migration, contract, deployment or J3 work. Next: object adapter, real-SDK full-chain offline integration, actual TLS and whole-call deadline. See docs/phase3-j4c-metadata-offline.md.

Latest J4c1 (2026-11-07): real pinned SDK offline facade prepared; default transport refuses I/O. tablestore 6.4.8 + 20 hash-pinned wheels installed only in task temp site, not global or production. Real codecs/signature handling tested against synthetic in-memory response pool; 20 new tests, combined 86 passed, exit 0 (Claude outputs/j4c-sdk-offline-7dc84b8a82.log). Explicit SDK and urllib3 no-retry settings, fixed endpoint/table/journal, transaction identity, conditional head request and Describe/Start/Abort preflight covered. First positive test failures were a response-fixture signing-date omission; checks were preserved. Real TLS, OSS object client, full atomic publication adapter, live worker deadlines, identity/permissions and anti-rollback unverified. No cloud/credential/resource action; 5 CNY budget does not authorize creation. J3 still deferred. Next live preparation scope/gates: docs/phase3-j4c-sdk-offline.md Section 6.

J4c budget confirmation (2026-11-07): Shanghai and 5 CNY test budget accepted. Continued preparation only; no billable resource creation, retention lock or deployment approval. The budget is not an automatic cloud billing ceiling. Client/offline preparation remains next; review exact live configuration before requesting creation approval. See docs/phase3-j4c-test-setup.md.

Latest J4c preparation (2026-11-07): Shanghai test region accepted; budget still pending. Prepared docs/phase3-j4c-test-setup.md with candidate isolated resources, conditional cost example, exact runtime permission templates, live acceptance/teardown gates and client-first order to avoid idle billing. Three placeholder policy JSON files parsed and statically checked; not bound or cloud-tested. No live client, SDK installation, credentials, real resources or billed actions. Suggested 5 CNY buffer is only a proposal, not owner approval or an automatic billing ceiling. Next preparation needs the real client's offline verification; resource creation and retention locking need separate explicit approval. J3 remains deferred.

Latest D6 J4b (2026-11-07): owner selected OSS + Tablestore design direction without authorizing billable resources. Implemented synchronous offline publication reference model with injected storage contracts; all adapters in tests are in-memory fakes. Twenty new tests, combined J1/J4a/J4b 66 passed, exit 0; logs Claude outputs/j4b-publication-tests.log. Thread-barrier tests cover identical operations, competing operations and conflicting payloads; no real process/cloud evidence. True checkpoint freshness/anti-rollback, live clients, bounded network calls, permissions, retention and cost remain OPEN. Full-history reads/prototype quotas are not a production design. No cloud/credentials/database/container/config/deployment action, no J3 continuation. Next: prepare authorized J4c test-resource setup after region/account/budget/client choices. See docs/phase3-j4-journal-provider.md Section 11.

Latest D6 J4a (2026-11-07): owner explicitly deferred further J3 work. Implemented a separate stdlib-only v2 batch envelope validator and 18 tests; unchanged J1 plus J4a = 46 passing tests, exit 0, logs Claude outputs/j4a-batches-tests.log. Validates full-operation identity, fingerprint, subject set and atomic acknowledgement representation; not a durable publisher or proof the producer captured every historical photo. Reports remain closed. docs/phase3-j4-journal-provider.md contains resource options, candidate publication protocol, credentials/retention/cost and zero-baseline cutover gates. OSS + Tablestore route is only a pending recommendation; no owner approval to create billable resources or lock retention. No real resource/credential/database/container/deployment action. Next J4b depends on chosen direction; J3b/c and J5 remain required before release.

Latest D6 J3a (2026-11-06): implemented CLOSED-only recovery-preparation protocol and fake Source/Store tests. Strict standalone C++17 compiled with warnings as errors: 124 assertions passed. Normal integration run usapp-j2c2-820f96000a: build passed; unit 2817, CTest 8/8 in 87.24 s, targeted journal cleanup/media/purge 147/79/1362; 42 migration checksums match; eraser violations and cleanup/erasure consistency failures 0. db_gated 28.95/30 s remains a timing risk. No production adapter or main wiring; all reports servingAllowed=false, including VerifiedClosed. Fake state retention is not real durability or restore verification. J3b PostgreSQL quarantine/replay enforcement and J3c startup/opening still required; J4/J5 remain open. No migration, contract, dependency, config, real provider or deployment change. Evidence and next adapter contracts: docs/phase3-deletion-journal.md Section 12.

Latest J2c2 Codex review repair (2026-11-06): lifetime epoch exhaustion and cross-source acknowledgement recovery repaired; script ownership checks avoid Chinese-path JSON decoding. Final disposable run usapp-j2c2-4edbb9ce0d exited 0: build passed, targeted cleanup/media/purge 147/79/1356 assertions, CTest 8/8 in 86.99 s, 42 checksums match, eraser violations and cleanup/erasure consistency failures 0. Python 28/28 passed. db_gated took 28.63/30 s; limits unchanged. New regressions cover epoch 17/18, reopened fake contents and both source orders. Prior attempts and the corrected legacy-backoff assertion are recorded in Section 11.10. Containers stopped and retained. No real provider, true process-restart/restore acceptance, deployment or J3. Historical crashes remain open. See docs/phase3-deletion-journal.md Sections 11.9-11.10.

Latest D6 journal J2c2 (2026-11-06): optional fake-journal protection for media object cleanup: first pass, sweep pass, late-object reconciliation and resumed-attempt removal. Intent is confirmed before any store call; journal and store calls run outside transactions and advisory locks; the effect rereads plan, cycle, sweep deadline and every key disposition, refuses any keep/live/unknown key, and records a pass only after all keys are confirmed absent. New narrow journal action cleanup (media only, cycle derived from the cleanup plan) for rejected/abandoned/unattached/leftover plans; user_deleted plans use purge on the exact deletion_id. Per-operation epochs (bound 16) let a later necessary removal run as a new operation without weakening delete/undo dedup; missing pass/late acknowledgements are reconciled by cursor. Journal-mode work selection rotates by a memory cursor with 300 s backoff after the bound. J1 validator accepts cleanup (28 tests). Run usapp-j2c2-c2ab8b8d8a: full build, targeted 92/79/1446 assertions, CTest 8/8 (82.72 s; db_gated 24.96 s of its 30 s limit), 42 checksums match, eraser violations 0, keep/live removal records 0. No migration, contract, dependency, provider or deployment change. Unjournaled legacy deletions are never purged in journal mode; real journal, legacy enrollment, retirement, replay/startup barrier (J3), J4 and J5 remain OPEN. Evidence: docs/phase3-deletion-journal.md Section 11. NOT DEPLOYED.

Latest D6 journal J2c1 (2026-11-06): optional fake-journal integration for MemoryPurgeJob text erasure. Select under scheduler transaction, external intent only after commit, guarded erase in a new transaction, then final acknowledgement. Up to 50 new due items plus 50 cyclic historical confirmation checks; scheduler completed means queued, not all erased. Run usapp-j2c1-cddf3fde72: full build, targeted 1422 assertions, CTest 8/8 (76.69 s), 42 checksums match, eraser violations 0, early/reversed erasures 0, unerased text/fingerprints 0; history 59, bound 3, actual 2 ticks. No production provider or flag configured; no migration/contract/dependency/deployment changes. J2c2 object cleanup, real journal, replay/startup barrier, safe retirement and restore gates remain OPEN. Evidence: docs/phase3-deletion-journal.md Section 10. NOT DEPLOYED.

Latest D6 journal J2b2 (2026-11-06): MemoryService delete/approve/cascade/undo integrated with injected fake journal. Frozen-batch lookup and request fingerprint binding, stable parent/photo cycle IDs, locked set revalidation, original guards and post-commit verification. No provider configured in main; no migration, grant, contract, dependency or deployment/config change. Run usapp-j2bm-1c24884148: full build, targeted 208 assertions, CTest 8/8 (72.57 s), 42 checksums match, eraser violations and cascade time/actor mismatches 0. Protocol 153 checks, J1 25 tests passed. Pending batches still need safe retirement/reconciliation. Purge integration, real journal, replay/startup barrier and restore drill remain OPEN. Evidence: docs/phase3-deletion-journal.md Section 9. NOT DEPLOYED.

Latest D6 journal J2b1 (2026-11-06): optional journal integration in MediaService deletePhoto and undoDeletion; authorization/preparation outside transactions, stable operation/cycle IDs, original locked effects and post-commit key-fence verification before external final acknowledgement. Injected fake journal only, no main/controller provider configuration. Disposable run usapp-j2b-e35aed5eb4: full build, 79 targeted assertions, CTest 8/8 (71.74 s), 42 checksums match, eraser violations 0. No migration/contract/production config change. Memory/cascade and purge integration, real journal, replay and restore gates remain OPEN. Evidence: docs/phase3-deletion-journal.md Section 8. NOT DEPLOYED.

Latest D6 journal J2a (2026-11-06): async C++ write-ahead protocol, journal/effect interfaces and in-memory fakes added. Standalone strict compilation and protocol checks passed in disposable no-network containers; synthetic transcript passed J1; J1 25 tests passed. NOT wired to services/jobs. Stable-key classification, authorization, frozen cascade revalidation, durable batch storage and checkpoint provider remain future integration. No migration/config/contract/deployment or real DB/storage operation. Evidence: docs/phase3-deletion-journal.md Section 7.

Latest D6 journal J1 (2026-11-06): offline strict hash-chain/checkpoint validation and conservative replay planning implemented in tools/deletion_journal.py; 25 stdlib Python tests passed. Always serving_allowed=false. No online writer, durable checkpoint provider, database replay or startup barrier yet. No migration, backend, contract, dependency, storage, database, config or deployment change. Production journal and restore gates remain OPEN. Evidence and J2-J5 sequence: docs/phase3-deletion-journal.md.

Latest D6 M2 frontend (2026-11-05): whole-memory delete/request/decision controls, exact-cycle recovery and retained-photo views; contract 0.36.0 unchanged. Fake-interface web tests and typechecks passed; isolated web build passed, no served dist overwritten. Real API/browser/device acceptance remains pending. No backend, migration, dependency, database, storage or deployment operation. NOT DEPLOYED. Evidence: docs/phase3-d6-memory-deletion-design.md Section 16.

Latest P3-2c (2026-11-04): shared albums web verified with fakes only; no backend or contract change; six contract gaps recorded; NOT DEPLOYED. Evidence: docs/phase3-p32-albums-design.md Section 13.
Earlier P3-2b (2026-11-04): album rename and requests implemented; D18 Option A approved; verified with a fake object store in disposable instances only; 0032_album_rename frozen there, not on shared usapp_test or production; contract 0.27.0; next migration 0033; NOT DEPLOYED. Evidence: docs/phase3-p32b-album-rename-design.md Section 7.
Earlier P3-2a (2026-11-04): shared albums backend implemented; D12, D9, D17, D8, D19 approved; verified with a fake object store in disposable instances only; 0031_albums frozen there, not on shared usapp_test or production; contract 0.26.0; next migration 0032; rename is P3-2b; old-build rollback rehearsal still required (docs/phase3-plan.md Section 18.1); NOT DEPLOYED. Evidence: docs/phase3-p32-albums-design.md Section 12.

P3-1d (2026-11-04): adapter verified offline; owner reports a basic real OSS and desktop browser flow passed on a Shanghai test bucket; signed URL expiry, overwrite races, late objects and most guarantees remain open; NOT DEPLOYED. docs/phase3-plan.md Section 24.

Earlier P3-1d planning (2026-11-03): real OSS setup and verification plan. docs/phase3-p31d-oss-plan.md.

Earlier P3-1c (2026-11-03): photo memories frontend, verified with fakes only (full web suite and both typechecks, plus a fake-data browser run); no backend, contract, migration or deployment change; NOT DEPLOYED. docs/phase3-plan.md Section 23.

Earlier P3-1b (2026-11-03): photo memories backend, fake object store only; 0030 frozen in disposable instances only; contract 0.25.0; a build needs 0030; no notifications, web or deletion; NOT DEPLOYED. docs/phase3-plan.md Section 22.

Earlier P3-1a (2026-11-03): private media, fake object store only; 0029 frozen in disposable instances only; contract 0.24.0; a build needs 0029; NOT DEPLOYED. docs/phase3-plan.md Section 21.

Latest P2-6 availability (2026-11-03): generation_enabled on the reminder list, contract 0.23.0, no migration; NOT DEPLOYED. docs/phase2-reminders.md Section 22.

Earlier P2-6 R5 (2026-11-03): stale reminder push suppression and bounded expiry; 0028 frozen, disposable isolated instances only, fake transport; a build with R5 needs 0028;
rollout not approved; NOT DEPLOYED. docs/phase2-reminders.md Section 21.

Earlier P2-6 R4 (2026-11-03): rule inheritance on approved edits; 0027 frozen, disposable isolated instances only; contract 0.22.0; a build with R4 needs 0027; R5 not
approved; NOT DEPLOYED. docs/phase2-reminders.md Section 20.

Earlier P2-6 R3 (2026-11-02): web controls and notification compatibility implemented; automated web tests only, browser/device acceptance pending; R4 and R5
not approved; NOT DEPLOYED. docs/phase2-reminders.md Section 19.

Earlier P2-6 R2 (2026-11-02): in-app generation implemented, off by default; disposable isolated verification only; no new migration; on neither shared
usapp_test nor main; R3-R5 not approved; NOT DEPLOYED. docs/phase2-reminders.md Section 18.

Earlier P2-6 R1 review repairs (2026-11-02): 0026 next-fire repair frozen, disposable isolated instances only; contract duplicate auth path merged;
R2-R5 not approved; NOT DEPLOYED. docs/phase2-reminders.md Section 17.8.

Earlier P2-6 R1 (2026-11-02): rule storage and API implemented; verified in disposable isolated instances only; 0025 frozen, on neither shared
usapp_test nor main; no generator; R2-R5 not approved; NOT DEPLOYED. docs/phase2-reminders.md Section 17.

Earlier P2-5c (2026-11-02): transport implemented and verified in disposable isolated stacks with stubs only; all real push
off by default; no real provider, real service worker lifecycle, device acceptance or deployment; shared usapp_test
still 0000-0023, main 0000-0017. docs/phase2-web-push.md Section 16.

Earlier P2-5b repairs (2026-11-02): verified in a disposable isolated instance with a fake transport only; 0024
is frozen; shared usapp_test still 0000-0023, main 0000-0017; NOT DEPLOYED. docs/phase2-web-push.md Section 15.

Earlier P2-5b (2026-11-02): implemented by Claude Code; automated usapp_test verification with a fake
transport only. 0023_push_dispatch is applied/FROZEN on usapp_test only; main remains 0000-0017.
NOT DEPLOYED; nothing real was sent. Evidence: docs/phase2-web-push.md Section 13.

Earlier P2-5a (2026-10-30): owner-approved; implemented by Claude Code; full automated usapp_test
verification passed. 0022_push_subscriptions is applied/FROZEN on usapp_test only; main remains
0000-0017. Subscription closed; NOT DEPLOYED; no device acceptance. Evidence: docs/phase2-web-push.md Section 12.

Earlier P2-4 (2026-10-30): owner-approved and implemented directly by Codex; full automated
usapp_test verification passed. 0021_task_reactions is applied/FROZEN on usapp_test only;
0000-0021 frozen on test, main remains 0000-0017. NOT DEPLOYED. Evidence and remaining
limits: docs/phase2-reactions.md Section 8. Earlier status paragraphs are dated history.

Latest (2026-10-29): P2-3a implemented/test-verified by Codex, 0019 frozen on usapp_test only. No main access or deployment. P2-3b (2026-10-30): implemented and test-verified on usapp_test by Claude Code; 0020 frozen on usapp_test only; not deployed. Details: docs/phase2-notifications.md Sections 11 and 13.

Updated: 2026-10-06, Asia/Shanghai. Later the same day: recorded owner-run acceptance results for the AdminFilter authentication fix (Section 2) and the HomeView member-label fix (Section 7, item 3). Subsequently: recorded the owner-made initial local Git baseline commit `b9cd0ea` (Section 6, "Git Baseline"). Subsequently: recorded documentation-only pre-push corrections to `README.md` and this file (Section 6, "Pre-Push Documentation Corrections"). Subsequently: recorded the owner-reported first push to a private GitHub repository, live administrator CSRF rejection checks, and scheduler heartbeat evidence (Section 6), and added the Phase 0 acceptance matrix `docs/phase0-acceptance.md`. Git hashes and repository status in this document are dated observations, not permanent current state; this document is not updated to record its own commits. Subsequently: recorded the owner's post-fix backend test timings and the pre-commit credential-pattern check (Section 6). Subsequently: recorded owner-run live user-side email-code Origin/CSRF negative checks and administrator session persistence after rejected logout requests (Section 6). Subsequently: recorded the owner's explicit acceptance of Phase 0 on 2026-10-06 as a local development baseline (Sections 1 and 6) and added the Phase 1 plan `docs/phase1-plan.md` (planning only). Statements elsewhere in this document that Phase 0 was "not accepted" or acceptance was "pending" are historical and describe the state when they were written. Subsequently: recorded the owner's approval of Phase 1 milestone M1 only, the C3/D3 correction, and Claude Code's M1 implementation, which has not been verified (Sections 1 and 6). Subsequently: recorded the owner-run M1 verification results (Section 6) and started the documentation-only M2 wallet/ledger design (`docs/phase1-plan.md` Section 9). Subsequently: recorded the owner's approved M2 design directions (D1, D2 bounds, D18, D19, D21) and revised the M2 design after review; M2 implementation remains unapproved (Section 1). Subsequently (2026-10-07): a second documentation-only M2 design revision (concurrency fixture scope, exact permission outcomes, effective privilege preflight, rollback-evidence limits); no approval status changed. Subsequently (2026-10-07): recorded the owner's approval of M2-A and Claude Code's M2-A implementation, verification pending (Sections 1 and 6). Subsequently (2026-10-07): recorded the owner-run M2-A verification on `usapp_test` (Section 6) and added the documentation-only M2-B design (`docs/phase1-plan.md` Section 9.15). Subsequently (2026-10-07): the owner assigned M2-B directly to Codex; implementation completed, verification pending (Section 6 and plan Section 9.16).
Latest evidence update (2026-10-09, Asia/Shanghai): M2-B passed owner-run verification, main 0006 is applied, and startup policy passed. See Section 6 and plan Section 9.19. Earlier delivery-time pending statements are historical. A separate formal M2 acceptance decision has not been recorded. Later on 2026-10-09: the owner committed and pushed M2-A/M2-B as `594a77b` (Section 6, "Phase 1 M2 Git Publication"; dated observation), and Claude Code wrote the documentation-only M3 design `docs/phase1-m3-design.md`. Later on 2026-10-09: M3-A was approved, implemented, and passed the recorded owner-run verification on `usapp_test` (Section 6, "Phase 1 M3-A Owner-Run Verification Completed"; design Section 11.3). Later on 2026-10-09: the owner committed and pushed M3-A as `6a9da4b` (Section 6, "Phase 1 M3-A Git Publication"; dated observation) and approved M3-B, which Claude Code implemented; verification has not been run (Section 6, "Phase 1 M3-B Approval and Implementation"; design Section 12). Later (2026-10-10): M3-B passed the recorded owner-run checks, 0007 was applied to the main database, and the M3-B runtime and web build were deployed (Section 6, "Phase 1 M3-B Owner-Run Verification and Main Rollout"; design Section 12.9). Later on 2026-10-10: the owner accepted M3-B as a local-development milestone, reported the M3-B commit `380696b` pushed to `origin/main`, and authorized M3-C design/planning only (Section 6, "Phase 1 M3-B Acceptance, Git Publication and M3-C Design Authorization"; `docs/phase1-m3c-design.md`).

Project root: `D:\JYJ\USapp`. Environment: Windows PowerShell.

Latest status (2026-10-20): M4 is deployed with owner-run automated, SQL and
representative live evidence in plan Section 10.8. The owner waived its remaining
live checks and started M5-A daily counting, assigned directly to Codex.
M5-A passed owner-run backend/web automated checks; 0009 is applied/frozen on
usapp_test and main; the M5-A runtime/web were deployed on 2026-10-22 and the
remaining live checks were waived, not passed: docs/phase1-m5-daily.md,
Verification. M5-B weekly counting (2026-10-22) is implemented by Claude Code,
Codex-run automated checks passed 2026-10-25; 0010 applied/frozen on usapp_test and main,
main API/web rollout completed 2026-10-25; live weekly workflows remain unverified: docs/phase1-m5-weekly.md, Verification results and Main rollout.
Earlier dated unapproved/pending statements below describe their original stage.

## 1. Responsibilities, Authorization, and Current Objective

**Phase 2 P2-1 (2026-10-27): implemented; owner-run backend/test-database verification
passed and Codex's frontend follow-up passed 266 tests. 0017 applied/frozen on test and
main; Codex deployed API/web on owner authorization. Phone and live message checks pending;
Evidence: docs/phase2-messages.md Sections 10, 12 and 13.**

**Phase 2 P2-2 (2026-10-29): design approved by the owner (O1-O10); implemented by Claude
Code; all usapp_test stages passed (prevalidation, apply with InitRoles, 0 privilege
deviations, CTest 3/3 with db_gated block 25, SQL script in both roles, web 289 tests and
typecheck). 0018 is applied and frozen on usapp_test only. NOT DEPLOYED. Evidence:
docs/phase2-anniversaries.md Sections 14-15.**

**Local LAN HTTPS (2026-10-27, owner-authorized): the canonical public URL is
now `https://10.6.245.114:8443` (`PUBLIC_ORIGIN`); `https://localhost:8443` redirects (308);
admin stays `https://localhost:8444` on loopback only. Every compose command must add
`-f .\deploy\docker-compose.lan.yml`, or api/caddy revert to localhost. The Wi-Fi switch
reused the current images and root CA. Android access on the current Wi-Fi is unverified. Details, certificate
fingerprint and rollback: deploy/LOCAL-HTTPS.md Sections 8–9.**

**M10 (2026-10-26): home and bottom navigation, owner-authorized for implementation by
Claude Code. Frontend only (no backend/contract/migration/grant change); web unit tests
and typecheck passed; frontend web published 2026-10-26 by Claude Code on owner
authorization (served hashes verified, API untouched). Browser/device/PWA acceptance
pending; Phase 1 not formally accepted. Details: docs/phase1-m10-home.md Section 8.**

**M9 (2026-10-26): directly assigned to Codex; request confirmation and mutual
cancellation/refund implemented and automated-test verified on usapp_test.
0016 frozen on test and main; owner-authorized main API/web rollout executed by
Claude Code 2026-10-26 (startup policies wallet 187 / task 972 ok). Live/browser
acceptance unverified; no Git publication or M10 work. Release evidence:
docs/phase1-m9-redemption.md Section 5.**

- Claude Code implements subsequent code changes, adds necessary regression tests, and revises changes based on acceptance feedback.
- Codex checks assumptions, reviews actual changes, designs acceptance checks, and guides the owner through validation one step at a time.
- **The owner manually runs builds, tests, database operations, container operations, installations, browser checks, and Git operations.** Neither Claude Code nor Codex should execute these actions unless the owner explicitly grants additional authorization.
- Commands in this document are references for the owner. Their presence does not authorize Claude Code to execute them.
- Follow the owner's current assignment: M5-A implementation is directly assigned to Codex. Claude Code implements only its assigned work. Do not ask again for permission for an explicitly assigned fix; obtain authorization for changes outside its scope.
- Preserve existing work. Do not reset, overwrite, or revert changes unrelated to the assigned fix. Git state is recorded as dated, owner-reported observations (Section 6): the initial baseline commit `b9cd0ea`, later a push to a private GitHub repository at `9e00049a6f4b669897ef1172047c4eb8bbdd7a57`, and on 2026-10-09 the M2 commit `594a77bcadb0d2742c0704f93aea20cb09a1e726` pushed to `main`. Treat any hash as the state at the time it was reported. Changes made after the last owner-reported state are uncommitted unless the owner reports a new commit; do not claim otherwise.
- Do not read the real `.env`. Do not disclose secrets, passwords, session cookies, plaintext verification codes, or invitation codes. Do not inspect database backups to obtain secrets.
- Deliver actual file differences, the reasoning behind the fix, and its verification status. Distinguish "files modified," "statically reviewed," "build passed," "tests passed," and "real workflow verified." Another tool's completion report is not test evidence.
- Use owner-approved fail-fast batches for independent checks; keep database/deployment stages guarded and wait for their results. Retain essential errors rather than lengthy logs.
- All content handed to Claude Code must be written in English. Codex continues to communicate with the owner in Chinese.

**M8 (2026-10-26): direct redemption, implemented/test-verified by Claude Code. Owner-authorized joint M7/M8 main rollout executed by Codex; 0014/0015 frozen on test/main, API/web deployed. Release evidence and unverified live/browser checks: docs/phase1-m8-redemption.md Section 7. M9/M10 not started; no Git publication.**

**M7 (2026-10-26): reward-shop products implemented/test-verified by Claude Code; 0014 frozen on test/main and API/web deployed jointly with M8 by Codex. See docs/phase1-m8-redemption.md Section 7 for release evidence and remaining limits.**

**M6 (2026-10-25): task edit/cancel with recipient confirmation, owner-approved for B1-B3 implementation and usapp_test verification by Claude Code (docs/phase1-m6-design.md). Implemented; 0013 applied/frozen on usapp_test and main; full CTest, web unit/typecheck and task/wallet SQL scripts passed; main API/web rollout completed 2026-10-25 (task policy 537 ok). Live workflows unverified; no acceptance recorded.**

**Previous objective (2026-10-25): M5-C custom date-range counting tasks, owner-authorized with test-scope build/test/usapp_test-migration commands delegated to Claude Code (docs/phase1-m5-custom.md). Implemented; migration 0011 and repair 0012 applied/frozen on usapp_test and main; full CTest, web unit tests/typecheck and task/wallet SQL scripts passed; main API/web rollout completed 2026-10-25 (owner-delegated to Claude Code). Live workflows unverified; no acceptance recorded.**

**Previous objective (updated 2026-10-25): M5-B weekly counting tasks, implemented by Claude Code; Codex-run backend/web automated and SQL compatibility checks passed (docs/phase1-m5-weekly.md, Verification results). 0010 is applied/frozen on test/main; main API/web rollout completed 2026-10-25, while live weekly workflows remain unverified. M5-A daily counting is deployed on main (0009 applied, startup policies ok, web built; docs/phase1-m5-daily.md, Verification); its remaining live checks were waived, not passed. Freeze 0000-0015 (all applied to test/main; M7/M8 release evidence: docs/phase1-m8-redemption.md Section 7). Do not build bind-mounted web dist before the new backend is healthy. M4 remaining live checks were waived, not passed (plan Section 10.8). No later Git publication is reported. Custom periods, edit/cancel, shop/redemption, undo and M6-M10 remain unapproved; D33 deferred; no reset, unrelated migration/grant, dependency or deployment-configuration change or timeout increase is authorized. The owner explicitly authorized Codex to apply 0010, repeat InitRoles and run database tests on usapp_test; the owner subsequently authorized and Codex completed main migration and API/web rollout; live weekly workflows remain unverified.**

Historical note: earlier on 2026-10-09 and 2026-10-10 the objectives were owner verification of M3-A, owner Git review/commit/push of M3-A, Codex review and owner verification of M3-B, and owner Git review/commit/push of M3-B; all are recorded in Section 6.

Historical note: until 2026-10-09 the objective was owner Git review/commit/push after M2-B verification; that step is recorded in Section 6.

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

### Phase 1 M2-A Approval and Implementation (2026-10-07; Verification Pending)

- The owner approved M2-A on 2026-10-07: database wallet/ledger foundation, reusable privilege checking, tests, and documentation. Excluded: `WalletService`, `WalletController`, contract/version changes, frontend, API startup integration, posting, redemption, M2-B, M3+.
- Claude Code added `database/migrations/0006_wallet_ledger.sql`, a re-runnable wallet section 7 in `database/init-roles.sql`, `database/tests/wallet_ledger_test.sql`, `services/api/src/db/WalletPrivilegePolicy.h/.cc` (not wired into startup), `services/api/tests/WalletLedgerProbe.h` and `WalletLedgerDbTests.h` (block 11 of `DbGatedTest.cc`), unit and static test cases, and `database/README.md` notes. Details, refinements, and unverified assumptions: `docs/phase1-plan.md` Section 9.13.
- The `db_gated` migration gate now requires `0006_wallet_ledger`, so `db_gated` fails until 0006 is applied to the test database.
- Review repair (2026-10-07): `db_gated` now checks `current_database()` before any fixture write or test case and exits with failure (not Skipped) unless it is exactly `usapp_test`; `wallet_ledger_test.sql` raises before any fixture DML unless `current_database()` is `usapp_test`. Covered by a pure `unit` case (new `services/api/tests/TestDatabaseGuard.h`) and a `static_check` ordering case; the rejection path was not run against any real database. When written, not built or tested.

### Phase 1 M2-A Owner-Run Verification on usapp_test (2026-10-07)

Recorded separately from the implementation entry above. M2-A evidence only.

- DryRun: the six existing migration checksums matched; only 0006 pending; exit 0. `usapp_api` and `usapp_push` existed and were not superusers.
- 0006 applied to `usapp_test` and `init-roles` completed, exit 0. Applied checksum `2633e17ea6ff6e664832df84023d1fab8aa9513b4cfe48786c4ca093bc7d543d`; this applied migration must never be edited. Repeated `init-roles`: exit 0.
- Test image build exit 0. CTest `unit`, `db_gated`, `static_check` Passed, `db_gated` not skipped; 0.01 / 0.88 / 0.00 s, total 0.90 s; exit 0. A later database-only CTest run: one test, no failures, total 1.07 s, exit 0. The full CTest run preceded the script-only repairs below.
- psql script: the first application-role run failed (exit 3, "syntax error at end of input" at the internal `THEN` of an unparenthesized `CASE` in an `IF`). The first maintenance-role run failed with exit 3: E9 expected UW001 but received 55006. Codex, under the owner's authorization, repaired the standalone script: seven `IF`/`CASE` conditions in E2-E8 parenthesized, and the maintenance E9 now runs `SET CONSTRAINTS ALL IMMEDIATE` before testing `TRUNCATE`. Retries: application role exit 0 (banner and group count not supplied); maintenance role 28 assertion groups passed, `ROLLBACK`, exit 0.
- Final `usapp_test` counts: accounts 0, operations 0, entries 0, wallet test users 0 (query exit 0).
- Limits: 0006 not applied to the main database; no API startup integration, wallet endpoint, frontend, or posting path; existing-account serialization and post-commit snapshot visibility untested (M3); targeted checks, not exhaustive security verification. No formal M2-A acceptance decision is recorded.
- Claude Code did not execute commands or SQL. The owner-run verification above supersedes the original pending-verification status; it does not establish the deferred M3 concurrency guarantees.

### Scheduler Heartbeat (Owner-Run)

- Runtime configuration: scheduler enabled (`enabled=true`), tick interval 15000 ms.
- Main-database heartbeat query, excluding the migration placeholder: 1954 completed records; latest timestamp `2026-10-06 08:41:54.591309+00`; age 13.137962 seconds at query time.
- This confirms that heartbeat jobs were executing and completing in the running service at that time.
- Limits: a single point-in-time query. It is not live multi-instance concurrency verification; advisory-lock mutual exclusion and rollback release are covered by the `db_gated` test only.

### Session Migration to Claude Code (2026-10-06; Documentation and Configuration Only)

- Work moved from a Claude (Cowork) session to Claude Code. Two files were added: `CLAUDE.md` (standing instructions for Claude Code) and `.claude/settings.json` (project permission rules denying Git, container, package-manager/install, build/test, and database commands, and reads of real environment files and `backups/`).
- Limits: documentation and configuration only. No code, test, contract, migration, or deployment file changed, and no build, test, database, container, or Git command was run. This adds no runtime verification and is not Phase 0 acceptance. Whether the installed Claude Code version enforces every rule in `.claude/settings.json` has not been verified.

### Phase 1 M2-B Direct Codex Implementation (2026-10-07)

Historical verification update (superseded by the owner-run entry below): owner test-image build exit 0; first full CTest had
unit/static_check Passed and db_gated Failed (591 assertions, 2 failures, exit 8).
Two new audit queries referenced nonexistent actor_user_id. Codex repaired them
to actor_type='user' AND actor_id. Owner rebuilt (exit 0) and reran full CTest:
all three Passed, db_gated not skipped, total 1.06 s, exit 0. First web test run
passed seven files/52 tests but failed to load home-wallet.spec.ts (exit 1).
Codex enabled the existing Vue plugin in the standalone Vitest config; web rerun
pending. See plan Sections 9.17-9.18.
The original delivery status below is historical; M2-B is not accepted.

- Owner authorized Codex directly for this milestone. Added GET /api/v1/wallet,
  own-balance WalletService/WalletController, OpenAPI 0.3.0, bounded startup
  WalletPrivilegePolicy enforcement and the memory-only HomeView wallet card.
- Startup checks run after DB readiness, before filters/listeners/scheduler/run;
  failure/timeout refuses startup. Session failures/account changes clear wallet
  state; load/reset generations discard late successes and failures.
- Added backend unit/database/static tests and four web test files. Exact changed
  files and verification limits: docs/phase1-plan.md Section 9.16.
- Files modified and statically reviewed only. VERIFICATION HAS NOT BEEN RUN.
  No build/test/database/container/install/Git command was run. Main 0006 apply
  still pending; perform a fresh backup and preserve the old runtime image before
  rollout. Web dist is a live bind mount, so build web after backend startup passes.
- M2-B is not accepted; no M3+, posting, grant, migration or dependency change.

### Phase 1 M2-B Owner-Run Verification (updated 2026-10-09)

These owner-reported results supersede pending statements in the implementation
entry above. The full evidence and limitations are in plan Section 9.19.

- Backend repaired test-image build exit 0; unit/db_gated/static_check all Passed,
  db_gated not skipped, 0.01/1.05/0.00 s, total 1.06 s, exit 0. Web rerun exit 0
  (successful-rerun counts not supplied), separate type check exit 0, production
  build with service worker exit 0. Earlier failures and repairs remain recorded.
- Main backup exported before migration:
  backups/usapp_before_m2b_0006_20261007_175748.dump, 104193 bytes, exit 0.
  This backup was not restore-tested. Main DryRun and apply with -InitRoles exit 0;
  the frozen checksum is unchanged. Basic main role/privilege checks passed.
- Old runtime preserved as usapp-api:before-m2b-20261007_180639, checked against
  sha256:f576443a538d44fdccff1c462f9e07d89360aba38b5c868bb8300b476e1ddf37.
  Old-runtime compatibility was owner-reported successful. Rollback not exercised.
- New runtime build/update exit 0; Healthy. Main startup policy log:
  role=usapp_api ok=true checks=187 mismatches=0 missing=0 unexpected=0.
- Live own-wallet card 0/0, membership 2/2; owner-reported offline refresh clears
  numbers and shows an error, online retry succeeds; account switch/refresh succeeds.
- Authenticated wallet GET 200 with no-store and 0/0; no-cookie GET and pre-auth
  GET both 401 UNAUTHENTICATED. Authenticated POST/PATCH/DELETE all 405.
- Final main users/spaces/members 2/1/2; wallet tables 0/0/0; wallet.% audit actions
  0; both final queries exit 0. The audit query covers that prefix only.
- Restart incident: Docker Desktop was not running, so the cached login page
  could open but session fetch failed. After starting Docker, api/postgres were
  Healthy and Caddy running; authenticated wallet access subsequently succeeded.
  No code/cache change. Generic CSRF text also covers bootstrap network failure;
  clearer wording is future work. Older initial blank-screen cause stays unknown.
- Limits: targeted checks; live balances both zero, nonzero isolation SQL tested
  only in rollback transactions. No separately recorded full-page persistence,
  live no-space 404, live startup refusal/timeout, or fresh-backup restore. Existing-
  account serialization/post-commit visibility/posting remain M3 requirements.
  HEAD/OPTIONS not separately checked. No formal M2 decision or later Git state
  is inferred. M3-M10 remain unapproved; owner Git review/commit/push is next.

This evidence update modifies only README.md, CLAUDE.md, docs/phase1-plan.md and
this handoff. No code, contract, migration or deployment change; no build/test/
database/container/Git command was executed by Codex or Claude Code.

### Phase 1 M2 Git Publication (Owner-Reported, 2026-10-09; Dated Observation)

- The owner committed and pushed M2-A and M2-B as `594a77bcadb0d2742c0704f93aea20cb09a1e726`, message `feat: add wallet ledger foundation and own balance display`.
- Local `HEAD` and remote `main` matched; tracked files had no outstanding changes.
- `.claude/`, `.workbuddy/`, `Claude outputs/` and the original V1 design document remained untracked.
- A limited credential-pattern check over the staged files returned no matches (exit code 1). It is not a full-history secret scan.
- Limits: owner-reported; Claude Code and Codex ran no Git commands. This describes the state on 2026-10-09 only; later Git state is not assumed. A commit or push is not an acceptance decision; M2 passed the recorded owner-run verification and no separate formal M2 acceptance decision has been recorded.

### Phase 1 M3-A Approval and Implementation (2026-10-09; Verification Pending)

- The owner approved M3-A on 2026-10-09 (task database foundation, reusable task privilege checking, regression tests, status documentation) and confirmed the product decisions recorded in `docs/phase1-m3-design.md` Section 2.3. M3-B and M3-C are not approved; no test-database reset or permanent cap fixture is authorized.
- Claude Code added `database/migrations/0007_tasks_one_off.sql`, `init-roles.sql` section 8, `database/tests/task_ledger_test.sql`, `services/api/src/db/TaskPrivilegePolicy.h/.cc` (not wired into startup), `services/api/tests/TaskLedgerDbTests.h` (db_gated block 13; migration gate now requires 0007), unit and static cases (including a check that 0006 still has checksum `2633e17e...7d543d`), and `database/README.md` notes. Details, refinements (actual UT001-UT007 mapping), coverage, limits and owner steps: design Section 11.
- Not changed: 0000-0006, init-roles section 7, wallet policy, API code, contract (still 0.3.0), frontend, `Db.h`/`Db.cc`, deployment, dependencies, `.claude/settings.json`.
- Limits: files written only; Claude Code ran no build, test, database, container, package or Git command and executed no SQL. When written, verification had not been run. All successful fixtures are rollback-only; separate-transaction attacks on genuinely committed settlements remain an M3-C requirement.

### Phase 1 M3-A Owner-Run Results on usapp_test (2026-10-09; Partial)

- DryRun: 0000-0006 checksums matched; only 0007 pending. 0007 applied to `usapp_test` with `-InitRoles`, exit 0; applied checksum `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`. 0007 is now applied and must never be edited.
- Basic task/wallet permission query matched, exit 0. Repeated `-InitRoles`, exit 0.
- Test image build exit 0. CTest: unit 0.02 s, db_gated 1.35 s (Passed, not skipped), static_check 0.01 s; total 1.38 s; all passed; exit 0.
- `task_ledger_test.sql`: application and maintenance modes each passed 27 groups, ended with ROLLBACK, exit 0.
- Wallet script repair (source review, no failed run observed): maintenance-mode E9 in `wallet_ledger_test.sql` now includes `task_completions` in the combined TRUNCATE when that table exists (no CASCADE; ledger table first; still expects UW001). Claude Code also predicted, from source review only, that maintenance-mode E8 would hit `55006` (pending trigger events from 0007's deferred ledger trigger); after authorization, maintenance mode now runs `SET CONSTRAINTS ALL IMMEDIATE` before E8, outside the expected-failure block (design Section 11.2). Both repairs are unverified until the owner runs the wallet script in both modes.
- Pending when written: wallet script reruns in both modes, final `usapp_test` fixture checks. Both were later completed (next entry).

### Phase 1 M3-A Owner-Run Verification Completed (2026-10-09)

- Owner-run under Codex guidance; Claude Code ran none of these. Full record and limits: design Section 11.3.
- DryRun: 0000-0006 checksums matched, only 0007 pending, exit 0. 0007 applied with `-InitRoles`, exit 0; frozen checksum `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`.
- Basic task/wallet permissions matched, exit 0; repeated permission initialization exit 0; test image build exit 0.
- Full CTest: unit Passed 0.02 s; db_gated Passed 1.35 s, not skipped; static_check Passed 0.01 s; total 1.38 s; exit 0.
- `task_ledger_test.sql`: both modes passed 27 groups each, ended with ROLLBACK, exit 0. Repaired `wallet_ledger_test.sql`: both modes passed 28 groups each, ended with ROLLBACK, exit 0. The E8/E9 compatibility repairs are now owner-run verified; their original issues were source-review predictions, not observed failed runs.
- Final `usapp_test` query: tasks, completions, accounts, operations and entries all 0; users matching the `wallet_` or `task_psql_` prefixes 0; exit 0. This is not an all-fixture claim.
- Limits: CTest preceded the E8/E9 script repairs and was not rerun afterwards; per-assertion counts and individual privilege-summary lines were not supplied; 0007 was not applied to the main database; task API/frontend, real settlement service, concurrency/replay tests and attacks on committed settlements are unverified (later stages); no test-database reset or permanent cap fixture was executed; no separate formal acceptance decision is recorded.
- Next (when written): owner Git review/commit/push. M3-B/M3-C implementation remained unapproved at that time.

### Phase 1 M3-A Git Publication (2026-10-09)

- Owner-reported: M3-A was committed and pushed as `6a9da4bd31e4f2b7863501c39e70931fadd5a6e2` ("feat: add one-off task database foundation").
- Local `HEAD` matched remote `main`; no outstanding tracked-file changes.
- The existing local tooling/output folders and the original V1 design document remained untracked.
- A limited staged credential-pattern check had no matches (exit code 1). It is not a full-history secret scan.
- Limits: owner-reported; Claude Code and Codex ran no Git commands. This describes the state on 2026-10-09 only; later Git state is not assumed. A commit or push is not an acceptance decision; no separate formal M3-A acceptance decision is recorded.

### Phase 1 M3-B Approval and Implementation (2026-10-09; Verification Not Run)

- The owner approved M3-B on 2026-10-09: task publication and read APIs, task list/publication UI, task privilege enforcement at API startup, and per-run isolated committed task fixtures (random identifiers, reserved test domain `@m3-fixture.test`, assertions scoped to those identifiers; successful committed fixtures may contain tasks and their non-wallet parents). M3-C and M4-M10 are not approved. No new migration is authorized.
- Claude Code added `TaskService` and `TaskController` (`POST /api/v1/tasks`, `GET /api/v1/tasks`, `GET /api/v1/tasks/{taskId}`), claim-first idempotent publication with same-transaction `task.created` audit and 503 `RESULT_UNKNOWN` for unconfirmed commits, NULL-safe keyset pagination with strict cursors, OpenAPI 0.4.0 (four new error codes, no completion operation), `TaskStartupCheck` in `main.cc` after the wallet check, the web task API/store/view with in-memory idempotency keys and session-generation guards, unit/static/db_gated (block 14) and web tests. Details, refinements, limits, acceptance steps and the main-database rollout order: design Section 12.
- Not changed: migrations 0000-0007 (0006 `2633e17e...7d543d`, 0007 `a9c680a0...556161`), `init-roles.sql`, grants, `Db.h`/`Db.cc`, wallet policy/service/controller, deployment, dependencies, `.claude/settings.json`.
- Limits: files written only; Claude Code ran no build, test, package, container, database or Git command. Verification has not been run. Committed M3-B test fixtures are permanent in `usapp_test` (about 18 users, 9 couples, 20 tasks and 5 audit rows per db_gated run, all under `@m3-fixture.test`); completed-history coverage is rollback-only; the unknown-commit path is code-reviewed only; there are no automated HTTP-level tests for the new routes. 0007 is not on the main database, and the new runtime refuses to start without it.
- Review repairs (2026-10-09, before owner verification; not run): self-consistent held-publisher and replay fixtures with self-checks (simulated original timing distinguished from the observed lock wait); `TasksView` retry only for unchanged normalized content, edited content is a new publication with a new key, success no longer erases a newer draft; component-local draft/errors/notices cleared on account change or logout with generation and unmount guards; task service/controller error details and logs use fixed `<operation> stage=<stage> reason=<reason>` labels and never forward ORM or transaction-callback text (Drogon's own ORM logging and `Db.h`/`Db.cc` unchanged). Design Section 12.3 items 9, 15, 16, 18 and Sections 12.4-12.5.

### Phase 1 M3-B Owner-Run Verification and Main Rollout (2026-10-10)

- Owner-run under Codex guidance; Claude Code ran none of these. Full record and limits: design Section 12.9.
- Backend: test image build exit 0. Initial CTest: unit Passed 0.04 s; db_gated Passed 3.70 s, not skipped; static_check Failed 0.01 s (browser-storage identifiers in the `apps/web/src/stores/tasks.ts` header comment); total 3.76 s; exit 8. Only that comment was rewritten, checks preserved. Rebuilt image exit 0; targeted static_check Passed 0.01 s, total 0.02 s, exit 0. All three targets passed across these runs; no later single full-suite rerun is claimed.
- Web: initial 10/11 files, 87/88 tests passed (the anonymous-reset assertion searched permanent help text); assertions scoped to the form alert with coverage preserved; rerun 11 files / 88 tests passed, 11.24 s, exit 0. Type check and production build exit 0; service worker precache 18 entries, 160.11 KiB.
- Main rollout: fresh backup `backups/usapp_before_m3b_0007_20261010_001433.dump` (164181 bytes; not inspected by Claude Code); single-transaction exit-on-error restore into `usapp_m3b_restore_20261010_001509` succeeded; basic counts matched (7 migrations, 2 users, 1 couple, 2 members, 11 audits; wallet tables empty; tasks absent; not a row-by-row comparison). DryRun: only 0007 pending, checksums matched. 0007 applied with `-InitRoles`, exit 0, checksum `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`; repeated `-InitRoles` succeeded; privilege checks matched before and after. Old runtime nickname/space/wallet workflow still worked. Rollback tag `usapp-api:before-m3b-20261010_002116` (image `sha256:2338ae4ef4af919f6ad4e47f4d7eb21bac02b2726b6cc09e22caee2108355a4f`); no rollback exercised. New runtime exit 0, healthy; wallet policy ok=true (187 checks), task policy ok=true (146 checks), zero mismatches/missing/unexpected.
- Live: flower-only, sunshine-only and both-reward publications; no-deadline, date-only and date+time display; partner view correct; 201 publish, same-key 200 replay of the same task, changed payload 409 `IDEMPOTENCY_KEY_REUSED`; zero rewards, past deadline, extra field, 41-code-point title 400 with list unchanged; missing/incorrect CSRF 403 `CSRF_TOKEN_INVALID`; wrong Origin 403 `ORIGIN_NOT_ALLOWED` (no authenticated cookie on that request); no-cookie and pre-auth list/detail/publish 401; open pagination limit=2 two pages, four tasks, no duplicates; detail/history 200; invalid limit/cursor 400; unknown task 404; `no-store` on publish and checked read/error responses; owner-reported offline block without replay and one task from a rapid double click. Final main query: 5 tasks, 5 `task.created` audits (ids 12-16), no completions or wallet rows; audit detail only `reward_flowers`, `reward_sunshine`, `has_deadline`.
- Limits: targeted, not exhaustive. Not exercised live: forced `RESULT_UNKNOWN`/mid-COMMIT connection loss, logout during an in-flight publish, cross-space non-member read (automated coverage is separate evidence). When written, no separate formal M3-B acceptance decision, commit or push was recorded (both followed later on 2026-10-10; next entry). M3-C implementation remains unapproved.

### Phase 1 M3-B Acceptance, Git Publication and M3-C Design Authorization (2026-10-10)

- Owner decision: M3-B is accepted within the scope and limitations of the recorded owner-run checks (previous entry; design Section 12.9). This is a local-development milestone, not production-readiness or exhaustive-security approval; all recorded limitations remain open.
- Owner-reported Git observation: commit `380696b689575744fc4c6de00391d9603c8c8b70` ("feat: add one-off task publication and viewing") pushed to `origin/main`; local `HEAD` equalled remote `main`; no outstanding tracked changes; `.claude/`, `.workbuddy/`, `Claude outputs/` and the original V1 document remained untracked; a limited staged credential-pattern check returned no matches (exit 1; not a full-history scan). Dated observation; later Git state is not assumed. Documentation changes made after it are uncommitted until the owner reports otherwise.
- Owner authorization: M3-C design/planning only. Claude Code wrote `docs/phase1-m3c-design.md` (documentation only; nothing implemented, built, tested or executed). M3-C implementation is not authorized; its open decisions are listed in that document's Section 13.

### Phase 1 M3-C Planning Decisions (2026-10-14)

- Owner decisions: lighter testing and acceptance workflow approved; D30 Option A and the closed-enum D32 fault hook confirmed as approved directions; D33 (persistent near-cap fixture) deferred, so real-service near-cap rejection is not in the next batches; D36 committed settlement fixtures retained for literal `usapp_test` (planning only: no fixture creation or database commands authorized).
- Claude Code revised `docs/phase1-m3c-design.md` to revision 3 (documentation only): retained real-service cases R1-R9, lightweight parameterized boundary tests, five frontend behaviour tests, M3-C1/M3-C2 batches with exact files, fixture growth estimates, execution/acceptance workflow and approval status (Sections 6-13). Detailed plan and future evidence live only in that document.
- Still pending when written: one implementation-batch approval for D36-D41 after Codex review.

### Phase 1 M3-C1 Approval and Implementation (2026-10-14; Verification Not Run)

- The owner approved the M3-C1 backend batch (D36, D37, D38, D39, D41; D30 Option A and D32 already approved). M3-C2 (web, D40) is not approved; D33 deferred.
- Claude Code implemented `TaskCompletionService`, the completion route, OpenAPI 0.5.0, four error codes, the exposed `TaskService` mapper, unit/static tests and db_gated block 15 (R1-R9). Details and limits: `docs/phase1-m3c-design.md` Section 16.
- Limits: files written only; no build, test, database, container or Git command; no fixture created. No real-service near-cap test (D33 deferred).
- Later on 2026-10-14: owner-run full CTest passed (design Section 16.5); not accepted. The owner then approved M3-C2 (web, D40); Claude Code implemented it, verification not run (design Section 17).

### Phase 1 M3-C Rollout and Direct M4 Assignment (2026-10-14)

- Owner-run M3-C automated checks, backup/restore, image/dist preservation,
  deployment and representative live success reports: design Section 18 only.
  No later Git publication or separate formal acceptance is recorded.
- The owner requested the next feature, then directly assigned M4 counting tasks
  to Codex. Source, migration 0008, narrow progress grants and focused tests are
  written; no commands affecting Git/build/tests/packages/containers/DB were run.
  Detailed implementation and pending owner sequence: plan Section 10.
- Preserve all unrelated M3 work and applied migrations. M5-M10 are unapproved;
  D33 deferred; no reset, dependency/deployment change or timeout increase.

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
- `docs/phase1-plan.md`: Phase 1 scope and milestone evidence; M4 deployment and limits are in Section 10.8. M5-A daily evidence: `docs/phase1-m5-daily.md`; current M5-B weekly implementation: `docs/phase1-m5-weekly.md`. Custom periods and M6-M10 remain unapproved.
- `docs/phase1-m3-design.md`: historical M3-A/M3-B design and evidence; approved product decisions in 2.3, M3-A checks in 11.3, M3-B acceptance/publication in 12.10. Completion evidence is in the separate M3-C design.
- `docs/phase1-m3c-design.md`: approved M3-C implementation and evidence, Sections 16-18. The owner-approved lighter workflow applies; D33 remains deferred. Its earlier design-only wording is historical.
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
handoff is a dated observation (latest: M2 commit 594a77b pushed on
2026-10-09); later changes stay uncommitted until I report a new commit.
Phase 0 was accepted
on 2026-10-06 as a local development baseline, not as production-ready;
its recorded limitations remain open. Phase 1 M1 (nickname editing) is
implemented and passed my acceptance checks. M2-A passed my checks on
usapp_test. I assigned M2-B directly to Codex; it is implemented and passed
owner-run verification (plan Section 9.19) and is committed/pushed. A
separate formal M2 acceptance decision has not been recorded. M3-A (task
database foundation) is implemented, passed my owner-run verification on
usapp_test and was committed/pushed as 6a9da4b. M3-B (task publication and read
APIs, task UI, task startup check) is implemented, passed my recorded owner-run
checks on 2026-10-10, is deployed, was accepted by me as a local-development
milestone and committed/pushed as 380696b (docs/phase1-m3-design.md Sections
12.9-12.10). M3-C is implemented with owner-run rollout/live evidence in its
design Section 18. M4 is deployed within the recorded scope; remaining live
checks were waived (plan Section 10.8). M5-A daily counting is directly assigned
to Codex, implemented and passed owner-run backend/web automated checks
(docs/phase1-m5-daily.md, Verification) and deployed on 2026-10-22; remaining
live checks were waived. M5-B weekly counting is assigned to Claude Code
(docs/phase1-m5-weekly.md). Custom periods and M6-M10 remain unapproved. Do not redo completed milestones or run any owner-only commands.
Freeze migrations 0000-0017 (all applied on test/main); M5-B automated
checks and main API/web rollout completed 2026-10-25; live weekly workflows remain unverified. Do not read the real .env, and
preserve unrelated changes.

Briefly confirm the root cause or design before editing. Do not expand
beyond the assigned milestone or redesign the architecture. Deliver actual file differences, regression
coverage, owner-run acceptance steps, and remaining unverified risks.
Explicitly state when verification has not been run; do not claim tests passed.
Write all handoff and implementation reports for Claude Code in English.
After each task, also provide a copy-paste-ready report for Codex.
```
