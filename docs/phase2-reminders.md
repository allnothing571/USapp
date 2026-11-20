# Phase 2 P2-6: Reminders

Status (2026-11-02): **owner approved the recommended defaults O1-O15 of revision 2. R1 (rule storage and API) is implemented and verified in disposable
isolated instances only: [Section 17](#17-r1-implementation-and-isolated-verification-claude-code-2026-11-02). R2 (in-app generation, off by default) is implemented and verified the same way: [Section 18](#18-r2-implementation-and-isolated-verification-claude-code-2026-11-02). R3 (web controls and notification compatibility) is implemented with automated tests only: [Section 19](#19-r3-implementation-and-automated-verification-claude-code-2026-11-02). R4 (rule inheritance on approved edits) is implemented and verified in disposable instances only: [Section 20](#20-r4-implementation-and-isolated-verification-claude-code-2026-11-03). R5 (stale push suppression and bounded expiry) is implemented and verified the same way, with a fake transport: [Section 21](#21-r5-implementation-and-isolated-verification-claude-code-2026-11-03). Reminder generation status is exposed: [Section 22](#22-reminder-generation-status-claude-code-2026-11-03). Rollout is not approved. NOT DEPLOYED.** Sections 1-16 are the design as approved; where R1 adds detail, Section 17 says so.
Revision 2 (2026-11-02) answers Codex's review: schedule-effective time (Section 6.1), period and fire bounds (Sections 5.2 and 6.2),
candidate selection (Section 9.1) and the SQLSTATE class (Section 8).

Labels:
- **[V1]** explicit requirement in `_req_extract.txt` (section numbers are V1's);
- **[Fact]** read from this repository's source or migrations;
- **[Doc]** stated in project documents, not independently confirmed;
- **[Rec]** proposed default;
- **[Owner]** decision needed (Section 15);
- **[Assumption]** unverified.

## 1. Baseline and evidence

| Item | State | Basis |
|---|---|---|
| Main database | 0000-0017, last recorded | [Doc] `CLAUDE.md`, `claude-code-handoff.md`. No database was accessed |
| Shared `usapp_test` | 0000-0023 | [Doc] same. No database was accessed |
| 0024 | Frozen; verified in disposable instances only | [Doc] `phase2-web-push.md` Sections 15-16. Its header still says DRAFT; the frozen hash is recorded there |
| P2-5c | Implemented and verified in isolation; real provider, service worker lifecycle and devices unverified; real push disabled | [Doc] `phase2-web-push.md` Section 16.10 |
| `db_gated` abort | Unexplained | [Doc] same |
| `AGENTS.md` | Does not exist | [Fact] no such file |
| Migrations on disk | 0000-0024, so the next number is 0025 | [Fact] `database/migrations` |
| V1 text used | `_req_extract.txt`; `app开发方案V1.docx` itself was not read | Same extract as `phase2-plan.md` |

Two findings affect scope:

1. **V1 Section 24 contains no reminder requirement.** It lists title, date, type, note, image and a Home choice. Anniversary
   reminders come only from the P2-6 row of `phase2-plan.md`. They are a planned feature, not a V1 rule, so their whole behaviour
   here is [Rec] ([Owner] O1).
2. **`phase1-plan.md` says task reminders "need push".** This design supersedes that: in-app reminders work with Push off or no
   device subscribed, as assigned. Push only adds a delivery channel.

## 2. Requirements versus proposals

**Explicit [V1]:**

| Section | Requirement |
|---|---|
| 11 | The publisher sets the deadline |
| 11 | The recipient decides whether to be reminded and when; examples are "22:00" and "30 minutes before" |
| 11 | The publisher must not dictate when the recipient receives a deadline reminder |
| 8, 9 | One-off, daily, weekly and custom periods; single and counted completion |
| 10 | An expired task: no penalty, "不产生强烈负面提醒", leaves the to-do area, no "owed task" pressure |
| 18.3 | One notification mechanism, not one per module |
| 44 | Natural-day values must not drift through UTC; store timezone and local date apart from instants |
| 52.4 | Web Push service and compatibility are not final |

**Everything else below is [Rec]:** defaults, bounds, grace windows, anniversary behaviour, the no-refire rule, and all schema.

**Not in scope:** a generic workflow engine, rule language or automation; partner-set reminders; overdue or nudge notices;
quiet hours; snooze; email; new Push behaviour beyond the stale fence in Section 10; rollout.

## 3. Who configures and who receives

| | Task reminder | Anniversary reminder |
|---|---|---|
| May configure | The assignee only, enforced by service and database | Either member, each for themselves |
| Receives | The configuring member only | The configuring member only |
| Partner visibility | None: the creator cannot read or infer it | None |
| Default | Off ([Owner] O2) | Off ([Owner] O2) |
| Actor on the notice | None (`actor_id` NULL, Section 8) | None |

There is no client `recipient_id`: the recipient is always the rule's owner, the authenticated caller at write time. A task's rule
requires `owner = tasks.assignee_id`; an anniversary's rule requires membership of its couple. A member without a partner can use
anniversary reminders. The creator's `task.published` notice (P2-3b) is unrelated and unchanged.

## 4. Time semantics

- All civil values are Asia/Shanghai. **[Fact]** `tasks_timezone_supported` allows only that zone, and every period guard computes
  with `AT TIME ZONE 'Asia/Shanghai'`. The device timezone is never used; the UI labels times 北京时间.
- A rule stores civil values (`at_time`, `days_before`, `before_minutes`). `fire_at` is a computed `timestamptz`, frozen on the
  occurrence. Rules never store UTC instants except for the audit time.
- `at_time` has minute precision (CHECK: zero seconds). Asia/Shanghai has no DST today, so no gap or overlap handling is added; the
  zone name is still used in every computation.
- **[Fact]** Deadlines are exclusive. A date-only deadline ends at the next midnight, and a decision must be strictly before it.
- All "now" comparisons use the database's `clock_timestamp()`, as Push and the task guards do. Precision is a tick (default 15 s),
  and a late tick delays rather than duplicates.

## 5. Task reminder rules

One rule per (owner, root task). Recurring rules attach to the **root**, because **[Fact]** periods are lazily materialized: an
untouched period has no row (0009, 0010).

Modes:
- `before_deadline`: `before_minutes`, bounded per schedule in Section 5.2 ("30 minutes before");
- `local_time`: `days_before` plus `at_time`, counted back from the period's last local date ("22:00");
- `absolute`: `at_local_date` plus `at_time`, only for a one-off with no deadline ([Owner] O5).

| Schedule | Period | Last local date | Deadline (exclusive) | Fire instant |
|---|---|---|---|---|
| One-off with time | once | `deadline_local_date` | `deadline_at` | `deadline_at - before_minutes`, or local date/time |
| One-off date only | once | `deadline_local_date` | next midnight | same |
| One-off no deadline | once | none | none | `absolute` only; `before_deadline` and `local_time` refused |
| Daily | Beijing date | that date | next midnight | same date; `days_before` must be 0 |
| Weekly | Monday-Sunday (**[Fact]** 0010) | Sunday | next Monday midnight | `days_before` 0-6 |
| Custom | one range (**[Fact]** 0011) | end date | midnight after end | once; must be on or after the start date |

Rules for every schedule:
- At most one reminder per period ([Owner] O8). Custom means once per range, not once per day.
- A fire instant at or after the deadline yields no occurrence. For a one-off or custom task the write is refused
  (`REMINDER_TIME_INVALID`).
- A custom fire before the start date is refused, since **[Fact]** no action is accepted before it (UC001).
- A one-off or custom write whose fire instant has passed is refused (`REMINDER_TIME_PASSED`). A recurring rule simply starts at the
  next period, and the response returns `next_fire_at`.
- Partial progress on a counted task does not suppress the reminder. Only completion does.

### 5.1 Task states

Checked at generation, under the root lock (Section 11).

| State of the target period | Generation | Basis |
|---|---|---|
| Completed | No occurrence. Recurring: only that period's | [Fact] `task_completions`; period row via `recurrence_id, period_date` |
| Expired | No occurrence, ever. No "you missed it" notice | [V1] 10 |
| Cancelled | No occurrence; rule writes refused (409) | [Fact] `task_closures.reason='cancelled'` |
| Superseded | No occurrence on the old root; the rule is copied to the successor | [Fact] `reason='superseded'`, `successor_task_id` |
| Pending edit or cancel proposal | **Still fires on the current terms**; rule writes allowed | [Fact] a proposal changes nothing until approved |

Supersession ([Owner] O7): the approve transaction inserts a copy of the recipient's current rule on the successor (`origin='successor'`),
so the recipient need not reconfigure. It is the recipient's own choice carried forward, never the creator's. If the new terms make it
unusable (say `before_deadline` and the deadline was removed, or the instant has passed), it yields no occurrence and the UI shows
"需要重新设置". The successor has a new identity, so one further reminder in the same period is possible; this is accepted because the terms
changed. **[Fact]** For recurring tasks, M6 refuses an edit once the current period has a stored row, which limits this.

### 5.2 Period bounds: no future-period enumeration

Rejected: enumerating future periods. Instead, a schedule-specific bound guarantees that **a fire instant lies inside its own period**, so generation only ever needs the
period containing `now` ([Owner] O13).

| Schedule | `before_minutes` | `local_time` | Earliest possible fire |
|---|---|---|---|
| Daily | 1-1440 | `days_before` = 0 | Period start, that day's 00:00 |
| Weekly | 1-10080 | `days_before` 0-6 | Period start, Monday 00:00 |
| One-off with a deadline | 1-10080 | `days_before` 0-7 | Not bounded by a period; there is one target |
| Custom | 1-10080, and fire on or after the start date | `days_before` 0-6, and fire on or after the start date | Range start |
| One-off no deadline | Refused | Refused | `absolute` only |

Writes outside these bounds are refused (`REMINDER_TIME_INVALID`). A larger daily value, such as 1500 minutes, would mean "before the period exists", and is not offered.

Why the current period is enough:
- `period start <= fire_at < deadline` by the bounds above.
- `stale_at <= deadline`, and a recurring deadline equals the next period's start.
- So when `now` is in a period's window, `now` is in that same period. A candidate is **never** in an earlier or a later period.

**Daily reminder before midnight.** "30 minutes before" on the period for date D fires at D 23:30, in period D, and is stale at D+1 00:00. A tick at D 23:45 after a short outage creates it late. A tick at D+1 00:00 considers
only period D+1, whose instant is D+1 23:30. No reminder for D can appear on D+1, and none for D+1 can appear on D.

**Tick spanning midnight.** The scan reads `now` once. Its candidates carry their `period_date` and `fire_at` to step 3, which re-checks `clock_timestamp() < stale_at`. A candidate that has crossed midnight is dropped, not moved to another period.

**Successors.** **[Fact]** A successor keeps its `schedule_type`, so a copied rule's bound still holds. Terms may still leave it unusable (Section 5.1).

## 6. Anniversary reminder rules

One rule per (owner, anniversary), with up to 3 `days_before` values from {0, 1, 3, 7, 14, 30} and one `at_time`, default 09:00 ([Owner] O9).
`fire_at = (target_date - days_before) + at_time`.

| Kind | Target dates | After the date |
|---|---|---|
| `countdown` | `event_date` once | Nothing further; the rule stays, inert |
| `anniversary` | `event_date + k years`, k >= 0, yearly | Continues yearly |

- **Leap day** ([Owner] O11): Feb 29 falls on Feb 28 in non-leap years, always computed from the base date. **[Fact]** this is
  `AnniversaryCalendar::addYears`. PostgreSQL's `date + make_interval(years => k)` gives the same result [Assumption: to be pinned by a
  parity test, Section 13]. Feb 28 of a leap year is not a target for a Feb 29 base.
- A reminder never fires for a date before the rule existed.

### 6.1 Schedule-effective time

The previous revision of this design compared `fire_at` with the latest revision's `created_at`, so a title edit at 09:05 suppressed a reminder due at 09:00 during its grace window. Cosmetic revisions must not affect scheduling.

**Schedule basis** of a revision: `(kind, event_date)`. Title and note are not part of it.

**`schedule_effective_at`**: `created_at` of the earliest revision in the **latest contiguous run** of versions sharing the current basis. **[Fact]** Versions are contiguous, and the guard makes `created_at` non-decreasing (UN002), so this is well defined from the immutable rows.

| Revisions (latest last) | Run | `schedule_effective_at` |
|---|---|---|
| v1 A, v2 A (title) | v1-v2 | v1 |
| v1 A, v2 B | v2 | v2 |
| v1 A, v2 B, v3 B (note) | v2-v3 | v2 |
| v1 A, v2 B, v3 A | v3 | v3 |
| v1 anniversary, v2 countdown, same date | v2 | v2 |

| Change | Identity | Not yet fired, instant passed | Not yet fired, instant ahead | Already fired |
|---|---|---|---|---|
| Title or note | Same | **Still fires inside its window** | Fires | No duplicate |
| Date A to B | New targets | A's never fires. B's only if `fire_at >= schedule_effective_at` | Fires on B's terms | A's stays in the inbox; B's targets may fire once |
| Date A to B and back to A | A's identities again | Skipped: conservative, since the instant precedes the new run | Fires | **No refire**: the identity already exists |
| Kind, same date | Same `target_date` where both schedules share it | Skipped ([Owner] O14) | Fires per the new kind | No duplicate |
| Countdown to anniversary | Yearly targets are added | As above | Later years fire | No duplicate |
| Anniversary to countdown | Future yearly targets stop | As above | Only `event_date` if still ahead | No duplicate |

A pending proposal changes nothing until approved. **[Fact]** Approval writes a new revision, which counts as a date or kind change only if the basis differs.

The occurrence records `anniversary_version` (latest at generation) and `schedule_version` (first of the run).

**Rule writes follow the same principle.** A re-save with the same schedule, or a change to another offset, must not suppress a due reminder. `rule_effective_at` is the `created_at` of the earliest revision in the latest contiguous run where the rule stays enabled and these values are equal:
- task: `mode`, `before_minutes`, `days_before`, `at_time`, `at_local_date`;
- anniversary: `at_time`, and **this** offset being in the set (evaluated per offset). Adding 7 to {1, 3} does not disturb 1 or 3.

Disabling breaks the run, so re-enabling counts as new.

### 6.2 Candidate target dates

Enumeration runs from the **fire date**, not from the event year, so year boundaries and leap years need no special case. `stale_at` is the midnight ending the fire date, so only today's fire date can be eligible.

For each offset `d` in the rule (at most 3), with Beijing date `today`:
1. `T = today + d`.
2. Countdown: a candidate only if `T = event_date`.
3. Anniversary: `k = year(T) - year(event_date)`; a candidate only if `k >= 0` and `event_date + k years = T`.
4. `fire_at = today + at_time`, a candidate only if it passes Section 7.

| Example | Result |
|---|---|
| Base Jan 5, `d` = 14, today Dec 22 2026 | T = Jan 5 2027, year from T, k = years to 2027. Fires |
| Base Feb 29 2024, `d` = 1, today Feb 27 2025 | T = Feb 28 2025, k = 1, base + 1 year = Feb 28. Fires |
| Base Feb 29 2024, `d` = 1, today Feb 27 2028 | T = Feb 28 2028, k = 4, base + 4 years = Feb 29, not equal. No fire |
| Same base, `d` = 1, today Feb 28 2028 | T = Feb 29 2028. Fires |
| Base Mar 1, `d` = 1, in a leap year | T = Mar 1, today = Feb 29. Fires |

The work is at most 3 candidates per rule per tick, with no lookahead table.

## 7. Missed runs, downtime, late commits and stale reminders

**Eligibility of an occurrence, one definition:**
```
GREATEST(rule_effective_at, source_effective_at) <= fire_at <= now < stale_at
```
`rule_effective_at` is from Section 6.1. `source_effective_at` is the root task's `created_at`, or the anniversary's `schedule_effective_at`.

| Case | Policy |
|---|---|
| `stale_at`, task | `LEAST(deadline_at, fire_at + 2 hours)` ([Owner] O4). Never after the deadline ([V1] 10) |
| `stale_at`, anniversary | Midnight ending `fire_at`'s Beijing date ([Owner] O4) |
| Downtime inside the window | The next tick creates it late, once, with its original `fire_at` |
| Downtime past the window | Permanently skipped. No catch-up burst and no backlog |
| Many due at once | At most 100 per tick, selected as in Section 9.1; the rest follow on later ticks while still in their window |
| Late-committing rule | Found: there is no watermark, each tick re-evaluates the window, as P2-5 fan-out does. Its `rule_effective_at` is before `fire_at` |
| Cosmetic edit after the instant | Does not affect eligibility (Section 6.1) |
| Rule saved after the instant | Not eligible for this period. It never fires retroactively |
| Scheduler disabled | No generation. A valid window on re-enable applies |
| Rollout | No backfill is possible: no rule exists before the feature ships |

A skipped occurrence writes nothing, because recording every non-event would grow without bound. Tick detail reports counts only.

## 8. Identity, uniqueness and data model

Proposed `0025_reminders` (sketch, not implemented; 0000-0024 stay frozen).

**SQLSTATE class `UV`** (`UV001` append-only, `UV002` rule guard, `UV003` occurrence guard, `UV004` deferred occurrence-notice link). **[Fact]** Checked against `database/migrations` 0000-0024: used classes are UB, UC, UD, UM, UN, UP, UQ, UR, UT, UW, UX, UY, UZ. `UR001`-`UR003` are reward products (0014), and the earlier `UR` proposal here was wrong. No `UV` value appears in migrations, source, tests or docs. Lock 33017 is unused today ([Fact] highest is 33016); recheck both at implementation.

**`reminder_rules`** (append-only; the highest `seq` per target is current):
- `id`, `couple_id`, `owner_id`, `seq`, `task_id` or `anniversary_id` (exactly one);
- `enabled`, `mode`, `before_minutes`, `days_before`, `at_time`, `at_local_date`, `days_before_set smallint[]`;
- `origin` ('user', 'successor'), `idempotency_key`, `request_fingerprint`, `created_at`;
- UNIQUE (owner, key); partial UNIQUE (owner, task_id, seq) and (owner, anniversary_id, seq);
- composite FKs to same-couple rows, per-mode shape CHECKs.

**`reminder_occurrences`** (append-only; each row is a fired reminder):
- `id`, `couple_id`, `owner_id`, `rule_id`, `event_kind`, `task_id`, `period_date`, `anniversary_id`, `anniversary_version`, `schedule_version`,
  `target_date`, `days_before`, `fire_at`, `stale_at`, `created_at`.

**Stable identity, independent of the rule revision:**

| Occurrence | Partial UNIQUE |
|---|---|
| One-off, custom | `(owner_id, task_id) WHERE period_date IS NULL AND task_id IS NOT NULL` |
| Daily, weekly | `(owner_id, task_id, period_date) WHERE period_date IS NOT NULL` |
| Anniversary | `(owner_id, anniversary_id, target_date, days_before) WHERE anniversary_id IS NOT NULL` |

Consequences:
- Concurrent instances, retries and rolling restarts cannot duplicate. **[Fact]** `pg_try_advisory_xact_lock` per job key already serializes ticks across
  instances; the index is the backstop, used with `ON CONFLICT DO NOTHING`.
- `period_date` is the Monday for weekly and the date for daily, matching **[Fact]** `tasks.period_date`, and needs no stored period row.
- **No refire** ([Owner] O3): after a period has fired, changing the time does not fire again in that period. The UI says so.

**Changes to `notifications`** (existing table, altered by the new migration, following the 0020 pattern):
- add `reminder_occurrence_id uuid` with a composite FK;
- replace `notifications_one_source`;
- partial UNIQUE (event_type, reminder_occurrence_id, recipient_id);
- **drop NOT NULL on `actor_id`**, with CHECK `(actor_id IS NULL) = (event_type LIKE 'reminder.%')`, because **[Fact]** a reminder has no actor and
  `CHECK (actor_id <> recipient_id)` forbids using the recipient. The partner would be false, and may not exist;
- `CREATE OR REPLACE usapp_notification_guard`, plus a closed family `reminder` resolving facts from the occurrence;
- deferred constraint trigger: an occurrence has exactly one notification (the 0020 intent-link pattern).

New closed event types: `reminder.task`, `reminder.anniversary`. Targets are `task` with the root id, and `anniversary`.

**Why not a separate inbox:** **[Fact]** `push_deliveries` has an FK to `notifications`, and fan-out scans that table. One row there gives the inbox, read
state, unread count and Push with no second path ([V1] 18.3).

**Privileges:** both tables SELECT, INSERT for `usapp_api`; nothing for `usapp_push` or PUBLIC; append-only triggers; a new `init-roles.sql` section; both
added to `TaskPrivilegePolicy` startup checks and `task_privilege_deviation.sql`.

## 9. Generation job

`ReminderJob`, job key `reminder.generate`, registered **before** `PushDispatchJob`, so that a reminder can fan out in the same tick. Guarded by a new
`REMINDERS_ENABLED` setting, default false (off: the job reports `dispatch=off` and writes nothing). Inside the existing scheduler transaction:

1. **Scan** (read): candidates per Section 9.1, with `now` read once.
2. **Lock**, one statement, in sorted order: `33004` for each task root or `33010` for each anniversary, then `33017` per (owner, target).
3. **Re-evaluate and insert occurrences**: `INSERT ... SELECT` re-checking every condition (rule latest and enabled, not completed, not closed, window) `ON CONFLICT DO
   NOTHING RETURNING`. Under READ COMMITTED this statement sees commits that finished before the locks were granted.
4. **Insert notifications** for exactly the returned rows.

Four statements, one transaction, no network, no `afterCommit`. The job does not reference Push, subscriptions or devices, so in-app generation is unaffected by
Push being off, paused or unsubscribed. A BEFORE INSERT guard on occurrences repeats the checks (and locks, same order) for direct SQL; a guard failure fails the
tick with fixed labels, and nothing partial commits.

A row dropped at step 3 (completed or disabled in between, or crossed midnight) wastes a slot for that tick only: on the next scan it fails the predicate.

Calculation lives in SQL, as a set-based statement like P2-5 fan-out, with a small STABLE function for the fire instant. `AnniversaryCalendar` remains the display
calculation; a parity test keeps them equal.

### 9.1 Candidate selection

The selection must not let finished or ineligible work occupy the limit. In this exact order:

1. **Enumerate** from the latest enabled rules: the current period (Section 5.2), or each offset's target date (Section 6.2). A rule with three offsets yields up to three separate candidates, and each counts on its own.
2. **Exclude everything not eligible** (window, effective times, completed, closed, schedule bounds).
3. **Exclude identities that already have an occurrence**, by `NOT EXISTS` on the same key as the partial UNIQUE indexes of Section 8.
4. **Then** `ORDER BY` and `LIMIT 100`.

Steps 2 and 3 precede step 4 by construction, so a generated identity can never reappear in the limited set, and `ON CONFLICT DO NOTHING` remains only a race backstop.

**Total order**, so selection is deterministic and repeatable:

| Key | Direction | Why |
|---|---|---|
| `fire_at` | ascending | Oldest first, nearest to going stale for equal windows |
| `owner_id` | ascending | Stable |
| target kind | task, then anniversary | Stable |
| target id (task or anniversary) | ascending | Stable |
| `days_before` | ascending | Stable between a rule's offsets |
| `period_date` / `target_date` | ascending | Completes the identity |

These keys cover the whole identity, so no two candidates tie. One owner with many items can use many slots in a tick; at 100 per 15 s this drains long before any 2-hour window, so no per-owner quota is added ([Assumption] couple scale).

**No poison row.** The guard repeats the same predicate from one shared function. If a candidate still failed the guard, the whole tick would fail each time until its `stale_at`, which bounds the outage. That shows in `scheduler_ticks` as `failed`, with fixed labels. A test asserts the scan and guard agree, so this is not expected.

## 10. Configuration changes and obsolete occurrences

Occurrences not yet created are **derived**, never stored, so there is nothing to cancel: a rule change, completion or date edit simply changes what the next tick
computes. No future-occurrence table exists to go stale.

Once created, a notification is immutable (**[Fact]** UN101):

| Event after the occurrence was created | In-app notification | Pending Push delivery |
|---|---|---|
| Rule disabled | Stays | Not sent (stale) |
| Rule time changed | Stays; no refire | Still sent: it is the reminder that was due |
| Task completed, cancelled, superseded | Stays; tapping shows current state | Not sent |
| Deadline or `stale_at` passed | Stays | Not sent |
| Anniversary date moved away | Stays | Not sent while the target is no longer valid |
| Date moved away and back | Stays; no second occurrence | Sent if a retry is still inside `stale_at`: validity is derived from the current schedule |
| Title or note edited | Stays; it holds no text anyway | Sent: the schedule did not change |
| Device revoked, logout, Push paused | Stays | Existing P2-5 fences apply |
| Already handed to a provider | Stays | Cannot be recalled. The label is fixed; a click goes to the current page, which is authoritative |

The inbox keeps stale reminders and they count as unread until read ([Owner] O6), which is the existing principle that notifications are informational and the
business endpoint decides. No mark-read happens automatically.

**Push integration (separate small batch):**
- `usapp_reminder_occurrence_valid(notification)`: a STABLE function, true for non-reminders.
- `kPreSendSql` gains a stale check and `LEAST` of the remaining TTL with `stale_at`, so a retry never outlives the reminder's usefulness.
- A stale attempt records a new internal outcome `source_stale_skipped` with final state `source_stale`. **It spends no budget and pauses nothing.** This is preferred over reusing
  `inactive_skipped`, which means the generation is off. It needs a new migration (the 0024 pattern for the class rule), `outcomeClass`, `terminalForOutcome`, and the `decidePreSend` order, after the
  exact-attempt fence.
- **No worker change.** **[Fact]** The worker's `event_type` pattern and `target_kind` set already accept these; it stays stateless. C++ owns eligibility.
- **Service worker:** add the labels to `notification-labels.ts`. **[Fact]** An old worker's closed table shows the generic text, which is safe.
- A check-to-send race remains, as documented for P2-5.

Fixed wording is gentle and holds no task or anniversary text ([V1] 10; [Owner] O10): `reminder.task` 「有一个任务快到截止时间了」, `reminder.anniversary` 「有一个重要的日子快到了」.

## 11. Lock order

Existing paths, from **[Fact]** service headers and migrations:

| Path | Order |
|---|---|
| Completion and counting | member FOR SHARE -> 33004 root -> wallet claim -> accounts FOR UPDATE -> audit |
| Period materialization | 33004 root (in guards too) |
| Task change propose and decide | member FOR SHARE -> 33004 root -> decision, successor, closure -> notification |
| Task publish, optional intent | member FOR SHARE -> ... -> 33012 |
| Anniversary writes | member FOR SHARE -> 33010 -> ... -> 33012 |
| Push registration | session, member FOR SHARE -> 33013 -> 33014 |
| Dispatch tick | scheduler job lock -> 33016 per claim |
| Push recording | 33014 for gone; 33015 before a pause |

Proposed:

| Path | Order |
|---|---|
| Rule write | member FOR SHARE -> 33004 or 33010 -> key classification -> 33017 -> insert -> audit |
| M6 approve, rule copy | existing order -> 33017 on the successor, last (a new id: uncontended) |
| Generation tick | scheduler job lock -> sorted 33004 / 33010 -> 33017 -> FK key-share on members and tasks |

Why this has no cycle:
- Every path takes a domain lock before 33017, and nothing holding 33017 waits for a domain lock.
- The tick takes several domain locks, but in sorted order, and no other path holds two existing roots: M6's second root is the successor, invisible until commit.
- Requests hold member FOR SHARE while waiting for 33004, and the tick holds 33004 while needing only FOR KEY SHARE on members, which does not conflict with FOR SHARE.
- **[Assumption]** No path updates a `couple_members` key column while holding 33004. Recheck during implementation.
- Jobs run serially, so the tick holds no 33016 or 33015. Wallet locks are never taken by the tick.

**Effect of the locks:** a completion or disable that commits before step 3 suppresses the reminder; one arriving later waits for the tick's commit and is then "after the reminder". This removes the read-then-insert race. The cost is that a completion can wait for a
tick (bounded to 100 rows and 4 statements). **[Assumption]** Duration unmeasured. A deadline decision time is read after the wait, so a completion arriving in the final moments could be refused as late; the bound keeps this small but does not exclude it.

## 12. Contract and service changes

Identified, not implemented. OpenAPI moves from **[Fact]** 0.20.0 to a new version.

- `GET /api/v1/reminders`: the caller's current rules.
- `POST /api/v1/tasks/{id}/reminder` and `POST /api/v1/anniversaries/{id}/reminder`: a full replacement, including `enabled: false`. `AuthFilter`, exact Origin, CSRF, Idempotency-Key, private `no-store`, no offline queue, and no automatic retry: only an explicit retry with the captured key and body, as elsewhere.
- Order: membership, domain lock, **key classification before state checks** so a replay of a committed write succeeds, then state.
- The response has immutable rule facts plus computed `next_fire_at` and `fired_this_period`. Last write wins between devices, like Home pins.
- A period id or a non-root id is refused: the client sends `recurrence_id ?? id`.
- Errors: 404 for another couple's target, `REMINDER_NOT_ASSIGNEE`, `REMINDER_TARGET_CLOSED`, `REMINDER_TIME_INVALID`, `REMINDER_TIME_PASSED`, `IDEMPOTENCY_KEY_REUSED`, `RESULT_UNKNOWN`.
- `Notification.actor_id` becomes nullable in the contract. **[Fact]** `NotificationService::list` reads it with `as<std::string>()` and must emit null. `apps/web/src/api/notifications.ts` types it as `string`, and no view uses it.
- `event_type` gains the two values.
- Rule writes add an audit row (ids, mode, no text). Occurrences add none: they are system facts with tick counts, like P2-5 dispatch. [Assumption] This meets the Phase 2 rule on audited notification sources; confirm in review.

**Web:** a small "提醒我" control on task and anniversary detail, shown to the eligible member only, with Beijing-time wording, presets, disabled and "已提醒" states, and existing inbox rendering. There is no dependency on the Push card. **Push wording must not promise device delivery**, since real push is unverified.

## 13. Tests

**Database scripts:** append-only; shape CHECKs per mode; assignee-only; same-couple FKs; identity uniqueness per family; the deferred link; a non-null actor refused on reminders and a null actor refused elsewhere; existing notification rows and event types unaffected; privileges in both roles; **0025 applies to a database holding 0024 data.**

**Pure and parity:**
- a fire table for all schedules, including date-only midnight, Sunday, range bounds and month ends;
- SQL against `AnniversaryCalendar` over Feb 29 bases, century years and offsets crossing a year.

**DB-gated, with the real scheduler (injected clock is not available, so short relative times or fixtures):**

| Area | Case |
|---|---|
| Concurrency | Two concurrent ticks and a retried tick: one occurrence, one notification; a rolled-back tick leaves nothing |
| Config race | Disable or change during a held tick: the write waits and applies after; disable committed first suppresses; a transaction open across `fire_at` then committed is still found |
| Completion race | Completion holding 33004 before the tick: no reminder; after: reminder, then completion succeeds; no deadlock under both orders, using `pg_stat_activity` waits rather than sleeps, as P2-5b did |
| Recurring | Daily and weekly: new period fires again; a completed period does not, the next does; untouched period needs no row, and generation creates no period row; partial count still fires |
| Task states | Expired never fires, closed never fires, pending edit fires on current terms, successor copy behaves, unusable copy is inert |
| Windows | Past `stale_at` skipped forever; catch-up inside the window once; no burst; the 100 limit drains in order |
| Anniversary | Yearly identity; Feb 29; countdown once; date change new identity, no retroactive fire; stale revision between scan and insert |
| Schedule-effective time | **Title and note edits after `fire_at` still fire inside the window**, with no duplicate; date change; date away and back, unfired and fired; kind change both ways; approved proposal with and without a basis change; `schedule_version` recorded |
| Rule-effective time | Identical re-save after `fire_at` still fires; adding an offset leaves others due; removing one stops only it; disable then enable counts as new |
| Bounds | Daily 1440 accepted and 1441 refused; weekly 10080 accepted and 10081 refused; `days_before` limits per schedule; custom fire before start refused; replay of a committed write still replays |
| Period | Daily "30 minutes before": fires at D 23:30, a tick at D 23:45 creates it late, a tick after midnight does not, D+1 stays independent; weekly at Monday 00:00; a tick spanning midnight; no future period is ever created |
| Year and leap | Offsets crossing Dec 31; the Section 6.2 table, including Feb 28 and Feb 29 in leap and non-leap years |
| Starvation | **More than 100 due (for example 250 across two owners, with multi-offset rules):** the first tick creates exactly 100 lowest in the total order; the second tick selects none of them and creates the next 100; the third creates the remaining 50; a fourth creates nothing. Every identity exactly once, and each tick's selection is asserted, not only the total |
| Selection | Completed, closed, disabled and out-of-window candidates never occupy the limit: 100 ineligible plus 1 eligible still fires it; ties resolve by the documented order; three offsets due together count as three; scan and guard agree |
| Isolation | Creator cannot read or write the rule; the other couple's target 404s; each member's own anniversary rules and notices are separate; no notice to the other member |
| Independence | With Push disabled and no subscriptions, the inbox and unread count work, and no push row appears |
| Push | Fan-out includes a reminder; stale pre-send records `source_stale_skipped`, no budget, no pause; TTL capped by `stale_at`; a retry stays bound; P2-5 replay and concurrency repairs still pass |
| Off switch | `REMINDERS_ENABLED=false` writes nothing, and startup writes nothing |
| Static | Migration uses no UPDATE or DELETE; `ReminderJob` does not include Push headers; no network in the job |

**Web:** eligible member only; replay with captured key and body; passed-time errors; identity reset discards late results; labels; null actor; the SW generic fallback for an unknown type.

**Starvation cost:** the fixture is set-based, and needs its own time check. Lowering the limit is not a substitute for the literal case above.

Mutation-check the race, identity, schedule-effective, selection and stale tests (moving the `NOT EXISTS` after `LIMIT` must fail the starvation case), which is what caught blind spots in P2-5. New tests must fit the existing 30 s `push_dispatch` and `db_gated` limits, or live in a separately approved target: **timeout increases are not authorized.** If the `db_gated` abort recurs, preserve diagnostics and stop.

## 14. Implementation batches

Each needs its own approval and stops for review. Nothing here authorizes applying to shared `usapp_test` or main.

| Batch | Scope | Depends on |
|---|---|---|
| R1 | 0025 schema, privileges, rule service and API, contract; SQL and DB-gated rule tests. No job, so no reminder can fire | Owner decisions; verify in a disposable instance |
| R2 | `ReminderJob`, in-app only, `REMINDERS_ENABLED` default false; concurrency, race, window and isolation tests | R1 |
| R3 | Web control, labels, null actor | R1; usable with R2 |
| R4 | Copy rules on M6 approval (one change in the `TaskChangeService` approve path) | R1 |
| R5 | Push stale fence, new migration, pre-send and TTL changes | R2, plus 0023-0024 present |

**R1-R4 do not need Push accepted** and could precede real-provider acceptance. R5 is safe while Push stays disabled. Rollout order, when separately authorized: 0018-0024 must reach the target database first. **[Doc]** the shared test database lacks 0024 and main lacks 0018-0024, so reminders cannot reach main without that earlier release decision.

## 15. Owner decisions

| # | Decision | Recommended |
|---|---|---|
| O1 | Include anniversary reminders, which V1 Section 24 does not state | Yes, per `phase2-plan.md` |
| O2 | Default | Off for both, opt-in per item |
| O3 | Changing the time after this period's reminder fired | No refire in that period |
| O4 | Grace after a missed instant | Task: 2 hours, never past deadline. Anniversary: to the end of that Beijing day |
| O5 | One-off with no deadline | Allow an explicit date and time |
| O6 | Stale reminders in the inbox | Keep, immutable, unread until read |
| O7 | Approved edit (successor) | Copy the recipient's rule; show if unusable |
| O8 | Reminders per task period | One |
| O9 | Anniversary offsets | Up to 3 from {0, 1, 3, 7, 14, 30}, default time 09:00 |
| O10 | Wording | The two fixed gentle labels in Section 10 |
| O11 | Feb 29 in non-leap years | Feb 28, matching P2-2 |
| O12 | Recurring custom-range reminders (daily within a range) | Not now |
| O13 | Lead-time limits | Daily up to 1440 minutes, weekly up to 10080, so a reminder is always inside its period |
| O14 | Kind change while a reminder is in its window | Treat as a new schedule: skip that one, keep later ones |
| O15 | Date moved away and back | No refire of a fired target; a passed, unfired one stays skipped |

## 16. Risks

- **The tick briefly blocks completions and task changes** on held roots (Section 11); unmeasured, bounded by batch size. If measured to matter, the fallback is no domain locks and accepting a rare stale reminder.
- **Nullable `actor_id` touches a live table's meaning** across reader code and the contract; all readers need review.
- **The scan runs every tick** over current rules. Fine at couple scale [Assumption], unmeasured; add indexes only with evidence.
- **A stale reminder stays in the inbox**, and a provider-held push cannot be recalled.
- **The successor can produce one more reminder** in a period.
- **Clock:** a database clock jump can move windows; identity still prevents duplicates.
- **Real Push remains unverified**, so reminders on a phone cannot be promised. iOS needs Home Screen installation (`phase2-web-push.md`).
- **Migration dependency:** 0025 sits on 0018-0024, whose main rollout is undecided.
- **`db_gated` abort** is unexplained and could affect verification of these batches.
- **The rule and revision run calculation runs in every scan.** Small at couple scale [Assumption], unmeasured.
- **A daily reminder cannot be set for "the evening before"**; the bound rules that out (O13).
- **Not reviewed in depth:** `TaskService` publication internals and the full M6 approve statement order. The order above comes from service headers and guards, so R1 and R4 must re-read them.

## 17. R1 implementation and isolated verification (Claude Code, 2026-11-02)

Owner approval: defaults O1-O15 adopted, authorizing R1 implementation and disposable-isolated verification only. R2-R5 remain unapproved.

### 17.1 State, stated exactly

| Stage | State |
|---|---|
| R1 implemented | Yes: migration, privileges, service, routes, contract, tests |
| Disposable-isolated verification | Passed, project `usapp-p26r1-20261102210159-4df2e7` |
| `0025_reminders` | **FROZEN**, sha256 `29a92d8222e47d40c410fe593d77d69450f521d6b1c7172598cf958261b9dcc2`. Further database corrections need a new migration |
| Application scope | Disposable instances only: first committed in `usapp-p26r1-20261102205304-ce2172`, then the passing run. **Not** shared `usapp_test` ([Doc] 0000-0023) and **not** main ([Doc] 0000-0017) |
| Generator, scheduler registration, web, M6 copy, Push fence | Not implemented (R2-R5) |
| Live HTTP | Not exercised: tests call the service directly, so `AuthFilter`, Origin, CSRF and routing on these three routes are unverified at runtime |
| Deployed | **No** |

### 17.2 What R1 adds

- `database/migrations/0025_reminders.sql`: append-only `reminder_rules` and `reminder_occurrences`, the closed notification family, and read functions. SQLSTATE class `UV` and lock 33017 were rechecked as unused.
- `ReminderService` and `ReminderController`: `GET /api/v1/reminders`, `POST /api/v1/tasks/{taskId}/reminder`, `POST /api/v1/anniversaries/{anniversaryId}/reminder`. Contract 0.21.0.
- `NotificationService::list` emits a null `actor_id`; it was the only reader assuming non-null. Others were checked: `NotificationWriter` and `NotificationIntent` write from their own non-null sources, and `PushDispatch` does not read it. The web type is left for R3, since no reminder can exist in production before R2.

### 17.3 Details decided while implementing

| Item | Behaviour |
|---|---|
| Disabled request | Exactly `{"enabled": false}`; any other field is a 400. Stored with every schedule column NULL |
| Disabling | Allowed after the instant passed, and on a completed or expired task. Refused on a cancelled or superseded root (`REMINDER_TARGET_CLOSED`), per Section 5.1, which says rule writes are refused there |
| Enabling on a completed one-off or custom task | `REMINDER_TARGET_CLOSED` |
| Identical re-save | Appends a revision; effective time does not move |
| Replay | Classification precedes every mutable check, so it succeeds on a closed target; `mutation` holds the original facts, `current` today's state, nothing is appended or restored |
| Period id as target | `REMINDER_TIME_INVALID`: clients send the root |
| Single definition | `usapp_reminder_task_rule_problem` gives both the service's error and the guard's refusal, so they cannot disagree |
| `origin = 'successor'` | The column exists, but the guard refuses it until R4 (a later migration replaces the guard) |
| List | Limit 1-100, default 50, keyset on each target's first revision (immutable). `next_cursor` is non-null whenever more exist. Disabled rules are listed |
| Audit | `reminder.rule_saved` with rule id, seq, enabled and mode only: never a time, offset or text |
| `next_fire_at`, `fired_this_period` | Computed on read. `fired_this_period` is always false via the API in R1, since nothing generates |

Lock order as designed: member FOR SHARE -> 33004 or 33010 -> classification -> state -> 33017 -> insert -> audit.

### 17.4 Verification

Complete logs were saved before filtering, in `%TEMP%\usapp-p26r1-20261102210159-4df2e7\`. `Claude outputs/p26r1-isolated-verify.ps1`. Representative data came from the earlier verified image's `db_gated` against 0000-0024: 22 notification event types, none with a null actor.

| Check | Result |
|---|---|
| Rollback prevalidation of 0025 and the SQL tests, on that data | 53 cases passed as `postgres` and as `usapp_api`. Afterwards: not registered, no tables, `actor_id` still NOT NULL, guard md5 and rows unchanged |
| Replacement guard against the 0020 body | One `IF` became `ELSIF`, one 3-line reminder branch added; every other line identical |
| Apply | Exactly 0025 |
| InitRoles and privilege deviation, twice | 0 deviation rows both times; repeat skipped 26, applied 0 |
| Checksums | 26 registered, all equal their local files |
| Existing notifications after 0025 | Per-type counts unchanged; 0 null actors |
| CTest, 4 targets, unchanged limits | 100%, exit 0, 32.20 s: unit 2137 assertions 11.21 s; db_gated 2669, 10.37 s; push_dispatch 391, 10.59 s; static_check 489, 0.03 s |
| SQL scripts | Dispatch in both modes; reminder, notification, choice, subscription, reaction, message, anniversary, task, wallet, product and redemption ledgers in both roles, all ROLLBACK. Reminder: 53 cases in each role |
| Occurrences | 0 after apply. 1 after CTest: the compatibility case's direct insert. No path in R1 creates one |
| Control state, credentials in logs | disabled; 0 |
| OpenAPI YAML parse | Not performed in this run; no parser was found in the web, admin or worker packages, host Python, or the `postgres:17` and test-runner images. **Superseded by 17.8**, which uses the installed Docker Compose loader. `static_check` reads the contract as text only |

**DB-gated cases (block 30):** assignee only; other spaces; each member's own anniversary rule; no-space; period id; every schedule bound at its limit and one past it; passed and invalid instants; disable after passing and after completion; pending proposal allowed, closed refused; replay separation after a later change and after closure; key reuse across bodies and targets; audit content; six concurrent writes staying contiguous; concurrent same-key writes appending one; task effective time (re-save, change, disable, re-enable); per-offset effective time (add, remove, re-add, re-save, time change, disable); schedule effective time (title, date, away and back, kind); pages stable under later changes and complete; null actor through list, unread count and read receipt; a null actor refused on another family.

**`db_gated` abort:** did not recur in these runs. It remains unexplained; this is not evidence of a fix.

**Mutation testing:** not run, as instructed.

### 17.5 Failures and repairs

| Found by | Problem | Repair |
|---|---|---|
| Review before any run | An empty offset array made the CHECK evaluate to NULL, which passes | COALESCE, before first application |
| Script | A PowerShell 5.1 parse error from non-ASCII text | ASCII only |
| Rollback prevalidation | PL/pgSQL ended an `IF` at the `THEN` inside a `CASE` | Parentheses, before first application |
| Script | Expected `t` for a boolean cast to text | Expectation corrected; nothing had persisted |
| CTest after first application | An audit assertion matched `offset` inside `anniversary_offsets`; an ambiguous `id` in a test query; three static checks pinned 0.20.0 and one the event list's end; one needle mismatched | Tests only. **No migration or service defect** |
| CTest | `push_dispatch`'s R2 needs the one-shot pre-0024 fixture, which the script's base run had consumed | The base step now runs `db_gated` only |
| SQL scripts | Two existing `TRUNCATE` assertions got `0A000`, since the new tables reference `tasks` and `anniversaries` | Both lists extended, the pattern those scripts already use; the assertions still reach the append-only trigger |

0025's bytes did not change after their first committed application.

### 17.6 Resources

All created by this verification. Stopped and retained, nothing removed: 16 exited containers and 5 internal networks of projects `usapp-p26r1-20261102204800-80e522`, `-205039-4e7897`, `-205304-ce2172`, `-205746-aadc2b`, `-210159-4df2e7`, with logs under `%TEMP%\<project>\`. Image `usapp-isolated-test-runner:p26r1`. Test fixtures lived only in those instances' tmpfs databases. The shared and main databases were not contacted.

### 17.7 Remaining risks

- **Release order:** the API's startup privilege policy now requires both new tables, so a build containing R1 fails its check on any database without 0025. `db_gated` likewise fails its gate on shared `usapp_test` until 0024 and 0025 are applied there, which has not been authorized.
- **Live HTTP unverified** on the new routes (17.1).
- The contract's pre-existing duplicate `/api/v1/auth/sessions` key is repaired, and the contract now parses strictly (17.8). OpenAPI semantics remain unvalidated.
- Direct SQL can supply `created_at` in the past (tests use this); the API never does. The guard only refuses the future and non-monotonic values.
- `ALTER TABLE notifications` takes an exclusive lock and validates a constraint over existing rows at apply time; duration on real data is unmeasured.
- The occurrence guard and read functions have no production caller until R2. Their guard is tested by direct inserts, without a concurrent generator.
- The other items of Section 16 stand.

### 17.8 R1 review repairs (2026-11-02)

**Defect 1:** `next_fire_at` could name an occurrence identity that had already fired. After a time change it returned a future instant the no-refire rule would never generate. **Repair:** additive `database/migrations/0026_reminder_next_fire.sql` replaces `usapp_reminder_task_next_fire` and `usapp_reminder_anniversary_next_fire`, and nothing else. 0025 is unchanged.

| Target | Identity excluded | Result |
|---|---|---|
| One-off, custom | Owner + task | NULL once fired |
| Daily, weekly | Owner + root + `period_date` | Advances to the next period |
| Anniversary | Owner + anniversary + `target_date` + `days_before` | Other unfired offsets and later yearly targets stay |
| Countdown | Same | NULL once its only target fired for all current offsets |

**Defect 2:** the two `/api/v1/auth/sessions` keys in the contract are merged into one path object holding `post` (`createSession`) and `delete` (`deleteSession`). Only the second key line and the blank line before it were removed. Contract version stays 0.21.0, since behaviour did not change.

**`0026_reminder_next_fire`: FROZEN**, sha256 `d280d9d83a5c8fe8c34035504c6243a0a7c27f6796922d95a8f7261118714e1c`. First committed in disposable project `usapp-p26r1r-20261102212339-5719ee`, then in the final run. **Not** on shared `usapp_test` ([Doc] 0000-0023) or main ([Doc] 0000-0017). Not deployed. Further corrections need a new migration.

**Strict YAML parse** (`Claude outputs/p26r1-yaml-check.py`): the installed Docker Compose v5.5.1 loader, used as a parser only. The contract is embedded unchanged under an `x-` field; nothing is started. This corrects the "no parser available" statement in 17.4.

| Input | Result |
|---|---|
| Negative control, duplicate path key | Exit 1, `mapping key "/a" already defined` |
| Contract before the merge | Exit 1, duplicate `/api/v1/auth/sessions` |
| Contract after the merge | Exit 0; OpenAPI 3.1.0, 0.21.0, 54 paths, 63 operations; that path has `delete` and `post` with both operationIds and their response sets (200, 400, 401, 403, 429; 204) |

**Verification** (`Claude outputs/p26r1-repair-verify.ps1`; final run `usapp-p26r1r-20261102212827-583a0d`; complete logs saved before filtering in `%TEMP%\<project>\`). An earlier run of the same flow, `usapp-p26r1r-20261102212339-5719ee`, also passed; afterwards the only source change was removing an unused test struct and correcting two test comments, and a pre-repair check was added to the script. The final run covers the final bytes.

| Check | Result |
|---|---|
| Representative data | 0000-0025 with the R1 image's `db_gated` (2669 assertions): 23 notification event types, 51 reminder rules, 1 occurrence |
| The new SQL regression on the 0025 functions | Fails as intended: exit 3, `fired one-off offered again` |
| Rollback prevalidation of 0026 plus the SQL test | 60 cases as `postgres` and `usapp_api`. Afterwards not registered; function md5s and rows unchanged |
| Function review against 0025 | `task_next` 2 lines removed, 11 added; `anniversary_next` 1 removed, 6 added: only the exclusions, their comments, and re-terminated conditions |
| Apply | Exactly 0026 |
| InitRoles and privilege deviation, twice | 0 deviation rows both times; repeat skipped 27, applied 0 |
| Checksums | 27 registered, all equal their local files, 0022-0025 at their frozen hashes |
| Existing rows after 0026 | Notification per-type counts and reminder row counts unchanged |
| Targeted next-fire SQL, first | 60 cases in each role, ROLLBACK |
| CTest, 4 targets, unchanged limits | 100%, exit 0, 32.24 s: unit 2137 assertions 11.19 s; db_gated 2688, 10.58 s; push_dispatch 391, 10.45 s; static_check 492, 0.01 s |
| Affected SQL scripts | Dispatch in both modes; reminder, notification, choice, anniversary and task ledgers in both roles |
| Control state, credentials in logs | disabled; 0 |

**Regressions.** SQL, rollback-only, each occurrence with its required notification: one-off and custom NULL; daily next day; weekly next week's Sunday; anniversary with an unfired offset preserved, then the later yearly target; countdown NULL. API, committed fixtures through the service: the same cases except custom (SQL only), reading `current.next_fire_at` from the create responses and `GET /api/v1/reminders` at service level, plus `fired_this_period` where a period fired, and SQL function agreement for daily.

**Exact remaining limitations**

- Live HTTP on the reminder routes is still unexercised.
- The pre-repair check shows the first new SQL assertion failing on the 0025 functions, because the script stops there. The others were reviewed by reading, not seen failing individually, and the API regressions did not run against 0025. Mutation testing was not run, as instructed.
- A replay of a fired-identity save was not separately asserted by these regressions.
- The fixtures read the Beijing date several times: a run straddling Beijing midnight could fail spuriously.
- The API's startup policy does not check for 0026. A build on a database with 0025 but not 0026 starts and returns the old values; only the `db_gated` gate requires 0026. `db_gated` still fails its gate on shared `usapp_test` until 0024-0026 are applied there, which is not authorized.
- The "next period is never fired" reasoning relies on occurrences existing only after their instant. R2's generator must keep that; no generator exists yet.
- The YAML check proves parsing and the named operations, not OpenAPI semantics.
- The `db_gated` abort did not recur and remains unexplained; this is not evidence of a fix.

**Resources.** Created by these runs, retained stopped, nothing removed: 3 containers and 1 internal network for each of the two projects above, and image `usapp-isolated-test-runner:p26r1r`. The shared and main databases were not contacted. R2-R5 were not started.

## 18. R2 implementation and isolated verification (Claude Code, 2026-11-02)

Owner authorization: R2 implementation and disposable-isolated verification only. R3-R5 are not started. **No new migration**: 0000-0026 are unchanged and suffice. Not on shared `usapp_test` ([Doc] 0000-0023) or main ([Doc] 0000-0017). **NOT DEPLOYED.** `REMINDERS_ENABLED` defaults to false.

### 18.1 What R2 adds

- `services/ReminderGeneration.{h,cc}`: the generator, as five statements on the scheduler's transaction: preflight, scan, lock, insert occurrences, insert notifications. `scheduler/jobs/ReminderJob.{h,cc}`: key `reminder.generate`, registered before `push.dispatch`. Nothing references Push, subscriptions or devices.
- `REMINDERS_ENABLED` (`AppConfig`, `.env.example`, `deploy/docker-compose.yml`), default false. Off, the tick reports `dispatch=off` and writes only the scheduler's own tick row. Registering writes nothing.
- **Fail closed without 0026:** when on, preflight requires both the `schema_migrations` row and both repaired function bodies. Otherwise the tick fails as `reminder_generate stage=preflight_missing_0026` before reading or writing anything else. Preflight also refuses anything but READ COMMITTED, since re-evaluation relies on a new snapshot per statement.
- **`TxScope::beginAsync`, used by `Scheduler::runJob`** (18.4). Other `TxScope::begin` callers are unchanged.

### 18.2 Decisions while implementing

| Item | Behaviour |
|---|---|
| One definition | The scan and the insert use the same SQL text for "due now". The insert takes facts from its own evaluation; a candidate only selects by identity, rule revision and instant. Anything else is dropped, never moved to another period |
| Lock order | One statement: sorted 33004, then 33010, then sorted 33017 |
| Bounded lock wait, **added beyond the design** | `lock_timeout` 3 s, local to the tick's transaction. A stuck holder fails the tick with a fixed label, which releases the roots it already held, and the next tick retries. Please review |
| Test-only scan clock | `Policy::scanAtForTest` sets only the scan's clock; insertion and the guards always use the real clock. `static_check` confirms `main.cc` never sets it |
| Per-row queries | None from the job. The frozen 0025 occurrence guard, notification guard and deferred link do run per row |
| Multi-offset | **Correction to Sections 9.1 and 13:** one anniversary has one date per year, and targets are `today + offset`, so two offsets of one rule can never be due in the same tick. The "three offsets due together" case cannot occur. Bulk fixtures use rules with two offsets, one matching |

**Write paths rechecked ([Fact]):** `TaskCompletionService`, `TaskChangeService`, `AnniversaryService` and `ReminderService` take member FOR SHARE, then the domain lock, and none locks a `tasks` row. Nothing in `services/api/src` or the migrations updates or deletes `couple_members` or updates `tasks` or `anniversaries`, which settles the Section 11 assumption. `task_completions` has one row per task, written only on completion, so partial progress does not suppress. Anniversaries have no deletion.

### 18.3 Verification

`Claude outputs/p26r2-isolated-verify.ps1`, project `usapp-p26r2-20261102221930-249b31`, RESULT: PASSED. Complete logs and exit codes were saved before filtering, in `%TEMP%\usapp-p26r2-20261102221930-249b31\`. Generated credentials only; no ports; PostgreSQL on tmpfs.

| Check | Result |
|---|---|
| Representative data | 0000-0025 with the R1 image's `db_gated` (2669 assertions): 23 notification event types, 52 rules, 1 occurrence |
| **Real database without 0026** | 9 assertions, exit 0: off writes nothing; on fails closed with the fixed label; occurrence and notification totals unchanged; one failed tick row |
| Apply frozen 0026, InitRoles and privileges, twice | Exactly 0026; 0 deviation rows both times; repeat skipped 27, applied 0 |
| Checksums | 27 registered, all equal local files; 0022-0026 at their frozen hashes |
| Targeted first | Reminder SQL, 60 cases in each role; R1 and R2 suites 408 assertions, 0 failed, exit 0, 3 s |
| Full CTest, 4 targets, unchanged limits | 100%, exit 0, 33.33 s: unit 2137 assertions 11.20 s; db_gated 2917, 11.71 s; push_dispatch 391, 10.41 s; static_check 511, 0.01 s. No separate target was needed |
| SQL scripts | Dispatch in both modes; reminder, notification, choice, anniversary and task ledgers in both roles, all ROLLBACK |
| After the run | 560 occurrences, 560 notices; 0 without exactly one notice; 0 push rows for reminder notices; control disabled; 0 credentials in logs |

**DB-gated block 31, real scheduler and services, fixtures in fresh spaces.** Every item below passed in that run:

| Area | Covered |
|---|---|
| Concurrency | A second tick overlapping the first is skipped by the job lock; retry creates nothing; two generators **without** the job lock, both queued behind a held root: created 1 in total |
| Rollback | Real scheduler, generation succeeds and then the job fails: no occurrence, no notification, failed tick recorded, retry fires once |
| Races, both orders, real services | Completion, disable and anniversary date edit, using `pg_blocking_pids` waits, no sleeps. Write first: the tick scanned 1, created 0. Tick first: created 1, the write then succeeds, no deadlock |
| Lock order | While 33017 is held, the tick holds the target's 33004 and waits. A stuck holder gives `stage=lock`, nothing written, then a clean retry |
| Midnight | A real tick whose scan clock was yesterday 23:45: candidate dropped, nothing written, no period row |
| Tasks | One-off, daily and weekly fire once; period identity; no period rows; partial progress and pending edit and cancel fire; completion, cancelled, superseded, expired and past-grace do not; a completed period leaves the next one due |
| Rules | Identical re-save fires; disable, re-enable and saved-after-instant do not; no refire after a time change that is already effective |
| Anniversaries | Cosmetic edit fires, with versions recorded; date change, away and back, and kind change do not; add, remove and re-save of offsets; disable and re-enable; countdown; yearly and Feb 29 tables and year boundary, through the scan's clock |
| Drain | 250 due across two members and both kinds: 100, 100, 50, 0, each tick's exact identities asserted against the scan's total order; 250 notices. Then 100 ineligible plus those 250 fired: exactly 1 scanned and created |
| Isolation | Owner-only inbox and unread count; partner and other space see nothing; each member's own rule on one item; a foreign owner refused |
| Independence | No subscription, control disabled: inbox and unread count work, no push rows |

### 18.4 Failures and repairs

| Found by | Problem | Repair |
|---|---|---|
| Targeted run, `usapp-p26r2-20261102215025-a306d6` | The case waited for the whole second tick while the first tick was held open | Test reads the skip as soon as `reminder.generate` reports. Ticks still overlap |
| **Hang**, `usapp-p26r2-20261102215424-c10a84`: over 19 minutes in a direct run with no timeout. Diagnostics in that project's `hang-diagnostics\` | **Product defect, predating R2:** `TxScope::begin` uses Drogon's blocking `newTransaction()`, and `Scheduler::runJob` called it inside the previous job's commit or rollback callback, on the event-loop thread that must finish that work. Main and both `DbLoop` threads were in `futex` waits. PostgreSQL had no lock waits or advisory locks, one connection `idle in transaction` with its rollback unsent, and one idle after commit. It needs two overlapping ticks in one process and a pool of 2; the automatic loop does not overlap ticks | `Scheduler::runJob` uses `TxScope::beginAsync`, which waits in the pool's queue. The overlap test is unchanged and now passes. `static_check` guards it |
| Same hang | A test executable run directly had no time limit | The script bounds direct runs at 30 s, the `db_gated` CTest limit; on expiry it saves output, activity and threads, then stops only that container. **This branch has not fired since it was added** |

**Lock holding (measured, fixture scale, shared Docker host):** a 100-row tick held its locks 22 ms in the targeted run and 26 ms in CTest (wall 39 and 46 ms), commit excluded. A real write queued behind a 100-row tick returned 51 and 49 ms after the blocking holder released. These are two samples each, on a small database, not a production measurement.

**`db_gated` abort:** did not recur. It remains unexplained; this is not evidence of a fix.

**Mutation testing:** not run, as instructed.

### 18.5 Resources

Created by these runs, stopped and retained, nothing removed: the containers and internal network of `usapp-p26r2-20261102215025-a306d6`, `-20261102215424-c10a84` and `-20261102221930-249b31`, with logs under `%TEMP%\<project>\`; image `usapp-isolated-test-runner:p26r2`. Test fixtures lived only in those instances' tmpfs databases. Shared and main databases and existing application services were not contacted.

### 18.6 Exact remaining limitations

- **Not run in the real process:** the API binary was not started. `REMINDERS_ENABLED` wiring and registration order in `main.cc` are static checks only, and the job was driven by `tickOnce`, not the automatic loop. "Startup writes nothing" is shown for constructing and registering the jobs, not for a full startup.
- **Midnight is simulated** by the scan's clock parameter. No run crossed a real midnight. If a candidate goes stale between the insert's evaluation and the guard, the guard raises and the whole tick fails and retries; this narrow path is unexercised.
- **`ON CONFLICT DO NOTHING` was not reached:** under the locks, the second generator's fresh snapshot excludes the identity first. The fail-closed test covers an unregistered 0026; the branch for registered but unrepaired functions is not separately exercised.
- **Enabling is not yet safe for a real user:** until R5, with Push also enabled, a reminder would fan out with no stale fence. (Web labels came with R3, rule copying with R4, the fence with R5, Section 21.)
- The scan reads every current rule each tick (Section 16). Its cost at real scale, and the blocking `TxScope::begin` used by other callers under real load, are unmeasured.
- Tests with due fixtures can fail spuriously across Beijing midnight or at a due fixture's time of day (such as 23:30). Both test-only `db_gated` variables bypass or narrow the run, but neither bypasses the database-name guard.
- `db_gated` fails its gate on shared `usapp_test` until 0024-0026 are applied there, which is not authorized.

## 19. R3 implementation and automated verification (Claude Code, 2026-11-02)

Owner authorization: R3 frontend, automated tests and documentation only. R4 and R5 are not started. No backend, contract, schema, migration, grant, scheduler, dependency or deployment-config change. **No browser or device acceptance has been done**, no web bundle was built or served, and nothing is deployed. `REMINDERS_ENABLED` remains off.

### 19.1 What R3 adds

- `api/reminders.ts` (client, types), `api/reminder-rules.ts` (schedule bounds, wording), `stores/reminders.ts` (reset at the existing identity boundary), `components/ReminderControls.vue`, used in the task detail and the anniversary detail. No new navigation.
- `notification-labels.ts` gains the two fixed labels of Section 10. `hasValidActor`: a null actor is accepted only on `reminder.task` and `reminder.anniversary`, which must have one; any other family needs a non-empty actor. A list page that breaks this is an error. This validation is new: the store previously checked no actor at all.
- `REMINDER_*` error codes and messages.

### 19.2 Behaviour

| Item | Behaviour |
|---|---|
| Target | The task API's `id` is always the root (`COALESCE(recurrence_id, id)`; [Fact] `TaskService.cc`), and `period_date` names the period, so the control always sends the root |
| Ownership | Task: assignee only, and the control is not rendered for the creator. Anniversary: each member, for themselves |
| Default | Off. Opening a page only reads. Nothing says "off" before a complete read: an incomplete read shows an error and offers no write |
| Closed targets | Cancelled or superseded: read-only. Completed or expired one-off or custom: disable only. A recurring root stays configurable after one finished period |
| Successor | At R3: stated that the reminder does not carry over. **Superseded by R4:** the old root now says the setting was carried over as it was (Section 20) |
| Current state | Only `current` enters the store; `mutation` is used only to check the action. A replay of an older save shows the newer rule, with a notice saying so |
| `next_fire_at`, `fired_this_period` | Shown as "calculated by the current settings". Null shows that there is no next time. When fired, states that changing the time will not repeat it |
| Scope wording | In-app, Beijing time, private, unrelated to Push, phone delivery not promised |

**After an unknown attempt** (order from `ReminderService.h`):

| Response to the retry | Result | Why |
|---|---|---|
| Replay or success with matching facts | Done | Confirmed |
| `VALIDATION_ERROR`, 401, 403 (CSRF, Origin, `REMINDER_NOT_ASSIGNEE`), 404, 429, offline, no token | Stays unknown, same key and body | Rejected before the key is classified, so it proves nothing about the earlier attempt |
| `REMINDER_TARGET_CLOSED`, `REMINDER_TIME_PASSED`, `REMINDER_TIME_INVALID`, `IDEMPOTENCY_KEY_REUSED` | Ends, with a notice | Reached only after classification found no write of this body under this key |

`REMINDER_TIME_INVALID` also exists before classification, only for a period id. That is immutable, so such a request can never have written, and this client sends only root ids.

**Stale reads:** a read cannot replace a higher seq, nor an equal seq if it started before that target's latest confirmation, since its derived fields may predate it. Superseded reads and all responses after an identity change are dropped. **Pagination:** one list per opened detail, reading every page; task cards read nothing. A failed page, malformed row or repeating cursor is an error, never a short list.

### 19.3 Verification

Vitest and TypeScript on this workstation; the logs are in `Claude outputs/p26r3-logs/`, saved in full before filtering.

| Check | Result |
|---|---|
| Targeted first, `tests/unit/reminders.spec.ts` | 104 tests passed, exit 0 |
| Full web unit suite, finished tree | 31 files, 546 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json` | Exit 0 |
| `tsc --noEmit -p tsconfig.node.json` | Exit 0 |

Covered: schedule bounds at their limits and one past them; ownership; exact disable body; old-key replay under newer current state; retry matrix above; 22 malformed write responses and 4 malformed list pages; stale and superseded reads; account and target changes, including the keyed remount the pages use; pagination; task and reward pending actions byte-identical after reminder reads and saves, with no key shared; null actor; labels; inbox rendering without read marking; service worker payload, label and destination helpers.

**Failures found and fixed while testing:** a number input's `v-model` yields a number, which crashed the draft parser, and the pages' key-based remount left a target's notice to reappear on return. Both are fixed, with tests.

**Review repair (2026-11-03):** `isUsableReminderResponse` accepted `mutation` and `current` with equal `seq` and `rule_id` even when their immutable facts contradicted each other, so a contradictory `current` could reach the display as confirmed. At an equal `seq`, all thirteen immutable facts must now agree: `action`, `rule_id`, `seq`, `target_kind`, `target_id`, `enabled`, `mode`, `before_minutes`, `days_before`, `at_time`, `at_local_date`, `offsets`, `created_at`. Derived fields are not compared. A contradiction stays unknown with its original key and payload and stores nothing, and a valid same-key retry resolves it. A higher `current.seq` may still carry any settings, and those are what is displayed. [Fact] Both objects come from the same SQL column list in `ReminderService.cc`, so a real equal-seq pair agrees by construction; this has not been run against a real API.

**Not run, as instructed or out of scope:** backend, SQL and worker suites (no frontend finding required them); mutation testing; a production web build.

### 19.4 Exact remaining limitations

- **Browser and device acceptance is pending and separate.** Tests use jsdom with a mocked API: no real browser layout, native date and time inputs, phone viewport, installed service worker, or real API call.
- **Release prerequisite, closed by Section 22; at R3:** no API exposes whether `REMINDERS_ENABLED` is on, and the frontend neither hard-codes service availability nor has an endpoint for it. With it off, as it is everywhere today, `next_fire_at` still shows a computed time at which nothing will fire. The wording says "calculated", but a user could still reasonably expect a reminder. **These controls must not be released to real users until this visibility gap is resolved.**
- **API gap, minor:** there is no single-target read, so opening a detail reads the whole list (at most two members' own rules).
- The contract's descriptions still say nothing is generated in this version, which R2 made stale. The contract was out of scope here. (Corrected in Section 22.)
- `reminder.task` opens `/tasks`, as other task notices do, not the specific task.
- An unresolved action stays until it is confirmed or the identity changes; like other stores, there is no abandon. A retry that keeps receiving a pre-classification rejection, such as 404, cannot end it.
- Until R5 (now Section 21), with Push also enabled, a stale reminder has no fence. An old service worker shows the generic text for these events.
- Backend limits in Section 18.6 stand. The `db_gated` abort remains unexplained.

## 20. R4 implementation and isolated verification (Claude Code, 2026-11-03)

Owner authorization: R4 implementation and disposable-isolated verification only. R5 and rollout remain unapproved. **NOT DEPLOYED**, and `REMINDERS_ENABLED` stays off.

### 20.1 State, stated exactly

| Item | State |
|---|---|
| `0027_reminder_successor_rules` | **FROZEN**, sha256 `fbf5f49eeff50466ab58d016c8e1bf3a50a15d46e3896be4d8e9bcd5618fff94`. First committed in disposable project `usapp-p26r4-20261103014827-70917f`, then the passing run `usapp-p26r4-20261103015123-ebfd4d`. **Not** on shared `usapp_test` ([Doc] 0000-0023) or main ([Doc] 0000-0017). Further corrections need a new migration |
| Contract | 0.22.0: `ReminderRuleCurrent` gains the derived `origin` and `needs_reset`. `mutation` is unchanged, and no endpoint is added |
| Web | Inherited and needs-reset wording and validation only. **No browser or device acceptance** |
| R5 | Not started |

### 20.2 Behaviour

- **Copy:** in the approval's transaction, after the closure and before the audit, one statement copies the assignee's own latest rule, enabled or disabled, exactly as it is. No rule means no copy. Nothing comes from the request, so neither proposer nor approver can choose the settings. The predecessor and its rules are not changed, and occurrences, notifications and fired state are not copied: the successor is a new identity (O7).
- **Provenance:** `origin = 'successor'` with `source_rule_id` and `source_proposal_id`. The guard requires an approved edit decision naming exactly this successor, the assignee as owner, the predecessor's latest revision as source, and identical terms. CHECK and unique constraints restrict a copy to the first revision and one per decision. A deferred trigger refuses a supersession that commits without the copy of an existing rule.
- **`needs_reset`:** one SQL definition (`usapp_reminder_rule_needs_reset`), used by the read, the generator's eligibility and the occurrence guard, so they cannot disagree. True for an enabled copy that does not fit the new terms or whose instant had passed when copied. Approval still succeeds, the choice is kept, `next_fire_at` is null and nothing generates. The next valid save by the user is an ordinary validated write that clears it. A usable copy does not later become `needs_reset` when its instant arrives.
- **Lock order:** member FOR SHARE -> 33004 on the root -> decision, successor, closure -> copy (33004 on the uncommitted successor, then 33017) -> audit. A save committed first is the one copied. A save reaching the root afterwards finds it closed: `REMINDER_TARGET_CLOSED`.
- **Replay:** classification precedes everything, so a replay appends nothing and does not re-copy.
- **Failure:** a copy failure fails the whole approval. No wallet write is involved.
- **Generator:** preflight now also requires 0027, failing closed with `stage=preflight_missing_0027`.

### 20.3 Verification

`Claude outputs/p26r4-isolated-verify.ps1`, final project `usapp-p26r4-20261103015123-ebfd4d`, RESULT: PASSED. Generated credentials only; no ports; PostgreSQL on tmpfs. Output reaches the logs line by line, in `%TEMP%\<project>\`. Direct test executables have a hard 30 s deadline, the `db_gated` CTest limit, and the whole CTest command has 120 s. On expiry, output, state, threads, activity and locks are saved, then only that container stops. **No deadline fired in these runs, so that branch is unexercised.**

| Check | Result |
|---|---|
| Representative data | 0000-0026 with the R2 image's `db_gated` (2917 assertions): 24 notification event types, 572 reminder rules, 279 generated occurrences |
| Real database without 0027 | 9 assertions, exit 0: off writes nothing; on fails closed with the fixed label; totals unchanged |
| Rollback prevalidation of 0027 and the SQL tests | 78 cases as `postgres` and `usapp_api`, exit 0. Afterwards: not registered, no new columns, function md5s and rows unchanged |
| Function review | Occurrence guard 0 lines removed, 4 added; next fire 0 removed, 2 added; rule guard 6 removed, 37 added: the old origin refusal and its comment, plus the problem check re-indented under `origin = 'user'` |
| Apply, InitRoles and privilege deviation, twice | Exactly 0027; 0 deviation rows both times; repeat skipped 28, applied 0 |
| Checksums | 28 registered, all equal local files; 0022-0027 at their frozen hashes |
| Existing rows after 0027 | Notification per-type and reminder row counts unchanged |
| Targeted first | Reminder SQL, 78 cases in each role; R1, R2 and R4 suites 523 assertions, 0 failed, exit 0 |
| Full CTest, 4 targets, unchanged limits | 100%, exit 0, 32.89 s: unit 2137 assertions 11.20 s; db_gated 3032, 11.56 s; push_dispatch 391, 10.12 s; static_check 517, 0.01 s |
| SQL scripts | Dispatch in both modes; reminder, notification, choice, anniversary and task ledgers in both roles, all ROLLBACK |
| After the run | 840 occurrences, 840 notices; 0 without exactly one notice; 0 push rows for reminder notices; control disabled; 0 credentials in logs |
| Contract strict YAML (`Claude outputs/p26r1-yaml-check.py`) | Negative control refused; contract parsed: 0.22.0, 54 paths, 63 operations |
| Web, logs in `Claude outputs/p26r4-logs/` | Targeted 112 passed; full suite 31 files, 554 tests passed; `vue-tsc` and `tsc` exit 0 |

**DB-gated block 32, real services:** enabled, disabled and no-rule copies with exact terms, provenance and an unchanged predecessor; a fired daily predecessor copying no occurrence, notification or fired state; daily and weekly successors; unusable copies from a removed deadline, a passed instant, an added deadline under `absolute`, and a custom range starting after the instant, each with approval succeeding, the choice kept, null next instant, and the scan empty at the instant where one lies ahead; a due rule approved and generating nothing; setting again; replay after the successor rule was edited; reject, withdraw and cancel copying nothing; rule save against approval in both orders using `pg_blocking_pids`; a copy refused by the guard rolling back decision, closure and successor, with no wallet write and a clean same-key retry.

**SQL, both roles:** a rejected decision as provenance, wrong owner, changed terms and enabled state, an older source revision, another target, a later revision, an anniversary target, an unknown origin, provenance on a user rule, a supersession without its copy, a second copy, and an occurrence on a rule needing reset, whose refusal message is asserted.

**Forced failure:** `TaskChangeService::Fault::RuleCopyRefused` is a test-only argument that makes the copy statement ask for a revision the database guard refuses. Production calls never pass it.

### 20.4 Failures and repairs

| Found by | Problem | Repair |
|---|---|---|
| Prevalidation, `usapp-p26r4-20261103013949-4597ba` | My SQL test: PL/pgSQL ended an `IF` at the `THEN` inside a `CASE` | Parentheses. Exit 3; nothing committed |
| Prevalidation, `usapp-p26r4-20261103014240-bcf556` | My SQL test expected the guard's code for a second copy; the CHECK refuses it | The test expects the CHECK, still requiring refusal. Exit 3; nothing committed. The script's post-rollback check did not run on these two failures, since it stopped first |
| CTest after first committed application, `usapp-p26r4-20261103014827-70917f` | A new static check compared source positions, while `copyReminderRule` is defined above its caller | It now checks the call site, in the closure's success callback. **Test only; 0027's bytes did not change** |
| Owner safeguard | Harness output and deadlines | Line-by-line logs and hard deadlines, above |

The `db_gated` abort did not recur. It remains unexplained; this is not evidence of a fix. Mutation testing was not run, as instructed.

**Lock holding** was re-measured on the final tree, at fixture scale: a 100-row tick held its locks 33 and 39 ms. A write queued behind it returned 29 and 36 ms after release. Approval's own added hold is one statement and was not separately measured.

### 20.5 Resources

Created by these runs, stopped and retained, nothing removed: containers and internal networks of `usapp-p26r4-20261103013949-4597ba`, `-014240-bcf556`, `-014558-e8362d`, `-014827-70917f` and `-015123-ebfd4d`, with logs under `%TEMP%\<project>\`; image `usapp-isolated-test-runner:p26r4`. Test fixtures lived only in those tmpfs databases. Shared and main databases and existing application services were not contacted.

### 20.6 Exact remaining limitations

- **Release prerequisite, then open, closed by Section 22:** the `REMINDERS_ENABLED` visibility gap (Section 19.4). No capability endpoint was added, as instructed.
- **Release order, stricter than before:** every approved task edit now runs the copy statement, which needs 0027's columns. On a database without 0027, a build with R4 **fails task-edit approvals, and they roll back**. Reminder reads also fail, since they call its function. The startup policy checks tables, not this migration. So 0027 must reach a database before this build does, and 0018-0026 before it.
- **Not run in the real process:** no API binary, automatic scheduler loop, live HTTP, browser or device. Web wording is tested in jsdom with a mocked API, and the `origin` and `needs_reset` fields only reach it through mocks.
- **Hash collision:** approval holds the root's 33004 while the guard takes 33004 on the uncommitted successor, so a hash collision with a root the generator holds could reach a deadlock, which PostgreSQL would detect and fail. The approval rolls back and the tick retries. Unexercised, and accepted as in Section 11.
- The second copy's refusal comes from a CHECK and the first-revision rule from both, so their messages differ; this is deliberate but means no single SQLSTATE covers all forgeries.
- `needs_reset` is measured at the copy's own time, so a copy can be usable and then simply have its instant pass, with no reset notice.
- The contract's descriptions still say nothing is generated in this version; that is stale since R2 and outside R4's limited change. (Corrected in Section 22.)
- Push: until R5 (now Section 21), with Push also enabled, a stale reminder has no fence. Other limits in Sections 18.6 and 19.4 stand.

## 21. R5 implementation and isolated verification (Claude Code, 2026-11-03)

Owner authorization: R5 implementation and disposable-isolated verification with a fake transport only. Rollout remains unapproved. **NOT DEPLOYED**; `REMINDERS_ENABLED` and real push stay off.

### 21.1 State, stated exactly

| Item | State |
|---|---|
| `0028_reminder_push_stale` | **FROZEN**, sha256 `f9a9695fad4fb0019fb9f8094b31f39fca28d4d73ca0ac608d487c19e90eda4b`. First and only committed in disposable project `usapp-p26r5-20261103125314-59cb6e`. **Not** on shared `usapp_test` ([Doc] 0000-0023) or main ([Doc] 0000-0017). Further corrections need a new migration |
| Contract, web, worker | Unchanged. Contract stays 0.22.0. The labels already exist (R3). The worker stays stateless, and it cannot report the new outcome: the transport's closed list refuses it |
| Real push, provider | None used. Stub worker in-process only |

### 21.2 Behaviour

**Source validity** is one function, `usapp_reminder_occurrence_valid(notification, at)`, true for anything that is not a reminder. It is derived from current rows, read only, and takes no lock.

| Source now | Task reminder | Anniversary reminder |
|---|---|---|
| Past `stale_at` (`at >= stale_at`), which is never after the deadline | Stale | Stale |
| Latest rule disabled | Stale | Stale |
| Time or mode changed, rule still enabled | Valid | Valid |
| This offset removed from the current set | n/a | Stale **(decided here; see 21.3)** |
| One-off or custom completed; this period completed | Stale | n/a |
| Partial progress; a pending proposal | Valid | n/a |
| Cancelled or superseded | Stale | n/a |
| Current schedule no longer falls on the target | n/a | Stale |
| Title or note edited | Not possible in place: see 21.3 | Valid |
| Moved away, then back | n/a | Valid, unless a terminal state was already written |

**Pre-send precedence**, every attempt, retries included. The first match decides:

| Order | Fact | Result | Terminal |
|---|---|---|---|
| 1 | Another instance's claim | Nothing written | No |
| 2 | Already observed | Nothing written | No |
| 3 | Expired, superseded or lease over | `lease_lost_skipped` | No |
| 4 | Terminal outcome exists | `terminal_skipped`; the outcome stands | Already |
| 5 | Delivery lifetime reached, or under a second of it left | `ttl_skipped` | `expired` |
| 6 | Before the cutover | `cutover_skipped` | `cutover_skipped` |
| 7 | Control not enabled | `control_skipped` | No |
| 8 | Generation or session inactive | `inactive_skipped` | `inactive` |
| 9 | **Source stale** | **`source_stale_skipped`** | **`source_stale`** |
| 10 | Under a second before `stale_at`, source still valid | `ttl_skipped` | `expired` |
| 11 | Otherwise | Send | |

Rows 1-8 and 11 are unchanged, so a non-reminder cannot behave differently. **Stale and expired together:** the delivery's own lifetime (row 5) wins, which agrees with the tick's own expiry sweep, since that never looks at the source. A paused reminder is not decided at all: the retry after resume is judged against the source at that time. A reminder at `stale_at` itself reports stale, not expired, because row 10 needs the source still valid.

**A stale skip** starts no request, is `internal` so it spends no budget, has no pause reason and revokes nothing. Its terminal row names the attempt, and the guard requires that exact attempt's own `source_stale_skipped`. Only a reminder may record either value. The guards deliberately do **not** ask whether the source is stale at recording time: it may have changed back since the check, and a refused record would let the lease expire into provider budget for a request that was never sent.

**Expiry:** the transport TTL and the absolute expiry are both `LEAST(notice + lifetime, stale_at)`, from the pre-send's database clock, so a retry keeps one expiry and a shrinking TTL. `LEAST` ignores a NULL `stale_at`, so a non-reminder's values are unchanged. Inbox notifications and read state are never written.

**Without 0028:** an enabled or paused tick refuses with `stage=preflight_missing_0028` before writing anything, since otherwise it would claim, fail every pre-send and let leases expire into budget. A disabled control stays idle and writes nothing, so a default-off rollout is unaffected.

### 21.3 Decisions worth reviewing

- **A task title or note edit is stale, by construction.** Tasks have no in-place edit: any approved edit, title alone included, supersedes the task (M6), and a superseded source is explicitly stale. So "a title or note edit stays valid" is achievable only for anniversaries, which do edit in place. A reminder on the predecessor is not sent; the successor has its own identity. If the owner wants a title-only task edit to stay valid, it needs a comparison of the proposal's schedule terms, which is not implemented.
- **Removing an offset is stale.** Section 10 does not list it. It is not a changed time: the user has said not to remind them then. Conservative, and one condition to change.
- **Re-enabling revives a non-terminal delivery**, as moving a date back does. Everything is derived from current rows, and the requirement only forbids resurrecting a terminal delivery.
- **No claim-time or maintenance sweep.** The assignment asks for the final pre-send, and an earlier sweep would write terminal states that a move back should have been able to avoid. A stale reminder therefore takes a claim slot for as long as its pre-send takes. While paused it stays pending, ending as `source_stale` after resume or `expired` at the delivery lifetime.
- **The under-a-second guard ends as `expired`**, as it does for everything else, and a reminder that close to `stale_at` gets no retry.

### 21.4 Verification

`Claude outputs/p26r5-isolated-verify.ps1`, final project `usapp-p26r5-20261103125314-59cb6e`, RESULT: PASSED, exit 0. Generated credentials only; no ports; PostgreSQL on tmpfs. Logs stream line by line to `%TEMP%\<project>\`. Direct executables have a hard 30 s deadline, the CTest limit, and the whole CTest 120 s. **No deadline fired, so that branch is still unexercised.** No test timeout changed, and rounds did not overlap.

| Check | Result |
|---|---|
| Representative data, 0000-0027, from the R4 image | `db_gated` 3032 and `push_dispatch` 382 assertions, 0 failed. 280 occurrences, 604 rules, 24 notification types; 57 deliveries, 74 attempts, 9 expiries, 15 observation outcomes and 7 final states. **None of those deliveries was for a reminder**: the R4 suite makes none. Reminder deliveries appear only inside the rollback tests and after 0028 |
| Real database without 0028 | 8 assertions, exit 0: disabled writes nothing; enabled fails closed with the fixed label, no delivery, no send |
| Rollback prevalidation | Reminder ledger 98 cases and dispatch 84 cases, each in both roles, exit 0. Afterwards: not registered, no function, guard and constraint md5s and all row counts unchanged |
| Function review | Observation guard 0 lines removed, 4 added; outcome guard 0 removed, 7 added |
| Apply, InitRoles and privilege deviation, twice | Exactly 0028; 0 deviation rows both times; repeat skipped 29, applied 0 |
| Checksums | 29 registered, all equal local files; 0022-0027 at their frozen hashes |
| Existing rows after 0028 | Notification, reminder and delivery summaries unchanged |
| Targeted first | R5 push cases 150 assertions; reminder SQL 98 cases per role; reminder suites 564 assertions; 0 failed, all exit 0 |
| Full CTest, 4 targets, unchanged limits | 100%, exit 0, 38.32 s: unit 2153, 11.19 s; db_gated 3073, 11.79 s; push_dispatch 541, 15.33 s; static_check 531, 0.01 s |
| SQL scripts | Dispatch in both modes; reminder, notification, choice, anniversary and task ledgers in both roles, all ROLLBACK |
| After the run | 902 occurrences, 902 notices, 0 without exactly one. Reminder deliveries, all from this run's fixtures: accepted 16, cutover_skipped 2, expired 6, inactive 2, source_stale 18. `source_stale` without its own internal skip 0; skips on a non-reminder 0; revocations from a skip 0; dispatcher control rows with another reason 0. Control disabled; 0 credentials in logs |

**Push pipeline, real scheduler and dispatcher, asynchronous fake transport (`PushDispatchDbTest.cc`):**
- A source change committed between claim and pre-send: rule disabled, real completion, date moved, anniversary rule disabled. Each: no request, `source_stale`, budget 0, control and revisions unchanged, device unrevoked, inbox row identical, no read.
- Time-only rule edit, title and note edit, and a disable reversed before the check: all sent.
- Retry: a valid retry is sent with a smaller TTL and the same expiry; then a stale third attempt leaves provider attempts at 2.
- Date away and back before a retry's check: sent, one occurrence. Checked while away: `source_stale`, and moving back later claims and sends nothing.
- Boundaries with a to-the-second deadline: sent with the window's remainder; under a second, `ttl_skipped` and `expired`; from `stale_at`, `source_stale`.
- Precedence against a stale source: lifetime, pause then resume, lost lease then the next claim, existing terminal, another instance, logout.
- Expiry: real `WorkerTransport` to a stub worker carried `stale_at` as `expires_at_ms` and a bounded `ttl_seconds`. With a shorter lifetime, reminder and message both carry the lifetime.
- Non-reminder: a forged skip is refused, nothing recorded, and it then sends normally. The earlier P2-5 cases all still pass.

**Against real services (`db_gated` block 33):** generated reminders; real completion, approved cancel and edit, a pending cancel, partial and full daily progress, rule service writes, anniversary edits including away, back and to the same day as a countdown.

**SQL, both roles:** microsecond boundaries at `stale_at` for both families, every rule transition, non-reminder and unknown ids; the class rule, family and attempt guards, no budget, no rewrite, no later claim.

### 21.5 Failures and repairs

| Found by | Problem | Repair |
|---|---|---|
| Prevalidation, `usapp-p26r5-20261103124729-56b533`, exit 3 | My SQL test: group H leaves constraints immediate, so the new group's occurrence was checked before its notice existed | The new group sets them deferred. Nothing committed. The script's post-rollback check did not run on this failure, since it stopped first; it ran and passed in `usapp-p26r5-20261103125029-805fba`, which stopped before applying anything |

The committed run passed first time, so 0028's bytes never changed. The `db_gated` abort did not recur. It remains unexplained; this is not evidence of a fix. Mutation testing was not run.

### 21.6 Resources

Created by these runs, stopped and retained, nothing removed: containers and internal networks of the three projects above, logs under `%TEMP%\<project>\`, image `usapp-isolated-test-runner:p26r5`. Shared and main databases and existing application services were not contacted.

### 21.7 Exact remaining limitations and release prerequisites

- **Check-to-send race remains**, as in P2-5. The check reads and releases; a source change committed after it can still be sent, and something handed to a provider cannot be recalled. The label is fixed and holds no task text, and a click reaches the current page, which is authoritative. A longer race is bounded by the expiry the worker enforces.
- **Release prerequisite, then open, closed by Section 22:** the `REMINDERS_ENABLED` visibility gap (Section 19.4).
- **Rollout order:** 0018-0028 reach the database before a build with R5. Without 0028 a build fails closed only while control is enabled or paused; with control disabled it is unaffected. **The startup policy does not check this migration.** So apply it while control is disabled, or a tick fails each time until it is applied.
- **A claim already held when 0028 is missing:** if a build with R5 somehow pre-sends without the function, the check fails, nothing is sent, and the lease expires into one provider attempt. The tick's preflight prevents new claims; only an attempt claimed by an older build during a rolling start could reach this. Unexercised.
- **Rollback:** 0028 is additive and is not reverted. **Reasoned from the code, not run:** an older build on a database with 0028 treats `source_stale` as any terminal state, since it only tests for presence, and sends what remains, but it has neither the fence nor the bound. So a backend rollback with Push and reminders on loses suppression; turn one off first. The reverse pairing fails closed, as tested.
- **Not exercised:** the API binary, the automatic scheduler loop (ticks are driven by `tickOnce`), the real worker process, any provider, live HTTP, service worker, browser or device. The stub worker is in the test process.
- **Representative data had no reminder deliveries before 0028**, as stated above, so no pre-existing reminder delivery met the replaced constraints. The constraints only widen.
- **Clock:** `stale_at` is compared on the database clock at the check. A claim held longer than `stale_at` is decided at its own pre-send, which is the requirement.
- Other limits in Sections 18.6, 19.4 and 20.6 stand.

## 22. Reminder generation status (Claude Code, 2026-11-03)

Owner authorization: close the availability prerequisite of Section 19.4. No migration, dependency, deployment, production configuration change or real Push. **NOT DEPLOYED**; `REMINDERS_ENABLED` stays off.

### 22.1 Behaviour

**API, contract 0.23.0:** every 200 page of the authenticated `GET /api/v1/reminders` carries a required top-level boolean `generation_enabled`, true only when `REMINDERS_ENABLED` and `SCHEDULER_ENABLED` are both on in this process's loaded configuration, the same values the job and scheduler start from. It is **configuration status**: not proof that a tick ran or that a reminder was delivered, and it says nothing about Push. Error responses do not carry it. `next_fire_at` is still computed either way, and the contract now says the field qualifies it. The stale "nothing is generated in this version" wording is gone from both places it appeared.

**Frontend, from a complete read only:**

| Server said, on every page | Status | Next time | Rule visible and editable | Saving an enabled rule says |
|---|---|---|---|---|
| `true` | enabled | Shown, as before | Yes | 提醒设置已保存。 |
| `false` | disabled, with its own line | Hidden | Yes | …但提醒生成目前未开启，暂时不会按它提醒。 |
| Missing, malformed, pages disagree, read failed, not yet read | unknown, with its own line | Hidden | Yes | …但暂时无法确认提醒生成是否开启，不代表一定会提醒。 |

The "only once per period" line is shown only when enabled, because it also implies firing. Neither status line mentions Push, and the status is not used for anything about Push. Only a strict boolean counts: the text `"true"` is unknown. A malformed status never fails the read: rules, pagination and errors behave as before. A status is forgotten on identity change, and a late answer for another account or a superseded read cannot set it. Pending saves keep their original key and body through a status change. A read taken before a save is what its notice reflects; writes do not carry the field.

### 22.2 Verification

| Check | Result |
|---|---|
| `tests/unit/reminders.spec.ts`, targeted | 153 tests passed, exit 0 (earlier mocks now state `generation_enabled: true`) |
| Full web unit suite | 31 files, 595 tests passed, exit 0 |
| `vue-tsc --noEmit -p tsconfig.json`; `tsc --noEmit -p tsconfig.node.json` | Both exit 0 |
| Strict YAML (`Claude outputs/p26r1-yaml-check.py`) | Negative control refused; contract parsed: 0.23.0, 54 paths, 63 operations; exit 0 |
| Disposable stack, `Claude outputs/p26-availability-isolated-verify.ps1`, project `usapp-p26avail-20261103131455-ccb552` | RESULT: PASSED, exit 0. 0000-0028 unchanged, 0028 at its frozen hash; 29 checksums match; 0 deviation rows twice; repeat skipped 29, applied 0 |
| **Real API binary, four processes, real cookie through `AuthFilter`** | 24 assertions each, 0 failed, exit 0: on/on true; on/off, off/on and off/off false. Anonymous, invalid token and bad limit do not expose the field; two pages both state it and paginate as before; `no-store`; a partner's session sees the same status and none of the rules |
| Targeted backend | R5 push 150; reminder suites 564; reminder SQL 98 cases per role; 0 failed |
| Full CTest, 4 targets, unchanged limits | 100%, exit 0, 37.44 s: unit 2170, 11.17 s; db_gated 3073, 11.57 s; push_dispatch 541, 14.69 s; static_check 534, 0.01 s |
| SQL scripts | Dispatch in both modes; reminder, notification, choice, anniversary and task ledgers in both roles, all ROLLBACK |
| After the run | Control disabled; 0 credentials in logs |

Web logs: `Claude outputs/p26-availability-logs-*.log`. Stack logs: `%TEMP%\<project>\`. The `db_gated` abort did not recur and remains unexplained. No deadline fired, so that harness branch is still unexercised. Mutation testing and a production web build were not run.

**Failure before the pass:** `usapp-p26avail-20261103131328-b69876` stopped at creating its network, because **the networks retained from earlier runs have used up Docker's default address pools**. Removing them is not authorized and nothing was removed; this harness gives its own internal network the subnet `10.213.77.0/24`, in a temporary file only. Later disposable runs need the same, or the owner's decision to clean up.

**Resources:** containers and internal network of `usapp-p26avail-20261103131455-ccb552`, including four stopped API and four stopped test containers, retained; image `usapp-isolated-test-runner:p26avail`. Shared and main databases, existing services and real Push were not contacted.

### 22.3 Exact remaining limits

- **`true` promises nothing more than configuration.** The on/on process ran for about a second; no test here asserts that its loop ticked or generated. The automatic loop remains unobserved in a long-running process.
- **The answer is per process and per read.** It is read when a detail opens, so a changed flag shows at the next read, and with differing instances pages may disagree, which shows as unknown. The scope sentence "只在应用内的通知里提醒你" stays, directly above the status line that qualifies it.
- **Rollout pairing:** a new web on an older API shows unknown, which is safe. An older web on a new API ignores the field and still shows the computed time, so the API and web should be released together.

### 22.4 Remaining release prerequisites

The availability gap of Section 19.4 is closed within this scope. Still open before reminders reach real users:

1. **Browser and device acceptance** of the reminder controls, including these status lines: only jsdom with a mocked API so far.
2. **Migrations:** 0018-0028 on the target database, before the build, with Push control disabled ([Doc] shared `usapp_test` 0000-0023, main 0000-0017). Sections 20.6 and 21.7 give the failure modes and rollback.
3. **A separate owner decision and live check** to set `REMINDERS_ENABLED`, observing real generation in the running service.
4. **If Push carries reminders:** real worker, provider and device acceptance (Section 21.7). It is independent of this field.
5. **The unexplained `db_gated` abort**, which is still not diagnosed.
