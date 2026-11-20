# Timestamp-guard failures: investigation and proposed design

Status (2026-11-05): **implemented and verified in disposable instances only; see Section 9 for the approval, final semantics, evidence, deviations and remaining risks.** 0041 is frozen there, not on shared usapp_test or production. NOT DEPLOYED. Sections 1-8 are kept as the investigation and proposal record; where they differ from Section 9, Section 9 is what was built.

## 1. Conclusion

**Reproduced:** in this Docker Desktop/WSL2 environment the wall clock steps backwards, and a step makes the investigated guards refuse production-shaped writes, through both time predicates, in a tight SQL loop.

**Not proven:** that a step caused any of the four recorded test failures. The timing fits, but none was caught with the instrument on, and no real service path refused during instrumented runs.

| Question | Answer | Standing |
|---|---|---|
| Does the wall clock step backwards here? | Yes: 0.55-2.6 ms, every 30 s, monotonic clocks unaffected | Observed |
| Can a step make each investigated guard refuse a production-shaped write? | Yes: both time predicates, on all three guards | Reproduced, tight SQL loop with real guards and defaults |
| Did a step cause the four recorded failures? | Consistent with the timing | **Unproven:** inferred, none caught |
| Which predicate failed in each recorded failure? | Not known. Only likelihoods (Section 4.3) | Not observed |
| Are fixtures the cause of the recorded failures? | No: all four were service statements with no supplied time | Observed in retained logs |
| Is timestamp assignment order the cause? | Not on its own. It sets the size of the exposed windows | Observed |

## 2. The clock

Three-clock probe (`Claude outputs/clock-dual-probe.c`), no database, in a separate container with no network, unpinned and pinned to one CPU.

- 55 distinct steps in run `usapp-tgi-20261105212931-bea48e`: median interval 30.001 s, size 548-2560 us, median 1373 us. An earlier run (`usapp-tgi-20261105211325-0d5704`) saw up to 2599 us.
- `CLOCK_REALTIME` steps. `CLOCK_MONOTONIC` and `CLOCK_MONOTONIC_RAW` did not move backwards at any step. No CPU change occurred at any step, and the pinned and unpinned probes report the same step in the same microsecond, so this is not TSC skew between CPUs.
- Steps drift slowly through the 30 s cycle: phase about 17.005 s at 13:15 UTC, 17.077 s at about 14:08 UTC.
- Kernel `6.18.33.2-microsoft-standard-WSL2`, clocksource `tsc`. `adjtimex` reports state 5 (`TIME_ERROR`), status 64 (`STA_UNSYNC`) and a frequency near 2.9 million, about 44 ppm. 44 ppm over 30 s is 1.3 ms, close to the median step. **Inference:** the clock runs fast and something sets it back every 30 s. What does this was not identified.
- `clock_timestamp()` reads `CLOCK_REALTIME`, so PostgreSQL sees this directly: the same steps appear in tight database sessions.

This corrects the earlier sleeping probes ("0 backward steps"), which could not see these steps.

**Production is unmeasured.** Nothing here says production steps at all, or by how much.

## 3. When each timestamp is assigned

The investigated columns are `DEFAULT clock_timestamp()`. No production write supplies them (Section 6.6).

1. The source row gets its time from its own default, in an earlier statement.
2. Service reads and locks follow: same transaction for notifications and intents, a later transaction for redemption events.
3. The insert statement starts.
4. `NEW.created_at` is assigned by the default as the row is built, **before any BEFORE trigger**.
5. The BEFORE trigger runs. `usapp_redemption_event_guard` takes advisory lock 33008 here, then reads the source.
6. The predicate reads `clock_timestamp()` again.

Measured on real service paths (20 rounds of the real suites, role `usapp_api`, query not naming `created_at`), in microseconds:

| Guard | Rows | Source to new: min, p50, p99 | New to guard: min, p50, p99 |
|---|---|---|---|
| notification | 10000 | 507, 3252, 22122 | 5, 36, 1085 |
| intent | 4407 | 892, 1850, 9477 | 217, 282, 746 |
| redemption event | 360 | 2722, 10123, 61713 | 53, 318, 1121 |

**Lock wait, measured:** with lock 33008 held by another session, `created_at` was assigned 423 us into the statement and the guard's clock read came 1019135 us later. So `created_at` records arrival, not the time the lock was obtained. A wait widens the new-to-guard gap, which makes `created_at > clock` harder to fail. It cannot explain a refusal.

A step refuses a row only if it lands inside one of those gaps and is larger than that gap.

## 4. Failures

### 4.1 Reproduced guard failures (run `usapp-tgi-20261105212931-bea48e`)

Instrumented copies of the guards (`Claude outputs/clock-guard-instrument.sql`) are built by exact text replacement from the installed source. The only change to acceptance is that the predicate's clock is read into a variable just before the `IF`. A refusal is logged with its predicate and microsecond differences only. Positive controls named every predicate correctly.

Tight loop (`Claude outputs/clock-guard-hammer.sql`): 8 sessions, 600 s, as `usapp_api`, real defaults, every trial rolled back, running only in a 0.3 s window around the predicted step and a 0.3 s control window 15 s away.

| | Trials | UN102 notification | UN104 intent | UN001 event | UB002 message | UM002 redemption |
|---|---|---|---|---|---|---|
| Step window | 36768 | 50 | 39 | 14 | 1 | 4 |
| Control window | 35690 | 0 | 0 | 0 | 0 | 0 |

Exact failed predicates of the instrumented refusals:

| Guard | `NEW.created_at < source` | `NEW.created_at > clock_timestamp()` |
|---|---|---|
| `usapp_notification_guard` | 11 (source to new -2479 to -463 us) | 39 (new to guard -2345 to -189 us) |
| `usapp_notification_intent_guard` | 8 (-2494 to -874 us) | 31 (-1895 to -403 us) |
| `usapp_redemption_event_guard` | 11 (-1587 to -633 us) | 3 (-1570 to -709 us) |

103 of 103 refusals fell within 5 ms of a step seen by the probe. All 21 steps in the window produced at least one. No identity predicate fired, and no refusal was unexplained.

**Limit:** this loop's gaps are shorter than a service path's. It proves the mechanism on these guards, not that a given test failed this way. `USAPP_MESSAGE_TIME` and `USAPP_REDEMPTION_TIME` were counted by SQLSTATE and message only; those two guards were not instrumented.

### 4.2 Not reproduced: the real service paths

20 rounds of `db_gated` and `push_dispatch` with the instrument on: **0 time refusals.** Every refusal logged was a deliberate identity case. From the measured gaps the expected rate is about 0.012 per round, 0.24 in 20 rounds (`Claude outputs/clock-guard-expected.py`), so 0 neither supports nor undermines the mechanism.

### 4.3 The four recorded failures: cause unproven

Every retained log was scanned (`Claude outputs/clock-guard-history.py`). Each row below is a real service statement with no explicit `created_at`, which rules out fixture timestamps:

| Time (UTC) | Phase in 30 s | Test | Guard |
|---|---|---|---|
| 08:15:46.535 | 16.535 | P2-3b direct SQL guards | intent |
| 09:04:16.615 | 16.615 | M9 rejected and withdrawn | redemption event |
| 12:30:46.961 | 16.961 | P2-2 proposals | notification |
| 12:31:16.962 | 16.962 | P2-5b four live leases | notification |

The phase of the steps measured later continues this trend: 17.005 at 13:15. The last two are 30.001 s apart, the step interval. For comparison, 122 deliberate refusals in the same logs spread evenly over the 30 s cycle (5 s buckets: 23, 19, 26, 22, 17, 15).

This is strong timing evidence. It is **not proof**: no step was observed at the moment of these failures, because the probe that can see steps did not exist then.

**Which predicate:** not observed; they predate the instrument. From measured gaps: notification 55% before-source, 45% after-clock; intent 46%, 54%. M9 is most likely `created_at > clock`, because its shortest measured source gap (2722 us) exceeds every step measured (largest 2599 us). All of this is inference.

**Correction to the earlier record:** the run with P2-5b also failed P2-2 in `db_gated` (3 assertions), which earlier reports and design Section 15.7 omitted.

### 4.4 Not explained by this

- **P3-1a delayed copy** (06:58:22-24 UTC): away from the phase, with no guard error. Open.
- **`DbClient` teardown crash:** recurred in round 8 of 20 (`[CRASH] std::terminate: Resource deadlock avoided`, during task completion R8). A process abort, not a refusal. Open.

### 4.5 Effect on a user

A refusal rolls back the whole transaction, so data stays consistent. The user sees a failed request for an operation that was valid. A retry with the same idempotency key should be safe; that comes from reading the design and was not tested against a refusal of this kind.

## 5. Cause classification

- **Clock movement:** the demonstrated cause of the reproduced refusals. The recorded failures fit it by timing only.
- **Timestamp assignment order:** not a cause by itself, but it is why the guards are exposed: a time assigned earlier is compared with a later reading of a clock that is not monotonic.
- **Fixtures:** excluded for the four recorded failures. Fixture timestamps were kept separate by role and by whether the query names the column, and produced no unexplained refusal.
- **Other:** no identity or unexplained refusal was seen in the reproduction. An unobserved cause of the recorded failures cannot be excluded.

## 6. Proposed correction (revised)

### 6.1 The chain

Direct source-to-consumer links for timestamps a 0041 guard would assign. "Floor" means the timestamp the consumer must not precede.

| Consumer guard | Consumer column | Floors | Who reads the column next |
|---|---|---|---|
| `usapp_message_guard` | `messages.created_at` | none | reply guard, notification source, intent source, list order |
| `usapp_message_reply_guard` | `message_replies.created_at` | `messages.created_at` | notification source, intent source |
| `usapp_redemption_guard` | `reward_redemptions.created_at` | `reward_product_revisions.created_at` | event guard, fulfillment guard, notification source, list order |
| `usapp_redemption_event_guard` | `reward_redemption_events.created_at` | redemption time, and the proposal's time for a decision | later decisions, notification source, state view |
| `usapp_redemption_fulfillment_guard` | `reward_redemption_fulfillments.fulfilled_at` | redemption time | notification source (`fulfillment` family) |
| `usapp_notification_guard` | `notifications.created_at` | `occurred_at` of its source | read guard, delivery guard, attempt guard, outcome guard, dispatch queries, inbox order |
| `usapp_notification_intent_guard` | `notification_intents.created_at` | `occurred_at` of its source | nothing compares it |
| `usapp_push_delivery_guard`, timestamp part | `push_deliveries.created_at` | `notifications.created_at` | outcome guard, recipient index |

Notifications also take floors from sources outside 0041: tasks, anniversary revisions, task and anniversary proposals and decisions, and reminder occurrences. Those guards are unchanged. They can still refuse a write as they do today, and their values are only read as floors.

### 6.2 Separating ordering from eligibility, TTL and deadlines

0041 changes **timestamp assignment and source ordering only**. Every rule below keeps its text, its comparison, its clock read and its error.

| Rule | Where | In 0041? |
|---|---|---|
| `NEW.created_at > clock` and `NEW.created_at < notice_at` on a delivery | delivery guard | **Yes:** ordering, and only when the caller supplies no time |
| `notice_at < activated` | delivery guard, fan-out | No: eligibility |
| `notice_at < ctl.cutover` | delivery, attempt and outcome guards, dispatch queries | No: eligibility |
| `notice_at <= clock - 24 hours`, and the fan-out, claim, expiry and pre-send lifetime checks | delivery and attempt guards, `PushDispatch.cc` | No: TTL |
| `LEAST(n.created_at + lifetime, stale_at)`, remaining TTL, absolute expiry | `PushDispatch.cc`, push worker | No: TTL |
| `s.status <> 'pending_fulfillment'` and event state checks | fulfillment and event guards | No: eligibility |
| Reminder window: `created_at < fire`, `>= stale`, `clock >= stale` | `usapp_reminder_occurrence_guard` | No: deadline |
| `closed_at < deadline_at` | `usapp_task_closure_guard` | No: deadline |
| Undo and purge windows, `usapp_media_undo_open`, `usapp_memory_erase` | media and memory guards | No: deadline |

**An unchanged rule can still see a different input.** `notifications.created_at` is the input to the eligibility and TTL rules above, and 0041 can make that value a floor rather than a clock reading. So, for a notice ahead of the clock:

- its lifetime and absolute expiry end correspondingly later on the clock;
- it can meet `notice_at >= activated` or `notice_at >= cutover` for a subscription or cutover recorded after the notice really happened, while the clock is behind it;
- `remaining_ttl` can exceed the configured lifetime. Once the excess reaches a whole second, a 24 h lifetime gives `ttl_seconds` above 86400, which `parseDeliverRequest` rejects.

A row written just before a backward step already has these effects today. 0041 adds notices that would otherwise have been refused and rolled back. A reminder's `stale_at` is not affected, because `LEAST` still applies it. None of this was tested.

### 6.3 What the algorithm guarantees

For a row the guard assigns: `T = GREATEST(c, f1 ... fn)`, where `c` is one clock read by the guard and `f1 ... fn` are that row's declared floors, as read in that statement.

**Guaranteed, for any number and size of backward steps:**

1. `T >= fi` for every declared floor.
2. `T >= c`.
3. `T` is exactly `c` or exactly one of the floors. No margin or arithmetic is applied.
4. Along a declared chain, times never decrease.
5. A root row with no floor has `T = c`, with no ordering guarantee relative to other rows.

**Not guaranteed:**

- **There is no bound on how far `T` can lead the clock.** The earlier claim of "at most one step" was wrong. Backward steps can accumulate: a chain keeps the largest value it has seen while the clock falls further behind it, so the lead is whatever the distance is between that value and the current clock. It depends on how far the clock has fallen, not on any one step, and the algorithm does not limit it. This environment's observed steps do not supply a guarantee either.
- `T` is not a true time of occurrence, and two rows with no declared link can be in either order.
- The lead does not end at any set time. It ends when the clock passes `T`, if it does.

### 6.4 Omitted, DEFAULT and explicit NULL

Once a column has no default, three forms reach the BEFORE trigger identically: omitting the column, writing `DEFAULT`, and writing `NULL`. **All three are the same request: the database assigns the time.** The guard cannot and need not tell them apart.

This changes one behaviour: an explicit NULL is refused today by `NOT NULL` (`23502`), and would be accepted. It gives the caller no control over the value. No test expects `23502` for these columns: the `23502` cases in `WalletLedgerDbTests.h` are wallet tables.

With triggers disabled (`session_replication_role = replica`) nothing assigns the time, so all three forms fail on `NOT NULL`. That is fail-closed, but it is a change for omitted forms, which succeed today through the default.

### 6.5 Exact proposed 0041 scope

**One additive migration. Eight guards and eight columns:**

| Table.column | `DROP DEFAULT` | Guard, last installed by | Floor expression |
|---|---|---|---|
| `messages.created_at` | Yes | `usapp_message_guard` (0017) | none: `clock_timestamp()` |
| `message_replies.created_at` | Yes | `usapp_message_reply_guard` (0017) | `m.created_at` |
| `reward_redemptions.created_at` | Yes | `usapp_redemption_guard` (0016) | `r.created_at` |
| `reward_redemption_events.created_at` | Yes | `usapp_redemption_event_guard` (0016) | `s.created_at`, plus the proposal's `created_at` for a decision |
| `reward_redemption_fulfillments.fulfilled_at` | Yes | `usapp_redemption_fulfillment_guard` (0016) | `s.created_at` |
| `notifications.created_at` | Yes | `usapp_notification_guard` (0025) | `facts.occurred_at` |
| `notification_intents.created_at` | Yes | `usapp_notification_intent_guard` (0020) | `facts.occurred_at` |
| `push_deliveries.created_at` | Yes | `usapp_push_delivery_guard` (0023), timestamp part only | `notice_at` |

`usapp_message_reply_guard` and the delivery guard's timestamp part are in because they consume assigned times and were otherwise left exposed. `usapp_redemption_fulfillment_guard` is added beyond the requested list because it sits inside the chain: it consumes a redemption's time and is the source of a notification's. The owner may remove it, at the cost of leaving that link exposed.

**Deliberately not in 0041:**

- `usapp_notification_read_guard` and `usapp_push_outcome_guard`. These are terminal: nothing uses their times as a floor. They keep today's predicates, and a notice or delivery ahead of the clock can make them refuse until the clock passes it. A read can be retried by the user, and the outcome paths run again on later scheduler ticks (from reading the code). This is a known remaining exposure, for the owner's review, and regressions 11 and 12 pin it down.
- `usapp_push_attempt_guard`: `claimed_at` is supplied by the claim statement, not assigned, and it has no source floor. Its lease check is separate.
- The other trigger functions in the static candidate list (43 in total, `Claude outputs/clock-guard-inventory.py`), including all deadline guards.

**0041 must not** contain any interval or tolerance on these comparisons, alter any rule in Section 6.2, backfill or rewrite any row, or add a lock class. The guards stay without a fixed `search_path`, as installed; a change there would need another way to test (regression 1). The next migration after it would be 0042.

### 6.6 Compatibility: writers and maintenance SQL

Reviewed by searching the repository. Not run.

| Area | Result |
|---|---|
| Service writers: `MessageService` (2), `NotificationIntent` (2), `NotificationWriter`, `ReminderGeneration`, `RedemptionService` (3), `PushDispatch` fan-out | None supplies the column. Compatible |
| `PushDispatch` outcomes and `NotificationService` reads | Outside 0041, defaults kept |
| Migrations 0000-0040 | No data insert into these tables |
| `database/apply.ps1`, `database/init-roles.sql`, `database/memory_purge_enablement_check.sql`, `deploy/docker-entrypoint-initdb.d/01-init-roles.sh` | No write to these tables. The enablement check is read-only |
| Startup policy and scripts | No reference to column defaults (`column_default`, `pg_attrdef`, `atthasdef`): none found |
| Backup tooling in the repository | No `pg_dump` or `pg_restore` found. A full restore loads rows with their stored values, so it does not depend on these defaults |
| Fixtures supplying a time: `message_ledger_test.sql:104`, `MessageDbTests.h:199,251`, `notification_ledger_test.sql:56,60` (reads, outside 0041), `IntentDiagnosticsDbTests.h:82` | Keep the original predicates |
| Fixtures omitting a time, triggers on: `push_dispatch_pre0024_fixture.sql` and the other SQL ledgers | Compatible. The pre-0024 fixture runs before 0041 exists |
| Fixtures with triggers off: `IntentDiagnosticsDbTests.h:82` is the only one writing these tables through `asMaintenance` or a `replica` statement that the search found | It supplies `created_at`, so it is compatible. **To recheck at implementation**, because this was a pattern search |
| Older build on a 0041 database | Compatible by reading: older builds omit the columns too. Not tested. Older-build rollback remains unrehearsed |
| SQL outside this repository (runbooks, one-off maintenance) | **Not reviewed: it could not be.** Anything inserting into these tables with triggers disabled and relying on a default will now fail. The owner should confirm |

### 6.7 Revised guard semantics

Common form, where `<floors>` is from Section 6.5. Identity, source-found and state checks keep their current position, text and errors.

```sql
IF NEW.created_at IS NULL THEN
    NEW.created_at := GREATEST(clock_timestamp(), <floors>);
ELSIF NEW.created_at < <floors> OR NEW.created_at > clock_timestamp() THEN
    RAISE ...;  -- original predicate, original error
END IF;
```

No interval appears, and a supplied value is refused however close it is.

**Guard-specific points:**

- **`usapp_message_guard`:** no floor. A NULL is assigned; a supplied value keeps `> clock` and `USAPP_MESSAGE_TIME`.
- **`usapp_message_reply_guard`:** remove `NEW.created_at IS NULL` from the early `RETURN NEW` only. The other columns' NULLs still return early for their own constraints. The actor check stays first with `USAPP_MESSAGE_REPLY_ACTOR`; then the common form, with `USAPP_MESSAGE_REPLY_TIME`.
- **`usapp_notification_guard` and `usapp_notification_intent_guard`:** capture `FOUND` first, assign if NULL, then run identity checks unchanged. The time predicates apply only when the caller supplied the value. Errors stay `USAPP_NOTIFICATION_SOURCE` and `USAPP_NOTIFICATION_INTENT_SOURCE`.
- **`usapp_redemption_guard`:** common form at the existing position, `USAPP_REDEMPTION_TIME`.
- **`usapp_redemption_event_guard`:** common form after lock 33008 and the actor check, so the time follows the lock holder. The floor adds the proposal's time, read by `NEW.proposal_id`. The existing `NEW.created_at < p.created_at` in the decision branch stays and cannot fail for an assigned time. Error `USAPP_REDEMPTION_EVENT_TIME`.
- **`usapp_redemption_fulfillment_guard`:** split the combined `IF`. `s.status <> 'pending_fulfillment'` stays with `USAPP_FULFILLMENT_STATE`, then the common form with the same error. The floor is the redemption time only, as today.
- **`usapp_push_delivery_guard`:** eligibility and TTL stay first, unchanged:

```sql
IF notice_at < activated OR notice_at < ctl.cutover
   OR notice_at <= clock_timestamp() - interval '24 hours' THEN
    RAISE ... 'USAPP_PUSH_DELIVERY_ELIGIBILITY';
END IF;
IF NEW.created_at IS NULL THEN
    NEW.created_at := GREATEST(clock_timestamp(), notice_at);
ELSIF NEW.created_at > clock_timestamp() OR NEW.created_at < notice_at THEN
    RAISE ... 'USAPP_PUSH_DELIVERY_ELIGIBILITY';
END IF;
```

### 6.8 Prototype evidence, and its limits

Run `usapp-tgp-20261105220229-27a383`, `Claude outputs/clock-guard-prototype.sql`, on a disposable database only.

| Check | Installed guards | Prototype |
|---|---|---|
| Fake clock, both step shapes, three guards (6 cases) | 6 refused, predicates named in the log | 6 accepted, never before source |
| Supplied past or future time (6 cases) | 6 refused | 6 refused |
| Real steps, tight loop | 108 refusals in 36768 step-window trials over 21 steps | **0 in 22454 step-window trials over 11 steps**, 0 in 21374 control trials |
| `db_gated`, `push_dispatch` | Passed in 39 of 40 instrumented runs; 1 `db_gated` crash (Section 4.4) | Passed, one run each |
| `notification_ledger_test.sql`, `notification_choice_test.sql`, both roles | Not run in these runs | Passed |

**This prototype predates the revision.** It covers messages, redemptions, events, notifications and intents. It does **not** cover `usapp_message_reply_guard`, `usapp_redemption_fulfillment_guard`, `usapp_push_delivery_guard`, explicit NULL, repeated steps, or the eligibility and TTL interaction. Nothing has run through the full CTest set, other ledgers, the real API binary or the upgrade path.

### 6.9 Deterministic regressions for the complete chain

All are in a new SQL ledger run under both roles and rolled back, using a fake clock (a function ahead of `pg_catalog` on the session `search_path`), so none needs a real step. Each must fail against the installed guards where it asserts acceptance.

**Ordering, each of the eight guards:**

1. Clock behind the row, and clock behind the source: accepted, `T >= floor`, and `T` equals the clock reading or the floor exactly.
2. Omitted, `DEFAULT` and explicit NULL all give an assigned time.
3. A supplied past or future time is refused with the original error, however close.
4. Wrong identity, actor or state is refused with the original error, including when the time is NULL.

**The whole chain under repeated steps:**

5. Message to reply to notification to intent, with the clock stepped back before each write and further each time: all accepted, times never decrease, and each child equals its floor while the clock stays behind. The test asserts the lead **grows** past any single step, which pins down Section 6.3.
6. Redemption to event proposal to event decision to notification, the same way. A decision is not earlier than its proposal.
7. Redemption to fulfillment to notification, the same way.
8. Notification to delivery, the same way.

**Rules that must not move (Section 6.2):**

9. Delivery with a NULL time: a notice before `activated`, before `cutover`, or at 24 h is still refused with `USAPP_PUSH_DELIVERY_ELIGIBILITY`; just inside each boundary is accepted. Disabled control is still `USAPP_PUSH_DELIVERY_DISABLED`.
10. `usapp_reminder_occurrence_guard`, `usapp_task_closure_guard`, the attempt guard and the media and memory deadline guards have source identical to before 0041, by checksum. Existing deadline tests are unchanged and pass.
11. Read guard: for a notice ahead of the fake clock, a read is refused with `USAPP_NOTIFICATION_READ`, then accepted once the clock passes it. This records the known exposure.
12. Outcome guard: the same, with `USAPP_PUSH_OUTCOME_TIME`. The `cutover_skipped` check is unchanged.

**Other:**

13. Lock order: an event or fulfillment inserted during a held 33008 wait gets a time after the wait.
14. Triggers off: all three NULL forms fail with `23502`, and a supplied value is stored.
15. Static check: 0041 drops exactly the eight defaults, holds no interval or tolerance in these comparisons, adds no fixed `search_path`, and adds no lock class.
16. Mutation: removing `GREATEST`, or removing the proposal floor, must fail regressions 1, 6.
17. Isolated harness, not CTest: the windowed tight loop extended to the chain must report 0 refusals on the eight guards, and "inconclusive" if the probe saw no step.
18. The full existing regression sequence, unchanged, plus the older-build check.

### 6.10 Rollout and rollback points

A build needs no code change, because no production write supplies these columns. Forward-only, like earlier migrations. Dropping 0041 by hand would need the defaults and the original guard bodies restored together; that is not rehearsed. Rows written under 0041 are valid under the original predicates only if the clock has passed them, which matters only for inserts, as these tables are append-only.

## 7. Not proven

- That any of the four recorded failures was caused by a step, and which predicate failed in each.
- That a real service path refuses under a real step: none did in instrumented runs.
- What sets the clock back, and whether it changes after a WSL or Docker restart.
- Whether production steps at all. Recommended, read-only and the owner's call: run `clock-dual-probe.c` on the production host.
- **The rate.** About 186 retained suite outputs suggest about 1 expected recorded failure against 4 observed, and M9 alone was unlikely at its measured gaps. The estimate assumes uniform timing and comes from one workload, so the mechanism is not reconciled numerically.
- The three guards added by this revision, explicit NULL, repeated steps, and the eligibility and TTL interaction: none has run.
- Instrument effects: logging adds time inside guards, which may shift the measured gaps.
- SQL outside this repository (Section 6.6).

## 8. Process record

### 8.1 Boundary breach

**During this investigation the unintended read below reached a shared instance, against an explicit instruction not to access shared databases.**

- **What:** one read-only query, `docker exec usapp-postgres-1 psql ... -d usapp_test`, reading `pg_proc` and `pg_namespace` for trigger function names.
- **Why:** `docker ps --filter 'name=usapp-tgp-' --filter 'name=postgres'`. Repeated `name` filters are ORed, so it matched the existing stack's container, and the script took the first result without checking it.
- **Effect:** no write, no row data, no credential. The output was seen, then discarded, and is not used in this document. It may appear in the session record.
- **Not undone:** the access itself can't be reversed, and the server may have logged the connection.

### 8.2 Requirement for future investigation scripts

Every script or one-off command that touches a container must:

1. **Resolve by project only:** take the ID from `docker compose -p <project> ps -q <service>`, with the project name generated by that run. Never use `docker ps --filter name=`, substring or pattern matches, `Select-Object -First 1`, or a container name as the target.
2. **Require exactly one ID**, and stop otherwise.
3. **Check ownership before use:** `docker inspect <id>` must show label `com.docker.compose.project` equal to this run's project, `com.docker.compose.service` equal to the expected service, and a name beginning with that project.
4. **Check isolation:** an internal project network and no published ports.
5. **Target the ID in every `exec`, `cp`, `logs` and `stop`,** rechecking that the ID still has that label immediately before each ad hoc command.
6. **Treat containers started with plain `docker run` the same way:** give them a project-prefixed name and an ownership label at creation, and check that label before use.
7. **Fail closed:** on any mismatch, stop and report, with no fallback to another match.

Existing harnesses resolve containers by the compose project label, which satisfies part of this, but they do not recheck ownership before each use and their `docker run` containers carry no label. They need this applied before their next use. They were not changed, as this revision is documentation only.

### 8.3 Void runs

- `usapp-tgi-20261105210927-9b0211` stopped on a harness bug.
- `usapp-tgi-20261105211325-0d5704` filled its 1 GB tmpfs with WAL within about 20 s and PostgreSQL panicked, so its suite rounds show nothing and its "PASSED" line means only that the harness finished. Its clock probe remains valid. Later runs set WAL limits on the disposable instance and limited the loop to windows.
- `usapp-tgp-20261105220019-d62df2` stopped on a syntax error in my test script.

Logs are under `%TEMP%\<project>`, with the analysis in `analysis.txt` in the `usapp-tgi-20261105212931-bea48e` directory.

## 9. Implementation (2026-11-05)

### 9.1 Approval and scope

The owner approved the eight-guard scope, the tradeoffs below, and the Push protection in 9.3. `0041_timestamp_guards`, sha256 `1987d23187759f257412833f39259bdc2c0389c5af306d29acf5bfd8b8292d77`, is FROZEN from its first committed application, in disposable project `usapp-tg41-20261105224934-37e677`. The number was rechecked: 0040 was the latest, so the next is 0042. Migrations 0000-0040 are unchanged. Contract unchanged at 0.36.0, because the new outcome is internal and no API or web code names these outcomes. A build needs 0041 at startup: the task privilege policy refuses to start while any of the eight columns has a default.

### 9.2 Final guard semantics

For a timestamp that is omitted, `DEFAULT` or explicit NULL, which are one request:

`T = GREATEST(the guard's clock reading, all required source floors)`, read after the guard's locks.

| Column | Guard | Floors |
|---|---|---|
| `messages.created_at` | `usapp_message_guard` | none |
| `message_replies.created_at` | `usapp_message_reply_guard` | the message |
| `reward_redemptions.created_at` | `usapp_redemption_guard` | the product revision |
| `reward_redemption_events.created_at` | `usapp_redemption_event_guard` | the redemption, and the proposal a decision answers |
| `reward_redemption_fulfillments.fulfilled_at` | `usapp_redemption_fulfillment_guard` | the redemption |
| `notifications.created_at` | `usapp_notification_guard` | the source's `occurred_at` |
| `notification_intents.created_at` | `usapp_notification_intent_guard` | the source's `occurred_at` |
| `push_deliveries.created_at` | `usapp_push_delivery_guard`, timestamp part | the notification |

A supplied non-NULL value meets the original predicate and error. Identity, authorization, state, ordering, eligibility, TTL and deadline checks are otherwise unchanged, and no interval or tolerance was added.

**What holds:** causal ordering along the declared chains. `T` is not earlier than any declared floor or than its clock reading, and is exactly one of those values.

**What does not hold:** a timestamp may lead a backward-moving clock, **with no fixed bound**, until the clock passes it. Regression C1 asserts that this lead grows past any single step. Two rows with no declared link can be in either order.

### 9.3 Final Push protection

Traced through each stage:

| Stage | Rule |
|---|---|
| Fan-out (`kFanoutSql`) | A notice ahead of the database clock is skipped and found again by the bounded scan on a later tick. Nothing is written |
| Claim (`kClaimSql`) | The same: no attempt exists, so none is spent |
| Pre-send (`kPreSendSql`, `decidePreSend`) | If the clock moves back after a claim, new closed outcome `notice_ahead_skipped`. It is internal, spends no provider budget, decides no terminal state and pauses nothing. The retry is judged again |
| Pre-send TTL (`boundedTtlSeconds`) | At most the remaining time, the whole-second lifetime and 86400. Under 1 is never sent. `expires_at_ms` is untouched, so the delivery's expiry is not extended |
| `WorkerTransport` | Sends those values unchanged |
| Worker (`parseDeliverRequest`, `deliver`) | **Unchanged.** Still refuses above 86400, negative and fractional values, and still skips under 1 second or past the expiry |

**Why pre-send records an observation:** a claim left unresolved expires, and an expiry with no observation counts against the provider budget. So "do nothing" would have spent an attempt.

**Precedence:** lease, terminal, TTL, cutover, control and inactive answer first, unchanged. `NoticeAhead` comes before the reminder source check, because that validity is read from the same clock that is behind. Expiry stays as before: a deferred notice expires from its own `created_at` plus its lifetime.

**The retry waits for the internal backoff** (60 s by default) rather than the next tick, in the rare pre-send case.

### 9.4 Evidence: run `usapp-tg41-20261105232556-aa4e56`

Disposable stack with a fake transport and the worker's in-process stub provider. Every database command first asserted the exact project label, container ID, internal network, database identity and that the target is not the existing stack: **226 checks, one container ID** (`target-checks.log`). Complete logs were saved before filtering. No CTest timeout changed.

| Stage | Result |
|---|---|
| Base image on 0000-0040 | `db_gated` 4133, `push_dispatch` 532, memory suite passed, 0 failed |
| New binary without 0041 | Refused to start, exit 1 |
| Rollback-only prevalidation, both roles | 11 combined ledgers; message, notification, choice, redemption, workflow and reminder ledgers each alone; timestamp ledger 115 (maintenance) and 111 (application) cases; push dispatch both modes. Nothing persisted |
| Committed apply | Only 0041. Privilege deviations 0, 0, 0 |
| Repeat apply and InitRoles | 42 skipped, 0 applied. 42 registered, every checksum matches |
| Functions | 149 in `public`. Exactly the eight guards changed, source only, with owner and grants the same. **This includes every deadline, undo, purge, lease and expiry function** |
| Timestamp ledger on the committed schema | 115 and 111 cases passed, rolled back |
| Mutants, none committed | 10 of 10 caught: each of 8 floors, a supplied-value predicate, and the delivery TTL |
| Targeted suites | MEMORY_DELETION 934, MEDIA_DELETION 376, MEDIA 239, MEMORY 260, ALBUM 726, OSS 41 |
| Worker unit tests | 65 of 65 |
| Real API binary | 276, flag unset |
| Full CTest | 8/8 in 77.07 s: unit 2540, db_gated 4139, media_db 239, media_deletion_db 376, memory_deletion_db 934, push_dispatch 591 (20.26 s of its 30 s limit), oss_stub 363, static_check 971 |
| Scheduler, WorkerTransport, worker, stub provider | Passed. Worker logs hold no endpoint, key, token or payload |
| Older M1d build's `db_gated` on 0041 | Passed |
| Strict YAML | Passed |
| Affected and other SQL ledgers, both roles | Passed. Final privilege deviations 0 |
| After the suites | 36 `user_deleted` plans, 3 early and all declared by exact id, 33 at or after deadline. Purges 39, none before deadline, none unerased. Sentinels 0 of 11243 PostgreSQL lines, 0 API lines. Credentials 0 |

**Deterministic Push tests:** future-dated notices become eligible when the clock catches up; nothing is spent while deferred, at fan-out or at claim; a step between claim and pre-send sends nothing and costs no budget, and the retry goes; expiry stands; a delayed pre-send is `ttl_skipped`; TTL bounds at 1, 86400 and beyond; invalid worker requests stay refused.

**Real clock steps during this run:** the probe saw 27 steps, 471-1519 us. No service-path time refusal occurred. Refusals in the log were deliberate test cases: 3 `USAPP_NOTIFICATION_SOURCE`, 3 `USAPP_NOTIFICATION_INTENT_SOURCE`, 2 `USAPP_MEDIA_DELETION_TIME`. **One passing run with a low expected failure rate does not show that the failures are gone.** The tight loop has not been rerun against the final eight guards.

The sleeping clock probe in that run again reported 0 backward steps. That is its known blindness (Section 2).

### 9.5 Runs that did not pass

None of these was an intermittent failure, and each was diagnosed before rerunning.

| Run | Cause |
|---|---|
| `usapp-tg41-20261105224156-3a96b6` | Harness: double quotes in a `docker inspect` format do not survive a native argument, so the target check stopped the run before any database command |
| `usapp-tg41-20261105224934-37e677` | First committed apply. `Intent diagnostics` failed its premise (9.6) |
| `usapp-tg41-20261105225928-562213` | My new static check matched an earlier `<=` in the expiry query. All 7 other targets passed |
| `usapp-tg41-20261105231209-dd7f58` | CTest 8/8 passed. Worker integration failed because this harness gave the workers random VAPID keys: `worker-prod` refused them, exit 78, and the stub could not sign. Fixed with a real throwaway pair and a health wait |

Prevalidation-only run `usapp-tg41-20261105224418-d3579a` passed before the first commit.

### 9.6 Deviations

1. **One existing test's fixture changed.** `Intent diagnostics: the real guard refuses the real write` used a source dated in the future to force a refusal, which is exactly what the approved semantics remove. It now forces the refusal by changing the source's actor during a held 33012 wait, and keeps its assertions about the sink, the failed write and the absence of ids. The predicate it reports changes from `time_created_before_source` to `actor`. A new case asserts the new behaviour for a future-dated source. **Review this, because it is an edit to an existing test.**
2. **The time labels of that diagnostic can no longer fire on service paths,** because those paths supply no time. They remain, tested as labels, and are test-only.
3. **Fulfillment guard split:** its one combined `IF` became a state check then a time check, with the same error.
4. **One new closed value and a startup check** beyond the eight guards, both needed for the protection and the project's startup convention.
5. **Older-build result covers `db_gated` only.** The older API binary and a real rollback were not run.

### 9.7 Remaining risks

- **Causes of the historical failures stay unproven.** The guard failures were reproduced in a tight loop. The four recorded test failures fit by timing only, and none was caught.
- **Read and outcome guards keep their predicates:** a read or outcome right after a leading notice or delivery can still be refused until the clock passes it. Tests pin this down.
- **The other guards in the static list keep today's exposure,** including `usapp_reminder_occurrence_guard`, whose window rule must not get this treatment. Some of their rows are floors for 0041 guards, but a refusal in the source guard itself can still occur.
- **Eligibility inputs can move:** a notice ahead of the clock can meet `activated` or cutover checks for something recorded after it really happened, while the clock is behind. Deferral stops it being sent until the clock passes it, but not this comparison afterwards.
- **Deadline decisions remain single clock readings,** so a step can shift one by its size. Their rules were not touched. Purge and undo were not tested against real steps.
- **A notice far ahead defers its push by as long as the clock stays behind.** There is no bound on that and no alert. Its lifetime also ends correspondingly later.
- **New intermittent test exposure:** a test that creates a notice and ticks within a step's window now sees it deferred. None occurred here, and it would look like a missed delivery.
- **`push_dispatch` now takes about 20 s of its 30 s limit.**
- **Triggers disabled:** omitted and DEFAULT forms now fail on `NOT NULL`, including in any SQL outside this repository, which could not be reviewed.
- **Production is unmeasured.** The clock source, the rollout and a real rollback are all untested there.
- **Other gaps:** real push providers, real storage, browsers, devices and the web suite were not run. The full sequence passed once, with no stress repeat.
- **Still open, unrelated to this fix:** the P3-1a delayed-copy failure and the `DbClient` teardown crash.
