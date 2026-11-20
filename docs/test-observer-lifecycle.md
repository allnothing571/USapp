# Test observer client lifecycle correction (2026-11-13)

## Problem and evidence

The previous export correction's run `usapp-e1-91797e0762` passed export checks but failed full CTest 25/26.
Default `db_gated` aborted at the M6/M7 boundary with `Resource deadlock avoided`. The stack contains `SqlCmd` and
`TaskTimeoutFlag` disposal, `DbClientImpl` destruction, and `EventLoopThread` destruction on a database event loop.

`wallet::Observer::connect` returned a short-lived single-connection client with a query timeout. A synchronous query or
a test promise becoming ready does not mean Drogon has finished disposing its callback/timeout objects. Dropping the
external owner can leave the driver's internal timeout object with the final client reference; dropping that reference
on DbLoop then attempts to destroy/join the same thread. Observers finish at suite boundaries and also serve as the
independent holder connections used by races.

Controlled disposable probe, with a bounded callback tail and no application data:

| Run | Control | Result |
|---|---|---|
| `usapp-reconnect-46cc61df2c` | Drop external client while callback still owns its internal references | `Resource deadlock avoided`, abort, `PROBE_EXIT=134` |
| `usapp-reconnect-561cdac11b` | Keep the external reference through owner-thread `closeAll`, then reset | 100 rounds, every tested weak reference expired; `PROBE_COMPLETE`, `PROBE_EXIT=0` |
| `usapp-reconnect-e0cd693821` | Final probe, including explicit database-name guard, same safe-close control | 100 rounds, all weak references expired; exit 0 |

The same failure class is reproduced deterministically. This does not establish that every historical client crash had
the same owner or that the independent export reconnect stall is fixed.

## Correction

Only the **test observer** owns a shared lifetime anchor. The anchor holds a strong client reference and invokes
`closeAll()` on its final test-owner thread before releasing that reference. The ordinary public client member is declared
after the anchor, so it is destroyed first. Observer copies share the anchor; replacement closes the replaced anchor while
its client reference remains alive. Existing observer/holder code keeps its own connection, timeout and server-state
checks. No connection pool, statement, assertion or transaction policy is replaced.

The installed driver's `closeAll` disconnects each connection while the caller still owns the client; its connection
disconnect queues work on the connection loop and waits for completion when called externally. This gives callback-tail
disposal an external owner. It is a test-owner teardown mechanism, **not** an export transaction-end proof.
See the official [DbClient implementation](https://github.com/drogonframework/drogon/blob/v1.9.11/orm_lib/src/DbClientImpl.cc)
and [PostgreSQL connection implementation](https://github.com/drogonframework/drogon/blob/v1.9.11/orm_lib/src/postgresql_impl/PgBatchConnection.cc).

Observer scopes/copies must remain on test-owner threads, not database callbacks; borrowed client pointers must not be
used after the last Observer owner ends. Current call sites retain Observers through holder transactions. Production
clients and export's deliberately retained dedicated clients are unchanged; there is no new leak or reaper thread.

## Regression and verification

`observer_lifetime_db`, with the existing 30-second limit and database guard, runs separately from default `db_gated`:

- 12 success and 12 error callback tails: immediately end the external owner, require weak-reference expiry and eventual
  server backend disappearance, with bounded polling (client close and server disconnect are distinct events).
- A surviving Observer copy remains usable after the original is reset.
- Move replacement closes the old client; the replacement stays usable and also expires after its final owner ends.

Run `usapp-e1-f6a24ffdab`:

- Build passed; the copied observer helper and new regression header matched workspace hashes.
- New focused suite: 78 assertions, two cases, all passed. Existing targeted admin/export/avatar/achievement suites passed;
  real local HTTP suite 942 assertions passed; exported-content markers in API log zero.
- **Full CTest 26/27 in 102.67 s, harness exit 1.** The previously aborted `db_gated` completed in 14.68 s;
  `observer_lifetime_db` completed in 0.20 s. No client teardown crash was recorded in this full run.
- The remaining failure is `push_dispatch`, cutover/device re-enable case: `USAPP_PUSH_COMMAND_TARGET` during registration,
  followed by an empty generation id reaching a UUID parameter. The guard combines ownership/session and timestamp
  predicates; no predicate-specific diagnostics were captured, so the exact cause is **unproven**. The push observer is
  retained for that entire executable and is not closed at this case boundary. No push code, guard or assertion was changed
  and no blind rerun was used to replace the failure.
- Because the full harness stopped at CTest, its after-CTest privilege/ledger checks and export loss/large/closure modes did
  not run in that database. A **separate** `recovery-89c13487` continuation recreated only that stopped run's tmpfs database
  and checked the compiled snapshot: EXPORT 449, CONNECTION_LOSS 8, LARGE 6, CLOSURE 23 passed. Connection loss released
  after 1,009 ms with `session_replaced` and the next export succeeded. That ordinary-error path does not establish that
  the intermittent missing-callback transport stall has disappeared.
- 48 checksums matched; privilege deviations zero after apply/repeat; 48 workspace migration files remained unchanged.
- No web rerun (no web changes). No browser, device, production, cloud or deployment acceptance. Containers stopped and
  retained; the focused closure database was closed by its test.

Evidence: `Claude outputs/observer-lifecycle-20261113`. The original 25/26 failure is preserved separately at
`Claude outputs/export-recovery-20261113/058.log`. The new focused passes do not replace either failed full-run result.

Status: the reproduced test-observer ownership race is corrected and specifically verified. Full regression remains
**not green** due to the push registration failure; broader client shutdown and export transport recovery are not claimed fixed.

**Follow-up 2026-11-14:** a narrow command timestamp correction (0048) and its controlled-clock comparisons were added in
the next batch. New full run `usapp-e1-e990d56b9b` passed CTest 27/27 and all remaining harness checks. This preserves the
26/27 result above as a failed historical run; it does not establish which predicate failed there. See
[push command timestamp evidence](push-command-timestamp.md).

Scope: no production source, migration, grant, dependency, contract, timeout, web, deployment or commit change.
