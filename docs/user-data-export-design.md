# User data export: E1 structured export (owner-approved design, implemented by Claude Code)

**Status (2026-11-10):**
- **Decisions:** the owner approved Q1–Q6 (Section 3) and authorized E1 implementation and automated verification.
- **Corrections:** this revision corrects the first proposal (Section 0) and records the implemented limits.
- **Evidence:** Section 11.
- **Baseline:** contract 0.45.0 → 0.46.0 (additive). No migration, grant or dependency; migrations 0000–0047 unchanged, and 0048 is not reserved.
- **Not done:** no photo binaries, provider calls, jobs, server artifacts or deployment.

**Labels:**
- **[fact]**: inspected in the repository.
- **[implemented]**: in E1 source.
- **[executed]**: observed in a recorded run (Section 11).
- **[reasoned]**: argued from code, not tested.

## 0. Corrections to the first proposal

1. **Resource bounds.**
   - A final `octet_length` check after aggregation bounds the response, not peak database memory.
   - E1 now builds every list section into a capped (`LIMIT cap + 1`), materialized row set.
   - It measures row counts and serialized row bytes **before** any array aggregation, and gates the aggregation behind those checks.
   - The exact final UTF-8 size check is kept.
   - An end-to-end deadline is added, distinct from the lock and statement timeouts (Section 7).
2. **Download client.** `request()` returns parsed JSON, so E1 adds a narrowly scoped `requestDownload()` that returns the raw bytes. `request()` is unchanged (Section 8.2).
3. **Media entitlement.**
   - `NOT deleted_now` alone is not enough.
   - Every photo reference uses the media read path's current-visibility predicate. A rejected or withdrawn proposal can therefore never expose the partner's private upload (Section 6.3).
4. **Recovery lock.**
   - The shared transaction lock is a separate statement, before generation.
   - The transaction is READ ONLY at READ COMMITTED, so generation's snapshot starts after the lock is held.
   - The lock covers only the database transaction (Section 6.1).
5. **Evidence wording.**
   - The export **does** disclose data the API never returned: the caller's own wallet ledger, and history beyond UI windows such as expired tasks older than 30 days.
   - `usapp_achievement_facts` is IMMUTABLE, not STABLE.
   - Static reasoning and executed evidence are now separated.
6. **Product visibility.** Products follow D14: the creator sees all their products, and the owner sees active ones only. The first matrix did not say this.

## 1. Requirement source

[fact] `app开发方案V1.docx`:
- **Section 39** requires "支持导出用户数据" (support exporting user data).
- **Section 36 (admin) and Phase 5** list "数据导出" (data export) again, with no detail.
- **Principles 5–6:** two equal partners, one private shared space.

Admin export is out of scope.

## 2. Facts that constrain the design

- **One space per user:** `couple_members.user_id` is UNIQUE, with no route to leave a space. The current space is the user's only space.
- **What can be deleted:** only memories and photos.
  - Mechanisms: deletion cycles with undo windows; partner delete requests; the creator-photo cascade; erasure, where `usapp_memory_erase` sets every text the memory used to `[erased]`/'' and zeroes fingerprints.
  - Every other domain is append-only.
- **Deletion predicates:** `usapp_memory_deleted_now` / `usapp_media_deleted_now` are true for an active cycle (undoable or purged) **or** while recovery is closed (`0042`).
- **J3 hold:**
  - `usapp_recovery_closed()` (a SECURITY DEFINER boolean) is true once any recovery attempt exists.
  - 32 media/memory/album/link tables then carry a RESTRICTIVE RLS policy hiding all rows from `usapp_api`, and a write trigger.
  - Writers take shared 33026; closure takes it exclusive.
  - Notifications, `shared_plans` and `shared_plan_events` are not held.
- **Media visibility** (`MediaService.cc`, `visibleTo`):
  - The uploader always sees their own photo.
  - Anyone else sees it only if it is shared now (bound to a live memory, or an active album item) or reserved by a pending proposal, and also READY and not being removed.
  - Nobody sees a deleted photo.
- **Products (D14):** `creator_id = caller OR (owner_id = caller AND latest status = 'active')`.
- **Redemptions:** the caller is the creator or the redeemer.
- **Caller-only reads:** wallet, avatar, achievements, reminder rules, notifications.
- **Shared reads, both members:** tasks, task changes, reactions, messages, anniversaries, memories, albums and plans.
- **Text and row limits:**
  - Text limits: task description 2000, message 500, reply 100, memory title/description 60/2000, album name 40, anniversary title/note 40/500, plan title/place/note 80/120/2000 (code points). Photos per memory: 9.
  - Row counts are unbounded.
  - DB pool default: 8 connections.

## 3. Owner decisions (approved 2026-11-10)

| Decision | Approved rule | User-visible consequence |
|---|---|---|
| **Q1 = B** | The caller's own account data, plus the currently authorized shared-space records, including the partner's contributions to them. The partner's private account data is excluded: wallet, purchases, loadouts, achievements, reminder rules, notifications, email | The partner's shared words appear in the caller's file and cannot be withdrawn after download |
| **Q2** | Currently deleted memories and photos are excluded, including undoable deletions and all of their history | An undo restores them in a later export |
| **Q3** | History of live, authorized records is included: revisions, proposals and decisions | |
| **Q4** | Expired tasks are included without the UI's 30-day window | This is an export disclosure beyond what the list UI shows |
| **Q5** | No database audit write. One process log line with user id, outcome, row count and byte count; no content | |
| **Q6** | Structured data only: no photo binaries, provider calls, signed URLs or asynchronous artifacts | |

**New disclosures relative to the current API:**
- the caller's own wallet ledger entries;
- expired tasks older than 30 days;
- full anniversary, memory, album and plan history in one file.

Each is approved under Q1, Q3 and Q4. None exposes another user's private data.

## 4. Field allowlist (implemented)

The document is built from explicit `json_build_object` field lists; no whole row is ever dumped.

**Never exported:**
- any `idempotency_key`, `request_key`, `request_fingerprint` or `create_*` key/fingerprint;
- raw `request` jsonb;
- `lock_token`, `staging_key`, object or `final_key` values, signed URLs;
- session, CSRF, code, invite and push-device data;
- admin and audit rows;
- `couple_id` columns (the space id appears once);
- `text_id`;
- reserved encryption columns;
- all deletion, purge, recovery, notification, intent, reminder-occurrence, cleanup and dispatch tables.

| Section | Rows | Fields |
|---|---|---|
| `account` | the caller | `id`, `email` (display form), `display_name`, `status`, `created_at` |
| `space` | the caller's couple | `id`, `name`, `timezone`, `created_by_user_id`, `created_at`, `members[]` (`user_id`, `slot`, `display_name`, `joined_at`; never the partner's email) |
| `tasks` | every task of the couple, expired included | `id`, `creator_id`, `assignee_id`, `title`, `description`, `reward_flowers`, `reward_sunshine`, `timezone`, `deadline_local_date`, `deadline_local_time`, `deadline_at`, `completion_mode`, `target_count`, `schedule_type`, `recurrence_id`, `period_date`, `start_local_date`, `created_at`. No derived status: completion, progress and closure are their own sections |
| `task_completions` | couple | `task_id`, `completed_by`, `reward_flowers`, `reward_sunshine`, `completed_at` |
| `task_progress` | couple | `id`, `task_id`, `user_id`, `progress_count`, `target_count`, `recorded_at` |
| `task_changes` | couple | proposal: `id`, `task_id`, `proposer_id`, `kind`, `title`, `description`, `reward_flowers`, `reward_sunshine`, `completion_mode`, `target_count`, `deadline_local_date`, `deadline_local_time`, `start_local_date`, `created_at`; `decision` (`decided_by`, `outcome`, `successor_task_id`, `decided_at`) or null |
| `task_closures` | couple | `task_id`, `reason`, `proposal_id`, `closed_at` |
| `task_reactions` | couple | `id`, `completion_task_id`, `author_id`, `kind`, `emoji_code`, `body`, `created_at` |
| `reminder_rules` | **the caller's own** | `id`, `task_id`, `anniversary_id`, `seq`, `enabled`, `mode`, `before_minutes`, `days_before`, `at_time`, `at_local_date`, `days_before_set`, `origin`, `created_at` |
| `wallet` | **the caller's own** | `balances` (`flower`, `sunshine`, the latest `balance_after` per asset); `ledger[]`: `id`, `asset_type`, `seq`, `amount`, `balance_after`, `created_at`, `operation_id`, `operation_type`, `source_type`, `source_id` |
| `reward_products` | D14-visible | `id`, `creator_id`, `owner_id`, `created_at` |
| `reward_product_revisions` | revisions of D14-visible products | `product_id`, `version`, `actor_id`, `action`, `title`, `description`, `price_flowers`, `redemption_mode`, `status`, `created_at` |
| `reward_redemptions` | caller is creator or redeemer | snapshot `id`, `product_id`, `product_version`, `creator_id`, `redeemer_id`, `title`, `description`, `price_flowers`, `redemption_mode`, `created_at`; `fulfillment` (`fulfilled_by`, `fulfilled_at`) or null |
| `reward_redemption_events` | events of those redemptions | `id`, `redemption_id`, `actor_id`, `action`, `proposal_id`, `created_at` |
| `messages` | couple | `id`, `author_id`, `recipient_id`, `body`, `created_at`; `reply` (`author_id`, `body`, `created_at`) or null |
| `anniversaries` | couple | `id`, `creator_id`, `created_at` |
| `anniversary_revisions` | couple | `anniversary_id`, `version`, `actor_id`, `action`, `proposal_id`, `title`, `event_date`, `kind`, `note`, `created_at` |
| `anniversary_proposals` | couple | `id`, `anniversary_id`, `proposer_id`, `base_version`, `title`, `event_date`, `kind`, `note`, `created_at`, `decision` |
| `anniversary_home_pin` | the caller's latest pin | `anniversary_id`, `seq`, `created_at`, or null |
| `memories` | `{state, items}`: live memories only (Q2) | `id`, `creator_id`, `created_at` |
| `memory_revisions` | revisions of live memories | `memory_id`, `version`, `actor_id`, `action`, `proposal_id`, `memory_date`, `title`, `description`, `created_at`, `photos[]` (6.3) |
| `memory_proposals` | proposals on live memories | `id`, `memory_id`, `proposer_id`, `base_version`, `memory_date`, `title`, `description`, `created_at`, `photos[]`, `decision` |
| `albums` | `{state, items}` | `id`, `creator_id`, `created_at` |
| `album_revisions` | couple albums | `album_id`, `version`, `actor_id`, `action`, `proposal_id`, `name`, `created_at` |
| `album_proposals` | couple albums | `id`, `album_id`, `proposer_id`, `base_version`, `name`, `created_at`, `decision` |
| `album_items` | active items only: not removed, not deleted, visible | `album_id`, `seq`, `added_by`, `created_at`, `photo`. Cover, removal and batch history are excluded in E1 |
| `shared_plans` | couple | `id`, `creator_id`, `created_at`, current `body`, `body_version`, `status`, `status_version`, `memory_link` (6.3) |
| `shared_plan_events` | couple | `id`, `plan_id`, `seq`, `actor_id`, `operation`, `body_version`, `status_version`, `status`, `body`, `expected_version` (numeric only), `proposal_id`, `outcome`, `created_at` |
| `avatar_purchases` | **the caller's own** | `id`, `item_id`, `item_version`, `name`, `category`, `asset_key` (public bundled name), `price_sunshine`, `created_at` |
| `avatar_loadouts` | **the caller's own** | `version`, six slot ids, `requested`, `starter_version`, `created_at` |
| `achievements` | the caller's, derived | `derived: true`, `enabled`, `enabled_at`, plus the facts from `usapp_achievement_facts` |

**Plan body allowlist:** every plan `body` is rebuilt from exactly `title`, `date`, `time`, `place` and `note`.

**O12 (kept):** plan text survives deletion or erasure of a memory converted from the plan. This is existing behaviour, not changed by E1. The text the plan carries is exported; the deleted memory is not.

## 5. Format, ordering and versioning

[implemented] **The file:**
- one UTF-8 JSON object, `application/json; charset=utf-8`;
- an attachment named `usapp-export-<YYYYMMDDTHHMMSSZ>.json`;
- `"export": {format: "usapp-user-export", schema_version: 1, generated_at, subject_user_id, scope: "own_and_shared_space", photos_included: false, complete: true}`.

Sections follow in the fixed order of Section 4. An empty section is `[]`; a memory or album section under a hold is `{"state": "unavailable", "items": []}`.

**Ordering:** every array is ordered by stable keys (`created_at, id`; versions; `asset_type, seq`). Two exports of an unchanged database differ only in `generated_at` and the file name.

**Format details:**
- Timestamps are UTC microseconds with a `Z` suffix; dates are `YYYY-MM-DD`.
- The output uses `json` (not `jsonb`), so key order is as written.

**Versioning:** `schema_version` is an integer. A new field or section is additive; any rename, removal or change of meaning increments it. The contract carries the v1 schema.

## 6. Consistency, deletion and recovery-hold model

### 6.1 Transaction, lock and snapshot

[implemented] Steps, in order:
1. **Admission:** the process-local gate (Section 7).
2. **Start the deadline timer.**
3. **Acquire a pooled connection and begin a transaction.**
4. `SET TRANSACTION ISOLATION LEVEL READ COMMITTED, READ ONLY`.
5. `SELECT set_config('lock_timeout', …, true), set_config('statement_timeout', …, true)`.
6. `SELECT pg_advisory_xact_lock_shared(33026, 0)`, a statement of its own.
7. **The generation statement:** one statement with membership, all sections, caps and the gated aggregation.
8. **Request a rollback**, which releases the lock.

At READ COMMITTED, each statement takes its own snapshot when it starts. Step 7 therefore sees every closure committed before the lock was granted, and none can commit while it runs: closure needs 33026 exclusive.

**Scope of the lock:** the database transaction only. It does not cover:
- the HTTP transfer after the rollback request;
- the browser's buffering;
- the saved file.

`Cache-Control: no-store` keeps HTTP caches from storing the response. It does not revoke, expire or erase a downloaded file or any copy of it.

### 6.2 Ordinary deletion and erasure

- **Before the snapshot:** a deletion, undo or erasure committed before step 7 starts is reflected.
- **After the snapshot:** one committed after step 7 starts is not. The file shows the state just before it, as a page loaded a moment earlier would.
- **Readers do not block writers.** The shared 33026 lock does not conflict with writers' shared locks, and E1 takes no row or domain locks.

### 6.3 References and the media predicate

**Memories:** a memory, with all its revisions, proposals, proposal texts and photos, is exported only if `NOT usapp_memory_deleted_now(id)`. Erased text is never reached, because a purged memory stays deleted now.

**`visible(media)`** [implemented], the one predicate for every photo reference path; it mirrors `MediaService::visibleTo`:

```
NOT usapp_media_deleted_now(o.id) AND (
  o.uploader_id = caller
  OR ((usapp_media_shared_now(o.id) OR usapp_media_pending_reference(o.id) IS NOT NULL)
      AND outcome = 'ready'
      AND NOT EXISTS (cleanup plan for o.id with scope 'everything')))
```

A visible reference carries `media_id`, `uploader_id`, `width` and `height`. It says nothing about current image-content access, and it carries no keys or URLs.

**Per path:**

| Path | Rule |
|---|---|
| `memory_revisions.photos` (live memory, any version) | The photo is bound to that memory (`media_bindings` is permanent and one per photo), so `shared_now` holds unless deleted. Visible: the reference. Otherwise `{position, photo: null, withheld: "unavailable"}` with no id |
| `memory_proposals.photos`, pending | Reserved, so visible to both |
| `memory_proposals.photos`, rejected or withdrawn | Visible to the uploader. Visible to the partner **only** if the photo became shared through another binding or active album item. Otherwise withheld. The partner's private upload never appears because of an old proposal |
| `album_items` | Only active items: not removed (`album_item_removals`) and not deleted, matching the album API. Then `visible(media)`; an invisible item is omitted, exactly as the album list omits it |
| `shared_plans.memory_link` | Under a recovery hold: `{memory_id: null, withheld: "unavailable"}` for every plan, because the link rows themselves are hidden. A deleted memory gives `{mode, memory_id: null, withheld: "unavailable"}`. A live one gives `{mode, memory_id, created_at}`. No link: null |

**Recovery closed** (`usapp_recovery_closed()` in the generation statement): `memories` and `albums` are `{state: "unavailable", items: []}`, and their history sections are empty. Other domains export normally.

### 6.4 No side effects

[implemented] The transaction is READ ONLY: any write would fail. The functions it calls:
- **STABLE reads:** the deleted-now, shared-now and pending-reference predicates, and `usapp_recovery_closed`;
- **IMMUTABLE:** `usapp_achievement_facts`.

No notification, intent, wallet operation, audit row, achievement activation, journal call or object-store call happens. Section 11 holds the test evidence.

## 7. Limits (implemented defaults)

| Bound | Default | What it covers | On breach |
|---|---|---|---|
| Admission, per user | 1 in flight | process-local | 429 `RATE_LIMITED`, `Retry-After` |
| Admission, global | 2 in flight | process-local; one dedicated session per slot (Section 7.1), outside the shared pool | 429 |
| Frequency, per user | 1 admission per 60 s | process-local, counted from admission whatever the outcome | 429 with the remaining seconds |
| End-to-end deadline | 20 s | from admission: tagging, BEGIN, settings, lock wait, generation and evaluation on the slot session | 503 `EXPORT_UNAVAILABLE`; may also queue cleanup after generation submission, but never releases without end proof (Section 7.1) |
| Lock wait | `lock_timeout` 5 s | the shared 33026 wait only | 503 |
| Statement | `statement_timeout` 15 s | each statement after it is set, mainly generation | 503 |
| Rows per section | 20 000 | each list section, measured on a `LIMIT 20 001` row set before aggregation | 413 `EXPORT_TOO_LARGE` |
| Total rows | 60 000 | sum over sections, before aggregation | 413 |
| Input bytes | 15 MiB | sum of each row's serialized JSON bytes, before array aggregation | 413 |
| Final response | 16 MiB | exact UTF-8 bytes of the document sent | 413 |

**How the bounds hold:**
- **Gated aggregation:** the document is built once, in a materialized CTE, inside `CASE WHEN (SELECT status FROM verdict) = 'ok' THEN … END`. PostgreSQL evaluates only the CASE branch it needs, so no section array aggregation runs when a check fails. Executed evidence is in Section 11.3. The first form, a lateral `WHERE`, was planned as a join filter, and the document expression was duplicated; see Section 11.2.
- **Peak database memory:** per-row JSON is built inside the capped row sets. Row text is schema-bounded and photo arrays have at most 9 entries. The materialized row sets spill to disk beyond `work_mem`; only the gated aggregation holds the whole document [reasoned].
- **API process memory (correction, 2026-11-11).** An earlier revision claimed the API process holds at most 2 × 16 MiB of documents. That claim is withdrawn; it was never measured and does not follow from the gate.
  - **What the gate bounds:** at most 2 *generations* in flight per process, each on its own session.
  - **Copies per export [reasoned, unmeasured]:** PostgreSQL builds the document text, so the database server holds it. libpq receives the result row into the API process. Drogon's `as<std::string>()` copies it into the service's string, which is then moved into the HTTP response body. Drogon's send path and the kernel socket buffers may hold further copies while it is transmitted.
  - **After release:** a slot is released when the transaction ends, which can be before the HTTP transfer of an earlier answer finishes, so responses still being sent are outside the gate.
  - No hard memory figure is claimed.

**Process-local limits:**
- The gate, frequency map and timers are per process. With several API processes they multiply.
- No cross-process enforcement is claimed.

### 7.1 Dedicated sessions and the transaction end (correction, 2026-11-11)

**What the installed Drogon offers** (1.9.11; only headers are installed in the build image, with no source):
- `Transaction::rollback()` has no completion signal.
- `setCommitCallback(bool)` reports commit only.
- What Drogon does after a failed statement inside a `Transaction` cannot be confirmed from headers.

So neither the first implementation's immediate release after requesting a rollback, nor a release in that request's own callback, proved the transaction had ended.

**Sessions [implemented]:**
- `ExportService::init(connection, 2)` (called from `main.cc`) creates one long-lived **single-connection** Drogon client per admission slot. Each is one extra PostgreSQL connection per API process, kept for the process lifetime, outside the shared pool.
- Each client runs this service's statements strictly one after another, and Drogon reconnects it by itself after a connection loss.
- The clients are held in a deliberately leaked heap vector and are never destroyed, at runtime or during static destruction. So no last reference can be dropped on a client's own event-loop thread (compare the recorded DbClient teardown crash). A static check forbids any removal or reset of them.

**Statement order per export:**
1. `SELECT set_config('application_name', <token>, false)`: a fresh random token per export, session-level, set **before** any transaction exists.
2. `BEGIN ISOLATION LEVEL READ COMMITTED, READ ONLY`.
3. Settings, with a check that this is still the tagged session inside the read-only transaction. The pid is captured here as a diagnostic only.
4. `pg_advisory_xact_lock_shared(33026, 0)`, with the tag checked again.
5. Generation, whose result also returns the tag and `transaction_read_only`, and is refused (`session_lost`) unless both match.

**Session identity is the token, not a pid.** Pids are reused, and a captured pid can name an unrelated same-role session. Because the token is set before `BEGIN`, a lost `BEGIN` reply still leaves the original session identifiable.

**The end and its confirmation [implemented]:**
- **Answer first:** every admitted export answers its caller once, then ends with a verification round on the slot connection: `ROLLBACK`, then a check of three things:
  - whether the current session carries the token;
  - how many *other* backends carrying the token are in any state but `idle`;
  - whether any backend carrying the token holds the 33026 lock.
- **Why the `ROLLBACK` reply counts:** if the check runs on the tagged session, the `ROLLBACK` before it ran on that same session, because a reconnect yields an untagged session and nothing re-tags it. So its success is the server's confirmation. A `ROLLBACK` with no transaction open only warns.
- **Release:** the slot, and with it the user's admission, is released **exactly once**, when no other tagged backend is open and no tagged backend holds the lock. The cleanup label is `ended` on the tagged session, or `session_replaced` after a reconnect.
- **Otherwise:** the slot stays held in state `unconfirmed`, with one warning log line, and the round is retried on a timer (50 ms doubling to 2 s), never blocking a thread.
- **No termination:** this service never terminates any backend. An earlier revision of this correction terminated the "old pid"; that was removed before verification, because a pid alone is not durable ownership.
- **Outside the mutex:** callbacks to the caller, and the test observers, run outside the admission mutex.

**Deadline versus cleanup (updated 2026-11-13):** the deadline answers the user and, once the final generation statement has been submitted, also requests the existing ordered cleanup. Before that stage it answers only, because earlier callbacks could still enqueue subsequent stages. The slot is released only by the end confirmation. Server-side `lock_timeout` and `statement_timeout` remain unchanged. The client is configured with a 35 s timeout (`deadline + statement`), but the missing-callback probe shows that configuration is not a reliable bound on receiving a callback after connection loss. No maximum slot-recovery time is promised.

**Connection loss and availability limits (honest):**
- After a server-side loss, how long Drogon's client takes to notice and reconnect varies. Observed: about 1 s in two runs, and over 20 s in one CTest pass of run `usapp-e1-09bfa86258`, where the deadline answered at 20 s.
- During that time the slot is held. With 2 slots, two such losses refuse every export in that process until they clear.
- If the original session lingers half-open (for example, a client-side network break the server has not noticed), the slot stays `unconfirmed` until PostgreSQL notices: TCP keepalive or the next write. Recovery is automatic once the session is gone or idle. An operator may end such a session; the service does not.

**Failure:** every failure returns an error envelope and **no document**, never a partial archive. A returned document is complete by construction. Retry is a new export after an explicit click; there is no idempotency key, because nothing is written.

## 8. API and UI

### 8.1 API (contract 0.46.0) [implemented]

`POST /api/v1/me/export`, behind AuthFilter, so the existing exact Origin and CSRF checks apply.

**Body:** absent, or exactly `{}`. Any member (for example `user_id` or `scope`), a non-object or invalid JSON gives 400 `VALIDATION_ERROR`. No Idempotency-Key.

**200 response:**
- the document;
- `Content-Type: application/json; charset=utf-8`;
- `Content-Disposition: attachment; filename="usapp-export-…json"`;
- the existing no-store, `nosniff` and `Referrer-Policy` headers.

**Errors:**

| Status | Code |
|---|---|
| 400 | `VALIDATION_ERROR` |
| 401 | `UNAUTHENTICATED` |
| 403 | `ORIGIN_NOT_ALLOWED`, `CSRF_TOKEN_INVALID` |
| 404 | `SPACE_NOT_FOUND` |
| 413 | `EXPORT_TOO_LARGE` (new) |
| 429 | `RATE_LIMITED` |
| 503 | `EXPORT_UNAVAILABLE` (new) |

The subject is always the session user.

### 8.2 Web [implemented]

**`requestDownload(path)` in `api/client.ts`:** a separate function. It keeps the same guard order as `request()`:
1. offline refusal;
2. CSRF, or no request is sent;
3. same-origin credentials;
4. a network error becomes `NETWORK_ERROR`;
5. non-2xx responses use the same error-envelope parsing.

On success it returns the response body as a `Blob`, plus the server's file name. `request()` is unchanged.

**Bounded reception (correction, 2026-11-11):**
- The first version buffered `response.blob()` before checking the size. It now reads `response.body` incrementally with a reader and counts the actual bytes.
- **Declared length:** a declared `Content-Length` above 16 MiB is refused without reading, and the body is cancelled. A missing or smaller declaration is not trusted.
- **Overflow:** when the next chunk would cross the cap, the reader is cancelled and `EXPORT_TOO_LARGE` is thrown. That chunk is not kept, and nothing is read afterwards.
- **Errors:** a read failure cancels and yields `NETWORK_ERROR`. An abort between chunks cancels and rethrows `AbortError`.
- **Never partial:** no partial Blob is ever returned, so no object URL or download starts for a refused or stale result.
- **No fallback:** without a readable stream the download fails with `DOWNLOAD_STREAM_UNSUPPORTED`; there is no unbounded fallback.

**Buffering:** the bound is on the bytes this code accumulates, at most 16 MiB in the kept chunks and then the Blob assembled from them. It does not bound the browser's or the transport's own buffering below the stream.

**Export store (`stores/export.ts`):**
- memory only: `status` (idle, working, done, failed) and an error message;
- identity generations, with `reset()` from the session's identity reset;
- `cancel()` on route leave, which aborts the fetch and ignores a late answer;
- no retry without a click;
- no localStorage, IndexedDB, Cache API or service-worker involvement (`/api/**` stays NetworkOnly).

**Saving:** a temporary object URL and an anchor with a `download` attribute. The URL is revoked on the next task.

**Cancellation:** cancelling or leaving the page cannot recall bytes already received or saved.

**MeView:** a "导出我的数据" ("export my data") card that explains:
- the scope: own account plus shared space, without the partner's private data;
- that photos are not included and deleted memories and photos are absent;
- that a downloaded file stays on the device and is not withdrawn by later deletions.

## 9. Batches

| Batch | Contents |
|---|---|
| **E1 (this)** | Everything above |
| E2 | Optional: cover, removal and batch history; notifications; an audit row. Separate approval |
| E3 | Photo binaries: asynchronous artifacts, owner, expiry, crash cleanup, deletion and J3 invalidation. Separate approval |

**Reused:**
- AuthFilter Origin/CSRF;
- no-store headers;
- `RateLimited`;
- the deletion and visibility SQL predicates;
- `usapp_recovery_closed`;
- `usapp_achievement_facts`;
- `TxScope::beginAsync`;
- the web client's guards.

**New:**
- `ExportService`, `ExportController` and the gate;
- the generation SQL;
- two error codes;
- the contract route and schema;
- `requestDownload`, the export store and the MeView card.

## 10. Acceptance tests

See Section 11 for what was executed. Planned coverage:
- **Access:** authentication, Origin/CSRF, the empty-body rule and identity injection.
- **Privacy:** caller, partner and other-couple isolation; a secret-key scan.
- **New disclosures:** the wallet ledger and the balance snapshot; old expired tasks.
- **Deleted material:** deleted, undoable, purged and erased memories; deleted photos; proposal photos.
- **Recovery hold:** a closure before generation, and both lock orders.
- **Snapshot boundary:** a deletion on either side of it.
- **No side effects.**
- **Limits:** admission, frequency, lock wait, deadline, generation timeout, row/byte caps and the final size, with no partial success; the byte agreement between the cap and the transmitted body.
- **Web:** the frontend flows.

## 11. Implementation and verification record (Claude Code, 2026-11-10 to 2026-11-11)

### 11.1 What was implemented

**Backend:**
- `services/api/src/services/ExportService.{h,cc}`: the admission gate, the deadline timer, the transaction sequence of Section 6.1 and the generation SQL (Sections 4–7).
- `services/api/src/controllers/ExportController.cc`: `POST /api/v1/me/export`, behind AuthFilter.
- `core/Errors.{h,cc}`: `EXPORT_TOO_LARGE` (413) and `EXPORT_UNAVAILABLE` (503), plus `attachmentJsonResponse`, which sends the string byte for byte with the existing no-store, nosniff and Referrer-Policy headers.
- `services/api/CMakeLists.txt`: registers the controller.

No migration, grant or dependency was added, and no existing service was changed.

**Contract:** 0.46.0. It adds the route, the two codes and the v1 schemas `UserDataExport`, `ExportRecords`, `ExportHeldSection`, `ExportPhotoHolder`, `ExportPhotoSlot` and `ExportMemoryLink`.

**Web:**
- `requestDownload` in `api/client.ts`, a new function; `request()` is unchanged;
- `api/export.ts`;
- `stores/export.ts`;
- `stores/session.ts`, which resets the export store on identity change;
- `views/MeView.vue`, the export card;
- the two codes in `api/types.ts` and `api/error-messages.ts`.

**Tests:**
- `services/api/tests/ExportDbTests.h`:
  - the main suite, CTest target `export_db` (TIMEOUT 30);
  - the harness-only `EXPORT_LARGE` and `EXPORT_CLOSURE` modes;
- `ExportLiveApiTest.h`, run inside the existing live-API step;
- `StaticCheckTest.cc`: three superseded `0.45.0` version assertions updated, plus one new E1 case;
- `DbGatedTest.cc`, which dispatches the suites;
- `tests/CMakeLists.txt`, which adds the `export_db` target;
- `apps/web/tests/unit/user-export.spec.ts` (11 tests).

**Harness and checks:** `tools/export-verify.ps1`, derived from `tools/achievement-verify.ps1`, and `Claude outputs/e1-yaml-check.py`.

### 11.2 Runs, including failures (all retained)

| Run (project) | Result and cause | Change |
|---|---|---|
| 1 `usapp-e1-ea5ada7186` | Build passed. Export suite 381/385. The lock timeout (302 ms) and the statement timeout (7 ms) both failed closed with no document, but were **labelled** `database`: SQLSTATE was not available from the Drogon PostgreSQL error. The EXPLAIN gate assertion also failed on its expected text | Classify by SQLSTATE, falling back to the fixed PostgreSQL timeout messages. The plan lines are now printed |
| 2 `usapp-e1-5f800e9659` | Labels correct. The real plan placed the gate as a **Join Filter**, not a one-time filter. The document subplans were not executed in that plan, but only because of where the planner put the projection | **Product change:** the document moved into a `CASE WHEN … = 'ok'` branch |
| 3 `usapp-e1-b50fdcd440` | The new plan check failed. A test bug miscounted lines with a wrapped `npos`, and the plan showed the **document expression duplicated**, apparently for `d.doc` and `octet_length(d.doc)`. On acceptance the document could have been built twice | **Product change:** the document is built once, in the MATERIALIZED CTE `gated`. The line match was fixed |
| 4 `usapp-e1-a8c849325e` | Export suite 406/406. The E1 live case passed. The existing **B3 loadout** live block failed 12 assertions: this harness had not run the avatar suites the previous harness runs before the live step, presumably the ones that create the starter configuration | Restored the previous suite order: export first, then the achievement and avatar suites |
| 5 `usapp-e1-c12745741a` | Everything through live passed (live 560/560). A log-count `grep` with a double-quoted pattern broke under Windows PowerShell 5.1 native argument passing | Quote-free pattern |
| 6 `usapp-e1-4276b40628` | CTest 19/20. `static_check` failed 2 of the new E1 assertions. `Idempotency` matched the controller's header comment, and the order check compared a function definition with a call site defined earlier in the file. Both were wrong test text | Assert no `getHeader`; check the order inside `begin()` |
| 7 `usapp-e1-10f23629da` | **All passed** (Section 11.3) | — |

No failure was intermittent, and none was rerun without a change. No timeout was raised, no existing assertion was weakened, and no other suite was moved.

### 11.3 Final run `usapp-e1-10f23629da` [executed]

**Setup** (ownership-checked disposable stack):
- PostgreSQL 17 on tmpfs, on an internal network with no published port;
- generated credentials;
- the existing compiler image `sha256:83afaf61…cbfb1`.

**Results:**
- **Migrations:** 48 applied unchanged and 48 checksums matched. Eraser violations 0. Privilege deviations 0 after apply, after repeat apply with InitRoles, and after CTest. All 48 migration file hashes were identical before and after the run.
- **Build:** passed.
- **Targeted suites, export first on a fresh database:** `EXPORT` 406 assertions, 4 cases; `ACHIEVEMENT` 22; `AVATAR_CATALOG` 157 (incomplete-configuration case covered); `AVATAR_PURCHASE` 114; `AVATAR_LOADOUT` 85. All passed.
- **Real API binary on its loopback:** live 560 assertions passed (the reminder/avatar block plus E1 live). The process log has 4 export lines and **0 lines containing the exported-content marker** (Q5).
- **Full CTest: 20/20 in 134.24 s.** `export_db` took 2.78 s and `db_gated` 23.69 s of its 30 s. Post-checks: early or reversed erasures 0, ledger chain breaks 0.
- **`EXPORT_LARGE`** (6 assertions passed): 8,500 synthetic messages of 500 CJK code points each.
  - **Accepted:** rows 8,500; input 14,730,500 bytes; output 14,749,868 bytes; 583 ms. This is under the 15 MiB input cap, the 16 MiB output cap and the 20 s deadline.
  - **Refused:** with 1,000 more rows, `input_bytes` at 16,463,500 bytes; 20,001 rows in one section gave `section_rows`. Neither refusal returned a document.
- **`EXPORT_CLOSURE`, run last** (23 assertions passed):
  - **Order 1:** the export held the shared lock, and a real `usapp_recovery_close` waited (`pg_locks`) until the export finished. That export saw the domain open.
  - **After closure:** memories and albums were `unavailable`, their history sections empty, every plan link withheld, and the plan text kept.
  - **Order 2:** a closure held the lock exclusively in an open transaction, and the export waited. It generated only after the commit and reported `unavailable`.
  - The database was left closed, with 2 recovery attempts; the containers are stopped.

**Gate evidence from EXPLAIN ANALYZE, plan text only:**
- Six sections were checked: `s_tasks`, `s_messages`, `s_ledger`, `s_plans`, `s_album_items` and `s_memory_revisions`.
- **Refused** (section cap 1): one counts scan ran, and the single aggregation scan was `never executed`.
- **Accepted:** both scans ran.
- So the document is built at most once, and never on refusal.

**Covered by the export suite:**
- **Scope:**
  - the partner's words in shared records;
  - D14 product visibility, where the owner does not see an archived product and its creator does;
  - Q4's 40-day-old expired task;
  - own reminder rules and avatar rows only;
  - another couple absent.
- **Wallet:** the own ledger as a new disclosure; balances equal to the wallet read and to the ledger sums and last `balance_after`; partner entry ids absent.
- **Achievements and secrets:** achievements equal to the achievement read; no forbidden key; no stored idempotency key or fingerprint value.
- **Determinism and side effects:** two exports identical apart from `generated_at`; the 15-table footprint unchanged; `SPACE_NOT_FOUND` without a space.
- **Deletion:**
  - undoable, purged/erased and plan-converted deleted memories absent, ids and text included, and `[erased]` absent;
  - exactly 3 withheld photo slots (the deleted photo in the approved revision and proposal, and the partner's rejected private upload), each with exactly the keys `position`, `photo` and `withheld`;
  - the pending proposal's photo visible;
  - the rejected upload visible in its uploader's own export;
  - the deleted album item omitted;
  - the plan link withheld with the O12 text kept;
  - undo restoring the memory in a later export.
- **Snapshot boundary:**
  - a deletion committed while the export holds the lock, before generation, is excluded;
  - a deletion after a returned export changes only the next one.
- **Bounds:**
  - `lock_timeout` at 300 ms, with no document;
  - the 400 ms deadline answered while the slot stayed held (`concurrent`), and was released after the lock;
  - `statement_timeout` on generation after the lock was taken.
- **Caps:**
  - section rows, total rows, input bytes and output bytes, each refused with no body or file name;
  - a cap equal to the exact document size was accepted with the same size;
  - frequency gave `Retry-After` between 1 and 61, and the global limit gave `global`.

**E1 live checks:** 401 without a session; 403 without Origin/CSRF, with a foreign Origin, and with a wrong CSRF token; no GET route; 400 for `user_id`, `scope`, `[]` and non-JSON bodies. The 200 response carries `application/json`, the attachment name, `no-store` and `nosniff`, and **`Content-Length` equals the bytes received**, within 16 MiB. The subject is the session user, the partner's shared message is present and its key absent, and an immediate repeat gets 429 with `Retry-After`. An absent body is accepted for the partner, a user without a space gets 404, and nothing is written.

### 11.4 Web and contract [executed]

| Check | Result |
|---|---|
| Focused `user-export.spec.ts` | 11 tests passed. The first run failed 1 test because jsdom's `Blob` has no `text()`; the test now reads bytes with `FileReader`, no product change |
| Full suite (`Claude outputs/e1-web-ee65e6abc1`) | **951 tests in 47 files** passed |
| Both typechecks | Passed |
| Isolated production build (temp `--outDir`) | Passed. `apps/web/dist` stayed 45 files with identical hashes and timestamps |
| `Claude outputs/e1-yaml-check.py` | Duplicate-key control refused. Contract 0.46.0, 105 paths, 122 operations, 190 `$ref`s resolved; export response set, request schema and the 32 required sections checked. YAML structure only, **not OpenAPI semantics** |

Web coverage:
- `requestDownload` guards: offline, CSRF, same-origin, envelope, `Retry-After`, network, the declared and received size caps, unsafe file names;
- `request()` unchanged;
- the cache policy for the route;
- the store: anchor save and a revoked object URL, no `localStorage` write, no automatic retry, explicit retry, a late answer dropped after an identity change, `cancel()`;
- the MeView card text, the disabled button and cancel on unmount.

### 11.6 Resource-control correction (Claude Code, 2026-11-11; owner-approved)

**Scope:**
- Fix 1: admission lifetime and the transaction-end signal (Section 7.1).
- Fix 2: bounded browser reception (Section 8.2).
- The documentation correction to API memory (Section 7).

Export scope, privacy rules, the contract (0.46.0) and the 48 frozen migrations are unchanged. The earlier runs in Sections 11.2–11.4 remain the historical record of the first implementation.

**Files:**
- `services/api/src/services/ExportService.{h,cc}`: dedicated slot sessions; token identity; `ROLLBACK` plus token check; the release; test seams.
- `services/api/src/main.cc`: calls `ExportService::init(…, 2)`.
- `services/api/tests/ExportDbTests.h`:
  - a new lifecycle case;
  - the harness-only `runConnectionLoss`;
  - `exportOf` waits, on a condition, for the slot release;
  - the guard destructor no longer throws.
- `ExportLiveApiTest.h`: the no-space call retries only while it gets 429, bounded.
- `StaticCheckTest.cc` and `DbGatedTest.cc`.
- `tools/export-verify.ps1`: adds the `EXPORT_CONNECTION_LOSS` step.
- `apps/web/src/api/client.ts`: streamed `requestDownload` and a new client code `DOWNLOAD_STREAM_UNSUPPORTED`.
- `apps/web/src/api/error-messages.ts`.
- `apps/web/tests/unit/user-export.spec.ts`.

**Backend regressions [executed in the final run]:**
- **Success, three states:**
  1. End requested: the caller is answered, the `ROLLBACK` is not sent (hook), the export's backend is `idle in transaction` and holds the lock, and another export is refused (`global`).
  2. `ROLLBACK` answered but verification failing (injected): the lock is gone, yet the slot is still held and another export still refused.
  3. Ended: a single release (`ended`); that backend is `idle` and lock-free; the next export is admitted.
- **Deadline:** answered at 300 ms while a holder blocks the lock. The slot is held, another export is refused, and it is released once after the holder lets go.
- **Database error:** `statement_timeout` on generation. The aborted transaction is ended and the end confirmed.
- **Lost `BEGIN` reply:** `BEGIN` applied with its reply treated as lost (pid never captured), while another session carrying the export's token keeps a transaction open.
  - The slot stays `unconfirmed`, another export is refused, and that session is **not** terminated.
  - Once its transaction ends, there is one release (`ended`, pid 0), and the session is still alive.
- **Unrelated pid:** the captured pid is pointed at an unrelated same-role session with an open transaction. The export is released on its own token, and the unrelated session stays `idle in transaction`, untouched.
- **Connection loss after the lock** (harness-only): the export's backend is terminated after the lock.
  - Generation landed on the reconnected session and was refused (`session_lost`).
  - The release was `session_replaced`, and the original pid is gone.
  - The next export is served.
- **Static:** `ExportService.cc` contains no `pg_terminate_backend` and no `TxScope`. The slot vector is leaked and never cleared, erased or reset. Tagging precedes `BEGIN`.

**Web regressions:**
- **Bytes:** exactly at the cap with and without `Content-Length`; a misleading smaller `Content-Length` and a missing one both refused at the first crossing chunk (4 reads, 1 cancel, 0 reads after cancel); an oversized declared length refused with 0 reads and 1 cancel.
- **Failures:** a read failure cancels; an abort between chunks cancels and rethrows `AbortError`; no stream gives `DOWNLOAD_STREAM_UNSUPPORTED`.
- **No download:** a refused stream creates no object URL and no click; the existing guard, identity, route and cancellation tests remain.

**Runs (all retained):**

| Run | Result |
|---|---|
| Web `Claude outputs/e1b-web-599532bead` | Full suite **957 tests in 47 files**, both typechecks, isolated build passed; `apps/web/dist` 45 files unchanged. Focused spec 17 tests |
| 8 `usapp-e1-33eb27215d` | **Failed** (exit 134). An ordinary successful export stayed `unconfirmed` (`transaction_open`): the first end check used `now() = statement_timestamp()`, which is not a reliable "no open transaction" test for Drogon's prepared statements. The test guard's wait then threw in a destructor and terminated the binary. Fixed: verification is `ROLLBACK` plus the token check, and the guard records a failed check instead of throwing |
| 9 `usapp-e1-09bfa86258` | Export suite 452/452, live 560. CTest 19/20: `export_db` **timed out at 30 s**. The connection-loss case's reconnect took over 20 s in the CTest pass (about 1 s in that run's targeted pass); the deadline answered at 20 000 ms. No timeout raised: the case moved, assertions unchanged, to the harness-only `EXPORT_CONNECTION_LOSS` mode with 60 s bounded waits |
| 10 `usapp-e1-babdaf6cdb` | **All passed** (below) |

An earlier draft that terminated the "old pid" was replaced before any run of it was used as evidence.

**Final run `usapp-e1-babdaf6cdb`** (disposable stack and checks as in 11.3):

| Check | Result |
|---|---|
| Migrations | 48 checksums; eraser violations 0; privilege deviations 0 after apply, repeat and CTest; 48 migration files unchanged |
| Build | Passed |
| Targeted suites | `EXPORT` **444 assertions, 5 cases**, with the expected `unconfirmed` warnings from the injected and impostor cases; ACHIEVEMENT 22, AVATAR_CATALOG 157, AVATAR_PURCHASE 114, AVATAR_LOADOUT 85 |
| Live API | 560; API log export lines 4, exported-content marker lines 0 |
| **CTest** | **20/20 in 96.91 s**; `export_db` 3.91 s, `db_gated` 13.32 s. Post-checks 0 |
| `EXPORT_CONNECTION_LOSS` | 8 assertions; `reason=session_lost`, answered 1,032 ms and released 1,034 ms after the termination, `session_replaced`, 1 verification attempt |
| `EXPORT_LARGE` | 8,500 rows accepted (output 14,749,868 bytes, 284 ms); `input_bytes` and `section_rows` refusals |
| `EXPORT_CLOSURE` | 23 assertions, run last |

**Not verified:**
- browser, device, real backend, production or cloud;
- a half-open network break the server does not notice (only a server-side termination was tested);
- process memory (unmeasured).

The reconnect delay after a connection loss varies (about 1 s versus over 20 s observed) and is not explained.

**Later observation (2026-11-11, run `usapp-e1-06d5f0ba61` of the admin wallet batch; no export code changed):** the harness-only `EXPORT_CONNECTION_LOSS` case failed. After the test terminated the export's backend, the deadline answered at 20 s, and the slot stayed held without a release inside the case's 60 s wait (80 s in all). That is the designed fail-closed behaviour, but the reconnect stall was longer than any observed before (about 1 s, then over 20 s, now over 60 s), so how long a slot stays unavailable after a connection loss is not bounded by anything this service controls. The cause remains unexplained. Recorded as an availability limit and not rerun until green. Every other check in that run passed, including full CTest 21/21.

### 11.7 Missing generation callback recovery (2026-11-13)

The later shared-plan run `usapp-e1-7c5c8605ea` again failed `EXPORT_CONNECTION_LOSS` after 80 seconds with no cleanup;
its CTest 26/26 result did not make that overall harness green. No export code had changed in that run.

A standalone disposable probe now reproduces a parameterized-query callback not arriving after a server-side session loss,
even after 40 seconds with a configured 35-second client timeout. Keeping the query string alive and preparing it before
the loss did not remove that observation. A sampled driver thread was in `epoll_wait`; that does not establish the cause.
See `tools/probes/export-reconnect/README.md` for failed and successful controls, including the failed multi-round watchdog.
The driver/timer implementation's root cause remains unproven.

Implemented correction: publish `generationSubmitted` only **after** the final generation statement is submitted to
the single client. Once that publication has occurred, the existing deadline also requests `endWith`, which queues the
existing ROLLBACK and token/lock verification, without depending on the missing generation callback. The submission path
also checks an already-answered deadline after publication, covering a deadline racing the submission. Exactly-once ending,
answer and release remain separate; the deadline never proves transaction end or directly frees a slot.

This is deliberately limited to the last statement-producing stage. A missing BEGIN/settings/lock reply can still leave its
slot unavailable: queuing cleanup while those earlier callbacks could still submit later stages is unsafe. No backend is
terminated by the service; no client is replaced or destroyed; connection counts, timeouts, caps and contract are unchanged.

The added lifecycle regression discards the generation reply, then requires the deadline answer, confirmed rollback, one
release, an idle gate and a successful next export. Existing cases still require a slot held during unfinished lock work and
failed token verification.

Verification:

- `usapp-e1-91797e0762`: build passed; source hashes for the service/header/test matched the workspace; EXPORT 449/449
  (including the five added checks), live API 942/942, privilege deviations zero after apply/repeat, 48 migration checksums.
- **Full CTest failed, 25/26 in 97.02 s.** `export_db` passed in 4.48 s. Default `db_gated` aborted at 8.90 s with
  `Resource deadlock avoided`; the stack includes `DbClientImpl` destruction through `TaskTimeoutFlag`/`SqlCmd` disposal on
  a database event loop and `EventLoopThread` destruction. This matches the recorded teardown-crash class; it is not fixed
  by this export change. The full harness exited 1 and never reached its loss/large/closure modes.
- A **separate focused continuation**, `recovery-8ed9a2ad`, restarted only those stopped, exact-owned containers, recreated
  their tmpfs database, reapplied migrations and verified the compiled source snapshot. EXPORT 449, CONNECTION_LOSS 8,
  LARGE 6 and CLOSURE 23 assertions all passed. The loss case answered `database` immediately and released after 1,009 ms
  with `session_replaced`; the next export passed. That particular loss took the ordinary error-callback path, so it does
  **not** prove the intermittent driver stall has disappeared. The added deterministic missing-reply case verifies the new
  deadline-driven cleanup path.
- 48 workspace migration files unchanged. No web change or web rerun. No browser, real provider, production or deployment.
- Logs: `Claude outputs/export-recovery-20261113`. Containers stopped and retained; the focused closure database was closed
  by its test. The full-regression failure is preserved alongside the focused passes.

Status: the missing-generation-callback cleanup gap is addressed and specifically tested; intermittent transport recovery
and the independent client-teardown crash remain open. This is **not** an all-green release gate or a driver root-cause fix.

**Later follow-up (2026-11-13):** the test-observer teardown ownership race was reproduced with a callback barrier and
corrected in test support, without changing export/production clients. Run `usapp-e1-f6a24ffdab` completed `db_gated` and
the new lifecycle target, but full CTest was 26/27 due to a push-registration guard failure. Its separate fresh-database
export continuation passed 449/8/6/23 checks; intermittent transport recovery still is not proven fixed. See
[test observer lifecycle evidence](test-observer-lifecycle.md). Earlier failure evidence above is preserved.

### 11.8 Deadline cleanup across every statement stage

Tagging, BEGIN, settings, lock acquisition, generation and the first ROLLBACK now submit
through one event loop. The deadline marks the export as ending before it queues cleanup.
A callback from an earlier stage can only enqueue a guarded task, which drops the
statement if ending has started. A submission already executing on the loop finishes
before ROLLBACK is submitted. This lets the same end verification run after a missing
early reply as well as after a missing generation reply.

The regression suppresses the reply after BEGIN has applied, waits for the deadline
answer, requires confirmed transaction end and one slot release, and runs a new
export through the same slot. The watchdog still depends on the dedicated database
client to carry ROLLBACK and verification. A driver or network failure that prevents
those operations from returning can still retain the slot. No database connection
is force-terminated or replaced by the service.

Disposable run `usapp-e1-7b8024ee15` passed 455 export assertions, all 1465 HTTP
assertions, and full CTest at 36/36. The connection-loss case answered at its
deadline and confirmed cleanup in about 20 seconds with `session_replaced`.
Large and closed-database probes passed. This run exercises one real PostgreSQL
backend interruption and the deterministic lost-callback controls. It does not
establish recovery from a sustained transport outage or every reconnect stall.

### 11.5 Limitations and unperformed acceptance

- **No browser run, device, real-backend, production, cloud or provider acceptance**, and nothing deployed.
- Exports were generated only from synthetic test accounts in disposable databases.
- **Process-local limits:** the admission gate and frequency map do not bound other API processes. Each process adds 2 dedicated PostgreSQL connections. After the 2026-11-11 correction, a slot is released only when its transaction end is confirmed (Section 7.1). An unconfirmable end keeps it held, which reduces export availability in that process until it clears.
- **Timeout labels** rely on SQLSTATE when available, otherwise on English PostgreSQL message text. A localized server would label timeouts `database`; the behaviour, failing closed, is unchanged.
- **Snapshot:** a deletion committed during the generation statement is not tested. MVCC excludes it [reasoned]; only both sides of the boundary are tested.
- **Downloaded files:** the browser buffers up to 16 MiB. A downloaded file cannot be revoked, and `no-store` does not change that.
- **Real volumes** remain unmeasured; the limits were sized with synthetic data only.
- **Unchanged and retained:**
  - the closure suite leaves its disposable database closed;
  - the containers of runs 1–7 are stopped and retained;
  - run 7's key logs are copied to `Claude outputs/e1-export-verify-10f23629da` (the credential env file is excluded);
  - Docker Desktop was started for these runs and is still running.
