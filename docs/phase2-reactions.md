# Phase 2 P2-4: Lightweight reactions

Status (2026-10-30): owner approved the three Section 7 defaults. Implemented directly by
Codex and automated verification passed on usapp_test (Section 8). Migration 0021 is
applied/FROZEN on usapp_test only. NOT deployed; main remains 0000-0017. Browser/device
acceptance remains deferred, not passed.

Grounding: V1 sections 13 and 25 in `_req_extract.txt`; phase2-plan; frozen migrations
0007/0017/0018-0020; TaskService's public-root/period mapping and 30-day history window;
message_replies single-reply primary key; current P2-3b notification/retry implementation.
0000-0017 are frozen on test/main, 0018-0020 on usapp_test only. Never edit these files.

## 1. Minimal product scope

V1 requires a light emoji or short text response to the partner's task completion, with
no effect on rewards, no nested comments and no unlimited replies. It also allows a light
response to a message, which already has a single-reply implementation in P2-1.

Approved scope:
- One reaction per actual task completion. Only the task creator (the other member), not
  the completer, may react. Both members can read it in their space.
- Choose EITHER one emoji from heart/clap/flower (render ❤️ / 👏 / 🌸), OR one plain-text
  line of 1-40 code points. A closed emoji code avoids inventing a Unicode grapheme parser.
- Task reactions are append-only, with no edit, withdrawal, deletion or response-to-reaction.
  No progress reaction before settlement, no assets, obligations or mutual approval.
- Message quick emoji buttons use the EXISTING reply endpoint and the SAME single reply
  slot. They are not a second reaction channel. Existing text reply limit stays 100; do
  not narrow old API behavior. A recipient chooses a text reply OR an emoji quick reply.
- The author cannot react to their own message. No reactions to replies. UI copy must say
  that a quick emoji consumes the one reply, and use an explicit submit/confirmation button
  rather than sending on an accidental emoji tap.

## 2. Task identity and authorization

`task_completions` is keyed by task_id (0007). A recurring completion uses the stored
child's internal id, but TaskService exposes the root's public id plus period_date.
Therefore public requests identify the root and, for daily/weekly, the EXACT period date.

- Single/count one-off and custom range: root id, no period_date.
- Daily/weekly: root id AND period_date are required. Weekly period_date must be Monday.
  Never default a missing recurring period to the current day/week for a write.
- Resolve the persisted raw tasks row and its task_completions row in the caller's space.
  A virtual period with no completion is not eligible. Partial progress is not eligible.
- Store the completion's actual task_id in the reaction, not just root/date text. The same
  root can have different reactions on different days/weeks without collisions.
- An old completed period remains reactable after midnight or template closure; completion
  is final and immutable. Do not apply completion-action stale-period rules to reactions.
- Keep the existing 30-day history list scope. An exact detail request can still identify
  an older persisted completion; do not claim the list discovers all historical reactions.
- Other space/missing target -> 404; an accessible task with no completed selected period
  -> 409 TASK_NOT_COMPLETED; completer/self reaction -> 403 TASK_REACTION_FORBIDDEN.
  Validate period syntax/schedule matching -> 400 before attempting a write.

## 3. Proposed database and transaction

Implemented migration `0021_task_reactions.sql`: ONE new
table `task_reactions` with id UUID PK, completion_task_id UNIQUE NOT NULL, couple_id,
author_id, kind (emoji/text), emoji_code nullable, body nullable, idempotency_key,
request_fingerprint and created_at. No new generic comment/content parent table.

- FK completion_task_id -> task_completions(task_id). The completion table does NOT have
  UNIQUE(task_id,couple_id); do not invent a composite FK against a nonexistent key.
- Also FK (completion_task_id,couple_id) -> tasks(id,couple_id), plus author membership FK.
  A guard verifies completion.couple_id and task creator/completed_by/author relationships,
  and created_at >= completed_at and <= clock_timestamp().
- CHECK exactly the content fields for its kind: known emoji_code and NULL body, or NULL
  emoji_code and trimmed, single-line 1-40-code-point text using existing character rules.
- UNIQUE(author_id,idempotency_key); fingerprint includes root id, exact period identity
  and normalized content. A different target/content with a used key is key reuse.
- Statement append-only triggers; only SELECT/INSERT to usapp_api; none to PUBLIC/usapp_push.
  Repeated InitRoles, startup policy, deviation query and fixtures include the new table.

Service transaction: membership FOR SHARE + visible target resolution -> key classification
-> actor/completion checks -> INSERT ON CONFLICT DO NOTHING -> classify a skipped insert
again -> same-transaction audit (ids/kind/length, never body) -> read back -> confirmed commit.
PK/unique constraints serialize conflicting reactions; there is no need for another advisory
or wallet lock because completions/terms are immutable. Do not take asset locks or re-enter
settlement services. If completion is still uncommitted, return not completed rather than
materializing or settling a period as a side effect.

Same key/content replays immutable facts, including after the root was closed. Different
key for an already reacted completion -> 409 TASK_ALREADY_REACTED. Concurrent same keys
produce one row/replay; different keys produce one winner and one already-reacted result.
Only confirmed commit reports success; unconfirmed commit is RESULT_UNKNOWN.

## 4. API and reads

Implemented POST `/api/v1/tasks/{taskId}/reaction`, authenticated with exact Origin, session
CSRF and Idempotency-Key. Body is `{kind, emoji_code}` OR `{kind, body}`, with period_date
for recurring targets. Reject unknown/ambiguous fields. Return 201 create / 200 replay,
`{mutation, task}` with immutable reaction facts and current exact-period task state.

Add `reaction` (null or read-only facts) to task list/detail responses. Join task_reactions
on the stored completion's task_id in TaskService, outside the read-model view where
possible; do not change public task ids, history sort/cursors or view column order.
No separate global reaction feed or per-item HTTP requests for each task card.

Message API/database stay unchanged: quick emojis call existing replyMessage with the
actual emoji body, captured optional notify_partner and existing key. Reuse its replay,
single-reply, author authorization, audit and notification guarantees.

## 5. Web, notifications and privacy

Show a small reaction action in a completed task's detail to its creator, or show the
existing reaction to either member. No button for uncompleted/partial-progress rows.
Capture task root id, selected period, kind/content, account and key before awaiting.
Use a separate action namespace/store keyed root+period so task completion/progress/change
unknown actions are never replaced or cleared. Session reset clears reaction state.
Malformed success and later errors after an unknown write preserve the original snapshot;
retry is explicit and same-key. Do not persist or queue actions offline. A stale read may
not erase a confirmed reaction, just as confirmed message replies are append-only.

For messages add the three quick choices beside the existing recipient reply form; make
the shared single-slot behavior explicit. Route/account changes clear local drafts and
late results cannot change another message's view. Existing text replies remain available.

Recommended notification boundary: NO new task-reaction notification event in this first
P2-4 batch; the result appears when the task is read/refreshed. Message quick replies retain
their EXISTING optional reply notification toggle/default. Task-reaction notifications,
if desired now, require an explicit scope addition: a sixth intent operation, new typed
notification source and source guard/deferred link, labels, preference/UI/tests. Do not
silently turn them into mandatory events or store body text in a notification. Push remains
P2-5 and is not a dependency of this interaction.

## 6. Verification and release boundaries

Verification checklist (executed evidence and explicit limits in Section 8):
- Parsing/content/emoji/fingerprint/error mapping unit cases.
- DB: incomplete/partial task refused; creator-only; space isolation; same/different key
  races; exactly one append-only reaction; raw-SQL source/actor/time/shape guards and grants.
- Daily/weekly same root across two stored completions stays distinct; missing period
  rejected; old completed period after rollover still maps to the original completion;
  template closure does not hide completed history or transfer a reaction to another period.
- Replaying reaction changes no wallet operation/ledger/completion count; failing audit
  rolls back reaction; GET creates nothing; old task fields/cursors remain unchanged.
- Message emoji uses the same reply slot: emoji then text and text then emoji reject a
  second response; same-key replay does not duplicate the existing optional notification.
- Web: role/eligibility/content controls, confirmation, captured-period/key retries,
  account/route changes, stale reads, and isolation from pending completion/progress.
- Rollback-only migration/SQL prevalidation, test-only apply + InitRoles/deviation twice,
  fresh test image, full CTest with db_gated, SQL both roles, web suite/typecheck. Extend
  maintenance TRUNCATE fixture dependencies without weakening expected SQLSTATEs.

Existing migrations remain frozen. Any future test migration freezes on first apply;
repairs then need another migration. No main access, served-dist build or deployment is
authorized by this document. Main would need 0018 through the new migration before a new
startup-enforced runtime. Browser/device checks and rollback compatibility remain unverified.

## 7. Owner-approved defaults (2026-10-30)

Approved by the owner with "可以"; implementation and test-database checks authorized:
1. One immutable creator reaction per completed task period: one of three emojis OR text
   1-40 code points; no self reaction, edits, deletes or nested responses.
2. Message quick emoji consumes the existing one-reply slot; no second message-reaction table.
3. No NEW task-reaction notification event in this batch; message quick replies reuse their
   current optional notification control.

A separate text response in addition to the emoji, task-reaction notifications, editing,
or deletion would need a separate scope decision; none was added.

## 8. Implementation and verification (Codex, 2026-10-30)

### 8.1 Files and implemented boundaries

- Database: new `database/migrations/0021_task_reactions.sql` and
  `database/tests/task_reaction_test.sql`; init-roles section 19; the privilege deviation
  query includes the new base table. Task/wallet SQL scripts gain an explicitly guarded
  TRUNCATE branch including this new FK dependency. Expected SQLSTATEs stay unchanged;
  no CASCADE, disabled guard, reset, or weakened assertion.
- API: new `TaskReactionService.h/.cc`; authenticated TaskController reaction route;
  three errors; TaskData/reaction JSON and a correlated stored-task lookup in TaskService;
  startup TaskPrivilegePolicy includes task_reactions. No existing view or task column is
  altered, and no wallet or notification writer is invoked by this service.
- Contract: OpenAPI 0.18.0, additive reaction endpoint, facts, nullable Task.reaction and
  errors. All 472 local component refs resolve; YAML parsing was not performed (no local
  parser installed; no dependency added).
- Web: API/types/errors/rules; independent task-reactions store; TaskReactionPanel in task
  detail; identity-reset registration. MessageDetail quick emojis populate the existing
  reply draft and require explicit submit, with the existing optional notification choice.
- Tests: new TaskReactionUnitTests and TaskReactionDbTests (gate requires 0021, block 28),
  UnitTest and StaticCheck registration, TaskService JSON shape assertion updated from 11
  to 12 fields with reaction null asserted; task-reactions.spec plus a message quick-reply
  test. Existing completion/progress/reward action stores are untouched.
- Reusable verification helper: `Claude outputs/p24-verify-usapp-test.ps1`, stages
  Prevalidate / Apply / Tests. Prevalidate and Apply require the pre-0021 test baseline;
  they intentionally refuse to re-apply against the now-upgraded baseline.

### 8.2 Database state and verification

Frozen migration SHA-256:
`f504ee5f8693be150134a5389bf0fa1e0f9c66a2fbc3623448697f5302846a8e`.
0000-0020 were not edited; registered checksums were checked against local files before
and after application. Earlier 0018/0019/0020 frozen hashes also rechecked unchanged.

1. Initial test state: 21 registered migrations (0000-0020), checksums matched.
2. Rollback-only prevalidation: postgres and SET ROLE usapp_api each passed 12 cases;
   ROLLBACK; afterwards no registration and no task_reactions table. A later SQL-only
   audit-failure subtransaction test increased the final script to 13 cases (below).
3. DryRun: only 0021 pending, expected SHA. Apply+InitRoles exit 0; privilege deviations 0.
   Repeat InitRoles exit 0, 22 skipped/0 applied; deviations 0. All 22 registrations match.
4. Fresh test image builds succeeded. The existing NotificationIntentUnitTests.h:42
   range-loop copy warning remains; no new warning was introduced by P2-4.
5. Final full CTest exit 0: unit 0.03s, db_gated 11.85s, static_check 0.01s;
   3/3 passed, total 11.89s, not skipped. Block 28 covers authorization, incomplete targets,
   space isolation, same-key and different-key races, key reuse, read facts, unchanged
   wallet balances and completion/settlement counts; explicit daily/weekly periods;
   two distinct completed periods older than 30 days per schedule, after root cancellation;
   replay and emoji/text competing for the single message reply slot.
6. Nine SQL scripts, each usapp_api and postgres, exit 0 and ROLLBACK (18 runs):
   reaction 13 cases; task 35 groups; wallet 28; product 8; redemption 6; messages 5;
   anniversaries 3 groups/34 cases; notifications 14 cases; choices 3 groups/17 cases.
   The reaction script checks actor/space/time, shape, duplicate, append-only and grant
   barriers; a failed audit inside a subtransaction leaves no staged reaction.
7. Final web suite exit 0: 27 files / 347 tests, 13.00s. vue-tsc and node tsc exit 0.
   Covers explicit confirmation, captured period/key/payload, unknown followed by 4xx,
   malformed facts (including wrong types), late account-switch results, stale task reads,
   unresolved retry remaining accessible after a read observes a reaction, role/draft
   boundaries and the existing message reply endpoint for quick emojis.

Logs: `%TEMP%/usapp-p24-Prevalidate-20261030_192251`,
`usapp-p24-Apply-20261030_192342`, `usapp-p24-Tests-20261030_193242`.
Generated host/container SQL files and all test fixtures are retained.

### 8.3 Failures and repairs kept on record

- Verification helper initially wrapped stdout in a nested array and falsely reported a
  baseline checksum mismatch. A direct read showed hashes matched. Fixed array return;
  no migration had executed at that point.
- First full CTest: db_gated passed; unit failed the old 11-field Task JSON size assertion;
  static failed 10 multi-line controller matches because CRLF was introduced. Added the
  new field assertion and restored actual LF controller bytes; no assertion removed.
- The expanded historical-period fixture failed because its final progress event omitted
  operation_id. Filled the required link; no schema or business-rule change. A subsequent
  run still exposed the Windows newline translation and was repaired with explicit LF
  byte output. Final full CTest passed with the historical case included.
- A first attempt to write the new web spec used the wrong working directory, creating no
  file; corrected the path. No application/test source failure resulted from that attempt.
- During review, an unresolved action could lose its retry control after a read returned
  the reaction. The facts and retry now coexist; a regression test covers this boundary.

### 8.4 Fixture effects, limits and next boundary

Observed after all runs: 16 committed task_reactions and 16 task.reacted audit rows;
all reaction authors are isolated @m3-fixture.test users. Counts include repeated full
regressions, not one block run. Full db_gated also commits its established task, wallet,
message, anniversary and notification fixtures. SQL/prevalidation fixtures roll back.
No reset or cleanup was performed.

Not verified: live HTTP/controller end-to-end, browser/device/PWA acceptance, injected
lost-COMMIT on the new service, API runtime startup, and previous-image rollback. The
failed-audit assertion proves PostgreSQL subtransaction rollback, not a service-level
fault injection. No new fault switch was added to production code.

No main access/migration, API/Caddy restart/replacement, backend release, served-dist
build, Git operation, dependency/timeout/permission change, or secret/backup read.
Compose consumes .env without reading or printing its contents. Direct Compose commands
use the LAN override; apply.ps1 uses its unchanged postgres exec/cp-only base convention.

Main release, if separately authorized, requires backup/restore/rollback artifacts,
0018 -> 0019 -> 0020 -> 0021 and repeated InitRoles/privilege checks before the new API,
then web publication and deferred acceptance. Do not declare these stages passed.
