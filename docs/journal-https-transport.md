# Journal HTTPS client transport (2026-11-15)

Subsequent component (2026-11-16): an injected Python HTTPS listener and actual C++/Python TLS integration are now
locally verified. Production backend/credential factory, main wiring and independent recovery authority remain open.
The dated client evidence below is retained; see [service scope and current proof](journal-https-service.md).

## Implemented component

`HttpsTransport` provides a real asynchronous Exchange for the existing C++ WireJournal. It owns one libcurl multi I/O
thread, with programmatically supplied origin/trust/deadline/admission settings. There is **no environment factory,
Python HTTP server, cloud-provider factory or production main wiring** in this batch. Existing startup refusal stays
in place. A client transport is not the independent recovery authority or permission to reopen restored content.

The only URL shape is `https://host[:port]`, with a fixed POST `/v1/journal`; no userinfo, query, fragment, arbitrary
path, plaintext HTTP or IPv6-literal support. Host labels are ASCII lowercase. Default/system trust or an explicit CA
file is supported; certificate and hostname verification cannot be disabled. Minimum TLS version is 1.2. Private
deployment must still choose/provision its trust and writer authentication separately. Test-only loopback mapping
preserves URL/SNI/name checks and can only target 127.0.0.1. It is never loaded from environment or network input.

Requests carry only the existing raw body and MAC header. The transport does not know the writer key; WireJournal
still authenticates and correlates the response. No body, MAC, URL, trust path, credential or library error is logged.
Proxy environment is explicitly bypassed, redirects are not followed, HTTP/1.1 and a fresh non-reusable connection are
used, and there is no application retry. The fixed path and response request/journal/nonce/hash binding remain separate
layers. Every failure is an unknown Reply, never rollback, absence or a terminal acknowledgement.

## Resource and lifetime rules

- The default five-second steady-clock deadline covers admission/queuing, connection, TLS and complete response I/O;
  configured values are 1–20000 ms. Both libcurl timers and removal of expired handles apply. The original absolute
  deadline is not reset when an I/O handle starts. Only asynchronous-DNS libcurl builds are accepted. This does not
  establish a real-time scheduler guarantee or vendor/DNS latency SLA; tests use loopback mapping and do not exercise
  a stalled real resolver or resolver-thread resource reclamation.
- Active plus queued I/O admission is capped at 8 (configurable 1–8), connection concurrency at 2 by default (up to the
  admission cap). Overflow is rejected immediately, outside mutexes, without sending. A completed I/O releases its slot
  before its callback so callbacks may submit another call. Callback/application ownership is outside that I/O bound.
- Response bodies are checked while receiving, capped at 65536 retained bytes; total headers at 8192 bytes/64 lines.
  Excess declared length, duplicate/invalid authentication headers, duplicate length and non-identity content encoding
  abort reception. Unknown-length bodies are still capped. These are retained-payload/admission bounds, **not a bound
  on whole-process memory**, libcurl/TLS buffers, OS buffers, DNS threads or application callbacks.
- Admitted callbacks run on the owned I/O thread and must remain nonblocking. Local invalid/admission/stopped rejection
  runs immediately on the caller. Callback exceptions are contained; no callback runs under the admission mutex.
- Destruction stops admission, wakes I/O, removes active handles and answers remaining calls as unknown. It joins when
  called elsewhere; last-owner release within a callback detaches only the thread wrapper, while thread-owned shared
  state lives through cleanup. No queued request is retained for a later reconnect. A weak Exchange after owner loss
  cannot restart the transport. A completed valid receipt may win a simultaneous shutdown race.

The I/O thread alone blocks SIGPIPE; it does not install a global signal policy. This complements NOSIGNAL: libcurl
documents that NOSIGNAL neither supplies synchronous-resolver timeouts nor suppresses every SIGPIPE corner case.
[libcurl NOSIGNAL documentation](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html). Failed setup/limits or signal-mask
installation refuses the transport/calls rather than weakening settings. Real received bytes may already have taken
effect remotely before cancellation; nothing here proves an aborted publication. Retries must retain the original
stable operation identity under the existing protocol and be chosen by the caller.

## Verification plan and current state

New local TLS target `journal_https` keeps the unchanged 30-second per-target limit. Tests use ephemeral synthetic CA/
server certificates, fixed test MAC key and loopback sockets only. They cover valid signed round-trip, proxy bypass,
untrusted/expired/wrong-name/system-trust refusal before application bytes; redirect/no-retry/lost-response handling;
header and exact/over body caps including unknown length; duplicate MAC and encoding refusal; slow TLS and progressing
response deadlines; admission overflow, pending/queued destruction and weak-exchange rejection; callback last-owner
release and per-thread signal masking. No DB, cloud, provider, real secret or private certificate file is used.

Shared TLS stub gained two optional fixture fields (omit length, trickle length); their defaults preserve prior OSS
behavior, and the existing OSS target is included in full regression. The separate C++/Python synthetic transcript and
startup refusal probes also remain in the combined harness. Contract 0.60.0, 54 numbered migrations/grants unchanged;
next migration 0054. No frontend or dependency change.

Final-source verification passed as recorded below. The earlier snapshot predates SIGPIPE hardening; its passing
outcome is retained separately and is not the proof for that change. This is not a
Python-over-HTTPS service test, real DNS/provider acceptance, durable journal integration, restore proof or full J3c.
No deployment or commit; production provider/Source/head/restore binding and J4/J5 remain open.


## Final evidence

Initial run `usapp-e1-47799fc2f6` passed the initial client 70 assertions and full 36/36 (112.49 s). It predates the final
thread-signal/checked-limit hardening, so those results alone do not validate final code. No product assertion failed in
that run. Final run `usapp-e1-07bc76b458` used new owned disposable database/compiler resources and matching final
source hashes (verified inside the compiler before result collection):

- Client source SHA-256 `a360357db8d2e0aa3a7f2797930b53b1871cecc66f1bb4df941ae86b7c627281`.
- TLS test source SHA-256 `c4a7665e8b92f54eeb683348b37b7484d2c260af9b8559c45b0f7f9d3964f257`.
- New TLS target **74 assertions / 9 cases**, zero failures, both targeted and ordinary CTest. The test installs a benign
  process signal handler and verifies that worker masking leaves both caller mask and process handler unchanged.
- Full CTest **36/36 in 112.04 s**; journal_https 2.04 s. All **610** previous cases retained, **9** new TLS cases,
  619 ordinary cases total; none removed. Existing OSS stub tests passed with their original default behavior.
- Real API **1465 assertions**, zero failures. Existing synthetic C++/Python transcript **12 requests / 15 assertions**
  passed; this is still not Python over actual HTTPS or a real provider test.
- All 54 numbered migration hashes unchanged; eraser/privilege checks zero after apply/repeat/CTest; early/reversed
  erasure and ledger breaks zero. Existing security/financial/memory/push/clock controls passed.
- Startup missing-provider/purge/closed-restart process refusal and port probes passed. Existing export loss/large/closure
  8/6/23 and closed album/memory 7/8 passed; no exported-content markers in API logs. This does not fix the separate
  half-open/reconnect availability issue or other legacy clock guards.

Owned containers stopped/retained; closure-mode DB is CLOSED. Public CA files are synthetic test fixtures; no real
private keys or credentials used. Evidence allowlist: `Claude outputs/journal-https-20261115`, excluding environment,
raw server logs, dumps or keys. No frontend/Python unit/independent backup rerun (their implementation was unchanged),
real provider/DNS fault, browser/device, production, restore-release, deployment or commit. Current contract 0.60.0,
54 frozen migrations through 0053, next 0054. Completed client transport only: protected Python service/backend factory,
independent recovery authority/head/restore binding and full J3c/J4/J5 remain required.
