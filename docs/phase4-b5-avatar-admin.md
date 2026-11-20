# B5: local-admin avatar catalog forms (Codex, 2026-11-10)

## Scope and behavior

The owner asked Codex to continue feature development after the B4 owner-operated browser checks. This batch adds `/admin/#/avatar` against the existing contract 0.44.0. No backend, schema, contract, dependency or deployment change. No database, cloud, credential or production access, commit or vault update.

The existing AdminFilter and SQL functions remain the authority for listener, session, Origin, CSRF, validation, locking and atomic audit. Frontend form restrictions do not implement an authorization boundary.

- Read the complete registered assets, current item catalog (including unlisted and owner counts), current starter configuration and an item's revision history.
- Create a paid or starter item from an unused registered family. New forms default to unlisted. Prices are sunshine only, integer 1–9999; starters send null. Category, starter status and family cannot be changed by a revision.
- Revise name, price, listing and the asset correction. Only the current or a later registry revision of the same family is offered. No-op revisions are blocked in the form. Unlisting stops new purchases; the UI explains existing ownership/equipping remains.
- Configure hair, clothes and expression with starter items of the matching category, including unlisted starters. Preserve expected_version; a changed base must be explicitly reopened. No automatic default selection or enrollment.
- Registry keys and hashes are rendered as text; no key-derived URL, file upload, inline SVG or image preview. The catalog forms do not register assets or write balances, grant items, refund or equip.

## Captured writes and lifecycle

One pending write at a time across the catalog, kept in a Pinia store across route changes. The first click captures a cloned request and a new idempotency key, then displays a separate confirmation region. Only explicit confirmation sends. Forms remain locked until cancellation, acknowledgement or an explicitly dismissed definite rejection.

Transport errors, malformed success, 5xx and RESULT_UNKNOWN retain the original operation/key for click-only retry. A 4xx after an uncertain send cannot settle the earlier attempt and remains uncertain. A first definite 4xx or the admin client's pre-send status=0 rejection is shown as rejected; a later attempt requires dismissal, review and a fresh confirmation/key. A server read never settles a write.

Receipts require the captured original mutation version. The returned current item must be at least that version; terms are compared only at the original version. A newer current item on replay is accepted without describing its terms as the original write. Target/category/family/starter invariants stay checked. Starter receipts are original facts, with exact version and captured ids; after acknowledgement the UI rereads current configuration. The notice explicitly distinguishes the acknowledged version from the reread state.

Session clear, logout, failed login, identity changes and bootstrap/login epochs synchronously clear private catalog, history, action and form state. Late reads/writes are ignored using generation, owner and sequence checks. No state or keys are persisted. Reload/logout therefore loses an unresolved prompt; the UI says so. No cross-reload unknown-operation recovery endpoint is added.

## Changed files

- `apps/admin/src/api/avatar.ts`: independent admin types, response guards, receipt agreement and existing-route client using the shared admin HTTP client.
- `apps/admin/src/stores/avatar-catalog.ts`: private reads/history, sequencing, session scoping and captured writes.
- `apps/admin/src/views/AvatarCatalogView.vue`: registry/catalog/history, create/revise/starter forms, review, refusal and retry.
- `apps/admin/src/stores/admin-session.ts`: in-memory session epoch used to invalidate even a same-admin re-login's prior operations; existing tests retained.
- `apps/admin/src/router/index.ts`, `App.vue`, `styles/main.css`: route, navigation and layout.
- `OverviewView.vue`: replaces the obsolete disabled shop action with the actual route. `CouplesView.vue`: relabels its still-disabled balance-correction action so it does not claim catalog maintenance is missing.
- New tests: `avatar-fixtures.ts`, `avatar-catalog.spec.ts`, `avatar-admin-client.spec.ts`.
- `tools/avatar-b5-verify.ps1`: offline verification retaining logs and an isolated admin build, snapshots of both existing dist directories, contracts and frozen migrations.

## Automated evidence

Final run: `Claude outputs/b5-admin-3ed9ec5caf`.

| Check | Result |
| --- | --- |
| Catalog/store/form and client tests | 30/30 |
| Full admin suite | 4 files, 63 tests, exit 0 |
| Application vue-tsc | exit 0 |
| Node-config vue-tsc | exit 0 |
| Isolated Vite production build | exit 0, output `admin-dist` inside evidence folder |
| Existing apps/admin/dist, apps/web/dist | unchanged (including an absent dist remaining absent) |
| Contracts and frozen migrations | unchanged |

Coverage includes exact and newer replay responses, too-old/unrelated responses, original configuration receipts, fixed key/body after unknown then 4xx, first rejection and new confirmation, malformed/aborted responses, pending-write serialization, session invalidation, late reads/writes, empty registry, atomic malformed-read refusal, correct admin CSRF/headers/credentials/paths, no persistence, category/starter immutability, correction family filtering, full configuration submission, draft preservation and stale-base refusal.

Initial 28 focused and 61 full tests/typechecks passed; two further form regressions were added before the final 30/63 run. No assertion or build failed during this B5 batch. Tests use fake responses and DOM rendering. No mutation testing, real admin/API/browser/device, audit database inspection or listener-boundary re-verification was performed. Backend tests and the user web suite were not rerun because their sources did not change.

## Pending / release boundaries

NOT DEPLOYED. No catalog or registry enrollment occurred. The real registry can still be empty. Browser layout, actual admin session/CSRF behavior, real HTTP writes, audit rows, concurrent admin edits and post-commit response loss remain unverified in this frontend batch; earlier backend evidence is not upgraded by frontend mocks. Admin build output is retained outside apps/admin/dist. Formal artwork, achievements, export, other admin functions and existing release gates remain separate work.
