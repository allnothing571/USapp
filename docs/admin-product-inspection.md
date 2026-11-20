# Admin reward product inspection (2026-11-14)

Read-only inspection of a selected couple's current reward catalog, including active and archived products. No catalog
write, redemption, cancellation, refund, asset correction, notification, image grant, migration, privilege, dependency or
timeout change. Contract 0.56.0 is additive. The 49 migrations remain frozen; the next number remains 0049.

## API and current state

- `GET /api/admin/v1/couples/{coupleId}/products`: current title, price, mode, status, version, creator, owner and times.
- `GET /api/admin/v1/couples/{coupleId}/products/{productId}`: those fields plus current description.

Both routes require AdminFilter and its attached independent admin identity. A user cookie or a forged surface header
alone cannot authorize these reads. Malformed path ids, limit or cursor return VALIDATION_ERROR 400; unknown couple gives
SPACE_NOT_FOUND 404; unknown/foreign product gives generic NOT_FOUND 404. Uppercase path ids are canonicalized. Responses
use the existing no-store JSON helper. Normal application database role and parent/couple joins remain in force.

Current means the highest immutable reward_product_revisions version, the same rule as ProductService. Admin visibility
includes archived products and both members' created items. This is independent admin metadata permission: ordinary D14
visibility is unchanged (creator can see archived items; owner cannot). No ordinary-user access or redemption eligibility
is restored. Archive remains irreversible through the existing product workflow.

Prices are canonical decimal strings, **1–999 flowers**, not sunshine; the admin client never converts them to Number.
Current catalog price/terms are not the historical purchase snapshot in a redemption. Editing or archiving a catalog item
does not rewrite any redemption. This page exposes neither redemption history nor full product revision history.

Projection is explicit. No keys, fingerprints, raw mutation/receipt JSON, wallet operations, notification fields, assets,
URLs or binary content. The service performs SELECT only; existing admin session housekeeping is unchanged. No new
recovery-hold or product-deletion policy is introduced.

## Pagination and client state

List defaults to 50, accepts 1–100, and bounds a materialized root page to limit+1 before revision projection. Immutable
creation time plus id sort descending; edits and archive do not move a row or remove it from this admin list. Cursor
`rp1.<positive microseconds>.<product id>.<couple id>` binds resource and parent context; numbers are canonical positive
int64 and UUIDs canonical lowercase. Existing indexes suffice. Every response is one snapshot; separate pages and detail
may observe different revisions. No cross-request snapshot guarantee or polling is added.

Protected admin route `/couples/:coupleId/products/:productId?` is linked from the couple table. Existing table/pager styles
are reused. The page separates current price from historical redemption facts and explains admin-only archived visibility.
No write controls exist. Exact validators check fields, subject ids, distinct creator/owner, integer versions, known status
and redemption mode, canonical price/range, duplicate ids and cursor context before showing a response.

List and detail request identities are independent: a new product selection cannot strand a list, and list refresh cannot
strand a pending detail. Couple/session/logout/unmount changes invalidate old replies. Current 401/403 clears every
displayed scope, including space metadata, and invalidates all delayed responses. Current product 404 invalidates a late
list and clears selection content; missing-couple list invalidates late detail. Ordinary errors require explicit retry.
Store data is memory-only, with no auth retry or persistence.

## Verification

Final disposable run **usapp-e1-377da79553** completed successfully (harness exit 0):

| Check | Result |
|---|---|
| Schema / roles | 49 migration checksums; eraser violations zero; privilege deviations zero after apply, repeat and CTest; migration bytes unchanged |
| Build / service suite | Passed; 91 assertions in two new cases |
| Real local HTTP binary | 1300 assertions passed; both-listener admin identity/surface, no-store, subject/limit/cursor validation, public isolation and no domain writes |
| Full CTest | **31/31 in 98.73 s**; admin_product_db 0.08 s; db_gated 13.68 s |
| Postconditions | Early/reversed erasures and ledger chain breaks zero |
| Existing export modes | Connection-loss 8, large 6, closure 23 passed; no transport-stall root-cause fix claimed |
| Existing recovery-closed modes | Album 7 and memory 8 passed; original media-domain closure boundary unchanged |
| Admin frontend | Focused 16; full **203 tests / 14 files**; both typechecks passed |
| Isolated production build | Passed; original admin 20 and web 45 dist files unchanged; output %TEMP%/usapp-admin-product-web-b27112b302 |
| Contract | Strict YAML/ref check passed: 0.56.0, 126 paths, 244 distinct refs; duplicate-key negative control refused. Not OpenAPI semantic validation |

Selected noncredential evidence is in `Claude outputs/admin-product-20261114/`. Generated credentials/env and server logs
were not copied. Containers are stopped and retained; the final database was deliberately recovery-closed by the existing
extra tests. No failed assertion or behavioral correction occurred in this batch's verification run.

The dedicated backend suite covers latest edits, prices 1 and 999, direct/request modes, archive, preserved ordinary D14
visibility, both creators/owners, foreign/unknown parents, empty one-member couple, field allowlists, no domain/financial
writes, tied creation-time paging after edits/archive, and cursor resource/couple/range checks.

Not verified: browser, real admin login, devices, production volumes/data, cloud or providers. Existing export transport
availability limits, clock-guard intermittents, journal/restore requirements and release gates remain open. Not deployed
or committed. No asset-correction or exception-handling workflow is claimed by this read-only batch.
