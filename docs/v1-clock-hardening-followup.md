# Memory decision clock correction and remaining V1 clock work

The historical failure and initial requirements below are preserved; the completed local verification is recorded later.

## Historical failure and approved correction scope

Observed in full run usapp-e1-ec0c2a5f0a: memory_deletion_db's approval-before-deletion race returned
USAPP_MEMORY_DECISION_TIME (UG005); its expected revision/binding assertions then failed. Personal-notice target/live
checks passed. Do not delete these failed assertions or claim a later passing run fixes the clock issue.

Current guard is in frozen 0036_memory_deletion.sql, replacing 0030's guard. The default decided_at=clock_timestamp() is
evaluated before the BEFORE trigger waits for the memory lock. After the wait, the guard strictly compares the earlier
value with p.created_at and another wall-clock reading. This is the same failure class documented for 0041/0048;
the exact predicate/clock readings in this historical failure were not captured, so its precise cause is unproven.

Before production, reproduce with a deterministic rollback-only controlled-clock test, not by changing the host clock.
Evaluate a new additive migration following 0041's established convention: drop that default, assign omitted/DEFAULT/NULL
time after the existing lock and state/actor/version checks as max(clock, declared causal sources). Supplied timestamps
retain the original strict tests/error; no tolerance, margin, TTL/deadline change, lock-order change or rewritten history.
Keep guard assigned time possibly ahead of a rewound wall clock explicit; do not weaken future-notice Push deferral.

Proof needs both roles, before/default/explicit-null/supplied cases, causal-source dominance, non-time refusal controls,
both deletion/approval orders, full memory suite and combined regression. Extend only after a measured reproduction;
other old media/reminder guards and export transport availability remain separate recorded issues. At this checkpoint
0052 is frozen and the next available number is 0053; do not edit any existing migration. No clock correction was
implemented as part of the mandatory personal-notice feature.


## 2026-11-15 bounded correction: deterministic and complete backend verification passed

The old guard failure mechanism is now reproduced without changing the host clock. Fresh owned run
`usapp-memory-clock-2179d4572a`, on the 53-migration baseline, ran 13 rollback-only controls per role against both
old and draft guards. Application and maintenance roles both observed the expected baseline failures and fixed
results; privilege deviations remained zero after rollback. This establishes the controlled mechanism, not the exact
wall-clock sequence of the historical intermittent failure.

New additive `0053_memory_decision_timestamp.sql` is frozen at SHA-256
`74024a0bf259b619da08f00c1d9ff527d49564cc6e2f1a2fba499c29360b8d3e`.
It drops only the decision column default and replaces only the decision guard. Omitted, DEFAULT and explicit NULL
are assigned `GREATEST(clock_timestamp(), p.created_at)` after the existing lock, deletion, actor and stale-version
checks. Caller-supplied times still use exactly the old strict before-source/future test and UG005 error. No time margin,
historical rewrite, lock-order, grants, TTL, undo deadline, deletion rule or public contract change. Automatic times may
lie ahead of a rewound wall clock; existing future-notification deferral is untouched. Contract stays 0.59.0; 54 frozen
migrations, next 0054. Explicit NULL intentionally now follows the automatic-time convention from 0041/0048.

Controls cover both rewind orders, omitted/default/null, causal dominance, supplied exact/before/future times, creator
versus proposer for reject/withdraw, wrong couple and missing proposal. Fixtures retain normal triggers and complete
revision/proposal photo links; deferred constraints are forced before and after each control. Test-only clock schema,
trigger/default bindings, fixtures and draft replacement are rolled back. The numbered schema's ordinary and concurrent
memory/deletion tests and full backend regression passed as recorded below; no browser/device/production acceptance.

Harness-only correction before execution: the first complete-regression invocation found an extra closing parenthesis
in the added SQL-ledger call; PowerShell rejected parsing before creating resources. Corrected before the real run.
No frozen migration was changed. Other old media/reminder clock guards and export reconnect availability remain open.


Reusable current-schema control runner `tools/memory-clock-preflight.ps1` also passed in
`usapp-memory-clock-283f3ea0cf`: all 54 migration hashes match; both roles see the 13 expected baseline and fixed
outcomes. The old frozen 0036 guard/default is restored only inside the baseline rollback transaction. Guard definition
fingerprint and column-default presence match before/after; privileges are unchanged. This runner no longer requires
checking out the historical 53-migration tree. Controlled-clock tests do not reproduce arbitrary host/network failures.


## Complete backend evidence

`usapp-e1-2cb34d2d2d` exited successfully: all 54 migration hashes unchanged, eraser violations zero, privilege deviations
zero after initial apply, repeat apply and CTest. Both-role fixed controlled clocks and existing memory SQL ledger passed;
financial/security/push ledgers also passed both roles. Build passed; real API 1417 assertions, zero failures; CTest **34/34
in 106.71 seconds**, including memory_deletion_db **1048 assertions** (8.63 s), db_gated (15.38 s) and static checks.
The memory-deletion suite retains both approval/deletion orders and their revision/photo assertions. Case-name comparison
against the final personal-notice run has **599 old cases retained, no removed cases, one new static boundary case**.
Assertion totals can depend on accumulated fixtures; they are not the case-registration comparison.

Postconditions: early/reversed erasure zero, ledger chain breaks zero. Export connection-loss 8, large-data 6,
closure 23, closed album 7 and closed memory 8 passed. API export-content marker count zero. The database was CLOSED
by the closure test; owned containers stopped and retained. This is fakes and owned disposable infrastructure only.
No frontend or public-contract change, so frontend/provider/device checks were not repeated and are not claimed.
Separate current-schema baseline/fixed rollback proof also passed in usapp-memory-clock-283f3ea0cf. The historical
failure's exact host readings remain unknown; only the controlled failure mechanism has been proved and corrected.

Evidence allowlist: `Claude outputs/memory-clock-20261115`; no environment files, raw server logs, keys or dumps.
Independent 54-migration backup/restore passed in usapp-backup-probe-f02a7deef0 (details below). Other legacy clock guards,
export half-open/reconnect availability, journal/provider/restore/release gates remain open; no deployment or commit.


## Independent backup/restore evidence

`usapp-backup-probe-f02a7deef0` passed on synthetic data, with encrypted/signed backup and a second fresh restore
instance. All 54 migration hashes, financial fixture, no-space personal login receipt, eraser role, ledger and privilege
checks passed. A new explicit assertion confirms the restored memory decision column has no default and the restored
guard contains the causal `GREATEST(clock_timestamp(), p.created_at)` assignment. Nonempty/app-owned target and source
file cap refusals remain tested. API/push CONNECT stays denied; ephemeral test key files removed and owned containers
stopped/retained. This is database-only, not independent journal/object replay or production release acceptance.

Completed bounded batch. No numbered migration was edited after freezing, no contract change, deployment or commit.
Next V1 work remains safe admin operational capabilities and the separate journal/provider/backup scheduling/custody/
off-site/browser/device/release gates. Other old clock guards and export availability limits remain recorded.
