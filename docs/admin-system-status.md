# Read-only admin operational snapshot (2026-11-15)

## Scope and boundaries

GET `/api/admin/v1/system/status` adds an authenticated snapshot to the existing local admin overview. It uses
AdminFilter plus an explicit independent admin identity check. Ordinary user cookies do not authorize it. As with
existing admin routes, database credentials cannot distinguish listeners: the trusted surface header, peer/Origin
policy and independent admin session remain the boundary. This batch does not change that architecture.

One SELECT reads only `count(*)` and `max(version)` from `schema_migrations`. A count/latest record is **not** a check
of applied file checksums, pending migrations, schema correctness or release readiness. The process allowlist contains
four booleans (scheduler/reminders/memory purge configured, current upload capability) and mail/registration mode.
Upload capability reuses MediaService::uploadsEnabled, including its current provider snapshot. Configuration flags
are not observations of scheduler/worker execution, provider delivery or another API process. Mail log mode does not
send email. No secret values, DSNs, paths, origins, allowlisted emails, bootstrap data, raw logs or user content appear.

Process values are sampled before the asynchronous database query. `read_at` is the response observation time, not a
claim that process/DB/provider states share one atomic snapshot. A successful response proves only that this query
succeeded. Database errors return a generic 500 with no partial snapshot or database diagnostic. Existing no-store
JSON responses apply. The query uses the existing pool/error handling; no new transport/acquisition deadline or
cross-process status aggregation is introduced. No business/audit writes occur; admin-session bookkeeping is unchanged.

No migrations, privileges, dependencies or deployment changes. All 54 numbered migrations remain frozen through 0053,
next 0054. Additive contract **0.60.0**. The endpoint changes no flags, purges nothing, triggers no backup/export,
opens no shell and grants no readiness/activation approval.

## Admin interface

The overview keeps the existing liveness probe separate and replaces the database placeholder with explicit observed
records and process settings. It labels limitations instead of inventing pending-migration or pool-health numbers.
The old disabled asset-correction placeholder becomes a link to choose a user for the existing audited financial path.
The backup button stays disabled, explaining the available signed/encrypted local tools and pending operational setup.

New snapshot state is memory-only, scoped to admin identity plus session epoch, with latest-request sequencing.
Logout, same-admin reauthentication, account change and unmount reset data and invalidate delayed answers. Refresh
clears an old snapshot immediately. Errors and malformed responses clear data; 401/403 show a fixed re-login message,
never raw server details. There is no automatic retry/poll. Validation is closed at root/database/process level so
unexpected fields are not retained. Unknown states are not converted to “off” or “healthy”.

## Verification

New focused snapshot spec: 9 passed. Final full admin suite: **243 / 17 files**, both typechecks and isolated Vite build
passed; existing admin dist 20 files unchanged. Output: `%TEMP%/usapp-admin-system-web-81604e250e`. Ten new cases total
(nine state/view/validation cases plus one real same-origin API binding case). No real browser or login acceptance.

Strict duplicate-key YAML parser and reference checks passed: 0.60.0, 129 paths, 252 distinct refs. This is not OpenAPI
semantic validation. Backend build and real HTTP **1465 assertions** passed in `usapp-e1-671835cce9`; full CTest **34/34 in 105.23 s** and final
postconditions passed. New HTTP checks exercise both
listeners (no admin session, ordinary user cookie, missing surface, valid admin), exact fields and migration values,
configured-off states, no-store and absence of session tokens/admin identity. Existing wallet/audit footprint assertions
remain. This fixture intentionally has no real media provider, scheduler or reminders enabled. Configured-on backend
branches, a forced DB-error HTTP response, real SMTP/push, browser/device and production acceptance are not claimed.

Known unrelated clock/transport/journal/provider and release gates remain open. Do not count this as online feature
control, remote backup execution, V1 completion or deployment.


## Final backend and status

Whole harness exited successfully. All 54 migration hashes unchanged; eraser and privilege deviations zero after apply,
repeat apply and CTest. Security/financial/push/memory SQL ledgers and both-role clock controls passed. Full registration
comparison against the last completed clock batch retained all **600** prior cases and added one static boundary case;
none removed. Real HTTP grew from 1417 to 1465 passing assertions (48 new checks). No financial/audit writes from the
new reads. Postconditions: early/reversed erasure zero and ledger chain breaks zero. Existing export loss/large/closure
8/6/23 and closed album/memory 7/8 passed. Export-content markers in API logs zero. Owned containers stopped/retained;
closure test database CLOSED by design. Existing export reconnect/half-open availability limits are not fixed by a pass.

No new backend target/timeout, migration, privilege, dependency or deployment. No user-web or worker changes, so their
suites were not repeated. The previous independent 54-migration backup restore remains the recorded DB-only evidence;
this batch did not rerun it or perform object/journal/real-key acceptance. Admin 243, types and final isolated build are
current; browser, actual admin login UI, device and provider acceptance remain unverified. Configured-on backend flags
and forced DB failure are not covered by the real HTTP fixture; this limitation is intentional and not a passing claim.
Evidence allowlist: `Claude outputs/admin-system-20261115`; no environment files, raw server logs, keys or dumps.
Implemented and locally verified; not committed or deployed. Remaining V1 operational workflows/release gates stay open.
