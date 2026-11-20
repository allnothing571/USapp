# Admin memory inspection (2026-11-14)

Read-only local-admin inspection of a selected couple's memories, current text/photo references, and pending/decided edit
proposals. No decision, content edit, media grant, deletion-policy, notification, wallet, schema, grant or dependency change.
Contract 0.54.0 is additive. The 49 migrations remain frozen; the next migration remains 0049.

## Routes and authorization

All four GETs require AdminFilter and its attached independent admin identity:

- `/api/admin/v1/couples/{coupleId}/memories`: latest summaries; immutable `(created_at,id)` descending keyset.
- `/api/admin/v1/couples/{coupleId}/memories/{memoryId}`: latest title, date, description, version and ordered photos.
- `.../{memoryId}/proposals`: summaries, pending and all three decision outcomes; current_version in the same statement.
- `.../{memoryId}/proposals/{proposalId}`: original base revision, proposed snapshot, decision and current_version.

Couple, memory and proposal are joined together. Unknown couple: SPACE_NOT_FOUND 404. Missing, foreign, deleted, purged or
recovery-held child: generic NOT_FOUND 404. Malformed path id, limit or cursor: VALIDATION_ERROR 400. Uppercase path ids
are canonicalized; cursors require lowercase canonical ids. JSON responses are no-store. Admin session housekeeping is
unchanged; these endpoints perform no domain or audit writes.

Each parent query applies `NOT usapp_memory_deleted_now`. Existing application-role RLS remains enabled, including
recovery closure. A recovery-held list is empty with availability=unavailable; detail/history/comparison return 404.
Deleted memories therefore cannot disclose historical text through their proposals. Physical erasure does not reopen them.

## Photo projection and historical meaning

Current, original and proposed snapshots use one explicit photo projection. A visible photo returns only position,
available=true, media_id, uploader_id and nullable dimensions. A deleted/purged photo returns **only** position and
available=false. It has no id, uploader or dimensions. This is deliberately stricter than the ordinary memory API's
retained deleted-photo reference. The current photo_count counts available entries, not withheld placeholders.

No object/staging keys, signed URLs, image bytes, ETags, grants, raw receipt/request JSON, fingerprints, idempotency keys,
text ids or wallet-operation references are returned. This page provides metadata inspection, not binary access.

An approval is a historical fact: its approved_version may have been superseded. History labels use that history response's
current_version; comparison labels use that comparison response's current_version. Neither borrows a racing detail version.
Original means the proposal's base revision, not today's body; proposed means its recorded request. No new decision path.

Every response is one SQL snapshot, not a transaction across pages or endpoints. A deletion/hold committed after a read's
snapshot begins may still leave that already-started response with its earlier view. Previously received text cannot be
revoked. The page states that independent responses can observe different snapshots.

## Pagination and client state

Both paginated routes default to 50, accept 1–100 and read at most limit+1 parent rows before projection. List cursor:
`am1.<positive microseconds>.<memory id>.<couple id>`. Proposal cursor:
`mp1.<positive microseconds>.<proposal id>.<memory id>.<couple id>`. Numeric values are canonical positive int64 and the
resource kind and all parents are bound. Mutable memory_date is never the parent pagination key. Existing indexes suffice.

The protected admin route is `/couples/:coupleId/memories/:memoryId?`, linked from the couple table. Existing table/pager
styles are reused. List, detail, history and selected comparison have independent sequence checks. Parent/couple/session
changes invalidate descendants; selecting another proposal invalidates only comparison. Refreshing history cannot strand
a pending detail. Current 401/403 clears all displayed data and invalidates all in-flight reads. Current child 404 clears
selected content and invalidates a delayed list. An unavailable list clears selected content. Logout/unmount resets state.
No persistence, automatic auth retry or write controls are added. Exact response fields, parent identities, photo union,
decision consistency, version relations, date values and cursor context are validated before display.

## Verification

Final disposable run **usapp-e1-3d71f582b6** completed with harness exit 0:

| Check | Result |
|---|---|
| Schema / roles | 49 checksums; eraser violations zero; privilege deviations zero after apply, repeat and CTest; migration bytes unchanged |
| Build and targeted memory suite | Passed; 193 assertions in two cases |
| Real local HTTP binary | 1156 assertions passed; all four new GETs exercised on both listeners with independent admin auth, surface checks, scope/validation/cursor checks, no-store and unchanged public visibility |
| Full CTest | **29/29 in 97.21 s**; admin_memory_db 0.31 s; db_gated 13.47 s |
| Postconditions | Early/reversed erasures and ledger chain breaks zero |
| Existing export extra modes | Connection-loss 8, large 6, closure 23 assertions passed; this is not a transport-stall root-cause fix |
| Recovery-closed modes | Memory 8 and album 7 assertions passed after real recovery closure; application-role RLS hides parent and historical children |
| Frontend | New focused 22/22; full admin **171 tests / 12 files**; both typechecks; isolated Vite build passed |
| Existing dist | Original admin 20 and web 45 files unchanged; output in `%TEMP%/usapp-admin-memory-web-07f9d773b3` |
| Contract | Strict YAML/ref check: 0.54.0, 122 paths, 234 distinct refs; duplicate-key negative control refused. Not OpenAPI semantic validation |

The service suite covers four statuses, a superseded approval, current versus original/proposed content, cross-couple and
cross-memory isolation, exact field allowlists, no-write footprints, photo deletion/undo withholding, parent deletion/undo
and physical text erasure, tied-microsecond parent/history walks and cursor type/parent/range binding. The post-closure
suite is harness-only and executes after ordinary CTest so it cannot poison that dataset early.

After the full run, the only runtime edit corrected a copied error-log label from `admin album` to `admin memory`.
The exact final controller was copied into the ownership-checked compiler container, rebuilt successfully and static checks
passed (1539 assertions). No database was restarted or read in that follow-up. The first invocation used the wrong static
binary name and failed to start; corrected invocation passed. The final frontend test edit only replaced `as never` with
its intended Proposal type; focused 22 and both typechecks were rerun. Product behavior was unchanged by these final edits.

Selected noncredential evidence: `Claude outputs/admin-memory-20261114/`. Both runs' containers are stopped and retained;
the successful database was recovery-closed deliberately by the final tests. Generated credentials were not copied.

First isolated attempt usapp-e1-6c45b343f0 stopped before compilation because the added CMake controller path was accidentally
placed inside the previous quoted path. Corrected without changing product behavior. A contract schema insertion initially
fell beneath top-level security; the strict ref checker rejected it and it was moved inside components.schemas before the
next verification snapshot. No assertion was weakened. Containers stopped and retained.

Not accepted: real browser, real admin login, production data/scale, devices, cloud or providers. No deployment or commit.
Existing E1 transport/reconnect availability limits, other clock-guard intermittents, journal and restore release gates
remain unchanged by this batch.
