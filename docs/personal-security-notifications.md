# Mandatory personal security notices (owner approved 2026-11-15)

Three initial rules approved by the owner: admin wallet corrections notify the corrected user; admin cancellation/refund
notifies the original redeemer (including an unpaid cancellation with zero refund); each newly authenticated session
notifies its own user. No private operator reason or admin identity in notices, location scoring, automatic freeze/debit,
new device fingerprinting, inferred anomaly detector or retroactive import. These are confirmed events and security
reminders; do not label a normal login as detected abuse. A second login in the same browser is another formal session.

## Implementation

0052_personal_security_notifications.sql is additive to frozen 0000–0051. SHA-256:
b8af60e34fa6652ce39961ed319d4786894fdf5ea4d714bd0e10f1cd8cff4dea.
53 migrations total; next number 0053. Draft rollback preflight usapp-security-draft-04037b54c0 passed both roles before
numbering. Existing 52 files unchanged. Contract 0.59.0 adds three closed event types and target kind `account`, preserving
the same seven-field notification response and six-field push payload. There is no new public write endpoint or toggle.

The existing unified notifications/reads/push pipeline is reused. Member actor is NULL for these three system events;
the true administrator remains in its separate original audit. New typed source columns identify the original adjustment,
cancellation or immutable login fact. BEFORE guard derives exact recipient, target, space and time from source rows;
source-specific unique indexes prevent duplicate notices. AFTER source triggers emit inside the source transaction;
deferred links require exactly one notice. Failed or rolled-back source writes leave no notice; original financial-key
replays insert no source row and therefore no extra notice. No remembered optional preference or notification intent.

`account_login_events` records UUIDs and time only, not tokens/hashes, IP, user-agent text or inferred device identity.
The insert guard validates an actual authenticated session and its current user/optional space. Session insertion and
false-to-true promotion capture a fact; re-setting true, CSRF rotation, last-seen updates, logout and preauth do not produce
another notice. Source session UUID is unique. There is deliberately no retention-blocking FK to mutable session/user
rows: deleting a session does not erase evidence. Minimal immutable UUID/time facts can outlive the session/account row;
this does not define an account-deletion or retention product policy. No existing rows are backfilled by the migration.

## Personal visibility and no-space accounts

Only login notices may have a NULL space. All legacy notice families still require a space and the old membership check.
For the three typed personal sources, recipient identity alone controls inbox visibility; a partner cannot list/count/read
another user's notice even in the same couple. Notification reads gain an unconditional id+recipient FK and use null-safe
space comparison, preserving the old time rules. The admin recipient-scoped read uses the same null-safe receipt join.

The inbox route and notification entry are accessible to authenticated users on create/join/invite pages before joining a
space. Other routes keep their previous guards. Account targets open the inbox and offer explicit acknowledgement; they
do not impersonate a reward/member event or invent a session-management page. Fixed copy carries no operator detail.

The worker and service worker accept only the existing exact push envelope plus the closed `account` target; extra
reason, admin_id, session_id or credential fields are refused. Marker and current-authorization rules remain. Actual push
still requires an enabled provider and eligible subscription; no-space users have no existing space-scoped subscription,
so their mandatory notice remains in-app. A new subscription does not backfill old login notices. Database row uniqueness
does not promise exactly-once device display; existing at-least-once transport limitations remain.

## Validation checkpoint

Both-role draft proof covered no-space/preauth/formal login, source/actor/recipient/target/space/time forgeries, private read
receipts, cleanup/touch behavior, financial replay and transaction rollback. Final standalone ledger also covers formal
promotion and repeated true assignment, passed under postgres and usapp_api in the owned current test run.

Frontend focused 76; full web 967/48 files; both typechecks passed. Worker 67/12 suites passed using fake transports and
fresh temporary output linked to existing installed dependencies; no provider request. First isolated worker attempt had
41 passes and two module-loading failures because its external output lacked package resolution, not failed assertions;
adding an owned dependency junction and module package marker fixed the harness, no dependency installation or dist edit.
Contract YAML/ref and duplicate-key checks passed (128 paths, 251 distinct refs), not full OpenAPI semantic validation.

First combined run dd16d6cb3d passed schema/grants/clock controls, then the old push-subscription ledger's zero-notification
fixture assumption failed because five authenticated fixture sessions now legitimately emit login notices. The check now
captures the notice baseline after login fixtures and proves subscription operations change it by zero; no failed assertion
was removed. The existing D20 no-partner assertion similarly scopes the three D20 event types instead of denying a new
mandatory login notice. A fresh full run is in progress, not yet claimed green. Combined release is required: old clients
can reject actorless events or the account kind. No deployment, commit, real browser/device/provider acceptance or claim
that V1 is complete.

Further validation attempts: bcbffb3b4b reached the admin-notification fixture and exposed its assumption that only two
message notices existed; the real formal session now adds a third login notice. Assertions now explicitly verify that
extra row while retaining original message identity/order, five-of-six bounded push metadata and unchanged footprint.
a6ada2f7e8 passed the expanded live security case but failed the old export footprint assertion by one: its no-space login
fixture was created after the baseline. Move that setup before the unchanged complete footprint assertion; export is
still required to create nothing. No source migration or runtime control was changed for these fixture corrections.

Review also found an invalid initial service-test result: the test process had no configured auth TTL (zero), and an empty
session result returned before exercising the substantive checks. The test now explicitly configures/restores a one-hour
TTL and asserts a nonempty result before proceeding. Do not count the earlier one-assertion PASS as login-service proof.
This is separate from the both-role SQL and real-binary login-notice checks, which did run. Fresh combined proof pending.

Backup regression usapp-backup-probe-d555d4355a passed 53-migration independent restore, including the new NULL-space login
notice and its read receipt exactly once. Financial fixture, erasure owner, chain, privilege and denied CONNECT checks
passed; target remained non-serving, test keys removed and containers stopped. This is still not object/journal or live
provider acceptance. Final isolated web build passed at %TEMP%/usapp-security-web-3313511038, original 45 dist files
unchanged. The added route-guard and no-space UI checks passed (18 focused); no production or device actions.

## Final local verification

Run **usapp-e1-84f38c2569** passed: 53 exact migration hashes unchanged; role deviations zero after apply/repeat/CTest;
both-role security and compatibility ledgers; build; security target **32 assertions / 2 cases**; real HTTP **1417**;
**CTest 34/34 in 112.32 s**. Erasure/financial chain checks zero. Export loss/large/closure 8/6/23 and closed album/memory
7/8 passed; whole harness exit 0. Generated containers stopped and retained. The earlier ec0c2a5f0a run was 32/34: two
superseded exact-list static assertions plus an old USAPP_MEMORY_DECISION_TIME failure. Static lists now match the closed
three-event/account extension; the memory guard was not changed. Its intermittent failure remains a separate release
issue, not fixed or waived by this passing run; see v1-clock-hardening-followup.md.

Final frontend **968 tests / 48 files**, both typechecks and isolated Vite build passed. It also rejects personal notices
whose target user differs from the current account or whose target kind is a content page. Original 45 web dist files
unchanged; final build %TEMP%/usapp-security-final-web-3c042cd847. Fake worker **67 / 12 suites** passed; no installed
dependency or existing worker dist change. Contract strict YAML/ref validation passed 0.59.0, 128 paths, 251 distinct refs,
including the three event enum values and account target; not full OpenAPI semantic validation. Evidence allowlist:
Claude outputs/security-notices-20261115, excluding env files, private keys, dumps and raw server logs.

Owner-approved first-version personal notices are implemented and locally verified. No deployment, commit, real browser,
device or provider acceptance. Remaining V1 operational/release rows stay open.
