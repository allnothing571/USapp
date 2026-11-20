# Admin anniversary inspection (2026-11-14)

Read-only local-admin inspection of anniversaries and countdowns in a selected couple. This batch adds current content,
one pending full replacement and the most recently decided proposal summary. It does not add a complete revision or
proposal-history browser, decisions, edits, deletion, pins, notifications, new schema/grants/dependencies or deployment.
Contract 0.55.0 is additive; all 49 migrations remain frozen and the next number remains 0049.

## API and scope

- `GET /api/admin/v1/couples/{coupleId}/anniversaries`: current title/date/kind/version, creator and creation/update times.
- `GET /api/admin/v1/couples/{coupleId}/anniversaries/{anniversaryId}`: current note, pending proposal terms and latest
  decision metadata in addition to those current fields.

Both require AdminFilter and its attached independent admin identity. Couple and parent identities constrain every child
join. Malformed identifiers/limit/cursor return VALIDATION_ERROR 400; unknown couple returns SPACE_NOT_FOUND 404; unknown
or foreign anniversary returns generic NOT_FOUND 404. Uppercase path ids are canonicalized; cursor ids are lowercase.
JSON uses the existing no-store helper. The service uses the normal application role and performs no domain/audit writes;
existing admin session bookkeeping is unchanged.

The list sorts by immutable `(created_at,id)` descending, not mutable event date or title. Limit 1–100, default 50; a
bounded materialized root page reads limit+1 before projecting revisions. Cursor `an1.<positive microseconds>.<id>.<couple>`
binds the resource kind and couple and accepts only canonical positive int64 values. Existing indexes are sufficient.

Each response is one SQL snapshot; list and detail/pages can observe different snapshots. Current terms use the highest
revision version. Pending uses the same undecided predicate as AnniversaryService. Last decision uses the same ordering
as that service: decided_at descending, proposal id descending. It is not an event feed or proof of chronological ordering
across backward wall-clock steps. approved_version is returned explicitly so an old approval is distinguishable from a
later current version. Rejected/withdrawn decisions have no approved version.

Dates are recorded civil dates; no cumulative/remaining day calculation or current Beijing-date claim is made here. Home
pin choices, reminder rules, notification preferences/facts, raw request/receipt JSON, keys, fingerprints, wallet references
and session credentials are excluded. All JSON fields are explicitly projected.

Anniversaries currently have no deletion workflow. Existing recovery closure/RLS protects media, memory, album and linked
plan domains; it does not add an anniversary hold. This batch preserves that boundary and does not invent deletion/hold
semantics for anniversaries. No image content or grant is returned.

## Admin client

The protected route `/couples/:coupleId/anniversaries/:anniversaryId?` is linked from the couple table. The existing table,
panel and cursor pager styles are reused. Current content and pending terms are separate sections; latest decision copy
explicitly says this is not complete history, and uses the same detail snapshot's current version for approval labels.
No write controls are present, and text is escaped by ordinary Vue interpolation.

List and detail have independent request sequences. Selecting another anniversary cannot strand a pending list; refreshing
the list cannot strand a pending detail. Couple, session, logout and unmount changes invalidate old reads. A current
401/403 clears all displayed data including the space metadata, settles loaders and invalidates other in-flight reads.
A current detail 404 clears selected content and invalidates a late list. A missing-couple list clears selected content
and invalidates a late detail. No automatic retry or persistence is added. Explicit refresh handles ordinary errors.

Validators reject extra fields, wrong parent identities, duplicate ids, foreign/range-invalid cursors, invalid calendar
dates, invalid kind, mismatched pending base versions and inconsistent decision/approval versions.

## Verification record

Final disposable run **usapp-e1-a770a904fa** completed successfully (harness exit 0):

| Check | Result |
|---|---|
| Schema / roles | 49 migration checksums; eraser violations zero; privilege deviations zero after apply, repeat and CTest; all migration bytes unchanged |
| Build / new service suite | Passed; 93 assertions in two cases |
| Real local HTTP binary | 1228 assertions passed; both listeners, independent admin identity/surface, unknown/foreign ids, limit/cursor validation, no-store, unchanged public visibility, no domain writes |
| Full CTest | **30/30 in 97.98 s**; admin_anniversary_db 0.19 s, db_gated 13.57 s |
| Postconditions | Early/reversed erasures and ledger chain breaks zero |
| Existing export extra modes | Connection-loss 8, large 6, closure 23 passed; no claim that the intermittent transport/reconnect stall is fixed |
| Existing recovery-closed modes | Album 7 and memory 8 passed; their hold boundaries unchanged |
| Admin frontend | Focused 16; full **187 tests / 13 files**; both typechecks passed |
| Isolated production build | Passed; original admin 20 and web 45 dist files unchanged; output in %TEMP%/usapp-admin-anniversary-web-1cc751708c |
| Contract | Strict YAML/ref check: 0.55.0, 124 paths, 240 distinct refs; duplicate-key negative control refused. Not OpenAPI semantic validation |

Selected noncredential evidence is in `Claude outputs/admin-anniversary-20261114/`. The generated env/credential file and
server logs were not copied. The run's containers are stopped and retained; its final database was recovery-closed by the
existing closure tests. No backend test assertion failed. One host authoring command failed to parse before writing any
frontend file; the files were subsequently created with the patch tool, then all frontend checks passed.

The targeted suite checks current versus requested terms; approved, rejected and withdrawn latest decisions; superseded
approval versus current version; empty/single-member and unknown spaces; foreign parents; exact field allowlists and
no-write footprints; tied-microsecond pagination despite a mutable title/date change; and cursor type/couple/range checks.

Not verified: real browser, real admin login, devices, production data/scale, cloud or providers. Existing export reconnect
availability limits, clock-guard intermittents, journal/restore requirements and release gates remain open. Not deployed
or committed. No operational correction or full-history feature is claimed by this inspection batch.
