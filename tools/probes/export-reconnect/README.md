# Export reconnect diagnostic

Run from the repository with `powershell.exe -NoProfile -File tools/export-reconnect-probe.ps1`.
The harness creates a new internal network and fresh tmpfs PostgreSQL database, publishes no ports, generates its own credentials,
and checks full container ids plus an exact ownership label before stopping its containers. Resources and logs are retained.
It never connects to the application's existing databases. The control connection terminates only its own probe session,
matching pid, random-run-local application name, database and role.

Variants: default `plain` submits ROLLBACK after the loss; `-Chain` submits a parameterized query followed by callback-driven
ROLLBACK/check; `-Variant warm` prepares that query before the loss; `-Variant short` uses a small query;
`-Variant watchdog` also requests cleanup independently after two seconds. `-DelayMs` and `-Rounds` control scheduling.
Queries stay alive for the entire run. A diagnostic-only signal handler collects thread backtraces after a 40-second stall.
No debugger installation, ptrace capability or relaxed container security profile is required by the final harness.

**Read `PROBE_EXIT` and all rounds, not the PowerShell exit status.** The wrapper intentionally collects failed probes without
throwing; only `PROBE_COMPLETE` with `PROBE_EXIT=0` means that probe finished. This is not export acceptance: the miniature
check does not implement the service's complete token/lock proof. The real export connection-loss suite remains authoritative.

2026-11-13 observations (host `%TEMP%/<run>/probe.log`):

| Run suffix | Observation |
|---|---|
| `c63e28bb91` | Plain: 12 rounds completed, exit 0. |
| `fc89c06159` | Chain with query kept alive: no query callback within 40 s; driver thread sampled in epoll_wait; outer timeout exit 124. |
| `6f4c095099` | Warm: same stall, exit 124. |
| `a545467bc7` | Watchdog: first round reconnected, but the subsequent round did not finish; exit 124. Not a pass. |
| `ccece8bb0b` | Single-round watchdog completed, exit 0. Does not prove repeated recovery. |

Earlier chain probes had a shorter SQL-string lifetime and were superseded by `fc89c06159`.
An early gdb attempt failed because the image has no gdb; its empty/missing stack is not evidence of a healthy thread.
These observations establish a missing-callback recovery gap, not a proven root cause inside the driver or its timer implementation.

## Owner-thread teardown regression (2026-11-13)

`-Variant teardown -Rounds 1` deliberately drops the external client reference while a bounded callback tail still owns
the driver's internal references. Run `46cc61df2c` aborted with `Resource deadlock avoided`, exit 134.
`-Variant teardown-close -Rounds 100` instead retains the external reference through `closeAll()` on the owner thread;
run `561cdac11b` completed all 100 rounds, exit 0. Every tested client weak reference expired: the control does not retain
those clients to conceal the crash. The query/callback body and scheduling barrier are otherwise the same.
This is a test-client lifetime regression, independent of the export transport-recovery probes above. The API export's
end proof and intentionally retained clients are unchanged.
