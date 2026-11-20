# Phase 2 P2-5: Web Push (design for owner review)

Status (2026-11-02): **P2-5a and P2-5b implemented by Claude Code and automated-test verified on
usapp_test (P2-5b with a fake transport only); `0022` and `0023_push_dispatch` applied/FROZEN on
usapp_test only; NOT DEPLOYED; nothing real was sent; no device acceptance.** Evidence:
[Section 12](#12-p2-5a-implementation-and-verification-claude-code-2026-10-30) and
[Section 13](#13-p2-5b-implementation-and-verification-claude-code-2026-11-02).
Repairs (2026-11-02): [Section 14](#14-p2-5b-correctness-repairs-and-db-verification-proposal-claude-code-2026-11-02)
describes them; [Section 15](#15-p2-5b-isolated-repair-verification-claude-code-2026-11-02) records their verification in a
disposable isolated instance. `0024` is frozen; the shared `usapp_test` is still 0000-0023 and main 0000-0017.
Sections 1-11 are the revision-3 design, written before
implementation; their "nothing executed" statements describe that time.

**P2-5c (2026-11-02): transport implemented and verified in isolation with stubs only; real subscription and dispatch
stay off by default; no real provider, real service worker, device acceptance or deployment:**
[Section 16](#16-p2-5c-implementation-and-isolated-verification-claude-code-2026-11-02). P2-6 is not authorized.

Codex accepted the direction of revision 1:
- C++ owns authorization and durable dispatch state;
- Node is a DB-free, authenticated transport;
- committed notifications are the durable source;
- network operations stay outside transactions and locks;
- no new event families.

Revision 2 resolves six review gaps: subscription lifecycle (§4), no-backfill eligibility
(§5), the dispatch state machine (§6), logout privacy (§7), transport security (§8) and batch
boundaries (§9).

Baseline:
- P2-1 is deployed.
- P2-2, P2-3a/b and P2-4 are implemented and automated-test verified on usapp_test, and not
  deployed.
- Migrations 0000-0021 are frozen on usapp_test. Main is at 0000-0017.
- Main rollout and phone/browser acceptance are deferred.
- `AGENTS.md` does not exist in the repository.

Labels:
- **[V1]** requirement (`_req_extract.txt`);
- **[Fact]** repository or official-source fact, with a reference;
- **[Rec]** recommendation;
- **[Owner]** decision needed;
- **[Assumption]** unexecuted or unverified.

## 1. Requirements and current state

**Requirements**
- [V1] §18.3 (`_req_extract.txt` 310-313): business event → business data → `notify_partner` →
  notification → Web Push, through one unified mechanism.
- [V1] §42 lists `PushSubscription`.
- [V1] §52.4 (999-1001): the Web Push service and compatibility are not final.
- [V1] Line 17: one partner uses an iPhone. Lines 21-22: Home Screen installation.

**Notifications**
- [Fact] Notifications are inserted only inside business transactions:
  - mandatory: `services/api/src/services/NotificationWriter.cc`;
  - optional: `NotificationIntent.cc` `kNoticeSql`.
- [Fact] Both run after the audit insert and before commit, so a committed `notifications` row
  commits atomically with its business write and is immutable (0019/0020).
- [Fact] P2-4 added no notification event (`docs/phase2-reactions.md` §5 117-122, §7).

**Scheduler**
- [Fact] Each tick takes a cross-instance `pg_try_advisory_xact_lock`, runs the job in one
  transaction, commits, then runs the job's `afterCommit` action, which must not use the
  transaction (`services/api/src/scheduler/Scheduler.h` 6-22, 66; `docs/scheduler.md` §2).
- [Fact] C++ already makes outbound calls through libcurl (`services/api/src/mailer/Mailer.cc`
  6, 144). Drogon also provides an asynchronous `HttpClient`.

**Sessions**
- [Fact] Session rows have `revoked_at` and `expires_at`, and logout revokes rather than deletes
  (`database/migrations/0002_identity.sql` 48-60; `SessionService.cc` 24-36).

**Web app**
- [Fact] The service worker has no `push`/`notificationclick` handlers (`apps/web/src/pwa/sw.ts`).
- [Fact] Updates are user-gated (`registerType: 'prompt'`, `sw.ts` 20-23, 80-86).
- [Fact] The manifest uses `display: 'standalone'` (`vite.config.ts` 22-34).

**Roles**
- [Fact] `usapp_push` has no table privileges (`database/init-roles.sql` 10, 48).

## 2. Conflicts in the current push-worker (static reading; nothing executed)

| # | Finding | Evidence |
|---|---|---|
| C1 | The policy forbids database access, but the README and queue comment plan a worker-read/written outbox. The diagram draws `PUSH --> PG`. | `src/policy.ts` 64-67; `README.md` 39 vs 91; `src/queue.ts` 135-139; `docs/architecture.md` 42 |
| C2 | The loopback-only host check runs before the `enabled` check, and Compose sets `0.0.0.0`, so starting the `push` profile would exit 78. | `src/config.ts` 253-258; `src/index.ts` 19-28; `deploy/docker-compose.yml` 121 |
| C3 | Loopback is per container: `api` cannot reach a worker bound to its own 127.0.0.1. | `docker-compose.yml` 111-138 |
| C4 | The worker receives PostgreSQL credentials. | `docker-compose.yml` 124-128 |
| C5 | `/deliver` is unauthenticated and echoes error text; the queue is in memory. | `src/server.ts` 133-162; `src/queue.ts` 173-209 |
| C6 | The image build requires a `pnpm-lock.yaml` that does not exist. | `deploy/Dockerfile.push-worker` 32-42 |
| C7 | The `env` parameter of `loadConfig` is ignored. | `src/config.ts` 218-221, 245 |

Resolution: C++ owns state. The worker becomes a stateless authenticated transport on the
private `usapp_net` bridge, reached as `http://push-worker:8090`, with no host port publish and
no database credentials. Its loopback check becomes "loopback, or private-network mode with a
token configured". The outbox text in the README and queue comment is superseded when the
worker is changed.

[Assumption] Compose DNS resolves `push-worker` from `api`; this is untested here.

P2-5c resolved C1-C7 as above (Section 16.5). Compose DNS from `api` under the production file remains unrun.

Rejected alternative: C++ implementing RFC 8291/8292 cryptography itself.

## 3. Product scope

- **What pushes:** a mirror of existing in-app notices only. That is the 18 mandatory P2-3a
  kinds and the five optional P2-3b kinds; optional kinds exist only when the sender chose them.
- **What is excluded:** no new events (reactions, reminders, progress, anomalies), no message,
  reply or note text, and no historical backfill (§5).
- **Authority:** in-app notices remain the authoritative record. Push is an optional per-device
  channel and never affects in-app notices.
- **Defaults:** off on every device. Permission is requested only on an explicit user tap.
- **Multiple devices:** a notice fans out to every eligible generation of the recipient (§5).

## 4. Subscription lifecycle

### 4.1 Identity and records

**Generation**
- A **subscription generation** is one immutable row in `push_subscriptions`: id, user_id,
  couple_id, session_id, endpoint, endpoint_hash, p256dh, auth, vapid_key_id, activated_at
  (`clock_timestamp()` at insert).
- A generation is never reactivated. Re-enabling always creates a new generation.

**Revocation**
- Revocations are immutable rows in `push_subscription_revocations` (one per generation:
  reason, revoked_at).

**Registration command (selected model)**
- Every accepted registration key is persisted as an immutable row in
  `push_registration_commands`: PK (user_id, idempotency_key), request_fingerprint,
  session_id, result (`created` | `already_active`), generation_id (FK), recorded_at.
- This includes no-op registrations. A key therefore always maps to its ORIGINAL result and
  never acts again.
- The alternative, where every key creates its own generation, was rejected: an identical
  re-registration would churn generations and revoke the active one for no reason.

**Active means all of the following:**
- no revocation row;
- the session is not revoked and not expired;
- `vapid_key_id` equals the configured key id.

### 4.2 Idempotency

**Register (`POST /api/v1/push/subscriptions`): Idempotency-Key required.**
- Fingerprint = SHA-256(`push-register:v1:` + length-prefixed endpoint_hash, p256dh, auth,
  vapid_key_id, session_id).
- The same key with a different fingerprint is IDEMPOTENCY_KEY_REUSED. A key from another
  session differs by session_id.
- Key classification runs before every state check, through the command table.

**Revoke (`POST /api/v1/push/subscriptions/{generationId}/revoke`): no key.**
- The target is an immutable generation id, so revoke is naturally idempotent.
- A retry returns the original revocation and cannot touch any other generation.

### 4.3 Serialization, device cap and lock order

**Lock order (global, always in this order, all transaction-scoped):**
1. The caller's session row `FOR SHARE`.
2. The **user lock**: advisory class U, `hashtext(user_id)`. It serializes every registration
   and revocation by that user, including user-wide counts.
3. The **endpoint lock**: advisory class E, `hashtext(endpoint_hash)`. Taken only on
   registration.

U and E are the next free classes; the current source uses 33004-33012, so recheck before
implementing.

**No deadlock**
- No path ever takes another user's U lock.
- A re-owning registration by user B takes U(B) then E. It inserts B's generation and appends
  A's revocation WITHOUT taking U(A).
- A revocation only lowers A's count, so it cannot break A's cap. A concurrent registration by
  A that counted the row first is at worst conservative (it may reject at the cap).
- Explicit revoke takes the session row and U only.
- Session logout/expiry writes nothing here: activity is derived from the session.

**Cap enforcement (W5)**
- Under U, count the user's ACTIVE generations, excluding the one being replaced in this
  command.
- A registration that would add a device at the cap returns **409 PUSH_DEVICE_LIMIT**. It
  never silently revokes another device; the user revokes one explicitly in the device list.
- Recommended default; owner approval required (§10).
- A replacement of the user's own active generation on the same endpoint (new keys, new
  session or new VAPID key) revokes and inserts in one transaction, so the count does not grow.
- Re-owning an endpoint from another user counts as a new device for the registering user.

**Database guard (re-entrant on the same locks)**
- At most one active generation per `endpoint_hash`.
- The registering user's active count is ≤ the cap after the insert.

### 4.4 Transition table

Each row is a NEW key unless stated. Every accepted key appends a command row.

| Situation | Result and recorded facts |
|---|---|
| Key already in `push_registration_commands`, same fingerprint | Replay: return the recorded result and generation id plus that generation's CURRENT state (`active` / `revoked` + reason). No new row; nothing revoked or restored. |
| Same session, same endpoint, keys and key id; generation active | Command (`already_active`, that generation); no generation row. |
| Same user, new session, same endpoint | New generation; old revoked `superseded`; command `created`. Count unchanged. |
| Same endpoint, changed p256dh/auth or VAPID key id | New generation; old revoked `replaced`; command `created`. Count unchanged. |
| After an explicit revoke of that endpoint | New generation (later activated_at); command `created`. Subject to the cap. |
| Another user's active generation on the endpoint (shared browser) | New generation for the caller; previous owner's revoked `reowned`; command `created`. Subject to the caller's cap. |
| New endpoint at the cap | 409 PUSH_DEVICE_LIMIT; no rows (a rejected key is not accepted, as with other 4xx). |
| Old revoke retry | Affects only its own target generation; no change if already revoked. |

**Required sequence (K = key, G = generation)**
1. K1 → G1 created; command(K1 → G1, `created`).
2. K2 (identical, same session) → command(K2 → G1, `already_active`); no new generation.
3. Revoke G1 → revocation(G1).
4. K3 → G3 created; command(K3 → G3, `created`).
5. Retry K2 → classification finds command(K2) → reply {result `already_active`, generation G1,
   current state `revoked`}. No generation is created; G3 is neither revoked nor altered; G1 is
   not restored.

### 4.5 Client capture and validation [Rec]

**Before submit, capture:**
- the request key;
- the request body (endpoint, keys);
- the account id;
- the session generation.

The generation id is unknown at submit time.

**On a response:**
- Apply it only if the account and session generation are unchanged.
- The returned facts must be consistent with the captured request: endpoint_hash = SHA-256 of
  the captured endpoint, vapid_key_id = the configured key, and a valid generation id.
- Only then store `{generationId, marker}`.
- A replay whose current state is `revoked` shows the device as off.
- Once a result is unknown, later errors keep the original key and body.
## 5. Eligibility and no-backfill

A comparison of `subscription.created_at` with `notification.created_at` alone does NOT
prevent backfill. It ignores disablement and cutover.

### 5.1 One authoritative dispatch control

[Rec] An append-only table `push_dispatch_control` (part of 0023):
- Columns: revision PK (contiguous), kind (`enable`, `disable`, `pause`, `resume`),
  recorded_at, actor (`owner` | `dispatcher`), reason code.

**Who writes**
- `enable`, `disable` and `resume` are written ONLY by an explicit owner operation: a
  documented SQL statement run with the maintenance role. There is no startup writer and no
  admin endpoint in this milestone.
- `usapp_api` may append only `pause` (automatic, §6.5). A guard enforces actor and kind by role.

**How instances follow it**
- Every instance reads the latest revision at startup and on each tick.
- Its local `PUSH_ENABLED` and transport configuration are a CAPABILITY, not a control.
- The dispatcher runs only when the latest effective state is `enable`/`resume` AND the local
  capability is complete.
- Mismatches fail closed:
  - control enabled but the local transport is not configured: no dispatch, and a structured
    error in logs and health;
  - local enabled but control disabled: no dispatch.
- Processes with differing flags therefore cannot move the cutover.

**Cutover**
- Cutover = `recorded_at` of the latest `enable` revision.
- `resume` after a pause does NOT move the cutover. `disable` followed by `enable` does.

**Supported rollout procedure**
1. Deploy instances with the capability configured (dispatch stays off, control absent).
2. The owner appends `enable`.
3. To stop, the owner appends `disable`.
4. After fixing a pause cause, the owner appends `resume`.

### 5.2 Eligibility of notice n for generation g (insert-time rule)

All of the following must hold:
1. `n.recipient_id = g.user_id`;
2. `n.created_at ≥ g.activated_at`;
3. `n.created_at ≥ cutover`;
4. `n.created_at > now − TTL`;
5. control is enabled (§5.1);
6. g is active at the pre-send check (§6).

All timestamps are **insert times** (`clock_timestamp()` at the row's INSERT), not transaction
begin or commit times. Consequences, stated exactly:
- A business transaction that began before activation or cutover but INSERTED its notice after
  it **is eligible**. Its notice exists after the boundary.
- A notice inserted before the boundary is skipped, even if its transaction commits after.
- Late commits are found because fan-out scans a created_at window bounded by the TTL, not a
  watermark.
- No claim is made that every boundary-straddling transaction is skipped. No extra machinery is
  added for that.

### 5.3 Cases

| Case | Behaviour |
|---|---|
| Ordinary restart, control unchanged | Pending eligible work within TTL is recovered. |
| Explicit disable, then enable | A new cutover: notices inserted before the new `enable` (including during disablement) are skipped [W3]; pending deliveries from before are terminated `cutover_skipped`. |
| Pause (automatic), then owner `resume` | The cutover is unchanged; work within TTL continues; notices older than TTL expire. |
| Device revoke, then re-enable | The new generation's activated_at excludes the gap. |
| Initial real-transport rollout | The first `enable` is appended only after batch c transport is deployed, so everything inserted earlier is never pushed. |
## 6. Dispatch state machine (batch b)

### 6.1 Tables (`0023_push_dispatch`)

All append-only; SELECT/INSERT for `usapp_api`.
- `push_deliveries(notification_id, generation_id)` PK. Inserted by fan-out after a guard
  re-checks §5.2 rules 1-4.
- `push_attempts(notification_id, generation_id, attempt_no)` PK; `claimed_at`, `lease_until`.
  attempt_no is contiguous; a new claim requires the previous attempt to be observed or
  expired.
- `push_attempt_observations(attempt key)` PK: the **observed** worker or provider response for
  that attempt (`outcome`, `class` = `provider` | `internal`, `http_status`, `retry_after_s`,
  `observed_at`). At most one per attempt, because one request yields one response; a duplicate
  recording conflicts.
- `push_attempt_expiries(attempt key)` PK: the **synthetic** lease expiry, written by a later
  claiming tick when no observation exists. Kept separate so it never occupies the observation
  slot.
- `push_delivery_outcomes(notification_id, generation_id)` PK: the terminal state, first
  writer wins.
- `push_dispatch_control` (§5.1).
### 6.2 States

A delivery's state is derived from these facts:
- **PENDING**: no attempt yet, or the last attempt's result is retryable and its backoff has
  elapsed.
- **CLAIMED**: the last attempt has no result yet.
- **Terminal** (`push_delivery_outcomes.final_state`):
  - `accepted`: the provider accepted it. This does NOT prove the device displayed it.
  - `gone`: the subscription is dead; the generation is revoked.
  - `rejected`: the message was refused.
  - `inactive`: the generation or session was inactive at claim or pre-send.
  - `expired`: TTL reached.
  - `exhausted`: the final attempt was transient or unknown, so delivery is undetermined.
  - `cutover_skipped`: §5.3.
  - `source_stale`: a reminder whose source no longer warrants it, decided at pre-send (P2-6 R5, migration 0028, [phase2-reminders.md §21](phase2-reminders.md#21-r5-implementation-and-isolated-verification-claude-code-2026-11-03)).

### 6.3 Transitions

| From | Event | Recorded facts | To |
|---|---|---|---|
| PENDING | TTL reached before the first attempt | outcome `expired` | expired |
| PENDING | Pre-claim check: generation inactive or notice before cutover | outcome `inactive` / `cutover_skipped` | terminal |
| PENDING | Claim (capacity permitting) | attempt k, lease = now + 60 s | CLAIMED |
| CLAIMED | Pre-send check finds the generation or session inactive | observation k `inactive_skipped` (class internal, no network); outcome `inactive` | inactive |
| CLAIMED | Provider `accepted` | observation k; outcome `accepted` | accepted |
| CLAIMED | Provider `gone` (404/410) | observation k; generation revocation `gone`; outcome `gone` | gone |
| CLAIMED | Provider `rejected` (400/413, 3xx) | observation k; outcome `rejected` | rejected |
| CLAIMED | Provider `transient` / `unknown`, budget left, TTL not reached | observation k | PENDING (backoff 1, 5, 30, 120 min, or `Retry-After` if longer) |
| CLAIMED | Provider `transient` / `unknown`, budget exhausted | observation k; outcome `exhausted` | exhausted |
| CLAIMED | Internal failure (`worker_auth_failed`, `transport_unavailable`, `busy`) | observation k (class internal); `pause` control row for auth failures | PENDING (not counted toward the budget; bounded by TTL) |
| CLAIMED | Lease expired, no observation | expiry row for k (written by the next claiming tick) | PENDING or exhausted (counts as one provider attempt of unknown result) |
| CLAIMED / PENDING | TTL reached during retry | outcome `expired`; an in-flight attempt may still be observed later | expired |
| any | Provider `provider_auth_failed` (401/403) | observation k; control `pause` | retryable; dispatcher paused |

**Provider-attempt budget (5)**

Count the attempts of a delivery that have either:
- an observation of class `provider`, or
- an expiry and no observation.

Attempts observed as class `internal` (worker auth, transport unavailable, busy, inactive
skipped) do not consume the budget. They are bounded only by the TTL.

### 6.4 Late and duplicate results

**Late observation after a synthetic expiry for the same attempt**
- The observation row IS inserted, because the observation slot was never occupied by the
  expiry. Both facts remain in the database: the expiry (synthetic) and the observation
  (actual).
- The attempt still counts once toward the budget.

**Delivery outcome**
- First writer wins.
- If the outcome is already `expired` (or `exhausted`) and a late `accepted` arrives, the
  outcome stays, the observation records the acceptance, and reports join both.
- If no outcome exists yet, a late `accepted` / `gone` / `rejected` inserts it.

**Side effects of a late or duplicate result**
- `gone` revokes the generation; revocation is idempotent per generation.
- An outcome insert-if-absent.
- Nothing else. A concurrently claimed attempt k+1 may still send, which is a duplicate.

**Duplicate observation for the same attempt** (for example a retried recording transaction)
- Conflicts on the observation PK. The first observation stands.
- Only the idempotent side effects above may run.

**Required test sequence**
1. Claim attempt 1.
2. The lease expires; the next tick writes expiry(1).
3. TTL passes; outcome `expired` is written.
4. A late provider `accepted` for attempt 1 arrives.

Expected: observation(1) = accepted is stored; the outcome stays `expired`; querying both
returns both facts; the budget count for the delivery is 1.

### 6.5 Capacity, fairness and pause scope

**Capacity**
- The authoritative global bound on concurrent provider sends is the **worker's own
  semaphore** (4), because the worker is the only component that opens provider connections.
- C++ claims are additionally limited, under the scheduler lock, to C = the worker semaphore
  size of unobserved, unexpired attempts. This keeps lease and claim volumes aligned.
- A send that arrives while the worker is full gets `busy` (class internal), so exceeding the
  bound cannot produce extra provider traffic.

**Fair selection**
- Order by provider attempts used ascending, then `n.created_at`, then generation id.
- At most 2 claims per recipient per tick.
- The fan-out scan is limited to 100 notices per tick.

**Pause**
- Scope is the whole dispatcher (shared VAPID key and transport).
- Started by a `pause` control row (automatic on provider or worker auth failure).
- While paused: no new claims; fan-out continues; TTL keeps running.
- Ends only with an owner `resume` revision (§5.1). The cutover is unchanged.
### 6.6 Execution without blocking Drogon threads

- The `afterCommit` action only starts asynchronous `drogon::HttpClient` requests to the
  worker, each with its 15 s deadline.
- Each response callback issues an asynchronous ORM transaction to record the result.
- No blocking wait, `std::future::get`, sleep or synchronous libcurl call runs on an event-loop
  or ORM callback thread.
- [Assumption] The Drogon `HttpClient` timeout covers the whole request; this must be confirmed
  in its documentation.

### 6.7 Delivery semantics

- Bounded retries can produce duplicate sends AND can still deliver nothing.
- Provider acceptance does not prove device display.
- The notification `tag` = notification id asks browsers to replace rather than stack
  duplicates. [Assumption] This holds on Chromium/Firefox and is unverified on iOS.

### 6.8 Locks

- Dispatch transaction: the scheduler lock only.
- Recording transaction: no advisory lock.
- Registration and revocation follow §4.3: session row, then U, then E.
- The dispatcher takes no domain, wallet, preference, user (U) or endpoint (E) lock. Its
  `gone` revocation appends a revocation row without U, which is safe because it only reduces
  counts (§4.3).
- No lock cycle exists.

## 7. Logout and account-switch privacy

### 7.1 Marker

**What it is**
- On a confirmed registration, the server returns an opaque **device marker**:
  HMAC-SHA-256(server secret, generation id), truncated to 16 bytes, base64url.
- The client stores it in IndexedDB (`usapp-push`, key `marker`) together with the generation
  id.

**Permitted purpose and contents**
- Its only purpose is to let the service worker decide whether a push belongs to the
  generation currently registered on this device.
- It contains no business text, user id, email or session token.
- It cannot authenticate any API call.

**Lifecycle (clear or replace)**
- Replaced on each confirmed new generation for this device.
- Cleared at:
  - logout initiation, before the server call;
  - local logout even when the server call fails;
  - every identity switch (the session store's identity reset);
  - bootstrap failure;
  - permission denied or revoked;
  - an explicit device revoke;
  - a `pushsubscriptionchange` event, until re-registration confirms.

### 7.2 Payload and service worker behaviour

**Payload** (≤ 1 KB, encrypted by the push protocol):
`{v:1, nid, event_type, target_kind, target_id, marker}`. No names or text.

**`push` handler (always shows a notification; userVisibleOnly)**
- Marker present and equal: the fixed per-event label from the existing inbox label table;
  `tag` = nid; data `{nid, target, marker}`.
- Marker absent or mismatched: generic "你有一条新通知"; `tag` = `usapp-generic`; data
  `{target: '/notifications'}`.
- An in-flight push that arrives after unsubscribe or logout falls into this branch.

**`notificationclick` handler**
- Re-reads the stored marker AT CLICK TIME.
- Equal: navigate to the allowlisted same-origin path built from `target_kind` and a
  UUID-validated id (`/us/messages/<id>`, `/us/anniversaries/<id>`, `/tasks`, `/rewards`,
  `/notifications`).
- Not equal (for example a notification shown for account A and clicked after switching to B):
  open `/notifications` only.
- Pages still enforce server authorization (another user's id is a 404).

**On logout**
- Best effort: close notifications that carry the old marker
  (`registration.getNotifications()`).

**Residual risk (not eliminated)**
- A label shown before logout may remain visible if closing fails.
- A push sent just before revocation can display the generic text on the next account's
  device session.
- Server-side session revocation and the pre-send check narrow these windows; they do not close
  them.

## 8. Transport security and deadlines (batch c)

### 8.1 Library facts

From the official sources: npm registry metadata, `web-push-libs/web-push` v3.6.7
`src/web-push-lib.js` and its README, read 2026-10-30.
- Version 3.6.7, license field `MPL-2.0`, engines `node >= 16`.
- Dependencies: jws, asn1.js, http_ece 1.2.0, minimist, https-proxy-agent.
- `sendNotification`: when a `proxy` option is set, the custom `agent` is ignored ("Agent option
  will be ignored because proxy option is defined") and replaced by `HttpsProxyAgent`.
- The response body is buffered with no size limit.
- `timeout` is a socket inactivity timeout (`'timeout'` event → destroy), not an absolute
  deadline.
- No redirect following: one `https.request`, and any non-2xx (including 3xx) rejects.
- `generateRequestDetails(subscription, payload, options)` is a documented public API returning
  `{endpoint, method, headers, body}`.

### 8.2 Selected path

The worker calls ONLY `generateRequestDetails` (with `vapidDetails`, `TTL`, `urgency` and
`contentEncoding: 'aes128gcm'`) for encryption and VAPID headers. It never uses
`sendNotification` and never passes `proxy`. It then sends the POST itself with `node:https`.

**Node 24 documentation facts** (nodejs.org/docs/latest-v24.x/api/http.html, read 2026-10-30):
- `agent: false` "causes a new `Agent` with default values to be used"; "Use `agent: false` to
  avoid connection reuse for a request".
- Agent `keepAlive` defaults to false.
- `lookup` is a request option ("Custom lookup function").
- `signal`: aborting it "will behave the same way as calling `.destroy()` on the request".
- `timeout` "will not abort the request or do anything besides add a `'timeout'` event".
- Environment proxies apply to the global agent when `NODE_USE_ENV_PROXY=1` or `--use-env-proxy`
  is set, or to a custom agent constructed with `proxyEnv`.

**Request-scoped connection**
- Each send constructs a NEW `https.Agent({ keepAlive: false, maxSockets: 1 })` with no
  `proxyEnv`, uses it for exactly that one request, and calls `agent.destroy()` when the
  request settles.
- A fresh agent has an empty pool, so no socket can be reused across sends.
- `keepAlive: false` alone is NOT relied on as proof of a fresh socket.
- `agent: false` would also give a one-time agent. The explicit per-request agent is chosen
  because it makes "no proxyEnv" and the destroy point explicit.

**Request options**
- `lookup` = the controlled resolver (§8.3 address policy). It normalizes IPv4-mapped IPv6 and
  returns only allowed addresses.
- Default TLS verification; SNI and the certificate check use the hostname.
- `signal: AbortSignal.timeout(10_000)` as the ABSOLUTE deadline over DNS, connect, TLS, write
  and response. The socket `timeout` option is not used as a deadline.

**Address actually used**
- On `secureConnect`, `socket.remoteAddress` is normalized and re-checked; the request is
  destroyed otherwise. With no reuse, this check covers every connection the worker makes.

**Proxies**
- The worker refuses to start if `NODE_USE_ENV_PROXY` is set or if `--use-env-proxy` appears in
  `process.execArgv` or `NODE_OPTIONS`.
- [Assumption, inferred] The docs do not state explicitly that a custom agent without
  `proxyEnv` ignores environment proxies. That is inferred from "custom agents can also be
  created with proxy support by passing a `proxyEnv` option". The startup refusal covers it
  either way.

**Responses**
- At most 4 KB of the response is read, then the request is destroyed; the text is never logged.
- Any 3xx is `rejected` and never followed.

**Concurrency**
- The worker semaphore (4) bounds simultaneous sends (§6.5); excess gets 503 `busy`.

### 8.3 Endpoint rules

Applied at registration (C++) and again in the worker:
- Parsed with a WHATWG URL parser.
- Scheme `https`, port empty or 443, no userinfo, no fragment, at most 2048 bytes.
- Host is lowercase ASCII `[a-z0-9.-]`, has no trailing dot and is not an IP literal.
- The host must equal a listed host or end with `.` + a listed suffix. The label boundary is
  required, so `evilpush.apple.com.attacker.net` and `xpush.apple.com` do not match `.push.apple.com`.
- Allowlist [Owner, configurable]:
  - exact: `fcm.googleapis.com`, `updates.push.services.mozilla.com`;
  - suffixes: `.push.apple.com` (Apple: "allow URLs from `*.push.apple.com`", WebKit blog 13878),
    `.notify.windows.com`.
- `p256dh` is a 65-byte uncompressed point starting with 0x04; `auth` is 16 bytes.
- Request body ≤ 4 KB. At most 10 active generations per user.

### 8.4 Authentication and failure classes

**Internal call (C++ → worker)**
- `POST /v1/deliver` with `Authorization: Bearer <PUSH_WORKER_TOKEN>`, compared in constant
  time.
- Body ≤ 4 KB. Concurrency semaphore of 4; excess requests get 503 `busy`.

**Failure classes C++ distinguishes**
- `worker_auth_failed`: the worker returned 401 for the internal token.
- `transport_unavailable`: connect failure or 503.
- `provider_auth_failed`: the provider returned 401/403 (VAPID/JWT problem).
- The provider outcomes in §6.3.

Both auth failures pause dispatch but are reported separately.

**Logging**
- Never log endpoints, `p256dh`/`auth`, VAPID private keys, tokens or payloads.
- Log the generation id, endpoint host suffix, outcome class and status code.

### 8.5 License note

- [Fact] The license field is MPL-2.0 (npm metadata). That is a file-level copyleft license
  with obligations; for example, distributing covered files carries source-availability and
  notice requirements.
- No legal conclusion is drawn here.
- [Owner] Confirm acceptability before the dependency is added.
- Adding it also requires the owner to generate and commit `pnpm-lock.yaml` (resolves C6).
- Superseded for P2-5c: the assignment authorized adding it and generating the lockfile (Section 16). Not committed.

### 8.6 VAPID keys

- Generated once by the owner, offline.
- The private key exists only in the worker's environment. The public key is in `api` and the
  worker, never in the database.
- `vapid_key_id` = SHA-256 of the public key.
- Rotation makes every old-key generation inactive (§4.1). Clients re-register on next use, and
  old generations are revoked as `key_rotated`.

## 9. Batches, migrations and approval boundaries

### P2-5a: subscription lifecycle and opt-in UI, behind closed gates

**Migration `0022_push_subscriptions`**
- Only `push_subscriptions` (generations), `push_subscription_revocations` and
  `push_registration_commands`.
- Guards: one active generation per endpoint; active count ≤ cap; command→generation link;
  actor/session/time; append-only.
- Advisory classes U and E (§4.3); grants; init-roles; startup policy; deviation query.
- No delivery or control tables.

**API**
- `GET /api/v1/push/config` → `{subscribe_available, vapid_public_key}`.
- `GET /api/v1/push/subscriptions` lists generations without endpoints.
- `POST /api/v1/push/subscriptions` with Idempotency-Key and command replay.
- `POST /api/v1/push/subscriptions/{generationId}/revoke`.
- 409 PUSH_DEVICE_LIMIT.
- Activity derived from sessions.

**Web**
- Me-page device card and store, following §4.5.
- Marker lifecycle (§7.1).
- Tests use mocked `Notification`, `PushManager` and IndexedDB only.

**Gate**
- `subscribe_available` is true only when `PUSH_SUBSCRIBE_ENABLED=true` AND transport is
  configured. Both are false in batch a.
- Real opt-in is therefore impossible outside tests that inject configuration.
- Batch a claims nothing about phone notifications.

**Acceptance**
- Every §4.4 row, including the K1/K2/K3 sequence (a retry of K2 neither revokes G3 nor
  restores G1).
- KEY_REUSED; authorization and 404s; Origin/CSRF.
- Concurrent re-own of one endpoint leaves exactly one active generation.
- **Concurrent distinct-endpoint registrations at cap − 1 by one user: exactly one succeeds,
  the other gets 409**.
- **At the cap, a same-endpoint key or session replacement succeeds and the active count stays
  at the cap**.
- Re-own at the caller's cap gets 409.
- Logout and expiry deactivate.
- No endpoint in any response or log.
- Old-response handling and account switch on the client.
- The SQL guard script in both roles.

### P2-5b: durable dispatch with fake transport

**Migration `0023_push_dispatch`**
- The §6.1 tables, including `push_dispatch_control`.

**Code**
- Control reader (no startup writer), `push.dispatch` job, async worker client behind an
  interface.
- A fake transport for tests.

**Gate**
- The dispatcher runs only when the §5.1 control is enabled AND the local capability is
  complete. No real transport exists yet, so nothing can be sent.

**Acceptance**
- Every §6.3 transition, including the final attempt transient/unknown → exhausted, TTL before
  the first attempt and during retry, and duplicate observation recording (first observation
  wins; only idempotent side effects).
- The §6.4 sequence: expiry(1), then outcome `expired`, then a late observed `accepted` for
  attempt 1. Both facts are stored, the outcome is unchanged and the budget count is 1.
- Internal failures do not consume the provider budget.
- §5.1 control: an instance with a mismatched local capability fails closed; a startup never
  writes control rows; `resume` does not move the cutover.
- Claim capacity respected across two concurrent ticks; worker `busy` beyond its semaphore.
- Fairness cap.
- Pause and resume.
- §5.3 cases: restart recovery, disable/enable skip, device re-enable skip, initial cutover,
  late-commit window, and an insert-time case (a transaction begun before activation that
  inserts after it IS eligible).
- A business rollback creates no notice, therefore no delivery; a replay creates none.
- The network call starts only after commit: the fake asserts no open transaction.
- No blocking calls on event-loop threads (review plus fake latency).

### P2-5c: real transport and service worker

**Worker**
- §8 (auth, limits, SSRF and socket checks, deadline, response cap).
- The `web-push` dependency after the license decision and the owner-generated lockfile.
- Compose C2-C4 fixes.

**Service worker**
- `push` / `notificationclick` (§7) and a capability reply (`{type: 'CAPABILITIES'}` →
  `push:v1`).

**Opt-in becomes available only when ALL of these hold:**
- the server reports `subscribe_available`;
- the ACTIVE service worker reports `push:v1` (old workers waiting behind the update prompt do
  not);
- the device supports `PushManager` (on iOS, only when installed to the Home Screen).

**Acceptance**
- Worker tests against a stub HTTPS endpoint and a stub resolver: status mapping, 3xx
  rejection, private and mapped-address refusal at lookup AND at socket, a new agent per send
  (two sends never share a socket), 4 KB cap, absolute deadline via the signal, semaphore
  `busy`, startup refusal with env-proxy settings, token failure, no secrets in logs.
- SW pure-function tests: labels, generic branch, click under a mismatched marker → `/notifications`.

**Separate approvals:** real-device acceptance (Android Chrome, iOS Home Screen) and any main
rollout of 0018-0023 each need their own owner authorization.

## 10. Owner decisions (recommended defaults)

**Needed for P2-5a**

| # | Decision | Default |
|---|---|---|
| W4 | Push stops at logout/session expiry (session-bound generations) | Yes |
| W5 | Active devices per user, and behaviour at the cap | 10. A new device at the cap is **rejected** (409); the user explicitly revokes another. Nothing is silently revoked. **Needs owner approval.** |
| W7 | Push-service allowlist, validated at registration | FCM and Mozilla (exact hosts); `.push.apple.com` and `.notify.windows.com` (label-boundary suffixes) |
| W9 | VAPID keys | Owner-generated once, environment only, rotate on compromise. The public key is needed only once subscription opens, which is batch c in practice. |

**Needed later (b/c)**
- W1: push all existing in-app kinds.
- W2: fixed per-event lock-screen label (alternative: generic).
- W3: skip notices created while disabled.
- W6: 24 h TTL / 5 provider attempts.
- W8: `web-push` 3.6.7 after MPL-2.0 acceptability is confirmed.

Migrations are per batch (0022 in a, 0023 in b, none planned in c). Retention is deferred.

## 11. Risks and unverified assumptions

- Nothing here was executed; all runtime statements are assumptions.
- Platform reachability (FCM from mainland-China networks, APNs) is unknown.
- iOS Web Push under the locally trusted LAN CA is unknown.
- `tag` replacement on iOS is unknown.
- Docker DNS reachability is untested.
- Node environment-proxy opt-in behaviour and Drogon `HttpClient` deadline semantics must be
  confirmed in their documentation.
- At-least-once delivery: duplicates are possible, and bounded retries can still deliver
  nothing.
- Privacy windows at logout and re-own are narrowed, not eliminated (§7.2).
- Eligibility uses insert time: a transaction that began before activation or cutover but
  inserted its notice after it is pushed; one inserted before is not (§5.2).
- Fan-out scan cost is unmeasured.
- Service-worker updates are user-gated, so push becomes available per device only after the
  update is accepted.
- Worker issues C2-C6 must be fixed before any push profile can run.

## 12. P2-5a implementation and verification (Claude Code, 2026-10-30)

Scope: owner-approved P2-5a only (W4 session-bound generations, W5 cap 10 with rejection,
W7 revision-3 allowlist; keys are the owner's and none was generated or requested). P2-5b
dispatch, P2-5c transport, P2-6 reminders, the push-worker, main database, deployment and the
served web dist are untouched.

### 12.1 Status

| Stage | State |
|---|---|
| Implemented | Yes: migration 0022, `PushService`, `PushController`, contract 0.19.0, web client/store/marker/card. |
| Automated-test verified | Yes, on usapp_test (12.4). |
| Applied on test | `0022_push_subscriptions` applied to **usapp_test only** and FROZEN (sha256 below). Main remains 0000-0017. |
| Deployed | **No.** No API/Caddy/web rollout; no served-dist build. |
| Device accepted | **No.** No real permission prompt, subscription or push was made; subscription is closed by design. |

### 12.2 What was built

**Migration `0022_push_subscriptions`** (only three tables, no UPDATE grant, no delivery or
control tables):
- `push_subscriptions`: immutable generations (user, couple, session, endpoint, endpoint_hash,
  p256dh, auth, vapid_key_id, activated_at). Member FK `(couple_id, user_id)`, session FK,
  endpoint allowlist CHECK (exact `fcm.googleapis.com` / `updates.push.services.mozilla.com`,
  label-boundary `.push.apple.com` / `.notify.windows.com`, https, no port or userinfo, printable
  path, at most 2048), `endpoint_hash = sha256(endpoint)`, base64url key shapes.
- `push_subscription_revocations`: one immutable row per generation; reason `user | superseded |
  replaced | reowned`; `caused_by` successor (deferred FK); actor FK to `couple_members`; the
  owner revokes `user/superseded/replaced`, another user only `reowned`.
- `push_registration_commands`: every accepted key, `created` or `already_active`, PK
  `(user_id, idempotency_key)`; exactly one `created` command per generation (partial unique).
- Guards: UY001 append-only (statement triggers, also TRUNCATE); UY002 generation guard (session
  valid and owned, time window, at most one unrevoked generation per endpoint, conservative cap);
  UY003 revocation guard (target exists and owner matches, time window, `user` revocation from the
  owner's space); UY004 command guard (generation, couple and session match, session valid,
  `already_active` only on an unrevoked generation); UY005 deferred links (every generation has
  its creating command; a successor revocation names a generation on the same endpoint created by
  the revoker, with the reason matching the actual change).
- Locks: 33013 user (`hashtext(user_id)`), 33014 endpoint (`hashtext(endpoint_hash)`), taken by
  the guards and re-entrant with the service.

**How the database guard enforces the cap and endpoint invariant without trusting the caller.**
The guard never reads a caller-supplied cap, key id or configuration: the cap is the literal 10
in the trigger, and the count is "unrevoked generations of this user whose session is
authenticated, not revoked and not expired at statement time", whatever their `vapid_key_id`.
The configured key id is server configuration the database cannot see, so the database rule is
deliberately conservative: it may count a generation the API shows as `inactive` (an old key),
never fewer. The endpoint invariant is "at most one unrevoked generation per endpoint_hash", across
users and session states. Both are evaluated after taking the user and endpoint locks, so two
transactions cannot both pass at cap - 1. The service uses the same counting rule, so a request
the service accepts is never rejected by the guard for the cap.

**Backend** (`services/api/src/services/PushService.{h,cc}`,
`services/api/src/controllers/PushController.cc`):
- Endpoints: `GET /api/v1/push/config`, `GET /api/v1/push/subscriptions` (complete
  slot-occupying `devices` plus bounded `history`, see 12.6; no endpoint, key or hash), `POST /api/v1/push/subscriptions` (Idempotency-Key),
  `POST /api/v1/push/subscriptions/{generationId}/revoke`. All behind AuthFilter (formal
  session; writes need exact Origin and session CSRF). Responses use the shared private no-store
  headers.
- Register order (one transaction): session row FOR SHARE (must be the caller's valid session)
  -> member row FOR SHARE -> user lock 33013 -> key classification (same fingerprint: replay the
  ORIGINAL command plus the generation's CURRENT state, read-only; otherwise
  IDEMPOTENCY_KEY_REUSED) -> endpoint lock 33014 -> the unrevoked generation on the endpoint ->
  `already_active` (same user, session, keys and key id) or a replacement reason -> cap count
  excluding only the user's own replaced generation (409 PUSH_DEVICE_LIMIT; nothing written) ->
  predecessor revocation -> new generation -> `created` command -> audit (ids and labels only) ->
  read back -> commit. Fingerprint: SHA-256 of length-prefixed endpoint_hash, p256dh, auth,
  vapid_key_id and session_id.
- Revoke: session/member FOR SHARE -> user lock -> the caller's own generation (other or unknown:
  404 PUSH_SUBSCRIPTION_NOT_FOUND) -> `user` revocation `ON CONFLICT DO NOTHING`; an existing
  revocation (earlier revoke or a successor) is returned as the original facts.
- Activity is derived (no writes at logout or expiry): revoked, else active when the session is
  valid and the key id is the configured one, else inactive.
- An unconfirmed commit is 503 RESULT_UNKNOWN; errors log fixed stage labels only; the endpoint
  and keys never reach a response list, audit detail or log.
- Gate: `subscribe_available = PUSH_SUBSCRIBE_ENABLED && valid PUSH_VAPID_PUBLIC_KEY &&
  transport`. `PushController::kTransportReady` is the constant `false` in P2-5a, so the API
  answers registration with 409 PUSH_UNAVAILABLE and config reports `false` / `null` key.
  No Compose or environment change was made.
- Marker: base64url of the first 16 bytes of HMAC-SHA-256(SESSION_SECRET,
  `push-marker:v1:` + generation id), returned only with an active generation.
- Errors added: PUSH_UNAVAILABLE (409), PUSH_DEVICE_LIMIT (409), PUSH_SUBSCRIPTION_NOT_FOUND (404).
- Registered in `init-roles.sql` section 20, `TaskPrivilegePolicy` (matrix, required columns,
  table list), the unit policy lists and `database/tests/task_privilege_deviation.sql`.

**Contract** 0.18.0 -> 0.19.0, additive: four operations, `PushConfig`,
`PushRegistrationRequest`, `PushGeneration`, `PushRegistrationResponse`,
`PushRevocationResponse`, three error codes. No local YAML parser exists; the contract was
checked by the existing static-check and contract tests only.

**Web** (`apps/web/src/api/push.ts`, `src/stores/push.ts`, `src/pwa/push-marker.ts`,
`src/pwa/push-env.ts`, `src/components/PushDeviceCard.vue`, Me page, session hooks):
- Closed availability gate: unsupported, loading, error, closed, denied, offline, available.
  Real opt-in additionally needs an active service worker advertising `push:v1`;
  `push-env.ts` `workerCapable()` is `false` in P2-5a (no worker push or click handler, no
  transport), so the button stays disabled outside tests. No permission prompt on load.
- Before submit the store captures the key, the body, the account, the identity epoch, the
  marker epoch and the expected `endpoint_hash` / `vapid_key_id`. Command facts and current state
  are validated separately; a malformed success stays unknown. Unknown results keep the exact
  key and body; only the explicit retry resends; no automatic retry, no offline queue.
- Ordering: identity epochs drop late results; one registration action at a time and one revoke
  action per generation; a list applies only as the newest read, and every confirmed write starts
  a new read, so a list in flight during a write is never applied; the device generation changes
  only on a confirmed active registration not older than the one that set it, and is turned off
  only by a result naming that exact generation; the marker is cleared per generation
  (`clearMarkerFor`) for revokes and lists. Revocation knowledge dominates late results (12.6).
- Marker lifecycle (IndexedDB `usapp-push`/`marker`): one serialized promise chain per tab;
  `clearMarker()` bumps the epoch synchronously, so a write captured before it is skipped and a
  write already running is followed by the delete. Cleared at logout initiation (before the
  server call), at every identity reset (logout, bootstrap failure, anonymous bootstrap, account
  switch), on permission denied, and per generation on revoke. On a first load (no identity in
  memory) the marker is kept and reconciled against the account's complete device set (12.6): one
  that is not an active current-session device of this account is cleared by generation.
- Cross-tab boundary (stated, not guaranteed): a clear is broadcast on `usapp-push-marker`; other
  tabs bump their epoch on receipt, so their not-yet-started writes are skipped. A write another
  tab had already started can still land after the delete. The authoritative protection is the
  server: an ended session's generation is inactive and the P2-5c worker will compare markers.
- No service-worker push or click handler, no worker transport, no new dependency.

### 12.3 Required tests

- C++ db_gated (`services/api/tests/PushDbTests.h`, block 29, real `PushService` on usapp_test):
  the K1/G1 -> K2 `already_active` -> revoke G1 -> K3/G3 -> retry K2 sequence (K2 replays its
  original facts with G1 `revoked`; G3 untouched; G1 not restored; no endpoint or key in the
  response or audit); revoke replay; same-key concurrency (one Created, one Replayed, one
  command); KEY_REUSED for another payload, another session and another key id; concurrent
  distinct endpoints at cap - 1 (exactly one Created, one DeviceLimit, nothing written for the
  rejected key); at the cap, `replaced` (new keys), `superseded` (new session) and `replaced`
  (new key id) keep the count at 10; a revoke frees a slot; re-own at the caller's cap is
  rejected without harming the prior owner; below the cap it re-owns (`reowned`, revoked_by =
  new owner); cross-user revoke is 404; concurrent re-own of one endpoint leaves exactly one
  active generation; logout and expiry make generations inactive without writes and block
  registration and revoke from the ended session, and a fresh session supersedes; no space ->
  SpaceNotFound; another user's session -> SessionInvalid; every generation has exactly one
  creating command; a generation without its command is rejected and leaves nothing;
  UPDATE/DELETE are denied for usapp_api.
- C++ unit (`PushUnitTests.h`): allowlist and label boundary; port, userinfo, fragment,
  uppercase, IP, backslash and length; strict base64url and key shapes; exact request shape;
  fingerprint inputs; marker determinism and opacity; closed gate without transport; list JSON
  without hashes; error mapping.
- Static check: append-only triggers, lock keys and order, no UPDATE grant, no delivery tables,
  the closed transport constant, list JSON without hashes, routes, contract, migration gate.
- SQL (`database/tests/push_subscription_test.sql`, both roles, ROLLBACK): 22 failure cases
  (allowlist, suffix, port, hash and key shape -> 23514; occupied endpoint, expired or foreign
  session and cap -> UY002; cross-space user revocation -> UY003; partner user revocation ->
  23514; second revocation -> 23505; no-op on a revoked generation and session mismatch -> UY004;
  successor elsewhere and generation without command -> UY005; duplicate key -> 23505; re-owned
  by the owner -> 23514; UPDATE/DELETE/TRUNCATE -> 42501 as usapp_api, UY001 as maintenance).
- Web (`apps/web/tests/unit/push.spec.ts`, 28 tests): closed gate (server false; no push:v1
  worker); unsupported, denied, offline and error; no prompt on load; permission only from enable;
  capture and marker write; unknown retries with the identical key and body (also after a 4xx);
  definitive rejection; eight malformed-success variants; a revoked replay affecting only its own
  generation; late register after an account change; pending marker write then identity reset; a
  queued write captured before a clear; logout clears the marker before the server call; late old
  revoke vs a newer generation; revoke of this device; malformed revoke success; a list started
  before a confirmed write; an older list read; first-load reconciliation; account-switch clear.
  Mutation check: removing the generation-targeted device-off guard or the marker epoch check made
  tests fail. A third guard (a write counter in `refresh`) was found unexercised, because every
  confirmed write already starts a superseding read, and was removed.

### 12.4 Verification on usapp_test (2026-10-30, Claude Code)

Scripts: `Claude outputs/p25a-verify-usapp-test.ps1` (run 1) and
`Claude outputs/p25a-verify-resume.ps1` (run 3, after the repairs). Logs:
`%TEMP%\usapp-p25a-verify-20261030_234609` and `%TEMP%\usapp-p25a-resume-20261030_234940`.

1. Baseline: 0000-0021 registered with matching checksums; frozen hashes of 0018-0021
   asserted; no push tables.
2. Rollback-only prevalidation (migration body + guard script in one transaction): both roles
   passed (`groups=3 cases=22`); afterwards 0 registrations, no push tables, 0 fixture users.
3. Migration reviewed against the service write order (session/member FOR SHARE -> 33013 ->
   classification -> 33014 -> predecessor revocation -> generation -> command); no change needed.
4. DryRun: 22 skipped, only `0022_push_subscriptions` pending,
   **sha256 `295d26983903999f71a52a69e79577a48063b16e8f44237ca478fec8c52aa610`**.
5. Apply with `-InitRoles`: exactly 0022 applied; privilege deviation query 0 rows.
6. Repeat `-InitRoles`: 23 skipped, 0 applied; deviation query 0 rows; all 23 registered
   checksums equal the local files (checked again in run 3).
7. Fresh test image (exit 0; the only compiler warning is the pre-existing
   `NotificationIntentUnitTests.h:42`) and full CTest: unit, db_gated (12.84 s; ran, not
   skipped) and static_check passed, 3/3.
8. SQL scripts, each as usapp_api and postgres, all ending in ROLLBACK: push_subscription (22
   cases), task_reaction, notification_choice, notification_ledger, message_ledger,
   anniversary_ledger, task_ledger, wallet_ledger, product_ledger, redemption_ledger.
9. Web: 28 files / 375 tests passed; `vue-tsc` and node `tsc` exit 0. No served-dist build.

**Failures recorded (run 1, after 0022 was applied; all test defects, fixed in tests only):**
- static_check: the needle `BEFORE UPDATE OR DELETE OR TRUNCATE ON <table>` did not match the two
  trigger definitions that wrap a line. The check now matches
  `CREATE TRIGGER <table>_append_only BEFORE UPDATE OR DELETE OR TRUNCATE`.
- db_gated, cap case: the test expected `inactive` in the register response of a call configured
  with a different key id. Under that configuration the generation is correctly `active`. The
  test now asserts `active` there and `inactive` in a list under the current key.
- db_gated, linkage case: an autocommit statement inserting a generation without a command left
  no row (count unchanged), but Drogon's pipelined connection did not surface the commit-time
  deferred error, so `rejected` stayed false. The case now makes the link immediate inside the
  statement (`DO ... SET CONSTRAINTS ALL IMMEDIATE; INSERT ...`) and keeps both assertions. This
  observation is recorded as a risk (12.5).
- Run 2 stopped before any database step: the generated resume script contained a tab-mangled
  path. It was fixed and rerun as run 3.

**Fixture effects:** the new FKs reference `sessions`, `couple_members` and `push_subscriptions`
only. No existing SQL script truncates or deletes sessions, members or users, and the Phase 0
db_gated cases that delete their own users never create push rows. No TRUNCATE list changed;
expected SQLSTATEs are unchanged. db_gated commits isolated `@m3-fixture.test` users, sessions
and push rows on usapp_test, as earlier milestones do (never reset).

### 12.5 Remaining limits and risks

- Not deployed; main is untouched (0000-0017). No browser, phone or real push service was used;
  no permission prompt, subscription or VAPID key exists. Subscription is closed by the
  `kTransportReady = false` constant and the web `workerCapable()` probe.
- The database cap is conservative (it counts old-key generations). A key rotation (P2-5c) needs
  the planned `key_rotated` handling, which needs a new migration because 0022 is frozen.
- Cross-tab marker coordination is best effort (12.2).
- (Superseded by 12.6: reconciliation now uses the complete device set.)
- Drogon pipeline mode did not report a commit-time deferred-constraint error for an autocommit
  statement (12.4). Services commit explicitly through `TxScope`; whether every explicit COMMIT
  failure is reported in this mode was not separately tested.
- The endpoint is validated by a hand-written parser, not a WHATWG URL parser. It is stricter
  than the database CHECK (it rejects ports, uppercase, `#` and `\`).
- `p256dh` is checked for shape (65 bytes, leading 0x04), not for lying on the P-256 curve.

### 12.6 Review repairs R1 and R2 (Claude Code, 2026-10-31)

Scope: the two Codex review findings only. 0022 unchanged (sha256 above; registered checksum
re-asserted on usapp_test); no schema or grant change; no migration, InitRoles or SQL script rerun;
nothing deployed.

**R1: a confirmed revocation dominates late results** (`apps/web/src/stores/push.ts`).
Defect: a delayed no-op registration response (active G1 with its marker) arriving after a
confirmed `revoke(G1)` found `device == null` and restored G1 and its marker; only a later refresh
could correct it, and that refresh may fail or stay pending.
Repair:
- Every revocation the store confirms or observes (revoke response, revoked replay, list history)
  is remembered per generation for the identity (cleared by the identity reset). It is immutable,
  so any later registration response, list or reconciliation that shows that generation unrevoked
  is older and is overlaid as revoked (`settle`). The device, its marker and the "enabled" notice
  are never restored; no follow-up refresh is relied on.
- A confirmed registration still ends its action (immutable command success); the device state
  comes only from the overlaid current generation, and the notice says the request was recorded
  but the device is off. Unknown actions keep their key and body as before.
- The marker is still cleared only per generation (`clearMarkerFor`), so an old G1 revoke cannot
  clear G3 (retested).
- Reconciliation re-checks identity, account and read sequence AFTER the asynchronous marker read;
  a superseded read applies nothing. Every successful list now reconciles.

**R2: complete device coverage** (`PushService.cc` `kListSql`, `PushController.cc`, contract,
`api/push.ts`, store, `PushDeviceCard.vue`).
Defect: a newest-20 list could hide every slot-occupying device behind newer revoked history, and
"not listed" was used as evidence when reconciling the marker.
Contract (0.19.0 was never released or deployed, so it was corrected in place):
- `devices`: COMPLETE set of slot-occupying generations (unrevoked and session valid, any key id,
  including old-key `inactive` ones). The 0022 guard bounds it at 10; the query fetches 11 so a
  broken invariant fails as an error (`cap_invariant`) instead of being truncated. No arbitrary
  limit was raised.
- `history`: newest 20 other generations (revoked or session ended), bounded, never displacing a
  device, never used as evidence.
- `occupied_slots` = length of `devices`; the card says "已占用 n / 10 个设备名额" and labels
  old-key devices as still occupying a slot. The empty state appears only after a successful list.
- Because `devices` is complete, absence from it is authoritative: the device generation and the
  stored marker are kept only for an active, current-session, not-known-revoked member of
  `devices`, otherwise turned off / cleared for exactly that generation.
- The web validator requires `occupied_slots == devices.length`, no revoked device, no duplicate
  ids and the bounds above.

**New regression cases**
- Web (`tests/unit/push.spec.ts`, now 36 tests): R1 delayed active registration -> confirmed
  revoke -> late registration, with the follow-up refresh failing and with it pending (2 cases);
  a later list plus a late marker showing the revoked generation active; a reconciliation
  superseded during its marker read; old G1 revoke vs G3; R2 rendering of every device (including
  an old-key one) behind 20 history rows with the matching marker retained; no false empty state
  on a failed list; malformed list contracts. Three earlier cases had post-write list mocks that
  omitted the generation the server had just created; under the complete-list contract that
  correctly turns the device off, so those mocks were made consistent with the server (no
  assertion was removed or weakened).
- Against the previous store logic (baseline copy, adapted only to read the new list fields) all
  4 new R1 cases FAILED; with the repair all pass. The R2 web cases passed on that adapted
  baseline because the adapter itself supplies the new list shape; R2's substance is the server
  contract, covered by the db_gated case below (not run against the previous query).
- db_gated (`PushDbTests.h`): two older counted generations (current key and old key) kept while
  23 newer generations are created and revoked; `devices` holds exactly both (states `active` and
  `inactive`), `history` is capped at 20 revoked rows, both are revoked successfully from what the
  list exposes, the count returns to 0, and a logged-out session's generation leaves `devices`.
- Static check: list JSON built from both buckets; the `cap_invariant` failure label exists.

**Checks executed (2026-10-31)**
- `vitest run tests/unit/push.spec.ts`: 36/36; full web suite: 28 files, 383 tests passed;
  `vue-tsc --noEmit -p tsconfig.json` exit 0; `tsc --noEmit -p tsconfig.node.json` exit 0.
- `Claude outputs/p25a-review-repairs-ctest.ps1` (LAN override, usapp_test only): 23 registered,
  0022 checksum equals the local file; fresh test image exit 0 (only the pre-existing
  `NotificationIntentUnitTests.h:42` warning); full CTest 3/3 (unit, db_gated 13.04 s ran,
  static_check). Logs: `%TEMP%\usapp-p25a-repairs-20261031_001210`.

**Remaining limits**
- Cross-tab marker coordination remains best effort (12.2); revocation knowledge is per tab and
  per identity, so another tab learns of a revoke only from its own list or write responses.
- Revocation knowledge lives in memory; after a reload the authoritative complete list is the
  source.
- The R2 query was not executed against the previous SQL for a before/after comparison.

## 13. P2-5b implementation and verification (Claude Code, 2026-11-02)

Scope: durable dispatch with a fake transport only. P2-5c real transport, the push-worker, P2-6,
new event families, backfill, main, deployment and the served dist are untouched.

### 13.1 Status

| Stage | State |
|---|---|
| Implemented | Yes: migration 0023, `push::Dispatcher`, `push.dispatch` job, contract 0.20.0, `gone` handling in the web client. |
| Fake-transport verified | Yes, on usapp_test (13.6). The transport is an asynchronous test double; no HTTP client or push service is called. |
| Test migration applied | `0023_push_dispatch` applied to **usapp_test only** and FROZEN, sha256 `577c17788b804921fa5beac2b9eaa1eb446dead6d801c318e1efdc637e2e4c15`. 0000-0023 on test; main remains 0000-0017. |
| Deployed | **No.** |
| Real-device accepted | **No.** Nothing was sent to any device or provider. |

### 13.2 Schema and 0022 compatibility

Tables (all SELECT/INSERT for `usapp_api`, none for `usapp_push`; append-only, UZ001):
`push_dispatch_control`, `push_deliveries`, `push_attempts`, `push_attempt_observations`,
`push_attempt_expiries`, `push_delivery_outcomes`. Also the index `notifications_created_idx`
(the fan-out window) and the functions `usapp_push_control_state()` and
`usapp_push_provider_attempts()`. Typed FKs carry the recipient on both sides of a delivery
(`notifications (id, couple_id, recipient_id)` and `push_subscriptions (id, user_id)`).

Guards: UZ002 control role and transition; UZ003 delivery eligibility (rules 2-4 of 5.2, same space,
control not disabled, literal 24 h bound); UZ004 claim (enabled, not before cutover, no terminal
outcome, contiguous, previous attempt resolved, budget left, at most 4 live leases under lock
33016); UZ005 observation and expiry time, and no expiry once an actual result exists; UZ006 outcome
facts the database can verify (`accepted` / `gone` / `rejected` need that observation on that attempt,
`exhausted` needs 5 provider attempts, `cutover_skipped` needs a notice before the cutover); UZ007
gone source. `expired` and `inactive` are policy decisions and are only time-checked.

**0022 compatibility (0022 itself is unchanged).** Revocation reason `gone` was added by replacing
three CHECKs and the guard function body. A gone revocation has `revoked_by IS NULL`,
`caused_by IS NULL` and `(gone_notification_id, gone_attempt_no)`, an FK together with
`subscription_id` to the exact observation, which must say `gone`. It therefore has no human actor,
and it can only ever revoke the generation that attempt was for, never a replacement on the same
endpoint. All other reasons keep their 0022 actor rules, now written so a NULL actor fails instead of
passing as unknown. API: `revoked_reason` may be `gone`; `revocation.revoked_by` is null exactly for
`gone` (contract 0.19.0 -> 0.20.0, additive). Web: the validator requires that pairing and the card
says "推送服务报告此订阅已失效". No key-rotation behaviour was added.

### 13.3 Control, eligibility and locks

**Owner operations** (documented SQL; run only on isolated usapp_test fixtures so far, and only a
superuser maintenance session may run them):

```sql
INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('enable',  'owner', 'rollout');
INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('disable', 'owner', 'stop');
INSERT INTO push_dispatch_control (kind, actor, reason_code) VALUES ('resume',  'owner', 'fixed');
SELECT * FROM usapp_push_control_state();
```

`revision` and `recorded_at` are assigned by the guard under lock 33015, so they cannot be
caller-supplied. Valid transitions: enable from disabled; disable from enabled or paused; pause from
enabled; resume from paused. `usapp_api` may only append `pause` with actor `dispatcher` and reason
`worker_auth_failed` or `provider_auth_failed`. There is no startup writer. The default is disabled.

**Capability** (`PUSH_ENABLED`, a valid `PUSH_VAPID_PUBLIC_KEY`, a transport, the marker secret)
never writes control. In this batch production passes a null transport, so an enabled control fails
closed: no dispatch, a `LOG_ERROR`, and `dispatch=fail_closed` in the `scheduler_ticks` detail.
Deviation from 5.1: the structured error is in logs and tick detail, not in `/health`.

**Eligibility** is the 5.2 insert-time rule: `n.created_at >= g.activated_at`, `>= cutover`,
`> now - TTL`, recipient and space match, generation unrevoked with a valid session and current key
id. Fan-out scans that TTL window with an anti-join (no watermark), oldest first, at most 100 new
deliveries per tick; a late commit is found on a later tick. A transaction that began before the
boundary but inserted its notice after it IS eligible; this is by design and tested.

**Lock order.** Tick: scheduler advisory lock (`pg_try_advisory_xact_lock`, job `push.dispatch`) ->
33016 per claim. Recording: no advisory lock, except 33015 immediately before a conditional pause.
Registration and revoke keep 0022's session -> 33013 -> 33014. A gone revocation takes neither 33013
nor 33014 (it only lowers counts). No path holds 33015 or 33016 while waiting on another of these.

### 13.4 A tick and the fake transport

Under the scheduler transaction: read control -> synthetic lease expiries -> `exhausted` -> TTL
`expired` -> `cutover_skipped` -> fan-out -> pre-claim `inactive` -> claim (enabled only). Paused:
everything except the claim. Disabled: no writes except the tick row. Claims are chosen by fewest
provider attempts, oldest notice, generation id; at most 2 per recipient per tick; live leases at most
4 globally; a 60 s lease.

`Result.afterCommit` captures only the dispatcher and plain attempt keys and runs after the confirmed
commit. It runs a pre-send query per claim, then `Transport::send` (asynchronous) or an explicit
internal observation (`inactive_skipped`, `ttl_skipped`, `cutover_skipped`, `control_skipped`,
`terminal_skipped`). No sleep, future wait or synchronous call runs on an event-loop or ORM thread;
the sleeps in the tests are on the test thread.

**Unavoidable race.** A logout, disable or expiry right after the pre-send check can still be sent,
and a lease cannot physically stop a stalled sender: after expiry a next attempt may run while the old
send completes, which is a duplicate. Delivery is bounded at-least-once and can also deliver nothing.
There is no exactly-once or guaranteed delivery, and provider acceptance never means display.

### 13.5 Budget, leases and late results

- Budget 5: attempts with a provider-class observation, or an expiry and no observation. Internal
  observations do not count and are bounded by TTL only. `provider_auth_failed` counts (a provider
  answered); `worker_auth_failed` does not. Both pause, each with its own reason.
- Backoff by attempts used: 1, 5, 30, 120 min, or Retry-After if longer; internal results 1 min.
- A lease expiry without an observation writes `push_attempt_expiries` on a later tick and counts as
  one unknown provider attempt.
- Recording is one transaction: observation `ON CONFLICT DO NOTHING`, then side effects derived from
  the STORED observation, so a different duplicate cannot change anything. Outcome is insert-if-absent;
  gone is insert-if-absent on its own generation; a pause runs only on the first recording.
- A late observation stays recorded after an expiry or a terminal outcome, and the outcome never
  changes, so `exhausted` or `expired` next to an `accepted` observation distinguishes a late fact from
  never sent. A later budget recalculation (a late internal result lowers the count) resurrects nothing.

### 13.6 Verification on usapp_test (2026-11-02)

Scripts: `Claude outputs/p25b-prevalidate.ps1`, `Claude outputs/p25b-verify-usapp-test.ps1`
(`-Stage Apply`, `-Stage Tests`), `Claude outputs/p25b-case-evidence.ps1`.

1. Baseline: 23 registered with matching checksums; frozen 0022 asserted; no dispatch tables.
2. Rollback-only prevalidation, both modes: `groups=9 cases=70`; afterwards 0 registrations, no
   dispatch tables, 0 fixture users.
3. Write order reviewed against the guards; the test image compiled before the first apply.
4. DryRun: 23 skipped, only 0023 pending, sha256 above.
5. Apply + InitRoles: exactly 0023; privilege query 0 rows; 0 control rows.
6. Repeat InitRoles: 24 skipped, 0 applied; 0 rows; all 24 checksums match.
7. Fresh image and full CTest, 4/4 including the new `push_dispatch` target: see the final-run lines
   at the end of this section.
8. `database/tests/push_dispatch_test.sql` in both modes with pass markers and ROLLBACK.
9. Ten scripts, each in both roles with ROLLBACK: push_subscription, task_reaction,
   notification_choice, notification_ledger, message_ledger, anniversary_ledger, task_ledger,
   wallet_ledger, product_ledger, redemption_ledger.
10. Web full suite and both typechecks. No served-dist build.

`push_dispatch` (11 cases, `services/api/tests/PushDispatchDbTest.cc`) uses the real Scheduler, real
Dispatcher and real services: owner-only control, default-off, mismatches and no startup write;
multi-device fan-out, recipient and space isolation, sends only after commit (at send time another
connection already sees the attempt), replay and rollback; insert-time eligibility (with
`xact_start < activated_at` read from `pg_stat_activity`) and a real late commit (the notice row held
uncommitted on the preference lock); initial cutover, pause/resume keeping the cutover, disable/enable
moving it, device re-enable, restart recovery with a new dispatcher; four live leases across ticks,
another instance holding the lock, concurrent ticks, two per recipient, fair order; crash recovery,
five expiries to `exhausted`, late accepted and late internal results; Retry-After, backoff, final
transient exhaustion, `rejected`, duplicates; internal failures with no budget and TTL, TTL before and
after attempts, the 6.4 sequence; gone with provider attribution, late gone for G1 after replacement by
G3 leaving G3 intact, revoke retry showing `gone`; distinct auth pauses and no re-pause on a duplicate;
every pre-send skip. It connects with the application role plus a maintenance connection for owner
operations, checks both report `usapp_test`, prints no connection string (checked: 0 matching lines)
and leaves control disabled.

Final run on the finished tree (`%TEMP%\usapp-p25b-Tests-20261102_151917`): 24 checksums match; repeat
InitRoles 24 skipped, 0 applied; 0 privilege rows; test image exit 0 in 39 s (only the pre-existing
`NotificationIntentUnitTests.h:42` warning); CTest 4/4 in 20.25 s (unit 0.04 s, db_gated 13.46 s,
push_dispatch 6.73 s, static_check 0.01 s, none skipped); push_dispatch_test.sql `groups=9 cases=70` in
both modes; ten scripts in both roles, all EXIT=0 with their markers and ROLLBACK; web 28 files, 386
tests; vue-tsc and node tsc exit 0.

**Maintenance credential for the test.** Only the maintenance role may enable, so
`deploy/docker-compose.test-maint.yml` gives the one-shot `test-runner` a superuser connection to
usapp_test, interpolated from `.env` without printing it. No other service changes; the api still
never holds that password. Without the file the target exits 77 (Skipped), which the verification
script treats as a failure.

**Failures and repairs** (migration unchanged by all of these):
- Docker Desktop was off at first. It was started by the owner, not by Claude Code, because
  `restart: unless-stopped` services could have been restarted.
- `me-view.spec.ts` failed deterministically: its fixture invite expired on 2026-11-01. Not a P2-5b
  effect. The fixture date moved to 2099.
- Prevalidation: three expectations in the new SQL script were wrong (a BEFORE guard fires before the
  CHECK, twice; and TRUNCATE refuses pending deferred events, 55006). Fixtures were completed and
  `SET CONSTRAINTS ALL IMMEDIATE` added, so the append-only trigger is genuinely reached.
- Two test compile errors (an optional result).
- Mutation check: re-pause on duplicate and removal of the pre-send inactive check were caught; removal
  of the per-recipient cap was NOT, because the notice order satisfied it by accident. The case was
  reordered and now catches it. Sources were restored and compared after each mutation.
- **Unexplained, not resolved:** one `db_gated` run aborted (`Subprocess aborted`, 8.64 s) in the clean
  CTest immediately after the first mutated build. Its output was lost to my output filter. It did not
  recur in 23 further db_gated runs, 14 of them under CPU load and two of them exact replays of the
  sequence. By timing alone it was probably before db_gated's P2-5a block, and db_gated does not run the
  dispatcher, but neither fact proves a cause, so it stays an open intermittent risk.

**Fixture effects.** `push_deliveries` references `notifications` and the chain extends to revocations,
so eight existing scripts' TRUNCATE lists gained the chain, listed last without CASCADE, keeping their
expected SQLSTATE. Committed `@m3-fixture.test` rows and control revisions accumulate on usapp_test,
as in earlier milestones (never reset); control ends disabled.

### 13.7 Remaining risks

- No real transport, provider, worker, browser or device has been involved. Status mapping, deadlines
  and the worker's own semaphore are P2-5c.
- Expiry guard race: an observation committing between the tick's scan and its expiry insert raises
  UZ005 and fails that tick; the next tick retries. It loses nothing, but appears as a failed tick.
- A failed pre-send query sends nothing, yet the lease expiry counts as one unknown provider attempt
  (conservative).
- A registration `already_active` racing a gone revocation can fail with UY004 (500); retrying the same
  key is safe.
- The pending-delivery scans have no time window. Fine at this app's scale; unmeasured at larger scale.
- Drogon pipeline commit-error reporting remains as noted in 12.5. 0023 adds no deferred constraint.
- Key rotation is not handled. An old-key generation is `inactive` at fan-out and pre-claim.
- Cross-tab marker coordination remains best effort (12.6).

## 14. P2-5b correctness repairs and DB verification proposal (Claude Code, 2026-11-02)

(Written before isolated verification; Section 15 supersedes the unverified and unapplied statements below.)

**Verification boundary.** Implemented and compiled. The database behaviour of these repairs is
**NOT verified**: migration `0024_push_dispatch_repairs` is a draft applied nowhere, and the database
tests below are written but have never run. Only compile, unit, static and web checks ran (14.6).
Sections 13.1 and 13.6 describe the state before these repairs.

### 14.1 Draft migration

`database/migrations/0024_push_dispatch_repairs.sql`, sha256
`751eb526f8e5502f1e3bb23818202b9d41fb0e12eb2b42da82dbb60b7be8bb6f`, **NOT applied, not even
rollback-only prevalidated**, so it is still editable and its SQL has never been parsed by a server.
0022 and 0023 keep their bytes. It adds `push_attempts.control_revision` (FK to the control log),
adds `lease_lost_skipped` to the internal outcomes, and replaces two guard function bodies.

The code now requires 0024, so `db_gated` and `push_dispatch` fail at their migration gate until it is
applied on a test database. A failure there means "not applied", not a regression.

### 14.2 R1: stale pre-send attempts

`kPreSendSql` now selects the exact attempt `(notification, generation, attempt_no)`, and
`decidePreSend` fences it first, in this order:

| Fact | Decision | Written |
|---|---|---|
| No such attempt | no send | nothing (an observation has nothing to reference); error log |
| Another instance's claim | no send | nothing (it belongs to that instance) |
| Already observed | no send | nothing (the first result stands) |
| Expiry row, newer attempt, or lease ended | no send | internal `lease_lost_skipped` |
| Then, as before: terminal, TTL, cutover, control, inactive | no send | their internal observations |

`lease_lost_skipped` is honest: nothing was sent, no provider fact is invented, and because it is
internal the attempt uses no provider budget, even when a synthetic expiry was written first. The
delivery stays retryable within TTL, unless a later attempt or outcome already decided it.

**Remaining race.** The check and the send are not atomic. A lease can end, or a disable or logout
commit, immediately after the check, and that request is still started. A started request cannot be
cancelled by a lease. Calling `sendClaims` twice for one unresolved claim is not fenced either; the
scheduler's `afterCommit` runs it once.

### 14.3 R2: automatic pauses against newer owner control

**Rule.** The guard stores the latest control revision on each attempt at claim time, never from the
caller. A result may pause only if `attempt.control_revision` equals the latest revision and the state
is `enabled`, evaluated while holding control lock 33015, the same lock every transition takes.

**Why it is reliable.** The revision is a contiguous log position, so no clock is compared. While the
state is `enabled`, the latest revision can only be the owner's `enable` or `resume`, because a
dispatcher pause leaves the state `paused`. So any owner action after the claim makes that attempt's
revision older and its failure stale, and a claim made after that action matches and still pauses. The
guard reads the revision before the state: a transition in between can only make a claim look older.
Attempts from before 0024 have a NULL revision and never pause.

The observation is written before this check and is always kept. `provider_auth_failed` still counts
toward the budget when stale, because a provider did answer. Duplicate recordings still do nothing.
The rule is enforced by the service statement; the database guard does not independently require
provenance on an application `pause` row (limitation).

### 14.4 R3: already_active versus gone

The defect: a registration read G as the unrevoked occupant, a gone revocation committed (it took no
endpoint lock), and the registration's command then failed the guard with UY004, an internal error.

The fix is in the guard, so no writer can bypass it: a `gone` revocation takes endpoint lock 33014
after loading its generation. Both orders are now defined:
- Registration first: gone waits until the registration commits. The result is `already_active`, then
  gone revokes G. A replay of that key returns `already_active` with G's current state `revoked` and no
  marker.
- Gone first: the registration waits on 33014, reads G as revoked, and creates a new generation.

Preserved: gone still targets only its own generation (FK); no replay restores a generation; cap and
endpoint guards are unchanged and not weakened. **Lock order:** registration is session -> 33013 ->
33014; a gone recording holds only 33014 and takes no 33013 or 33015, so no cycle exists. This updates
6.8 and 13.3, which said gone takes no endpoint lock. A session expiring in the instant between a
registration's read and its command can still give UY004; that is unchanged and much narrower.

### 14.5 Proposal for safe database verification (needs owner approval; nothing built or started)

**Boundary.** A superuser on the shared instance can read and change every database in it, including
main, and can reach the server's filesystem and programs. Naming `usapp_test` in the connection string
limits nothing. `deploy/docker-compose.test-maint.yml` did exactly that in the previous turn's runs; it
is now marked not approved and the three scripts that use it refuse to run.

**Recommended: a separate, disposable PostgreSQL instance.**

| Property | How | Boundary it gives |
|---|---|---|
| Separate process and data | own `postgres:17` container, data on `tmpfs` | no main data exists in it; a superuser there can only reach this empty instance |
| Separate network | own Compose project `usapp-isolated`, network `internal: true`, no published ports | cannot reach the `usapp_net` network or main; no outbound network |
| No production credentials | never passes `--env-file .\.env`; a throwaway random password per run, generated by the script, passed by process environment, never written or printed | nothing in it can log in elsewhere, and nothing valuable is exposed |
| Same schema path | roles via the existing init script, then 0000-0024 applied in order, then init-roles | also proves a clean apply from empty, which the shared test database cannot |
| Same test image | `test-runner` built as today, attached only to the isolated network; `USAPP_TEST_DSN` and `USAPP_TEST_MAINT_CONNINFO` point at the isolated host | the superuser credential is real, but only for that disposable instance |

Run order, fail-fast: start the instance -> rollback-only prevalidation of 0024 on top of 0000-0023 -> apply
0024 -> privilege query (0 rows), repeat -> `push_dispatch_test.sql` in both modes -> full CTest -> affected SQL
scripts -> keep all logs -> stop. Teardown removes only project `usapp-isolated`; `tmpfs` leaves nothing.

Needs from the owner: approval of a new file `deploy/docker-compose.test-isolated.yml`, its verification
script, and the `postgres:17` image already in use. Whether the shared `usapp_test` also receives 0024 is
then a separate decision.

Limit: it proves behaviour on a fresh database, not on the fixture history of the shared `usapp_test`.

**Alternative, not recommended: a least-privileged role on the shared instance.** A dedicated `NOSUPERUSER`
login with `CONNECT` on `usapp_test` only and `pg_hba` limited to the test network. It cannot pass the 0023
control guard, which requires superuser, so it would need a new migration trusting that role for owner
operations, and that rule would then also exist on main. It would also change existing access
configuration. It weakens a production boundary to suit tests.

### 14.6 Tests: executed versus written only

**Executed** (`Claude outputs/p25b-repairs-nodb.ps1`, logs in
`%TEMP%\usapp-p25b-repairs-*`, complete output written before any filtering): frozen 0022 and 0023 hashes;
test image builds every target, exit 0; `unit` and `static_check` with `--no-deps`, 2/2; web 28 files, 386
tests; vue-tsc and node tsc exit 0.

**Written, NOT executed** (blocked by the 14.5 gate):
- `PushDispatchDbTest.cc`, R1: claim k, expire, claim k+1, run old k: no request, `lease_lost_skipped`, no
  budget, k+1 sends once; lease ended with no successor; already observed; another instance's claim;
  nonexistent attempt.
- R2, for both auth reasons and both owner sequences (pause/resume, disable/enable): stale first observation
  stored without a pause; duplicate harmless; a current failure pauses; its duplicate after resume does not.
- R3: both interleavings, made deterministic with `LOCK TABLE ... IN SHARE MODE` on a maintenance connection
  holding each side at a known point. Against the old guard the first interleaving should reproduce the
  UY004 error; that has not been run.
- `push_dispatch_test.sql`: control revision assigned and not caller-chosen; `lease_lost_skipped` class and
  budget.
- Lease lengths in two older cases were raised (0.15 s to 0.5 s, 0.1 s to 0.4 s), because the fenced
  pre-send would otherwise make them timing-sensitive. These also have not run.

### 14.7 The db_gated abort remains unresolved

Retained evidence, inspected read-only; no stress loop was run:
- The test's own output is gone: the run was filtered and only lines with `Test #` or `tests passed` were
  kept, and the container was removed. No retained log contains an abort marker.
- PostgreSQL's log still has the server side. On 2026-11-02, `07:06:06.156 UTC` pid 1343:
  `USAPP_TASK_SETTLEMENT_ENTRIES` on `commit`. `07:06:06.447` pid 1364: `could not receive data from client:
  Connection reset by peer`, the only such line in the 30-minute window 06:55-07:25 UTC. `07:06:06.484`:
  the next target, `push_dispatch`, begins.
- In a passing run the next server event after that same error is `division by zero`, 0.19 s later. The
  aborted run has no such event.

This places the end of the process after that settlement error and before the next logged failure case. It
does not show what aborted or why, and later passes do not show it is fixed. **No root cause is known.**

For next time: `installCrashDiagnostics()` makes stdout line-buffered, so `[ RUN ]` lines survive, and prints
the exception from `std::terminate` and a backtrace on SIGABRT or SIGSEGV, in `db_gated` and `push_dispatch`.
Verification scripts must write complete stdout, stderr and the exit code to a log before filtering, as
`p25b-repairs-nodb.ps1` does.

### 14.8 Remaining limits

- Everything in 14.2-14.4 is unverified against a real database until 14.5 is decided.
- 13.7's `already_active` risk is addressed by 14.4 once applied and tested; its other items stand.
- The previous turn's `test-maint` runs used a superuser connection on the shared instance before approval;
  their results remain as recorded in 13.6, and they are not repeated.
- In this repair turn, the shared instance was touched in two ways. Its container log was read for 14.7. And
  one stray `SELECT 1` was run by mistake through `psql -U postgres -d usapp_test`, appended to a file-hygiene
  command, output discarded. It read and changed nothing, but it used the superuser role on the shared
  instance and should not have been run under this gate. No migration, fixture, control row or main database
  was touched.

## 15. P2-5b isolated repair verification (Claude Code, 2026-11-02)

Owner-approved. This section supersedes the "not verified" statements of Section 14 for the disposable
instances below, and only for them.

### 15.1 State, stated exactly

| Item | State |
|---|---|
| Isolated verification | **Passed** in a standalone disposable PostgreSQL 17 instance; project `usapp-isolated-20261102162854-382cfa`, database `usapp_test` (final run). |
| `0024_push_dispatch_repairs.sql` | **FROZEN since its first application**, sha256 `751eb526f8e5502f1e3bb23818202b9d41fb0e12eb2b42da82dbb60b7be8bb6f`, unchanged from the draft. Applied only in disposable instances, whose data is gone. |
| Shared `usapp_test` | **Not accessed. Still 0000-0023.** Its `db_gated` now fails its migration gate until 0024 is applied there, which is a separate decision. |
| Main | **Not accessed. Still 0000-0017.** |
| Deployment, real delivery, device acceptance | **None.** Fake transport only. |

### 15.2 Isolation

`deploy/docker-compose.test-isolated.yml` is standalone and is run only by
`Claude outputs/p25b-isolated-verify.ps1`.

Actual boundary, read from the created containers with `docker inspect` (no probe of main):
- Network: exactly one, `<project>_isolated`, `Internal=true`, bridge. `NetworkMode` is that network for both
  containers.
- Ports: `PortBindings` empty and publish-all false for both; `Privileged` false.
- Storage: `postgres` data on `tmpfs` (`rw,size=1g`). The only mount is
  `deploy/docker-entrypoint-initdb.d` -> `/docker-entrypoint-initdb.d`, read-only. `test-runner` has no mounts.
  No named or anonymous volume and no Docker socket. The script fails on anything else.
- Credentials: three random 32-byte values generated per run, passed by process environment, restored in
  `finally`. Only their variable NAMES were recorded. `generated credentials found in retained logs: 0`, by
  scanning every retained file for the values.
- `.env`: `COMPOSE_DISABLE_ENV_FILE=1`; an explicit, empty, test-only `--env-file`; conflicting `COMPOSE_*`, `PG*`
  and `POSTGRES_*` variables cleared for the process and restored afterwards. `apply.ps1` is unmodified and
  received `-EnvFile` explicitly, because it otherwise falls back to the repository `.env`. Container variable names
  show no `POSTGRES_SUPERUSER`, `SESSION_SECRET` or SMTP values. The image build uses the repository's
  `.dockerignore`, which excludes `.env`.
- Pre-start inspection: `config --no-interpolate` (credentials stay placeholders) checked services, network, ports,
  mounts, project name and images before anything started.
- Roles: `postgres` super, `usapp_api` and `usapp_push` not. The application connection is `usapp_api`, the
  maintenance connection is `postgres`, and `push_dispatch` checks both report `usapp_test`.

Identity: PostgreSQL 17.11 (Debian 17.11-1.pgdg13+2), image `postgres:17`
`sha256:d74eeac9a635390a49bc21bd49fccd973de707e2a53a76ac49b552b8712ec46f`.

### 15.3 Sequence and results

Three invocations, each a new instance built from empty: `first` and `final` (identical product code; only the
script's proof option was added between them), and one mutation proof (15.5).

Final run, fail-fast:
- Baseline: 0000-0023 from a byte-verified staged copy through unmodified `apply.ps1` with InitRoles: 24 applied,
  24 checksums match. 0024 could not be reached.
- Pre-0024 data: `database/tests/push_dispatch_pre0024_fixture.sql` committed 3 real attempts, one observed.
- Prevalidation, rollback-only: `groups=9 cases=74` in both modes. Afterwards 0 registrations, no
  `control_revision`, 0 fixture users, guard bodies and the class rule unchanged by checksum.
- Guard equivalence against frozen 0023, by line diff. Attempt guard: only added lines (declare, read latest
  revision, reject a differing caller value, assign it). Revocation guard: added the gone endpoint lock; `NOT FOUND`
  became `g.id IS NULL`, needed because `PERFORM` resets `FOUND`. Nothing else differs.
- Apply: exactly 0024 applied. Privilege query 0 rows; 25 checksums match. Repeat InitRoles: 25 skipped, 0 applied;
  0 rows; 25 match. 3 pre-0024 attempts kept with NULL `control_revision`; the column is `bigint`, nullable.
- CTest 4/4 in 16.42 s, verbose, none skipped: unit 0.04 s (1727 assertions), db_gated 8.45 s (2509),
  push_dispatch 7.92 s (373), static_check 0.01 s (446). Test image exit 0 in 40 s.
- SQL, each exit 0 with marker and ROLLBACK: push_dispatch (`groups=9 cases=74`) both modes; in both roles,
  push_subscription 22 cases, task_reaction 13, notification_choice 17, notification_ledger 14, message_ledger 5
  groups, anniversary_ledger 34, task_ledger 35 groups, wallet_ledger 28 groups, product_ledger 8 groups,
  redemption_ledger 6 groups.
- Control ended disabled. Server log: 0 deadlocks, 0 FATAL, 0 PANIC.
- Web checks were not repeated: no web file changed since their last pass (28 files, 386 tests, both typechecks).

No failure occurred, so no repair of 0024, the service or the tests was needed in this step.

### 15.4 Repair acceptance

- **R1.** Claim k, expiry, claim k+1, then the old k's after-commit work: no request, `lease_lost_skipped`, no budget,
  k+1 sends exactly once. Lease ended with no successor, already observed, another instance's claim and a nonexistent
  attempt start no request. No provider result is invented. The check-to-send race stays (14.2).
- **R2.** The guard assigns provenance. For both auth failures after pause/resume and after disable/enable: the stale
  first observation is stored, control is unchanged and no revision is added. A current failure pauses. Duplicates do
  nothing. Real pre-0024 attempts with NULL provenance are stored and do not pause.
- **R3.** Both interleavings, forced with table locks. Registration first: `already_active`, then gone; a replay keeps
  the command facts and shows current state `revoked`, no marker. Gone first: a new generation, gone stays on its own
  generation, exactly one active occupant. In the final run the server log shows 0 `USAPP_PUSH_COMMAND_REVOKED`.
- **Lock order, rechecked.** Registration: session -> member -> 33013 -> 33014. User revoke: session -> member -> 33013.
  Gone recording: 33014 only. Pause recording: 33015 only. Owner transition: 33015 only. Tick: scheduler lock -> 33016.
  No path takes 33013 or 33014 after 33015 or 33016, so no cycle exists among them. 0 deadlocks in any server log. This
  is by reading plus those runs, not a proof over every interleaving.

### 15.5 Mutation proof

Project `usapp-isolated-20261102162604-1d0f94`. The R1 fences and R2 provenance were removed from the service source for
that one build, and the 0023 gone guard was restored inside that throwaway database only. The migration file was not
altered. Result: all of `repair R1`, `repair R2` (both cases) and `repair R3` FAILED, as did the matching unit and static
checks, while `db_gated` passed. With the old guard, the server log recorded `USAPP_PUSH_COMMAND_REVOKED` once, which
reproduces the original UY004 defect. The source was restored and its hash compared
(`53e561ec...d7c7`). The final run came after this, so the last image built is clean.

### 15.6 The earlier abort remains unresolved

Complete output, exit code and container state are now saved before filtering, with `ctest -V`, and crash diagnostics
are on. No abort, `[CRASH]` or `Exception:` occurred in the two clean or one proof `db_gated` runs here. That says
nothing about the earlier abort, which happened on a different instance: **its cause is unknown and it is not fixed**
(14.7). No stress loop was run.

### 15.7 Created resources

Stopped by ID at the end of each run; no down, prune or volume deletion. Retained stopped, with their networks, because
removing them was not authorized. Their tmpfs data is gone, and their configuration holds only dead throwaway
credentials. Logs: `%TEMP%\<project>\`.

| Project | Result | Containers (test-runner, postgres) | Network |
|---|---|---|---|
| `usapp-isolated-20261102162236-31fd95` | passed | `15e107afd350`, `25936d005d2d` | `48ca606e0764` |
| `usapp-isolated-20261102162604-1d0f94` | mutations caught | `f7cd94ed34ed`, `eb5641ea745d` | `26eaeed5f786` |
| `usapp-isolated-20261102162854-382cfa` | passed (final) | `998b66484811`, `0e927e38bf99` | `ac4eb3d5deed` |

Also: image tag `usapp-isolated-test-runner:p25b`, plus an empty, value-free env file in each log directory.

### 15.8 Remaining limits

- A fresh instance does not exercise the shared `usapp_test` fixture history. Applying 0024 there or on main is separate.
- Check-to-send race (14.2); the DB guard does not require provenance on an application pause row (14.3).
- `pg_stat_activity` waits and table locks make R3 deterministic for those two orders, not for every timing.
- The other items of 13.7 stand: no real transport, provider, worker or device; a failed pre-send query later counts as
  an unknown attempt; the expiry-observation race fails one tick; scans are unmeasured at scale; key rotation is not
  handled; cross-tab marker coordination is best effort.

## 16. P2-5c implementation and isolated verification (Claude Code, 2026-11-02)

Assigned after Codex accepted P2-5b within isolated automated verification. This section is the single detailed
record. It supersedes Section 2's worker text and the C1-C7 findings, which were resolved here, and Section 8.5's
owner-generated lockfile: the assignment authorized adding pinned `web-push` 3.6.7 and generating the lockfile.

### 16.1 State, stated exactly

| Stage | State |
|---|---|
| Transport implemented | Yes: worker, C++ `WorkerTransport`, service worker handlers, browser subscription flow |
| Stub-tested | Yes: worker against local TLS peers and stub resolvers; C++ against a scripted loopback socket; web with mocks |
| Isolated integration-tested | Yes: scheduler to `WorkerTransport` to stub-wired worker and to the production entry, on an internal network |
| Real IndexedDB, two same-origin frames | Yes, in the built-in browser pane, page contexts only |
| Real service worker | **No.** The browser pane refused registration on `127.0.0.1` and `localhost`, cause not determined. Built `sw.js` was only loaded with a stub scope |
| Real provider tested | **No** |
| Device accepted | **No** |
| Deployed | **No.** No main or shared `usapp_test` access, no served-dist build, no API or Caddy restart |

Real subscription and dispatch stay off by default. Schema: no change; 0000-0024 are unedited.

### 16.2 Responsibility boundary and default-off gates

C++ keeps eligibility, claims, leases, budget, retries, pauses, revocation and every durable fact. The worker
(`services/push-worker`) is stateless: no database access or credentials, no queue, no assets, no business decision.
It takes one decided delivery, encrypts it and reports what happened.

| Gate | Default | Effect |
|---|---|---|
| `PUSH_WORKER_ENABLED` | false | Worker logs one line, exits 0, listens on nothing |
| `PUSH_WORKER_PRIVATE_NETWORK` | false | Non-loopback bind refused (exit 78). Compose sets it true but publishes no port |
| `PUSH_ENABLED` + `PUSH_WORKER_URL` + `PUSH_WORKER_TOKEN` | off | No `WorkerTransport`; an enabled control row fails closed |
| `push_dispatch_control` | disabled | Dispatch needs the owner's row. Startup writes nothing and no control writer was added |
| `PUSH_SUBSCRIBE_ENABLED` + valid key + worker configured | off | `subscribe_available` false. This checks configuration, not worker liveness |
| ACTIVE service worker answers `push:v1` | browser check | A waiting worker does not count; no environment flag alone opens opt-in |

### 16.3 Endpoint, crypto, deadline, TTL and result guarantees

**Endpoint and connection (`src/endpoint.ts`, `src/transport.ts`).** Raw-text checks before URL parsing, then HTTPS,
no port or userinfo, no IP host, exact or label-boundary allowlist. A controlled `lookup` returns public addresses only,
including mapped and compatible IPv6; `secureConnect` checks the actual peer, and nothing is written until it passes.
`rejectUnauthorized: true` with SNI. A new `https.Agent` per request is destroyed on settlement, and a test counts one TLS
connection per request rather than inferring it from `keepAlive:false`. No redirect is followed. The result comes from
the status line; the body is never read and the response is destroyed. Headers are capped at 8 KB (this replaces the 4 KB
response cap of Section 8.2). `NODE_USE_ENV_PROXY` and `--use-env-proxy` stop startup. Test wiring reaches these options only
through `createProviderTransport`'s argument; `src/index.ts` passes none, and no flag bypasses them.

**Crypto.** `web-push` is used only through `generateRequestDetails`; this repository implements no cryptography.
C++ `validPublicKey` now uses OpenSSL (`EC_POINT_oct2point`, `EC_POINT_is_on_curve`), and the worker `ECDH.convertKey`.
Fixtures use published points (RFC 8291 Section 5, RFC 8292), not a weakened check. A test VAPID pair is generated
per run and discarded.

**Deadlines.** Worker: one absolute 10 s `AbortController` from accepting the request. C++: one absolute 15 s
deadline on its own event loop, at most one result per call; no event-loop or ORM thread blocks. Network cancellation
is in 16.10.

**TTL.** `kPreSendSql` reads `remaining_ttl` and `expires_at_ms` from the database clock for the delivery's fixed lifetime.
Under one second left is an existing `ttl_skipped`. The worker sends `min(ttl_seconds, remaining)` and answers
`ttl_skipped` without sending once the absolute expiry passed.

**Result classification.**

| Situation | Outcome | Budget |
|---|---|---|
| Provider 2xx | `accepted` (a provider accepted; never device display) | spent |
| 404, 410 | `gone` | spent |
| 401, 403 | `provider_auth_failed`, automatic pause | spent |
| 408, 425, 429, 5xx | `transient`, with Retry-After bounded to a day (the only outcome carrying a delay) | spent |
| 3xx, other 4xx | `rejected` | spent |
| Worker lost the provider after writing | `unknown` | spent |
| Nothing written: refused address, lookup, TLS failure | `transport_unavailable` | not spent |
| Worker 401 / 503 | `worker_auth_failed` (automatic pause) / `busy` | not spent |
| Worker 400, 408, 413 | `transport_unavailable` | not spent |
| Cannot connect to the worker (Drogon `BadServerAddress`) | `transport_unavailable` | not spent |
| 15 s deadline with no request bytes written (16.10) | `transport_unavailable` | not spent |
| Lost response, 15 s deadline after writing, worker 5xx or other status, malformed, wrong job, contradictory fields | `unknown` | spent |

A 200 counts only if its `job` equals the exact attempt, its keys are exact, it is at most 1 KB, and its outcome, status and
Retry-After form a combination the worker can produce (16.10). P2-5b's stored-observation, budget, late-result and stale-pause rules are unchanged, and so are the
exact-attempt pre-send fence and generation-specific gone revocation.

### 16.4 Service worker, subscription and marker

**Service worker (`src/pwa/push-sw.ts`, wired in `sw.ts`).** A push always shows a notification. Strict bounded parsing;
fixed labels from one table shared with the inbox (`src/api/notification-labels.ts`); no message, reply, title or body text.
A missing or mismatched marker shows the generic text with tag `usapp-generic`. The tag dedupes best effort, never exactly
once. A click rechecks the stored marker, follows only an allowlisted same-origin path, otherwise opens `/notifications`,
and navigates a same-origin window so that the page establishes its own session. `pushsubscriptionchange` clears the marker and
does nothing else. No handler makes a network request: no business write and no automatic mark-read.

**Subscription (`src/pwa/push-env.ts`, `src/stores/push.ts`).** Explicit gesture only; nothing is awaited before
`requestPermission`. Existing retries with the captured key and body, and the identity and read sequencing, are unchanged.
A confirmed revoke of this device's generation unsubscribes this browser; revoking another device never does. Marked
notifications are closed at an identity boundary, best effort. Logout does not unsubscribe the browser: the marker is
cleared and the generation is inactive on the server.

**Marker (`src/pwa/push-marker.ts`).** The earlier separate read and delete could erase a newer tab's marker, and a delayed write
could restore a cleared one. Now each operation is one readwrite IndexedDB transaction: `clearAll` increments a persisted fence and
deletes; `compareDelete` deletes only that generation and remembers it (last 16); `putIfFence` refuses a stale fence, a remembered
generation, or an older activation. BroadcastChannel was removed. Limits:
- A clear in the milliseconds between `primeMarkerFence` starting and its read running is seen as the new fence. The server
  session remains the authority.
- `activatedAt` compares at millisecond precision.
- The service worker context itself was not exercised against IndexedDB.

### 16.5 Networking

Production Compose was changed and validated statically only (`config --no-interpolate`, env file disabled); the existing
stack was not run and no interpolated configuration was printed. `push-worker` lost `POSTGRES_*`, its host port and `depends_on`
and gained the private flag and token; restart is `on-failure`, so a disabled worker's exit 0 does not loop. `api` gained
push variables defaulting to off. API, Caddy, admin and database networking are otherwise unchanged. The runtime image holds
`dist/src`, production dependencies, notices and the license only. `web-push` declares MPL-2.0; unmodified; license copy kept
(`services/push-worker/THIRD_PARTY_NOTICES.md`). No legal conclusion is drawn.

### 16.6 Verification results

Final full pass: project `usapp-p25c-20261102182242-a70c4e`, `Claude outputs/p25c-isolated-verify.ps1`, internal network, tmpfs database.

| Check | Result |
|---|---|
| Worker typecheck, build (finished tree) | exit 0 |
| Worker unit tests, Windows and Linux image | 62/62, about 1 s |
| Migrations | 0000-0023, pre-0024 fixture, frozen 0024 (hash checked), repeat InitRoles applied 0, 0 privilege deviations |
| Production image | default off exit 0; refuses proxy, short token, public bind, off-curve VAPID with exit 78; no `dist/tests`; `/health` 200, unauthenticated 401 |
| CTest, four targets, limits unchanged | exit 0 in 22.64 s: unit 1828 assertions 3.45 s; db_gated 2509, 9.12 s; push_dispatch 383, 10.06 s; static_check 469, 0.01 s |
| Worker integration (scheduler, real `WorkerTransport`), separate from CTest | exit 0, 4 cases, 11 s. Stub: accepted:201, gone:410, transient:429, rejected:302, unknown x2, provider_auth_failed:403. Production entry: transport_unavailable on a non-public address, no lookup left the container |
| Worker logs | no endpoint, key, token or payload |
| SQL scripts | dispatch in both modes; 10 ledger scripts as both roles; all ROLLBACK; final control disabled |
| Web, whole suite | 30 files, 442 tests, exit 0, 32.36 s; both typechecks exit 0 |
| Web push specs (finished tree) | 3 files, 95 tests, exit 0 |
| Real IndexedDB, two frames | atomic compare-delete lost a newer marker 0 of 150; the same race with the old read-then-delete lost it 150 of 150; delayed write restored 0 of 30; marker after a simultaneous clear 0 of 150; revoked rewrite and older overwrite refused; fence persisted across reload |
| Temporary `vite build` to the scratchpad | exit 0; `apps/web/dist` hashes unchanged |
| Built `sw.js` with a stub scope | handlers present; `push:v1` reply; generic notification; click to `/notifications`; 0 fetches |
| Generated credentials in retained logs | 0 |

The stub-scope check proves wiring in the bundle only. It does not verify registration, activation, updates, background
delivery or notification clicks.

**Mutation proof.** Worker 17 and web 8 (`Claude outputs/p25c-mutation-proof.py`, targeted specs in the second part): all
caught, every file restored and compared. C++ (`-ProveMutations`, `unit` and `push_dispatch` only, image tag `p25c-mutated`): job
correlation, failure classification, curve block, remaining TTL and the one-second skip all caught; sources restored and compared.

**Restoration.** All 287 files under the API, worker and web sources and tests, migrations, production Compose, the Dockerfile and
the lockfile compare equal to the `/workspace` of the verified `usapp-isolated-test-runner:p25c` image, and no source file is new. After
that pass only documentation, the worker README, the proof and verification scripts, and the runner image tag in
`deploy/docker-compose.test-p25c.yml` changed.

**The earlier `db_gated` abort** did not appear in these runs. It remains unresolved.

### 16.7 Failures and repairs

| Found by | Problem | Repair |
|---|---|---|
| Worker test | `Date.parse('-5')` made a bad Retry-After a date | Source: date form required. A real bug |
| Worker test | Agent sockets checked before their close event | Test waits |
| Isolated run 1 | PowerShell 5.1 stripped quotes from the probe's expected JSON | Script parses the JSON |
| Isolated run 2 | New C++ test read a string from a destroyed temporary | Test helper returns a copy |
| Mutation run | Semaphore mutation hung `node --test`, blocking the harness on its pipe; the hung tree was killed and the file restored | Harness writes to a file, with a hard limit and `--test-force-exit` |
| Mutation run | One target text did not match compiled output | Target changed |
| C++ proof 1 | Removing only `EC_POINT_is_on_curve` survived, because `EC_POINT_oct2point` also refuses off-curve points: an equivalent mutant, not a test gap | Mutation removes the whole block, which is caught. The explicit call remains as a redundant statement of intent |

### 16.8 Remaining limits and prerequisites

- Real provider behaviour, status codes, and TLS and DNS are unobserved; every provider in the tests is a stub.
- Real service worker lifecycle, background delivery and notification clicks are unverified, as is the reason the pane refuses registration.
- Real permission prompt and `PushManager.subscribe`, Android Chrome, iOS Home Screen: unverified.
- The worker link is plain HTTP with a bearer token on the private network, and Compose DNS from `api` to `push-worker` under the production file is unrun.
- Drogon's client buffers a worker response before the 1 KB check; only the deadline bounds that. The worker is authenticated and private.
- `expires_at_ms` compares the database clock with the worker clock, so skew shifts it.
- A generation stored earlier with an off-curve key gets `transport_unavailable` from the worker until its TTL ends. No such rows are expected on main (0000-0017).
- `tests/support.ts` embeds a test-only TLS key, trusted only through an explicit `ca` in tests and absent from the runtime image; secret scanners may flag it.
- The other items of 15.8 stand, except that cross-tab marker coordination is now as described in 16.4. Key rotation stays unhandled.

Limits of the review repairs are in 16.10.

Before real use, each needing its own authorization: production VAPID keys and worker token; applying 0018-0024 to main; API, worker and web
release; controlled real-provider test; real-device acceptance; the owner's enable row.

### 16.9 Resources

Created, stopped and retained, nothing removed or pruned: 37 exited containers and 5 internal networks of projects
`usapp-p25c-20261102181718-d16346`, `-181954-c23a92`, `-182242-a70c4e`, `-192427-031d90`, `-192706-3ef25c`. Images:
`usapp-isolated-test-runner:p25c` (verified), `usapp-isolated-test-runner:p25c-mutated` (proof only, never use),
`usapp-isolated-worker-stub:p25c`, `usapp-isolated-worker-prod:p25c`. Logs: `%TEMP%\<project>\`. Image builds used the network for public base
images and packages, never a push provider. Worker `node_modules` and `dist` stay in the working tree, ignored by the image context.

### 16.10 Review repairs: response matrix and network deadline (Claude Code, 2026-11-02)

Two findings from Codex's review. No schema change; real push stays off.

**Finding 1: response semantics.** `classifyWorkerResponse` checked only accepted against 2xx, so `gone` + 201 would have revoked a
generation and `provider_auth_failed` + 410 would have paused dispatch. `consistentWorkerResult` now enforces the whole matrix, and
anything else becomes a plain `unknown` with no status or delay carried forward:

| Outcome | Status | Retry-After |
|---|---|---|
| `accepted` | 200-299 | none |
| `gone` | 404, 410 | none |
| `provider_auth_failed` | 401, 403 | none |
| `transient` | 408, 425, 429, 500-599 | none, or 0-86400 |
| `rejected` | 300-499 except 401, 403, 404, 408, 410, 425, 429 | none |
| `unknown`, `transport_unavailable`, `ttl_skipped` | none | none |

Numbers must be written as integers (`410.0` and `true` are refused). Internal outcomes the worker never reports in a 200 are refused.
So that real provider responses fit, the worker now forwards Retry-After only on `transient` and no status on `unknown`; before this, a
410 with Retry-After would have been contradictory. Tests: a table of 71 rows, positive and negative; a worker test that a delay is
dropped on 201, 410, 403 and 400; and a real-dispatcher case on the isolated database where `gone` + 201 and `provider_auth_failed` + 410
store `unknown`, with no final state, no revocation, and control still enabled.

**Finding 2: network deadline.** Confirmed as a real defect. In Drogon v1.9.11 `lib/src/HttpClientImpl.cc`, each `requestsBuffer_` entry
captures `thisPtr = shared_from_this()`, and the connection callback sends the buffer on connect; `HttpClient` has no cancel call. So
releasing the transport's reference did not cancel anything. A deterministic test holds connection establishment with a full accept
queue, past the deadline, then releases it:

| Against | Reported outcome | Request bytes received after release |
|---|---|---|
| Plain Drogon client, reference dropped (kept as a characterization test) | n/a | more than 0 |
| Unfixed `WorkerTransport` | `unknown` | **700**, after the result had been reported |
| Fixed `WorkerTransport` | `transport_unavailable` in under 1 s | **0** in a 5 s window |

The unfixed transport also left an already-written request's connection open past the deadline (the stub saw no end of stream in 3 s).

Fix in `PushWorkerTransport.cc`:
- The deadline is now Drogon's per-request timeout, whose timer erases a request still in `requestsBuffer_` (`HttpClientImpl.cc`, the timeout
  overload of `sendRequestInLoop`). With no entry left, nothing keeps the client alive and nothing can be sent on connect.
- `bytesSent()` separates the two cases, because `sendReq` is the only place that writes and counts: 0 is **cancelled before sending**,
  `transport_unavailable`, no budget; above 0 is an **already-started request**, `unknown`, budget spent.
- In the started case the socket captured through `setSockOptCallback` is shut down, so the connection ends. The worker sees the end; this does not recall
  what it read, which is why the outcome stays unknown.

**Verification.** Unfixed logs were saved before the fix, and complete logs before any filter.

| Check | Result |
|---|---|
| New tests against the unfixed transport | exit 1, 3 assertions failed: outcome `unknown`, 700 bytes, no end of stream |
| Targeted: worker `transport.test.js`; C++ unit | 18/18; 2053 assertions, 0 failed |
| Finished tree, project `usapp-p25c-20261102195310-489053`, `-SkipSql` | PASSED |
| Worker unit tests in Linux image | 63/63 |
| CTest, four targets, limits unchanged | exit 0 in 29.73 s: unit 2053 assertions 11.19 s; db_gated 2509, 8.44 s; push_dispatch 391, 10.09 s; static_check 469, 0.01 s |
| Worker integration | 4 cases, exit 0, 11 s; same outcomes as 16.6, including transport_unavailable from the production entry |
| Worker logs; generated credentials in logs; final control | clean; 0; disabled |

Not rerun, as out of scope for this repair: web suite, SQL scripts, the mutation campaign. The matrix rows were not run against the old
classifier; the 0-byte and started-request tests were (see above).

**Remaining limits.**
- Slow DNS: the resolver callback also holds the client (`HttpClientImpl.cc`). From reading the source, after the deadline the queue is empty, so a late resolve can
  connect but sends nothing. No deterministic test covers this path; the tests use an IP address, while production uses a name.
- The pinned Trantor submodule source was not read, as the lookup of its commit was rate-limited. That destroying a connecting client stops it was observed only as
  0 bytes in the 5 s window, on Linux loopback.
- `bytesSent` above 0 means handed to a connection, not received by the worker: counted as `unknown`, conservatively.
- After the C++ deadline on a started request, the worker can still reach a provider until its own 10 s deadline: it does not abort when its caller disconnects.
- A worker that hangs every connection repeats `transport_unavailable` with no budget spent, bounded by the TTL and the 60 s internal backoff, with no automatic pause.
- A real provider or device is still unexercised; 16.8 stands.

**Resources.** Totals across P2-5c, all stopped and retained, nothing removed: 49 exited containers and 6 internal networks. Added images:
`usapp-isolated-test-runner:p25c-review-unfixed` (defect evidence, never use) and `:p25c-review-fixed`. `usapp-isolated-test-runner:p25c` and the worker
images hold the tree of that pass, one line before the correction below. Logs: `%TEMP%\usapp-p25c-20261102195310-489053\`.

**Correction: `retry_after_s` (Codex review, 2026-11-02).** The repair above required integer form for `http_status` and `attempt_no` but left `retry_after_s` on
`isInt()`, which accepts `120.0` and so let a floating-point delay through. It now uses `isWholeNumber()`. Integers in 0-86400 stay valid; an invalid delay
becomes plain `unknown` with no status or delay. New targeted tests: transient with `120` valid; `120.0` and `true` unknown, with neither field carried forward.

| Check | Result |
|---|---|
| C++ unit target only, finished tree, image `usapp-isolated-test-runner:p25c-retry-after`, `--network none` | exit 0, 12 s, 2062 assertions (9 more), 0 failed |
| Test image build, which also compiles the other test targets and the API | exit 0 |

Complete output and exit code were saved before filtering, in this session's scratchpad (`retry-after-unit.log`, `retry-after-unit.log.build`). As scoped, these were not repeated for this one-line
change: isolated verification, CTest's other targets, worker integration, SQL, web and mutation runs. Their latest results are those in the tables above, on the tree one line earlier.
The new tests were not run against the uncorrected line.

**Acceptance boundary.**
- Implementation and isolated automated verification are complete, within the scope recorded in this section.
- Real provider, real service worker lifecycle and device acceptance remain unverified.
- The earlier `db_gated` abort remains unexplained, not fixed. Its absence from these runs is not evidence of a fix.
- Real push remains disabled. No deployment acceptance is claimed.
