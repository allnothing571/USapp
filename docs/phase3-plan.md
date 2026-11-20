# Phase 3 plan: memories

Current status correction (2026-11-06): the older milestone paragraphs below describe their original batches. P3-1, P3-2 and P3-3 core implementation is present; acceptance remains scoped as recorded in each evidence document. D6 photo deletion and whole-memory deletion, requests, cascade and erasure backend are implemented through 0040; M2 frontend passed automated checks, not real API/browser acceptance ([memory deletion Section 16](phase3-d6-memory-deletion-design.md#16-m2-前端实施与证据2026-11-05)). Timestamp correction 0041 is implemented and isolated-test verified. The external journal has J1 offline validation/planning, J2a async protocol, J2b1 media delete/undo, J2b2 memory delete/approval/cascade/undo, and J2c1 text-erasure integration, tested with injected fake journals and disposable PostgreSQL; [evidence](phase3-deletion-journal.md). No real provider is configured. Object-cleanup integration, database replay, production persistence and the restore drill remain unimplemented; frozen pending batches also need safe retirement/reconciliation. **NOT DEPLOYED. No real purge enablement.**

Status (2026-11-04, revision 2): **P3-1a, the P3-1b backend and the P3-1c frontend are implemented and verified against fakes: [Section 21](#21-p3-1a-implementation-and-isolated-verification-claude-code-2026-11-03), [Section 22](#22-p3-1b-implementation-and-isolated-verification-claude-code-2026-11-03) and [Section 23](#23-p3-1c-implementation-and-verification-with-fakes-claude-code-2026-11-03). The P3-1d adapter passed offline, and a basic real OSS and desktop browser flow passed on a test bucket, with most guarantees still open: [Section 24](#24-p3-1d-offline-oss-adapter-verification). P3-2 is [design for owner review](phase3-p32-albums-design.md). Everything else is design for review.** Revision 2 makes READY refer to immutable validated bytes (Section 4), and adds deletion and restore policy (Section 4.8). Later stages start only after owner review.

Labels:
- **[V1]** explicit requirement, `_req_extract.txt` (section numbers are V1's);
- **[Owner]** stated by the owner in this assignment;
- **[Fact]** read from this repository;
- **[Vendor]** from Alibaba Cloud documentation, read 2026-11-03, not exercised (Section 17). **No real OSS was contacted**;
- **[Rec]** proposed default, not a requirement;
- **[Assumption]** unverified.

## 1. Baseline and release frame

- **[Owner]** All Phase 3 features are finished and verified before the next production release. Production is at migration 0024; Phase 2 reminders and later migrations wait for the same combined release.
- **[Fact]** The newest migration is `0028_reminder_push_stale`, and no `0029` exists, so **the next available number is 0029**. Recheck before each new file. 0000-0028 are preserved unchanged.
- **Conflict to note:** [Doc] older status paragraphs in `CLAUDE.md` and `docs/phase2-*.md` say main is at 0000-0017. They are dated. This plan uses the owner's 0024, which was not independently checked here, because production is out of scope.
- **[Fact]** Free today: advisory locks from 33018 (highest in use is 33017); SQLSTATE class `UF` (no `UF`, `UG`, `UH` or `UK` value appears in `database/`, `services/` or `docs/`).
- **[Fact]** The stack stays Vue 3, C++/Drogon, PostgreSQL, Caddy and Docker Compose. `usapp_core` already links OpenSSL and libcurl, which is what signing and object-store calls need.

## 2. Requirements versus proposals

| Topic | [V1] says | Proposed here ([Rec]) |
|---|---|---|
| Memory (§26) | Timeline; date, title, description, one or more photos, creator, created time. "Story + time" | Limits: 1-9 photos, title 1-60, description up to 2000, date not after today in Beijing |
| Album (§27) | Separate from memories. Create, name, cover, upload many, both view, both upload. Categories are examples | No seeded albums. Cover must be one of the album's photos |
| Plan (§28) | Want, Plan, Finished. Things to do together, not goods or gifts. Plan can add date, time, place, note. Finished can link to or become a memory | Forward moves only, unless the owner wants undo (D10) |
| Edit (§29-30) | Creator edits directly; the partner cannot overwrite; a request holds original, suggestion, requester, created time, status (Pending, Approved, Rejected). Creator accepts or rejects | Add proposer `withdrawn`, as anniversaries do. Allow one pending request per item and block creator edits while pending (the 0018 "option A") |
| Direct actions (§31) | State changes that do not damage the body: Want → Plan, Plan → Finished | Whether cover, adding album photos and adding plan details count is D8, D9, D11 |
| Images (§45, §39) | No BLOB; object storage; store `object_key`, metadata, uploader, `couple_id`, `mime_type`, size, `created_at`. Access authenticated, by authenticated or short-lived signed URL. Private storage | Exact flow below. `object_key` is server-generated |
| Vendor (§52.1, §52.5) | Vendor and region **undecided** | The assignment's "OSS" with cloud-side processing is read as Alibaba Cloud OSS with its IMG feature: **D1** |
| Notifications (§18.1) | New photo, memory and wish are optional: "🔔 通知对方" | Reuse the 0020 intent pattern (Section 11) |
| Backup (§38; [Doc] `architecture.md` §5.7) | Private storage plus backup. Ready originals need independent media backup and one restore drill | The replication strategy stays open (D5). A verified database-and-object restore drill is a release gate either way |
| Encryption fields (§40) | Suggests reserving `encryption_version` and similar. No E2EE in V1 | Do not reserve: additive columns can arrive with the feature. D13 |
| Deletion | **Silent** for memories, albums and plans | Policy in Section 4.8, in force before any real photo is accepted. Its sub-choices are D6 |

V1 §42 suggests `Memory`, `MemoryPhoto`, `Album`, `AlbumPhoto`, `Wish / DatePlan`, `EditRequest`, leaving table design to the implementer. [Doc] `architecture.md` §5.6 says shared content uses independent tables with no `shared_contents` parent, so edit requests stay per domain, as anniversaries do.

## 3. Milestones

| Order | Milestone | Scope | Depends on |
|---|---|---|---|
| 1 | **P3-1 Photo memories** (first batch, below) | Media workflow, memories, timeline, edit requests | Phase 2 schema |
| 2 | P3-2 Shared albums | Albums, cover, photos from either member. [Design](phase3-p32-albums-design.md); one narrow media read-rule change | P3-1 media |
| 3 | P3-3 Shared plans | Want → Plan → Finished, details, link to or create a memory | P3-1 memories |

P3-2 and P3-3 are independent of each other. Each gets its own design and approval. Sections 12 and 13 sketch only what would affect P3-1's shape.

**First-batch stages, each stopping for review:**

| Stage | Contents | Proves |
|---|---|---|
| P3-1a (done, fake store only) | `0029_media`, `ObjectStore` guarantees and fake, upload, promotion, view, cleanup job, late-object recovery. **User deletion left for D6 review** | Immutable READY, lifecycle and isolation with no vendor |
| P3-1b | `0030_memories`, memory service, edit requests. **Built and verified without notifications or deletion: Section 22.** Notifications await review (Section 11); photo and memory deletion await D6 | Attachment, approval, retries; deletion cascade with D6 |
| P3-1c | Web: picker, upload queue, timeline, detail, edit, request review. **Built and verified with fakes: Section 23.** Delete and restore await D6 | Client state, account switching |
| P3-1d | **Separately authorized:** real OSS acceptance in a test bucket; browser and device checks | The [Vendor] items in Section 17 |

P3-1d is the only stage needing owner credentials. It must prove the store guarantees in Section 4.2 and the evidence gates in Section 6. **Real OSS integration is not enabled, and the real store adapter is not trusted, until it has.**

## 4. Media model

A **media object** is one uploaded image, owned by one couple and one uploader from the moment of the grant.

**The rule of revision 2: READY names one immutable object, and every view and attachment resolves to that object.** A grant can stay usable after completion, and PostObject cannot be revoked. So a client-writable key can never be what READY refers to, and a recorded ETag or CRC64 does not protect later reads of a mutable key. Hence two prefixes:

| Key | Who can write | Who can read | Purpose |
|---|---|---|---|
| Staging: `staging/{couple_id}/{media_id}/upload` | Any holder of the grant, until it expires | API only, for HEAD and copy. **No signed read URL is ever issued** | Untrusted input. May change or reappear at any time |
| Final: `media/{couple_id}/{media_id}/{attempt_uuid}/original` | API only, once per key. **No grant ever names this prefix** | API, and signed reads after READY | What READY refers to |

### 4.1 Lifecycle

```
 grant ─> GRANTED ──────────────── expired, abandoned ───────┐
            │                                                │
   browser POST to staging (repeatable until expiry)         │
            │                                                │
 complete:  attempt row committed                            │
            copy staging -> new final key                    │
            HEAD, image/info, HEAD on the final key          │
            │                                                │
            ├── valid ─> READY (names that attempt's key)    │
            │             │  staging and losing attempts ─┐  │
            │             │                               │  │
            └── invalid ─> REJECTED ──────────────────────┼──┤
                          │                               v  v
                 referenced, viewable               cleanup plan
                          │                         first pass, then sweep after
                  unreferenced 7 days ─> plan         the last possible late write
                  deleted by a user  ─> Section 4.8
```

State is derived, not updated, following P2-5's append-only pattern. `usapp_api` gets `SELECT, INSERT` only, except the one purge path in Section 4.8.

### 4.2 Store guarantees

`ObjectStore` is defined by these guarantees, which the fake enforces and tests rely on. **Which vendor primitive provides each is a P3-1d question.** If one cannot be proved, real integration stays off and the design returns for review.

| # | Guarantee | Fake | [Vendor] status |
|---|---|---|---|
| G1 | A grant can write only its exact staging key, within size and type, and nothing after expiry, except a request already in flight, whose time is bounded | Refuses any other key, so a grant used on a final key fails | Key, type and size policy documented. **Expiry timing of in-flight requests not found** |
| G2 | A client credential cannot write a final key. Server identities are separated by prefix | Tracks identity per call | RAM policy: to prove |
| G3 | Copy is atomic: the destination appears whole, holding one version of the source, or not at all. It may succeed and report failure | Modes: hold, fail before, **succeed then report failure**, overwrite source just before | Copy documented. **Atomicity not found in documentation** |
| G4 | After a successful copy, HEAD and image/info read that object (read-after-write) | Immediate | **Not found** |
| G5 | A HEAD's ETag changes if the bytes change. Used only as a consistency check | Content hash | ETag documented; to prove for copies |
| G6 | Delete is idempotent; deleting an absent key succeeds | Yes | Documented |
| G7 | A signed read names one key, one transform and one method, and cannot write | Refuses altered URLs | To prove |
| G8 | Listing eventually shows every object under a prefix. Late objects are only found through it (added in P3-1a) | Immediate | To prove |

**Deliberately not required:** `x-oss-forbid-overwrite` and `x-oss-copy-source-if-match`. [Vendor] Both are documented, the first with the versioning caveat from revision 1. But each attempt writes its own never-reused key, and inspection reads that key, so correctness needs neither. **Bucket versioning is therefore no longer forced off,** which leaves D5 open. Whichever is chosen, versions must be handled by deletion (Section 4.8).

### 4.3 Upload grant

`POST /api/v1/media/uploads`, with exact Origin, CSRF, `Idempotency-Key`. Body: `content_type`, `size_bytes`. No filename is accepted or stored: V1 does not ask for it and it leaks.

In one transaction, with **no network**: caller's member row `FOR SHARE`, per-user lock 33018, key classification, bounds, insert. Then sign, which is local HMAC.

| Bound | [Rec] value | Why |
|---|---|---|
| Types | `image/jpeg`, `image/png`, `image/webp` | HEIC excluded: see Section 6 |
| Size | 1 byte to 20 MB | [Vendor] the default IMG source limit is 20 MB |
| Validity | 15 minutes | Long enough for one phone upload |
| Open grants | 20 per user | Stops runaway clients |
| Grants | 200 per user per day | Cost bound |
| Couple storage | 20 GiB, checked at grant | Cost bound: D4 |

**[Rec] PostObject with a V4 policy, no STS.** [Vendor] A policy can state `["content-length-range", min, max]`, so OSS enforces size, plus exact key and content type. The response holds endpoint, form fields, policy, signature and `expires_at`. It is `no-store` and never logged.

**Replay:** same key and fingerprint, media still GRANTED and unexpired: same media id, a freshly signed form with the **original** expiry. Otherwise it returns the current state with no form, so a terminal media object never receives a new form. A different body is `IDEMPOTENCY_KEY_REUSED`.

### 4.4 Direct upload

The browser POSTs to the staging key with `withCredentials=false`. Cookies and CSRF tokens never go to OSS, and bytes never pass through the API. At most 3 concurrent uploads, no offline queue. A retry simply re-POSTs: staging is allowed to change, because nothing trusts it.

### 4.5 Promotion: binding READY to the inspected bytes

`POST /api/v1/media/uploads/{id}/complete`: uploader only, retry-safe.

**Inspect after copying, on the copy.** The bytes inspected are therefore the promoted bytes by construction, and there is no interval between inspection and finalization for an overwrite to enter. Whatever happens to staging afterwards is irrelevant.

| Step | Where | What |
|---|---|---|
| 1 | Read | Outcome exists: return it. Past `expires_at` + 15 minutes: stop |
| 2 | Network | HEAD staging. Absent: `MEDIA_NOT_UPLOADED`. Store down: 503. **Neither consumes an attempt** |
| 3 | Transaction, 33018 | Refuse if an outcome or cleanup plan exists, or 5 attempts exist. Insert the **attempt row with a new random final key**, so every key that could exist is known before it can exist |
| 4 | Network, no lock | Copy staging to that key |
| 5 | Network, no lock | **HEAD final → `image/info` final → HEAD final.** The two HEADs must agree. If not, G4 or G2 has been violated: fail with an operator-visible error, never READY |
| 6 | Validate | From the final object only: size equals the grant, decoded format allowed and equal to the declared type, pixel bounds |
| 7 | Transaction, 33018 | Refuse if an outcome or plan exists. Insert the outcome naming this attempt, with size, ETag, CRC64, dimensions and real format. Audit. Plan cleanup of staging and losing attempts |

**HEAD and image/info describe the same object** because the key is written once, by this attempt, and no other principal can write it (G1, G2). The bracketing HEADs detect a violation of that; they are not the protection.

**Stored `Content-Type`** comes from untrusted staging, so it is never trusted: signed reads override the response type from the validated format, and add `nosniff`.

**Recovery:**

| Failure | State left | Retry does |
|---|---|---|
| Step 3 commit lost | Attempt may or may not exist | Starts over. An unused attempt only ever means an absent or sweepable key |
| Copy fails | Attempt, no object | New attempt |
| **Copy succeeded, response lost** | Attempt, object nobody relies on | New attempt, new key. The old key is known and swept |
| Crash after the copy | Same | Same |
| **Store succeeded, step 7 failed** | Validated object, no outcome | New attempt, new key, inspected afresh. Bytes may differ if staging changed, and the new inspection is what counts |
| Step 7 commit unknown | Possibly READY | Step 1 returns READY if it committed; otherwise a new attempt |
| Two concurrent completes | Two keys, each inspected | First outcome stands. The loser's key is swept, and the loser returns the stored outcome |
| One finds good bytes, one bad | Same | First outcome stands, naming **its own** bytes. Either is honest about what it names |
| 5 attempts used | Attempts, no outcome | REJECTED `attempts_exhausted`. The client uploads anew |

**Outcomes:** REJECTED reasons are `size_mismatch`, `not_image`, `format_mismatch`, `dimensions`, `attempts_exhausted`. A store outage or a HEAD disagreement is never a rejection. Rejection is terminal: another try takes a new media object.

**Attaching and viewing** take the media id and resolve only through the READY outcome to its attempt's key. No code path constructs a final key from a client value, and a static check keeps it so.

### 4.6 Viewing

`GET /api/v1/media/{id}/content?variant=thumb|view|original`, with the session cookie and no CSRF (read). `<img src>` works same-origin: [Fact] the cookie is `SameSite=Lax`. The API checks membership and state, then answers **302** with `Cache-Control: no-store` to a short-signed URL for the READY key.

| Who | When visible |
|---|---|
| Uploader | READY, even unattached, for draft preview |
| Either member | Once bound to a memory or pending proposal of their couple |
| Anyone, in any state before READY, with a cleanup plan on the object, or after deletion | Not viewable. **Before READY there is no preview, since staging is never readable** |
| Another couple or unknown id | 404. Anonymous 401. Same body, so existence is not revealed |

**Signed URL:** valid at most 10 minutes. The signing time is floored to a 5-minute bucket, so the same image keeps its URL and the browser cache works. It signs the variant's `x-oss-process`, so nobody can request an arbitrary transform.

**Accepted limit:** anyone holding a signed URL can use it until it expires. V1 §39 explicitly allows this. [Fact] Service worker rules exclude `/api/` and only cache same-origin shell requests, so no photo enters its cache. A test keeps it so.

**CSP change needed:** [Fact] `deploy/Caddyfile` sets `connect-src 'self'` and `img-src 'self' data: blob:`. Both gain exactly the bucket origin. Bucket CORS allows exactly the public origin, for POST, and only staging needs it.

### 4.7 Cleanup, including writes after a delete

A grant cannot be revoked, so **a staging object can reappear after being deleted, until the grant is dead.** A copy already in flight can do the same to a final key. So one delete is never treated as final. Scheduler job `media.cleanup`, off unless `MEDIA_ENABLED`, decides in the tick's transaction and calls the store after commit.

A **cleanup plan** has a scope and a `sweep_not_before`:

| Trigger | Scope | Deletes | Sweep not before |
|---|---|---|---|
| READY | `leftovers` | Staging and losing attempts. **Never the READY key: the guard and the key list both exclude it** | Later of `expires_at` + 1 hour, last attempt + 1 hour |
| REJECTED, **grant possibly still valid** | `everything` | Staging and all attempts | Same |
| Abandoned (no outcome at `expires_at` + 1 hour) | `everything` | Same | Same; so the first pass may be the sweep |
| READY, unreferenced 7 days | `everything` | Staging and all attempts, including the READY key | Now |
| Purge after deletion (4.8) | `everything` | Same | Now |

**Passes:** a first pass runs at once. A **sweep pass** runs at or after `sweep_not_before`, and only its success marks the plan complete. Late writes before the sweep are removed by it.

**After the sweep:** in-flight upload time is bounded in practice but [Vendor] not documented. So:
- A **bucket lifecycle rule on the `staging/` prefix**, 2 days [Rec], is now safe as a backstop, because staging never holds anything READY depends on. Revision 1 rejected lifecycle rules because they could not tell states apart; separate prefixes solve that. **No lifecycle rule covers `media/`.**
- A late staging object can never become anything: `complete` refuses once an outcome or plan exists. Its only cost is storage until the rule fires.
- A reconciler lists prefixes against PostgreSQL. As implemented in P3-1a (Section 21.2) it removes only keys the database calls `late`, and only reports keys no row names.

**Guards, in the database:** `everything` for an unreferenced READY object repeats the predicate under 33018, and refuses anything referenced by any revision or pending proposal. Attachment takes 33018 too, so it either sees the plan and refuses, or commits first and the plan is refused. Bounded at 100 plans per tick; network failures leave the plan to retry.

### 4.8 Deletion policy (in force before any real photo is accepted)

**Real photos are not accepted, in production or anywhere, until this is implemented and tested.** The owner approved the D6 defaults below.

**Who may delete**

| Item | Who | Why |
|---|---|---|
| A photo | **Its uploader**, always, including one sitting in the partner's memory | It is that person's image. Takes priority over §29's editing rule, which concerns overwriting body text: **D6a** |
| A memory | Its creator. The partner sends a delete request through the existing proposal flow | §29 |
| An album, a plan | Decided in P3-2, P3-3 under the same principles | |
| Administrators | Not a routine path (V1 §37). Any repair is audited and separate | |

A memory whose photos were all deleted stays as text with a placeholder: **V1's "one or more" applies when creating or editing, not to history.** A pending proposal naming a deleted photo can no longer be approved.

**Status correction (2026-11-06): D6a–D6d defaults and whole-memory O1–O12 approved.** Photo deletion is implemented from 0035. Whole-memory deletion, requests, cascade and text erasure are implemented through 0040 and verified with fakes in disposable instances; M2 frontend passed automated checks only. [Whole-memory evidence](phase3-d6-memory-deletion-design.md) supersedes the older text-deletion sketch below: memory text is overwritten under a narrow privilege boundary. The [external journal J1](phase3-deletion-journal.md) is an offline validator/planner only, not online durable logging or a restore barrier. Real storage, restore and release gates remain open. NOT DEPLOYED.

**When reads stop**

| Moment | What stops |
|---|---|
| **Commit of the deletion** | All API reads, new signed URLs, attachment and new proposals. The route answers 404, like anything absent |
| Up to 10 minutes after | **A signed URL already issued may still work until the object is purged or it expires.** Cannot be recalled |
| Never recallable | Copies already in a device's browser cache, or saved by the partner. The UI says so |

**Recovery window, then originals:**
- Deleted bytes stay for **7 days** [Rec, **D6b**] so that an accident on irreplaceable photos can be undone by the deleter, with reads still stopped throughout. A `media_deletions` row is followed, if undone, by a `media_deletion_reversals` row, both append-only.
- At `purge_after`, under 33018, the job commits a purge plan: **irreversible from that commit**, racing undo safely. The plan deletes every key of the object with the passes of 4.7.
- Variants are computed on demand and are not stored, so there is nothing else to purge. **A CDN, if D1 adds one, must be added to this list.**

**Metadata and text**

| Data | After purge |
|---|---|
| Media row: id, couple, uploader, size, dates, keys | Kept, as evidence and for the sweep. No bytes |
| Memory title and description | **Erased.** Text moves to a dedicated `memory_texts` table referenced by revisions and proposals. It is the one table with a delete path: a narrow `SECURITY DEFINER` function that requires a tombstone past its window. Every other table stays append-only |
| Audit rows | Kept; they hold ids and never text |

**Backups, for any D5 strategy.** The strategy stays open, but it must meet these:
1. **Deletion reaches every copy:** a replica either replicates deletes or expires independently, within a stated bound: **30 days after purge** [Rec, **D6c**]. If versioning is on, non-current versions fall under the same bound.
2. **Database backups carrying text** follow the same retention bound.
3. **A restore must not resurrect deleted photos.** Restoring an older database loses tombstones. So each deletion and purge also writes a media id, and no content, to an **append-only deletion journal outside PostgreSQL**, and restore replays it before serving anything.

The privacy statement shown to users states these bounds exactly. "Deleted" never implies instantly gone from every backup.

### 4.9 Restore drill (release gate)

A **verified database-and-object restore** is required before the combined release, whichever D5 strategy is chosen. In an isolated environment, from real backups of a test deployment:

| Check | Pass condition |
|---|---|
| Database restored | Migrations and checksums match |
| Objects restored | Every READY key exists; size and CRC64 equal the recorded values |
| Readable | Variants render through the API from a real session |
| **A photo deleted after the backup was taken** | Journal replay removes it. It is not served |
| Orphans | Reconciliation report reviewed, nothing unexplained |
| Recorded | Backup age achieved, restore duration, operator steps |

**Not satisfied** by checking that replication runs, or by a database-only restore.

## 5. Why no Node/sharp service now

[Rec] **Do not add one.** No concrete evidence requires it. Variants come from signed `x-oss-process` URLs, with no extra stored objects and no decoder on our hosts, which keeps image-decoder vulnerabilities off our side. The existing push worker is Node, but it does no image work and holds no credentials for this.

## 6. Evidence gates for cloud-side processing

Variants, [Rec]: `thumb` (`image/auto-orient,1/resize,w_480/format,webp`), `view` (same at `w_1600`). These strings are checked at P3-1d.

[Vendor] limits found that could force a change:

| Finding | Effect | Resolved by |
|---|---|---|
| HEIC sources are limited to 4096 px a side | **Larger phone HEIC photos would fail** | First batch refuses HEIC. iOS Safari is expected to convert when `accept` omits HEIC: **[Assumption], check on a device** |
| Rotation sources are limited to 4096 px a side | **Auto-orient of a larger phone JPEG with an EXIF rotation may fail or come out sideways.** The docs do not say how auto-orient relates | Must test with real phone photos at P3-1d |
| EXIF handling is undocumented in what was found | **GPS could survive in a variant** | Must test: variants must have no GPS. Originals keep it, private |
| Source limit 20 MB (quota can be raised) | Larger originals refused | Bound in Section 4.3 |

**Decision order if a gate fails, cheapest first:**
1. Change the IMG parameters.
2. Serve only variants proven clean, and never expose `original` to the client.
3. Client-side canvas normalization before upload: fixes orientation and size and removes EXIF, but discards the true original, so it needs **owner consent (D3)**.
4. **Only then propose a sharp service**, and only with: the failing sample class, the IMG result, why 1-3 fail, and the cost of a new decoding service with bucket credentials.

## 7. Schema sketch (not final)

All tables append-only with `usapp_api` `SELECT, INSERT`, composite same-couple FKs, and DB guards that recompute facts. `0029_media`, SQLSTATE `UF0xx`:

```
media_objects              id, couple_id, uploader_id, staging_key UNIQUE, declared_type, declared_size,
                           expires_at, idempotency_key, request_fingerprint, created_at
                           UNIQUE (id, couple_id); UNIQUE (uploader_id, idempotency_key)
                           FK (couple_id, uploader_id) -> couple_members
                           CHECK staging_key is exactly staging/{couple_id}/{id}/upload
media_promotion_attempts   media_id, attempt_no, couple_id, final_key UNIQUE, created_at
                           PK (media_id, attempt_no); at most 5
                           CHECK final_key starts media/{couple_id}/{media_id}/ and differs from staging
                           guard: no outcome, no plan, within the window, under 33018
media_upload_outcomes      media_id PK, outcome (ready, rejected), reason, attempt_no, real_format, width, height,
                           stored_size, etag, crc64, decided_at
                           FK (media_id, attempt_no) -> attempts
                           guard: ready needs an attempt and all facts; bounds; no plan
media_cleanup_plans        media_id, scope (leftovers, everything), reason, sweep_not_before, created_at
                           PK (media_id, scope); guard repeats eligibility under 33018
media_cleanup_passes       media_id, scope, pass_no, kind (first, sweep), completed_at
media_deletions            media_id PK, couple_id, deleted_by, purge_after, created_at
                           guard: the uploader only
media_deletion_reversals   media_id PK, reversed_by, created_at
                           guard: the deleter, before any purge plan, under 33018
```

The READY key has one source: `outcome -> attempt -> final_key`. Nothing stores or accepts it separately.

`0030_memories`, mirroring 0018, SQLSTATE `UG0xx`, lock 33020 per memory (33019 went to grant limits in P3-1a). This is the sketch; what was built differs as Section 22.1 records:

```
memories                   id, couple_id, creator_id, created_at; UNIQUE (id, couple_id)
memory_texts               id, couple_id, title, description            <- erasable: Section 4.8
memory_revisions           memory_id, version, couple_id, actor_id, action (create, edit, approve), proposal_id,
                           memory_date date, text_id, key, fingerprint, created_at
memory_revision_photos     memory_id, version, position, media_id, couple_id
                           PK (memory_id, version, position); UNIQUE (memory_id, version, media_id)
                           FK (media_id, couple_id) -> media_objects   <- database-level same couple
memory_edit_proposals      id, couple_id, memory_id, proposer_id, kind (edit, delete), base_version, memory_date, text_id, key, fingerprint
memory_proposal_photos     proposal_id, position, media_id, couple_id
memory_edit_decisions      proposal_id PK, couple_id, decided_by, outcome (approved, rejected, withdrawn), key, fingerprint
memory_deletions           memory_id PK, couple_id, deleted_by, purge_after, created_at
memory_deletion_reversals  memory_id PK, reversed_by, created_at
media_bindings             media_id PK, couple_id, target_kind ('memory'), target_id, bound_by, created_at
```

**Why `media_bindings`:** one media object belongs to one target forever, whichever revisions or proposals reference it. So a photo cannot be reused across items, cleanup has one place to look, and P3-2 adds `'album'` to the closed set. Photo guards require READY, no cleanup plan, no standing deletion, same couple, and binding to this very memory (or first binding here).

**Memory date** is a `date` ([V1] §44): no UTC shift.

## 8. API sketch (contract 0.24.0)

| Method, path | Purpose | Caller |
|---|---|---|
| `POST /api/v1/media/uploads` | Grant | Member |
| `POST /api/v1/media/uploads/{id}/complete` | Verify | Uploader |
| `GET /api/v1/media/{id}` | Facts: state, dimensions, uploader | Per 4.6 |
| `GET /api/v1/media/{id}/content` | 302 to signed URL for the READY key | Per 4.6 |
| `POST /api/v1/media/{id}/delete`, `/restore` | Tombstone, and undo within the window | Uploader; deleter |
| `GET /api/v1/memories?cursor=&limit=` | Timeline, with `uploads_enabled` | Member |
| `POST /api/v1/memories` | Create, with ordered `media_ids`, `notify_partner` | Member |
| `GET /api/v1/memories/{id}` | Current state, photos, pending proposal | Member |
| `POST /api/v1/memories/{id}/revisions` (built as the anniversary route, not `/edit`) | Full replacement, with `expected_version` | Creator |
| `POST /api/v1/memories/{id}/proposals` | Edit or delete request with `base_version` | Partner |
| `POST /api/v1/memories/{id}/delete`, `/restore` | Tombstone, and undo within the window | Creator; deleter |
| `GET /api/v1/memories/{id}/proposals/{pid}` | Original (base revision) and suggestion | Member |
| `POST /api/v1/memories/{id}/proposals/{pid}/decision` | approve, reject (creator); withdraw (proposer) | Per role |

All responses `no-store`. All writes have exact Origin and CSRF; creations have `Idempotency-Key`. Timeline order: `memory_date DESC, created_at DESC, id DESC`.

**`uploads_enabled` from the start**, learning from the reminder availability gap: with `MEDIA_ENABLED` off, uploads answer 503 `MEDIA_DISABLED`, existing memories remain listed, and the UI says so.

**Config:** `MEDIA_ENABLED` (default false), `OSS_ENDPOINT`, `OSS_REGION`, `OSS_BUCKET`, and separate credentials per role, [Rec]: upload signer (write staging only), read signer (read final only), server (read staging, write and delete both). Environment only, never logged, placeholders in `.env.example`. `ObjectStore` is an interface with an asynchronous real implementation and a test fake, as `push::Transport` is. **Never a blocking call on an event-loop or ORM thread**: the R2 hang was exactly that.

## 9. Same-couple isolation

The couple always comes from the session's member row, never from the request.

| Step | Enforcement | Foreign attempt |
|---|---|---|
| Grant | Member row `FOR SHARE`; server builds the key | No couple: `SPACE_NOT_FOUND` |
| Direct upload | Signed policy names one exact **staging** key | Store refuses another key, and every final key (G1, G2) |
| Promote | Join on caller's couple and `uploader_id`; source and destination built by the server | 404, nothing read from the store |
| Attach | Service join, then composite FK and guard | 404; DB refuses even direct SQL |
| View | Join on caller's couple, binding rules, READY key only | 404, no signature made |
| Delete, restore | Join on caller's couple; uploader, creator or deleter in the guard | 404; wrong role 403 |
| Edit, propose, decide | Join on caller's couple; roles in the guard | 404; wrong role 403 |
| Cleanup | System job, operates by id; deletes only keys recorded for that row | Key CHECKs tie them to the couple |

[Fact] `couple_members.user_id` is unique and no leave or unbind path exists. If one arrives, access to shared media must be redesigned then. Not solved here.

## 10. Editing, retries and concurrency

**Reuse 0018's pattern as it fits:** identity plus revisions, full-replacement proposals against the current version, one decision per proposal, creator edits blocked while one is pending, classification before state checks so retries replay. The photo list is part of each revision and proposal.

**Lock order:** member `FOR SHARE` → 33020 memory → key classification → sorted 33018 media → inserts → 33012 preference → audit. Cleanup holds only 33018 and never waits for 33020. So no cycle.

| Race or retry | Result |
|---|---|
| Same create key, response lost | Replays the committed memory |
| Same key, different body or photos | `IDEMPOTENCY_KEY_REUSED` |
| Two creator edits, same `expected_version` | One wins; other `MEMORY_VERSION_CONFLICT`, with current state |
| Creator edit against a pending request | `MEMORY_PROPOSAL_PENDING` (option A) |
| Partner proposal against a moved version | `MEMORY_VERSION_CONFLICT` (built as anniversaries answer; no separate `MEMORY_PROPOSAL_BASE`): review the new text |
| Two proposals at once | One pending; other refused |
| Approve against reject or withdraw | PK on the decision: first wins; other replays or conflicts |
| Attach against cleanup | 33018: plan refused, or attachment refused and client told to re-upload |
| Complete against cleanup | 33018 at steps 3 and 7: first wins. A copy landing after the plan is removed by the sweep |
| Undo against purge | 33018: undo first keeps the object; purge plan first makes undo refuse |
| Approve against the photo's deletion | 33018: a deleted photo makes the approval stale |
| Photo in two memories concurrently | `media_bindings` PK: second refused |
| Commit unconfirmed | `RESULT_UNKNOWN`; same key retry, as elsewhere |
| Account switch during upload | Client discards; late answers dropped; orphan cleaned |
| A partner's uploaded photo in a rejected proposal | Not on the memory; removed 7 days after the rejection unless used again (Section 22.2) |

**Approved proposal:** the approve revision copies the proposal's text and photos exactly, as the 0018 guard enforces, in one transaction.

**Web:** a store modelled on `reminders.ts`: identity-scoped, captured key and body, retry by user click only, malformed responses treated as unknown.

## 11. Notifications

[V1] §18.1 makes these optional. [Fact] This touches frozen behaviour by addition only:
- `0030` extends `notifications`, its closed event and `target_kind` sets, and the intent operations, by the 0020 and 0025 pattern, with `memory.created`, `memory.edited`, `memory.edit_proposed`, `.edit_approved`, `.edit_rejected`, `.edit_withdrawn`. Target kind `memory`.
- **The stateless push worker must change:** [Fact] `services/push-worker/src/deliver.ts` has a closed `TARGET_KINDS` of task, reward, anniversary, message. Without `memory` it refuses those jobs. P2-6 did not need this, so it is new work. Web labels and `targetPath` change too.
- Text and images never appear in a notification or Push payload.

## 12. P3-2 albums, shape only

**P3-2a is implemented and verified with a fake store in disposable instances only; NOT DEPLOYED: [phase3-p32-albums-design.md](phase3-p32-albums-design.md) Section 12.** D12, D9, D17, D8 and D19 are approved. That design (2026-11-04, revision 2) supersedes this sketch: albums hold independently uploaded photos only, one home per photo, and removal hides without rebinding. **Correction:** "reuses media unchanged" was wrong. `visibleTo` must stop treating every bound photo as readable by the partner, or a removed album photo would stay readable. Grant, complete and cleanup are unchanged. Creator rename and partner edit requests are P3-2b, implemented and verified the same way: [its design](phase3-p32b-album-rename-design.md) Section 7. Open: D20.

## 13. P3-3 shared plans

See the [shared plans design and evidence](phase3-p33-shared-plans-design.md) for permissions, transaction boundaries and acceptance cases. The owner approved D10, D11 and the P3-3 defaults. P3-3a and P3-3b passed isolated backend verification. P3-3c passed web tests and isolated two-session HTTP probes. Manual browser acceptance remains open.

`plans` + revisions (title, date, time, place, note, under §29-30) + append-only `plan_status_events` (either member, §31) + `plan_memory_links`. "Become a memory" creates the memory through the same service and writes the link in one transaction, so a retry cannot produce two. Scope check from [V1] §28: no goods or wishlists. Open: D10, D11.

## 14. Decisions requiring owner confirmation

**Stay open**

| # | Decision | Note |
|---|---|---|
| D1 | **Vendor and region.** V1 §52 left both open, and region affects filing, speed and the bucket | Working assumption: Alibaba Cloud OSS. **Blocks P3-1d, not P3-1a to c** |
| D5 | **Replication strategy:** cross-region replication, a scheduled copy, versioning, or a mix | Open by instruction. Whichever is chosen must satisfy Section 4.8's backup rules and pass the drill in 4.9. **The drill is a release gate regardless** |

**Approved deletion choices (policy defined in 4.8)**

| # | Decision | Recommendation |
|---|---|---|
| D6a | Uploader may delete their own photo from the partner's memory | Yes. Alternative: only through an edit request, which lets the partner keep someone's photo against their wish |
| D6b | Recovery window before bytes go | 7 days. Zero is stronger erasure but no undo for irreplaceable photos |
| D6c | Backup bound after purge | 30 days. Must be stated to users |
| D6d | Delete request for memories: partner asks, creator decides | Yes, through the existing proposal flow |

**Confirm**

| # | Decision | Recommendation |
|---|---|---|
| D2 | Accepted types | JPEG, PNG, WebP; no HEIC, GIF or SVG now |
| D3 | Originals: keep the true file, or normalize in the browser if gates fail | Keep true originals |
| D4 | Limits: 20 MB per photo, 1-9 per memory, 20 GiB per couple | As stated; these drive cost |
| D7 | Can the partner directly add a photo to the creator's memory? | No: through an edit request |
| D8 | Album name and cover | Name per §29, in P3-2b; cover direct (§31 by analogy), from active photos, with fallback |
| D9 | Who may remove an album photo? | Its uploader |
| D17 | Removing an album photo | Hides at once; bytes stay, bound, until D6; no restore in the first batch |
| D18 | Album rename | **Approved 2026-11-04, Option A:** creator direct and partner edit request, together in P3-2b: [the P3-2b design](phase3-p32b-album-rename-design.md) Section 2 |
| D19 | Album limits | Name 1-40; 1-20 photos per add; no reorder, move or album delete; empty albums allowed |
| D20 | Album notifications | Decide with memories (Section 11) |
| D10 | Plan undo, or skipping Want → Finished | Approved: forward only, exactly the two steps V1 shows |
| D11 | Partner adding plan details to the creator's item | Approved: edit request |
| D12 | A photo in both a memory and an album | One home: album photos are uploads only; a memory photo needs a second upload. Later reference path specified, not built |
| D13 | Reserve `encryption_version` columns (V1 §40) | No |
| D14 | Future memory dates | Refuse |
| D15 | Test credentials for P3-1d: separate RAM identities and a test bucket | Yes, owner-created and owner-held |
| D16 | Production's Push control state during migration | Owner to confirm: must be disabled |

## 15. Tests

Same discipline as P2: targeted first, one full regression per stage, unchanged timeouts, no new framework or dependency.

**Isolated strategy, with no production credentials or real OSS:**
- **Disposable stack** as in `Claude outputs/p26-availability-isolated-verify.ps1`: generated credentials, internal network, PostgreSQL on tmpfs. Docker's default address pools are full of retained networks, so it needs a fixed subnet or an owner cleanup decision.
- **Fake object store, in-process,** enforcing G1-G7 and manually holdable like `FakeTransport`: scripted overwrite at any point, copy that succeeds and reports failure, in-flight copies and uploads landing late, unavailability, delays, recording every call. It records which identity made each call, and **fails the test on a second write to any final key, or any write to a final key by a grant.**
- **Fake OSS HTTP stub** on the isolated network, using the existing raw-socket `StubWorker` approach, for the real client's request shape. It independently recomputes signatures, refuses a wrong key, size or expiry, and serves fixtures for `image/info`.
- **Signature vectors:** fixed inputs and expected outputs for policy and URL signing, taken from vendor documentation where published. Stub agreement alone would only prove self-consistency, which is why P3-1d remains necessary.
- **Real API binary**, as in the reminder availability check, for the `302`, cookie, 401 and 404 paths.
- **SQL ledgers in both roles**, with rollback prevalidation against 0000-0028 and representative data. Function review of anything replaced. InitRoles and privileges twice.
- **Migrations:** first committed application freezes them, as before.

**What this cannot prove,** left to P3-1d: real signature acceptance, CORS, IMG output, EXIF, orientation, phone formats, real browser upload, cache behaviour.

## 16. Acceptance cases

**Immutable READY (revision 2)**
1. **Overwrite between inspection and finalization:** after `image/info` passes and before the READY commit, overwrite staging. READY still names the inspected object: fetched bytes equal the validated CRC64.
2. **Overwrite between staging HEAD and copy, including with an undecodable or oversized-claim file:** the copy holds whichever whole version it read, inspection judges that, and a mismatch is rejected. Never READY on bytes not inspected.
3. **Overwrite after READY:** re-POST to staging with the still-valid grant. Every variant and `original` still return the validated bytes, for attached and unattached media.
4. **A valid grant aimed at a final key:** refused, before and after READY (G1). The fake fails the test on any such write.
5. **HEAD disagreement around `image/info`** (injected): no READY, no rejection, operator-visible error, retry recovers.
6. **Copy succeeded, response lost:** next attempt uses a new key, READY names that, and the old key is swept.
7. **Store success, READY commit fails** (forced): nothing READY; retry yields READY once; exactly one key survives the sweep.
8. **READY commit unknown:** retry returns READY with no new copy when it had committed.
9. **Two concurrent completes,** with staging changed between them: one outcome, naming the bytes it inspected; the loser's key is swept; the survivor is never deleted.
10. Attaching and viewing never build a key from input: static check, and an altered id or key gets 404.
11. Staging is never readable: no signed URL names it, for anyone, in any state.

**Upload and lifecycle**
12. Grant, upload, complete: READY, with facts from the store, not from the client.
13. Replayed grant key: same id and original expiry. After expiry or any outcome: no form.
14. Grant over 20 MB, wrong type, 21st open grant, over quota: refused, nothing inserted.
15. Complete before upload: `MEDIA_NOT_UPLOADED`, no attempt used. Store down: 503, retryable, **never rejected**.
16. Size mismatch, PNG declared as JPEG, text renamed `.jpg`: REJECTED, plan, deleted. A sixth attempt: refused.
17. No database lock held across any store call; held calls block nothing else.
18. Media disabled: `uploads_enabled=false`, 503 on upload, reads fine, job writes nothing.

**Cleanup**
19. **Late upload after cleanup:** reject, first pass deletes, upload arrives while the grant is valid: the sweep removes it. `complete` refuses throughout.
20. **Late upload after the sweep** (simulated): never promotable, never readable, reported, removed by the staging rule.
21. **Copy landing after a plan:** held through plan commit, released: the sweep removes it, and step 7 refuses.
22. After READY, a staging upload arriving is swept and **the READY key is untouched**.
23. Abandoned grant: passes complete only after `sweep_not_before`. A crash between plan and passes: next tick finishes. Delete of an absent key succeeds.
24. **Attachment racing cleanup, both orders,** using `pg_blocking_pids`: never a memory pointing at a planned or deleted object. A referenced object: the guard refuses the plan, even by direct SQL.

**Deletion**
25. Uploader deletes, including from the partner's memory: API reads stop at commit, for both members. Partner cannot. Another couple: 404.
26. Memory creator deletes; partner's request needs approval. Text is hidden at once and erased after the window; other tables unchanged.
27. Undo within the window restores exactly; by anyone else, refused. **Undo racing purge, both orders.**
28. Purge deletes all keys, and text through the one function, which refuses a target with no tombstone or inside its window. Metadata remains.
29. A pending proposal naming a deleted photo cannot be approved. A memory with all photos deleted shows a placeholder.
30. **Restore drill case, in test form:** restore a database copy from before a deletion, replay the journal: the photo is not served.

**Isolation**
31. Another couple's member: complete, attach, view, content, proposal, decision, delete all 404, with no store call and no signature.
32. Attaching another couple's media through direct SQL: FK refuses.
33. Anonymous: 401. Partner completing the uploader's media: refused. Partner viewing an unbound draft: 404.
34. The 302 is `no-store`; the URL expires; altering the transform fails (stub).
35. A signed URL or policy never appears in any log or audit row. Service worker does not cache `/api/v1/media/*`.

**Memories and editing**
36. Create with several photos: order kept; one to nine bound; zero, ten, duplicates, not READY: refused and nothing partial.
37. Timeline order with equal dates; pagination; Beijing date boundary with no UTC shift.
38. Creator edit: new version; old revision and photos kept. Wrong version: conflict.
39. Partner direct edit: refused. Partner proposal: original and suggestion both visible; content unchanged until approval.
40. Approve: revision equals the proposal exactly. Reject, withdraw: nothing changes. Replay of each: appends nothing.
41. Creator edit while pending: refused. Approve against reject, concurrently: one decision.
42. Same-key create retried with the response lost: one memory, one notification.
43. Notify on and off, remembered per the 0020 rule. Push payload holds no text. `memory` accepted by the worker's closed set.

**Client**
44. Account switch mid-upload and mid-read: nothing crosses accounts, late responses dropped.
45. Unknown save: same key and body on user retry only. Offline: blocked, no queue.

**Real, at P3-1d only, owner-authorized**
46. **Each of G1-G7 proved,** with the failing request recorded for each refusal, including a grant replayed against a final key, and copy atomicity under overwrite as far as it can be observed. Whatever cannot be observed is reported as such, for an owner decision.
47. Section 6's gates with real phone photos: tall, wide, rotated, over 4096 px, with GPS. Variants upright and without GPS. iOS and Android upload.
48. Public URL refused. Expired signed URL refused. CORS from another origin refused. Staging rule removes a test object; `media/` is untouched.

## 17. Vendor facts to confirm at P3-1d

**Superseded for planning by [phase3-p31d-oss-plan.md](phase3-p31d-oss-plan.md)** (2026-11-03), which rechecks these against current documentation and sets the tests. This table is kept as first written.

Read from documentation only; versions disagree in places. G1-G7 are in Section 4.2.

| Item | Status |
|---|---|
| PostObject V4 policy, `content-length-range`, exact conditions | Documented |
| Policy expiry against a request in flight | **Not found** |
| CopyObject; `x-oss-copy-source-if-match` (412); `x-oss-forbid-overwrite` | Documented, on an older page. **Not relied on** |
| Copy atomicity, read-after-write consistency | **Not found.** G3 and G4 depend on them |
| RAM policies separating staging and final by prefix | **Not checked** |
| Lifecycle rule by prefix | **Not checked** |
| IMG source formats, 20 MB, 30000 px, 250 million pixels, HEIC and rotation at 4096 px | Documented; pages differ |
| `image/info` as a decode check | Documented as a diagnostic |
| Auto-orient over 4096 px; EXIF in output; WebP output of every source | **Not found** |
| Default-domain behaviour for images in `<img>` | **Not checked** |
| Signing `x-oss-process` and response overrides in a V4 URL; time-bucketed signing | **Not checked** |

## 18. Combined release dependency

From the verified production baseline of **0024** [Owner]:

```
production 0024 -> 0025-0028 (Phase 2 reminders, frozen) -> 0029 (P3-1a, frozen) -> 0030+ (Phase 3) -> build with everything -> web
```

**Hard order, from recorded failure modes:**
- A build with R1 needs 0025 at startup; with R4, without 0027, task-edit approvals fail; with R5, without 0028, an enabled or paused dispatch tick fails closed ([phase2-reminders.md §20.6, §21.7](phase2-reminders.md)). A build with P3-1a refuses to start without 0029, as verified. **So all migrations go before the build.**
- Apply while Push control is disabled ([§21.7](phase2-reminders.md)). **[Owner] to confirm production's current control state.**
- API and web ship together ([§22.3](phase2-reminders.md)).
- Caddy CSP changes ship with the web that needs them; `MEDIA_ENABLED` and `REMINDERS_ENABLED` stay off until their live checks.

**Gates for the combined release:**

| Gate | Status today |
|---|---|
| Phase 2 browser and device acceptance | Pending |
| Reminder live generation check | Pending |
| Real Push acceptance, if Push is on | Pending |
| The unexplained `db_gated` abort | Open |
| P3-1, P3-2, P3-3 verified, including P3-1d | P3-1a-c verified with fakes; P3-1d basic flow only (Section 24.1); P3-2a and P3-2b verified with a fake store only, P3-2c web with fakes only ([Section 13](phase3-p32-albums-design.md)), P3-2d scoped live rehearsal completed ([Section 14](phase3-p32-albums-design.md#14-p3-2d-live-rehearsal)); P3-3a and P3-3b backend passed isolated verification; P3-3c passed web tests and isolated HTTP probes, manual browser acceptance pending |
| Store guarantees G1-G7 proved (P3-1d) | Basic flow passed on a test bucket; **the guarantees are not proved** (Section 24.1) |
| Bucket hardening: private, block public access, encryption, CORS, separated RAM identities, staging lifecycle rule | Not started |
| **Deletion policy (4.8) implemented and tested, before any real photo** | Photo and whole-memory deletion backend implemented with isolated fake-store tests; M2 frontend automated checks passed. Browser/device and external journal integration remain open |
| **Verified database-and-object restore drill (4.9)**, including a deletion after the backup | Not started; D5 strategy open |
| **Rehearsal 0024 → final:** disposable database at 0024 with synthetic representative data, applying everything, then the final build, then rollback pairing | Isolated 0024 → 0053 schema upgrade and post-upgrade replica restore passed. Application build pairing and rollback acceptance remain open. See [database upgrade rehearsal](database-backup-tools.md#rehearse-a-database-upgrade-from-migration-0024). |
| **Old build on the new schema,** if the release has any window where it runs | **Never tested**; [§21.7](phase2-reminders.md) only reasons about it |
| **Old-build album rehearsal (Section 18.1),** before any rollback relies on it | Not started; design only |

**Rollback:** migrations are additive and are not reverted. The media objects written after release have no meaning to an older build, which does not list them. **That is not the whole answer once albums exist: see below.**

### 18.1 Old-build compatibility risk: removed album photos

**[Fact]** A build with P3-1 media but without P3-2 decides partner access from `bound` alone ([MediaService.cc:51, :202](../services/api/src/services/MediaService.cc)). Run against a schema that has album rows, it would treat a **removed** album photo as readable by the partner, and sign new URLs for anyone holding its ID. It would ignore `album_item_removals` entirely.

| Build | Exposure ([Fact] from the code; **[Unverified]** until rehearsed) |
|---|---|
| With P3-1 media, without P3-2, media enabled | **Exposed.** New signed URLs for removed album photos |
| Same, `MEDIA_ENABLED` off | Bytes safe: `content` answers disabled and signs nothing ([:932](../services/api/src/services/MediaService.cc)). `facts` still answers |
| Before P3-1, such as the build at production's 0024 [Owner] | No media routes, so no such read. Whether it starts and behaves on the final schema is the open gate above. **The production build itself was not inspected** |
| With P3-2 | Not exposed |

**Release rule until the rehearsal says otherwise:** after albums have been used, do not roll back to a build with media but without P3-2 with `MEDIA_ENABLED` on. A rollback with media off, or a roll forward, is the path. Since Phase 2 and 3 release together, no such build should exist in production. The rule covers test stacks and any later partial rollback.

**Rehearsal, required before the combined release.** Disposable instance only, synthetic data, fake or test store:

1. Bring a database from 0024 to the final schema, using the existing rehearsal (the gate above).
2. With the final build, create an album, add photos as two accounts, remove one, keep one active.
3. Stop it. Start each candidate rollback build against that database: the production-baseline build, and a build with P3-1 media but without P3-2.
4. For each: does it start; health and existing features; as the partner, `facts` and `content` for the removed photo, the active photo and a memory photo, by ID; cleanup job behaviour on album bindings, with media on and off.
5. Return to the final build: rows unchanged, removal still in force, no duplicate writes.
6. Record results per build, and confirm or replace the release rule above.

## 19. Risks

| Risk | Note |
|---|---|
| Vendor assumptions fail | Stage P3-1d exists to find this. P3-1a-c are written behind an interface, and Section 6 gives the order of fallbacks |
| Photos are irreplaceable | Hence the drill, the guard on deletion, the recovery window, and no lifecycle rule on `media/` |
| A store guarantee cannot be proved | Real integration stays off and the design returns for review. Copy atomicity may be only partly observable; that goes to the owner |
| Extra copy per upload | One server-side copy and up to two transient objects per photo. Cost unpriced until D1 |
| A deletion is reversed by an old restore | The journal exists for this. It lives outside PostgreSQL, so it must itself survive the failure being restored from |
| Cost | Bounded by Section 4.3. IMG volume and storage are unpriced here: check with D1 |
| Long release batch | Everything ships at once. A failure late in Phase 3 holds Phase 2. The owner has accepted this |
| Timeline pagination | Ordering by a mutable date means an edit during paging can skip or repeat a row. Client deduplicates; accepted at couple scale |
| `0030` touches `notifications` again | Additive, same pattern as 0020 and 0025, but it needs full ledger regressions |

## 20. Sources

- [PostObject](https://www.alibabacloud.com/help/en/oss/developer-reference/postobject)
- [POST V4 signature](https://www.alibabacloud.com/help/en/oss/developer-reference/signature-version-4-recommend)
- [Restrict file type and size during uploads](https://www.alibabacloud.com/help/en/oss/how-do-i-limit-object-formats-and-sizes-when-i-upload-objects-to-oss)
- [Prevent overwriting with x-oss-forbid-overwrite](https://help.aliyun.com/en/oss/developer-reference/prevent-objects-from-being-overwritten-by-objects-that-have-the-same-names-3)
- [IMG parameters, methods and limitations](https://www.alibabacloud.com/help/en/oss/user-guide/overview-17/)
- [Limits and performance metrics](https://www.alibabacloud.com/help/en/oss/product-overview/limits)
- [CopyObject](https://www.alibabacloud.com/help/en/oss/developer-reference/copyobject)
- [copy-object (ossutil)](https://www.alibabacloud.com/help/en/oss/developer-reference/copy-objects-by-calling-an-api-operation)

## 21. P3-1a implementation and isolated verification (Claude Code, 2026-11-03)

Owner authorization: P3-1a with the fake object store only. **NOT DEPLOYED.** No vendor, bucket or vendor credential was used, and no real store adapter exists. User deletion, recovery periods and the external deletion journal (Section 4.8) are **not implemented**; they wait for D6 review. D1 and D5 stay open.

### 21.1 State, stated exactly

| Item | State |
|---|---|
| `0029_media` | **FROZEN**, sha256 `ab17943a8c76fe34b5afcfb8580174c2abd3da9b448f3c000dcb8586de003e78`. Next number confirmed free before writing. First committed in disposable project `usapp-p31a-20261103170923-1e97f4`. **Not** on shared `usapp_test`, or production. 0000-0028 unchanged, by checksum. Further corrections need a new migration |
| Contract | 0.24.0: four media routes |
| Real processes | `main.cc` configures no store, so every real process answers `MEDIA_DISABLED`, and the job writes nothing. **A build with this batch needs 0029 at startup:** the privilege policy refuses otherwise, which was exercised |
| Web | Unchanged (P3-1c) |

**Changes from Section 7's sketch:** lock 33019 is per couple, for grant limits, so a memory lock in P3-1b takes 33020. `media_deletions` and its reversals, and `memory_texts`, are not created. `media_late_objects` and guarantee G8 are new. A `media_bindings` row has no route yet; P3-1b adds it and a target FK.

### 21.2 The fence, and late final objects

**Once a cleanup plan exists, nothing can publish READY.** The fence is in the database guards as well as the service: `USAPP_MEDIA_OUTCOME_CLEANUP` and `USAPP_MEDIA_ATTEMPT_CLEANUP`, under lock 33018, so direct SQL is refused too.

**A late final object** is one that appears after its plan was swept: a copy that was delayed, or reported failure and took effect later. **It can never be read:** a signed read is only ever made for the key a READY outcome names, so until removal it is a storage cost, not an exposure.

`usapp_media_key_disposition(key)` is the one definition:

| State | Meaning | Action |
|---|---|---|
| `keep` | The READY object | Never removed |
| `live` | No outcome and no plan: something may still be running | Left alone |
| `pending_sweep` | Dead; the plan's sweep will remove it | Sweep |
| `late` | Dead, and the sweep has already run | Removed and recorded |
| `unknown` | No row names it | **Reported, never removed** |

Dead is final, because a plan blocks every later attempt and outcome, and nothing removes a plan. So reading a state and then acting on it is safe.

**Detection and recovery have two paths:**
1. **The attempt itself, if it resumes.** After its copy it reads the state, and again under the lock before publishing. Finding a plan or an outcome, it removes its own key if dead, records `removed/attempt_resume`, and returns the stored state.
2. **The reconciler, if nothing resumes.** Each `media.cleanup` tick lists one page of `staging/` and of `media/`, and removes what the function calls `late`, recording `removed/reconcile`. Revision 2 called reconciliation report-only. This narrows that: it removes only keys the database has already declared dead, and still only reports keys it does not know.

A staging object uploaded after a sweep takes the same path, since `complete` refuses once any outcome or plan exists.

### 21.3 Verification

`Claude outputs/p31a-isolated-verify.ps1`, final project `usapp-p31a-20261103170923-1e97f4`, RESULT: PASSED, exit 0. Generated credentials only; no ports; PostgreSQL on tmpfs. Logs stream to `%TEMP%\<project>\`. Direct executables have a hard 30 s deadline, the CTest limit, and the CTest command 120 s. **No deadline fired, so that branch is still unexercised.** No test timeout changed, and rounds did not overlap.

| Check | Result |
|---|---|
| Representative data, 0000-0028, previous image | `db_gated` 3073 and `push_dispatch` 532 assertions, 0 failed: 81 deliveries, 104 attempts, 311 occurrences, 464 tasks |
| New API binary without 0029 | Refused to start, exit 1 |
| Rollback prevalidation | 53 cases as `postgres` and `usapp_api`, exit 0. Afterwards: not registered, no tables, rows unchanged |
| Apply, InitRoles and privilege deviation, twice | Exactly 0029; 0 deviation rows both times; repeat skipped 30, applied 0 |
| Checksums and existing rows | 30 registered, all equal local files; existing rows unchanged |
| Targeted first | Media SQL, 53 cases per role; media suite 215 assertions, 0 failed, exit 0, 10 s |
| **Real API binary, real cookie, media disabled** | 34 assertions, 0 failed, exit 0: no CSRF 403; anonymous 401; 503 `MEDIA_DISABLED` on upload, complete and content; unknown id 404; bad variant 400; nothing written |
| Full CTest, 4 targets, unchanged limits | 100%, exit 0, 47.48 s: unit 2213, 11.20 s; db_gated 3288, 21.12 s; push_dispatch 541, 15.14 s; static_check 552, 0.01 s |
| SQL scripts | Dispatch in both modes; media, reminder, notification, choice, anniversary and task ledgers in both roles, all ROLLBACK |
| After the run | Control disabled; 0 credentials in logs |
| Strict YAML, `Claude outputs/p31a-logs-yaml.log` | Negative control refused; contract 0.24.0, 58 paths, 67 operations; exit 0 |

`db_gated` now takes 21.12 of its 30 s, mostly deliberate sleeps past short grants. **That margin has narrowed**, and P3-1b should shorten these or use a separate target rather than raise the limit. The summary prints the targeted result a second time after stage J; that is the same run's stored output repeated by the script, not a second execution.

**The media suite covers, with the fake's fault injection:**

| Required case | How |
|---|---|
| Copy succeeds but reports failure | Known key left `live`; retry uses a new key; READY names it; sweep removes the lost copy and staging, not READY |
| Concurrent completion | Two held copies, staging changed between them: one outcome naming bytes it inspected; both callers told READY; loser's key gone, winner stays and serves those bytes |
| Attachment racing cleanup, both orders | `pg_blocking_pids`: each waits on 33018, then the second refuses. Bound objects are never planned, by the job or by direct SQL |
| **Delayed attempt resuming after cleanup swept its key** | Held in the copy, and held at inspection, both released after the sweep: never READY, key removed. A third, past inspection with valid bytes, is refused at commit. Direct SQL is refused too |
| Late final object, nobody resuming | A copy that reports failure and lands after the sweep; a stopped process whose copy lands later; found and removed by the reconciler. The winner is untouched |
| Overwrite between inspection and finalization, and after READY | Staging changed at every step after the copy, and re-uploaded with the live grant: all variants serve the validated bytes |
| Late upload after cleanup | Before the sweep, and after it |
| Others | Grants and replay; a grant aimed at a final key; outage never a rejection; rejection reasons from bytes; store success then forced database failure; five failed attempts ending; a final key changing during inspection; no lock across a store call; same-couple isolation with no store call for a foreign request; pass failure repeating; unknown keys only reported. The fake also fails on a second write to any final key, and on any signed read of staging |

**Failures before the pass:** a compile error, `usapp-p31a-20261103170229-8296ee` (a conditional made a `const std::string` rvalue, which Drogon's binder rejects), nothing created. Prevalidation exit 3, `usapp-p31a-20261103170313-9e8398` (my SQL test put an out-of-order attempt in a CHECK list, though the guard fires first), nothing committed, and the script's post-rollback check did not run on it. It ran and passed in `usapp-p31a-20261103170625-482444`, which stopped before applying. **0029's bytes never changed after commit.**

**Not run:** real OSS, real worker or browser, web tests (web unchanged), mutation testing. The `db_gated` abort did not recur and remains unexplained.

### 21.4 Unresolved guarantees: P3-1d release gates

**Everything below is assumed by the fake and unproved for any vendor.** The real adapter stays off until each is proved or the design is changed.

| # | Reliance | If it does not hold |
|---|---|---|
| G1 | A grant writes only its exact staging key. **Expiry of requests in flight is not documented** | Sweep timing is only a cost; another key would break isolation |
| G2 | No client credential writes a final key; identities are separated by prefix | **READY's immutability fails** |
| G3 | Copy is atomic and one version | A partial object could be inspected and pass |
| G3b | **The adapter sends each copy once, with no automatic retry at any layer**, and enforces a timeout | A retried copy could overwrite a READY key with newer staging bytes. The real client must be checked for this specifically |
| G4 | HEAD and `image/info` read a finished copy | Spurious 503s, or inspection of the wrong state |
| G5 | ETag changes with the bytes | The bracketing check detects less |
| G6 | Delete is idempotent | Passes repeat |
| G7 | A signed read names one key, one variant, read only | Arbitrary transforms or keys |
| G8 | **Listing eventually shows every object** | **A late object with nobody resuming is never found**, and stays as a cost |
| T1 | `sweepGraceSeconds` (1 hour) outlasts almost all late writes | More objects fall to the reconciler; no exposure |
| T2 | How late a copy that reported failure can take effect is **unknown, and nothing here depends on a bound** | Covered by the reconciler for as long as it runs |
| T3 | Staging lifecycle rule (Section 4.7) | Not created. Late staging objects rely on the reconciler alone |
| T4 | Every store call returns or times out | **One round of background work is held at a time, so a call that never returns stops cleanup until restart.** The adapter must bound each call |

**Other known limits:**
- READY's bytes are not re-verified after publication. Nothing detects a READY object changed or lost by something outside this design, until the restore drill compares CRC64. A periodic check is not in this batch.
- Reconciliation markers are in memory: a restart begins the scan again, which is harmless, and scales with object count.
- An unknown key is recorded once, by key.
- `RESULT_UNKNOWN` on an outcome commit is coded for, but no test injects it. A forced failure before commit and replay after commit are tested.
- The 201 and 302 success paths have not run through the real HTTP layer, because no real process has a store. They are covered at the service level.
- `sweep_not_before` is set by the service, and the guard does not repeat its formula. An early sweep only produces more late objects.
- The earlier release dependency stands, now through 0029 (Section 18). Docker's default address pools remain full; this harness picks a random internal subnet per run.

### 21.5 Resources

Created by these runs, stopped and retained, nothing removed: containers and internal networks of `usapp-p31a-20261103170313-9e8398`, `-170625-482444` and `-170923-1e97f4`; image `usapp-isolated-test-runner:p31a`. The first failed project created no network. Shared and production databases, existing services and real Push were not contacted.

## 22. P3-1b implementation and isolated verification (Claude Code, 2026-11-03)

P3-1b adds the photo memories backend: create, paginated timeline, detail, creator edits, and partner edit requests with approval, rejection and withdrawal. Everything ran in disposable isolated instances against the fake object store. **NOT DEPLOYED.** No web UI (P3-1c), real OSS (P3-1d), user deletion (D6) or notifications.

### 22.1 State, stated exactly

| Item | State |
|---|---|
| Migration | `0030_memories` FROZEN by its first committed application, in `usapp-p31b-20261103175250-945d50` only. sha256 `ea55569491ce00dcb66fd33a2acbdade0ce5af65e953ff8dd5e9b42835a924ff`. 0000-0029 unchanged. Not on shared `usapp_test` or production |
| Contract | 0.25.0: seven operations on six memory paths, eight `MEMORY_*` codes, `uploads_enabled` on the timeline |
| Build | Needs 0030 at startup: the privilege policy refuses without it (measured, exit 1). `MediaService` also calls its functions |
| Limits | Reviewed values: 1-9 photos, title 1-60 and description up to 2000 code points, date 1900-01-01 to today in Beijing |

**Changes from Sections 7, 8 and 10's sketch:**
- No `kind` on proposals and no deletion tables: both wait for D6.
- A proposal's new photos are not bound, which Section 7 implied ("first binding here"). See 22.2.
- Route `/revisions`, as anniversaries, not `/edit`. A moved version answers `MEMORY_VERSION_CONFLICT`, as anniversaries; there is no `MEMORY_PROPOSAL_BASE`.
- `media_bindings.target_id` has no FK, since the target kind is meant to grow (P3-2). Section 21.1 said "a target FK". The binding guard requires the target instead, and memories are append-only, so the target cannot disappear.
- No `33012 preference` step and no `notify_partner`: no notifications in this batch.
- `memory_texts` exists as sketched, but has no erase path yet, because that is D6.

### 22.2 Photo references: bound, reserved, released

The assignment asked how a rejected or withdrawn proposal releases its pending references without making history eligible for cleanup. The answer is two different kinds of reference.

| State | Meaning | Cleanup | Who reads it |
|---|---|---|---|
| Unreferenced | READY, nothing names it | After 7 days | Uploader |
| **Reserved** | Named by an undecided proposal; only a `memory_proposal_photos` row | Refused (`USAPP_MEDIA_PLAN_RESERVED`) | Uploader, and the partner reviewing it |
| **Bound** | A revision used it; `media_bindings` row, forever | Never, even once a later edit drops it | Both members |
| **Released** | Its proposal was rejected or withdrawn | 7 days after that decision | Uploader again |

- **Binding happens only at approval**, in the transaction that inserts the decision, the revision and its photos. So nothing has to be undone on rejection: deciding the proposal is the release, and it writes no row about the photo.
- **History is safe by construction.** A revision's photos are always bound, and the plan guard refuses a bound object, so a rejection cannot touch anything a revision references. The binding row outlives the edit that drops the photo.
- **`usapp_media_released_at(media)`** is the one definition of the clock, used by both the guard and the job: the later of READY and the last decision on a referencing proposal, or NULL while bound, reserved or not READY. So a photo released today is not taken today just because it became READY long ago.
- **0030 replaces two 0029 function bodies** by `CREATE OR REPLACE`, not the file. The script checks after applying that every earlier refusal is still present. The binding guard now also requires a real memory of the same couple, an entitled actor (the uploader, or the approver of a proposal naming it on this memory), and no reservation on another memory.

**One limit follows:** a proposal nobody answers reserves its photos indefinitely. It is the partner's own photo and the proposer can withdraw, so this is not treated as a defect, but no timeout exists.

### 22.3 Locks and atomicity

**Order:** member `FOR SHARE` → 33020 memory → key classification → role and state checks → sorted 33018 media → inserts → audit → commit. Media completion and cleanup hold only 33018 and never 33020, so the order is one-way. A create has no memory lock, because its id is new; its photos serialize it. If a same-key create took the photos first, the loser re-classifies and replays rather than reporting them unavailable.

**Approval, in one transaction:** decision, then revision equal to the proposal (guard), then bindings by the approver, then revision photos, then audit. Deferred checks refuse an approval without its revision and any revision or proposal without 1-9 contiguous photos. A replay is classified first and writes nothing.

**A committed revision takes no more photos:** the guard requires the revision row's `xmin` to be the current top-level transaction. This fails closed inside a savepoint, which the service does not use.

### 22.4 Verification

Project `usapp-p31b-20261103175250-945d50`, `Claude outputs/p31b-isolated-verify.ps1`. Logs are under `%TEMP%\usapp-p31b-20261103175250-945d50`.

| Stage | Result |
|---|---|
| Build | `A-build` exit 0 |
| Base data from the P3-1a image | `db_gated` 3288 assertions, `push_dispatch` 532; both exit 0 |
| New API binary without 0030 | Refused to start, exit 1 |
| Rollback-only prevalidation, in two roles | Memory ledger 64 cases, media ledger 53, each as `postgres` and `usapp_api`, exit 0. Persisted nothing: not registered, no tables, no new function, rows unchanged. Ran in project `-174927-6b5cd5` and again in the full run |
| Apply 0030 | Exactly 0030, exit 0 |
| Grants | 0 privilege deviations after applying and after repeat InitRoles (31 skipped, 0 applied) |
| Checksums | 31 registered, each equal to its local file; existing rows unchanged |
| Replaced guards | Plan guard 5 and binding guard 7 expected refusals and locks present |
| Targeted | Memory suite 260 assertions in 1 s, media suite 215 in 10 s, memory and media SQL in both roles; all exit 0 |
| Real API binary | 51 assertions, exit 0: memory routes through the real filter and controller, `uploads_enabled` false, media still disabled |
| Full CTest | 4/4 in 48.60 s, exit 0: `unit` 2322 (11.21 s), `db_gated` 3548 (**21.75 s** of 30 s), `push_dispatch` 541 (15.63 s), `static_check` 563 (0.01 s) |
| SQL, both roles | Dispatch, memory, media, reminder, notification, notification choice, anniversary, task; all exit 0 with ROLLBACK |
| Strict YAML | Negative control refused; contract 0.25.0, 64 paths, 74 operations; exit 0 |
| Abnormal terminations | None; the `db_gated` abort did not recur |
| Credentials in retained logs | 0 |

**What `MemoryDbTests.h` covers**, with the real service, media service and job:

| Required | How |
|---|---|
| Cross-couple access | Every route as the other couple, a non-member, either couple's photo, and direct SQL |
| Owner and partner permissions | Each member attempts the other's edit, propose, approve, reject and withdraw |
| Attachment against cleanup, both orders | Plan first: a held plan transaction makes create and propose wait, then refuse, leaving nothing. Attachment first: holding the higher photo lock pauses the service between its two locks, a plan then waits behind it and is refused. Run for create, proposal and approval |
| Concurrent proposals and decisions | Two proposals; edit against proposal; approve, reject and withdraw at once, four rounds; same key concurrently for create and approve |
| Rollback on a failed binding | An injected fault after the revision, before binding: no decision, revision, binding or audit; still pending; the same key then succeeds |
| Approval replay | No new revision, binding or audit, including after later edits; a replay reports the original version |
| Stable pagination | Ties split across pages, an insert and an edit during the walk: no repeat, no skip |
| Lock order | Holding 33020, completion, the job and reads finish while the edit waits; opposite photo orders, five rounds, no deadlock |
| Release clock | Committed history aged 8 days is planned; 3 days and pending are not, though READY is older |

`db_gated` gained about 1 s, within its limit, so **no separate test target was needed**. Its margin is thin (8.25 s) and is mostly earlier suites' sleeps.

**Two script notes.** After the run, 0030 was added to the script's frozen-hash map, which only adds a check; that edit has not been rerun. The first prevalidation attempt failed for a script reason: the two test files shared one transaction, and the first left constraints immediate. A reset was added between them; no migration or test logic changed.

### 22.5 Limits and open items

- **Notifications are left for review.** They need a new event family in a further migration and a change to `TARGET_KINDS` in `services/push-worker/src/deliver.ts:43`. Neither was made; a static check holds the worker unchanged.
- **A failed binding is tested by fault injection.** At approval the guards should make a real binding refusal unreachable, since reservations already exclude cleanup. The guard refusals themselves are tested in the SQL ledger. The eligibility check on a photo list refuses before any write, so it does not exercise rollback of written rows.
- **Pagination:** an edit that changes a memory's date moves that row, so a walk in progress can show it twice or miss it. Stated in the contract. Order by identity, as anniversaries do, was rejected because a timeline must sort by memory date.
- **Records outlive release:** a rejected proposal keeps its text and photo ids, and its dimensions, in the review record, though the partner can no longer read the image. Text is not erasable until D6.
- **Bound is permanent until D6:** no one can remove an attached photo. This is why D6 stays a release gate (Section 4.8).
- **Not exercised:** real object store, browser, device and live workflows; no web client calls these routes. The tests were not mutation-tested. The harness deadline branch has still never fired.
- **Web checks were not run:** no web file changed.
- **Open as before:** the earlier `db_gated` abort is unexplained, P3-1d gates G1-G8, G3b and T1-T4, decisions D1, D5, D6a-d and others. Phase 3 stays one combined release, now through 0030 (Section 18).

### 22.6 Resources

Created, stopped and retained, nothing removed: containers and internal networks of `usapp-p31b-20261103174537-4b42d0`, `-174927-6b5cd5` and `-175250-945d50`; image `usapp-isolated-test-runner:p31b`. Each run's postgres was on tmpfs. Shared and production databases, existing services, real Push and any vendor were not contacted.

## 23. P3-1c implementation and verification with fakes (Claude Code, 2026-11-03)

P3-1c adds the photo memories frontend: paginated timeline, detail, create and edit with ordered photos, partner requests with approval, rejection and withdrawal, and the upload flow. Web only. Backend, contract, migrations, dependencies and deployment configuration are unchanged. **NOT DEPLOYED.** No web build was served. No real API, real store, device or shared data was involved.

### 23.1 What was built

| Area | Files | Behaviour |
|---|---|---|
| API | `src/api/media.ts`, `memories.ts`, `memory-rules.ts` | Contract 0.25.0 calls, response validators, client-side rule mirror |
| Memory state | `src/stores/memories.ts` | Modelled on the anniversary store: identity generation, read sequences, one action per target, captured key and payload |
| Upload state | `src/stores/memory-uploads.ts` | Grant, staging POST, completion; at most 3 transfers; no offline queue; every retry a click |
| Views | `MemoriesView`, `MemoryNewView`, `MemoryDetailView`, `MemoryEditor`, `MemoryPhotoView` | Routes `/us/memories`, `/us/memories/new`, `/us/memories/:id`, under the "us" navigation entry, linked from `UsView` |

`checkTaskTitle` and `normalizeTaskDescription` gained an optional limit; their task defaults are unchanged.

**Upload states.** Each failure has its own safe retry, which is the reason they are separate:

| State | Means | The retry |
|---|---|---|
| `grant_unknown` | The grant may exist | Same key and body, which replays it |
| `interrupted` | The POST did not finish, or the store says nothing arrived | Re-POST the same form while it is valid |
| `unconfirmed` | The POST finished; completion has no answer | Complete the **same media id**. No grant and no POST |
| `expired` | The form or grant has ended | Only an explicit "upload again", with a new key |
| `rejected` | The bytes were inspected and refused | None: terminal |
| `failed` with `local` | Wrong type or size, refused here | None: nothing was sent |

**Attachment:** a photo joins a submission only once the backend has answered READY, or it is already this memory's. There is no local preview, because nothing before READY is displayed.

**Replay:** a retry resends the captured key and payload. A success must match the captured action. The returned memory may be newer than the confirmed mutation, because a replay carries today's memory, but never older (Section 23.6); content is compared only at the mutation's own version. An approval replay therefore shows its original facts and the current state, and writes nothing. If a re-read already shows the approval applied, the unconfirmed action remains retryable on the page.

**Private material:** the file, signed form and key live in a plain `Map` outside reactive state, and are dropped once the outcome is decided. Nothing is written to storage or the console. The staging POST uses `credentials: 'omit'`, `no-referrer`, `no-store` and `redirect: 'error'`, and its errors carry no URL or field. Photos are `<img>` on the same-origin `/api/` route, which the service worker never caches; script never sees the signed URL.

### 23.2 Verification

**Automated (Vitest, fake API and store):** exit 0.

| Check | Result |
|---|---|
| Focused: `tests/unit/memories.spec.ts`, `media-client.spec.ts` | 61 and 5 tests passed |
| Full web suite | 33 files, 661 tests passed |
| `vue-tsc --noEmit -p tsconfig.json` | Exit 0 |
| `tsc --noEmit -p tsconfig.node.json` | Exit 0 |

| Required | Covered by |
|---|---|
| Disabled uploads | Flag false, flag missing, flag unreadable, and `MEDIA_DISABLED` mid-flow: memories stay readable, the reason is stated, no add route is offered, existing photos stay editable |
| Malformed responses | List, later page, detail, review, write, grant, form and completion: each is an error or unconfirmed state, never a success |
| Partial upload failures | Each state above, each retry's exact calls, and a failed upload blocking submission until removed |
| Account switching | Real identity path clears both stores; late list, write, grant and POST results are dropped; the POST is aborted |
| Route changes | Late detail and review answers dropped; a form's uploads forgotten on leaving; an unconfirmed create survives leaving and returning, locked to its capture |
| Pagination | Cursor passed unchanged, a moved row deduplicated, a failed or malformed later page keeps the first and resumes from the same cursor |
| Edit permissions | Creator edits, partner requests, approve, reject and withdraw per role, nothing writable while pending |
| Approval replay | Same arguments on both calls, one notice, the replayed version with today's memory; and after a re-read |
| Cache exclusion | The shared policy for the route, signed URL and POSTs under each destination; sensitive route registered first in `sw.ts`; no storage or console call in a full flow; no such call in these sources |

Writing the tests found one real defect, now fixed: an abort's `DOMException` is not an `Error` instance everywhere, so an aborted POST could have been reported as a failed upload.

**Browser (separate from the above):** the real app on Vite dev, in the built-in browser at 375 px and desktop, against a throwaway fake API and store on 127.0.0.1 kept outside the repository. Fake data only.

| Flow | Observed |
|---|---|
| Timeline | 20 rows, then 25 after "load more", and the button gone. Images loaded through the 302. No horizontal overflow at 375 px |
| Interrupted, then unconfirmed | "继续上传" re-POSTed the same media id; "重试确认" completed it; submitting before READY was refused with a message |
| Create | Opened its detail page, with text and photo correct |
| Partner request | "申请修改" only; memory unchanged; review showed current and proposed with differences marked; withdraw offered, approve not |
| Approval, answer lost | "结果未确认" shown; retry replayed on the server (fake's log shows the replay); version 2, not 3; one notice |
| Uploads off | Reason shown, no add link, rows still listed and paging |
| Missing photo | Placeholder beside the loaded photo; row and timeline intact |
| Storage | `localStorage` and `sessionStorage` empty; no caches; no service worker registered |

**What this browser run does not show.** Vite dev registers no service worker, so cache exclusion rests on the automated policy tests. There is no CSP in dev and the fake store is same-origin, so neither CSP nor CORS was exercised. Thumbnails were not real variants. Screenshots and DOM reads used test hooks, not a person. Script set the file input, so the native picker was not used.

### 23.3 API gaps, for review (nothing changed)

1. **A proposer cannot see a decision later.** `Memory` has no last decision (anniversaries have `last_decision`) and no route lists proposals, so once it is decided the UI cannot find the record. Only a notice in the same session reports it.
   *Superseded 2026-11-08 by Section 25:* `GET /api/v1/memories/{memoryId}/proposals` (contract 0.37.0) lists every request with its decision, and memory detail shows that history. The other three gaps below are unchanged.
2. **`uploads_enabled` is only on the timeline.** A directly opened editor reads the first timeline page to learn it. A small field on detail, or a dedicated read, would avoid that.
   *Superseded 2026-11-09 by Section 29:* `GET /api/v1/media/capabilities` (contract 0.40.0) answers it directly, and the pickers use it. It is a snapshot; the upload routes still enforce.
3. **No list of the caller's READY unattached uploads.** After a reload or on leaving a form they cannot be recovered, and are removed after their unattached period. Uploading again is the only path.
   *Superseded 2026-11-08 by Section 28:* `GET /api/v1/media/unattached` (contract 0.39.0) lists them, and the memory editor and album adder can choose them again. It is a snapshot, not a reservation.
4. **A broken image gives no reason.** `<img>` cannot distinguish 404, 503 and an expired signature, so all show one placeholder.

### 23.4 Limits and open items

- **Real storage needs deployment work, as Section 4.6 says:** `deploy/Caddyfile` has `connect-src 'self'` and `img-src 'self' data: blob:`, which would block the staging POST and redirected images. Bucket CORS must allow the POST. All P3-1d.
- **Today's real processes have no store:** `uploads_enabled` is false and content answers `MEDIA_DISABLED`, so real memories cannot yet be created.
- **Unconfirmed actions are memory only:** a reload or identity change clears them; re-reading shows whether the write happened. An unknown edit is hidden while a later proposal is pending, until that is decided.
- **A signed URL may sit in the browser's HTTP cache** until it expires, the limit Section 4.6 accepted. The service worker exclusion does not cover that.
- **A reservation or attachment has no removal in the UI:** user deletion is D6. Notifications remain a separate decision.
- **Not exercised:** real API, real store, device, real service worker lifecycle, human acceptance. Backend tests were not rerun, since no backend file changed.
- **Open as before:** Section 22.5's items.

### 23.5 Resources

A throwaway Node process and an in-app browser tab, both stopped; the script is in the session scratchpad. No repository file, container, database or vendor was created or contacted for the browser run.

### 23.6 Review repair: response version regression

**The defect:** `isUsableMemoryResponse` compared content only when the returned memory's version equalled the mutation's, and accepted any other version. So a response confirming version 2 while returning version 1 counted as a success, ended the action, and could overwrite the page with the older copy.

**The repair**, in `apps/web/src/stores/memories.ts`: for create, edit and approved decisions, the returned version must be at least the mutation's. At equal versions, create and edit still compare the submitted terms. A newer version on replay is still accepted. Rejection and withdrawal are unchanged: their mutation has no version, and any current version is accepted.

| Check | Result |
|---|---|
| Memory tests: `memories.spec.ts` 66, `media-client.spec.ts` 5 | 71 passed, exit 0 |
| Full web suite | 33 files, 666 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` | Exit 0 |
| `tsc --noEmit -p tsconfig.node.json` | Exit 0 |
| With the new check temporarily disabled | 3 of the 5 new tests failed, exit 1 (edit flow, approval flow, acceptance matrix). Check restored before the runs above |

The new flow tests prove that an older version leaves the action unconfirmed, with no notice and no change to the page, and that an explicit retry succeeds with the same key and payload.

**Two of the five new tests do not depend on this check.** The create test's older copy is version 0, which the shape check `isMemory` already refuses: a create confirms version 1, so no valid memory can be older. The rejection and withdrawal test pins behaviour that did not change.

**Not rerun:** the browser flows of Section 23.2. No backend, contract, view or dependency changed. The API gaps of Section 23.3, including proposal result visibility, remain separate work.

## 24. P3-1d offline OSS adapter verification

The adapter uses the existing libcurl and OpenSSL stack. The offline tests cover V4 signing, TLS certificate and hostname checks, absolute deadlines, single-copy attempts, response loss, malformed success replies, and read headers that ignore staging metadata. O8 restricts originals to the uploader and allows the partner to view approved variants.

The first completed run passed the targeted checks, but `db_gated` exceeded its existing 30-second limit. The fake-store media cases now run as `media_db` under their own 30-second limit, retaining every assertion. A subsequent run hit a Docker subnet overlap before starting PostgreSQL. The harness now compares IPv4 address ranges and supports explicitly reusing a previously built test image.

Final run `usapp-p31d-20261103193057-000fb3` passed with exit 0. CTest passed all six targets in 64.89 s, including `db_gated` (20.30 s), `media_db` (10.86 s) and `oss_stub` (5.14 s). The HTTPS stub passed 363 assertions, targeted OSS service tests passed 41, and the live API fixture passed 51. Both-role SQL checks passed, all 31 registered migration checksums matched, and there were zero privilege deviations. Push control ended disabled. Complete logs are under `%TEMP%/usapp-p31d-20261103193057-000fb3` on the verification host.

At the time of that run, real OSS conformance, browser upload against the test bucket, and device acceptance were open. Section 24.1 records what has since passed.

### 24.1 Basic real OSS and desktop browser check (owner-reported, recorded 2026-11-04)

**[Owner]** Passed on the dedicated test bucket in Shanghai, on a desktop browser, with a synthetic image: account A uploaded the image and created a memory; account B viewed the photo and submitted an edit request; A approved it. Afterwards the isolated browser stack was stopped, its temporary CA was removed, and the owner deleted the two test objects.

This records the owner's statement in the P3-2 assignment. No log or result from that run was reviewed when writing this, and it was not repeated. `Claude outputs/p31d-browser.ps1` and `p31d-oss-smoke.ps1` are the scripts on disk for such a run.

| Status | Item |
|---|---|
| Shown by this check | One real upload, promotion, cross-account read, edit request and approval, end to end, on one region and one browser |
| **Still open** | Signed URL expiry, overwrite races, late-object behaviour |
| **Not established by this check** | The other guarantees and gates in [phase3-p31d-oss-plan.md](phase3-p31d-oss-plan.md): G1-G8 negatives, G3b, T1-T4, the image checks (real formats, orientation, metadata and GPS), CORS and CSP negatives, the service worker in a production build, phone devices |

**Real photos stay refused** until deletion exists (Section 4.8). `MEDIA_ENABLED` stays off in any release until these close.

## 25. Memory edit-request history (Claude Code, 2026-11-08)

Closes gap 1 of Section 23.3: a proposer could fetch a proposal only by its id, and had no durable way to find a decided request after a reload or a new sign-in. The owner asked to continue product work before consolidated live acceptance. **NOT DEPLOYED.** Real browser, device, cloud and restore acceptance are deferred, not passed and not waived.

### 25.1 Implemented behaviour

**Backend.** New `GET /api/v1/memories/{memoryId}/proposals`, beside the existing POST on the same path. It is an authenticated read behind `AuthFilter`; it writes, locks, reserves and releases nothing.

| Aspect | Behaviour |
|---|---|
| Who | Both current members of the memory's couple. Another couple's member, an absent id and a malformed id get `404 MEMORY_NOT_FOUND`; a user in no space gets `SPACE_NOT_FOUND`; no session gets 401 |
| Deleted or purged memory | Hidden by the same `usapp_memory_deleted_now` predicate as every other memory read, so the list answers 404 exactly like the detail and the proposal read. Under a J3b recovery hold that predicate also hides it |
| Order and pages | `(created_at DESC, id DESC)`, keyset cursor encoded as the recovery lists' (`AlbumService::encodeCursor`). Proposals are immutable, so a walk never repeats or skips one; a newer proposal never enters a walk in progress. `limit` 1-100, default 50 |
| Each item | `id, memory_id, proposer_id, base_version, created_at, status, decided_by, decided_at, approved_version`, read from the same tables and joins as the existing proposal read. No title, description, photo, media id, object key, signed URL, idempotency key or fingerprint |
| Page | `proposals`, `next_cursor`, and `current_version`: the memory's latest version, read by the same statement, so an old approval can be compared with the current state |

No migration: the existing index `memory_edit_proposals_memory_idx (memory_id, created_at DESC, id DESC)` from 0030 serves the query. 0000-0042 are unchanged; the next number stays 0043. **Contract 0.36.0 -> 0.37.0**, additive: `listMemoryProposals` and `MemoryProposalListResponse`.

**Web.** Memory detail gains an "edit request history" card (`修改申请记录`):

- Every request, with pending, approved, rejected and withdrawn labels, who asked, the base version, and who decided when. "Load more" pages through the cursor.
- The card says that a decision is a historical fact and not the memory's current content. An approval names the version it created and says whether that is still the current version or has since been replaced.
- A decided request opens in the existing comparison. Its markup moved unchanged into `MemoryProposalCompare.vue`, which the pending review now also uses. In the history, the original side is labelled as the version the request was based on.
- A pending request points to the existing controls above. The history has no decision controls and no second state machine; approve, reject, withdraw, unknown-result retry and replay are unchanged.
- The store keeps its own history and history-comparison request sequences. Leaving the page, an identity change, a memory that cannot be read, and the caller's own deletion all clear them. Late answers for another memory, route or account are dropped. Nothing is kept in browser storage.
- The history is requested only after the memory itself has read for the current account. A deleted memory therefore never requests its history, text or photos through this card.

### 25.2 Files

- **Backend:** `services/api/src/services/MemoryService.{h,cc}` (`listProposals`, `USAPP_PROPOSAL_LIST_SQL`, `ListResult::currentVersion`) and `services/api/src/controllers/MemoryController.cc` (route and handler).
- **Contract:** `contracts/openapi.yaml`.
- **Backend tests:**
  - New `services/api/tests/MemoryProposalHistoryDbTests.h`, run inside the `memory_deletion_db` target after the erasure suite.
  - `DbGatedTest.cc`: registration only.
  - `ReminderLiveApiTest.h`: the route through the real API binary.
  - `StaticCheckTest.cc`: the route is behind `AuthFilter`, and the query hides deleted memories and reads no text, photo, key or fingerprint. The version checks moved to 0.37.0.
- **Web:**
  - `apps/web/src/api/types.ts`, `memories.ts` and `memory-rules.ts` (summary and page validators, with status-consistency checks).
  - `src/stores/memories.ts`, `src/views/MemoryDetailView.vue`, and the new `src/components/MemoryProposalCompare.vue`.
- **Web tests:** new `tests/unit/memory-proposal-history.spec.ts` (16 tests). Three existing specs only add the new function to their API mocks.
- **Harness:** `Claude outputs/ph-memory-history-verify.ps1`, derived from `j3b-recovery-verify.ps1`.

### 25.3 Automated verification (Claude Code, fakes and disposable instances only)

**Backend:** run `usapp-ph-87023dece9`, existing local image `sha256:83afaf61...cbfb1`, internal network, no published port, tmpfs PostgreSQL 17, newly generated one-time credentials. Container IDs and the project label were checked before each database step.

- Full build passed. 43 migrations were applied, and every checksum matches its local file. Eraser violations 0.
- **Targeted:**

  | Suite | Assertions | Result |
  |---|---|---|
  | `USAPP_TEST_ONLY_MEMORY_DELETION` (new suite included) | 1048 | 0 failed |
  | `USAPP_TEST_ONLY_MEMORY` | 260 | 0 failed |
  | Live API run (real `usapp_api` binary on the container's loopback) | 304 | 0 failed |

- **Full CTest 9/9, 103.90 s:**

  | Target | Assertions | Time |
  |---|---|---|
  | unit | 2817 | 11.24 s |
  | **db_gated** | 8104 | **29.31 s** |
  | media_db | 239 | 11.39 s |
  | media_deletion_db | 376 | 5.93 s |
  | memory_deletion_db | 1048 | 8.23 s |
  | push_dispatch | 582 | 20.02 s |
  | oss_stub | 363 | 5.00 s |
  | static_check | 1133 | 0.05 s |
  | journal_cleanup_db | 147 | 12.73 s |

- Afterwards: early or reversed erasures 0; unerased text or fingerprints 0; removal records for keep or live keys 0; privilege deviations 0. Crash markers in `LastTest.log` 0; PostgreSQL PANIC/FATAL 0; generated credentials in the retained logs (other than the env file) 0. The retained logs and both containers (stopped) are under `%TEMP%/usapp-ph-87023dece9`.
- **db_gated took 29.31 s of its 30 s limit.** This batch adds nothing to the default db_gated run: the new suite runs in `memory_deletion_db`. The thin margin was already recorded at J3b (Section 13 of phase3-deletion-journal.md, one timeout at 30 s). No timeout was raised.
  *Follow-up 2026-11-08: Section 26 moves the journal service suites out of `db_gated`; this 29.31 s remains the historical result of that run.*

What the new backend cases cover:

- Both members list; another couple, a user in no space, an unknown id and a proposal id used as a memory id are refused. A table and reservation footprint is unchanged by every read, and a reserved photo stays reserved.
- All four statuses, newest first, with `decided_by`. An approval created version 2, the creator then made version 3, and the list keeps 2 as a fact with `current_version` 3. Each decided request's detail is still readable. Decision replays still answer `Replayed`, a conflicting decision `MEMORY_PROPOSAL_DECIDED`, and the list and footprint are unchanged afterwards.
- Five proposals given one identical `created_at` (a fixture written with triggers off) walk in id order at limits 1, 2, 3, 5 and 100. A walk in progress, interrupted by a decision and a newer proposal, finishes with exactly the original set.
- A deleted memory hides the list and every proposal detail for both members. Undo restores them, and shows that the deletion rejected the open request. An expired cycle erased by `usapp_memory_erase` stays hidden on the list, the proposal reads and the memory read.
- Through the real API binary: 401 without a session; 200 with `no-store` for both members; `limit=0`, `limit=101` and a malformed cursor answer 400; an absent, malformed or other-couple memory answers `MEMORY_NOT_FOUND`; deletion answers 404 for both members; undo answers 200 again.

**Web (host, fake API):**

| Check | Result |
|---|---|
| Focused: `memory-proposal-history.spec.ts` 16, `memories.spec.ts` 66, `memory-deletions.spec.ts` 13, `media-deletions.spec.ts` 47 | 142 passed, exit 0 |
| Full web suite | 38 files, 810 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` and `tsc --noEmit -p tsconfig.node.json` | Exit 0 |
| `vite build --outDir %TEMP%/usapp-ph-web-334477363a` | Exit 0, 47 files. A hash over all 45 files of `apps/web/dist` was identical before and after |

The new web tests cover:

- The summary rules for each status.
- Store paging and a failed later page.
- Malformed pages, and pages naming another memory.
- Late answers after a route change, after leaving and after an account switch.
- A late comparison.
- A proposer finding their decided request after signing in again, with an empty fresh store.
- Every label and the historical versus current version text.
- Opening the comparison, empty and failed history with retry, and a reload after a decision through the existing controls.
- A deleted memory, which never requests its history and loads no image.
- Deletion while the page is open.
- Route and account switches in the mounted view, and no storage writes.

### 25.4 Runs that did not pass, and why

| Run | What happened |
|---|---|
| First attempt (no project created) | Docker Desktop was not running; `docker network create` failed at step 1, so nothing was created. The owner started Docker |
| `usapp-ph-2f7e22f8d5` | Build, checksums, both targeted suites (new suite 1048 assertions) and the live API run passed. The harness then failed at its own API stop step: `pkill -f build-test/usapp_api` matched its own `sh -c` command line and killed itself (exit 143). Harness defect; the API is now stopped by its recorded PID |
| `usapp-ph-2b1117731d` | Same passes. The stop command's double quotes were broken by Windows PowerShell 5.1 native argument passing (`sh: Syntax error`), the same class of problem Section 11.9 of the journal document records. The quotes were removed |
| `usapp-ph-87023dece9` | Passed, above |

No product or test assertion failed in any run, and no run was repeated without a diagnosed cause. The containers of each run are stopped and retained, and their networks are retained.

### 25.5 Not covered, and release gates

- **Not exercised:** a real browser or device, the service worker in a production build, the real object store, HTTP-level checks of a list with decided proposals (those are covered at the service level), performance with long histories, and concurrency between a list and a decision beyond the ordering argument above.
- **Contract:** the response has no per-item photo or text, so the UI needs the existing detail read to compare. A decision made by the deletion itself shows as an ordinary rejection by the deleter, as the existing proposal read already reports it.
- **Unchanged:** notifications and D20 are not implemented or resolved. Deletion and undo deadlines, cascade, erasure and journal semantics are unchanged. Gaps 2-4 of Section 23.3 remain.
- **Open as before:** all release gates of Sections 18, 22.5 and 23.4, D6 and the deletion journal. This includes the db_gated time margin, the historical intermittent failures and the DbClient teardown crash, none of which recurred here.

## 26. db_gated split: journal service suites in their own target (Claude Code, 2026-11-08)

Test infrastructure only. No product code, API, contract (0.37.0), migration (0000-0042, next 0043), dependency or deployment change. The existing 30 s limit of `db_gated` is unchanged, and no assertion, case or fixture was removed.

### 26.1 Why

History reported before this batch: `db_gated` took 28.63 s (J2c2 review repair), 28.95 s (J3a), a 30 s timeout once during J3b, and 29.31 s in run `usapp-ph-87023dece9` (Section 25), each against its 30 s limit.

Measured in this batch (run `usapp-dbsplit-1b529040d0`, before the change, one sample each, on a fresh database):

| Selection | Time | Assertions |
|---|---|---|
| Default `db_gated`, as CTest sets it | 19.37 s | 5911 |
| `USAPP_TEST_ONLY_JOURNAL_PURGE` | 5.46 s | 4931 |
| `USAPP_TEST_ONLY_REMINDER_JOB` | 3.46 s | 564 |
| `USAPP_TEST_ONLY_ALBUM` | 2.16 s | 724 |
| `USAPP_TEST_ONLY_MEMORY` | 1.22 s | 260 |
| `USAPP_TEST_ONLY_JOURNAL_MEMORY` | 0.63 s | 208 |
| `USAPP_TEST_ONLY_JOURNAL_MEDIA` | 0.27 s | 79 |
| `USAPP_TEST_ONLY_OSS` | 0.23 s | 41 |

The default run took 19.4 s on a fresh database but 29.3 s inside a full verification. The journal purge suite's historical confirmation checks grow with the rows that earlier suites and targets leave behind: 4931 assertions here, compared with 8104 in the whole `db_gated` of Section 25. It is the largest group whose cost depends on accumulated data, so it was moved, together with the other two journal service suites that run beside it. All three already run last and already have their own focused modes.

### 26.2 The split

| Suite | Before | After |
|---|---|---|
| `journalmedia::run` (J2b1) | default `db_gated` tail | `journal_services_db` |
| `journalmemory::run` (J2b2) | default `db_gated` tail | `journal_services_db` |
| `journalpurge::run` (J2c1) | default `db_gated` tail | `journal_services_db` |
| Every other default suite | `db_gated` | `db_gated`, unchanged, same order |

- `services/api/tests/DbGatedTest.cc`:
  - New focused mode `USAPP_TEST_ONLY_JOURNAL_SERVICES` runs the three suites in the full run's order. It sits after the intent-diagnostic sink is set, so the conditions match the full run.
  - The default tail runs them only when `USAPP_TEST_SEPARATE_JOURNAL` is unset, the same pattern as `USAPP_TEST_SEPARATE_MEDIA`. A direct run of the binary without that variable still runs everything.
  - The single-suite modes `USAPP_TEST_ONLY_JOURNAL_MEDIA`, `_MEMORY` and `_PURGE` are unchanged.
- `services/api/tests/CMakeLists.txt`:
  - `db_gated` now sets `USAPP_TEST_SEPARATE_MEDIA=1;USAPP_TEST_SEPARATE_JOURNAL=1`.
  - New target `journal_services_db` (same binary, `USAPP_TEST_ONLY_JOURNAL_SERVICES=1`, `SKIP_RETURN_CODE 77`, `TIMEOUT 30`). 30 s is the limit of every database target, and the measured runs below used 6.4-10.7 s of it.
  - CTest still runs targets one after another; nothing was made parallel.
- `services/api/tests/StaticCheckTest.cc`: one boundary check. It verifies:
  - both CMake environments and the new target are present;
  - both places hold the same three calls in the same order;
  - the focused mode comes after the diagnostic sink.
- `Claude outputs/dbsplit-verify.ps1`, derived from `ph-memory-history-verify.ps1`. Same ownership checks by full container ID and label, internal network, no published port, tmpfs PostgreSQL 17, generated one-time credentials, and containers stopped and kept at the end.
  - `-Measure` times the current default run and every focused group it contains.
  - Without it, the script runs the focused modes, full CTest, then three CTest repeats of `db_gated` and `journal_services_db`.

### 26.3 Registration, before and after

The case lists come from the `[ RUN ]` lines of each target in the complete CTest `LastTest.log`. "Before" is run `usapp-ph-87023dece9`, the same sources without the split; "after" is run `usapp-dbsplit-d828577d99`.

| Target | Cases before | Cases after |
|---|---|---|
| unit | 133 | 133 |
| db_gated | 203 | 179 |
| media_db / media_deletion_db / memory_deletion_db | 17 / 15 / 41 | 17 / 15 / 41 |
| push_dispatch / oss_stub | 29 / 14 | 29 / 14 |
| static_check | 42 | 43 (the new check) |
| journal_cleanup_db | 12 | 12 |
| journal_services_db | (none) | 24 |
| **Total** | **506** | **507** |

- No case is missing.
- The only added case is the new static check.
- The 24 moved cases all ran in `db_gated` before and in no other target after.
- No case name appears twice in either run.

Assertion counts of the purge suite depend on the history in the database (Section 26.1). Totals per target are therefore not comparable across runs and are not used as evidence of completeness: `db_gated` 4137 plus `journal_services_db` 8494 after, against `db_gated` 8104 before.

### 26.4 Results of this batch

**Run `usapp-dbsplit-c3ba24e4c6`:**
- Build passed, 43 checksums matched, eraser violations 0.
- Focused modes `JOURNAL_SERVICES` 1754, `JOURNAL_MEDIA` 79, `JOURNAL_MEMORY` 208 and `JOURNAL_PURGE` 4949 assertions, 0 failed.
- **Full CTest 10/10 in 98.81 s** (db_gated 17.14 s, journal_services_db 8.47 s).
- Repeat 1 passed (19.81 s / 9.15 s).
- **Repeat 2: `db_gated` FAILED at 21.16 s** with 1 failed assertion out of 4139. `journal_services_db` passed (10.68 s). The script stopped there, so repeat 3 did not run.
  - Failed case: `P2-6 R1: schedule bounds keep each instant inside its period; a custom instant stays inside the range` (`ReminderDbTests.h:347`). The server answered `USAPP_REMINDER_TIME` from `usapp_reminder_rule_guard` line 85.
  - That line is a causal-order check: a new rule's `created_at` (default `clock_timestamp()`) must be no later than the clock and no earlier than the task or the previous rule.
  - The case sets several rules on one target in quick succession. This guard is not among the eight that 0041 hardened, so it keeps the exposure to the backward-stepping database clock recorded in docs/phase3-timestamp-guards.md.
  - The same case passed in that run's full CTest and repeat 1. The suite was not moved or changed.
  - **The cause is consistent with that known class but is not proven:** no clock instrument ran. The complete log is kept as `063.log` in `%TEMP%/usapp-dbsplit-c3ba24e4c6`, and it holds no credential.
- No crash marker in this run.

**Run `usapp-dbsplit-d828577d99`** (a fresh instance, rerun at the owner's request after the diagnosis above):
- Build passed, 43 checksums matched, eraser violations 0.
- Focused modes `JOURNAL_SERVICES` 1817, `JOURNAL_MEDIA` 79, `JOURNAL_MEMORY` 208 and `JOURNAL_PURGE` 4949 assertions, 0 failed.
- **Full CTest 10/10 in 92.35 s.** Every target passed, with these times and assertions:

  | Target | Time | Assertions |
  |---|---|---|
  | unit | 11.21 s | 2817 |
  | db_gated | 15.34 s | 4137 |
  | media_db | 10.31 s | 239 |
  | media_deletion_db | 5.70 s | 376 |
  | memory_deletion_db | 8.19 s | 1048 |
  | push_dispatch | 19.57 s | 582 |
  | oss_stub | 4.98 s | 363 |
  | static_check | 0.04 s | 1140 |
  | journal_cleanup_db | 10.58 s | 147 |
  | journal_services_db | 6.42 s | 8494 |

- Early or reversed erasures 0; removal records for keep or live keys 0.
- Crash markers, PostgreSQL PANIC/FATAL, and generated credentials in the retained logs: 0 in each of the three runs of this batch.

**Every timed result of the two affected targets in this batch:**

| Run | Sample | db_gated | journal_services_db |
|---|---|---|---|
| c3ba24e4c6 | full CTest | 17.14 s pass | 8.47 s pass |
| c3ba24e4c6 | repeat 1 | 19.81 s pass | 9.15 s pass |
| c3ba24e4c6 | repeat 2 | **21.16 s FAIL (reminder clock guard, above)** | 10.68 s pass |
| d828577d99 | full CTest | 15.34 s pass | 6.42 s pass |
| d828577d99 | repeat 1 | 17.19 s pass | 7.13 s pass |
| d828577d99 | repeat 2 | 17.78 s pass | 7.84 s pass |
| d828577d99 | repeat 3 | 19.63 s pass | 10.55 s pass |

`db_gated` ranged from 15.3 to 21.2 s, against 28.6-29.3 s historically. These are seven samples on one host, not a guaranteed runtime.

### 26.5 Remaining risks

- **Both targets get slower as one database accumulates rows.** Within a run, each repeat took longer: `db_gated` went from 15.3 to 19.6 s, and `journal_services_db` from 6.4 to 10.6 s. Ordinary verification uses a fresh disposable database per run. Running many CTest rounds against one long-lived database will erode this margin again; the purge suite's history-driven checks are the main growth.
- **The reminder rule's `created_at` guard can still refuse a write** after a backward clock step. It is an existing product-level exposure outside 0041 and is unchanged here. The historical intermittent failures and the DbClient teardown crash remain open.
- **Not run:** the J3b recovery-only harness, the SQL ledger scripts, the web suite (no web file changed), and any shared database, cloud, real storage, real journal or real push.
- Containers of `usapp-dbsplit-1b529040d0`, `-c3ba24e4c6` and `-d828577d99` are stopped and kept, with their networks and their `%TEMP%` work folders.

## 27. Album rename-request history (Claude Code, 2026-11-08)

`GET /api/v1/albums/{albumId}/proposals` and an album-detail history card, the album counterpart of Section 25. Contract **0.38.0**; no migration (0032's index serves it); next migration 0043. The new database suite runs in its own CTest target `album_history_db`, outside `db_gated`.

Verified with fakes only, by Claude Code:
- Run `usapp-ah-6250d956c3`: CTest 11/11 in 89.56 s, with `db_gated` at 15.70 s and `album_history_db` at 3 cases, 97 assertions.
- Live API run 334 assertions; 43 checksums match.
- Web: 827 tests, both typechecks, and an isolated build with `dist` unchanged.

The same batch fixed a watcher in `MemoryDetailView.vue` (Section 25) that re-read the history whenever the memory object was replaced with an equal version.

Real browser, device, cloud and restore acceptance are deferred, not passed. **NOT DEPLOYED.** Evidence: docs/phase3-p32b-album-rename-design.md Section 8.

## 28. Recovering unattached uploads (Claude Code, 2026-11-08)

Closes gap 3 of Section 23.3 and gap 3 of phase3-p32-albums-design.md Section 13.3. Before this batch, an uploader whose READY photo was not yet attached had no route to it after a reload or after leaving the editor: it waited in its unattached period and was then cleaned up. **This is not the retained-photo list (D6 M1c) and not a deletion recovery: nothing deleted is restored here.** **NOT DEPLOYED.** Real browser, device, cloud and restore acceptance are deferred, not passed and not waived.

### 28.1 Implemented behaviour

**Backend.** New `GET /api/v1/media/unattached`, registered before `/api/v1/media/{mediaId}`. It is an authenticated read with the same pager, page size, cursor and error answers as `/api/v1/media/retained`.

| Aspect | Behaviour |
|---|---|
| Who | Only the uploader's own photos, in the caller's current couple. The partner and another space see none of them. No space: `SPACE_NOT_FOUND`. No session: 401 |
| Which | READY photos that may take their **first** home now. The conditions are one SQL fragment, `USAPP_MEDIA_UNHOMED` in the new `services/api/src/services/MediaEligibility.h`: no committed `everything` cleanup plan, no binding of any kind, no pending-proposal reservation, and not deleted now |
| One home | A binding is forever, so a photo once in a memory or album is never offered again. That includes a photo removed from an album, one dropped by a later memory revision, and one whose memory was deleted (the retained list covers those) |
| Shared definition | The album photo check now uses the same fragment, with its SQL text unchanged. The memory check's unbound branch states the same conditions inline, and a static check keeps it equal |
| Order and pages | `(uploaded_at DESC, id DESC)` at microsecond precision, keyset cursor, `limit` 1-100 |
| Each item | `media_id, uploaded_at, width, height`. No object key, upload grant, signed URL, file name or fingerprint |

**Snapshot, not a reservation.** Listing or choosing a photo reserves nothing, renews no grant and moves no cleanup deadline; the footprint test checks this. The attachment that follows decides under its existing photo locks and guards. If cleanup, deletion or another attachment won first, it answers `MEMORY_PHOTO_UNAVAILABLE` or `ALBUM_PHOTO_UNAVAILABLE` as before.

**No migration.** `media_objects_couple_idx (couple_id, created_at DESC)` serves the query, filtered by uploader. 0000-0042 are unchanged; the next number stays 0043. **Contract 0.38.0 -> 0.39.0**, additive: `listUnattachedMedia`.

**Web.**
- The memory editor (create, creator edit, partner request) and the album photo adder each get a "选择之前上传的照片" (choose previously uploaded photos) entry, loaded when opened. Photos render through the existing authenticated `MemoryPhotoView`.
- The panel says what the list is:
  - It holds the caller's own photos that are not used in any memory or album, and no deleted photos.
  - The list may have changed since it was opened.
  - Choosing neither keeps a photo nor extends how long it is kept.
  - A photo that became unavailable is refused at save, and the other choices stay.
- **Memory editor:** chosen photos join the order as already-known photos (`m:` entries), never as uploads, so no grant or file is invented. An id the form already holds is neither offered nor added again. The nine-photo limit counts every entry. While an action holds the form (`busy`), the picker is hidden, so an unconfirmed write keeps the payload it captured.
- **Album adder:**
  - Chosen photos stay in their own list, apart from the upload queue. One order covers both kinds, and an add sends chosen photos and READY uploads in that order, each id once.
  - The per-add limit (20) counts both kinds, and the file input is disabled at the limit.
  - While an add is unconfirmed, the picker and each "不加这张" (don't add this one) are disabled, and its retry resends exactly the captured ids and key.
  - A refused add (`ALBUM_PHOTO_UNAVAILABLE`) keeps every choice, says the list may have changed, and reads the list again; it never resends by itself.
  - A chosen photo leaves the selection once the album shows it, which also covers an unconfirmed add confirmed later through its own retry.
- **Store:** `src/stores/unattached.ts` is reset by the session store on any identity change. Reads carry a sequence and a generation, so late pages for another account are dropped. Chosen ids live only in the form, and nothing is kept in browser storage.
- **Not added:** upload-deadline renewal, deletion controls, notifications.

### 28.2 Files

- **Backend:**
  - New `services/api/src/services/MediaEligibility.h`.
  - `services/api/src/services/MediaService.{h,cc}`: `listUnattached`, `USAPP_UNATTACHED_SQL`.
  - `services/api/src/services/AlbumService.cc`: photo check through the shared fragment; same text.
  - `services/api/src/controllers/MediaController.cc`: route and handler.
- **Contract:** `contracts/openapi.yaml`.
- **Backend tests:**
  - New `services/api/tests/MediaUnattachedDbTests.h`, run by the new CTest target `unattached_db` (`USAPP_TEST_ONLY_UNATTACHED=1`, `TIMEOUT 30`), outside `db_gated` and outside a direct full run.
  - `DbGatedTest.cc` and `tests/CMakeLists.txt`: registration.
  - `ReminderLiveApiTest.h`: the route through the real API binary.
  - `StaticCheckTest.cc`: the shared fragment and its two users, the memory check's equal conditions, route order, contract and target. Version checks moved to 0.39.0.
- **Web:**
  - New `src/api/unattached.ts`, `src/stores/unattached.ts` and `src/components/UnattachedPhotoPicker.vue`.
  - `src/components/MemoryEditor.vue`, `src/components/AlbumPhotoAdder.vue` and `src/stores/session.ts` (reset).
- **Web tests:** new `tests/unit/unattached-photos.spec.ts` (12 tests).
- **Harness:** `Claude outputs/ua-unattached-verify.ps1`, derived from `ah-album-history-verify.ps1`.

### 28.3 Automated verification (Claude Code, fakes and disposable instances only)

**Backend, final run `usapp-ua-ed5e2a4829`:** existing local image, internal network, no published port, tmpfs PostgreSQL 17, newly generated one-time credentials. Container IDs and labels were checked before each database step.

- Build passed. 43 checksums match. Eraser violations 0.
- **Targeted:**

  | Suite | Cases | Assertions | Result |
  |---|---|---|---|
  | `UNATTACHED` | 3 | 58 | 0 failed |
  | `ALBUM` | 22 | 726 | 0 failed |
  | `MEMORY` | | 260 | 0 failed |
  | `MEDIA` | | 239 | 0 failed |
  | Live API run (real binary) | | 347 | 0 failed |

- **Full CTest 12/12, 91.49 s:**

  | Target | Assertions | Time |
  |---|---|---|
  | unit | 2817 | 11.23 s |
  | **db_gated** | 4137 | **15.44 s** |
  | media_db | 239 | 10.76 s |
  | media_deletion_db | 376 | 5.74 s |
  | memory_deletion_db | 1048 | 7.77 s |
  | push_dispatch | 582 | 19.80 s |
  | oss_stub | 363 | 5.00 s |
  | static_check | 1175 | 0.03 s |
  | journal_cleanup_db | 147 | 11.49 s |
  | journal_services_db | 4168 | 3.73 s |
  | album_history_db | 97 | 0.19 s |
  | unattached_db | 58 | 0.31 s |

- Consistency and privilege checks 0. Crash markers 0, PostgreSQL PANIC/FATAL 0, generated credentials in the retained logs 0.

What the backend cases cover:

- **Classes.** Each of these is absent from the uploader's list:
  - the partner's photo and another space's photo;
  - a granted but incomplete upload, and a rejected upload;
  - a photo bound to a memory, and a photo added to an album and then removed;
  - a photo reserved by a pending proposal;
  - a deleted photo, and a planned aged photo.

  A fresh READY photo and an aged unplanned one are present. Items have exactly four members and no key text. A user in no space gets `SpaceNotFound`. A footprint over grants and deadlines, outcomes, plans, bindings, reservations, deletions, attempts and audit rows is unchanged by every read. After a proposal is rejected, its released photo is listed again, and an undone deletion lists its photo again.
- **Both orders, through existing service paths:**
  - Cleanup first: a listed aged photo is planned, then album add and memory create are refused, and the list drops it.
  - Attachment first: a listed photo is added to an album, a plan is then refused, and the list drops it. The same holds through a memory create, after which a second home in an album is refused.
  - Concurrent races of attachment against cleanup remain covered by the existing P3-1b and P3-2a suites; this batch did not add new concurrent races.
- **Pages.** Five photos given one identical `created_at` (a maintenance fixture with triggers off) walk in id order at limits 1, 2, 3, 5 and 100. A newer upload never enters a walk in progress, and an independent later read finds all six.
- **Through the real API binary:** 401; 200 with `no-store` listing the uploader's directly committed READY photo, with no `staging/` or `media/` text; the partner and another space do not see it; `limit=0` and a malformed cursor answer 400; the literal path is not taken for a media id.

**Web (host, fake API):**

| Check | Result |
|---|---|
| Focused: `unattached-photos.spec.ts`, `albums.spec.ts`, `memories.spec.ts`, `media-deletions.spec.ts`, `memory-deletions.spec.ts` | First run: 192 of 193 passed (below) |
| Full web suite (final) | 40 files, 839 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` and `tsc --noEmit -p tsconfig.node.json` | Exit 0 |
| `vite build --outDir %TEMP%/usapp-ua-web-e0e943cedc` | Exit 0, 48 files. The hash over all 45 files of `apps/web/dist` was identical before and after |

### 28.4 Failures in this batch, all diagnosed

| Where | What happened |
|---|---|
| Web, focused run (`Claude outputs/ua-web-focused-1.log`) | The limit test failed: with 20 chosen photos the album adder's file input stayed enabled, because it counted only the upload queue. A real gap: `pick()` already capped files by the shared room, but the control did not show it. The input is now disabled on the combined room |
| Backend run `usapp-ua-e7226a053c` | Build, checksums and all four targeted suites passed. The live API run failed one assertion: the new fixture used an invalid PostgreSQL interval (`'1 hour 00:10:00'`), the exception ended the live case, and the harness counted it. A test defect, corrected to `'70 minutes'` |
| Backend run `usapp-ua-ed5e2a4829` | Passed, above |

The unchanged album suite counted 724 assertions in the first run and 726 in the second. The same 22 cases ran; its cursor walks assert per page, and the pages depend on rows present. This is not a failure. The containers of both runs are stopped and retained, with their networks and `%TEMP%` work folders.

### 28.5 Limits and release gates

- **No reservation guarantee:** a chosen photo can still be cleaned up, deleted or attached elsewhere before the save, which then refuses. The list does not show how long a photo will still be kept, and nothing renews it.
- **Not exercised:** a real browser or device, a production service-worker build, real object storage (the panel renders thumbnails through the existing content route), large photo libraries, and an HTTP check of each excluded class (those are covered at the service level).
- **Memory editor refusal:** the memory forms keep every entry after `MEMORY_PHOTO_UNAVAILABLE`, with the existing message. The picker's note gives the explanation; unlike the album adder, the memory form does not re-read the list automatically.
- **Unchanged:** notifications and D20, timestamp guards, the journal, the deletion policy and the cleanup rules. Gaps 2 and 4 of Section 23.3, and gaps 2 and 4-6 of the albums design, remain.
- **Open as before:** all release gates of Sections 18, 22.5 and 23.4, D6 and the deletion journal.

## 29. Upload capability query (Claude Code, 2026-11-09)

Closes gap 2 of Section 23.3 and gap 2 of phase3-p32-albums-design.md Section 13.3. A directly opened memory editor, new-memory page or album detail used to load a timeline or album-list page only to learn `uploads_enabled`. **NOT DEPLOYED.** Real browser, device, cloud and restore acceptance are deferred, not passed and not waived.

### 29.1 Implemented behaviour

**Backend.** New `GET /api/v1/media/capabilities`, an authenticated read registered before `/api/v1/media/{mediaId}`.

| Aspect | Behaviour |
|---|---|
| Answer | `{"uploads_enabled": bool}` with `Cache-Control: no-store`. The value is `MediaService::uploadsEnabled()`, the same predicate the memory timeline and album list use (whether an object store is configured now). Those fields are kept unchanged for compatibility |
| Who | A member of a space, after one membership query. A caller in no space: `SPACE_NOT_FOUND`. No session: 401 |
| What it never does | It contacts no store, creates no grant, writes no row and reserves nothing. It names no provider, configuration, endpoint, bucket or failure detail |
| Snapshot | An enabled answer is never permission: `createMediaUpload` and `completeMediaUpload` still answer `MEDIA_DISABLED` by themselves, as before |

No migration; 0000-0042 are unchanged and the next number stays 0043. **Contract 0.39.0 -> 0.40.0**, additive: `getMediaCapabilities`.

**Web.**
- One identity-scoped store, `src/stores/media-capability.ts`, is the only thing pickers ask. Its value is true, false, or unknown (null while unknown, loading or after a failed or malformed read). Every picker treats anything but true as "cannot start a new upload".
- **Sources:**
  - its own capability read;
  - the `uploads_enabled` that a timeline or album-list page still carries (`observe`);
  - a real operation's `MEDIA_DISABLED` (`markDisabled`), still reported to the memories and albums stores as before.
- **Ordering:**
  - Each read takes a token when it starts. A new read, `MEDIA_DISABLED` and an identity reset each move the epoch, so an answer that started earlier is dropped.
  - `MEDIA_DISABLED` therefore outranks an enabled answer still in flight, and only a read started after it can turn uploads back on.
  - The session store resets it on any identity change, and a late answer for another account is dropped.
- **Direct navigation:** the new-memory page, a memory's edit form and album detail call `ensure()`, one capability read when nothing is known, instead of loading the timeline or album list. A list load still happens where its content is shown: the timeline, the album list, and the shared plan's memory picker. The plan's convert editor uses the capability query for the upload decision. *Corrected 2026-11-09 (Section 29.6):* as first delivered, opening convert also loaded the memory list, which convert does not show; it now loads it only for link.
- **Explicit retry:** a failed read shows its reason and a "重新检查" (check again) button that reads the capability again, in the memory editor, the album adder and the new-memory page. While loading, the pages say so.
- **What a disabled or unknown capability does not touch:** uploads already in the queue, READY photos, their retries, captured write payloads and unconfirmed actions. Choosing previously uploaded photos (Section 28) also stays available.
  - **Behaviour change:** the new-memory page and the shared plan's convert editor used to hide the whole form while uploads were off or unknown. They now explain the state and keep the form, so a memory can still be made from previously uploaded photos; only the file picker is withheld.
- Nothing is persisted in browser storage.

### 29.2 Files

- **Backend:**
  - `services/api/src/services/MediaService.{h,cc}`: `Capabilities`, `capabilities`.
  - `services/api/src/controllers/MediaController.cc`: route and handler.
- **Contract:** `contracts/openapi.yaml`.
- **Backend tests:**
  - New `services/api/tests/MediaCapabilityDbTests.h`, run by the new CTest target `media_capability_db` (`USAPP_TEST_ONLY_MEDIA_CAPABILITY=1`, `TIMEOUT 30`), outside `db_gated`.
  - `DbGatedTest.cc` and `tests/CMakeLists.txt`: registration.
  - `ReminderLiveApiTest.h`: the route through the real binary.
  - `StaticCheckTest.cc`: the handler reads `uploadsEnabled()` and no store, row or lock; the timeline and album list use the same predicate; route order, contract and target. Version checks moved to 0.40.0.
- **Web:**
  - New `src/api/media-capability.ts` and `src/stores/media-capability.ts`.
  - `src/stores/session.ts` (reset), `memory-uploads.ts` (`markDisabled`), `memories.ts` and `albums.ts` (list pages observe through a token).
  - `src/components/MemoryEditor.vue` and `AlbumPhotoAdder.vue`.
  - `src/views/MemoryNewView.vue`, `MemoryDetailView.vue`, `AlbumDetailView.vue` and `SharedPlanDetailView.vue`.
- **Web tests:**
  - New `tests/unit/media-capability.spec.ts` (9 tests).
  - `memories.spec.ts`, `albums.spec.ts` and `unattached-photos.spec.ts` updated to set capability through the new store or query instead of list pages (see 29.4).
- **Harness:** `Claude outputs/cap-capability-verify.ps1`, derived from `ua-unattached-verify.ps1`.

### 29.3 Automated verification (Claude Code, fakes and disposable instances only)

**Backend: three attempts, all reported.** Each used the existing local image, an internal network, no published port, tmpfs PostgreSQL 17 and newly generated one-time credentials. Container IDs and labels were checked before each database step.

| Run | Result |
|---|---|
| `usapp-cap-924cc5ff1c` | Docker Desktop was not running; step 1 failed and nothing was created |
| `usapp-cap-f5a5666dbf` | Build passed; 43 checksums match; eraser violations 0. Targeted `MEDIA_CAPABILITY` 1 case / 17 assertions, `MEDIA` 239 and `UNATTACHED` 58 passed, and the live API run passed 360. **Full CTest 12/13 failed:** `unattached_db` stopped its first case at the first upload grant (27 assertions, 1 failed). The other 12 targets passed, including `media_capability_db` (17) |
| `usapp-cap-7ffc2a0c6f` | Owner-approved rerun after the diagnosis below. Same targeted results. **Full CTest 13/13 passed, 124.77 s** |

Per-target results of the passing run:

| Target | Assertions | Time |
|---|---|---|
| unit | 2817 | 11.32 s |
| **db_gated** | 4139 | **27.61 s** |
| media_db | 239 | 12.50 s |
| media_deletion_db | 376 | 7.84 s |
| memory_deletion_db | 1048 | 12.64 s |
| push_dispatch | 582 | 22.02 s |
| oss_stub | 363 | 5.14 s |
| static_check | 1189 | 0.07 s |
| journal_cleanup_db | 147 | 16.71 s |
| journal_services_db | 3979 | 7.62 s |
| album_history_db | 97 | 0.45 s |
| unattached_db | 58 | 0.77 s |
| media_capability_db | 17 | 0.07 s |

Afterwards: erasure and keep/live consistency 0, privilege deviations 0, crash markers 0, generated credentials in the retained logs 0. Each server log has one FATAL line, written while the readiness check connected during PostgreSQL's initial startup ("starting up", or "database does not exist" before init finished). Neither is during the tests.

**The failed run, diagnosed.**
- The server refused a `media_objects` insert with `USAPP_MEDIA_TIME`, from `usapp_media_object_guard` (0029). That guard compares the row's default `created_at`, one `clock_timestamp()` reading taken when the row was formed, with a second `clock_timestamp()` taken after an advisory lock. It can only fire if the database wall clock moved backwards between the two readings: the backward-step class recorded in docs/phase3-timestamp-guards.md.
- This guard is not among 0041's eight. The cause is consistent with that class but **not proven**: no clock instrument ran.
- The failing suite does not involve this batch's code. It passed in the same run's targeted step, and in the rerun. Its log is kept as `068.log` in `%TEMP%/usapp-cap-f5a5666dbf`.

**db_gated took 27.61 s and 27.81 s in this batch's two full runs,** against 15.44 s in Section 28's run, with the same assertion counts (4139 and 4137). Every database target slowed by a similar factor in both runs, for example `memory_deletion_db` 12.64 s against 7.77 s, while `unit` stayed at 11.3 s. This batch added nothing to `db_gated`: its new suite has its own target. The slowdown appeared after Docker Desktop was restarted for this batch and is not explained. It leaves `db_gated` near its 30 s limit again on this host. No timeout was raised.

What the backend cases cover:
- **Service:**
  - Both members read `true` while a fake store is configured, and the value equals `uploadsEnabled()`.
  - Store `calls`, read signings and copies are unchanged by the reads, and so are the media rows and audit rows.
  - With no store, the read is `false`, and a grant is refused with `Disabled`.
  - Configured again it reads `true`; with the store then removed, a grant is still refused, so an enabled read is only a snapshot.
  - A user in no space gets `SpaceNotFound`.
- **Real API binary (no store configured):** 401; 200 with `no-store` for both members, with exactly one boolean member `false`, agreeing with the timeline's `uploads_enabled:false`. A user in no space gets 404 `SPACE_NOT_FOUND`, and the literal path is not taken for a media id.

**Web (host, fake API):**

| Check | Result |
|---|---|
| New `media-capability.spec.ts` | 9 passed |
| Mutation check: `markDisabled` without its epoch bump | The ordering test failed (1 of 9), as it should. The source was restored before the runs below |
| Full web suite (final) | 41 files, 848 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` and `tsc --noEmit -p tsconfig.node.json` | Exit 0 |
| `vite build --outDir %TEMP%/usapp-cap-web-054aa2cee3` | Exit 0, 48 files. The hash over all 45 files of `apps/web/dist` was identical before and after |

### 29.4 Failures, and existing tests changed

- **First full web run** (`Claude outputs/cap-web-full-1.log`): 20 of 839 tests failed, all in `memories.spec.ts`, `albums.spec.ts` and `unattached-photos.spec.ts`. Each set the upload capability through a timeline or album-list page, the mechanism this batch replaces. They were updated, and each kept its intent:
  - capability now comes from `getMediaCapabilities` or the store;
  - the album "disabled uploads" test also asserts that album detail does **not** read the album list;
  - `MEDIA_DISABLED` also turns the capability store off.
- **Two new-memory-page tests changed meaning, as 29.1 says.** "Explains instead of showing a form when uploads are off" now expects:
  - the explanation, and the form;
  - no file picker, and the previously-uploaded entry;
  - one capability read and no timeline read.

  The "unknown" test expects the file picker to appear after the retry.
- **A typo in the new spec** (escaped quotes) failed the typecheck once; it was corrected before the runs above.
- **Backend:** see 29.3. Docker was not running for the first attempt; one full CTest failed in `unattached_db` (`USAPP_MEDIA_TIME`, diagnosed above); the owner-approved rerun passed. `db_gated` ran near its limit in both full runs.

### 29.5 Limits and release gates

- **Capability is a snapshot:** an upload may still be refused with `MEDIA_DISABLED` after an enabled answer, and the page then switches its picker off. Nothing polls the capability: a page learns about a later change only from such a refusal, a retry, an identity change or a reopened page.
- **Not exercised:** a real browser or device, a production service-worker build, a real provider (in this test environment "enabled" means a fake store is configured), and a store configured or removed while a page is open. The service tests do cover a configuration change between reads.
- **Unchanged:** the upload lifecycle, cleanup, deletion, the journal, timestamp guards and notifications/D20. Gap 4 of Section 23.3, and gaps 4-6 of the albums design, remain.
- **Open as before:** all release gates of Sections 18, 22.5 and 23.4, D6 and the deletion journal.

### 29.6 Correction: the shared plan's convert path (Claude Code, 2026-11-09)

**What was wrong.** As first delivered in this batch, `openMemory()` in `SharedPlanDetailView.vue` called `memories.load()` for both choices and also `capability.ensure()` for convert. The memory list is rendered only for `mode === 'link'`, so "create a new photo memory" still loaded an unrelated list. Section 29.1 described convert as using the capability query, and did not say convert also loaded the list.

**The fix.** It is the only product change in this correction:
- Opening link loads the memory list.
- Opening convert calls `capability.ensure()` and loads no list.
- Switching from convert to link loads the list normally.

Linking, conversion, the convert editor and the uncertain-write handling are otherwise unchanged. No backend, contract or migration file changed, so backend tests were not run.

**Verification (Claude Code, host, fake API):**

| Check | Result |
|---|---|
| New `tests/unit/shared-plan-memory-open.spec.ts` (3 tests: link only; convert only; convert -> link -> convert) | 3 passed |
| The same spec against the old code (the unconditional load restored temporarily, then removed) | Both convert tests failed and the link test passed, as expected. The source was restored and checked byte for byte before the runs below |
| Focused: the new spec, `shared-plans.spec.ts` 13 and `media-capability.spec.ts` 9 | 25 passed |
| Full web suite | 42 files, 851 tests passed, exit 0 (`Claude outputs/cap-fix-web-full.log`) |
| `vue-tsc --noEmit -p tsconfig.json` and `tsc --noEmit -p tsconfig.node.json` | Exit 0 |

No web build was made for this correction; the batch's isolated build in Section 29.3 predates it. Real browser and device acceptance remain deferred.

## 30. D20 opt-in partner notifications (Claude Code, 2026-11-09)

**Owner approval (D20, quoted from the assignment).** Approved product rules:

1. Creating a photo memory offers "notify partner", default OFF.
2. Creating a shared plan offers "notify partner", default OFF.
3. Adding a batch of photos to an album offers "notify partner", default OFF. At most one notification per successful batch, never one per photo.
4. Do not add notifications for edits, rename requests/decisions, covers or plan state changes in this batch.
5. Deletion and undo remain silent under existing rules.
6. Notifications contain generic text only: no titles, descriptions, photos, signed URLs or storage keys.
7. Clicking a notification uses current authorization. It must not reveal unavailable, deleted or cross-couple content.
8. Same-key replay must never create another notification.

**NOT DEPLOYED.** Real Push, cloud, browser, device and restore acceptance are deferred: not passed and not waived. Nothing ran on shared `usapp_test` or production.

### 30.1 Implemented behaviour

The three writes reuse the P2-3b mechanism (docs/phase2-notifications.md Section 12): one append-only intent per operation, an optional notice in the same transaction, the deferred link UN105, and the guards UN102 and UN104. Only what differs is listed here.

| Aspect | Behaviour |
|---|---|
| Operations | `memory_create` (event `memory.created`, target kind `memory`, source `memory_id`); `shared_plan_create` (`shared_plan.created`, `shared_plan`, `shared_plan_id`); `album_photos_add` (`album.photos_added`, target kind `album`, target id the album, source `album_batch_id`, the `album_photo_batches` row) |
| Request | Optional boolean `notify_partner` on `createMemory`, `createSharedPlan` and `addAlbumPhotos` only. It is taken out of the body before the existing strict parser, so omitted and false are one request. A non-boolean answers 400 before anything is written. Every other plan write still refuses the field as an unknown field |
| Default | Off. Omitted means false. The three operations never remember a choice: no `preference_seq`, enforced by the replaced `notification_intents_preference_presence` check, and they are not in the remembered-choices read |
| Recipient | The other member of the couple, ordered by slot, at the time of the write. With no partner the write succeeds and reports `no_partner`, with no notice |
| Response | `notification`: the immutable facts of the original decision `{requested, created, reason, notification_id}`, on the create or batch answer only. A replay reads the original intent; a batch or memory from before 0043 replays as `null` |
| Replay | Memory and album: `true` changes the request fingerprint (`notify-partner:v1`), so a different choice under the same key is `IDEMPOTENCY_KEY_REUSED`. Shared plan: the plan stores its request without the choice, so the controller compares the replayed intent's `requested` with the new choice and answers `IDEMPOTENCY_KEY_REUSED` on a difference; omitted and false agree |
| One per batch | The source is the batch row; unique indexes allow one notice per batch and recipient and one intent per batch. A three-photo batch makes one notice |
| Transaction | The intent and notice are written in the write's own transaction, after its effects and before its commit. A rejected attachment or any later failure rolls both back |
| Plan conversion | `createForPlan` never records an intent. The resolver also refuses a memory with a `shared_plan_memory_links` row in mode `create`, so the database refuses a notice or intent for it as well |
| Not notified | Edits, edit and rename requests and decisions, covers, removals, plan state changes, conversion, deletion and undo. Unchanged code paths |
| Payload | Notices name only event, kind and id. The inbox item shape is unchanged (seven fields). Push fan-out is unchanged; the push worker's closed `TARGET_KINDS` gains `memory`, `shared_plan` and `album` |
| Opening | The inbox and the service worker open `/us/memories/:id`, `/us/plans/:id` or `/us/albums/:id`. Those pages apply today's authorization: a deleted, unavailable or other couple's item is not found |

**Migration `0043_phase3_notifications`** (SHA-256 `48a0cbb8d85821c13033c2b0aed2541e7af9077f3e4dcbcb21055ea48bc44952`), additive:
- three nullable source columns on `notifications` and `notification_intents`, with foreign keys: memory and plan by `(id, couple_id)`, batch by `id` (the guard checks the batch's couple);
- superset replacements of `notifications_one_source`, `notifications_target_kind_valid`, `notification_intents_operation_valid`, `notification_intents_family` and `notification_intents_preference_presence`;
- six partial unique indexes;
- `CREATE OR REPLACE` of `usapp_notification_optional_source` (three branches), `usapp_notification_guard` and `usapp_notification_intent_guard`. Both guards keep their 0041 bodies, including the existing timestamp assignment; the correction is not applied to any other guard;
- the `notifications_optional_intent` constraint trigger is recreated to cover the three events.

It has no grant, no SECURITY DEFINER, no row write and no new lock class. It requires 0042. Frozen migrations 0000-0042 are unchanged. A build with D20 needs 0043 at startup (`TaskPrivilegePolicy` identity row `phase3_notifications`/`sources_present`); the next migration number is 0044. **Contract 0.40.0 -> 0.41.0**, additive.

**Web.**
- A new `PartnerNoticeToggle.vue` ("通知对方") appears on the new-memory page, the shared-plan create form and the album photo adder only.
  - It reads and writes no remembered preference. The P2-3b `NotifyPartnerToggle` and its choices store are unchanged.
- It is off on every fresh form or batch. It resets on an identity change, on a new plan form, and after a confirmed batch.
- With no partner it is disabled and the write sends `false`.
- The choice is captured with the payload and key, and a retry resends both.
  - While a send is in flight or unconfirmed, the toggle shows the captured choice and is locked, including after leaving and returning.
- A response confirms an action only when its `notification` facts agree with the captured choice (`isValidNotificationFacts(..., allowNoPartner = true)`). Otherwise the action stays unknown.
- `MemoryEditor` and `SharedPlanForm` gain a default slot before their submit button. Only the create pages fill it, so edit, request and convert forms show no toggle.
- Fixed generic labels: 对方记录了新的回忆, 对方添加了新的共同计划, 对方往相册里添加了照片. The service worker's closed kinds and allowed paths gain the three pages.

### 30.2 Files

- **Migration:** `database/migrations/0043_phase3_notifications.sql` (new).
- **Backend:**
  - `services/api/src/services/NotificationIntent.{h,cc}`: operations, sources and `remembersChoice`.
  - `MemoryService.{h,cc}` and `AlbumService.{h,cc}`: choice, fingerprint, intent and facts.
  - `controllers/MemoryController.cc`, `AlbumController.cc` and `SharedPlanController.cc`.
  - `db/TaskPrivilegePolicy.cc`.
- **Push worker:** `services/push-worker/src/deliver.ts` and `tests/deliver.test.ts`.
- **Contract:** `contracts/openapi.yaml`.
- **Tests:**
  - New `services/api/tests/PhaseThreeNotificationDbTests.h`, run by the new CTest target `phase3_notification_db` (`USAPP_TEST_ONLY_PHASE3_NOTIFICATIONS=1`, `TIMEOUT 30`), outside `db_gated`.
  - `DbGatedTest.cc`, `tests/CMakeLists.txt`, `UnitTest.cc`, `ReminderLiveApiTest.h` and `StaticCheckTest.cc`.
  - New `database/tests/phase3_notification_test.sql`.
- **Web:**
  - New `src/components/PartnerNoticeToggle.vue`.
  - `src/api/{notification-labels,notifications,types,memories,albums,shared-plans}.ts` and `src/pwa/push-sw.ts`.
  - `src/stores/{memories,albums,shared-plans}.ts`.
  - `src/components/{MemoryEditor,SharedPlanForm,AlbumPhotoAdder}.vue`.
  - `src/views/{MemoryNewView,SharedPlansView,NotificationsView}.vue`.
- **Web tests:** new `tests/unit/phase3-notify.spec.ts`; `memories.spec.ts`, `albums.spec.ts` and `unattached-photos.spec.ts`.
- **Harness:** `Claude outputs/n3-notify-verify.ps1`, derived from `cap-capability-verify.ps1` and `j3b-recovery-verify.ps1`.

### 30.3 Automated verification (Claude Code, fakes and disposable instances only)

Each backend run used:
- the existing local image;
- an internal network with no published port;
- tmpfs PostgreSQL 17;
- newly generated one-time credentials.

Container IDs and owner labels were checked before each database step. Push was never configured: no transport, provider or subscription.

| Run | Result |
|---|---|
| Prevalidation 1-2 | Failed in the new ledger, not the migration; see 30.4 |
| Prevalidation 3 (`usapp-n3-7c80d69b17`) | 0043 plus the new ledger inside one rolled-back transaction, owner and application roles: 20 refusal cases each passed; 0043 columns absent afterwards |
| 1 (`usapp-n3-f25e56954f`) | Rollback prevalidation passed. Apply passed, 44 checksums matched, eraser violations 0, privilege deviations 0 after apply and after a repeat apply with InitRoles. Ten SQL ledgers passed in both roles or both modes: `phase3_notification_test`, `notification_ledger_test`, `notification_choice_test`, `message_ledger_test`, `memory_ledger_test`, `album_ledger_test`, `shared_plan_ledger_test`, `shared_plan_memory_ledger_test`, `timestamp_guard_test`, `push_dispatch_test`. Build passed. `phase3_notification_db` failed one new assertion (test error, 30.4); stopped there |
| 2 (`usapp-n3-f71cd70496`) | Same database stages passed. Targeted suites passed: `phase3_notification_db` 114 assertions (4 cases), memory 260, album 726, memory deletion 1048; live API 394. CTest 13/14 in 137.92 s: `static_check` failed three superseded assertions (30.4); every other target passed (`db_gated` 28.08 s, 4139 assertions; `phase3_notification_db` 1.10 s, 114) |
| 3 (`usapp-n3-bebaf6bf51`) | Same database stages, targeted suites (114/260/726/1048) and live API (394) passed. CTest 12/14 in 137.63 s: `static_check` passed. **`db_gated` timed out at 30.01 s** after starting 175 of its 179 cases with no failed assertion. **`media_db` failed two assertions** in "grants are bounded and replay safely" (`MediaDbTests.h:297-298`): the first grant got `USAPP_MEDIA_TIME` from the 0029 `usapp_media_object_guard` |

**Historical status at the end of the Claude Code batch: no single run had passed all 14 CTest targets.** Every target passed in at least one run, except `static_check`, which passed in run 3 only after its update. **Superseded for automated backend acceptance by the complete 15/15 run in Section 30.6; the failed runs and unresolved defects below remain part of the evidence.**
- **`media_db`:** `USAPP_MEDIA_TIME` matches the known backward step of the database clock (Sections 28-29). It is not proven. D20 changes no media code.
- **`db_gated` timeout:**
  - The margin was already thin: 27.61 and 27.81 s in Section 29, and 28.08 s in run 2 here.
  - `db_gated` runs memory and album suites whose creates and batches now also write one intent row each, so a contribution from D20 cannot be excluded.
  - No timeout was raised.
- Logs and work folders are retained, and every container was stopped.

**Push worker.** Typecheck passed (exit 0). The first test run built into a temporary directory and two test files could not load `web-push` there (`ERR_MODULE_NOT_FOUND`, a harness error, no assertion ran in those files). With the package's own `node_modules` linked into that directory: 66/66 passed, including a new test that accepts the seven closed kinds and the three events and refuses near-miss kinds. Log: `Claude outputs/n3-push-worker-tests.log`.

**Web.**
- Full suite: 43 files, 873 tests passed (851 before; 22 new).
- Focused D20 set: 7 files, 226 tests.
- Both typechecks passed (`vue-tsc` and the node `tsc`).
- An isolated `vite build --outDir` into a temporary directory passed (48 precache entries).
- `apps/web/dist` was not written:
  - It holds 45 files; the newest was written 2026-11-03 12:47, and the build ran 2026-11-09 16:43.
  - Its file-hash manifest is `6320ef91…696f`.
  - The script's own before/after comparison captured no files, so it proved nothing. It is not counted as evidence.

### 30.4 Failures, and existing tests changed

- **Prevalidation attempts 1-2** failed inside the rolled-back transaction because of the new ledger, not the migration:
  - Attempt 1 made the optional-intent link IMMEDIATE before its valid notices, which are written before their intents, as the service writes them.
  - Attempt 2: the second, application-role pass inherited IMMEDIATE from the first pass in the same transaction.
  - Fixed in the ledger: it sets DEFERRED before its valid fixtures and IMMEDIATE after them.
- **Backend run 1** (`usapp-n3-f25e56954f`): build, migration and ledgers passed. `phase3_notification_db` failed one new assertion: it expected six inbox fields where every item has seven. This was a test error, now an exact key-set check. The other three cases passed.
- **Backend run 2** (`usapp-n3-f71cd70496`):
  - Targeted suites and the live API passed. CTest 13/14: `static_check` failed three existing assertions that encoded the pre-D20 state ("notifications are left for review"):
    - the contract event enum ending at `reminder.anniversary]`;
    - "the worker never mentions memory";
    - "the worker never mentions album".
  - They were updated, not removed, to the approved state: the full new enum tail; the worker's exact seven-kind set; the worker still names no `memory_id`, `album_batch` or `album_id`. **Review point:** these were deliberate guards, superseded by D20.
- **Web:** five existing tests failed when the pages began sending an explicit `notify_partner: false`:
  - their fake 0.41 responses lacked the `notification` facts, so the stores rightly kept the result unconfirmed;
  - the fakes `created()` (memories), `done()` for `add_photos` (albums) and `added()` (unattached photos) now return `not_requested` facts, as a 0.41 server does.
- **No timeout was raised.** The new target is outside `db_gated`.

### 30.5 Limits and release gates

- Fakes and disposable instances only. No real Push provider, browser, device, cloud storage or restore was used, and none of these is accepted.
- The notice records the partner at write time. If the couple later changes, the old notice stays with its recipient, and opening it goes through today's authorization.
- A memory, plan or batch created before 0043 replays with `notification: null`.
- Deployment order: 0043 before the API build, and the API before the web build. A web build sends `notify_partner` and would get 400 from an older API.
- Rolling back to a build before D20, after any D20 notice exists, is not rehearsed. By static reading only:
  - an older web build labels such a notice with its generic fallback text and would open the anniversary page for an unknown kind, which then says not found;
  - an older push worker refuses the new kinds from its closed set, so the delivery attempt fails.

### 30.6 D20 verification follow-up and reminder test split (Codex, 2026-11-09)

**Outcome: one complete ordinary CTest run passed 15/15, using fake providers and a fresh disposable database. NOT DEPLOYED.** No product source, contract, web code, deadline rule or migration changed in this follow-up. The change is test orchestration and verification evidence; it does not fix the historical time-guard failures, DbClient crash or the general slowdown.

#### Static assertions reviewed before database suites

The three assertions from Section 30.4 encoded a pre-D20 closed set, not an enduring prohibition on approved notifications:

- The old event enum tail is extended with memory.created, shared_plan.created and album.photos_added, retaining the earlier events in order.
- The exact push target set includes memory and album as approved generic targets (along with shared_plan); the worker still must not mention memory_id, album_id or album_batch internals.
- Existing intent privilege/append-only checks and the absence of direct NotificationWriter use in MemoryService remain. D20's dedicated checks pin default-off/no remembered choice, conversion exclusion and additive SQL boundaries. These are text proxies, not substitutes for runtime tests.

The first follow-up built successfully and ran static_check first: 1204 assertions, zero failures. After the test split, static_check passed again before DB cases and inside full CTest: 1208 assertions, zero failures.

#### First follow-up and measured reason for splitting

Run `usapp-n3f-f2ee517c01`, retained under `%LOCALAPPDATA%/Temp/usapp-n3f-f2ee517c01`:

- Standard apply.ps1 path: 0043 rollback prevalidation passed 20 cases per role, then 0043 applied; 44 checksums matched, eraser violations zero, privilege deviations zero after apply and repeat. The eight affected SQL ledgers passed both roles; timestamp and push ledgers passed both modes.
- Focused assertions: D20 114, memory 260, album 726, memory deletion 1048, reminder group 564; live API binary 394. All passed.
- Host stopwatch durations, including Docker invocation: D20 1.674 s, memory 2.477 s, album 4.943 s, memory deletion 12.977 s, reminder group 6.150 s. They do not isolate database execution or prove D20 overhead.
- **Full CTest: 13/14, exit 8, 139.09 s.** Only db_gated timed out at 30.01 s after starting 175 of 179 cases, with no failed assertion or crash marker. media_db passed; USAPP_MEDIA_TIME did not recur. Log: `129.log`, with complete `LastTest.log` retained.
- Earlier D20 runs also show slower media, push and journal targets. A general environment contribution is plausible, but not measured causally. The exact share due to D20, fixture growth or host conditions remains unknown.

This reproduced budget failure, plus the approximately six-second independent reminder group, justified a coherent split rather than another unchanged full rerun.

#### Test-only changes and registration proof

- `services/api/tests/CMakeLists.txt`: db_gated adds USAPP_TEST_SEPARATE_REMINDERS=1; new reminder_services_db uses the existing USAPP_TEST_ONLY_REMINDER_JOB=1 entry, TIMEOUT 30 and SKIP_RETURN_CODE 77. No existing timeout changes.
- `DbGatedTest.cc`: only wraps the original reminder, reminderjob, remindersuccessor and remindersource calls in the separation guard. Their focused entry and order are unchanged. Direct full binary runs still include them without that variable.
- `StaticCheckTest.cc`: updates the complete environment-list assertion and adds one case pinning identical four-suite call blocks in the focused and full paths. No original runtime cases or assertions were edited.
- `Claude outputs/n3-registration-check.py` compared the complete pre-split D20 run `usapp-n3-f71cd70496/LastTest.log` with the final ordinary CTest log: old default 179 cases = new default 150 + moved reminder 29; zero omitted or duplicated cases. Complete totals 522 -> 523 add exactly the new static case. Assertion totals are not used as the coverage proof because some depend on fixture history or branches.

#### Final verification after the split

Evidence directory: `%LOCALAPPDATA%/Temp/usapp-n3f-f2ee517c01/reminder-split`.

The exact owned, stopped compiler was reused as a build cache and incrementally rebuilt. A **new** PostgreSQL container (`usapp-n3f-f2ee517c01-split-postgres`) used empty tmpfs, the prior run's internal network, no published ports and only generated test passwords. Ownership was checked by full container ID and owner label. The empty `usapp_test` database was checked before loading.

For this test-only rerun, frozen migration SQL was batched into one psql input, preserving each migration's transaction and injecting its checksum registration immediately before its unique outer COMMIT. InitRoles followed. All 44 installed checksums and privilege deviations were checked. This avoided repeating the slow deployment runner after a test-only split; **it is not an additional apply.ps1, preflight, rollback or upgrade rehearsal**. That standard runner's evidence is the first follow-up above.

Focused reminder and separated-default runs passed before ordinary CTest. Then **15/15 passed, 141.05 s total**:

| Target | Assertions | Seconds |
|---|---:|---:|
| unit | 2818 | 11.39 |
| db_gated | 3575 | 25.41 |
| media_db | 239 | 13.37 |
| media_deletion_db | 376 | 8.52 |
| memory_deletion_db | 1048 | 14.58 |
| push_dispatch | 582 | 22.96 |
| oss_stub | 363 | 5.14 |
| static_check | 1208 | 0.08 |
| journal_cleanup_db | 147 | 19.25 |
| journal_services_db | 3901 | 9.27 |
| album_history_db | 97 | 0.54 |
| unattached_db | 58 | 0.96 |
| media_capability_db | 17 | 0.07 |
| phase3_notification_db | 114 | 1.13 |
| reminder_services_db | 564 | 8.36 |

Post-CTest privilege deviations and six consistency counts were zero: unsafe eraser violations; early or reversed-cycle text purges; keep/live late-object removal records; plan-conversion notification intents; multi-notice album batches; remembered D20 choices. All 44 host migration files remained byte-identical, including frozen 0043 sha256 `48a0cbb8d85821c13033c2b0aed2541e7af9077f3e4dcbcb21055ea48bc44952`.

Across 176 retained log files in this follow-up, a scan for the three generated database passwords found zero hits; crash markers and PostgreSQL PANIC markers were zero. This is not a claim to have scanned every secret or every historical log.

#### Harness failures, retained resources and limits

- The first parser-check command was itself invalid because its reference variables were uninitialized; its printed PASS was not evidence. The corrected parser invocation passed, as did parsing after the final harness edit.
- The initial split-run invocation failed before Docker access because Windows PowerShell 5.1 misdecoded a literal Chinese default path. The default now derives from LOCALAPPDATA. No tests ran in that invocation.
- After the green 15/15 CTest run, the split harness exited 1 while capturing Docker stderr: a normal initdb local-auth warning was treated as a PowerShell error, interrupting teardown. **CTest itself returned zero; the original harness exit was not zero.** Warning handling was corrected. `n3-split-finalize.ps1` then ran the six post-checks and privileges, saved logs, and stopped the exact owned compiler and fresh database; its exit was zero. The patched full harness was not rerun merely to exercise teardown.
- Containers and networks were retained; no old resources removed. Supporting scripts/evidence: `n3-followup-verify.ps1`, `n3-split-verify.ps1`, `n3-split-finalize.ps1`, `n3-registration-check.py`, `n3-followup-registration.json`, `n3-followup-log-audit.json`, `n3-followup-frozen-before.txt` under `Claude outputs`.
- The split reduces db_gated's measured budget pressure but does not establish a runtime guarantee or resolve the broader slowdown. No stress repeats, time-guard fix or DbClient fix were performed.
- No web/worker rerun because their sources did not change. Earlier web 873 and worker 66/66 remain prior evidence, not fresh results here. Real Push, cloud, browser/device acceptance, J3/J4/J5, restore and pre-D20 rollback gates remain deferred and unpassed. No deployment, commit, purge enablement or vault update.


## 31. Admin redemption inspection (Codex, 2026-11-12)

Implemented read-only per-couple redemption list/detail and admin UI, contract 0.49.0. Uses the existing authoritative state view and immutable product snapshot; status-filtered keyset pages, bounded event history and exact decimal-string prices. No mutations, refund/correction path, new grants or migrations. AdminFilter boundaries and late-response/auth-loss isolation retained. Evidence and limits: [admin-redemption-inspection.md](admin-redemption-inspection.md).

Disposable run usapp-e1-4dff189ce9: ADMIN_REDEMPTION 205, live HTTP 767, CTest 23/23; admin 104 tests, typechecks, isolated build; 48 frozen migrations unchanged, privileges and financial footprint unchanged. Browser/real login, production volume and deployment remain unverified. Historical reconnect, clock-guard and DbClient issues remain open. NOT DEPLOYED.


## 32. Admin user-session metadata (Codex, 2026-11-13)

Read-only per-user session metadata and admin UI, contract 0.50.0. AdminFilter, exact-user paging and identity-scoped frontend; only UUID/timestamps/revocation state, no credentials, raw device data or revocation controls. Time-valid does not imply the account may authenticate. No schema, grant, financial or session-policy change. Evidence/limits: [admin-user-sessions.md](admin-user-sessions.md).

Disposable run usapp-e1-1fc360b797: targeted 74, real API 813, CTest 24/24; admin 112 tests, typechecks, isolated build; 48 frozen migrations unchanged, privilege/ledger checks 0. Initial failure was a test-fixture SQL semicolon, corrected without product change. Browser/real-login and production-scale acceptance remain pending; historical operational issues unchanged. NOT DEPLOYED.


## 33. Admin notification / push metadata (Codex, 2026-11-13)

Read-only per-user notification paging and recorded push results, contract 0.51.0. AdminFilter, exact recipient scope, no private content/credentials, newest five delivery generations and latest attempt with total. No send/retry/mark-read/control changes, schema or grants. Admin UI labels stored facts without promising device display or future retries. Evidence/limits: [admin-notification-inspection.md](admin-notification-inspection.md).

Disposable run usapp-e1-2940666e63: targeted 150, real API 866, CTest 25/25; admin 121 tests, typechecks, isolated build; 48 frozen migrations unchanged, privilege/ledger checks 0. A triggers-off timestamp fixture was corrected before the final fresh run. Real Push/provider, browser/real-login and production scale remain unverified. Historical operational issues unchanged. NOT DEPLOYED.


## 34. Admin shared-plan inspection (Codex, 2026-11-13)

Implemented read-only per-couple plan list/detail, contract 0.52.0: authoritative current versions/status, explicit five-field bodies, newest 50 events plus total, admin auth/couple scope and frontend stale-answer protection. Memory links/ids/media and raw requests/keys excluded; O12 plan text independently retained. No writes, migration or grants. Evidence: [admin-shared-plan-inspection.md](admin-shared-plan-inspection.md).

Scoped verification: target 220, real API 942, CTest 26/26; admin 130 tests, types/build, frozen 48 migrations unchanged. **Overall harness exit 1:** the existing EXPORT_CONNECTION_LOSS mode failed 5/9 after 80 s with a slot still held; no export source change or blind rerun. Other extra modes passed. Browser/production acceptance and existing release gates remain open. NOT DEPLOYED.

## 35. Test observer ownership (Codex, 2026-11-13)

Test-support correction only: deterministic callback-tail abort control (exit 134), owner-thread close-before-release control (100 rounds), and a shared Observer lifetime anchor with no retained-client leak. New observer_lifetime_db: 78 checks, 30 s limit unchanged, outside default db_gated. Run usapp-e1-f6a24ffdab built and passed targeted suites/HTTP 942. Full CTest **26/27**, not green: db_gated completed (14.68 s), lifecycle target passed (0.20 s), but push_dispatch cutover registration failed USAPP_PUSH_COMMAND_TARGET; exact cause unproven. A separate fresh-tmpfs export continuation passed 449/8/6/23; it does not replace the failed full result or prove the intermittent reconnect stall fixed. No product, schema, contract, dependency, timeout or web change; not deployed/committed. See [test observer lifecycle](test-observer-lifecycle.md) and Claude outputs/observer-lifecycle-20261113.

## 36. Push command timestamp (Codex, 2026-11-14)

New 0048 applies the established guard-assigned timestamp convention to registration command recorded_at only. Omitted/DEFAULT/NULL becomes max(clock, activation) after the original lock and actor/session checks; supplied times stay strict. Old 48 files, grants, activation/revocation, dispatch eligibility/TTL, keys and API contract 0.52.0 unchanged. Controlled clocks reproduced both original refusal paths and verified the fix under both roles; the exact historical failure predicate remains uncaptured. New full run usapp-e1-e990d56b9b **27/27 CTest passed**, whole harness exit 0, 49 checksums and zero privilege/postcondition deviations; HTTP 942, export loss/large/closure passed. The old 26/27 run remains failed in its record. No live/provider acceptance or deployment. Next migration 0049. See [push command timestamp](push-command-timestamp.md).

## 37. Admin album inspection (Codex, 2026-11-14)

Read-only per-couple album list/detail plus current/removal item pages, metadata only, exact parent/cursor scope and AdminFilter identity. Deleted/purged media are omitted; recovery hold hides album content through existing RLS. No image-content grants or restored partner access. Admin page has independent read identities, auth-loss/404/hold clearing and existing table/pager styles. Contract 0.53.0; no migration/grant/dependency/timeout change, 49 frozen files unchanged. Full usapp-e1-81c4e573b9 passed: album 181, HTTP 1045, CTest **28/28**, all postchecks zero, export loss/large/closure and closed-album mode passed. Admin 149 tests, typechecks and isolated build passed; dist unchanged. No browser/real-provider acceptance, deployment or commit. See [album inspection](admin-album-inspection.md); evidence Claude outputs/admin-album-20261114.


## 38. Admin memory inspection (Codex, 2026-11-14)

Read-only per-couple memory list/detail, edit-request history and original/proposed comparison. Deleted/purged parents hide
all historical text; recovery-held data stays hidden through RLS. Deleted photos become position-only unavailable markers
in every snapshot. Historical approvals are labelled using that response's current_version. Four independent frontend read
identities, auth-loss/404/hold clearing and existing table/pager styles; no write controls or image grants. Contract 0.54.0;
no migration/grant/dependency/timeout change. Run usapp-e1-3d71f582b6: target 193, real HTTP 1156, **CTest 29/29**, postchecks
zero, export extras and recovery-closed memory 8 passed. Admin 171 tests, both typechecks and isolated build passed; original
dist and all 49 migration files unchanged. Final diagnostic-only label correction rebuilt/static checked. No browser,
real-login, production/provider acceptance, deployment or commit. Existing release gates remain open. See
[inspection evidence and limits](admin-memory-inspection.md); evidence Claude outputs/admin-memory-20261114.


## 39. Admin anniversary inspection (Codex, 2026-11-14)

Read-only per-couple anniversary/countdown list and detail; current terms, one pending full replacement and most recently
decided proposal summary (not complete history). Independent admin identity/surface, immutable creation-time pagination,
strict parent/cursor scope, no pins/notifications/keys, no edits/decisions or new deletion/hold semantics. Admin UI separates
current, pending and latest-decision sections, with independent list/detail sequencing and auth-loss/404 clearing. Contract
0.55.0; no migration/grant/dependency/timeout change. Run usapp-e1-a770a904fa: target 93, HTTP 1228, **CTest 30/30**, zero
postcondition deviations, existing export extras and closed media-domain checks passed. Admin 187 tests/13 files, types and
isolated build passed; original dist and 49 migrations unchanged. Not browser/real-login/provider accepted, deployed or
committed. See [inspection evidence and limits](admin-anniversary-inspection.md), evidence Claude outputs/admin-anniversary-20261114.


## 40. Admin reward product inspection (Codex, 2026-11-14)

Read-only per-couple reward catalog list/detail, including archived metadata under independent admin authority. Highest
revision gives current terms, flower-price decimal string, direct/request mode, creator/owner and times. Existing ordinary
D14 visibility and historical redemption snapshots unchanged. Immutable creation-time pagination, strict cursor/context
scope, no keys, write controls, asset correction or exception-handling workflow. Admin page has independent list/detail
identities with auth-loss/404/reset clearing. Contract 0.56.0; no migration/grant/dependency/timeout change. Run
usapp-e1-377da79553 passed: target 91, HTTP 1300, **CTest 31/31**, zero postcondition deviations, export extras and closed
media-domain checks passed. Admin 203 tests/14 files, types/build and YAML/ref check passed; original dist and 49 migrations
unchanged. No browser/real-login/production/provider acceptance, deployment or commit. See
[inspection evidence and limits](admin-product-inspection.md); evidence Claude outputs/admin-product-20261114.

## 41. Approved admin financial exceptions and DB-backup tools (Codex, 2026-11-15)

Bounded asset corrections (0049/0050) and cancellation/refund of unfulfilled redemptions (0051) are implemented under the
owner-approved rules. Preserve true admin identity, immutable audit, original financial facts and click-only same-key
recovery of unknown outcomes. Full run usapp-e1-5f6305059c passed CTest 33/33, HTTP 1393 and postconditions; admin 233/web
958, types/build passed. Original 52 migration bytes and dist unchanged. Independent synthetic signed/encrypted DB-backup
restore passed in usapp-backup-probe-b413dcc15f with application CONNECT left denied. DB-only proof does not close J5,
object recovery or off-site/key-custody/scheduling/provider/device/release gates. V1 is not complete or deployed.
See [financial exception proof](admin-redemption-exception-design.md), [backup tools and limits](database-backup-tools.md),
and the [active V1 completion ledger](v1-completion-plan.md). Earlier failed attempts are retained in these records.

## 42. Mandatory personal-security notifications (Codex, 2026-11-15)

Owner-approved first version: admin wallet correction to the corrected user, admin cancellation/refund to the original
redeemer, and each new authenticated session to its own user. Same-transaction mandatory rows, source/link guards and
unique identities; no opt-out, backfill, private reason/token/IP/agent payload, inferred anomaly scoring or freeze/debit.
No-space users can discover and acknowledge their own login notices; old space-scoped families keep their membership
boundary. Fixed account push target, existing provider/marker/cutover rules. Contract 0.59.0, new frozen 0052 (53 files,
next 0053). Final usapp-e1-84f38c2569 passed target 32, HTTP 1417, CTest 34/34, whole harness and zero postconditions.
Web 968, worker 67, types/build and 53-migration independent DB restore passed. Old clock-guard failure remains open;
real browser/device/provider, object/journal and deployment acceptance are not claimed. See
[full proof and limits](personal-security-notifications.md) and [remaining clock work](v1-clock-hardening-followup.md).


## 43. Memory decision automatic timestamp correction (Codex, 2026-11-15)

Additive frozen 0053 (54 migrations, next 0054), contract unchanged 0.59.0. Deterministic rollback clocks reproduce old
source-ahead and pre-trigger-default rewind failures; automatic omitted/DEFAULT/NULL times are assigned after the same
lock/deletion/actor/stale checks. Strict supplied-time checks remain unchanged. Both roles pass 13 baseline/fixed
controls; full usapp-e1-2cb34d2d2d passed CTest 34/34 (106.71 s), HTTP 1417, memory deletion 1048 and whole harness.
No old case removed, one static boundary case added. Independent 54-migration backup/restore f02a7deef0 passed with guard/default checks and app CONNECT denied. Other clock guards,
provider/device/object/journal/release gates are not waived. See [proof and limits](v1-clock-hardening-followup.md).


## 44. Authenticated read-only admin operational snapshot (Codex, 2026-11-15)

Contract 0.60.0, no migration (54 frozen through 0053, next 0054). Overview now reads recorded migration count/latest
and allowlisted current-process settings/capability, with explicit separation from worker liveness, checksums, delivery,
backup freshness and release readiness. No secret/raw-log output, online flag change, shell or backup/export execution.
Old asset-correction placeholder replaced by the existing user-selection entry. Session/epoch/sequence-scoped state.
Final 671835cce9 passed full CTest 34/34 (105.23 s), HTTP 1465 and whole harness; all prior cases retained. Admin 243,
types/isolated build (original dist unchanged), strict YAML/ref check passed. Not browser/provider/production acceptance.
See [implementation, proof and limits](admin-system-status.md); V1 operational/release gates remain open.


## 45. Partial J3c: refusal-only API startup layer (Codex, 2026-11-15)

Before store configuration/listeners/jobs/bootstrap, refuse closed or unconfirmed recovery observations and requests for
real media/purge while no durable Journal exists. No production provider, external Source/head/restore binding or reopen
protocol; an open restored marker is not release permission. No migration or contract change (54 / 0.60.0, next 0054).
Full ab46c00368 passed CTest 35/35 (112.82 s), HTTP 1465, open/closed DB 9/9 and three real process refusal/port probes.
Independent 41bd74214f passed real blocking-query timeout and missing-function refusal, cleanup and zero privileges.
No existing case removed. Not deployed/committed or real-provider/object/journal restore accepted. See [scope and remaining
required work](deletion-startup-refusal.md). Full J3c/J4/J5 and V1 release gates stay open.


## 46. Injected journal wire/adapter component (Codex, 2026-11-15)

C++ Journal adapter and Python injected publication dispatcher with authenticated raw-byte/request/journal/nonce and
full-operation binding, conservative unknown/terminal states and validated operation lookup. No network/SDK/env factory,
production main wiring, recovery Source or release permission. Python 74 and actual compiled C++/Python synthetic
12-request/15-assertion transcript passed; full 169cf84da7 passed 35/35 (108.95 s), HTTP 1465 and whole harness. All
605 old cases retained plus five unit cases. Contract/migrations 0.60.0/54 unchanged; next 0054. Full J3c/J4/J5 remain
open; no cloud/deployment/commit. See [wire scope, proof and required integration](deletion-journal-bridge.md).


## 47. Journal HTTPS client Exchange (Codex, 2026-11-15)

Programmatic HTTPS-only client with mandatory peer/name verification, fixed path, no proxy/redirect/retry, bounded I/O
admission/headers/body, absolute steady-clock deadline/cancellation and callback-safe owner destruction. I/O-thread-only
SIGPIPE mask, no process policy override. Final 07bc76b458 passed new target 74 assertions/9 cases, full 36/36
(112.04 s), HTTP 1465, prior wire/startup/extras and zero postconditions; all old cases retained. Earlier first snapshot
passed before final hardening, kept as history. No contract/migration/dependency change, Python HTTP/backend factory,
main provider wiring, production or recovery release. See [scope and proof](journal-https-transport.md); full J3c/J4/J5 remain open.

## 48. Injected Python HTTPS journal service (Codex, 2026-11-16)

TLS-only private-address listener, strict authenticated bounded HTTP frames and one serialized publication worker.
Deadline/disconnect never frees active publication or manufactures an aborted outcome; close reports network/backend
drain separately. Real C++ HTTPS Exchange -> Python Dispatcher -> injected publication model passed 14 assertions,
2 operations/4 batches, non-serving. Final 35fb10e7d8 passed host/container Python 92 each, CTest 36/36 (105.64 s),
HTTP 1465 and whole harness; all 619 ordinary cases retained unchanged. Intermediate compile failure was a missing
test include; earlier scoped 91-test proof predates the final admission-deadline regression and is not final evidence.
No contract/migration/grant change (0.60.0/54, next 0054), frontend/deployment/commit or cloud-resource access. Temporary
synthetic keys removed, owned containers stopped. Real backend/credential factory, main provider wiring, independent
Source/head/incarnation/restore binding, full J3c/J4/J5 remain open. See [evidence and limits](journal-https-service.md).

## 49. Restricted SDK journal backend composition (Codex, 2026-11-16)

Programmatic existing TableStore/object adapters with distinct publication/checkpoint credentials, independent candidate
readback, first-valid-operation worker preflight, no automatic initialization, managed dispatch and client-pool drain.
Slow cleanup retains one worker and shares the close deadline; no pool teardown while publication remains active.
Final scoped f7a3bf8752 passed host/container Python 125 each, actual C++ HTTPS 14 assertions against conditional metadata
fakes, full pinned SDK offline 184 reported/4 explicit old local-TLS skips. Two operations/four batches, one preflight,
validator PUT 0, network/backend/resources drains true, non-serving. Existing C++ binary reused after source checks;
previous full CTest36/36/HTTP1465 remains historical and was NOT rerun. No C++/frontend/contract/migration/grant change
(0.60.0/54, next0054), cloud resource calls/credentials, commit or deployment. Default transports still blocked and
resource allowlists remain trial-only. Production identity/credential/transport/main, independent Source/head/restore
binding and full J3c/J4/J5 remain open. See [evidence and limits](journal-backend-composition.md).

## 50. Read-only external recovery material (Codex, 2026-11-16)

Dedicated read-only SDK/table/object facades and recovery frame domain/context. Full chain/index validation followed by
complete fresh metadata recheck; missing/corrupt/moving/expired reads stay unknown. V2 batch head binding and per-subject
intent ids projected to staging shape, whole-frame cap, no release or initialization. Direct and HTTP reads retain owned
resources through completion. Final scoped 0d87082007 passed Python149 each, pinned SDK188 reported/4 explicit old local-
TLS skips and existing publication C++ interop14; not C++ recovery Source interop. Two intended negative controls drove
deadline next-call and direct-read lease fixes. No C++/DB/frontend/schema/contract/grant change (0.60.0/54, next0054),
cloud resources, commit or deployment. Previous full36/36/HTTP1465 not rerun. Independent active incarnation/current
authority, C++ Source, main/release and J3c/J4/J5 stay open. Prior stopped PG used tmpfs; historical CLOSED is not retained
data or a restorable fixture. See [proof and remaining boundaries](journal-recovery-reader.md).

## 51. C++ authenticated recovery Source (Codex, 2026-11-16)

Pinned WireSource consumes the Python recovery frame with independent MAC domain, fresh nonce/digest/full context,
strict checkpoint/projection and callback guards. Source now must bind the full restore Request; coordinator checks
after confirmed close and before final head. Context mismatch/revocation/head change cannot record success or reopen.
Initial5d75 full green preceded a newly reproduced signed-NUL parser gap (3/3187 unit failures). Explicit NUL refusal
and regressions added to both Source/publication consumers. Final2423c0644f passed unit3189/eight new cases, full36/36
(106.03 s), HTTP1465, recovery HTTPS21 (fake recovery Store), publication14, Python149 twice and complete harness.
619 old cases retained +8=627; 54 migration bytes, contract0.60.0/privileges/timeouts unchanged, next0054. Keys removed,
owned containers stopped (tmpfs data not retained). Older35fb compiler was mutated for probes; use latest2423 proof.
No production Source/main/release factory, real Source/Postgres recovery-only drill, cloud/provider/restore acceptance,
deployment or commit. Full J3c/J4/J5 remain open; see [implementation and evidence](recovery-source-bridge.md).
