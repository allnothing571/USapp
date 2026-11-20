# B4: avatar shop, wardrobe and home (Codex, 2026-11-10)

**Implemented with placeholder art; offline automated verification passed. NOT DEPLOYED.** The owner authorized Codex to take over after Claude Code's quota ended. No B4 files were found at takeover. This batch uses contract 0.44.0 and changes no backend service, contract, dependency, deployment configuration or migration. Frozen migrations remain 0000–0046; no new migration number was reserved.

## 1. Product behavior

- `/avatar` is the shop; `/avatar/wardrobe` is the wardrobe. The home character and the existing “我们” page provide entry points. Existing home balances, tasks, anniversary, message and pending-work sections remain.
- The shop reads the global authenticated catalog, the caller's wallet and paginated owned items. A starter is free availability, not a purchase. Unknown/error/partially read ownership is never advertised as a complete inventory: further purchase confirmation waits until the owned list is complete. Pagination remains explicit.
- A confirmation captures the item, revision, price and owner before the purchase. The write captures a new key once, sends only item_id/expected_version, and never auto-equips. Original receipt facts and original post-debit balance are labelled as historical; the shared wallet store reloads through its own sequence rather than installing those numbers.
- The wardrobe offers catalog starters, the caller's owned items (including unlisted items), and items already in the caller's appearance. Unowned paid catalog items are not offered for equip. Same-revision merges retain a known listing flag when the appearance API omits it.
- Draft and server appearance are separate. A complete six-slot save uses the captured loadout version. Null required slots mean server-resolved defaults, not an arbitrary starter chosen by the client; their preview is explicitly unresolved. Missing starter configuration is shown honestly, and the backend remains authoritative.
- On a refusal/conflict the draft stays. The user can read the current appearance, compare it, then explicitly accept its version as the new base while retaining the draft. No automatic merge or overwrite occurs.
- An unconfirmed purchase/save keeps its captured key and body and blocks another operation of the same kind. Retry requires a click. A later 4xx cannot prove the original uncertain write did not commit, so the action stays unresolved. A first definite refusal can be explicitly dismissed before making a new request. Uncertain actions are memory-only and are lost on reload.
- A confirmed save with current=null is still a confirmed save: show the confirmation, fail the current-view read and offer reread. It does not request another write.
- Generation/owner checks discard another identity's replies; read sequences discard superseded reads. Save replies cannot reduce the confirmed loadout version. For an identical item, a delayed reply cannot replace already-observed newer item metadata at the same loadout version.
- Home refresh, foreground/online refresh and identity changes also read the caller's appearance. A loading/error/incomplete figure is labelled as a base placeholder, never as the confirmed saved outfit.

## 2. Placeholder assets and registry enrollment boundary

There are **14 developer-made SVG files and nine registry entries**, on a shared transparent 512×512 canvas: two hairstyles (back/front/front-under-hat), two clothes, two expressions, one hat, one handheld flower and one neck accessory, plus the fixed base body. They are **not final artwork or an approved final art direction**.

Layer order is rear accessory → back hair → body → clothes → expression → neck accessory → normal/under-hat front hair → hat → face accessory → handheld → above-hat accessory. Optional positions are metadata-driven; only the neck accessory is supplied in this set. This order does not prove every future combination fits; that remains an art-review requirement.

`src/assets/avatar-manifest.json` maps known keys and exact placement metadata to bounded public files and SHA-256 values. Unknown keys or mismatching metadata never become arbitrary URLs. Missing files produce a stable base fallback with an explicit notice; saved IDs are not rewritten. An explicit image retry is available.

`node tools/avatar-placeholder-assets.mjs` verifies the actual bytes, manifest, exact file set and layer sets. `--write` creates missing files only after checking every existing versioned file/manifest entry; it refuses overwriting different bytes under an existing version. Corrections require a new asset version.

**No registry, catalog or default was enrolled in any database.** An empty live registry/catalog is therefore still empty. The interface never fabricates purchasable demo items.

Prepared enrollment input can be obtained without any database call:

```powershell
node tools/avatar-placeholder-assets.mjs --registry-json
```

This produces nine avatar_assets-shaped rows, with per-layer hashes, not items, grants, prices or starter configuration. The final verification retains it as `registry-review-only.json`.

Before enabling a real catalog, a separately authorized maintenance batch must review and add these rows under the existing registry privileges, refuse conflicting existing keys, and compare the registered hashes with the exact deployed files. The audited B1 admin APIs then create items and explicitly select the required defaults. The suggested placeholder starters are `hair.placeholder_bob.v1`, `clothes.placeholder_cream.v1`, `expression.placeholder_smile.v1`; registry rows alone do not make them starter items. This enrollment and its disposable-database verification remain pending; no maintenance write or migration was hidden in B4.

Hash checking here is local/build evidence, not browser subresource-integrity or real-provider verification. Public bundled layers may be cached on first use; private catalog/owned/appearance/purchase/save API data remains outside the service-worker cache and all avatar state remains in memory. Avatar image files are excluded from precache to avoid preloading the complete art set.

## 3. Files

New web code:

- `apps/web/src/api/avatar.ts`, `avatar-art.ts`: contract types, guarded responses, API client and closed art resolver.
- `apps/web/src/stores/avatar.ts`: identity, sequence, ownership paging and captured purchase/save actions.
- `apps/web/src/components/AvatarFigure.vue`, `AvatarHome.vue` and `src/views/AvatarView.vue`.
- `apps/web/src/assets/avatar-manifest.json` and 14 files under `public/avatars/`.
- `apps/web/tests/unit/avatar.spec.ts`, `avatar-client.spec.ts`, `avatar-fixtures.ts`.

Existing web wiring: session reset, router, HomeView, UsView, error codes/messages, styles and the public-art precache exclusion in vite.config.ts. Existing home/home-wallet tests gain a fake appearance API; their original checks stay, plus one home-integration regression.

Tools: `avatar-placeholder-assets.mjs` (offline checker/generator/review JSON), `avatar-b4-verify.ps1` (offline verification), `avatar-b4-browser.mjs` (explicitly launched loopback Vite/fake API only). The fake API is never imported into production configuration and contains no real users, credentials or wallet data.

## 4. Verification and failures

Final evidence: `Claude outputs/b4-web-8c7ba75cef/`.

| Check | Result |
|---|---|
| Avatar-focused tests | 46 passed (43 store/component/art, 3 real-client-with-fake-fetch) |
| Full web suite | 45 files, 920 passed; exit 0 |
| vue-tsc | exit 0 |
| Node-config tsc | exit 0 |
| Isolated production build | exit 0, under evidence/web-dist |
| Built art bytes | all 14 hashes match |
| Production service-worker build | 50 precache entries, 520.52 KiB; no avatars/ image entry |
| Existing apps/web/dist | all 45 files unchanged |
| Frozen migrations | all 47 files unchanged |

The assertions exercise confirmation before a write; sunshine-only payloads; starters/owned/unlisted items; empty catalog/defaults; paging; unknown responses and fixed-key retries; conflicts preserving drafts; a confirmed save with current=null; owner changes and late responses; wallet refresh sequencing; full-slot saves; public-art ordering/fallback/hashes; no Storage.setItem from avatar operations; private API cache exclusion; and home integration. They are fake-data/DOM/client evidence, not browser network traces or real API acceptance.

Failures retained as part of the development evidence:

1. Initial typechecks found unknown layer-array typing, a SlotIds/FigureSlots mismatch, typed mock-call access and an optional image attribute. Corrected without relaxing typechecks.
2. A new client test incorrectly reused one Response body across three reads; its fixture now creates a fresh Response per call.
3. A regression failed before the fix: a delayed save receipt replaced newer item metadata at the same loadout version. The store now preserves the newer observed revision of the same item. The focused and final suites pass afterwards.
4. A further regression failed before the fix: an equipped-item read without listed erased an owned row's known unlisted flag. Equal-revision metadata is now merged without losing that field; a newer revision still replaces older metadata. Final tests cover this.
5. The first verification passed focused/full tests and both typechecks but its production build failed during the service-worker stage under a per-invocation sandbox Temp directory. The complete diagnostic file could not be recovered after that sandbox ended; the exact cause is unresolved. A controlled workspace output containing `#` built successfully, so a simple “# path causes it” hypothesis is not supported. Verification now retains outputs in a fresh workspace evidence folder, and both later production builds passed; this is not claimed as proof of the original cause or an application-code fix.

An earlier persistent run (`b4-web-4edf0e5e97`) passed 919 tests/build before the final unlisted regression and fix. Only `b4-web-8c7ba75cef` is the final-source result above. No assertion failure was silently rerun without a diagnosis/change.

## 5. Browser attempt and remaining acceptance

**Owner-operated follow-up (2026-11-10):** automatic Chrome control again stopped on URL identification. The owner opened the loopback fake site manually. Their supplied 375 px shop screenshot showed viewport 375, scrollWidth/bodyScroll 360, localStorageKeys/sessionStorageKeys/cacheNames empty, balance 100 and no visible horizontal overflow in that screen. The owner reported purchase success and later reported wardrobe/save/home success after being instructed to retry the deliberately unknown save. The exact response/key/count sequence was not captured, so this is owner-reported workflow evidence, not independently observed replay, balance or network proof. The owner explicitly stopped remaining checks; desktop, other layers, account switching and empty/missing-art browser scenarios were not passed or waived as release gates. The temporary server was stopped before B5 verification. The historical paragraphs below describe the original attempt.

The loopback fake API was started on 127.0.0.1:3764 with synthetic accounts and deliberately unknown first purchase/save replies. The in-app browser was unavailable. Native Windows computer-use then opened a temporary Chrome incognito window, but the tool halted before navigating to the test site because it could not determine the browser URL confidently enough to enforce policy. No workaround or further UI input was attempted.

**No browser flow, 375 px/desktop visual check, screenshot, actual layout measurement, cache/storage browser trace or device acceptance passed.** The prepared iframe-width harness and fake scenarios remain unexecuted in a browser. The local server was stopped; no listener remained on 3764. The temporary incognito window could not be closed through the halted tool and may be closed manually by the owner.

Backend tests were not rerun: backend, schema and contract did not change. Real post-commit response loss for B1/B2/B3 remains unverified; frontend mocked response loss does not close it. Registry enrollment, formal art, B5 admin forms, real API/browser/device/service-worker lifecycle, restore and existing release gates remain pending. No production/shared database/cloud/provider access, purge enablement, deployment, commit, push or vault update occurred.
