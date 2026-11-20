# P3-2b shared albums: rename and rename requests (design for owner review)

Status (2026-11-04): **implemented and verified with a fake object store in disposable isolated instances only: Section 7. NOT DEPLOYED.** The owner approved D18 Option A. `0031_albums` is frozen and was not edited: everything below is additive, in a new migration. Background: [phase3-p32-albums-design.md](phase3-p32-albums-design.md) (P3-2a, Section 12).

**Labels.** [V1] stated in the original requirements. [Fact] read from this repository. [Rec] proposed default. [Unverified] nothing has executed it.

## 1. Summary

- **Creator:** renames directly, with `expected_version`.
- **Partner:** sends a rename request. The creator approves or rejects it, and the partner may withdraw it. One pending request per album.
- **While a request is pending:** the creator must decide it before renaming. That is why a pending request can never go stale.
- **Photos and covers are independent of the name:** they do not change the album's version and never conflict with a rename or a request.
- **Precedent:** the memory and anniversary edit flows, narrowed to one field. Rename takes only lock 33021, never a media lock, and calls no store.

## 2. Owner decision: D18

**Approved by the owner on 2026-11-04: Option A, with the six rules below.** No public stale-proposal code was added: the build exposed no reachable stale case (Section 7.3).

| | **A. Direct for the creator, request for the partner ([Rec])** | B. Either member renames directly | C. Creator only, no requests |
|---|---|---|---|
| Rule | As in Section 1 | A name is treated as not body content, like a cover (§31 by analogy) | The partner has no way to change a name |
| What users see | The partner's idea waits for the creator. The creator must answer before renaming again | Either can change a name at once, without asking. The last write wins | The partner must ask outside the app |
| Fits [V1] §29-30 | **Yes**, if a name is body content. V1 does not say whether it is | Only if a name is not body content | Partly: it omits the request §29 describes |
| Build | Two tables, two guards, four routes | One route, a relaxed revision guard, no tables | One route, no tables. A later request flow needs another migration |
| Blocks | P3-2b's migration and contract | Same | Same |

**Exact default to approve (Option A):**

1. An album's name is body content for §29. Only its creator renames it directly.
2. The partner sends a rename request. The creator approves or rejects it. The partner may withdraw it.
3. At most one request is pending per album.
4. While one is pending, the creator's direct rename is refused with `ALBUM_PROPOSAL_PENDING`.
5. Approval applies exactly the requested name, as a new revision.
6. No notification is sent. D20 stays open, so the other member sees a request only by opening the album.

Withdrawal and rule 4 are [Rec], not [V1]: §30 lists only Pending, Approved and Rejected. They follow the memory flow. **If rule 4 is the part you would change**, the alternative is that a direct rename makes a pending request stale, which adds a reachable stale state, an error code and more tests.

## 3. What each write does

**Order inside each write:** member row `FOR SHARE` with album → 33021 → key classification → role and state → inserts → audit → read-back → commit. No 33018, no 33020. `name` is normalized before any of it, with the P3-2a rules: 1-40 code points, task-title rules.

### 3.1 Rename and request

Checked in this order. **Classification comes first, so a replay never sees a state error.**

| Case | Creator: `POST .../revisions` | Partner: `POST .../proposals` |
|---|---|---|
| Absent or another couple's album | `ALBUM_NOT_FOUND` 404 | Same |
| Same key, same body | **200 replay:** original facts, today's album | Same |
| Same key, different body | `IDEMPOTENCY_KEY_REUSED` 409 | Same |
| Wrong member for this route | `ALBUM_EDIT_FORBIDDEN` 403 | Same |
| `expected_version` is not current | `ALBUM_VERSION_CONFLICT` 409 | Same |
| A request is pending | `ALBUM_PROPOSAL_PENDING` 409 | Same |
| Same name as now | `VALIDATION_ERROR` 400 | Same |
| Otherwise | 201: text, revision `edit`, audit | 201: text, proposal, audit |

### 3.2 Decision: `POST .../proposals/{id}/decision`, `{outcome}`

| Case | Answer |
|---|---|
| Absent or foreign album | `ALBUM_NOT_FOUND` 404 |
| Proposal not of this album | `ALBUM_PROPOSAL_NOT_FOUND` 404 |
| Same key, same outcome | **200 replay:** original facts, today's album, even after later renames |
| Same key, other outcome or proposal | `IDEMPOTENCY_KEY_REUSED` 409 |
| `approve` or `reject` by the partner; `withdraw` by the creator | `ALBUM_EDIT_FORBIDDEN` 403 |
| Already decided, new key | `ALBUM_PROPOSAL_DECIDED` 409 |
| `reject`, `withdraw` | 200: decision, audit. The name does not change |
| `approve` | 200: decision first, then revision `approve` with the proposal's text, as `base_version + 1`, then audit, all in one transaction |

`actor_id` of an approved revision is the creator, who approved it. The proposer stays on the proposal. A rejected or withdrawn request keeps its text and stays readable.

### 3.3 Stale base revisions

| Situation | Result | Reachable? |
|---|---|---|
| Rename or request with an old `expected_version` | `ALBUM_VERSION_CONFLICT`, nothing written | Yes |
| Request's base moved before approval | Decision guard refuses `USAPP_ALBUM_DECISION_STALE`. The service reports a 500 with a fixed label | **No, through any permitted write:** rule 4 blocks every revision while a request is pending |
| A decided request against a newer version | `ALBUM_PROPOSAL_DECIDED`, or replay on its own key | Yes |

**So there is no public `ALBUM_PROPOSAL_STALE` code** [Rec]. Memories and anniversaries carry one for this state ([Fact] `MemoryService.h:58`), and it could not be reached through the service here. The database guard still stands. It is tested by a superuser-only ledger case that disables the revision guard inside a rolled-back transaction.

### 3.4 Concurrent cover and photo changes

All of these take 33021 on one album, so they serialize. None of them conflict:

| Write | Touches `version`? | Effect on rename or request |
|---|---|---|
| Add photos, remove a photo, set a cover | **No** | None. They take no `expected_version` and do not stale a request |
| Rename, approve | **Yes** | None on items, removals or cover events |

Each response reads today's album after its write, so whichever commits second shows both changes. A replay shows the original facts and today's album, including later photo and cover changes.

### 3.5 Concurrent name writes

| Race | Outcome |
|---|---|
| Two renames at one version | One 201, one `ALBUM_VERSION_CONFLICT` |
| Rename against request | Rename first: `VERSION_CONFLICT` for the request. Request first: `PROPOSAL_PENDING` for the rename |
| Two requests | One 201, one `PROPOSAL_PENDING` or `VERSION_CONFLICT`, by whichever check it reaches first after the wait |
| Approve, reject and withdraw at once | One wins; the others `PROPOSAL_DECIDED` |
| Same key at once, on any route | One write, one replay |

## 4. Smallest implementation scope

### 4.1 Migration: next free number `0032_album_rename` (recheck before use)

`0031_albums.sql` is not edited. New SQLSTATE values in the existing class: `UH007` proposal, `UH008` decision; `UH006` for the new deferred link.

```
album_edit_proposals   id, couple_id, album_id, proposer_id, base_version, text_id, key, fingerprint, created_at
                       UNIQUE (id, couple_id); UNIQUE (proposer_id, key); FK (album_id, base_version) -> album_revisions
album_edit_decisions   proposal_id PK, couple_id, decided_by, outcome (approved, rejected, withdrawn), key, fingerprint, decided_at
                       UNIQUE (decided_by, key)
```

Both are append-only, `SELECT, INSERT` for `usapp_api`, nothing for `usapp_push`.

| Change to a 0031 object, additive by `ALTER` or `CREATE OR REPLACE` | Why |
|---|---|
| `album_revisions`: add nullable `proposal_id` with `UNIQUE` and an FK | Links an approved revision to its request |
| `album_revisions`: drop `NOT NULL` on the key and fingerprint; `CHECK` that they are NULL exactly for `approve` | An approval's key lives on its decision, as in 0030 |
| `album_revisions_action_valid`: add `approve`; `CHECK (action = 'approve') = (proposal_id IS NOT NULL)` | 0031's Section 6.2 reserved this for P3-2b |
| `usapp_album_revision_guard`: refuse `edit` while a request is pending; `approve` needs an approved decision by the actor, base equal to the previous version, and the proposal's text | Rule 4 and rule 5, in the database |

| New guard | Checks |
|---|---|
| Proposal | 33021; proposer is a member and not the creator; base is current; no other pending request |
| Decision | Proposal of this couple; 33021; approve and reject by the creator, withdraw by the proposer; approve only if base is current |
| Deferred | An approved decision has exactly one `approve` revision |

The lock order in 0031 holds: 33021 only, never 33018. `usapp_media_shared_now`, the binding guard and the plan guard are not touched.

### 4.2 API: contract `0.27.0`, additive

| Route | Caller | Success |
|---|---|---|
| `POST /api/v1/albums/{id}/revisions`, `{expected_version, name}` | Creator | 201; 200 replay |
| `POST /api/v1/albums/{id}/proposals`, `{expected_version, name}` | Partner | 201; 200 replay |
| `GET /api/v1/albums/{id}/proposals/{proposalId}` | Both | 200: original name, proposed name, proposer, created time, status, decider. §30's required fields |
| `POST /api/v1/albums/{id}/proposals/{proposalId}/decision`, `{outcome}` | By outcome | 200; 200 replay |

`Album` gains `version` and `pending_proposal` (`id`, `proposer_id`, `base_version`, `created_at`, or null). New codes: `ALBUM_EDIT_FORBIDDEN`, `ALBUM_VERSION_CONFLICT`, `ALBUM_PROPOSAL_PENDING`, `ALBUM_PROPOSAL_NOT_FOUND`, `ALBUM_PROPOSAL_DECIDED`.

### 4.3 Service, policy and tests

`AlbumService` gains the three write operations and the proposal read. `AlbumController` gains the four routes. The two tables join `TaskPrivilegePolicy`, `init-roles.sql` section 26 and the deviation script. A build with P3-2b needs 0032 at startup.

**Existing assertions that must change, because they state P3-2a's scope:** the `0.26.0` version checks, the static check that the `/revisions` route is absent, and the live check that it answers 404 or 405. Nothing else in `album_ledger_test.sql` or `AlbumDbTests.h` should change, and any further change would be reported first.

### 4.4 Left out

Web (P3-2c), real OSS (P3-2d), notifications (D20), a list of past requests, photo or cover requests, deletion (D6), and P3-3.

## 5. Isolated verification cases

Disposable instance, fake store, no shared database, no vendor. Harness modelled on `Claude outputs/p32a-isolated-verify.ps1`: 0000-0031 applied, representative data including albums, API without 0032 refuses, rollback-only prevalidation in both roles, first apply, repeat apply, checksums, zero privilege deviations, targeted suites, real API binary, full CTest, all ledgers, strict YAML. Existing timeouts stay. Complete logs first, bounded steps, and only this run's containers are stopped.

| # | Area | Must show |
|---|---|---|
| 1 | Creator rename | Version +1; old revision stays; list order unmoved; audit holds ids only, never the name |
| 2 | Rename refusals | Partner 403; old version 409; same name 400; name shape 400, at 40 and 41 code points; foreign and absent albums answer alike |
| 3 | Rename replay | Same key 200 with original facts and today's album, after a later rename too; different body `KEY_REUSED`; no new revision. A create key and a rename key by one actor cannot pose as each other |
| 4 | Request | Partner 201; name unchanged until approved; creator 403 on this route; one pending; both members read it; another couple 404 |
| 5 | Approve | Name becomes exactly the proposal's; version +1; `action = approve`; the actor is the creator; decision and revision commit together |
| 6 | Reject and withdraw | Name unchanged, version unchanged; the wrong member 403; the request stays readable with its status; a new request is then allowed |
| 7 | Decision replay | Same key 200, after later renames too, with original facts; other outcome `KEY_REUSED`; new key `PROPOSAL_DECIDED`; one decision and one revision |
| 8 | Pending blocks | Creator rename `PROPOSAL_PENDING` until decided, in the service and in the database |
| 9 | Stale | Old `expected_version` refused on both routes. Superuser ledger only: with the revision guard disabled inside a rolled-back transaction, approval refuses `USAPP_ALBUM_DECISION_STALE` and an approve revision refuses a mismatched base |
| 10 | Name races | Each row of Section 3.5, repeated, with no deadlock and exactly the stated row counts |
| 11 | Photo and cover independence | With a request pending: add, remove and cover all succeed, and the request's base stays valid and approvable. Approve racing a cover, an add and a removal: both commit, and the final album shows both. Version moves only on name writes |
| 12 | Lock independence | A held 33018 blocks no rename, request or decision; a held 33021 blocks them, deterministically by blocked-statement count; a held 33020 blocks nothing here; completion and the cleanup job finish throughout |
| 13 | Rollback | Injected failure after the approval's decision leaves no decision, revision or audit row; the request stays pending; the same key then succeeds |
| 14 | Isolation in SQL | Text, proposal and decision across couples refused by foreign keys and guards; append-only on both tables; `usapp_push` has no access |
| 15 | P3-2a and memory regressions | `album_ledger_test.sql` passes unchanged, with `edit` still creator-only. Both cleanup races, partner access after removal, and memory create and approval pass unchanged |
| 16 | Real API binary | New routes through the real filter: 401, 403 without CSRF, 400 without a key, then the real creator and partner flow end to end. No store is needed, because a rename touches no photo |
| 17 | Old build | The P3-2a binary starts on 0032 and reads an album with an `approve` revision. Added to the release rehearsal in Section 18.1 of the plan |

## 6. Basis

**Static, read directly, nothing executed:** `_req_extract.txt` §29-31; `database/migrations/0031_albums.sql`, `0030_memories.sql`; `services/api/src/services/MemoryService.{h,cc}`, `AlbumService.{h,cc}`; `services/api/tests/AlbumDbTests.h`, `ReminderLiveApiTest.h`, `StaticCheckTest.cc`. P3-2a's verification (design Section 12) is the only execution evidence, and none of it covers P3-2b.

Before this batch, the `ALTER` statements on existing rows and an old P3-2a binary on 0032 were unverified. Section 7 records what was then executed.

## 7. Implementation and isolated verification (Claude Code, 2026-11-04)

**Real workflow not verified.** No browser, device, real OSS, shared `usapp_test` or production was used, and nothing is deployed. Every result is from disposable instances with `FakeObjectStore`, on an internal network with no vendor credential.

### 7.1 State, stated exactly

| Item | State |
|---|---|
| Names | Rechecked before use, all free: `0032_album_rename`, `UH007`, `UH008`, the five new `ALBUM_*` codes. Contract was `0.26.0`, now `0.27.0`. `init-roles.sql` section 26 |
| `0032_album_rename` | sha256 `dcc89ec0375801c61596fac49748b63e6cd069598022c0f5a45ea2765170e68a`. **FROZEN** by its first committed application, in disposable project `usapp-p32b-20261104173152-2e579a`. **Applied in disposable instances only:** not on shared `usapp_test`, not on production |
| `0031_albums` | Not edited: sha256 still `651dcddf086a01bd34078abf9f7072acc732a23874c0f2621cfb3670850c7c16` |
| Build dependency | A build with P3-2b needs 0032 at startup. Without it the API refuses to start, naming the 0032 tables. On a database without 0032, `db_gated` fails its migration gate |
| Next migration number | `0033` |
| Flags and config | None added or changed |

**Files.** New: `database/migrations/0032_album_rename.sql`, `database/tests/album_rename_ledger_test.sql`, `services/api/tests/AlbumRenameDbTests.h`, `services/api/tests/AlbumOldApiTest.h`, `Claude outputs/p32b-isolated-verify.ps1`, `Claude outputs/p32b-logs-yaml.log`. Changed: `AlbumService.{h,cc}`, `AlbumController.cc`, `TaskPrivilegePolicy.cc`, `Errors.{h,cc}`, `database/init-roles.sql`, `database/tests/task_privilege_deviation.sql`, `contracts/openapi.yaml`, and the test wiring in `DbGatedTest.cc`, `UnitTest.cc`, `AlbumUnitTests.h`, `StaticCheckTest.cc`, `ReminderLiveApiTest.h`. `album_ledger_test.sql`, `memory_ledger_test.sql`, `media_ledger_test.sql`, `AlbumDbTests.h`, `MediaService`, `MemoryService` and every frozen migration are unchanged.

### 7.2 Verification

Run `usapp-p32b-20261104173152-2e579a`, exit 0, with `Claude outputs/p32b-isolated-verify.ps1`. Complete logs are saved before filtering under `%TEMP%/usapp-p32b-20261104173152-2e579a` on the verification host. Generated credentials found in those logs: 0. An earlier `-StopAfterPrevalidation` run, `usapp-p32b-20261104172753-9b183e`, committed nothing.

| Check | Result |
|---|---|
| Rollback-only prevalidation, against 0031 with existing album data (28 albums, 73 items, 30 removals, 9 cover events, 28 revisions, all from the P3-2a image), in one transaction with four ledgers | As `postgres`: rename 36, album 76, memory 64, media 53 cases. As `usapp_api`: rename 34, album 76, memory 64, media 53. The two extra cases are the superuser-only forced stale state. `ROLLBACK`. Afterwards: 0032 unregistered, no tables, column or function; rows and the album-revision digest unchanged |
| API without 0032 | Refused to start, exit 1, naming the 0032 tables |
| First committed apply | Exactly 0032. Existing rows, bindings and notifications unchanged. Digest of every existing `album_revisions` row, over its 0031 columns, unchanged |
| Repeat apply and InitRoles | 33 skipped, 0 applied |
| Checksums | 33 registered, each equal to its local file |
| Privilege deviations | 0 after apply, 0 after repeat |
| Targeted suites, direct | `oss_stub` 363 assertions; album with rename 722 (2 s); memory 260; media 239 (11 s); service-through-adapter 41. No failures |
| Full CTest | **6 of 6 passed, 56.31 s:** unit 11.19 s (2473 assertions), `db_gated` 14.74 s of its 30 s (4098), `media_db` 9.92 s of 30 s (239), `push_dispatch` 15.43 s (541), `oss_stub` 5.01 s (363), `static_check` 0.01 s (625). No timeout changed; no target added |
| Real API binary, through the real filter, media disabled | 124 assertions passed, including rename and request end to end with two real sessions: 401, 403 without CSRF, 400 without a key, each refusal code, approve, replay and decided |
| SQL ledgers in both roles after CTest | Album rename, album, memory, media, reminder, notification, notification choice, anniversary and task pass; push dispatch in both modes passes; each `ROLLBACK` |
| Strict YAML | `Claude outputs/p26r1-yaml-check.py`, log `Claude outputs/p32b-logs-yaml.log`: negative control refused; contract parsed, 0.27.0, 73 paths, 85 operations. YAML only, not OpenAPI semantics |
| PostgreSQL server log | `deadlock detected`: 0 occurrences |
| After CTest | 54 requests, 44 decisions, 36 approve revisions. Approved decisions without one approve revision, approve revisions without an approved decision, and pending requests not on the current version: 0, 0 and 0. Album bindings with no item and items with no binding: 0 and 0. Plans on any bound photo: 0. Push control: disabled |

**Section 5's cases.** 1-8, 10-13 and 15 ran through the real service in `AlbumRenameDbTests.h`; 9 and 14 in `album_rename_ledger_test.sql`; 16 through the real binary; 17 in Section 7.4. Case 9's forced stale state is superuser-only, inside a block that undoes itself.

### 7.3 No reachable stale case

The build exposed none, so **no `ALBUM_PROPOSAL_STALE` was added** and the database guard stays. Evidence: a request's guard requires the current version; a direct rename is refused while one is pending, in the service and in the database; approval can only act on that request; and all of these take 33021. Rename against request raced repeatedly, each outcome being one of the two stated in Section 3.5, and no album in the run ended with a pending request off its current version. This shows the service paths exercised; it is not a proof over every interleaving.

### 7.4 Observed rollback compatibility: the P3-2a binary on 0032

Image `usapp-isolated-test-runner:p32a`, `sha256:0fd5cbef04b3...`, built from the P3-2a tree. Media disabled, scheduler off. Fixtures with an approved rename and a pending request were written by this build's service. Only the HTTP answers come from the older binary.

| Observed | Result |
|---|---|
| Startup on 0032 | Started. Errors logged: 0 |
| Its own live test against itself | 90 assertions passed |
| Reads an album whose current revision is `approve`, with a NULL key | Correct name in get and list. Its older shape has no `version` or `pending_proposal` |
| Its writes under a pending request | Create album 201, add photo 201, set cover 201, remove photo 200 |
| Rename routes | `revisions`, reading a request, and deciding one all answer 404 or 405 |
| Back on this build | The pending request survived with its base still current, the older binary's writes were all present, and it approved normally to version 3. The older binary then read that name |

All 24 assertions of the probe passed. **What this means for a rollback to P3-2a:** data stays compatible in both directions as far as tested. While on the older binary, nobody can see or decide a pending request or rename an album, and those requests stay pending until a roll forward.

**Not covered:** any build older than P3-2a, including the P3-1 and production-baseline builds in Section 18.1 of the plan. That rehearsal, and the removed-photo exposure it concerns, remain open. The scheduler was off for the older binary here.

### 7.5 Deviations and limits

| Item | Detail |
|---|---|
| Assertions changed for scope | As Section 4.3 predicted: three `0.26.0` version checks, the static check that `/revisions` is absent, and the live check that it answers 404 or 405 |
| Harness | The P3-2a image's `db_gated` provides the album data with `USAPP_TEST_SEPARATE_MEDIA=1`, as its CTest target does, to stay inside the 30 s direct limit |
| Added beyond Section 4 | `album_edit_proposals_album_idx`; mutation `version` on create; `USAPP_ALBUM_APPROVAL` as one refusal for every way an approve revision can mismatch |
| Fault hook | Covers failure after the decision only. A commit refused for a missing approve revision is tested directly |

**Unverified or left out:** browser and device, real OSS, deployment, frontend (P3-2c), notifications (D20: a request is seen only by opening the album), and every open item from P3-2a's Section 12.4. Stale-state guards are executed only by forcing them as a superuser. The earlier `db_gated` abort stays unexplained; it did not recur, which says nothing about its cause.

## 8. Rename-request history (Claude Code, 2026-11-08)

Closes gap 1 of phase3-p32-albums-design.md Section 13.3: a proposer could not find a decided request after the same session ended. Modelled on the memory history (docs/phase3-plan.md Section 25), with album rules kept. **NOT DEPLOYED.** Real browser, device, cloud and restore acceptance are deferred, not passed and not waived.

### 8.1 Implemented behaviour

**Backend.** New `GET /api/v1/albums/{albumId}/proposals`, beside the existing POST on the same path. It is an authenticated read behind `AuthFilter`; it writes, locks or changes nothing.

| Aspect | Behaviour |
|---|---|
| Who | Both members of the album's couple. Another couple's member, a caller in no space, an absent id and a malformed id get `404 ALBUM_NOT_FOUND`, which is what `listAlbumPhotos` answers. No session gets 401. Under a J3b recovery hold, the album tables' row security hides the album, with the same answer |
| Order and pages | `(created_at DESC, id DESC)` at microsecond precision, with the album list's keyset cursor (`AlbumService::encodeCursor`). Requests are immutable, so a walk never repeats or skips one. `limit` 1-100, default 50 |
| Each item | `id, album_id, proposer_id, base_version, created_at, status, decided_by, decided_at, approved_version`, from the same tables and joins as `getAlbumProposal`. No name, idempotency key or fingerprint. The existing detail read returns `original_name` and `proposed_name` |
| Page | `proposals`, `next_cursor`, and `current_version`: the album's latest name version, read by the same statement |
| Name versions | Only rename revisions create them. Photo adds, removals and cover choices move neither `current_version` nor any entry (tested) |

**No migration:** `album_edit_proposals_album_idx (album_id, created_at DESC, id DESC)` from 0032 serves the query. 0000-0042 are unchanged; the next number stays 0043. **Contract 0.37.0 -> 0.38.0**, additive: `listAlbumProposals` and `AlbumProposalListResponse`.

**Web.** Album detail gains a "rename request history" card (`改名申请记录`):

- All four statuses, who asked, the base version, and who decided when, with "load more".
- The card explains that a later rename may have replaced a name that was approved then. Each approval says whether its version is still today's name.
- A decided request opens its original and proposed names, read from the existing detail endpoint. The original is labelled as the version the request was based on.
- A pending request points to the existing controls. The history has no decision controls; the album store's per-album write serialization, unknown-result retry and replay are unchanged.
- The store keeps its own history and comparison request sequences. Leaving the page, an identity change and an album that cannot be read all clear them; late answers for another album, route or account are dropped. Nothing is kept in browser storage.
- The history is read only after the album has read for the current account, and again only when the name version or the pending request changes.

**A defect found and fixed during this batch.**
- The first watcher returned a new array on each evaluation. Vue therefore fired it whenever the album object was replaced, so a cover answer with the same version read the history again.
- The tightened web test caught it after its fake cover response was corrected.
- The memory detail view (Section 25 of the plan) had the same pattern. Both views now use separate watch sources.
- A new memory test checks that a photo refresh returning the same version reads nothing. That test was written after the fix and was not run against the old code; the album test did fail against it.

### 8.2 Files

- **Backend:** `services/api/src/services/AlbumService.{h,cc}` (`listProposals`, `USAPP_PROPOSAL_LIST_SQL`, `ListResult::currentVersion`) and `services/api/src/controllers/AlbumController.cc`.
- **Contract:** `contracts/openapi.yaml`.
- **Backend tests:**
  - New `services/api/tests/AlbumProposalHistoryDbTests.h`, run by the new CTest target `album_history_db` (`USAPP_TEST_ONLY_ALBUM_HISTORY=1`, `TIMEOUT 30`). It is kept out of `db_gated`, whose margin Section 26 of the plan restored, and is not part of a direct full run, like `memory_deletion_db`.
  - `DbGatedTest.cc` and `tests/CMakeLists.txt`: registration.
  - `ReminderLiveApiTest.h`: the route through the real API binary.
  - `StaticCheckTest.cc`: route behind `AuthFilter`; query joins the member's album, orders by `(created_at, id)`, and reads no name, key or fingerprint; the target exists. Version checks moved to 0.38.0.
- **Web:**
  - `apps/web/src/api/types.ts`, `albums.ts` and `album-rules.ts` (summary and page validators with status-consistency checks).
  - `src/stores/albums.ts` and `src/views/AlbumDetailView.vue`.
  - `src/views/MemoryDetailView.vue`: the watcher fix only.
- **Web tests:**
  - New `tests/unit/album-proposal-history.spec.ts` (16 tests).
  - `tests/unit/memory-proposal-history.spec.ts`: one new test.
  - `albums.spec.ts` and `media-deletions.spec.ts`: the new function in their mocks.
- **Harness:** `Claude outputs/ah-album-history-verify.ps1`, derived from `ph-memory-history-verify.ps1`.

### 8.3 Automated verification (Claude Code, fakes and disposable instances only)

**Backend:** run `usapp-ah-6250d956c3`, existing local image `sha256:83afaf61...cbfb1`, internal network, no published port, tmpfs PostgreSQL 17, newly generated one-time credentials. Container IDs and the project label were checked before each database step.

- Full build passed. 43 migrations were applied, and every checksum matches. Eraser violations 0.
- **Targeted:**

  | Suite | Cases | Assertions | Result |
  |---|---|---|---|
  | `USAPP_TEST_ONLY_ALBUM_HISTORY` | 3 | 97 | 0 failed |
  | `USAPP_TEST_ONLY_ALBUM` (existing album and rename suites) | | 724 | 0 failed |
  | Live API run (real `usapp_api` binary on the container's loopback) | | 334 | 0 failed (304 before the new block) |

- **Full CTest 11/11, 89.56 s:**

  | Target | Assertions | Time |
  |---|---|---|
  | unit | 2817 | 11.20 s |
  | db_gated | 4139 | 15.70 s |
  | media_db | 239 | 10.15 s |
  | media_deletion_db | 376 | 5.59 s |
  | memory_deletion_db | 1048 | 7.80 s |
  | push_dispatch | 582 | 19.88 s |
  | oss_stub | 363 | 5.00 s |
  | static_check | 1155 | 0.03 s |
  | journal_cleanup_db | 147 | 10.64 s |
  | journal_services_db | 4168 | 3.38 s |
  | album_history_db | 97 | 0.17 s |

- Afterwards: erasure and keep/live consistency 0; privilege deviations 0. Crash markers 0, PostgreSQL PANIC/FATAL 0, API server ERROR/FATAL lines 0, generated credentials in the retained logs 0.
- Both containers are stopped and retained under `%TEMP%/usapp-ah-6250d956c3`. This was the batch's only backend run; no backend failure occurred.

What the new backend cases cover:

- Both members list, starting from an empty history with `current_version` 1. Another couple, a user in no space, an unknown id and a request id used as an album id are refused. A footprint over proposals, decisions, revisions, texts, items, removals, cover events, batches, bindings, plans and audit rows is unchanged by every read.
- Rejected, withdrawn and approved requests, with `decided_by`:
  - The approval created version 2. Adding two photos, choosing a cover and removing a photo left the version at 2 and every entry identical.
  - The creator then renamed to version 3. The list keeps 2 as a fact, with `current_version` 3, and shows the new pending request.
  - Each decided request's detail matches its status and has the original and proposed names.
  - Replays answer `Replayed` and a conflicting decision `ALBUM_PROPOSAL_DECIDED`, with the footprint and the list unchanged.
- Five requests given one identical `created_at` (a maintenance fixture with triggers off) walk in id order at limits 1, 2, 3, 5 and 100. A walk in progress, interrupted by a decision and a newer request, finishes with exactly the original set.
- Through the real API binary:
  - 401 without a session; 200 with `no-store` for both members, nine members per entry, `approved_version` 3, `current_version` 3, and neither name in the body.
  - `limit=0`, `limit=101` and a malformed cursor answer 400.
  - Another couple, an absent id and a malformed id answer `ALBUM_NOT_FOUND`.

**Web (host, fake API):**

| Check | Result |
|---|---|
| Focused: `album-proposal-history.spec.ts` 16, `albums.spec.ts` 55, `media-deletions.spec.ts` 47 | 118 passed, exit 0, before the fixture correction below |
| Full web suite (final) | 39 files, 827 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` and `tsc --noEmit -p tsconfig.node.json` | Exit 0 |
| `vite build --outDir %TEMP%/usapp-ah-web-202dde5d6d` | Exit 0, 47 files. The hash over all 45 files of `apps/web/dist` was identical before and after (same value as Section 25 of the plan) |

**Web failures in this batch, all retained in `Claude outputs/ah-web-*.log`:**

1. The cover case first asserted nothing useful. Its fake response used `action: 'cover'` without an `event_no`, which the store rightly keeps unconfirmed. Once the test asserted the cover was confirmed, it failed (`unknown`). The fixture was corrected to the contract's `set_cover` with `event_no`.
2. The corrected test then failed with three history reads instead of two. That was the watcher defect in 8.1, fixed in both views.

After that, the full suite passed.

### 8.4 Not covered, and release gates

- **Not exercised:** a real browser or device, a production service-worker build, the real object store, a decided request listed over HTTP from another couple's perspective beyond the 404 check, and performance with long histories.
- **Contract:** the list carries no names by design, so a comparison costs one detail read per opened request.
- **Unchanged:** notifications and D20 are not implemented or decided. No timestamp-guard, deletion, journal, purge, ownership or authorization-policy change. Gaps 2-6 of the albums design Section 13.3 remain.
- **Open as before:** the older-build rollback rehearsal (plan Section 18.1), device acceptance, D6, and the release gates of the plan and the deletion journal.
