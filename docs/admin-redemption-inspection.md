# Admin redemption inspection (read-only)

## Scope and boundary

Contract 0.49.0 adds GET /api/admin/v1/couples/{coupleId}/redemptions and /redemptions/{redemptionId}.
AdminFilter checks the peer CIDR, admin surface header and independent admin session; the controller also checks the attached admin identity. User cookies never authorize. Malformed ids/filter/limit/cursor: 400. Unknown space: SPACE_NOT_FOUND; foreign and unknown redemption: REDEMPTION_NOT_FOUND. Existing empty space: 200. Archived spaces remain readable.

No creation, approval, fulfillment, cancellation, refund, asset correction, export, notification or other write exists here. Public APIs, migrations 0000-0047, grants, dependencies and CTest timeouts are unchanged. Next migration 0048 remains unreserved.

## Read model and format

The service explicitly allowlists columns from reward_redemption_state, the same authoritative view as RedemptionService. Product identity/version, creator, redeemer, original title and price, redemption mode, status, fulfillment/cancellation facts and pending cancellation reference are shown. Detail adds the original description and newest 100 events ordered by (created_at DESC,id DESC), plus total count. Fulfillment is a separate row represented by fulfilled_at/fulfilled_by, not a synthetic event. Events expose ids, actor, action, exact proposal reference and time only. No keys, fingerprints, operation identifiers or raw requests.

Price is a non-negative decimal string, never converted to Number by the admin client. Direct-mode debit/confirmation/refund semantics are unchanged and are not reimplemented here. Current catalog edits/archive never rewrite the displayed purchase snapshot.

One statement per answer includes space existence and the data in the same snapshot. List: newest (created_at,id), limit 1-100, default 50, one lookahead. Cursor r1.<status-or-all>.<integer-microseconds>.<redemptionId>.<coupleId>, canonical UUIDs and positive signed-64-bit microseconds, bound to space and filter. No cross-page snapshot. Status is mutable: records can enter/leave a filtered result between pages; refresh restarts from the newest row. There is no time window.

## Admin page

CouplesView links to /couples/:coupleId/redemptions/:redemptionId?. The page is read-only and explains snapshot/capped-history limits. All state is memory-only. Reuses the task inspection store pattern: separate list (session/space/filter) and detail (session/space/redemption) identities, separate sequences; filter changes do not strand a detail. Current 401/403 invalidates all reads and clears both displayed scopes. Session change/unmount resets everything. No automatic retry.

## Verification

Completed with automated verification (Codex, 2026-11-12). New frontend tests cover response scope, exact decimals, caps, paging, independent loading, late answers, auth loss, reset, errors and the read-only view. Backend tests use real redemption workflows in disposable infrastructure, compare public status, check immutable snapshots and the unchanged financial footprint, and drive both listeners. A separate admin_redemption_db CTest target keeps cases out of default db_gated under the existing 30-second limit.

- Final disposable run `usapp-e1-4dff189ce9`: build passed; targeted ADMIN_REDEMPTION **205 assertions / 2 cases**, ADMIN_TASK 209, ADMIN_WALLET 112; no failures.
- Real API binary **767 assertions**, including **80 new assertions** on both listeners: no session, user-cookie/forged-surface, missing/wrong admin surface, successful reads, malformed/unknown/foreign ids, limit/status/cursor validation, no-store and excluded fields, ordinary public read still couple-scoped, unchanged write footprint.
- Full CTest **23/23 passed in 96.86 s**; db_gated 13.48 s, admin_redemption_db 0.15 s. No timeout raised.
- 48 migration checksums matched; all migration files unchanged. Eraser violations and privilege deviations after apply/repeat initialization/CTest: 0; early/reversed erasures and ledger-chain breaks: 0. Test-credential matches in the run's logs and CTest crash markers: 0.
- E1 additional modes: connection loss 8 assertions (session_replaced, release 1,009 ms), large 6, closure 23; all passed. This does not resolve the earlier reconnect stall. The disposable database was intentionally CLOSED by the closure test.
- Admin: focused **10 tests**; full **104 tests / 7 files**, both typechecks, isolated Vite build passed. Build output `usapp-ar-admin-d3e0847699` is under the sandbox temporary directory; existing dist directories were not build targets.
- Strict YAML/ref check passed: contract 0.49.0, 111 paths, 205 distinct references resolved; duplicate-key negative control refused. OpenAPI semantics were not validated.

Earlier run `usapp-e1-bbb08895b9` stopped during compilation on a missing closing quote in the new SQL macro; corrected before the final fresh run. No backend assertions ran in that failed attempt. Two typecheck attempts caught an unknown-type comparison and unused imports in the new tests; corrected. No assertions were weakened or timeouts changed.

Evidence: `Claude outputs/ar-verify-4dff189ce9/`; original host work folder `%TEMP%/usapp-e1-4dff189ce9`. Both final-run containers were confirmed exited; stopped containers and networks from both runs were retained. No other containers were changed.

Not verified: browser rendering or real admin login, production-scale query performance, identical-timestamp event fixtures or >100-event truncation against a live database (the SQL limits the history and the client rejects oversized pages), archived-space fixtures, SQL ledgers, real providers/storage, shared/production database, deployment. Status-filter membership can change between pages. Historical clock-guard intermittents and the DbClient teardown crash remain unresolved. NOT DEPLOYED; nothing committed.
