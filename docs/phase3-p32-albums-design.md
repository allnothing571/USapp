# P3-2 shared albums: design and first batch (for owner review)

Status (2026-11-04, revision 2): **P3-2a is implemented and verified with a fake object store in disposable isolated instances only: Section 12. NOT DEPLOYED.** The owner approved D12, D9, D17, D8 and D19 as written in Section 9. P3-2b is [verified the same way](phase3-p32b-album-rename-design.md). P3-2c, the web, is verified with fakes only: Section 13. P3-2d completed a scoped live rehearsal (Section 14). Sections 1-11 are the design as approved; where the build differs, Section 12.3 says so. Background: [phase3-plan.md](phase3-plan.md) Sections 2, 4, 12, 14, 18, 22-24.

**Labels.** [V1] stated in the original requirements, with V1's section number. [Fact] read from this repository. [Rec] proposed default. [Decision] needs the owner. [Unverified] depends on behaviour nothing has executed (Section 10).

## 1. Summary

- **Model:** an album holds **independently uploaded photos only**. Each photo's one home is its album, recorded by `media_bindings` with a new target kind `'album'`. An album never references a memory photo, and nothing is ever rebound.
- **Removal hides, it does not delete:** after a removal commits, the partner cannot obtain a new signed URL. A URL already issued may remain usable until it expires. Bytes stay, bound and out of cleanup, until D6 defines erasure.
- **One correction to the plan:** "reuses media unchanged" was not safe. `visibleTo` lets the partner read anything bound, for good, so without a change a removed album photo would stay readable by its ID. The first batch changes that rule for album photos only.
- **Names free when checked on 2026-11-04:** migration `0031_albums`, lock `33021`, SQLSTATE class `UH`, error codes `ALBUM_*`, contract `0.26.0`. Recheck immediately before writing the migration.

## 2. What the requirements say

**Source check:** the original `app开发方案V1.docx` was opened and compared with `_req_extract.txt`. §18, §27, §29, §31, §34, §42 and §47 are in the original. It has no tables or comments, and the only differences are multi-line paragraphs that the extract splits. **Nothing below rests on the extract alone.**

| Topic | [V1] says | Status of everything else |
|---|---|---|
| Separation | §27: albums are separate from photo memories. They stress "照片分类存储" (photos stored by category) | |
| Examples | §27: daily life, travel, food, memes, embarrassing moments | Examples only. [Rec] no seeded albums |
| Creation and name | §27: create an album; set a name | Who may rename: §29 applies if a name counts as body content. [Decision] D8, D18 |
| Cover | §27: set a cover | Who may set it, and what it may be: [Decision] D8. §31's only examples are plan status changes, so applying it to a cover is by analogy |
| Photos | §27: upload multiple photos; **both members may upload** | V1 mentions uploading only. Selecting existing memory photos is **not** mentioned |
| Visibility | §27: both members view together. §39: everything bound to `couple_id`, checked in the API, never only in the client; private storage; authenticated or short-lived signed access | Exact read rules: Section 5 |
| Edits | §29-30: the creator edits directly; the other member cannot overwrite body content and sends an edit request. §31: changes that do not damage the body may be direct | Whether album fields are body content is not stated |
| Notifications | §18, §18.1: a new photo is user-optional ("🔔 通知对方") | Left for review, as for memories: D20 |
| Data model | §42 suggests `Album` and `AlbumPhoto`, leaving table design open | |
| Navigation | §47: "我们" can hold albums. Marked as a recommendation | [Rec] a link from "我们" |
| Achievements | §34 mentions "上传第 100 张照片" | Out of this batch. Whichever batch adds it must define whether album photos count |

**V1 says nothing about:** ordering, reordering, removing a photo, deleting an album, moving a photo between albums, limits, or empty albums. A search of the original finds no 删除, 排序 or 移除. Everything in this design on those points is a proposal.

## 3. Ownership model

### 3.1 The options

| | A. Uploads only ([Rec]) | B. References to memory photos only | C. Both |
|---|---|---|---|
| Meets §27 "上传多张照片" | Yes | **No** | Yes |
| §27 "分开" (separate) | Fully | Partly | Partly |
| Keeps "one photo, one home" | Yes | Yes, needs a second kind of reference | Yes, two reference kinds |
| Access risk | One rule to change, which removal needs anyway | Must track memory state at read time | Both |
| D6 cascade | One home | Fan-out across domains | Fan-out across domains |

**Chosen: A.** B fails an explicit requirement. C adds a capability V1 does not ask for, with a read-time rule, more tests and a harder D6. A does not foreclose C: Section 3.4 shows it can be added later without touching anything built now.

**The cost:** putting a memory's photo in an album means uploading it again, which stores it twice. A partner's original cannot be re-uploaded, because only its uploader can download it (O8). This is D12.

### 3.2 Lifecycle of an album photo

```
upload (existing flow) -> READY, unbound -> add to album -> bound to the album, item active
                                                                      |
                                                     uploader removes | hidden from the album
                                                                      v
                                                     item removed, still bound, bytes kept until D6
```

### 3.3 What happens when

| Event | Album | `media_bindings` | Partner's read | Uploader's read | Cleanup |
|---|---|---|---|---|---|
| Added | Item appears | Row added, `kind='album'`, in the same transaction | Variants only (O8) | All variants | Refused: bound |
| Removed from the album | Item hidden; cover falls back | **Unchanged.** Never released or rebound | **No new signed URL after commit** (Section 5.2) | Still readable, so nothing is lost by accident | Refused: bound. **Bytes stay until D6** |
| Same photo added anywhere after removal | Refused | Unchanged | | | |
| Removed from a memory's current revision | **Not applicable:** an album-home photo is never in a memory, and a memory photo cannot enter an album | | | | |
| Reserved by a pending memory proposal | Cannot be added until released | None yet | | | Refused: reserved |
| Uploaded but never added | Nothing | None | 404 | Readable | Existing rule: unattached after 7 days |
| **D6 (once decided)** | Item hidden, cover falls back, for all readers | Stays as evidence | 404 | 404 from the deletion commit, unless the deleter restores | D6's purge path must admit album photos, as it must memory photos. It replaces `usapp_media_shared_now` and the plan guard in its own additive migration |

**Why not release the binding on removal, so cleanup can reclaim the bytes?** Bindings are append-only and "bound means never cleaned" is the single rule cleanup relies on. A release would make removing a photo quietly irreversible 7 days later, on irreplaceable photos, before any deletion policy exists. The cost is storage held until D6 (D17).

### 3.4 Memory references, if D12 is ever changed

Specified so the decision can be judged, **not built.** A separate additive table, `album_memory_refs`, with no `media_bindings` row.

| Rule | Behaviour |
|---|---|
| Eligible | A photo in the **current** revision of a memory of the same couple. Never a historical revision, so an album cannot bring back a photo an edit dropped |
| Access | Decided by the memory at read time: shown only while the photo is in the current revision. Nothing is extended, and no binding changes |
| Removed from the album | The reference ends. The memory is unaffected |
| Removed from the current memory revision | Hidden from the album automatically. The memory's history keeps it |
| D6 | The tombstone hides it everywhere |
| Locks | 33021, then 33020 for a read of the current revision. Memory paths never take 33021, so this adds no cycle |

## 4. First-batch rules that V1 leaves open

All [Rec], each pending under Section 9.

| Rule | Proposal | Why |
|---|---|---|
| Who adds | Each member adds only photos they uploaded | §27 "both may upload"; nothing lets one member publish another's private upload |
| Who removes | The photo's uploader only (D9) | §29: not the other's to overwrite; 4.8's uploader principle |
| Name | Set at creation, 1-40 code points, task-title rules. **Creator rename and the partner's edit request are both P3-2b** | §29 is built once, as a whole |
| Cover | Either member, directly, from the album's active photos (D8) | §31 by analogy: reversible, and shows only variants both may already see |
| No cover chosen, or the chosen photo removed | Highest `seq` among active photos: the first photo chosen in the latest add. None if empty. Removal writes no cover event | Never points at a hidden photo |
| Photos per add | 1-20, all or nothing | "Upload many", with one transaction and one replay |
| Empty album | Allowed | Create and add are separate writes |
| Reorder, move, album delete | Not offered | V1 silent. Delete waits for D6 |
| Limits on albums and photos per album | None added | Grants and couple storage already bound cost (Section 4.3 of the plan) |

## 5. Authorization

**Principle:** the couple always comes from the caller's member row, never from the request. A foreign ID is indistinguishable from an absent one.

| Request | Guessed or another couple's ID | Wrong member, same couple |
|---|---|---|
| Album read or write | `ALBUM_NOT_FOUND` 404, with no store call and no role check first | |
| Add a photo | `ALBUM_PHOTO_UNAVAILABLE` 409 for the whole request, the same answer for absent, foreign, not READY, planned, reserved, bound anywhere, or the other member's | Same answer |
| Remove a photo | `ALBUM_PHOTO_NOT_FOUND` 404 | Active: `ALBUM_PHOTO_FORBIDDEN` 403. Already removed: 404 (Section 7.4) |
| Set cover | `ALBUM_PHOTO_NOT_FOUND` 404, also for a removed photo | Either member may |
| `GET /media/{id}` and `/content` | `MEDIA_NOT_FOUND` 404, no URL signed | Partner: variants only while the item is active; `original` always 404 (O8) |

**Historical memory revisions:** closed by construction. A photo bound to a memory, whether current or historical, already has a `media_bindings` row, so it can never enter an album. Album photos cannot enter a memory either ([Fact] [0030:429](../database/migrations/0030_memories.sql) refuses a different `target_id`; [MemoryService.cc:126](../services/api/src/services/MemoryService.cc) requires `target_kind = 'memory'`; [0030:366](../database/migrations/0030_memories.sql) requires it for a revision photo).

### 5.1 The read rule

**Exact predicate:**

```sql
CREATE FUNCTION usapp_media_shared_now(p_media uuid) RETURNS boolean
LANGUAGE sql STABLE AS $shared_now$
    SELECT EXISTS (SELECT 1 FROM media_bindings b
                    WHERE b.media_id = p_media AND b.target_kind = 'memory')
        OR EXISTS (SELECT 1 FROM album_items i
                     JOIN media_bindings b ON b.media_id = i.media_id
                                          AND b.target_kind = 'album'
                                          AND b.target_id = i.album_id
                    WHERE i.media_id = p_media
                      AND NOT EXISTS (SELECT 1 FROM album_item_removals r
                                       WHERE r.album_id = i.album_id AND r.media_id = i.media_id))
$shared_now$;
```

**Service change, and nothing else in the read path:** [MediaService.cc:51](../services/api/src/services/MediaService.cc) selects `usapp_media_shared_now(o.id) AS shared` instead of `bound`, and [:202](../services/api/src/services/MediaService.cc) reads `shared`. The `reserved` column stays as it is ([:52](../services/api/src/services/MediaService.cc)). `bound` is read only at line 202, so nothing else depends on it. The couple join ([:54](../services/api/src/services/MediaService.cc)) and the checks for READY, removing and O8 ([:942-952](../services/api/src/services/MediaService.cc)) do not change.

`kFactsSql` serves four callers ([:300, :559, :914, :939](../services/api/src/services/MediaService.cc)), so a missing grant on the album tables would break upload grants and completion too. Section 8.2 tests this.

| State, for the partner | Today | With `shared_now` | Same? |
|---|---|---|---|
| Memory-bound, current revision | Variants | Variants | Yes |
| Memory-bound, only in history | Variants | Variants | Yes |
| Reserved by a pending proposal | Variants, through `reserved` | Same | Yes |
| Released by rejection or withdrawal | 404 | 404 | Yes |
| Unbound | 404 | 404 | Yes |
| Being removed by cleanup | 404, through `removing` | Same | Yes |
| Album item, active | | Variants | New |
| Album item, removed | | **404** | New |

The uploader short-circuits at [:201](../services/api/src/services/MediaService.cc), unchanged.

### 5.2 Two different things after a removal

| | Rule | Basis |
|---|---|---|
| **Authorization, after commit** | Removal stops the partner from obtaining a new signed URL. Any `content` request whose read starts after the commit gets 404 and signs nothing | [Fact] Each request reads `kFactsSql` afresh; the redirect is `no-store` ([MediaController.cc:94](../services/api/src/controllers/MediaController.cc)). The read is not locked, so a request whose read ran just before the commit may sign just after it. That URL belongs to the next row |
| **URLs already issued** | May remain usable until they expire. Nothing can revoke them | [Fact] Validity ends at `floored + 600`, so a URL has 5-10 minutes left when issued ([MediaService.cc:953-957](../services/api/src/services/MediaService.cc), [OssObjectStore.cc:555-562](../services/api/src/services/OssObjectStore.cc)). [Unverified] real OSS enforcement |
| **Bytes already fetched** | May stay in a browser's cache, and can be saved. Not recallable | [Fact] Reads carry `private, max-age=600` ([OssObjectStore.cc:564](../services/api/src/services/OssObjectStore.cc)), counted from the fetch, so a copy can outlast its URL. [Unverified] actual browser behaviour |

The UI and privacy statement must not call a removal instantly effective everywhere (Section 4.8 of the plan).

### 5.3 Which layer enforces what

| Rule | Service | Database |
|---|---|---|
| Couple derived from the session; 404 before anything else | **Yes** | |
| IDs normalized before use and locking | **Yes** (Section 7.2) | Guards lock from typed `uuid`, so always canonically |
| Friendly refusals and error codes | **Yes** | |
| Album, item, text, cover and revision in one couple | Join | **Composite FKs** on `(id, couple_id)` |
| Photo belongs to the couple | Join | **FK** `(media_id, couple_id)` to `media_objects` |
| Member belongs to the couple | Join | **FK** to `couple_members` |
| One home | Check | **`media_bindings` PK** |
| READY, no plan, no reservation elsewhere | Check after locks | **Binding guard** (Section 6.4) |
| Adder and remover are the uploader | Check | **Guards** |
| Binding target is an album of this couple | | **Binding guard** |
| Item has a matching album binding | | **Item guard** |
| Every album binding has a matching item at commit | | **Deferred trigger** (Section 6.5) |
| Cover names an active item | Check | **Cover guard**, under 33021 |
| Append-only | | **Triggers and `SELECT, INSERT` grants** |
| Partner may read while active | **`visibleTo`** | Predicate lives in `usapp_media_shared_now` |

## 6. Data and API

### 6.1 Names

Read from the workspace on 2026-11-04. **Recheck immediately before creating the file.**

| Item | Proposed | Evidence |
|---|---|---|
| Migration | `0031_albums` | Highest on disk is `0030_memories` |
| Advisory lock | `33021`, per album | In use: 33004, 33007, 33008, 33010-33020 |
| SQLSTATE | `UH001` append-only; `UH002` revision; `UH003` batch and item; `UH004` removal; `UH005` cover; `UH006` deferred links | In use: UB, UC, UD, UF, UG, UM, UN, UP, UQ, UR, UT, UV, UW, UX, UY, UZ. `UH` appears nowhere in `database/`, `services/` or `docs/` at the time checked |
| Error codes | `ALBUM_*` | No album reference in `database/`, `services/` (first-party), `contracts/` or `apps/web/src` |
| Contract | `0.26.0` | Currently `0.25.1` |
| Grants | `init-roles.sql` section 25 | Last section is 24 |

### 6.2 `0031_albums`

All append-only; `usapp_api` gets `SELECT, INSERT`; `usapp_push` nothing. 0000-0030 are not edited. Changing function bodies and a CHECK here follows 0030's precedent.

```
albums                id, couple_id, creator_id, created_at; UNIQUE (id, couple_id)
album_texts           id, couple_id, name. Apart from revisions so D6 can erase text, as memory_texts
album_revisions       album_id, version, couple_id, actor_id, action (create, edit), text_id, key, fingerprint, created_at
                      PK (album_id, version); UNIQUE (actor_id, key)
album_photo_batches   id, album_id, couple_id, added_by, key, fingerprint, created_at
                      UNIQUE (added_by, key). Holds the add's identity once
album_items           album_id, seq, couple_id, media_id, batch_id, added_by, created_at
                      PK (album_id, seq); UNIQUE (album_id, media_id); UNIQUE (media_id)
                      FK (media_id, couple_id) -> media_objects
album_item_removals   album_id, media_id, couple_id, removed_by, key, fingerprint, removed_at
                      PK (album_id, media_id); UNIQUE (removed_by, key); FK to album_items
album_cover_events    album_id, event_no, couple_id, media_id, actor_id, key, fingerprint, created_at
                      PK (album_id, event_no); UNIQUE (actor_id, key); FK (album_id, media_id) -> album_items
```

`album_revisions` ships whole because the migration freezes: P3-2a writes only `create`. Its `edit` rule is exercised by the SQL ledger until P3-2b adds the service path, and an approved request's action is added by P3-2b's own migration.

| Change to existing objects | Detail |
|---|---|
| `media_bindings_kind_valid` | Dropped and re-added as `IN ('memory', 'album')` |
| `usapp_media_binding_guard` | `CREATE OR REPLACE`: Section 6.4 |
| New `usapp_media_shared_now(media)` | Section 5.1 |
| New deferred trigger on `media_bindings` | Section 6.5 |
| **Unchanged, by reading them** | `usapp_media_plan_guard` (a bound object is refused), `usapp_media_released_at` (NULL while bound), `usapp_media_key_disposition`, memory guards, `MediaCleanupJob` |

### 6.3 Guards other than the binding guard

**Lock rule for every album guard: 33021 before 33018.** A guard must never take 33021 after 33018.

| Table | Order inside the guard |
|---|---|
| `album_items` | **33021 on the album; then 33018 on the media;** batch inserted in this transaction (the `xmin` rule of 0030, which fails closed inside a savepoint); binding exists with this album and kind `album`; `added_by` is the uploader and equals `bound_by`; `seq` is exactly max + 1 |
| `album_photo_batches` | 33021; `added_by` is a member of the album's couple |
| `album_item_removals` | 33021; item exists; `removed_by` is the uploader |
| `album_cover_events` | 33021; item active; `event_no` contiguous |
| `album_revisions` | 33021; `actor_id` is the creator; contiguous version |

Deferred: every album has revision 1.

### 6.4 Binding guard: exact check order

Steps 1-5 are 0030's body, in the same order ([0030:272-284](../database/migrations/0030_memories.sql)). Only the new branches differ.

| Step | Check | Memory kind | Album kind | Refusal |
|---|---|---|---|---|
| 1 | Any of `media_id`, `created_at`, `target_kind`, `target_id` NULL | Return, so the NOT NULL constraint reports it | Same | |
| 2a | Album lock | **Not taken** | 33021 on `target_id` | |
| 2b | Media lock | 33018 | 33018 | |
| 3 | READY | Same | Same | `USAPP_MEDIA_BINDING_NOT_READY` |
| 4 | No `everything` plan | Same | Same | `USAPP_MEDIA_BINDING_CLEANUP` |
| 5 | `created_at` not in the future | Same | Same | `USAPP_MEDIA_BINDING_TIME` |
| 6 | Target | `memories` row of this couple | `albums` row of this couple. Any other kind refuses | `USAPP_MEDIA_BINDING_TARGET` |
| 7 | Actor | Uploader, or the approver of a proposal naming this photo on this memory | **Uploader only.** Proposals are not consulted | `USAPP_MEDIA_BINDING_ACTOR` |
| 8 | Reservation | Refused if reserved by a proposal on another memory | **Refused if reserved at all** | `USAPP_MEDIA_BINDING_RESERVED` |

All refusals stay `UF007`. Step 1's two new NULL conditions change no memory outcome, because both columns are NOT NULL.

### 6.5 Deferred trigger: exact definition and when it fires

```sql
CREATE FUNCTION usapp_album_binding_item_check() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $check$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM album_items i
                    WHERE i.album_id = NEW.target_id AND i.media_id = NEW.media_id
                      AND i.couple_id = NEW.couple_id AND i.added_by = NEW.bound_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH006', MESSAGE = 'USAPP_ALBUM_BINDING_ITEM';
    END IF;
    RETURN NULL;
END
$check$;

CREATE CONSTRAINT TRIGGER media_bindings_album_item AFTER INSERT ON media_bindings
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW
    WHEN (NEW.target_kind = 'album')
    EXECUTE FUNCTION usapp_album_binding_item_check();
```

| Condition | Fires? |
|---|---|
| Insert with kind `album` | Queued; runs at commit, or at `SET CONSTRAINTS ... IMMEDIATE` |
| Insert with kind `memory` | **Never queued** |
| Update, delete or truncate | No such event: the table is append-only, and its own triggers refuse them |

A commit refused by this trigger means a bug. It takes the existing unconfirmed-commit path, and rolls back.

### 6.6 How memory behaviour is preserved

| Memory path | Sequence ([Fact]) | Effect of 0031 |
|---|---|---|
| **Create** | Identity [MemoryService.cc:556](../services/api/src/services/MemoryService.cc), text and revision [:500-503](../services/api/src/services/MemoryService.cc), bind [:511](../services/api/src/services/MemoryService.cc) with literal `'memory'` [:167-170](../services/api/src/services/MemoryService.cc), photos [:513](../services/api/src/services/MemoryService.cc) | Guard steps 1-8 give the same result: the memory exists, the actor is the uploader. No 33021. Trigger not queued |
| **Creator edit** | Same, from the bind on | Same |
| **Propose** | Only `memory_proposal_photos`; no binding | Guard and trigger not involved. The photo guard is unchanged |
| **Approve** | Decision [:700](../services/api/src/services/MemoryService.cc), revision [:679](../services/api/src/services/MemoryService.cc), bind [:687](../services/api/src/services/MemoryService.cc), photos [:689](../services/api/src/services/MemoryService.cc) | The decision is first, so step 7's approver rule passes and step 8 finds no pending reference. Same as today. Trigger not queued |
| **Partner reads** | `kFactsSql` | Section 5.1 table |
| **Cleanup** | Plan guard reads any binding | Unchanged for memory photos, and album photos are refused the same way |

An album-home photo reaching a memory path is refused by [MemoryService.cc:126](../services/api/src/services/MemoryService.cc) first, then by the revision-photo guard [0030:366](../database/migrations/0030_memories.sql) or the proposal-photo guard [0030:429](../database/migrations/0030_memories.sql).

### 6.7 API, contract 0.26.0

| Method, path | Purpose | Caller | Success |
|---|---|---|---|
| `POST /api/v1/albums` | Create, `{name}` | Member | 201; 200 replay |
| `GET /api/v1/albums?limit&cursor` | List, each with name, cover, active count; plus `uploads_enabled` | Member | 200 |
| `GET /api/v1/albums/{id}` | Current state | Member | 200 |
| `GET /api/v1/albums/{id}/photos?limit&cursor` | Active photos | Member | 200 |
| `POST /api/v1/albums/{id}/photos` | Add, `{media_ids}` | Uploader of each | 201; 200 replay |
| `POST /api/v1/albums/{id}/photos/{mediaId}/removal` | Remove | Uploader | 200; 200 replay |
| `POST /api/v1/albums/{id}/cover` | Set cover, `{media_id}` | Member | 201; 200 replay |

Every write needs Origin, CSRF and `Idempotency-Key`; responses are `no-store` and never contain object keys. New codes: `ALBUM_NOT_FOUND`, `ALBUM_PHOTO_UNAVAILABLE`, `ALBUM_PHOTO_NOT_FOUND`, `ALBUM_PHOTO_FORBIDDEN`, `ALBUM_PHOTO_REMOVED`. `getMedia`'s description changes to state the active-item rule.

**P3-2b adds:** `POST /api/v1/albums/{id}/revisions` (creator rename), partner requests and decisions, `ALBUM_EDIT_FORBIDDEN`, `ALBUM_VERSION_CONFLICT`, and the proposal codes.

## 7. Transactions, replay, order and locks

### 7.1 Lock order

```
member FOR SHARE -> 33021 album -> key classification -> role and state -> sorted 33018 media -> inserts -> audit -> commit
```

| Path | Holds | Cycle risk |
|---|---|---|
| Add photos | 33021, then sorted 33018 | None: nothing holding 33018 waits for 33021 |
| Remove, cover | 33021 only | None. Removal changes visibility, not bytes |
| Create album | No album lock, because the ID is new; key UNIQUE decides | None |
| Memory writes | 33020, then sorted 33018 | Never take 33021, and albums never take 33020 |
| Media completion, cleanup job | 33018 only ([Fact] [MediaService.cc:30, :109](../services/api/src/services/MediaService.cc)) | Never take a higher lock |

Sorted means the existing statement: `DISTINCT hashtext(x) ... ORDER BY 1`, identical to the cleanup job and memories.

### 7.2 ID normalization, before anything else

`hashtext` hashes text, so `ABC...` and `abc...` are different lock keys ([MediaService.cc:109](../services/api/src/services/MediaService.cc)). Sorting and mutual exclusion both depend on one spelling.

| Input | Rule, before fingerprint, classification or lock |
|---|---|
| `media_ids` | Strings only; lowercase; `isCanonicalUuid`; duplicates refused **after** lowering; request order kept. As [MemoryService.cc:329-330](../services/api/src/services/MemoryService.cc) |
| `mediaId` in a path, `media_id` in a cover body | Same |
| Album ID | From the row the member join returned, as memories do, not from the request text |

An invalid spelling is `VALIDATION_ERROR` for a body field, and the same 404 as absent for a path segment. Guards lock from `uuid::text`, which is always canonical, so a service mistake here fails closed rather than breaking exclusion.

### 7.3 Each write is one transaction

| Write | Order inside it |
|---|---|
| Create | Member; classify; identity, text, revision 1; audit; read back |
| **Add** | Member with album; 33021; classify; sorted 33018; eligibility **read after the waits**; batch; bindings; items by ascending `seq` (Section 7.6); audit. Any ineligible photo refuses all. On refusal, re-classify first, so a same-key winner replays |
| Remove | Member with album; 33021; classify; Section 7.4; removal; audit |
| Cover | Member with album; 33021; classify; item active; event; audit |

Network calls: none. Bytes were uploaded and promoted earlier.

### 7.4 Removal and `ALBUM_PHOTO_REMOVED`

Evaluated in this order, under 33021. **Classification comes first, so a replay never sees a state error.**

| Case | Answer | Writes |
|---|---|---|
| Same actor, same key, same album and photo | **200 replay:** the original removal facts and today's album | Nothing |
| Same key, different album or photo | `IDEMPOTENCY_KEY_REUSED` 409 | Nothing |
| Item absent or in another couple | `ALBUM_PHOTO_NOT_FOUND` 404 | Nothing |
| Already removed, caller is not the uploader | `ALBUM_PHOTO_NOT_FOUND` 404: a removed photo is no longer the partner's to know about | Nothing |
| Active, caller is not the uploader | `ALBUM_PHOTO_FORBIDDEN` 403 | Nothing |
| **Already removed, caller is the uploader, new key** | **`ALBUM_PHOTO_REMOVED` 409** | Nothing |
| Active, caller is the uploader | 200 | One removal, one audit row |

Concurrently: the same key gives one removal and one replay. Different keys give one removal and one `ALBUM_PHOTO_REMOVED`. `ALBUM_PHOTO_REMOVED` is used only here. A cover naming a removed photo is 404, and an add naming one is `ALBUM_PHOTO_UNAVAILABLE`.

### 7.5 Idempotency

As anniversaries and memories. Same actor, key and fingerprint: read-only replay of the **original** facts with **today's** state, 200, writing nothing, before any state check. Same key with a different body: `IDEMPOTENCY_KEY_REUSED`. Unconfirmed commit: `RESULT_UNKNOWN`, retry with the same key. Fingerprints use normalized IDs, and include the album ID, and photo order for adds. Later changes do not override: replaying an add after a photo was removed still returns the original facts.

### 7.6 Order and pagination

**`seq` resolved.** An add of N photos allocates the next N values. **The first photo chosen gets the highest.** Items are inserted lowest `seq` first, so the guard's max + 1 holds:

```
request [A, B, C], album's max seq is 7:   C = 8, B = 9, A = 10
seq DESC shows:                            A, B, C, then older photos
```

So newest-first shows newer adds first, and the chosen order inside each add. The fallback cover is the first photo chosen in the latest add.

| List | Order | Cursor | During a walk |
|---|---|---|---|
| Albums | `created_at DESC, id DESC`, immutable | `timestamp_uuid`, strictly decoded, as anniversaries | A new album never repeats or hides a row. A rename, once P3-2b adds it, moves nothing |
| Photos | `seq DESC`, total | Strictly decoded integer | A new photo has a higher `seq`, so it never repeats or hides a row. A removed photo simply stops appearing |

Limits use the existing `TaskService::parseLimit` (default 50, maximum 100). [Rec] the web asks for 30 photos at a time and lazy-loads, to stay within Shanghai's documented 50 image QPS. Burst behaviour is [Unverified].

## 8. First batch

### 8.1 Stages

| Stage | Scope | Needs |
|---|---|---|
| **P3-2a** (this approval) | Backend: 0031, `AlbumService`, `AlbumController`, `kFactsSql` change, contract 0.26.0, policy and grant wiring, tests, isolated harness. Create, list, get; add, list, remove; cover | Fake store only |
| **P3-2b** | **Creator rename and partner edit requests together**, with approval, rejection and withdrawal, in an additive migration. **Implemented, fake store only, NOT DEPLOYED:** [design and evidence](phase3-p32b-album-rename-design.md) | D18 approved |
| P3-2c | Web: routes under `/us/albums`, list, detail, add through the existing upload queue with scope `album:<id>`, remove, cover, retries as in the memories store. **Implemented, fakes only, NOT DEPLOYED: Section 13** | Done |
| P3-2d | Live: real OSS test bucket, two desktop accounts; devices if ready | Owner-run setup; separate approval |

**P3-2a excludes:** rename, web, real OSS, notifications, album or photo deletion, reorder, move, memory references, achievements, P3-3. Until P3-2b, a name is what was set at creation. That is acceptable only because everything ships together.

**Harness:** new migration prevalidated with rollback in both roles, then a disposable-instance apply, checksums, zero privilege deviations, repeat InitRoles, API without 0031 refuses to start. The suite goes in `db_gated`, with no sleeps. That target took 20.30 s of 30 s in the last recorded run, so **if it passes about 25 s I report before adding a target.** The limit is not raised.

### 8.2 Backend acceptance

All against the fake store, on a disposable instance.

| Area | Must show |
|---|---|
| **Cross-couple** | Every album route from another couple gives the same 404 as an absent ID, with no store call. Adding another couple's photo refuses, leaving no row. Direct SQL: item, binding, cover and revision across couples are all refused |
| **Permissions** | Adding the partner's photo refuses. Partner removal 403, uploader succeeds. Either member sets cover |
| **Access** | Partner: 404 before add; variants while active; `original` always 404; 404 from `facts` and every variant immediately after removal, with no URL signed. Uploader reads throughout. Another couple never |
| **Memory access unchanged** | Asserted one by one for a current photo, a photo only in history, reserved, released and unbound |
| **Grants** | Upload grant and completion work with 0031. Without it the API refuses to start |
| **Normalization** | Uppercase `media_ids` and path IDs: same fingerprint as lowercase, a replay rather than a new write, and the same lock keys as the job. Case-only duplicates refused |
| **Rebinding** | A memory-bound photo, current or historical, cannot be added. An album photo cannot enter a memory or proposal, or a second album, even after removal. Binding row byte-identical after removal |
| **Binding guard** | Every existing refusal message in the same order. `memory` kind with an album ID and `album` kind with a memory ID both refuse `TARGET`. Album binding by the non-uploader refuses `ACTOR`, even with an approved proposal naming it. Reserved photo refuses `RESERVED` |
| **Lock order** | Direct SQL proves the album branch takes 33021 before 33018, and the memory branch never takes 33021 |
| **Removal** | Each row of Section 7.4, including same-key replay, `ALBUM_PHOTO_REMOVED` with no new row, and the partner's 404 after removal |
| **Duplicates and replay** | Each write replays with no new row. Same key, different body refused, including reordered `media_ids`. A replayed add after a later removal returns its original facts. Same key at once into two albums: one write, then replay or `KEY_REUSED` |
| **Concurrency** | Same key at once: one write, one replay. Same photo into one album, and into two albums, with different keys: one wins, the other refuses, no deadlock. Overlapping photo sets in opposite orders, repeated. Remove against cover: cover never names a removed item |
| **Rollback** | Injected failure after the first binding of an add: no batch, item, binding or audit row; the same key then succeeds. One ineligible photo in a batch of many: nothing added |
| **Ordering and paging** | `[A, B, C]` lists as A, B, C; a later add lists before it. Albums and photos walk with no repeat or skip, across an insert and a removal mid-walk. Exact-page boundary gives no cursor. Bad cursors 400 |
| **Cover** | Chosen cover used; removal falls back to the first photo chosen in the latest add, with no event; empty album has none; a removed or foreign photo refused |
| **Schema** | SQL ledger in both roles for every guard, the `album_revisions` `edit` rule, append-only on all seven tables, deferred checks. **Existing media and memory ledgers pass unchanged**, or any change is reported first |
| **Regression** | Full CTest on all targets, SQL ledgers both roles, strict YAML, API with 0031 through the real filter for routes and refusals, static checks: no album path calls the store for bytes, no `33020` in album code, no album code in the push worker |

### 8.3 Deterministic cleanup races, through the real job

Each uses the real `Scheduler::tickOnce`, `MediaCleanupJob`, `MediaService::runTick` and `AlbumService`, with `FakeObjectStore`. No sleeps; bounded waits fail the test.

**Fixture for both:** two aged READY photos by one uploader, unbound and eligible for planning, named LOW and HIGH by `hashtext`. `planLimit` raised, so leftovers from earlier cases cannot crowd them out. A **holder on its own connection** holds 33018 on HIGH, because the test pool has only two connections. Waits count blocked `SELECT pg_advisory_xact_lock(33018%` statements in `pg_stat_activity`. The add and job statements have the same text, so **the count, not the text, tells them apart.**

| | **Add first** | **Plan first** |
|---|---|---|
| 1 | Start add of `[LOW, HIGH]`. It takes LOW, blocks on HIGH: count 1 | Start `tickOnce`. It reads both as candidates, takes LOW, blocks on HIGH: count 1 |
| 2 | Start `tickOnce`. It reads both as candidates, because the add is uncommitted, then blocks on LOW: count 2 | Start add of `[LOW]`. It blocks on LOW, held by the job: count 2 |
| 3 | Release the holder | Release the holder |
| 4 | The add takes HIGH and commits | The job takes HIGH, plans both, commits |
| 5 | The job's new statement ([MediaService.cc:1017](../services/api/src/services/MediaService.cc)) sees `usapp_media_released_at` as NULL: **zero rows for these photos, no error** | The add's eligibility read sees the plan: **`ALBUM_PHOTO_UNAVAILABLE`** |
| Assert | Tick reports success. No plan for either photo. Both bound, both items active. Partner gets variants | Plans exist. **No text, batch, binding or item row.** The same key then refuses again or replays consistently. Partner 404 |
| Then | A later tick plans nothing for them. `clean()` holds | The sweep proceeds, and `clean()` holds |

**Plus the direct-SQL pairs** already used for memories, which are a different path: a held add, then a plan insert refused `USAPP_MEDIA_PLAN_BOUND`; a held plan, then a binding refused `USAPP_MEDIA_BINDING_CLEANUP`.

**Also:** a reserved photo cannot be added. A released photo can, by its uploader. A photo removed from an album, aged past the window: the job plans nothing, and direct SQL refuses `BOUND`. An unadded READY photo is still planned after 7 days. `keep` disposition unchanged. The job holds 33018 while a 33021 holder proceeds, and the reverse.

### 8.4 Memory regressions for the deferred trigger

| Case | Must show |
|---|---|
| **Service create** | 1 and 9 photos, through `MemoryService`: commits; binding kind `memory`; existing assertions unchanged |
| **Service creator edit** | Retained and new photos commit |
| **Service approval** | New partner photo and a retained photo: commits; `bound_by` is the approver; replay writes nothing; `FailPhotoBinding` still rolls back everything and the same key then succeeds |
| **Not queued** | Direct SQL, in one transaction: a memory binding and full memory rows, then `SET CONSTRAINTS ALL IMMEDIATE`, raises no `UH006` |
| **Negative control** | Album binding with no item: `SET CONSTRAINTS ALL IMMEDIATE` raises `USAPP_ALBUM_BINDING_ITEM`. Item in a different album, and `bound_by` differing from `added_by`, both refuse |
| **Definition** | Catalog check: deferrable, initially deferred, insert only, with the `album` condition |
| **Suites** | `MemoryDbTests.h`, `MediaDbTests.h`, `OssServiceDbTests.h`, `memory_ledger_test.sql` and `media_ledger_test.sql` pass with no change to their assertions |

### 8.5 Web acceptance (P3-2c, later)

Disabled uploads keep albums readable. Attach only after READY. Interrupted upload differs from unconfirmed completion, and from an unconfirmed attach. Keys and payloads preserved. Late results dropped on account or route change. Missing or removed photos show a placeholder. Each member sees a remove control only on photos they uploaded. `ALBUM_PHOTO_REMOVED` shows as already removed. Cache exclusion unchanged.

### 8.6 Live acceptance (P3-2d, later, owner-run)

A creates an album and adds a synthetic photo; B views a variant, cannot get the original, adds a photo and sets a cover. A removes a photo; B's route gives 404, while a previously issued signed URL's remaining life is **measured and recorded**. Open from P3-1d and not closed by this: signed URL expiry, overwrite races, late objects, and the wider list in Section 24.1 of the plan.

## 9. Owner decisions

D12, D9, D17, D8 and D19 are **approved as written** (owner, 2026-11-04, in the P3-2a assignment). **D18 is approved as Option A** (owner, 2026-11-04): [P3-2b design](phase3-p32b-album-rename-design.md) Section 2. D20 and D6a-d stay open.

| # | Recommended default | What users see | Blocks |
|---|---|---|---|
| **D12** | Album photos are uploads only; a memory photo never enters an album | A photo wanted in both places is uploaded twice. A partner's original can't be re-uploaded | P3-2a |
| **D9** | Only a photo's uploader removes it | An album's creator can't remove the partner's photo from their own album | P3-2a |
| **D17** | Removal hides at once, keeps bytes bound until D6, with no restore in this batch | The partner gets no new access after removal, though an issued URL may work until expiry. The uploader keeps it. Storage isn't freed. It can't return to any album | P3-2a |
| **D8** | Either member sets the cover from active photos; it falls back to the first photo chosen in the latest add. The name is set at creation, and the creator renames | Either member can change the cover without asking | Cover: P3-2a. Rename: P3-2b |
| **D19** | Name 1-40; 1-20 photos per add, all or nothing; no reorder, move or album delete; empty albums allowed | One bad photo refuses a whole add. Order can't be rearranged, and mistaken albums stay | P3-2a |
| D18 | The partner's rename goes through an edit request. Alternative: treat a name as not body content | | P3-2b |
| D20 | Album notifications follow whatever is decided for memories (Section 11 of the plan) | | Release, if wanted |
| D6a-d | Deletion and purge, including album delete | **Real photos stay refused until implemented** | Release |

**Alternatives considered:** D9: the album's creator may remove too, which lets one member hide the other's photo. D17: release a removed photo to cleanup after 7 days, which frees storage but makes a removal quietly irreversible (Section 3.3). D18: creator-only rename with no request.

## 10. Basis of these conclusions

### Static findings: read directly from files, nothing executed

- **Requirements:** the original `app开发方案V1.docx` and `_req_extract.txt`.
- **Plan and status:** `docs/phase3-plan.md`, `docs/phase3-p31d-oss-plan.md`, `docs/claude-code-handoff.md`, `CLAUDE.md`.
- **Schema:** `database/migrations/0029_media.sql`, `0030_memories.sql`, and the directory listing.
- **Backend:** `MediaService.cc`, `MemoryService.cc` (SQL and write order), `OssObjectStore.cc` (signed reads), `MediaController.cc`, `services/api/tests/CMakeLists.txt`, `DbGatedTest.cc`, `MediaDbTests.h` and `MemoryDbTests.h` (test mechanics).
- **Contract and web:** `contracts/openapi.yaml`, `apps/web/src/stores/memories.ts`, `apps/web/src/router/index.ts`. `MemoryEditor.vue` and `memory-uploads.ts` were not reread.
- **Namespaces:** text searches of `database/`, `services/`, `contracts/`, `apps/web/src`.

The objects proposed here do not exist, so every statement about them describes the design, not behaviour.

### Unverified: needs execution

| Area | Assumption | Proved by |
|---|---|---|
| **PostgreSQL** | A row-level BEFORE trigger in a volatile function sees earlier rows of the same statement | P3-2a ledger |
| | Under READ COMMITTED, a statement after a lock wait gets a fresh snapshot, so eligibility and the job's plan see the other side's commit. No isolation override was found | Section 8.3 |
| | `WHEN` stops a deferred constraint trigger from being queued | Section 8.4 |
| | Dropping and re-adding the CHECK validates existing rows safely inside the migration | Prevalidation |
| | Sorted `hashtext` acquisition cannot deadlock against the job | Section 8.2-8.3 |
| **Browser cache** | How long fetched bytes persist under `private, max-age=600`, and whether a page keeps showing them after removal | P3-2c and P3-2d observation |
| **OSS** | Real expiry of a signed URL, enforcement of its signature, and image QPS under a burst. The owner-reported P3-1d flow does not establish these (Section 24.1 of the plan) | P3-2d and P3-1d's open tests |
| **Old builds** | How a previous build behaves on this schema | Rehearsal in Section 18 of the plan |

## 11. Review findings folded in

| # | Finding | Where |
|---|---|---|
| 1 | Job path gives zero rows, not `BOUND`; needs its own test | 8.3 |
| 2 | IDs normalized before locking | 7.2, 8.2 |
| 3 | Item guard: 33021 before 33018 | 6.3, 6.4 |
| 4 | Exact `shared_now`; `reserved` stays | 5.1 |
| 5 | Exact deferred trigger and firing conditions | 6.5, 8.4 |
| 6 | Binding guard check order | 6.4 |
| 7 | Signed URL statement separated into three windows | 5.2 |
| 8 | `seq` contradiction resolved | 7.6 |
| 9 | `ALBUM_PHOTO_REMOVED` defined, with replay | 7.4 |
| 10 | Old-build rollback risk | Plan Section 18 |
| 11 | Original V1 checked | 2, 10 |

## 12. P3-2a implementation and isolated verification (Claude Code, 2026-11-04)

**Real workflow not verified.** No browser, device, real OSS, shared `usapp_test` or production was used, and nothing is deployed. Every result below is from disposable instances with `FakeObjectStore`, on an internal network with no vendor credential.

### 12.1 State, stated exactly

| Item | State |
|---|---|
| Names | Rechecked before use, all free: `0031_albums`, lock `33021`, SQLSTATE class `UH`, `ALBUM_*`, contract `0.26.0` (was `0.25.1`), `init-roles.sql` section 25 |
| `0031_albums` | sha256 `651dcddf086a01bd34078abf9f7072acc732a23874c0f2621cfb3670850c7c16`. **FROZEN** by its first committed application, in disposable project `usapp-p32a-20261104164444-7cc730`. **Applied in disposable instances only:** not on shared `usapp_test`, not on production |
| Build dependency | A build with P3-2a needs 0031 at startup: without it the API refuses to start, naming the album tables. On a database without 0031, `db_gated` fails its migration gate |
| Next migration number | `0032` |
| Flags and config | None added or changed |

**Files.** New: `database/migrations/0031_albums.sql`, `database/tests/album_ledger_test.sql`, `services/api/src/services/AlbumService.{h,cc}`, `services/api/src/controllers/AlbumController.cc`, `services/api/tests/AlbumDbTests.h`, `services/api/tests/AlbumUnitTests.h`, `Claude outputs/p32a-isolated-verify.ps1`. Changed: `MediaService.cc` (`kFactsSql` and `visibleTo` only), `TaskPrivilegePolicy.cc`, `Errors.{h,cc}`, `services/api/CMakeLists.txt`, `database/init-roles.sql`, `database/tests/task_privilege_deviation.sql`, `contracts/openapi.yaml`, and the test wiring in `DbGatedTest.cc`, `UnitTest.cc`, `StaticCheckTest.cc`, `ReminderLiveApiTest.h`, `OssServiceDbTests.h` (Section 12.3). `MemoryService`, `MediaCleanupJob`, the plan guard and every frozen migration are unchanged.

### 12.2 Verification

Run `usapp-p32a-20261104164840-026a18`, exit 0, with `Claude outputs/p32a-isolated-verify.ps1`. Complete logs are saved before filtering under `%TEMP%/usapp-p32a-20261104164840-026a18` on the verification host. Generated credentials found in those logs: 0.

| Check | Result |
|---|---|
| Rollback-only prevalidation, before any commit, 0031 plus three ledgers in one transaction, as `postgres` and as `usapp_api` | Album 76, memory 64, media 53 cases, each role; `ROLLBACK`. Afterwards: 0031 unregistered, no album tables, no new function, rows unchanged |
| API without 0031 | Refused to start, exit 1, naming the album tables |
| First committed apply over representative data (81 deliveries, 129 media, 40 memories, 61 bindings, 464 tasks) | Exactly 0031 applied. Existing rows, notifications and bindings unchanged |
| Repeat apply and InitRoles | 32 skipped, 0 applied |
| Checksums | 32 registered, each equal to its local file |
| Privilege deviations | 0 after apply, 0 after repeat |
| Targeted suites, direct, before the full run | `oss_stub` 363 assertions; album 407 (2 s); memory 260; media 239 (10 s); service-through-adapter 41. No failures |
| Full CTest | **6 of 6 passed, 54.98 s:** unit 11.19 s (2424 assertions), `db_gated` 14.01 s of its 30 s (3781), `media_db` 9.86 s of 30 s, `push_dispatch` 14.92 s (541), `oss_stub` 4.98 s (363), `static_check` 0.01 s (606). No timeout changed; no target added |
| Real API binary, through the real filter, media disabled | 90 assertions passed. Anonymous 401, missing CSRF 403, missing key 400, create 201, replay 200, reused key 409, list with `uploads_enabled:false`, foreign and absent both `ALBUM_NOT_FOUND`, refusals for add, removal and cover, bad cursors 400, no rename route, `no-store` |
| API with `MEDIA_ENABLED=true` and nothing else | Refused to start, exit 1 |
| SQL ledgers in both roles after CTest | Album, memory, media, reminder, notification, notification choice, anniversary and task pass; push dispatch in both modes passes; each `ROLLBACK` |
| Strict YAML | `Claude outputs/p26r1-yaml-check.py`, log `Claude outputs/p32a-logs-yaml.log`: negative control refused; contract parsed, 0.26.0, 69 paths, 81 operations. This checks YAML only, not OpenAPI semantics |
| PostgreSQL server log | `deadlock detected`: 0 occurrences |
| After CTest | 57 albums, 146 items, 60 removals, 18 cover events. Album bindings with no item and items with no matching binding: 0 and 0. Plans on any bound photo: 0. Push control: disabled |

**What the album suite shows**, through the real services and fake store:

| Area | Shown |
|---|---|
| Both cleanup orders, through the real `Scheduler::tickOnce` and `MediaCleanupJob` | **Add first:** the job, blocked behind the add, completes with no error, plans nothing for either photo; both stay bound and readable. **Plan first:** the add, blocked behind the job, refuses `ALBUM_PHOTO_UNAVAILABLE` with no batch, binding, item, text or audit row; the same key refuses again; the sweep proceeds. Ordering is forced by a holder lock and the blocked-statement count, with bounded waits and no timed sleep deciding the order |
| Direct-SQL pairs | A plan behind a held add refuses `USAPP_MEDIA_PLAN_BOUND`; a binding behind a held plan refuses `USAPP_MEDIA_BINDING_CLEANUP` |
| Lock order | The album branch holds no media lock while waiting on 33021; memory writes do not wait on 33021; completion and the job finish while 33021 is held; removal and cover finish while 33018 is held |
| Memory regressions | Service create with 1 and 9 photos, creator edit, proposal approval with forced failure, retry and replay. `SET CONSTRAINTS ALL IMMEDIATE` after a direct memory binding raises nothing. Album binding with no item refuses at that check and at a real commit; item in another album and differing `added_by` refuse; the catalog shows the trigger's definition |
| Partner access | Variants while active; `original` always 404; from the removal's commit, 404 for facts and each variant with nothing signed; uploader keeps all; binding row byte-identical; key disposition `keep`. Against the fake only, which models no expiry, a URL issued earlier still resolves |
| Removal table | Every row of Section 7.4, including same-key replay of an already removed photo, `ALBUM_PHOTO_REMOVED` with no new row, and both concurrent cases |
| Duplicates and concurrency | Replays write nothing; reordered ids, another album or another photo are `KEY_REUSED`; same key at once; one key into two albums at once; one photo into one or two albums; overlapping sets in opposite orders, repeated, with no deadlock failure; cover against removal never leaves a removed cover |
| Rollback | Injected failure after binding leaves no batch, item, binding or audit row, and the same key succeeds. One bad photo in either position adds nothing |
| Order and cover | `[A, B, C]` lists A, B, C with A at the highest seq; a later add lists first; walks across inserts and removals neither repeat nor skip; fallback, chosen and empty-album rules |
| Isolation | Foreign and absent ids answer alike with no store call; normalization: uppercase ids give the same fingerprint, replay and lock |

Assumptions in Section 10 now executed: BEFORE-trigger visibility of earlier rows in one statement; a fresh snapshot after a lock wait, under READ COMMITTED, for the add and the job; `WHEN` keeping memory bindings off the deferred check; replacing the CHECK over existing rows.

### 12.3 Deviations from the design, and one out-of-scope repair

| Item | Detail |
|---|---|
| **Stale OSS assertion, not caused by P3-2a** | The first full run, `usapp-p32a-20261104164444-7cc730`, stopped at the service-through-adapter suite: `OssServiceDbTests.h:96` still expected `image/jpeg` for an original. `OssObjectStore.cc` changed after the last full run (2026-11-03 23:36, against that run at 19:30) so stored objects keep `application/octet-stream`, because OSS rejects `response-content-type`, and `OssStubTest.cc:620` already asserts the neutral type. Line 96 now matches. **Review decision, below** |
| Added deferred check | `album_photo_batches_items` and `usapp_album_batch_valid` enforce D19's 1-20 per add at commit (`USAPP_ALBUM_BATCH_ITEMS`). The design listed only the service check. Additive and stricter |
| Added guard refusals | Item guard's `USAPP_ALBUM_ITEM_ACTOR` is separate from the binding check. Same rule as the design |
| Response shape | The design named fields only. `cover.chosen`, `photo_count`, and the mutation fields are defined in contract 0.26.0 |
| One fresh rerun | Run `...-026a18` rebuilt the image after the one-line repair. 0031's bytes did not change |

**Review decision (2026-11-04): the `application/octet-stream` assertion at `OssServiceDbTests.h:96` is consistent with the existing original-object behaviour, and stands.**

| Basis | What it shows |
|---|---|
| [OssObjectStore.cc:651-654](../services/api/src/services/OssObjectStore.cc) | `copy` stores every final object as `application/octet-stream`, and states that originals keep the neutral stored type |
| [OssObjectStore.cc:566-573](../services/api/src/services/OssObjectStore.cc) | A read sends no `response-content-type`. An original is served as an attachment. Only a processed variant gets its type from the image processor |
| [OssStubTest.cc:601-607, 618-625](../services/api/tests/OssStubTest.cc) | The adapter's own suite asserts the neutral stored type, `application/octet-stream` on an original read, and no echo of an unexpected format into a type a browser would render |

**Scope of this decision:** it concerns consistency between a test and the adapter against the local TLS model. **The real OSS header check stays unverified for this run:** run `usapp-p32a-20261104164840-026a18` had no vendor access, and no real response header was observed. That check remains with item I9 in [phase3-p31d-oss-plan.md](phase3-p31d-oss-plan.md). I9's text still expects `response-content-type` with `x-oss-process` to work, which the adapter's comment contradicts; it was not edited here and needs correcting when P3-1d resumes.

### 12.4 Limits and open items

- **Not proved here:** real signed-URL expiry, browser cache after removal, and every open item in Section 24.1 of the plan. Real album add and removal have not run through the HTTP layer with a store, because the real binary has media disabled; the service paths ran directly with the fake.
- **Old builds:** the rollback risk in Section 18.1 of the plan stands. Its rehearsal has not run.
- **Test gaps:** an item across couples is refused by the binding guard and the item's foreign keys, but no case states it alone. The `Fault` hook covers failure after binding only. Removal racing a concurrent partner read is not forced: a read that ran just before the commit may sign just after it, as Section 5.2 records.
- **Not in this batch:** rename and edit requests, web, notifications, deletion (D6), reorder, move, P3-3.
- **Unrelated, still open:** the earlier `db_gated` abort stays unexplained. It did not recur in these runs, which says nothing about its cause.

## 13. P3-2c web implementation and verification with fakes (Claude Code, 2026-11-04)

Web only. Backend, contract, migrations, dependencies and deployment are unchanged: `0031` is still `651dcddf...7c16` and `0032` still `dcc89ec0...e68a`. **NOT DEPLOYED.** No web build was served. No real API, real store, device, shared database or credential was involved. **Real workflow not verified.**

### 13.1 What was built

| Area | Files | Behaviour |
|---|---|---|
| API | `src/api/albums.ts`, `album-rules.ts`, `types.ts`, `error-messages.ts` | Contract 0.27.0 calls, response validators, the name rule, ten new error messages |
| State | `src/stores/albums.ts` | Modelled on the memories store: identity generation, read sequences, one action per target, captured key and payload |
| Views | `AlbumsView`, `AlbumDetailView`, `AlbumPhotoAdder` | `/us/albums` with inline creation, `/us/albums/:id`, under the "us" navigation entry, linked from `UsView` |
| Shared | `src/stores/memory-uploads.ts`, `session.ts` | `MEDIA_DISABLED` now tells both stores; the album store resets with identity. The upload queue is otherwise reused with scope `album:<id>` |

**Backend behaviour checked against the code, not assumed:** `AlbumService.cc` facts for each action (`actor_id`, and `action: edit` for a rename), the role and state order in Sections 7.4 and 3.1 of the rename design, and that a removal takes no body. Against the code only: the web has not called the real API.

| Rule | How the web follows it |
|---|---|
| Upload and add are separate | READY means "uploaded, not yet in the album". Only an explicit click adds, with READY ids only, in queue order. An unfinished upload stays behind |
| Atomic add | A refusal adds nothing and keeps every upload. The user can drop one and add again, as a new request with a new key |
| Unconfirmed write | Listed with what it captured; only a click resends the same key and payload; it survives leaving the page. Its uploads cannot be dropped meanwhile |
| Confirmed facts | Each mutation is checked against the request: actor, album, ids in order, media id, version, outcome. A name is compared only at the mutation's own version, and an album older than the facts stays unconfirmed. Newer state on replay is accepted, and today's cover and count are not compared |
| One write per album | At most one in flight per album, retries included, so one member's answers cannot arrive out of order. A write's answer outranks a read in flight, and an older version never replaces a newer one |
| Late results | Dropped after an account change or on leaving the page, for reads, writes and uploads |
| Removal | Shown only on the caller's own photos, with this explanation before confirming: it hides the photo, the partner cannot open it afterwards, a link already opened may work for a few minutes, the file is retained, and it cannot be restored or added anywhere else |
| Cover | Either member, from active photos. A fallback comes from the server's answer and is labelled as automatic |
| Pending request | No name form for either member; the creator approves or rejects, the proposer withdraws. Photo and cover writes stay available |

### 13.2 Verification

**Automated (Vitest, fake API and store):** exit 0 on the final tree.

| Check | Result |
|---|---|
| Focused: `tests/unit/albums.spec.ts` | 55 tests passed |
| Memory regression: `memories.spec.ts`, `media-client.spec.ts` | 71 tests passed |
| Full web suite | 34 files, 721 tests passed |
| `vue-tsc --noEmit -p tsconfig.json` | Exit 0, after fixing two errors from its first run |
| `tsc --noEmit -p tsconfig.node.json` | Exit 0 |

Covered: both member roles, pending restrictions, replay with the same arguments, atomic refusal, cover fallback, removal and each definite refusal, disabled uploads (flag false, missing, unreadable, and `MEDIA_DISABLED` mid-flow), malformed responses, and late reads, writes and uploads.

**Do the tests bite?** Nine behaviours were broken one at a time and restored: older album accepted, add ids not compared, identity not checked, serialization removed, older write replacing newer, uploads dropped on refusal, remove shown to everyone, rename form while pending, non-READY ids sent. Eight failed tests at once. **Serialization on retry did not**, which was a real test gap; a test now covers it, and the same breakage then failed it. Restoration was checked in the source before the final run.

**Browser (separate from the above):** the real app on Vite dev, in the built-in browser at 375 px, against a throwaway fake API and store on 127.0.0.1, kept outside the repository and written to follow `AlbumService.cc`. Fake data only. Stopped afterwards; port closed.

| Flow | Observed |
|---|---|
| Create | Empty album opened; no cover |
| Upload, then add | Two READY uploads, labelled as not yet in the album, with no photos in the album |
| Refused add | Notice that nothing was added; both uploads kept |
| Add, answer lost | Unconfirmed, locked; retry replayed on the fake (one logged replay); two photos, queue emptied, automatic cover |
| Cover, then remove it | Chosen label; explanation shown; fallback from the answer; notice that the file is retained |
| Creator rename | Name changed |
| Partner | No remove control on the other's photo; "申请改名"; name unchanged; both names shown; withdraw offered, approve and reject not; cover allowed while pending |
| Creator, approval answer lost | Approve and reject, no rename form; unconfirmed; retry replayed; name changed once, version 3 on the fake |
| Uploads off | Explained on list and detail; no picker; creation, rename and cover still there |
| Environment | No horizontal overflow at 375 px; `localStorage`, `sessionStorage` and caches empty; no service worker. Console errors: only the fake's deliberate 409 and 503 |

**What this browser run does not show.** The fake is my reading of the backend, not the backend. Vite dev has no service worker or CSP, and the fake store is same-origin, so neither CSP nor CORS was exercised. Script set the file input and clicked, so no person or native picker was involved. The media read rule after removal is not modelled by the fake: real partner access after removal is P3-2a's backend evidence.

### 13.3 Contract gaps, for review (nothing changed)

1. **A proposer cannot find a decision later.** `Album` has no last decision and no route lists requests, so once decided the request's id is gone from the album. Only a notice in the same session reports it. With no notifications (D20), a rejected partner may simply see the old name.
   *Superseded 2026-11-08 by phase3-p32b-album-rename-design.md Section 8:* `GET /api/v1/albums/{albumId}/proposals` (contract 0.38.0) lists every request with its decision, and album detail shows that history. D20 is still undecided; gaps 2-6 below are unchanged.
2. **`uploads_enabled` is only on the album list.** A directly opened album reads the first list page to learn it.
   *Superseded 2026-11-09 by phase3-plan.md Section 29:* album detail reads `GET /api/v1/media/capabilities` (contract 0.40.0) instead of the album list. A snapshot; the upload routes still enforce.
3. **No list of the caller's READY unattached uploads.** On leaving the page or reloading, READY photos not yet added cannot be recovered, and the server removes them after the unattached period. The same limit as memories.
   *Superseded 2026-11-08 by phase3-plan.md Section 28:* the album adder can choose them again from `GET /api/v1/media/unattached` (contract 0.39.0), a snapshot, not a reservation.
4. **An uploader cannot list their removed photos.** The design keeps them readable to the uploader, by id only, and no route lists them, so in practice the page cannot show them again.
5. **`ALBUM_PHOTO_UNAVAILABLE` names no photo.** Deliberate, one answer for every cause. So after an atomic refusal the UI cannot say which photo to drop.
6. **A cover or photo write does not move `version`,** so an older same-version album on a replay cannot be told from a current one. Serialization covers one client; a concurrent partner's write shows on the next read.

### 13.4 Limits and open items

- **Not exercised:** real API with this web, real store, device, real service worker lifecycle, human acceptance. No backend test was rerun, since no backend file changed.
- **Unconfirmed actions are memory only:** a reload or identity change clears them; re-reading shows whether the write happened.
- **Real storage still needs the deployment work** in Section 23.4 of the plan: Caddy CSP and bucket CORS. P3-2d.
- **Open as before:** the older-build rollback rehearsal (Section 18.1 of the plan), D20 notifications, D6 deletion, and Section 12.4 here.

## 14. P3-2d live rehearsal

The owner ran two desktop browser sessions against the isolated stack `usapp-p32d-browser-d2bee4e275`, with migrations through `0032` and synthetic photos in the dedicated Shanghai test bucket. The owner reported passing empty album creation, one three-photo batch in queue order, default cover and partner cover selection, cross-account viewing, direct creator rename, and approval, rejection and withdrawal of partner rename requests.

Real batch rejection and rollback, photo removal, fallback after removal, fresh signed reads after removal, prior URL expiry and browser cache behavior were not exercised. The owner skipped the removal checks. Device acceptance, proposer decision history, D6, D20 and the older-build rollback rehearsal remain open.

The stop output confirms that the three containers and the run network were removed and an eight-key manifest was exported. The owner reported deleting all eight objects and removing the temporary root certificate. The final bucket listing was not reported. No release was performed.
