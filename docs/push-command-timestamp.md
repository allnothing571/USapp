# Push registration command timestamp (2026-11-14)

## Finding and evidence boundary

Run `usapp-e1-f6a24ffdab` failed the push cutover/device re-enable case with `USAPP_PUSH_COMMAND_TARGET`, followed by an
empty UUID error. That guard combined generation existence, user/couple/session identity, session validity and two time
predicates into one error. **The exact predicate in that historical failure was not captured.**

New disposable diagnostics did not repeat that failure: 2,000 default-shaped registration samples were accepted, two
explicit invalid-time label controls were refused, and the instrumented fake-transport suite passed. This does not
invalidate the original failure or prove its cause. A current 35-second native three-clock probe observed one wall-clock
backward step of 886 microseconds; both monotonic clocks stayed forward. Production clocks remain unmeasured.

Controlled-clock tests on the **original unchanged guard** demonstrated both mechanisms:

1. The clock moves backward after the generation was activated: an omitted command timestamp is earlier than its source.
2. The column default is evaluated, then the clock moves backward before the BEFORE trigger checks it: the omitted
   timestamp is ahead of the later clock reading.

Both are legal, omitted-time registration writes, rejected as `UY004 / USAPP_PUSH_COMMAND_TARGET` by the original guard.
This establishes the defect mechanism, not retrospective proof of which predicate failed on 2026-11-13.

## Narrow correction

New migration `0048_push_command_timestamp.sql`, SHA-256
`3854c3f1f73a6ea9710de0ec14d5409c1add3183c6464125f8b19f7c7a2edec9`:

- Drops only `push_registration_commands.recorded_at`'s default. NOT NULL remains.
- After the existing user lock and source/identity/session checks, omitted/DEFAULT/NULL timestamps are assigned once as
  `GREATEST(clock_timestamp(), generation.activated_at)`, following the convention already used in 0041.
- A supplied non-null timestamp retains the strict original source-floor and current-clock checks, with the same SQLSTATE
  and message. There is no tolerance, grace period, sleep or automatic retry.
- The unrevoked check for `already_active`, append-only guards, unique creating-command index, deferred links, locks,
  idempotency classification and stored replay facts are unchanged. Existing rows are not rewritten. No grant is added.

This timestamp is an immutable command fact, not a dispatch cutover. It is read into registration command facts by
`PushService`; dispatch cutover comes from the **separate** `push_dispatch_control` table. Generation activation,
revocation timestamps, dispatch eligibility, leases, TTL and device caps are unchanged. A generated command time can lead
a clock that has moved behind its source; it is a causal floor, not proof of physical wall-clock accuracy or a bounded lead.

The generation and revocation guards retain their earlier default/clock comparisons and can still be exposed to clock
steps. This change does not claim to make the entire registration chain or every timestamp guard immune to clock changes.

The cutover test now checks re-registration success and a nonempty generation before using it. A refusal prints only the
fixed stage and numeric outcome, avoiding the misleading secondary empty-UUID failure. No assertion is weakened.

## Verification

`database/tests/push_command_timestamp_test.sql` uses a session-local controllable clock, rolls back every fixture/catalog
change, and runs under application and maintenance roles. It does not change the operating-system clock.

- Before 0048: five controls per role; both omitted-time paths refused, explicit before-source/future inputs and wrong
  identity still refused.
- After 0048: six controls per role; the two omitted-time paths accepted with their **exact** source/clock maximum,
  explicit invalid times and wrong identity still refused, and valid explicit NULL assigned.
- A final negative-control run temporarily restored the frozen 0022 guard/default **inside the test rollback** on the new
  schema. Both roles reproduced the original refusals; both roles then passed the fixed controls in that same database,
  demonstrating restoration of the corrected guard/default after rollback.

Full disposable run `usapp-e1-e990d56b9b` (internal network, no published ports, generated credentials, fake transports):

| Check | Result |
|---|---|
| Migrations and permissions | 49 checksums match; old 48 files byte-identical; eraser violations zero; privilege deviations zero after apply, repeat apply and CTest |
| SQL clock controls | Six per role passed |
| Existing push subscription ledger | 22 cases per role passed, including original identity, append-only and permission barriers |
| Build / targeted suites | Passed; observer lifetime 78, export 449, existing admin/avatar/achievement suites passed |
| Real local HTTP binary | 942 assertions passed; exported-content marker lines zero |
| **Full CTest** | **27/27 in 98.66 s**; `db_gated` 14.30 s, `push_dispatch` 20.42 s, `observer_lifetime_db` 0.17 s |
| Postconditions | Early/reversed erasures zero; ledger chain breaks zero |
| Export loss / large / closure | 8 / 6 / 23 assertions passed; loss answered `session_lost` and released after 1,033 ms; next export succeeded |
| Full harness | Exit 0, complete; all 49 migration files unchanged during the run |

Final negative control: `recovery-f78a7f47` of the same exact-owned retained containers, with a newly recreated tmpfs database.
It is separate from the full-run dataset. The earlier failed 26/27 run remains failed and preserved; this new run does not
retroactively turn it green. Export's ordinary successful reconnection is not proof that its intermittent transport stall
has been eliminated.

Evidence: `Claude outputs/push-command-time-20261114`. Diagnostic attempts include a stopped Docker engine, one SQL-fixture
CASE-expression syntax error, and one verification-script parenthesis error before resource creation; these were tool/test
errors, fixed without changing product checks. The first instrumented run and its non-reproduction result are retained.

No web or REST contract change (0.52.0), no web rerun, real browser/device/provider acceptance, shared database or cloud
access, deployment or commit. Containers were stopped and retained. The full harness's closure mode ran last on its own
dataset; later rollback-only controls recreated the tmpfs database. Next migration is 0049.
