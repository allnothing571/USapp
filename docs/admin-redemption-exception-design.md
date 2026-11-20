# Admin redemption cancellation (approved minimum, implementation in progress)

Owner approval 2026-11-14: reason/audit/idempotency and explicit confirmation; cancel unfulfilled redemptions, returning
only the original charged flowers if a debit actually happened. No forced fulfillment, duplicate refund, catalog rewrite,
arbitrary task repair or history mutation. Pending confirmation has no debit and no refund. Numbered 0051 is now frozen;
52 migrations total, next number 0052.

## Chosen integration

Keep legacy member event rows and their actor semantics unchanged. Add an immutable admin_redemption_cancellations table
and overlay it into the existing authoritative reward_redemption_state view. A cancellation makes status=cancelled,
cancel_reason=admin, cancelled_at=the admin record time, and clears visible pending cancellation references. Keep all old
view columns and types in order. No fake member event or member actor is inserted. Original debit operation stays visible
as the original debit; the admin refund is its own original fact, not a changed purchase snapshot.

Existing event and fulfillment guards already read this view under advisory lock 33008; the admin path takes that same
lock. Once either party wins a terminal transition, the competing action fails. An existing user cancellation proposal is
retained, but terminal admin cancellation supersedes it; it is neither deleted nor forged as a user's approval. A repeat
of an earlier member action returns its original facts with today's terminal state through the existing replay path.

New ADMIN_REDEMPTION_REFUND wallet operations require actor_admin_id and null actor_user_id. Extend the conditional actor
check and 0050 member-actor guard for exactly this new type; all old missing-member errors stay 23502. Preserve flower
entry bound 999. Original product price is at most 999, so a charged cancellation needs one positive flower entry for the
original redeemer. No refund operation exists for an uncharged pending-confirmation request.

## Transaction and constraints

Active admin -> per-admin/key lock 33033 -> key/fingerprint classification before state -> exact couple/redemption lookup
under 33008 -> reviewed expected_status comparison -> if charged, redeemer flower account lock and current balance cap
check -> immutable audit/parent/refund operation/entry -> named deferred checks -> confirmed commit. Same-key replay
returns the immutable original receipt even after later balance changes. A changed command under that key is refused.
Current state/refund bound refusal leaves the redemption, proposal, audit and wallet untouched. Unknown outcome keeps the
exact key/body for explicit same-command retry.

Parent BEFORE INSERT validates current unfulfilled state, immutable original price/redeemer/couple, request fingerprint,
reason and refund presence. New deferred parent/operation/entry checks prove exact source, amount, actor, audit and target
account and prohibit extra or orphan refund operations. Extend the correction audit immutability guard to this new audit
action as well. No SECURITY DEFINER, mutable wallet balance, unrestricted repair SQL or production operation.

## API/UI implementation

POST /api/admin/v1/couples/{coupleId}/redemptions/{redemptionId}/cancellations, exact expected_status and reason plus key.
Session-derived admin; AdminFilter Origin/CSRF/surface. Review captures original redemption identity/price/status; explicit
second confirmation. Refund value is computed by the server, never user-supplied. Preserve unknown command across route
changes, no automatic retries/new keys. Label original refund balance as historical and reread current data separately.

Extend public/admin cancellation reason validation and display for admin; never expose private admin reason/identity to
ordinary users by default. Admin detail may show the separate admin action/why in addition to legacy member-event history.
No complete admin action may masquerade as a member event in the existing history list. Update old-build compatibility
and rollback release notes: older clients may reject the new cancellation reason, so this is a combined-release change.

## Required proof

Uncharged/charged/direct/request cases; original snapshot after catalog edit; old member cancellation proposal remains but
is terminally superseded; same-key and changed-key races; original debit refunded exactly once to original account; no
refund beyond original price; max-balance refusal atomic; fulfilled/already-cancelled/wrong-couple refusal; admin/member
actor separation; forged/orphan/mismatched refund and audit refusals; concurrent fulfillment and member cancellation in
both orders; discarded confirmed commit reply followed by replay; public read/replay and web labeling; both listeners'
independent admin auth/Origin/CSRF/surface; frontend capture, retry, auth/route race tests and full regression/old ledgers.

Status: SQL, API, admin UI and public cancellation label implemented; complete combined regression in progress.
V1 goal remains active. No real financial action, production access, deployment or commit.

## Evidence and limitations (2026-11-14)

Draft preflight usapp-admin-cancel-draft-de9002de7d refused the PL/pgSQL CASE comparison syntax before test execution;
parenthesized the expression. Second preflight b4ca24a7cb reached the assertions and exposed a wrong test expectation:
the existing member-event guard returns UN001, not UN003. Its exact original state refusal was retained. Final draft
fb52e4e29f passed rollback proof under postgres and usapp_api, including paid/unpaid, duplicate/replay, terminal-state,
immutable audit and six forged/orphan probes. Existing adjustment ledger also passed; privilege deviations zero.

0051_admin_redemption_cancellations.sql SHA-256:
df846c323f905cf5465a91306a0436aabd90495338ad24c7e2c45ddb72878b4c.
Existing 51 migrations untouched. Schema preserves legacy member events and actor requirements. Append-only SQL test
fixtures explicitly include the newly referencing table in their current-schema TRUNCATE list; expected errors unchanged.

First full run usapp-e1-08ee5dd325 passed all 52 migration checksums, grants, new SQL ledger and wallet/product/redemption/
workflow compatibility ledgers under both roles. Build failed because two new live GET helper calls omitted their explicit
null body argument; test-only correction. Next run is in progress and not yet claimed green. Its SQL ledger adds genuine
balance-cap saturation after an original debit and proves the refusal leaves pending state and audit unchanged.

Admin focused 14 tests and full 233/16 files passed; a subsequent canonical-price input guard has focused coverage passing.
Both admin and web typechecks passed. New public reward-label test passed (6 focused tests total). Final full/build evidence
pending. Strict YAML/ref check passed 0.58.0, 128 paths, 251 distinct refs and duplicate-key negative control; not OpenAPI
semantic validation. The new schemas were initially appended below root security and not under components.schemas;
the checker caught the unresolved reference and placement was corrected before the second full run.

Second run usapp-e1-0b06205a03 passed build, target 42/3 cases and live HTTP 1393. Full CTest was 32/33: the new static
key-order check searched the entire migration and accidentally matched the earlier snapshot-trigger read. It now scopes
the comparison to usapp_admin_redemption_cancel. The unchanged production SQL already classifies key before current state.
An ownership-checked compiler-only rebuild passed static 1616/1616 without restarting its database. A fresh combined run
usapp-e1-996556be53 is in progress; do not substitute the focused static pass for a passing full run.

Final frontend: admin full 233/16 files and web full 958/47 files passed; both typechecks and isolated Vite builds passed.
Original admin 20 and web 45 dist files unchanged. Build output: %TEMP%/usapp-cancel-web-d406263f55.

The confirmed-commit fault discards a successful commit callback's service reply. It does not simulate a real half-open
transport. Operator reason and identity are absent from public reads but are visible in the independently protected admin
detail. Older clients may not understand cancel_reason=admin: rehearse a combined-release/rollback before enabling.
No new anomaly notification is emitted yet; that remains an explicit V1 work item.

### 2026-11-15 continuation

Run usapp-e1-996556be53 completed as 32/33, not green: the old journal_cleanup_db late-ack setup refused its fake upload;
new cancellation 42, real HTTP 1393 and static 1616 all passed. One-second test grants are floored to whole-second signed
expiry, leaving almost no setup window near the next second. This is a concrete fixture timing risk, not proof of the
exact last run's cause. Only that late-ack fixture now uses a three-second grant; actual expiry rejection remains enabled,
all assertions and CTest limits unchanged. The shared upload helper now labels expired-grant versus other fake validation
refusals without printing credentials/URLs/keys. Fresh combined run is pending. Product upload policy is unchanged.

On continuation Docker was stopped; the first backup retry failed before resource creation (52a5727fb4). Docker was started
from the verified per-user install path, not the absent system-wide path. This does not count as product acceptance.

Final run **usapp-e1-5f6305059c** passed: all 52 checksums unchanged, privilege deviations zero after apply/repeat/CTest,
both-role cancellation and old compatibility SQL ledgers, build, new target 42/3 cases, live HTTP 1393 and **CTest 33/33 in
105.48 s**. Erasure/financial chain checks zero. Export connection-loss/large/closure 8/6/23 and closed album/memory 7/8
passed; full harness exit 0. The earlier intermittent upload failure remains recorded; longer test setup expiry is not
proof that every historical clock/transport issue is solved. The earlier correction service's diagnostic-only edit was
also compiled and exercised here. Containers stopped and retained, no deployment/production action/commit.
