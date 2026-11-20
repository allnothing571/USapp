# Phase 2 P2-1: Partner message board

Status (2026-10-27): owner-assigned to Claude Code for design and implementation;
owner-run verification on usapp_test passed (Section 10). 0017 is applied and frozen on
test and main; the API and web were deployed by Codex on owner authorization (Section 13).
Live message workflows and phone acceptance are pending. Plan: [phase2-plan.md](phase2-plan.md).

## 1. Requirements traced

| V1 | Requirement | P2-1 handling |
|---|---|---|
| §3.1 | Both members have equal rights; both can post messages | Either member posts to the partner |
| §25 | "留言板 + 单条回应", not chat; each message is a small note; write a message, show its time, view history, reply to one message with one sentence; no endless chat, multi-level replies, real-time chat or IM | Message board with history and detail; one short reply per message; no real-time updates |
| §25 | "可以发送一个轻量回应" | Reactions are P2-4 (with §13 task reactions); not in P2-1 |
| §19 bottom-right | Partner's latest message with time and short content; tap opens detail/history | Home bottom-right card; tap opens the message detail |
| §47 | Suggested navigation 首页 / 任务 / 奖励 / 我们 / 我的; "我们" hosts messages (and later anniversaries, memories, albums, plans) | Fifth navigation item "我们" (`/us`), currently the message board |
| §42 | Entities `Message`, `MessageReply` | Two independent tables `messages`, `message_replies` |
| §54 | Every couple data access verified server-side | Membership checked in every SQL statement; database foreign keys tie rows to the couple |
| §18 | Notifications are a unified mechanism | Not in P2-1. Stable ids and same-transaction audit rows are kept for P2-3 |

## 2. Product rules and conservative defaults

The requirements leave several details open. These defaults are conservative and
reversible (later additions do not change existing rows).

1. **Author and recipient.** Either member writes a message. The recipient is always the
   other member of the space. A space without a partner gets 409 `PARTNER_REQUIRED`.
2. **Text only.** Body is plain text, 1-500 Unicode code points, line breaks allowed.
   Same normalisation as task descriptions: CRLF becomes LF, other control characters
   are rejected, and leading/trailing spaces and blank lines are trimmed. There are no
   images or attachments (Phase 3 storage, §45).
3. **One reply, by the recipient.** Only the recipient may reply. The author cannot reply
   to their own message. Each message has at most one reply ("单条回应", "对一条留言回复一句话").
   The reply is one sentence: 1-100 code points, single line, same character rules.
   A second reply gets 409 `MESSAGE_ALREADY_REPLIED`; the author replying gets 403
   `MESSAGE_REPLY_FORBIDDEN`.
4. **No editing or deletion in P2-1.** Messages and replies are append-only notes. §25
   lists no edit or delete. The §29/§30 creator-edit and edit-request rules address shared
   content (anniversaries, plans, memories), not a personal note to the partner. This
   needs no task-style mutual approval. See Section 9, open question 1.
5. **Visibility.** Both members see every message in their space, in both directions,
   with its reply. Nobody outside the space can read, reply to or detect a message
   (another space's message id gives 404).
6. **Ordering and paging.** Newest first by `(created_at, id)`. Cursor pages (default 50,
   1-100) are not snapshots, but new messages only appear before the first page, so
   following `next_cursor` neither skips nor repeats rows.
7. **Home.** The bottom-right card shows the partner's latest message: time, a short
   preview (first 60 characters of the first line) and whether it has been replied to.
   Tapping opens the detail.
   - Empty: "对方还没有留言".
   - Loading: no content is shown.
   - Error: an error with a retry button, never "no messages".
8. **Us page (`/us`).** Compose form, history with "加载更多", and per-message status:
   replied, "待我回复" (I am the unreplied recipient), or "等待对方回复".
   Detail page `/us/messages/:id` has the reply form for the recipient.
9. **No read receipts or unread counters.** These belong to notifications (P2-3).
10. **Retries.** Creation and reply carry an `Idempotency-Key`. An unknown result is
    retried only by the user, with the same key and body. There is no offline queue and
    no automatic replay.

## 3. Why no mutual approval

Task edits need confirmation because they change what the partner must do and what they
earn (§32). A message changes no asset or obligation. Requiring approval would turn a note
into a workflow, so P2-1 has none.

## 4. API (contract 0.14.0)

All endpoints require a session and membership. Responses are `Cache-Control: no-store`.
Writes need exact Origin, CSRF and an `Idempotency-Key`.

| Method and path | Result |
|---|---|
| `POST /api/v1/messages` `{body}` | 201 created / 200 same-key replay: `{mutation, message}` |
| `GET /api/v1/messages?limit&cursor&from=all\|partner` | `{messages, next_cursor}`; `from=partner` lists only messages written by the partner (Home uses `limit=1`) |
| `GET /api/v1/messages/{id}` | `{message}` |
| `POST /api/v1/messages/{id}/reply` `{body}` | 201 / 200 replay: `{mutation, message}` |

`Message`: `id, author_id, recipient_id, body, created_at, reply` (`reply` is `null` or
`{author_id, body, created_at}`).

`mutation` holds the immutable facts of that write (action, ids, body, time) and stays
identical on replay. `message` is the current state.

Errors: `VALIDATION_ERROR`, `SPACE_NOT_FOUND`, `PARTNER_REQUIRED`, `MESSAGE_NOT_FOUND`,
`MESSAGE_REPLY_FORBIDDEN`, `MESSAGE_ALREADY_REPLIED`, `IDEMPOTENCY_KEY_REUSED`,
`RESULT_UNKNOWN`.

## 5. Database (migration `0017_messages.sql`)

- **`messages`:**
  - columns `id`, `couple_id`, `author_id`, `recipient_id`, `body`, `idempotency_key`,
    `request_fingerprint`, `created_at` (`clock_timestamp()`);
  - `UNIQUE (id, couple_id)` and `UNIQUE (author_id, idempotency_key)`;
  - author and recipient foreign keys to `couple_members (couple_id, user_id)`;
  - `CHECK author_id <> recipient_id` (a space has two members, so the recipient is the
    partner);
  - body shape and key/fingerprint shape checks;
  - indexes `(couple_id, created_at DESC, id DESC)` and
    `(recipient_id, created_at DESC, id DESC)`.
- **`message_replies`:**
  - primary key `message_id`, which enforces one reply per message;
  - `couple_id`, `author_id`, `body`, `idempotency_key`, `request_fingerprint`, `created_at`;
  - `(message_id, couple_id)` foreign key to `messages`; author membership foreign key;
  - `UNIQUE (author_id, idempotency_key)`; body is one line, 1-100 code points.
- **Triggers:**
  - `UB001`: statement-level append-only on both tables (UPDATE, DELETE, TRUNCATE).
  - `UB002` on messages: `created_at` not in the future.
  - `UB002` on replies: the reply author must be the message's recipient; `created_at`
    not before the message and not in the future.
- **Grants:** `SELECT, INSERT` to `usapp_api` only (migration and `init-roles.sql`
  section 15, so the grants survive repeated InitRoles).
- **Privilege enforcement:** both tables join the startup task privilege policy and
  `database/tests/task_privilege_deviation.sql`. The API therefore requires 0017 before
  it starts.

## 6. Concurrency and transactions

- **Create** (one transaction):
  1. Read the caller's membership `FOR SHARE` together with the partner.
  2. Look up the key on `(author, key)`. The same fingerprint replays the committed
     message; a different one gets 409 `IDEMPOTENCY_KEY_REUSED`.
  3. Insert with `ON CONFLICT DO NOTHING`. If the insert was skipped, re-run the key
     lookup, which covers a concurrent same-key request.
  4. Write the audit row `message.created`, read back, commit.
- **Reply** (one transaction):
  1. Read membership `FOR SHARE` joined to the message of the same space.
  2. Look up the key on `(author, key)`; replay or reject as above.
  3. The recipient check returns 403 for the author.
  4. Insert with `ON CONFLICT DO NOTHING`. When two replies race, the primary key lets
     exactly one insert succeed. The loser sees no inserted row, re-runs the key lookup,
     and then gets 409 `MESSAGE_ALREADY_REPLIED`.
  5. Write the audit row `message.replied`, commit.
- Success is reported only after a confirmed commit. An unconfirmed commit is 503
  `RESULT_UNKNOWN`. No advisory locks are needed: no read-modify-write of shared state
  exists beyond unique constraints.
- The message ids and audit rows are the stable events P2-3 will turn into notifications.

## 7. Web

- **New files:**
  - `src/api/messages.ts`;
  - `src/stores/messages.ts`: list, detail, and compose/reply actions that keep their key
    while the result is unknown; session-generation guards;
  - `src/views/UsView.vue`, `src/views/MessageDetailView.vue`.
- **Routes:** `/us` and `/us/messages/:id`, behind the existing authentication and space
  guards.
- **Bottom navigation:** five items; "我们" is active on both Us routes.
- **Home:** the home store gains an independent "partner latest message" read; the
  bottom-right card replaces its placeholder.
- **Session isolation:** a session reset clears message state; stale results are
  discarded.

## 8. Verification

Section 10 records the evidence once.

## 9. Open questions for the owner (non-blocking; defaults above apply)

1. Should an author be able to delete (withdraw) their own message, and should the
   replier be able to delete a reply? Not implemented; adding it later is additive.
2. Is 100 characters right for the one-sentence reply, and 500 for a message?
3. Should Home prefer an unreplied partner message over the newest one? Current default
   is the newest one.

## 10. Verification evidence (2026-10-27)

### 10.1 Claude Code, during implementation

- Web typecheck: exit 0.
- Affected web specs passed:
  - new `messages.spec.ts`, 13 tests;
  - updated `home.spec.ts`, `home-wallet.spec.ts` and `bottom-nav.spec.ts`;
  - `me-session.spec.ts`.
- Two failures were repaired, both in test code:
  - The bottom-nav spec's test router lacked the `us` route and expected four items; it now
    covers five items, the detail page owning "我们", and the `/us` guards.
  - A detail test typed a newline into the single-line reply input, which the browser
    strips; it now checks the 100-character limit instead.
- Claude Code could not run the database stages itself:
  - auto mode's safety check returned no verdict for PowerShell, Edit and permission-mode
    calls;
  - Docker is denied in Bash by the project rules;
  - Docker Desktop was briefly not running.
- No permission setting was changed. The owner ran the reviewed script from Windows
  PowerShell.

### 10.2 Owner-run verification on usapp_test (reported by the owner)

- `0017_messages.sql` SHA-256:
  `155851497176fde9a66120cf0157d1d2eb72c9176e1cf48ce43d035699380c0c`.
- Before the change, usapp_test had 0000-0016 with checksums matching the local files.
- Rollback-only pre-validation (0017 body plus `message_ledger_test.sql` in one
  transaction ending in `ROLLBACK`):
  - postgres/maintenance and usapp_api/application each passed 5 groups;
  - afterwards 0017 was unregistered and both message tables were absent.
- Fresh test image build: exit 0, 42 s.
- DryRun: 17 skipped; only 0017 pending, with the hash above.
- Apply with InitRoles: 0017 applied and registered with the matching checksum.
  Privilege deviation query: 0 rows.
- Repeat InitRoles: all 18 migrations skipped. Privilege deviation query: 0 rows.
- Full CTest: exit 0, all three targets passed. db_gated ran, including the new block 24,
  and was not skipped.

  | Target | Time |
  |---|---|
  | unit | 0.03 s |
  | db_gated | 10.32 s |
  | static_check | 0.01 s |

- `message_ledger_test.sql` on the applied database: usapp_api/application and
  postgres/maintenance each passed 5 groups, exit 0, `ROLLBACK`.
- Web unit suite: exit 0, 22 files / 230 tests. Web typecheck: exit 0.
- Not performed:
  - main migration;
  - API deployment;
  - served web dist build;
  - live HTTP or browser workflows.

Migration 0017 is applied and frozen on usapp_test; it must never be edited.

### 10.3 Committed test fixtures

db_gated block 24 commits isolated fixture spaces on usapp_test. They are the same
`@m3-fixture.test` pattern as earlier blocks: users, couples, members, messages, replies and
audit rows. The rollback-only and SQL-script runs commit nothing. Exact row counts were not
reported.

### 10.4 Release prerequisites (not authorised in P2-1)

1. Fresh main backup and restore check.
2. Main DryRun showing only 0017 pending with the hash above.
3. Apply with InitRoles, privilege query with 0 rows; repeat InitRoles, privilege query
   with 0 rows.
4. Rebuild and deploy the API. The startup task policy includes both message tables, so the
   API refuses to start without 0017. Check the startup policy lines.
5. Only after the API is healthy, build the bind-mounted web.
6. Browser acceptance:
   - write, reply and replay;
   - the Home latest-message card;
   - "我们" navigation;
   - account switching.

## 11. Review repairs (Claude Code, 2026-10-27; frontend only)

Historical record: the retry behavior and task/reward audit below are superseded by
the follow-up in Section 12. The earlier test results remain historical evidence.

Codex reviewed P2-1 after the owner-run checks. These repairs change only
`apps/web/src/stores/messages.ts` and `apps/web/src/views/MessageDetailView.vue`, plus the new
`apps/web/tests/unit/messages-repairs.spec.ts`. No backend, schema, migration (0017 frozen),
grant or contract change.

### 11.1 Unknown result followed by a rejected retry

- Once a send has an unconfirmed result (network error or 5xx, including `RESULT_UNKNOWN`),
  its action is marked `unresolved`.
- A later retry may be rejected before the message service classifies the idempotency key:
  `UNAUTHENTICATED`, `CSRF_TOKEN_INVALID`, `ORIGIN_NOT_ALLOWED` or `RATE_LIMITED`. That
  rejection only proves the retry was not processed. The action keeps:
  - its status `unknown`;
  - the original key;
  - the original body.
  It then waits for another user-initiated retry with the same key.
- Unchanged:
  - a confirmed 2xx (including a same-key replay) resolves the action;
  - a business rejection after classification (for example `MESSAGE_ALREADY_REPLIED`) ends it;
  - a first attempt that never reached the server (offline block, missing token) is not
    `unresolved`, so a later CSRF rejection ends it;
  - a definitive first-attempt rejection ends it.
- Identity reset (logout, account switch, bootstrap failure) still clears all in-memory
  actions, including unresolved ones. They are never persisted or shown to another account.
  After re-login, the user can see whether the message or reply was written by checking the
  history. Writing it again uses a new key and may create a duplicate message; a duplicate
  reply is impossible because of the one-reply rule.

### 11.2 Detail page target changes

- `MessageDetailView` clears the reply draft and error when the message id or the account
  changes.
- `submit` and `retry` capture the message id and account before awaiting. They clear the
  draft only if the page still shows the same message for the same account.
- Action notices stay keyed by `reply:<id>`, so A's result never appears on B.

### 11.3 Reads never hide a confirmed reply

- Replies only appear and never disappear. The store remembers every reply it has seen or
  confirmed; the set is cleared on identity reset.
- It merges that memory into later list, load-more, detail and single-message reads. A read
  that started before a confirmed reply and lands after it therefore cannot show the message
  as unreplied.
- Session generations and request sequences are unchanged.
- Not covered: Home's latest-message card has its own read and refreshes on return or rollover.

### 11.4 Verification (Claude Code)

| Command (apps/web) | Result |
|---|---|
| `pnpm run typecheck` | exit 0 |
| `vitest run` messages-repairs, messages | exit 0, 26 tests (13 new) |
| `pnpm run test:unit` | exit 0, 12 s: 23 files / 243 tests (was 22 / 230) |

New tests:
- unknown create followed by a CSRF, auth, Origin or rate-limit retry rejection: the action is
  kept with the same key and body, and a later authorised retry sends the same key and body
  and resolves;
- the same for a reply;
- business rejection after an unknown attempt ends the action;
- unsent first attempt plus CSRF retry ends the action;
- definitive first-attempt rejection ends the action;
- an account switch discards the unresolved action;
- a stale detail read and a stale list read after a confirmed reply keep the reply;
- detail page A to B clears the draft and error;
- A's late reply result leaves B's draft untouched and A's notice off B.

No failures occurred. Not run: backend tests (no backend change), SQL scripts, browser checks,
main, deployment, served-dist build.

### 11.5 Audit: task and reward stores (static reading; not reproduced; not changed)

Both stores treat any 4xx on any attempt as definitive and drop the key, even after an
earlier attempt with that key had an unknown result. A retry rejected before classification
(session expiry, CSRF mismatch, Origin, rate limit) therefore discards a key whose original
request may have committed. If the user acts again, a new key is used:

| Path | Code | Consequence if the original committed |
|---|---|---|
| Task publish | `stores/tasks.ts` `publish`, catch: `isDefinitiveRejection` clears `pending` | Duplicate task with its own reward. Completing both would settle twice (asset-related). |
| Single completion | `sendCompletion`, `isDefinitiveRejection` then `endAction` | No double credit: the server rejects a second completion with `TASK_ALREADY_COMPLETED`. The user sees a rejection instead of the confirmation, and the wallet is not reloaded. |
| Counting progress (`kind: 'progress'`, including daily/weekly periods) | same branch | A new key records another +1 event, double counting. It can reach the target earlier and settle it (asset-related). |
| Task change propose/decide | `sendChange`, `isDefinitiveRejection` then `endChange` | Likely rejected by the one-pending-proposal or already-decided rules; misleading notice only. |
| Product create | `stores/rewards.ts` `send`, `!isUnconfirmedOrUnsent` then `end` | Duplicate product. |
| Product edit/archive | same | Same `expected_version` with a new key gives `PRODUCT_VERSION_CONFLICT`; no duplicate. |
| Direct redemption | same | A second redemption and a **second flower debit** if the balance allows (asset risk). |
| Request redemption | same | A duplicate pending request; confirming both would debit twice. |
| Fulfil / transitions / cancel approve | same | Server-side state, proposal-id and exactly-once refund rules prevent duplicates; misleading notice only. |

How likely this is in practice is not measured; it needs one of those rejections right after an
unknown result. This was not reproduced with tests or live requests, and those stores were not
changed. Codex will review the scope, especially the asset paths.

## 12. Unconfirmed mutation retry follow-up (Codex, 2026-10-27; frontend only)

The owner authorized Codex to implement the P2-1 follow-up and repair the analogous
task and reward retry paths. No backend, schema, contract, grant or migration change was made.
The local 0017 SHA-256 remained
`155851497176fde9a66120cf0157d1d2eb72c9176e1cf48ce43d035699380c0c`.

- Once an attempt has an unknown result, message, task completion/progress/change and
  reward actions retain their original key and payload across later errors, including
  local offline/CSRF blocks, authentication failures, business 4xx and unrecognized
  gateway responses. The UI continues to say the earlier result is unknown. A valid
  same-key success ends the action; an initial 4xx still rejects the first attempt.
  Conservatively retaining business 4xx avoids relying on assumptions about whether
  each endpoint checked the key before returning that error.
- An unknown task publication cannot be replaced by a changed payload. The form keeps
  the new draft and allows an explicit retry of the original snapshot. Malformed or
  incomplete successful publication responses retain the original key. Publication
  captures its payload independently of subsequent caller mutations.
- Message success responses must match the submitted author and text, including the
  reply. Reward success responses must match the target product/redemption and, for
  redemption, the captured product version; otherwise their result remains unknown.
- No automatic retry, offline mutation queue or browser persistence was introduced.
  Identity reset still clears pending actions. After logout, account switch or a
  fail-closed bootstrap, an unconfirmed request cannot be retried with its old key;
  the user must check the relevant history before acting again. There is no guarantee
  against duplicate actions across such a reset. A later design must address safe
  reconciliation without leaking data across identities.
- If a valid same-key success never arrives, the action remains unresolved in the
  current session even when a later business rejection might in fact be definitive.
  This deliberate conservative behavior can require manual review and is not a
  complete reconciliation protocol.

Verification in `apps/web`, using the already-installed local binaries:

| Check | Result |
|---|---|
| Focused message, task, reward and view tests during repair | 6 files / 102 tests passed |
| Final full Vitest suite | exit 0; 23 files / 266 tests passed, 10.82 s |
| `vue-tsc --noEmit -p tsconfig.json` | exit 0 |
| `tsc --noEmit -p tsconfig.node.json` | exit 0 |

The first full suite revealed an incorrect product-target fallback in the new client
validation and a stale message-body test fixture; both were corrected before the final
passing run. `corepack.cmd pnpm run typecheck` initially could not resolve the package
registry, so the existing local `vue-tsc` and `tsc` binaries were used instead. No
dependency was installed. Tests mock network or service responses; no lost-commit fault
was injected into a live service. No main database access, deployment or served-web build
was performed.

## 13. Main rollout and Wi-Fi address switch (Codex, 2026-10-27)

The owner authorized the release and then requested switching to the computer's current
WLAN address. The service and browser checks below do not establish live message/reply or
phone acceptance.

- Preflight: main `usapp` had 0000–0016, with matching checksums; DryRun showed only
  `0017_messages` pending. Local SHA-256:
  `155851497176fde9a66120cf0157d1d2eb72c9176e1cf48ce43d035699380c0c`.
- Backup directory: `backups/before-p21-20261027_203943/`. Exported database dump
  `usapp_before_0017.dump`: 442375 bytes, SHA-256
  `7ba70dacd86fc18494b1bea5b4e5161410bc4779b0bcc1d75dee5fec192caaea`.
  The exported file was copied back and restored into
  `usapp_p21_restore_20261027_203943` using template0 and a single transaction.
  All 25 public base table counts matched the script's before/after stability rules;
  this was not a row-by-row comparison. The scratch database and temporary files remain.
- Rollback image `usapp-api:before-p21-20261027_203943` points to the previously running
  `sha256:a5e17286c6f82161244924bdf67a665f3fead08eb665909a72f19c47d6b480b9`.
  Served-web backup `web-dist/` contains 23 files / 303642 bytes, copied from Caddy.
  `rollback-info.json` records the paths and counts. The old LAN configuration was saved
  in `lan-before-switch/`.
- Main 0017 applied with InitRoles; the full privilege deviation query returned zero rows.
  Repeated InitRoles skipped 18 migrations, applied none, and again returned zero rows.
  All 18 registered checksums matched the local files.
- API image `sha256:1cc0d7b56bd77ca2ceeac437de0e8233b5c841009e1142e17ed6acfe0f4f1b02`
  built and was deployed; API healthy. Startup wallet policy: 187 checks, zero deviations;
  task policy: 1093 checks, zero deviations.
- Local installed Vite binary built the web (typechecks had already passed in Section 12):
  PWA precache 25 entries / 245.64 KiB; dist 26 files / 323634 bytes. Caddy and local
  hashes matched: index
  `ce14e12ee0d0c92eaae1f348b02ec379721dc2635b2d15e48ecb9cf02e0960ec`; service worker
  `8e85d9f6ca3b2bfd0ce3051965bb36ada078644d4ea1e859701b9b4084aef367`.
- The computer's WLAN address changed to `10.6.245.114`, so the owner requested a switch
  from `192.168.1.55`. Candidate Caddy and Compose configuration validated; API was
  recreated with the same new image and Caddy reloaded. The current canonical origin is
  `https://10.6.245.114:8443`; every Compose invocation must still include the LAN
  override. The public root CA file still matches the already-exported certificate.
- From the computer, TLS was verified against that root and the new IP; revocation lookup
  was disabled because the private CA has no published CRL. Health returned 200/no-store,
  anonymous `/api/v1/messages` returned 401, `/admin/` returned 404, and HTTPS index/SW
  hashes matched the build. This does not prove phone-to-PC reachability.
- Rollback of code would leave 0017 applied and cannot undo later user data. Restoring the
  database dump over main would discard later changes. No rollback or cleanup was performed.
  Android access on the new Wi-Fi, login, installation/update and message workflows remain
  unverified. Unresolved actions still do not survive an identity reset (Section 12).
