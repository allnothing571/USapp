# Deletion startup refusal layer — partial J3c (2026-11-15)

## Implemented scope

The current C++ process has no production durable Journal adapter or independent checkpoint authority. The existing
Python publication/trial tools are not a C++ provider. This batch does not substitute a fake, file, environment assertion
or a restored database marker for that missing authority. It implements a necessary refusal layer, **not complete J3c**.

Before MediaService store configuration, listeners, scheduler startup or beginning-advice bootstrap writes, main now
queries `public.usapp_recovery_closed()` through the existing application role. A confirmed closed state refuses the
whole API process, including its admin listener, even when media and purge are disabled. Missing/null/malformed/error
observations and a five-second callback deadline also refuse. Only fixed reason labels are logged, no raw SQL error.
The asynchronous probe must enqueue nonblocking work; callbacks retain heap state, so a timeout/late or duplicate reply
cannot access a departed stack or change an already returned refusal. No event-loop handler is blocked: this runs
before the application loop. It uses the existing pool, not an additional observer or privileged maintenance connection.

With a confirmed open marker, `MEDIA_ENABLED=true` or `MEMORY_PURGE_ENABLED=true` refuses startup because the durable
journal provider is unavailable. Valid OSS settings are parsed only; no store is configured and no external call is
authorized by them. The production path has no fake-provider or readiness override. This deliberately prevents the old
opt-in real store/purge configuration from being mistaken for an approved release. Already registered route classes
do not make a listening service: neither listener is registered before this check passes.

Both features disabled and a confirmed open marker may still start the existing no-provider process. This does **not**
authorize serving a restored backup: an old backup can lose the CLOSED marker itself, and existing content can remain
in that backup even when new uploads are off. The gate neither detects that case nor fetches an independent head or
restore-incarnation binding. Operators must keep restored databases disconnected/closed under the existing restore
procedure. A successful check is named `AllowedWithoutProvider`, not recovered/verified/ready. Future production
integration must replace this limited acceptance with independently bound authority and validated release proof.

No recovery close/stage/verify/open mutation, history rewrite, grant, migration, dependency, public contract or deletion
policy change. Contract remains 0.60.0; 54 frozen migrations through 0053, next 0054. The pre-existing J3b database
fences/RLS still protect its closed domain if closure commits after the startup query; this one-time check is not a
lease, continuing independent-head check or solution to the open-window problem. There is no reopening operation.

## Verification plan and current state

- Pure unit controls: open/closed/unknown across all four feature combinations, absent/throwing probe, timeout, safe
  late/duplicate replies and fixed reason text. A fake probe never reaches the production main path.
- Real database read-only controls: open marker classification, feature-required refusals and unchanged recovery-table
  row counts. Separate harness-only controls run after a committed closure in another process.
- Real main binary: fully shaped synthetic OSS settings and separately purge enabled, both with scheduler requested,
  must exit exactly 1 with the missing-provider reason, before listener/store/scheduler/bootstrap markers. Both proposed
  ports are probed before and after. A committed closed database must also refuse a new API process, even with both
  feature flags off. Normal no-provider real HTTP still runs in the combined regression.
- Full CTest and frozen-hash/permission/erasure/ledger checks; existing export loss/large/closure and closed-domain modes.

Combined verification passed in usapp-e1-ab46c00368: **35/35 CTest (112.82 s)**, real HTTP **1465**,
open/closed DB controls **9/9 assertions each**, all three real process refusals and their closed-port probes passed.
Whole harness exited successfully; all 54 migration hashes unchanged; privilege/erasure/ledger checks zero. Existing
export loss/large/closure and closed album/memory modes passed. All prior 601 CTest cases retained; four new cases
(two unit, one static, one DB) produce 605 ordinary cases, with no removed case. The old store opt-in static assertion
was updated to its new declaration spelling while preserving parse-before-configure and adding refusal-before-effect checks.

Separate real blocked-query/missing-function probes passed in usapp-startup-fault-41bd74214f. No cloud credentials/resources, provider access, production, actual restore release, real photos,
J4 latest-checkpoint/SDK durability, J5 object+journal replay, browser or device acceptance. No deployment or commit.
The complete provider bridge, independent binding/head freshness and serving-release protocol remain required work.


## Independent real fault evidence

`tools/deletion-startup-fault-probe.ps1` uses a fresh owned database and a fresh compiler/runtime container. It checks the
exact full ID, ASCII owner label and pinned base image of the previously built source container, then copies the already
verified binaries; it does not rebuild or invoke a shared service. The API binary hash in this probe was
`7432e625acdcf58b93c5b580961bf35b36347a5a38e32d6312527e3f2b1f9090`.

A maintenance transaction held a real ACCESS EXCLUSIVE relation lock on the recovery-attempt table for 15 seconds,
then rolled back naturally. The lock was observed as granted before the startup test. Main's real SELECT exceeded its
five-second callback deadline, exited exactly 1 with `recovery_query_timeout`, left both ports closed and never started
the requested scheduler/store/bootstrap. The reported 6333 ms (`...PROCESS_MS` in the original log) measures the whole
refusal-test step including port probes, process launch, file copy and Docker overhead, not the database wait alone.
No backend termination or host clock change. The fixture waited for the blocking lock to disappear before proceeding.

The same probe temporarily renamed the existing recovery function in its disposable database. Actual main exited
exactly 1 with `recovery_query_unconfirmed`; both ports stayed closed. A finally block restored the function name.
Subsequent open-state DB controls (9 assertions) and the privilege matrix passed. Containers stopped/retained. No query
error text, credentials or raw server log is needed in repository evidence. All faults are in owned synthetic resources.

No numbered migration or grants changed. The old dedicated J3b stage/verify recovery-only harness was not repeated;
this batch tested durable CLOSED markers via the ordinary harness's committed export-closure mode and the new checker,
not a full replay. No frontend or contract changes, so their suites were not repeated. After verification only comments,
configuration documentation and status pointers changed; executable implementation remained unchanged.

Completed **refusal layer only**. Evidence allowlist: `Claude outputs/deletion-startup-20261115`. Full J3c, production
journal/independent Source integration, restore binding, ongoing-head/open-window proof and J4/J5 remain unfinished.
No deployment, commit, real keys, cloud/resource operation or restored-data serving authorization.
