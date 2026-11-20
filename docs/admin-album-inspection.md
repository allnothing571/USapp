# Admin album inspection (2026-11-14)

Implemented scope: read-only album and photo-item metadata for a selected couple, through the independent local admin
surface. Photo items include uploader, addition and optional removal facts; this batch does not issue image-content grants
or fetch binaries. No album/media mutation, notification, deletion-policy change, migration, grant or dependency change.
Contract 0.53.0 is additive; the existing 49 migrations remain frozen and the next migration is 0049.

## API and visibility

All three GET routes use AdminFilter and also require its attached admin identity:

- `/api/admin/v1/couples/{coupleId}/albums`: newest `(created_at,id)` keyset page, limit 1–100 (default 50).
- `/api/admin/v1/couples/{coupleId}/albums/{albumId}`: current name/version, creator/time, visible current count, current
  cover reference and visible history-item count.
- `/api/admin/v1/couples/{coupleId}/albums/{albumId}/items?view=current|history`: newest addition sequence first,
  independently paginated. History is one row per added item with its optional removal, not a removal-time event feed.

Malformed identifiers, limit, view or cursor: 400. Missing couple: `SPACE_NOT_FOUND` 404. An absent, foreign or unavailable
album: generic `NOT_FOUND` 404. Uppercase path ids are canonicalized; cursor ids are canonical lowercase. GET responses
use the existing no-store JSON response helper. Admin session housekeeping is unchanged; there are no domain/audit writes.

The service runs as the normal application database role. Parent selection includes the exact couple, and children are
joined through that album. Existing recovery RLS is not bypassed. Under a recovery hold, the list returns an empty page
with `availability=unavailable`; detail/items return 404. The couples row itself remains ordinary admin metadata.

Current items exclude both album removals and `usapp_media_deleted_now` media. History includes removed items only while
their media remains visible, and also excludes deleted or purged media. Undo restores the original item/removal state.
Counts follow those same predicates. The selected cover is used only while active; otherwise the highest active sequence
is the fallback, matching AlbumService. No deleted cover identifier or historical private reference is returned.

Removed-item history is an independent **admin metadata** permission. It does not restore a partner's access to that
photo: the ordinary MediaService uploader/shared visibility rule is unchanged, and this page grants no binary access.

Projection is explicit. No object/staging keys, ETags, signed URLs, grant fields, raw request/receipt JSON, fingerprints,
idempotency keys or album text history are returned. Each response is a single SQL snapshot. A deletion/hold committed
after that snapshot begins can still leave an already-started response with its earlier view; no stronger race guarantee
is claimed.

## Paging and client state

Album cursor: `aa1.<positive microseconds>.<album id>.<couple id>`. Item cursor:
`ai1.<current|history>.<positive sequence>.<album id>.<couple id>`. Decoders enforce canonical numeric ranges and all
context components. No OFFSET, new index or schema is needed. Album roots are bounded before their metadata projection;
item pages use the existing `(album_id,seq)` index. Counts are exact nonnegative decimal strings.

The admin page extends existing table/pager styles. The couple table links to it and fills the previously empty plan-link
column while keeping headers/cells aligned. List, selected detail and selected item view have independent read identities
and sequences. A view change cannot discard a pending detail; a different album, couple, session or unmount invalidates
old answers. Any current 401/403 clears all shown data, including the space snapshot, settles all loaders and invalidates
other in-flight reads. No automatic auth retry is added. A current 404 clears selected data and invalidates a delayed list;
an unavailable list clears all selected content. Store data is memory-only.

The client validates exact field sets, subject identities, decimal ranges, page bounds, descending sequences, removal
consistency and cursor context before accepting a response. Counts, detail and item pages are separate snapshots, stated
on the page. Removed means removed from this album; it is not a media deletion or proof of physical erasure.

## Verification record

Final backend run `usapp-e1-81c4e573b9` completed successfully in one disposable dataset:

| Check | Result |
|---|---|
| Schema and role checks | 49 migration checksums; eraser violations zero; privilege deviations zero after apply, repeat and CTest; all 49 files unchanged |
| Build / targeted album suite | Passed; album suite 181 assertions, two cases; existing targeted suites also passed |
| Real local HTTP binary | 1045 assertions passed, including both-listener auth/surface boundaries on all three new GETs, subject/limit/view/cursor errors, no-store and no domain writes |
| **Full CTest** | **28/28 in 96.95 s**; new admin_album_db 0.44 s, default db_gated 13.43 s |
| Postconditions | Early/reversed erasures and ledger chain breaks zero |
| Export loss / large / closure | 8 / 6 / 23 assertions passed; ordinary error-callback reconnection, not a transport-stall root-cause fix |
| Recovery-closed album mode | Seven assertions passed after real recovery closure: empty unavailable list; parent/detail/items hidden by application-role RLS |
| Full harness | Exit 0; containers stopped and retained |

The targeted album suite covers cross-couple isolation, exact field allowlists and no-write footprints, current/removal
facts, cover fallback, deletion and undo, a real fake-store purge of a removed item, tied-microsecond album paging,
sequence paging and cursor context/range checks. The recovery-closed case is harness-only and runs after closure, so it
cannot poison ordinary CTest's database.

Earlier attempts, preserved:

- `usapp-e1-458abf9d90`: build passed; new fixture wrongly submitted the partner's upload in the creator's batch and asked
  the creator to remove it. Existing authorization correctly refused it, causing 13 cascading assertions. Fixed the fixture
  to use each uploader for their add/removal; no product permission changed.
- `usapp-e1-c0e776a5aa`: build passed, 180/181 new checks passed. The remaining fixture assertion expected `Ok` for cover
  selection, whose existing service result is `Created`; corrected the assertion.
- Frontend initial 19 tests passed; strengthening the six current-401/403 combinations with already-loaded space metadata
  made all six fail, proving a retained-space bug in this batch. Clearing the space snapshot fixed it. Final full admin
  suite: 149 tests / 11 files; focused 19; both typechecks and isolated build passed. Existing admin dist 20 files and web
  dist 45 files unchanged; build logs `usapp-admin-album-web-9fd6030a2e` in the host temp directory.
- Strict YAML/ref check passed: 0.53.0, 118 paths, 223 distinct refs; duplicate-key control refused. This is not semantic
  OpenAPI validation.

Evidence: `Claude outputs/admin-album-20261114`. No browser, real admin login, device, cloud, real store, performance/load or
deployment acceptance is claimed. Long-history query performance is unmeasured. The metadata feature is implemented and
automatically verified; image-content preview remains outside this batch. Nothing committed or deployed.
