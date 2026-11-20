# V1 completion ledger (Codex, 2026-11-14)

Owner objective: finish V1, not another isolated read-only page. This ledger supersedes informal estimates; implemented,
automatically verified, manually accepted and deployed are distinct states. Source: app开发方案V1.docx and dated decisions.

## Approved final scope

The owner approved on 2026-11-14:

- Admin asset correction: required reason, explicit second confirmation, signed flower/sunshine delta, absolute total at
  most 9999, no negative resulting balance, immutable audit/idempotency; never update a balance or old entry directly.
- Admin exception handling: cancel/refund unfulfilled redemptions only, with reason, audit and idempotency. No forced
  fulfillment, historical rewrite or arbitrary task repair.
- Minimal complete V1: retain the five approved badges and current avatar assets. Reward/anniversary optional pictures,
  photo-binary export, extra achievements and final-art replacement are excluded from this closeout.

Implementation compatibility: original flower-entry bound is 999, and one operation has at most one entry per account.
A larger approved correction must therefore be an atomic parent adjustment with multiple bounded child operations, not
relaxed legacy bounds or multiple entries for one operation/account. Admin identity must be represented as admin identity.

## Work ledger

| Area | Current evidence / remaining acceptance |
|---|---|
| Core account/space/task/reward/message | Implemented; preserve earlier scoped acceptance. Recheck combined release; old live waivers remain waivers |
| Anniversary/memory/album/shared plans | Implemented and isolated automated checks; consolidated browser/device/provider acceptance pending |
| Notifications and D20 | Implemented; real email/push/device delivery pending |
| Avatar B1–B5 | Implemented; keep current assets; real catalog/default enrollment and combined acceptance pending |
| Five achievements | Implemented; production activation is explicit and still requires release steps |
| User structured export | Implemented. Deadline cleanup now covers lost early and generation callbacks. Run `usapp-e1-7b8024ee15` passed 455 targeted assertions, 1465 HTTP assertions, CTest 36/36 and the isolated PostgreSQL connection-loss probe. A sustained failure to carry rollback or verification can still retain a slot. Browser acceptance pending. |
| Admin reads/catalog | Users/couples/sessions/wallet/tasks/redemptions/notifications/plans/albums/memories/anniversaries/products and avatar catalog implemented |
| Admin wallet correction | Implemented through 0049/0050 and two-phase UI; CTest 32/32, HTTP 1348 and admin 219 tests passed; real acceptance pending |
| Admin redemption exception | Implemented with original-price refunds, independent audit and two-step UI; contract 0.58.0, CTest 33/33 and HTTP 1393; real acceptance pending |
| Account/asset security notices | Approved three-event first version implemented via 0052; backend 34/34, HTTP 1417, web 968 and fake worker 67 passed; no scoring/automatic freeze/debit or real-provider acceptance |
| Admin system capability | Authenticated read-only operational snapshot locally verified (34/34, HTTP 1465, admin 243). Audited operational workflows/feature controls remain separate; no remote shell, raw logs or secret dumping |
| Backup / restore | The unattended Windows job, freshness checks and restore from the checked encrypted replica passed again in disposable run `usapp-backup-probe-6ecd3f3633`. An operator can stage a single retention candidate for review after a replica check. Production storage restore, scheduler activation, live rotation, key custody and off-site destination acceptance remain open. See database-backup-tools.md. |
| Deletion journal | Run `usapp-backup-probe-20730081af` passed joint restore with the fenced publisher consuming committed admission and drain receipts, the separate issuer and the read-only storage observer. Restore withheld the deleted synthetic photo and kept serving closed. Live producer enrollment, provider-backed reconciliation and restore, and release wiring remain open. See recovery-source-bridge.md. |
| Combined release | The running LAN stack matched all 25 frozen 0024 migration checksums and the local release-0024 API image. Isolated 0024 → 0053 migration and replica restore passed with synthetic legacy data. Run `usapp-backup-probe-8adc4daf48` passed candidate → running 0024 image → candidate on the upgraded schema for health, anonymous wallet refusal, wallet and messages. Media and background jobs were off. Current web and admin builds passed. Album rollback, real storage hardening and browser/device acceptance remain open. |

The §36 admin list is phrased as suggested capabilities. This closeout uses the explicitly approved bounded exception paths,
not unrestricted repair SQL, blanket session impersonation or remote host command execution through the web application.

## Execution and evidence rules

1. Complete and verify coherent functionality, then move directly to the next pending row while the V1 goal is active.
2. Add new migrations only. Preserve all 54 already-frozen files through 0053; 0054 is the next available number at this checkpoint.
3. Test fakes/disposable owned infrastructure before any real provider or production action. Never infer acceptance from
   build output or fake-service tests. Record each failure and the actual corrective validation.
4. Capture financial commands once, preserve key/body on uncertain results, and retry only the same command explicitly.
5. Prepare concrete release artifacts and rehearse first. Deployment, production migration, maintenance activation and
   irreversible enablement require a final concrete release review; ordinary development authorization is not deployment.
6. Browser, phone, provider and restore evidence must name the environment and action actually exercised. Missing evidence
   keeps its row open. This ledger must not say V1 complete merely because the automated suite is green.

Historical baseline before correction: contract 0.56.0, 49 migrations, isolated usapp-e1-377da79553 (CTest 31/31, HTTP 1300), admin 203 tests.

2026-11-19 release inventory: the local LAN stack's API health passed with the running 0024 image.
Mail runs in log mode and push is disabled. A separate provider-backed V1 staging environment,
off-site destination, and production backup key custody have not been identified. Chrome control
stopped before navigation because the automation could not validate the current tab URL.
This baseline is local recorded evidence, not a new production inspection or deployment claim.

Historical intermediate checkpoint: asset correction contract 0.57.0 and numbered 0049 (50 files total), before compatibility 0050.

Asset-correction checkpoint: contract 0.57.0; 51 frozen migrations through 0050, next 0051. Run usapp-e1-5e706f9f3d is the passing combined baseline. Earlier failed full runs remain recorded. V1 is still in progress; no deployment or real financial action.

Current checkpoint (2026-11-15): contract 0.58.0; 52 frozen migrations through 0051, next 0052. New passing combined run
usapp-e1-5f6305059c: CTest 33/33, HTTP 1393; admin 233 and web 958, typechecks and isolated builds passed. Backup tools
passed independent synthetic restore in usapp-backup-probe-b413dcc15f with application connections left disabled. This
does not close production, device, provider, off-site or database+object/journal release gates. Next business-rule question
for mandatory financial/account security notices is pending; no risk scoring or notification event was invented.

2026-11-15 notice checkpoint: owner accepted the three initial mandatory personal-security events. Implementation uses
frozen 0052 (53 files, next 0053), contract 0.59.0. See personal-security-notifications.md; full combined verification is
pending, not release acceptance. Earlier pending-question statements are historical.

Final notice checkpoint: usapp-e1-84f38c2569 passed CTest 34/34 and whole harness; 53 migrations, contract 0.59.0, next
0053. Frontend 968, fake worker 67, types/build and independent 53-migration DB restore passed. Personal-security rules
are no longer pending a decision. V1 still needs the bounded operational/admin diagnostics work and the actual
scheduling/key-custody/off-site/object+journal/provider/browser/device/release gates. Old clock-guard intermittency and
transport limitations remain recorded; a green run is not a permanent fix. No deployment/activation/commit.

2026-11-15 memory-clock correction completed: deterministic baseline/fixed rollback controls passed both roles; frozen
0053 adds automatic decision-time assignment after existing checks (54 migrations, next 0054), contract unchanged
0.59.0. Run 2cb34d2d2d passed CTest 34/34 (106.71 s), HTTP 1417 and whole harness; no prior case removed. Independent
54-migration restore f02a7deef0 passed including restored guard/default proof; app CONNECT denied. Other clock guards,
transport limits, operational/journal/provider/device/release gates remain open. See v1-clock-hardening-followup.md.

2026-11-15 admin operational snapshot: additive contract 0.60.0, no migration; current process configuration and recorded
migration count/latest shown with explicit limits. Admin 243 tests, typechecks, isolated build and strict YAML/ref check
passed; full backend 34/34 (105.23 s), HTTP 1465 and whole harness passed in 671835cce9. This does not activate flags or run backup/export/shell commands. See admin-system-status.md.


2026-11-15 deletion startup refusal layer: ab46c00368 passed 35/35, HTTP 1465 and all actual process refusal probes;
independent 41bd74214f passed real blocked-query and missing-function faults, restoration and privilege checks. No
migration/contract change (54/0.60.0, next 0054). This is **not complete J3c or journal integration**: an old backup may
lose CLOSED itself; independent authority and restore-release proof remain mandatory. See deletion-startup-refusal.md.


2026-11-15 journal wire adapter component: C++ Journal adapter/Python injected dispatcher and authoritative-under-contract
validated operation lookup implemented; Python 74, synthetic cross-language 12 requests/15 assertions, full 35/35
(108.95 s), HTTP 1465 and whole harness passed in 169cf84da7. No HTTP/SDK/env factory or main provider wiring; this is
not real durable integration or full J3c. Contract/migrations unchanged (0.60.0/54, next 0054). See deletion-journal-bridge.md.


2026-11-15 journal HTTPS client: final 07bc76b458 passed client 74 assertions/9 cases, CTest 36/36 (112.04 s), HTTP
1465, existing wire/startup/extras and zero postconditions; final source hashes confirmed. No Python service/backend
factory or main wiring. Contract/migrations unchanged (0.60.0/54, next 0054); no deployment/commit or real provider/
recovery release. See journal-https-transport.md; complete J3c/J4/J5 and V1 gates remain open.

2026-11-16 journal HTTPS service: injected TLS-only Python listener, one bounded publication worker, pre-admission
authentication/deadline, strict frame/concurrency limits and separate network/backend drain. Late or disconnected work
retains its slot until local completion; no invented abort/retry. Final 35fb10e7d8 passed Python 92 twice, actual HTTPS
C++/Python 14 assertions (2 operations/4 batches), CTest 36/36 (105.64 s), HTTP 1465 and whole harness; all 619 ordinary
cases preserved. Synthetic keys removed, containers stopped; 0.60.0/54 frozen migrations unchanged. No real cloud backend,
factory/main wiring or recovery release. See journal-https-service.md; V1 release gates remain open.

2026-11-16 restricted SDK backend assembly: final scoped f7a3bf8752 passed Python 125 in both environments, C++ HTTPS
14 assertions through validating metadata with conditional fakes, and full pinned SDK offline 184 reported/4 explicit
legacy local-TLS skips. Independent validation credentials, one-time/latching worker preflight and drained pool teardown
implemented; missing head never initializes. C++/DB unchanged; previous full 36/36/HTTP1465 NOT rerun/reclaimed. Defaults
still block network; no general production configuration/daemon/main/recovery authority. Contract/migrations unchanged;
see journal-backend-composition.md. Production/provider/restore and V1 release gates remain open.

2026-11-16 read-only recovery reader: scoped 0d87082007 passed Python149 twice, pinned SDK188 reported/4 explicit
legacy local-TLS skips, prior publication C++ interop14. Full external chain/index validation, metadata recheck, separate
MAC/context domain, no data writes/initialization/partial response and direct/HTTP leases implemented. C++ recovery
Source/active-incarnation/authoritative-provider proof and release NOT implemented. No schema/contract change; earlier
CTest36/36/HTTP1465 not rerun. See journal-recovery-reader.md. Temporary database CLOSED reports describe the test's
end state only: its tmpfs data disappears on stop; retained containers are not backup/restartable recovery fixtures.

2026-11-16 C++ recovery Source: final2423c0644f passed unit3189/eight new cases, CTest36/36 (106.03 s), HTTP1465,
actual recovery HTTPS21 with fake recovery Store, publication14, Python149 twice, whole harness/zero postchecks. All619
old ordinary cases retained +8=627. Coordinator now checks full restore bindings after close/before final head; no
reopen path. Initial5d75 green predates a signed-NUL syntax regression, fixed in both wire consumers and rerun on the
final full tree. No schema/contract/timeout change, cloud/deployment/commit. Real Source/Postgres recovery drill and
active-authority/release gates stay open; see recovery-source-bridge.md. Latest verified compiler2423 replaces probe-mutated35fb.
