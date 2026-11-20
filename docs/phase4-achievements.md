# First task achievements — 2026-11-10

## Approved rules

The owner approved these five first badges, no sunshine or flowers, and no retrospective credit before feature activation:

| Code | Badge | Requirement |
| --- | --- | --- |
| first_task | 第一步 | First final task completion |
| task_10 | 积少成多 | 10 final task completions |
| task_50 | 五十次小进步 | 50 final task completions |
| streak_3 | 三日同行 | A run of 3 distinct consecutive Asia/Shanghai dates with final completions |
| streak_7 | 一周足迹 | A run of 7 such dates |

Count only the session caller's `task_completions` in their current couple, with `completed_at >= enabled_at`. Single/count/recurring tasks each count their final completed occurrence once; progress increments, publication, reactions, wallet postings, partner tasks and completion retries do not add counts. Multiple completions on one Beijing date add to the total but not the number of streak days. The streak is the historical longest run since activation; a later rest or gap never removes an earned badge. The first crossing's stored completion instant is earned_at. No current-day/browser-clock comparison and no deadline or existing timestamp-guard changes.

## Implementation boundary

Badges are **read-only projections of the existing append-only final completion records**, not new grants, wallet credits, claim buttons, worker events or persisted award rows. Repeated reads do not create anything. This keeps the approved badge-only batch separate from asset rewards, avoids a completion-path transaction/lock change and makes replays irrelevant to badge issuance. Future task/source erasure or couple dissolution must define how badge history survives; this batch does not invent those policies.

Migration `0047_achievements.sql` adds one global immutable activation row and a pure SQL rule kernel. There is no new per-user mutable state, advisory-lock class, security definer or reward type. SQLSTATE AZ001 protects activation (name searched before use). The application has SELECT on activation and EXECUTE only on the pure kernel; it cannot insert, update, delete, truncate or activate. `init-roles.sql` section 39 restores these exact grants. TaskPrivilegePolicy now requires this read-only table, so a new binary requires 0047.

`usapp_achievement_activate()` is invoker-rights, maintenance-only. First committed use assigns database `clock_timestamp()`; callers cannot supply/backdate a timestamp. Repeat calls preserve the existing timestamp. Migration application and API startup never call it. `database/achievement_enable.sql` is an explicit owner-operated maintenance entry, **not executed against a shared or production database in this batch**. There is no pause/reset/backfill or admin UI toggle. Installing 0047 alone leaves enabled=false, null enabled_at and all progress zero.

`AchievementService` reads membership, cutover, caller/couple-filtered final completion instants and all five badge facts in one statement. `GET /api/v1/achievements` is behind AuthFilter, takes identity exclusively from the session, returns SPACE_NOT_FOUND without current membership and uses the existing no-store JSON response helper. Query strings cannot select another user. Contract 0.45.0 adds `AchievementSnapshot`; the response includes no task ids, task text, other-member details, keys or URLs.

The webpage `/achievements`, linked from “我们”, presents five text/star badges, capped progress, earned dates, total and historical longest streak. It distinguishes disabled, loading, read failure and real zero progress; retry is explicit. Positive copy, no claim/spend controls or art requirements. State remains in memory, is reset by identity lifecycle and page unmount, and ignores late/out-of-order responses. Private API caching policy is unchanged.

## Files

- `database/migrations/0047_achievements.sql`, `database/tests/achievement_test.sql`, `database/achievement_enable.sql`.
- `AchievementService.{h,cc}`, `AchievementController.cc`, controller CMake wiring; TaskPrivilegePolicy and init-roles/deviation matrix.
- `AchievementDbTests.h`, DbGatedTest registration/migration gate, separate `achievement_db` CTest target with unchanged 30 s policy; live HTTP assertions and static boundary checks. Existing three contract-current-version checks advance to 0.45.0; no old behavior assertion removed.
- Web `api/achievements.ts`, `stores/achievements.ts`, `views/AchievementsView.vue`, session reset, router and UsView entry; `tests/unit/achievements.spec.ts`.
- `tools/achievement-verify.ps1`: new copy adapted from the retained B3 harness, with exact container IDs plus owner label, internal network, no published ports, literal isolated usapp_test identity, generated credentials, checksum/privilege checks, rollback ledgers, targeted tests, real local binary and full CTest. Copies current public source directories rather than maintaining a stale per-file list. Does not select existing project containers by broad name filters.
- `tools/achievement-contract-check.py`: local strict YAML parser/duplicate-key negative control and component refs; not an OpenAPI semantic validator.

## Verification status

Implemented and automatically verified in disposable infrastructure. Final source passed; not deployed and not enabled in a real/shared environment.

Completed checks:

- Rollback-only run `usapp-ach-11c99ab39d`: 17 kernel/privilege cases per role; no 0047 objects persisted. The two function-permission checks were added before the full run.
- First full run `usapp-ach-451ac39605`: rollback prevalidation 19 cases per role, committed first application of 0047 (sha256 `75d67669f3ac1ac31bb11fa7fc4282662710fa9137fd5a26729c7fc716edf132`), 48 matching checksums, eraser violations 0, privilege deviations 0 after apply and repeat. Ten SQL ledgers in both roles plus timestamp and push-dispatch ledgers in both modes passed/rolled back. Exact owned-container read during build: activation 0, task completions 0, proving installation/ledgers did not enable or populate achievements. Build passed, targeted achievement 18 / catalog 157 / purchase 114 / loadout 85 and real local API 528 assertions passed. CTest **18/19**, 100.67 s: unit failed 4 assertions because its complete expected-privilege fixture lacked the new table. The fixture was completed and three read-only expectations added, without changing product policy or removing an assertion. The running container also held the achievement test copy from before the last counted-progress/footprint test additions, so those additions are not claimed tested here. The final new disposable run used the complete test files; its passing result is recorded below.
- Web focused 20/20; full 46 files/940 tests, exit 0. Application vue-tsc and Node-config tsc pass; isolated production build in `Claude outputs/ach-web-8f433a70f5/web-dist` passes, existing apps/web/dist hashes unchanged.
- Strict YAML duplicate-key negative control refused; 0.45.0, 104 paths, 121 operations, 184 refs resolve. YAML/ref structure only.

**Final run `usapp-ach-240cb4530c`:** current copied tests include counted progress/final replay, the stronger read footprint and completed privilege-policy fixture. Prevalidation 19 cases per role; 48 checksums match; eraser violations 0; privilege deviations 0 after apply, repeat/InitRoles and CTest. All ten ledgers passed both roles and timestamp/push-dispatch passed both modes, rolling back. Targeted achievement **1 case/22 assertions**, catalog 157, purchase 114, loadout 85 and real local API **528 assertions** pass. **Full CTest 19/19, 97.65 s**: unit 2836, db_gated 3573, media 239, media deletion 376, memory deletion 1048, push dispatch 582, OSS stub 363, static 1338, journal cleanup 147, journal services 4168, album history 97, unattached 58, capability 17, D20 114, reminder services 564, catalog 144, purchase 114, loadout 85, achievement 22; all zero failures. No timeout changed.

Final post-check steps 95–104 all returned zero consistency violations (including exact purchase debit/audit, ledger chain, eligible/audited/contiguous loadouts, early/reversed erasure, keep/live removal and D20 constraints). Step 106's privilege-deviation query returned no rows, exit 0. Exact-owned compiler/PostgreSQL stop steps 108/110 both exit 0. `LastTest.log`, API/PostgreSQL logs and command evidence remain in `%TEMP%/usapp-ach-240cb4530c`. Log scan: zero crash markers and zero hits for the three generated database passwords; not a claim about unknown/non-retained secrets. The tool process handle was unavailable on the follow-up turn; completion was recovered from retained logs rather than a live process poll. Docker was no longer running at that later check and was not restarted. No production state is inferred from this.

0047 is byte-identical to its first committed apply; 0000–0046 match B4's saved 47-file inventory. Next migration 0048 is unreserved. Only documentation/status edits followed the passing tests. The earlier paragraph's pending final run is superseded by this result.

Rule coverage includes exact-microsecond cutover/no backfill, 50 completions on one day, threshold first instants, Beijing midnight rather than UTC, duplicate days/gaps, first seven-day run persisting after later inactivity, input order/null timestamps, repeat activation, time forgery, application write/activation refusal and push activation refusal. Service tests use real task publication/settlement and counted progress/replay; read footprints, partner/other-couple/no-space coverage. Web tests cover consistency validation, disabled state, earned-vs-unearned, explicit retries, identity reset, out-of-order/late results, no persistence and no claim/image controls.

Development artifact: the first Python helper attempted `Path.write_text(..., newline=...)` on the installed older Python, which rejected that keyword before any file write. The helper was corrected; no product behavior or migration changed for that error. An inherited prevalidation output line still said “0046 objects absent” in the first running harness; its actual SQL checks 0047's table/kernel. The saved harness wording has been corrected. A separate inherited case-count display filtered only Avatar B1/B2/B3 names, so it printed zero achievement cases beside the passing 18 assertions; it now recognizes the achievement case name. These are label/filter errors, not failed tests or database identity checks. The earlier 47 migrations were independently compared with B4's saved inventory: 47/47 unchanged.

## Remaining risks / acceptance

No shared/production enablement, deployment or commit. No real browser/device/production-service-worker or provider acceptance. The pure kernel uses all post-cutover completion instants; long-history query plans, memory and latency are unmeasured. Global cutover is irreversible through ordinary roles. Maintenance can, as elsewhere, override triggers as superuser; that is outside application authority. Historical source retention and current-couple scope matter to derived badges. Real concurrency of two maintenance activation calls has not been forced; ON CONFLICT keeps the first row by database semantics. Other achievement definitions, sunshine rewards, partner comparisons and retroactive credit are excluded.

The cutover rule is explicitly about stored `completed_at`, not transaction commit order or a monotonic event sequence. A completion whose decision instant precedes cutover stays excluded even if it commits later; a backward clock or a leading stored event time can complicate the physical ordering around activation. The deterministic timestamp boundary tests do not prove immunity to real clock steps. Production clock behavior remains a release concern; this batch adds no tolerance and does not rewrite task completion timestamps.

Existing release gates and historical clock/DbClient failures remain open. This batch does not change those guards or claim to fix them.
