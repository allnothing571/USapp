# Phase 2 P2-3: In-app notifications

Status (2026-10-29): **P2-3a implemented directly by Codex and test-verified on usapp_test; NOT DEPLOYED.** Owner approved mandatory proposals and outcomes. 0019 is applied/frozen on test only. Detailed evidence: Section 11. P2-3b is not assigned. Sections 1-10 retain the historical design/review baseline; their not-started statements describe that earlier stage.
Revised after Claude Code's independent static review: use typed source references and
split P2-3a (mandatory inbox) from P2-3b (optional notifications/choice memory).
Live/device and deployment verification remain deferred. P2-2 is implemented and
test-verified on usapp_test, including the pin-replay repair; it is not deployed.

Sources inspected: V1 sections 17, 18, 18.1-18.3, 42 and 52.4 in `_req_extract.txt`;
`docs/architecture.md` section 5; `docs/phase2-plan.md`; current message, anniversary,
task and redemption services; `services/push-worker/README.md`.
Frozen migrations: 0000-0017 on test/main; 0018-0019 on usapp_test only. Do not edit them.

## 1. Scope and boundaries

Deliver a recipient-private inbox, unread count, individual read receipts and ordinary
content notification toggles. Integrate the existing business write paths; do not create
a second business backend or generic workflow engine.

Exclude Web Push, push subscriptions, delivery queues, email, reminders, reactions,
images, real-time sockets, notification deletion and mark-all-read. Those can be added
in their own milestones. Do not change asset settlement, pricing, refunds or permissions.

Guarantee: when a transaction requiring a notification commits, the recipient's durable
in-app notification exists in that same commit. This does not guarantee that a person
has viewed it. Notifications are informational; the business endpoint remains authoritative
for current state and allowed actions.

## 2. Event and recipient policy

The following is the proposed initial event registry. Required by V1 and recommended
product additions are explicitly separate. Event names are proposed contract names,
not a claim that these identifiers already exist in source code.

| Business event | Recipient | Delivery policy and basis |
|---|---|---|
| Direct redemption created | Product creator | Mandatory, V1 section 17; one notice for the original redemption |
| Request redemption created | Product creator | Mandatory, V1 sections 17/18.2; unpaid requests also notify |
| Task-change proposal created | The member entitled to decide | Recommended mandatory: actionable request |
| Anniversary edit proposed | Anniversary creator | Recommended mandatory: actionable request |
| Redemption cancellation proposed | The member entitled to decide | Recommended mandatory: actionable request |
| Above requests approved/rejected/withdrawn | Other affected member | Recommended mandatory outcome notice; no notice to the actor |
| Redemption request confirmed/rejected/withdrawn | Other affected member | Recommended mandatory outcome notice |
| Redemption fulfilled | Redeemer | Recommended mandatory outcome notice |
| Task published | Task assignee | Optional, V1 section 18.1 |
| Anniversary created or directly edited | Other member, if present | Optional, V1 sections 18.1/29 |
| Message created or replied | Message recipient / original author | Recommended optional, explicit sender choice; not declared mandatory by V1 |

Do not emit twice for overlapping descriptions of one transition. Approving an anniversary
proposal creates one outcome notice, not another optional direct-edit notice. Approving
a paid cancellation produces one outcome notice even though it also posts a refund.
Confirming an existing redemption request does not recreate its initial request notice.
Recurring task publication can notify once; lazy daily/weekly period materialization does
not notify. An M6 edit approval inserts a successor root task, but emits only its
decision-outcome notice, never a task-published notice. Personal home-pin changes do
not notify the partner. The event registry must distinguish a user publication from
an internal successor insert; notification creation is not a generic tasks INSERT hook.

No task-progress or completion notices in this milestone. Their volume and later reaction
UX need a separate product decision. V1's account/asset-anomaly notifications have no
implemented detector or defined thresholds: do not pretend to implement them here and
do not produce anomaly notices for normal low balances or rejected requests.

Recipients are resolved from authoritative same-space business rows, never a client
`recipient_id`. For ordinary shared content without a partner, creation still succeeds
with no notification and no retroactive delivery when a partner later joins. In P2-3b,
return successful business data with an explicit non-error notification result:
`{requested: true, created: false, reason: "no_partner"}` when notification was requested.
Do not update choice memory in that case. Disable the toggle when the current space has
no partner, but the server must still handle a stale client's true request this way.
Mandatory redemption paths already require their domain counterpart; do not weaken that.
Before P2-3b implementation, specify durable evidence for the optional notification result:
a replay must not infer the original no_partner decision from CURRENT membership. Distinguish
per-operation notification-decision evidence from effective remembered preference; recording
the former must not update the latter for omission/no partner. This is an open P2-3b schema
detail, not a reason to add speculative tables to P2-3a.

## 3. Ordinary notification choices and compatibility

This section is P2-3b only. P2-3a adds no notification toggle, choice table or changes to
existing business request bodies/fingerprints.

Architecture section 5 already states: ordinary notifications default off and remember
the last choice per user plus operation type. Preserve that rule.

- Add an optional boolean `notify_partner` only on the ordinary operations in section 2.
- Omitted means **false**, including for old clients and replays. Do not resolve omission
  from a mutable preference at request execution time.
- Forms load the last successful choice for that user/operation; the form submits an
  explicit boolean. Loading failure defaults off with a retry hint; it must not submit true.
- Capture the boolean with the exact payload/key when submitting. Changing the toggle
  cannot alter an unresolved action; retry sends the original choice.
- On the first committed ordinary write with an EXPLICIT boolean and an available partner,
  save its choice in the same transaction as the business write. Omission never writes a
  choice event: an old client's operation must not reset a newer device's remembered true.
  Validation failure saves nothing. If commit is unknown to the client, the preference
  event may already be committed; a replay must only read it, never append it again.
- A same-key business replay must not update the latest preference or create another notice.
- Mandatory events ignore preferences. Their controls are explanatory text, not a toggle.

Fingerprint compatibility needs per-service review. Keep the existing fingerprint bytes
for omitted/false. For true use an explicitly versioned/domain-separated extension that
includes the choice; store the resulting fingerprint in the EXISTING fingerprint column
of the immutable operation record. Do not add notify_partner columns to frozen business
tables. The typed choice event records the explicit choice, while the fingerprint detects
same-key/different-choice reuse. A retry with
the same key and a different choice is key reuse, never a second write. Do not change
normalization or the fingerprints of unrelated actions. Test old-key replays explicitly.

## 4. Minimal proposed database model

Proposed P2-3a migration: `0019_notifications.sql`, subject to checking the actual migration
set before implementation. This file does not yet exist. It requires 0018: anniversary
proposal integration stays in P2-3a if its mandatory policy is approved. Main release order
is 0018 before 0019; this design does not authorize an independent earlier notifications
release. P2-3b gets a later new migration, with its number chosen then.

1. `notifications`: immutable rows with `id`, `couple_id`, `recipient_id`, `actor_id`,
   closed-enum `event_type`, TYPED source references, `created_at`, and safe display facts.
   Source families are task_id, task_change_proposal_id, redemption_id,
   redemption_event_id, anniversary_id + anniversary_version,
   anniversary_proposal_id, and message_id. Add only the families used by each batch.
   A per-event CHECK requires its exact reference family and forbids unrelated non-null
   references; a composite anniversary revision reference requires BOTH fields non-null.
   Source identities for decisions/fulfillment/replies use their existing primary keys
   with DISTINCT event kinds (proposal_id, redemption_id and message_id respectively).
   For each family use a partial UNIQUE index on (event_type, typed key, recipient_id),
   including anniversary_version for revisions. Do not rely on an ordinary UNIQUE across
   nullable families: nulls would defeat dedup. Never invent a new source UUID on retries.
2. `notification_reads`: one immutable receipt per notification, with `notification_id`,
   `couple_id`, `recipient_id`, `read_at`; composite FK to the exact notification/recipient/
   space; notification_id is the primary key. Enforce read_at >= notification.created_at
   and read_at <= clock_timestamp() in a guard. Its key makes read marking idempotent.
3. P2-3b ONLY: `notification_choice_events`, append-only per-user/per-operation history with
   sequence, explicit boolean, stable business operation reference and creation time.
   Serialize using a dedicated user+operation lock; the highest sequence is current.
   PK (user_id, operation, seq), and the same typed operation-reference approach as above.
   Partial unique indexes on (user_id, operation, typed business key) prevent a replay
   from remembering an old choice again; revision keys include their version.
   Do not add a global preference switch that can disable mandatory notifications.

Membership FKs and per-table append-only triggers follow existing conventions. Define
shape, enum and actor/recipient/time checks. Use same-space composite foreign keys where
existing keys support them. Anniversary revisions have PK (anniversary_id, version), not
a UUID: reference that key and use the identity's (id, couple_id) FK for the space.
reward_redemption_events has PK(id) but no UNIQUE(id, couple_id): reference FK(id) and
enforce same-space/action/actor/recipient in a guard. Use this approach too for existing
decision/fulfillment tables lacking a matching composite unique key. Do not alter old
tables just to create new reference keys. Guards supplement FKs for closed-event semantics:
a proposal reference alone does not prove its decision exists or has the claimed outcome.
Do not accept arbitrary titles, links, recipient ids or source ids through a public API.

Grant only SELECT/INSERT to usapp_api, none to PUBLIC/usapp_push. Add objects to repeated
InitRoles, startup privilege enforcement and the deviation query. The shared application
role can read other domains today; recipient privacy is enforced in service SQL as well
as through reference/actor constraints, not claimed as row-level SQL isolation.

Indexes: notifications (recipient_id, created_at DESC, id DESC), its typed dedup indexes,
and UNIQUE(id, couple_id, recipient_id) for read receipts; notification_reads PK
(notification_id) plus (recipient_id). In P2-3b the choice PK supports latest choice via a
backward scan for user/operation. Start unread-count with a recipient-filtered NOT EXISTS
query; count(notifications) minus count(reads) is also correct only while the exact
receipt FK and matching filters hold, in ONE statement snapshot. Choose on measured plans,
not a claim that either query is necessarily faster.

If the append-only preference design proves disproportionate, propose a narrowly mutable
preference table with explicit column grants and a concurrency model before implementation.
Do not silently expand UPDATE privileges or alter existing tables' immutability.

## 5. Transactional integration and lock order

Use a small C++ NotificationWriter helper that operates on the caller's existing TxScope.
The domain service determines the committed transition and allowed optional choice; the
helper applies the closed registry and recipient policy. It neither commits independently
nor opens a second transaction. It has no wallet or business state-changing methods.

Order for a NEW business operation:

1. Existing membership/target locks and idempotency classification.
2. Existing domain advisory/account locks, checks and business inserts.
3. Existing audit insert; resolve stable transition identifiers.
4. Optional preference lock/event, then notification insert when required/selected.
5. Read response and confirmed commit, preserving the existing unknown-commit handling.

A notification insert failure rolls back the business operation, including any debit or
refund. Prove this with failure injection on the new helper; do not merely assume it.
This intentionally couples availability: an optional-notification bug also blocks content
creation in P2-3b. A confirmed rollback is an ordinary error, not RESULT_UNKNOWN. Reserve
RESULT_UNKNOWN for cases where the commit outcome cannot be established. Database load,
storage failures and timeouts can fail notification writes too, not only programming bugs.
For a newly classified operation use a plain notification INSERT: dedup collisions must
surface rather than silently swallowing a required notification. Read receipt INSERTs may
use ON CONFLICT for their deliberately idempotent endpoint.
The helper must not acquire another domain lock after a preference lock or call back into
another service. Check every integrated path for cycles, especially operations that already
hold wallet locks. Read marking never takes business or preference locks.

Classified replays return immutable original facts without running the event writer again.
No historical backfill and no scan of all audit rows. Existing audit details were not a
versioned event contract. P2-3 directly writes new notifications at the authoritative
transaction boundary; existing rows remain unchanged.

For future P2-5, a notification id is a stable delivery source. Push dispatch will need
durable delivery state or an outbox and retry/claim semantics. Do not call the current
in-memory push-worker queue durable, and do not send Push inside a business transaction.
The worker remains delivery-only and makes no recipient, financial or workflow decisions.

## 6. Proposed HTTP contract

All endpoints require membership and recipient ownership, private no-store responses,
and existing exact-Origin/session-CSRF protections for writes.

- `GET /api/v1/notifications?limit&cursor&unread_only`: default 50, max 100; immutable
  `(created_at DESC, id DESC)` keyset cursor, strict validation; returns rows and next_cursor.
- `GET /api/v1/notifications/unread-count`: exact current unread count, independent of
  a bounded page scan; no client inference of zero from a failed or partial page.
- `POST /api/v1/notifications/{id}/read`: naturally idempotent receipt insert; replay
  returns the original read receipt. Another user's or space's id returns 404. Unknown
  response is safely retried against the same notification, never locally assumed read.
- P2-3b only: `GET /api/v1/notification-choices` returns the caller's ordinary-operation
  defaults. This milestone needs no separate preference-write endpoint: successful
  ordinary business writes remember the choice.

List read status and unread count can use one statement snapshot if returned together;
separate requests may legitimately differ during concurrent reads. Cursor pages are not
snapshots; with unread_only, receipts can remove items between pages. Long-running writes
can also commit a notification with an earlier created_at after a page was read. Refresh
from the head on return/explicit refresh; never promise that following cursors captures
all newly committed notices or use created_at alone as a future delivery checkpoint.

No mark-all-read in this scope: a naive timestamp watermark can wrongly consume an unseen
late-committing notification. A later design must define an observed set or safe boundary.

## 7. Web and notification content

App.vue currently has no shell header. Add one minimal authenticated/member-only
NotificationEntry component inside app-shell, above main, linking to `/notifications`.
It contains the bell, label and badge; do not refactor every page header or add a sixth
bottom-nav tab. Hide it on auth/space onboarding and unauthorized routes. Show a badge
from the count endpoint; failure shows a retry/error state, not zero.
Use explicit loading, empty, error and pagination states. Opening the inbox does not
automatically mark the entire page read. Proposed UX: a row click marks that row read and
navigates to its target; preserve an explicit retry if marking failed.

Render fixed event labels and minimal snapshots as escaped plain text. Do not include
message bodies, reply text, anniversary notes, email addresses or arbitrary HTML/URLs in
notifications. For a target no longer actionable, show its current detail or an honest
unavailable state. Do not imply that an old notice proves a request is still pending.

In P2-3a task events link to `/tasks` and reward events to `/rewards`, with copy such as
"前往任务页处理". These pages have no item-detail routes; no new item-selection mechanism
is in scope. Anniversary events use `/us/anniversaries/:id`; P2-3b messages can use the
existing `/us/messages/:id`. Do not invent task/reward detail routes. Typed source facts
retain proposal/period identity for truthful labels or later enhancements.

Keep inbox, count and mutation request sequences plus session-generation guards. Reset
all notification data and captured choices on identity changes. Notification reads cannot
reset task/reward/message/anniversary actions or their unknown keys. Local read updates
must be monotonic within one account; a stale list response cannot undo a confirmed receipt.
Unread counts need both request sequencing and a local receipt epoch. On a confirmed
receipt invalidate every earlier count request and refetch; accept a response only if its
request sequence, receipt epoch and session generation remain current. Do not blindly
decrement a badge when a repeated read may already have been counted as read. Multiple
devices can still change state after a valid snapshot; refocus/refresh reconciles it.
Refresh on mount, visibility return, focus and manual refresh, with coalescing to prevent
duplicate in-flight refreshes. No background polling or real-time transport in this scope.

## 8. Verification plan (not executed)

- Unit: closed event/recipient matrix, payload safety, request parsing, cursor validation,
  preference operation allowlist and legacy/false/true fingerprint compatibility.
- DB: creator/recipient/cross-space checks, append-only constraints, receipt idempotence,
  event dedup, sequential preference changes and replay not restoring an old choice.
- Integration: mandatory direct/request redemption, optional on/off paths, proposal and
  outcome recipients, no self notices, approval not double-notifying, concurrent same-key
  writes, no notices on rejected transactions, forced notification failure rolls back
  business/audit/wallet rows; committed unknown result followed by replay adds nothing.
- Preserve full M3-M9 and P2-1/P2-2 regressions; do not weaken ledger fixtures or constraints.
- Web: badge/list failures, pagination, confirmed read versus stale read, explicit retries,
  captured toggle on unknown retries, choice defaults, target navigation and account races.
- Test migration: rollback-only prevalidation, apply+InitRoles, privilege query twice,
  frozen checksums, fresh test image and full CTest with db_gated not skipped, SQL assertions
  in both roles, full web suite/typecheck. No served-dist build during test verification.

Actual runtime policy counts must be measured, not inferred from this design. No new
tests or migration have been written or executed by this document.

## 9. Implementation batches and approval decisions

Proposed split, after owner approval:

1. **P2-3a: mandatory inbox.** Notifications/receipts, typed sources, indexes, grants;
   NotificationWriter and mandatory integrations; inbox/count/read and minimal shell entry.
   Mandatory direct/request redemption creation is required. Actionable proposals and
   outcome events join this batch only after the policy table is approved. No choices,
   notify_partner fields, preference history or business fingerprint changes in this batch.
2. **P2-3b: optional notifications.** Separate migration for typed choice history; ordinary
   task/anniversary toggles and optional messages if approved; backward-compatible
   fingerprints, explicit-choice memory and no-partner response semantics.
3. Within each milestone iterate with affected tests, then one full relevant regression
   and SQL assertions in both roles. Main rollout remains separately authorized, requires
   0018 first, and remains deferred with device acceptance. Neither batch is implemented.

Owner decisions:

| Decision | Recommendation |
|---|---|
| Scope | In-app only, no Push dependency |
| Milestone split | P2-3a mandatory inbox first, P2-3b choices/toggles second |
| Mandatory events beyond V1 redemption creation | Actionable proposals and their outcomes as in section 2 |
| Message/reply notifications | Optional with remembered choice, initially off |
| Ordinary defaults | Preserve existing off + per-user/operation last successful choice |
| Read marking | Individual only; no mark-all-read or deletion |
| Existing history | No backfill; begin with new committed operations after deployment |
| Content privacy | Fixed safe labels, no personal message/note bodies |

This design adds no production-readiness or acceptance claim. Tests passed in earlier
milestones remain dated evidence, and deferred checks have not been converted into passes.

## 10. Independent review disposition (2026-10-29)

Claude Code's report was a read-only static review, not runtime verification. Codex checked
the relevant migration keys and App.vue before revising this design.

- H1: replace polymorphic source_id with typed references and per-family partial uniqueness.
- M1: split mandatory inbox from optional toggles/fingerprint changes.
- M2/M3: define successful no-partner response; no choice memory on omission/no partner;
  no new notify_partner columns on existing business tables.
- M4: invalidate stale counts across confirmed receipts, not only stale list state.
- M5: M6 successor creation never emits task-published; add an integration assertion.
- L1/L2/L4/L5: concrete minimal shell entry, honest list-page links, 0018 release dependency,
  indexes and receipt time guards.
- L3: P2-5 must reconcile the worker README's no-database boundary with its outbox note.
  P2-3 promises stable notification ids only; no worker change is authorized.
- L6: document atomic availability trade-off, ordinary rollback errors versus unknown
  commit outcomes, and visible dedup collisions. Notification failure is not limited to bugs.

Add explicit tests for repeated anniversary edits with distinct version references,
cross-space redemption-event ids, malformed reference-family combinations, stale count
after confirmed read, omitted choices not overwriting true, no-partner successful writes,
and no second task-published notice from a successor. P2-3a tests only its implemented
events; P2-3b adds its choice/compatibility matrix. No test has been executed for this design.


## 11. P2-3a implementation and verification (Codex, 2026-10-29)

### Actual implementation

Owner approved direct implementation with mandatory actionable proposals and outcomes.
No optional content notices, preferences or fingerprint changes (P2-3b).

New files:
- database/migrations/0019_notifications.sql; database/tests/notification_ledger_test.sql.
- services/api/src/services/NotificationWriter.{h,cc}, NotificationService.{h,cc};
  services/api/src/controllers/NotificationController.cc.
- services/api/tests/NotificationDbTests.h (db_gated block 26).
- apps/web/src/api/notifications.ts, stores/notifications.ts,
  components/NotificationEntry.vue, views/NotificationsView.vue;
  apps/web/tests/unit/notifications.spec.ts.

Modified: TaskChangeService, RedemptionService, AnniversaryService; API CMakeLists;
TaskPrivilegePolicy and unit fixtures; DbGatedTest and StaticCheckTest; init-roles section
17 and privilege deviation query; OpenAPI 0.16.0; web App/router/session/styles and the
bottom-nav test router; existing task/wallet/product/redemption/anniversary SQL scripts
(TRUNCATE dependencies only, all assertions and expected SQLSTATEs preserved); status docs.

Database: two append-only tables, typed source references, partial per-family uniqueness,
member/space/actor/recipient/time guards UN101-UN103, receipt FK, inbox/count indexes and
SELECT/INSERT-only grants. Closed resolver usapp_notification_source derives the 18 event
kinds from immutable business rows. Anniversary revision references are not needed until
P2-3b optional direct-edit notices. Guards validate notices; requiring every business
source to have a notice is a SERVICE-TRANSACTION guarantee, not a deferred DB constraint.
Direct-SQL fixtures do not automatically notify. The application SQL role is not row-level
isolated; endpoint SQL restricts the caller/space/recipient.

NotificationWriter joins the caller's existing TxScope after audit and before commit.
It takes no independent commit/network/domain lock. All three service integrations call
it only for newly inserted operations; classified replays bypass it. Ordinary content
creation, lazy periods and M6 successors do not produce extra notices. No backfill.

API: recipient-private list, exact unread count, individual read marking with an empty
JSON object. AuthFilter, strict cursor/limit/filter validation and no-store responses.
NOT_FOUND hides absent/other-user/other-space ids. The notification id is the natural
idempotency target; read writes need Origin/CSRF but no Idempotency-Key. Explicit TxScope
commit gates read success. A concurrent receipt invisible to the statement snapshot can
return an unconfirmed response for safe explicit same-id retry.

Web: /notifications, minimal shell entry above main, unread badge, Home return link,
list/filter/pages/error states and read/retry. Existing five bottom-nav destinations stay
unchanged. Task/reward links honestly open their list pages; anniversary links use the
existing detail route. Fixed labels contain no message/note text. Confirmed read memory
prevents stale-list regression; receipt epoch/count sequence discard pre-read counts.
Confirmed reads refetch count instead of blindly decrementing it. Account reset clears
all state and late responses. Focus/online/visibility/manual refreshes coalesce. No polling,
persistence or automatic mutation retry.

### Verification performed

- Rollback-only prevalidation first exposed excess default grants for usapp_api. Before
  first application, added explicit REVOKE for that role, then SELECT/INSERT grants.
  Both roles then passed 14 notification SQL cases and rolled back. Temporary generated
  repository wrappers were removed. No migration bytes changed after application.
- DryRun exit 0: 19 matching skips, only 0019 pending. Apply+InitRoles exit 0 and 0 privilege
  deviations. Repeat exit 0: 20 skipped/0 applied, again 0 deviations. All 20 registered
  checksums matched local files.
- Frozen 0019 SHA-256:
  6da7c5c157191e2acd2fe05e27dd0fecd441ec52e47b76f9ab96d4d8a381e0e9.
- Fresh test image builds exited 0. First full CTest failed two NEW test fixtures/assertions:
  request expected Redeemed instead of Requested; audit fixture used actor_user_id instead
  of actor_id. Fixed the tests without weakening intended checks. Fresh image and second
  full CTest exited 0: unit 0.03 s, db_gated 11.20 s (block 26 ran), static_check 0.01 s;
  3/3 passed, total 11.25 s.
- Observed test-process policy matrix clean: task 1536 checks, wallet 187 checks. This is
  not a startup observation of the deployed API.
- New DB coverage: anniversary proposal/approval/replay; receipt authorization/replay;
  unread filtering; direct/request/confirm/cancel/refund/fulfillment; pages; successor notice
  count; failing NotificationWriter rolls back staged wallet operation, ledger entry and
  audit in its real caller TxScope. Fault is a missing source (zero rows), not lost COMMIT.
- Seven SQL scripts in both roles: 14/14 exit 0, ROLLBACK. Task 35 groups; wallet 28;
  product 8; redemption 6; messages 5; anniversaries 3 groups/34 cases; notifications 14 cases.
- Full web: 25 files / 313 tests passed, exit 0, 14.30 s; both typechecks exit 0. A test-router
  missing the inbox route caused a warning; fixed it. After narrow view refresh/navigation
  changes and one extra test, affected specs passed 22 tests (notifications 13 + nav 9),
  without stderr; vue-tsc and node-config tsc passed. Full suite was not repeated solely
  for those narrow follow-ups.
- Web coverage: old counts/list after reads, same-target unknown retries including later
  rejection, malformed receipts, account/late-result isolation, page dedup/invalidation,
  failure not showing zero, actual targets, error view and refresh coalescing.

After the two CTest runs, read-only observation found 176 notices across all 18 event kinds,
2 receipts and the existing isolated @m3-fixture.test domain fixtures on usapp_test.
Failed tests can leave committed fixtures. SQL scripts commit nothing. No reset or fixture
cleanup. Logs retained at elevated-executor %TEMP%/usapp-p23a-ctest.log (failure) and
usapp-p23a-ctest-final.log (pass); container /tmp/p23_* copies remain.

### Not done / limitations

No main access, API/Caddy replacement/restart, served-dist build, deployment, Git,
dependency installation, permission-rule change, backup read or browser/device acceptance.
P2-3b, Push and reminders are not started. No live HTTP controller acceptance, actual
lost-COMMIT fault injection or load benchmark. No installed YAML parser was available for
the optional parse check: contract static checks passed, dedicated YAML parsing not run.

Notice/receipt history grows append-only. Counts/pages reflect separate query snapshots;
other-device changes require refresh. Source text is not stored. Main needs 0018 then
0019 before this API starts. Previous-image startup/rollback compatibility is untested.
Deferred checks remain deferred, not passed. Review priorities: closed source registry,
replay bypasses, writer-failure rollback, read concurrency and frontend receipt/count/
session epochs.

Final backend follow-up: added direct NotificationService unread-count assertions before
and after a confirmed receipt (1 -> 0) and safer optional-result checks in the tests.
Fresh image build exited 0; third full CTest exited 0: unit 0.03 s, db_gated 10.97 s,
static_check 0.01 s, total 11.02 s. No backend code or schema changed in this follow-up.
The earlier 176-notice/2-receipt observation above was before this additional fixture run.
Final read-only fixture observation: 264 notifications and 3 receipts on usapp_test.

## 12. P2-3b concrete design (2026-10-30; implementation approval pending)

The owner asked to continue after P2-3a. Codex inspected the current fingerprint,
classification and notification code before preparing this next milestone. This section
resolves the open decision-evidence issue from sections 2-4. No P2-3b migration, business
code, tests, database operation or deployment was performed by this design update.

### 12.1 Proposed scope

Five independent operation types: task publication, anniversary creation, anniversary
creator direct edit, message creation, and message reply. Each has an optional
`notify_partner` boolean. This includes messages/replies as the previously recommended
product default, subject to owner confirmation. No product edits, task progress/completion,
M6 successor publication, or extra notice for an anniversary proposal approval.
Existing P2-3a mandatory notices cannot be disabled by any of these choices.

Task publication and messages already require their counterpart. Keep PARTNER_REQUIRED;
do not reinterpret those operations as partnerless creations. Anniversary creation/direct
edit may succeed without a partner, with no notification and no effective preference change.

### 12.2 Immutable per-operation decision, separate from effective preference

Use one new append-only `notification_intents` table rather than separate intent and
choice-history tables. Its rows record the actual original decision; only eligible rows
participate in remembered defaults. This replaces the proposed standalone
notification_choice_events table for P2-3b; no table by that name exists today.

Proposed columns: id, couple_id, actor_id, operation (closed enum), typed source reference,
choice_explicit, requested, recipient_id (nullable), notification_id (nullable),
preference_seq (nullable), created_at. References are exactly one of:

- task_id for a real user publication, excluding M6 successors;
- anniversary_id AND anniversary_version for create/direct-edit revisions only;
- message_id for message creation or its reply, distinguished by operation.

Use same-space FKs where possible, exact-family CHECKs (both anniversary fields required),
actor/source/time guards and partial UNIQUE indexes for each operation/source/actor key.
FK a revision by its real composite primary key; the anniversary identity supplies the
space FK. A reply must reference the actual reply row, not merely an existing message.

Every NEW ordinary operation records one intent, even for omission or no partner:

| Input / original recipient | requested | Effective preference update | Notice |
|---|---|---|---|
| Omitted / any | false | No | None |
| Explicit false / present | false | Remember off | None |
| Explicit true / present | true | Remember on | Exactly one |
| Explicit false or true / absent (anniversary only) | Submitted boolean | No | None |

`preference_seq` is non-null ONLY for explicit choices with an original recipient. Such
rows acquire the dedicated (33012, hashtext(actor_id + operation)) advisory transaction
lock and allocate max(preference_seq)+1 for that actor/operation. Use a partial unique
index on (actor_id, operation, preference_seq) and an index supporting latest non-null
sequence lookup. This lock is acquired after existing domain locks; the code must not
return from it to acquire another domain or wallet lock. The current source tree uses
33010/33011 for anniversaries, not 33012; recheck before implementation.

The latest non-null sequence supplies that user's remembered default; absent means off.
Omitted/no-partner intents provide decision evidence without participating in the default.
No additional preference table, arbitrary preference-writing API, or UPDATE grant is needed.
Retention is append-only in this milestone.

For requested=true with an original recipient, preallocate a notification id, append the
intent and notice in the same business transaction, and enforce their exact one-to-one
link with a deferred check/FK. The notification must match the intent's actor, space,
recipient, operation and typed source. Off/no-partner intents must link no notification.
This prevents a forged optional notice claiming an operation that elected not to notify.
The later migration must extend the source resolver/notification guard and exact-family
constraints without editing 0019. Recheck existing constraint names when writing it.

### 12.3 Replay and backward compatibility

Keep existing fingerprint bytes unchanged for omission/false. For true, use a documented
domain-separated hash extension of the legacy fingerprint, e.g.
SHA-256("notify-partner:v1:true:" + legacy_fingerprint), stored in the EXISTING fingerprint
column. Preserve all previous normalization/schedule/version/idempotency semantics. Only
the five ordinary operations use this extension; mandatory action fingerprints stay intact.

Omission and false are the same notification choice for key classification, but have
different first-write preference effects. The FIRST committed intent is authoritative:
replaying an omitted request as false cannot retroactively remember off. True/false with
the same actor/key and business body is key reuse. Check each service's request parser,
controller DTO and canonical fingerprint function; do not append a field only in the web.

Classify existing actor/key before new recipient/choice decisions. After a classified
replay, read its original intent or return notification=null for a legacy operation with
no intent. Never infer the original decision from current membership/preferences; never
insert a missing intent/notice or update defaults on replay. Legacy replays retain their
normal business mutation result and explicitly have no recorded notification result.

New mutation responses add `notification` (original facts):
`{requested, created, reason, notification_id}`. Reasons: delivered_to_inbox,
not_requested, no_partner. `created` means a durable IN-APP row, not Push delivery or human
attention. Legacy responses use null. No-partner is a successful business response; a
partner joining later does not change that original result or create a deferred notice.

Intent, effective preference event (when eligible), notice, business and audit commit
together. A notification bug can therefore block an otherwise valid ordinary write; this
is the explicitly proposed atomic-availability trade-off. A confirmed rollback is an
ordinary error; only an unconfirmed commit produces RESULT_UNKNOWN. Notification audit
facts must contain no message/reply/note body.

### 12.4 API and web

- Add GET /api/v1/notification-choices for the caller's five operation defaults and their
  sequences. No other user's choice is returned; no write endpoint in this milestone.
- Retain default off. Load remembered defaults when a form starts; an asynchronous response
  cannot overwrite a toggle the user has already touched or a submitted/unknown action.
  On read failure display off plus a retry hint; never silently enable notifications.
- Disable the anniversary toggle without a partner; the server still handles a stale true
  request by recording no_partner without changing effective preference.
- Capture explicit boolean, full payload, target/version, account and key before awaiting.
  A pending unknown action blocks replacement; later errors keep the exact original action.
  The user explicitly retries. Do not reload a current preference and rebuild an old retry.
- Validate the business mutation and optional notification facts separately. A legacy null
  notification result must not invalidate a valid old business replay. A malformed field
  on a new response leaves its result unknown rather than inventing notification success.
- Reuse the P2-3a inbox, count/read safeguards and session reset; extend fixed labels and
  supported targets only. Refresh preference defaults for FUTURE forms after confirmed
  writes; never update another active draft or another account from a late response.

### 12.5 Implementation and tests (proposed; not executed)

1. New migration (candidate 0020, recheck actual set) extends optional typed source
   references, creates intents/guards/links/indexes and adds narrow grants/startup policy.
   Preserve frozen 0000-0019. No main operations or served-dist build.
2. Shared optional notification helper plus TaskService and anniversary create/direct-edit
   integrations. Then message/reply integrations if approved. Keep domain decisions in C++.
3. Five form toggles, preference reads, exact captured retries and response validation;
   keep existing first-attempt rejection and session-reset behavior.
4. Affected tests while iterating, then one relevant full regression on usapp_test.

Critical tests: old literal fingerprint fixtures unchanged for all task schedules; legacy
same-key replay; explicit false versus omission first-write/replay; true/false key reuse;
two anniversary edits yield separate composite revision sources; message/reply actor matrix;
same-key concurrency creates one intent/notice; replay after newer preference does not
restore it; omitted/no-partner writes do not participate in preferences; partner joins after
no-partner result, old replay still returns no_partner; commit-unknown retry retains the
captured choice; missing/forged source or mismatched intent link rejected; rollback removes
all staged records; late preference read does not override a touched toggle; account races;
mandatory P2-3a notices still occur regardless of these defaults; successor publication
and approval do not create a duplicate optional notice.

### 12.6 Confirmation needed

Recommended defaults: all five operation types above, optional messages/replies included,
off initially with successful explicit choice remembered, and single-table immutable
decision evidence with an eligible preference sequence. This concretizes the earlier
schema choice, rather than authorizing implementation by itself. Confirm this scope before
the milestone's code/database changes. Deployment/device verification remains deferred.

## 13. P2-3b implementation (Claude Code, 2026-10-30)

The owner assigned P2-3b implementation and usapp_test-only automated verification to Claude
Code with the Section 12 defaults: five operation types including messages/replies, off
initially, explicit successful choices remembered, single-table immutable decision evidence.

**State: implemented and test-verified on usapp_test (Section 13.5). 0020 is applied and
FROZEN on usapp_test only; main is unchanged. Not deployed.** Main and device checks remain
deferred, not passed.

### 13.1 Files

- New: `database/migrations/0020_notification_choices.sql` (SHA-256
  `849dfbafa98d6a39dbe18de4a1b209b521e4e486568a5d7feb79d37cb085f448`; applied/frozen on
  usapp_test 2026-10-30, not on main);
  `database/tests/notification_choice_test.sql`;
  `services/api/src/services/NotificationIntent.{h,cc}`;
  `services/api/tests/NotificationIntentUnitTests.h`, `NotificationChoiceDbTests.h` (db_gated
  block 27); `apps/web/src/stores/notification-choices.ts`,
  `components/NotifyPartnerToggle.vue`, `tests/unit/notification-choices.spec.ts`.
- Backend changed: TaskService (.h/.cc), AnniversaryService (.h/.cc), MessageService (.h/.cc);
  Task/Anniversary/Message/Notification controllers; TaskPrivilegePolicy; UnitTest, DbGatedTest
  (gate `0020_notification_choices`, block 27), StaticCheckTest (contract 0.17.0, P2-3b case).
- Database changed: init-roles section 18; privilege deviation query (+notification_intents).
  Existing SQL scripts (task, wallet, product, redemption, message, anniversary, notification):
  TRUNCATE dependency lists only, because 0020's new foreign keys would otherwise make
  PostgreSQL's FK truncate check (0A000) mask the append-only rejection. Every expected
  SQLSTATE and assertion is unchanged; the first listed table still fires first.
- Contract 0.17.0: `notify_partner` on CreateTaskRequest, MessageTextRequest,
  CreateAnniversaryRequest and the new AnniversaryEditRequest (proposals keep the strict
  AnniversaryRevisionRequest); `TaskPublishResponse` and `OptionalNotificationResult`; the
  `notification` field on message and anniversary mutation responses; five new inbox event
  types and `message` targets; `GET /api/v1/notification-choices`.
- Web changed: api (notifications, messages, anniversaries, tasks, types), stores (messages,
  anniversaries, tasks, session reset), views (Tasks, Us, MessageDetail, AnniversaryDetail,
  Notifications), components (AnniversaryForm, AnniversarySection). Existing specs: response
  fixtures of tasks-view, messages, messages-repairs and anniversaries now carry the
  `notification` facts a new server returns, and one expected publish payload includes the
  form's explicit `notify_partner: false`. No assertion was removed or weakened.

### 13.2 Behaviour as implemented

- **Schema (0020).** `notifications` gains typed optional sources (`task_id`,
  `anniversary_id` + `anniversary_version`, `message_id`, `message_reply_id`) with same-space or
  composite FKs. The two unnamed 0019 checks (one source; target kind) are replaced by
  definition lookup (the migration fails unless exactly two are found), adding `message` targets
  and the revision pair check. Per-family partial unique indexes are added.
  `usapp_notification_optional_source` resolves the five operations from immutable business
  rows: a task root publication only (no lazy period, no M6 successor); create/edit revisions
  only (never `approve`); message author → recipient; reply author → message author. The
  recipient is the partner at that time, or NULL for a partnerless anniversary revision.
  `usapp_notification_guard` is replaced (CREATE OR REPLACE in 0020; 0019 is unedited), keeping
  the 0019 registry byte-for-byte and adding the optional events.
- **notification_intents** (append-only, SELECT/INSERT): one row per NEW ordinary operation with
  `choice_explicit`, `requested`, original `recipient_id`, optional `notification_id` and
  `preference_seq`. CHECKs: exact family per operation, create = version 1 / edit > 1,
  `requested` ⇒ explicit, notice ⇔ requested with a recipient, preference ⇔ explicit with a
  recipient, no self recipient. FKs: members, typed sources (revision by its composite key),
  and `(notification_id, couple_id, recipient_id)` to the notice. One intent per source.
  Guard UN104: source/space/actor/recipient/time from the resolver; contiguous preference
  sequence under advisory lock 33012 (`actor:operation`); a linked notice must match event and
  typed source. Deferred UN105: an optional notice must have its requesting intent at commit.
- **Fingerprints.** The optional field is removed from the body before the existing strict
  parsers, so omitted/false keep the literal legacy bytes for every task schedule; true uses
  SHA-256(`notify-partner:v1:true:` + legacy) in the same column. True vs false with one key
  is IDEMPOTENCY_KEY_REUSED; omitted and false classify as the same request, and the first
  committed intent alone determines preference effects.
- **Transactions.** After the existing audit insert: resolve source → (notice) → preference
  lock → intent → read back → commit. The preference lock is the last lock; nothing calls back
  into a domain or wallet. Replays (classification and ON CONFLICT re-classification) only read
  the original intent; a legacy operation without one replays with `notification: null`.
  A confirmed rollback is an ordinary error; only an unconfirmed commit is RESULT_UNKNOWN.
- **Responses.** Task publication `{task, notification}`; message create/reply and anniversary
  create/edit add `notification` = `{requested, created, reason, notification_id}` (reasons
  delivered_to_inbox / not_requested / no_partner), the immutable original facts.
- **Web.** Toggles on the five forms (none on proposals); partnerless anniversary toggles are
  disabled and submit false. A form takes the remembered default once, never over a touched,
  submitting or unknown action; failed reads stay off with a retry. Stores capture the explicit
  boolean with payload, key, target/version and account; after an unknown result every later
  error keeps that snapshot and retries resend it. Facts are validated separately from the
  business mutation: legacy null resolves; missing/mismatched/inconsistent facts stay unknown.
  Choices are per account (session reset, late reads discarded, reads coalesced) and refresh
  only for future forms after confirmed writes. Inbox labels are fixed text; message events open
  the message detail.

### 13.3 Verification run by Claude Code (2026-10-30)

- Web, installed local binaries: `vitest run` exit 0, **26 files / 326 tests** (13 s), including
  the new notification-choices spec (12 tests: facts validation, labels/targets, per-account
  choices with late/stale/malformed reads, toggle default/touched/locked/unavailable, captured
  message/anniversary/task choices across unknown results, later errors and changed defaults,
  legacy null). `vue-tsc` and node-config `tsc` exit 0.
- The first full run after integration failed 6 existing tests because their mocked
  responses lacked the new `notification` field and one expected payload lacked the form's
  explicit `notify_partner: false`; the fixtures were updated as listed above.
- Static: LF/final newline/no trailing whitespace in changed files (four pre-existing CRLF web
  files untouched by P2-3b were left as they are); frozen hashes 0018 `58137f31…f9f` and
  0019 `6da7c5c1…0e9` unchanged.

### 13.4 First attempt blocked (superseded by Section 13.5)

Docker Desktop was unreachable at every check during this session:
`failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine … The system
cannot find the file specified.` (EXIT=1). Not started by Claude Code. Therefore NOT run:
migration-state check, prevalidation, DryRun/apply/InitRoles/privilege queries, the test image
build (**the C++ has not been compiled**), CTest (unit, db_gated block 27, static_check) and the
SQL scripts. The contract was not YAML-parsed (no validator installed).

`Claude outputs/p23b-verify-usapp-test.ps1` (untracked) runs these stages fail-fast: state
0000-0019 with frozen hashes asserted; prevalidation in both roles (`groups=3 cases=17`, nothing
persisted); DryRun 20 skipped + 0020 pending; one apply; 0 privilege deviations twice; repeat 21
skipped; 21 matching registrations; CTest 3/3 with db_gated Passed and nothing skipped; eight
SQL scripts in both roles with their recorded group/case markers and ROLLBACK; web suite and
typechecks. Fixture effects so far: none (no database was touched).

### 13.5 usapp_test verification (Claude Code, 2026-10-30, after Docker Desktop was started)

Ran `Claude outputs/p23b-verify-usapp-test.ps1` once (work folder
`%TEMP%\usapp-p23b-verify-20261030_184457`). Script exit 0; every stage asserted fail-fast;
no repair was needed.

- State before: local 0000-0020; usapp_test exactly 0000-0019 with matching checksums
  (0018/0019 frozen hashes asserted); 0020 unregistered; no notification_intents.
- Rollback-only prevalidation (0020 body + notification_choice_test.sql): postgres/maintenance
  and usapp_api/application each `groups=3 cases=17`, exit 0; afterwards 0 registrations and
  no notification_intents. This also exercised the 0019 check replacement against the live
  catalog (exactly two checks found and dropped inside the rolled-back transaction).
- Fresh test image build: exit 0, 51 s. One new compiler warning, test code only:
  `NotificationIntentUnitTests.h:42` range-loop copy (`-Wrange-loop-construct`); not fixed so
  as not to trigger another committed-fixture CTest run for a cosmetic change.
- DryRun: 20 skipped, only 0020 pending with the hash above. Apply+InitRoles: exactly one
  `[apply]`/`[ok]` 0020; 0 privilege deviations. Repeat InitRoles: 21 skipped, 0 applied; 0
  deviations. All 21 registered checksums equal the local files.
- Full CTest: exit 0, 3/3 passed, nothing skipped: unit 0.04 s, db_gated 12.46 s,
  static_check 0.01 s (total 12.51 s).
- Evidence that block 27 ran: a read-only count afterwards found exactly 6 optional notices
  (task.published / anniversary.created / anniversary.edited / message.created /
  message.replied), the number block 27 commits once; no other code path writes them.
  The same observation found 106 intents and 11 remembered-choice rows on usapp_test, which
  also include other blocks' service writes (omitted choices), so they are not attributed.
- Eight SQL scripts in both roles, each exit 0 with its marker and ROLLBACK: notification
  choices 3 groups/17 cases; notifications 14 cases; messages 5 groups; anniversaries 3
  groups/34 cases; tasks 35 groups; wallet 28; products 8; redemptions 6 (the last seven
  include the TRUNCATE dependency edits).
- Web: 26 files / 326 tests exit 0; vue-tsc and node-config tsc exit 0.

Migration 0020 is applied and frozen on usapp_test; it must never be edited.

Fixture effects: block 27 committed isolated `@m3-fixture.test` spaces with tasks, anniversaries,
messages, replies, intents, notifications and audit rows on usapp_test, like earlier blocks.
Prevalidation and the SQL scripts committed nothing. No reset or cleanup; container /tmp copies
and the work folder remain.

Not verified: YAML parsing of the contract; live HTTP/browser/phone; main rollout (requires
0018, 0019 and 0020 in order, all still absent from main); previous-image rollback against 0020.

### 13.6 Review repairs (Claude Code, 2026-10-30; web only)

**1. Notification facts validation (`apps/web/src/api/notifications.ts`, callers).**
`isValidNotificationFacts(value, captured, allowNoPartner = false)`:
- `null` (a legacy original without a decision) is valid only when the captured choice was
  omitted or false. A captured true uses its own fingerprint and cannot replay a legacy write.
- `no_partner` is valid only where a partnerless write exists. The anniversary store passes
  `allowNoPartner = true` for create and creator edit. Tasks, messages and replies keep the
  default false, so they treat it as unconfirmed.
- Invalid facts throw inside the existing send path, so the action stays unknown with its
  original key and payload.
- The earlier test that accepted `null` for a captured true was corrected.

**2. Preference freshness (`stores/notification-choices.ts`, `components/NotifyPartnerToggle.vue`,
callers).**
- Each read has a sequence. Only the latest-started read applies (`readySeq`).
- `load()` still coalesces concurrent reads.
- `refresh()` is used after every confirmed write (messages, anniversaries, tasks). It drops the
  older in-flight read so its late response is discarded, and starts a fresh read. It never
  reuses the old promise.
- A new form calls `mark()`: it joins a read in flight at that moment or starts one. It applies a
  default only from a read with `readySeq >= mark`, never from an earlier cached result, so
  changes from another device reach new forms.
- Unchanged: the default is applied once and never over a touched, locked
  (submitting/unknown) or unavailable toggle; a failed read keeps off with a retry; account
  reset clears sequences.

**Tests.** `notification-choices.spec.ts` grew from 12 to 19 tests with deferred-response cases:
- an old read starts, a new choice commits, the old response arrives late and is discarded, and
  the fresh read applies;
- a new form after a remote change ignores the cached default and applies the fresh read;
- a failed read for a waiting form stays off, and its retry applies;
- message `null`-with-true and message `no_partner` stay unknown with key/body/choice, then a
  valid answer resolves;
- task `no_partner` and `null`-with-true stay unknown with the same key and payload;
- anniversary false + `null` resolves, true + `null` stays unknown, and anniversary `no_partner`
  still resolves.

**Results.**
- Affected specs (notification-choices, messages, messages-repairs, anniversaries, tasks-view,
  tasks-store): 129 tests passed.
- Full web suite: 26 files / 333 tests, exit 0.
- `vue-tsc` and node-config `tsc`: exit 0.
- No backend, contract, SQL or migration change. Backend/SQL checks were not rerun.
  0000-0020 unchanged.

**Concurrent-pin report (P2-2): investigated read-only, not confirmed, and refuted by the code
path.**
- `AnniversaryService::pinTarget` takes `pg_advisory_xact_lock(33011, hashtext(user))` in its
  own statement (`kPinLockSql`, AnniversaryService.cc:938) before key classification and before
  `kPinInsertSql` (line 184) computes `COALESCE(max(seq), 0) + 1`.
- Under READ COMMITTED that INSERT takes a new snapshot after the lock wait, so a second device's
  pin by the same user waits and then sees the first one's committed row. The 0018 pin guard
  re-takes the same lock re-entrantly and finds `seq = previous + 1`.
- The earlier concern assumed the sequence was computed before any lock. That was wrong for the
  service path.
- Evidence limits: no concurrent-pin db_gated test exists, so this is code reading, not an
  execution. A direct-SQL insert that bypasses the service lock can still be rejected with
  UN006, which is the guard working as designed.
- To reproduce if wanted: in a db_gated case, start two `AnniversaryService::pin` calls for the
  same user with different keys and targets, wait for both, and expect two `Ok` results with
  seq 1 and 2 and no `Error`. No P2-2 code was changed.
