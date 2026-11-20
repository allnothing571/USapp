# Protected Python journal HTTPS component

Subsequent restricted backend assembly (2026-11-16) now composes the SDK/object/validating-metadata path and owns client
cleanup. Scoped 125-test/real HTTPS proof is separate from the historical full run below; no new CTest/HTTP full-run
claim or production factory/authority. See [composition evidence and boundaries](journal-backend-composition.md).

This is a programmatically constructed TLS listener around an **injected** `Dispatcher` and `Publisher`.
It is not a production daemon, credential loader, cloud backend factory, recovery Source or authorization to serve
restored data. The API's existing missing-provider/startup refusal remains unchanged. Contract **0.60.0**, all **54**
numbered migrations through 0053, privileges and original regression timeouts remain unchanged; next migration 0054.

## Request and resource boundaries

`tools/journal_https_server.py` uses the standard library. Configuration accepts a literal IPv4 loopback or RFC1918
bind address, a certificate/key pair, 0.05–10 s total connection budget and 1–8 admitted connections (defaults: loopback,
2 s, four). Wildcard/public/DNS/IPv6 bindings are refused. These are local component restrictions, not a firewall or
deployment policy. TLS 1.2 is the minimum; the certificate/key load cannot prompt for a password. There is no plaintext
listener, environment-driven configuration, proxy, redirect, shell entry point or automatic publication retry.

One acceptor and bounded connection threads enforce admission **before** TLS handshake. TLS, headers, body, backend
wait and response share an absolute monotonic deadline; new progress does not reset it. This is cooperative I/O time
bounding, not a hard real-time CPU/scheduling guarantee. Admission overflow closes the raw connection, without creating
another TLS worker. The kernel backlog is 16; operating-system/TLS buffers and whole-process memory are not measured or
bounded by the retained-frame limits.

Only `POST /v1/journal HTTP/1.1` is accepted. Headers are bounded to 8192 bytes / 64 lines; body to 65536 bytes. Duplicate
or folded headers, missing Host/length, transfer coding, Expect/Upgrade, content coding and an alternate content type
are refused. Already-buffered trailing bytes are refused; each connection carries one request and is always closed,
so this does not promise rejection of every possible late-arriving pipelined byte. No keep-alive, decompression or
chunked decoding is provided. Non-200 replies are empty, unsigned and no-store; raw input, filenames and exception
messages are not returned or logged by this component.

The dispatcher authenticates bounded raw bytes before backend admission; its normal `handle` authenticates again.
The existing protocol still binds reply bytes to journal identity, request digest and nonce; complete normalized
operation identity and terminal-state rules are unchanged. TLS provides confidentiality, while possession of the
configured MAC key authorizes this single configured journal. No independent recovery authority/freshness proof is
created by transport authentication. A captured authenticated request can be replayed; publisher identity/idempotency
rules, not a nonce-replay cache, prevent an additional semantic effect. Local concurrency limits are not distributed
rate limits or a denial-of-service guarantee.

## Publication and shutdown

One fixed daemon publication thread serializes the injected Publisher. There is at most **one admitted job total**,
including a not-yet-started job; no waiting-job queue or replacement worker. Invalid MACs never enter it. A busy service
returns 503 for a second authenticated request. If a client times out or disconnects after work starts, the job retains
the slot until the invoked dispatcher actually returns. Network timeout is **unknown**, never rollback or aborted.
No aborted record, remote cancellation or automatic re-execution is invented.

`close(0..2 s)` first forbids admission, cancels a not-started job, closes tracked sockets and waits within one deadline.
It reports network and backend drain separately. A hung active call reports backend undrained and retains the same
worker/slot; there is no forced thread termination, new worker or release-on-timeout. Repeated close can observe later
completion. Daemon threads do not make process exit a proof of backend cancellation or durability: an eventual real
service host must keep the process alive/drain according to its deployment policy and handle unknown remote effects.
Local completion/drain itself says nothing about a remote provider's atomicity, durability or latest head.

Python TLS behavior reference: [Python 3.12 ssl documentation](https://docs.python.org/3.12/library/ssl.html).
The component constructs its SSLContext directly; it does not enable environment-driven TLS key logging.

## Verification arrangement

- `tests/test_journal_https_server.py`: real loopback TLS using ephemeral synthetic certificates; injected in-memory
  object/metadata model. Covers protocol/auth refusal, absolute slow-input deadline, TLS admission, signed publication,
  late completion/replay without another write and shutdown while publication is still active.
- `JournalServiceInteropTest.cc`: harness-only helper generates synthetic PKI in a fresh private `/tmp` directory and
  runs the actual C++ HTTPS Exchange + WireJournal against Python. No public DNS lookup or host listener.
- `tools/journal_service_fixture.py`: test-only host of the existing in-memory publication model, with explicit
  synthetic marker, finite owned-harness lifetime and final non-serving report. No production/backend/SDK factory.
- `tools/journal-service-verify.ps1`: verifies exact container ownership, host tests plus pinned Python 3.12 runtime
  tests, real C++/Python HTTPS and separate network/backend drain. Python shares only the owned compiler container's
  network namespace on the existing internal network; no published ports. Private synthetic keys are deleted by exact
  filename in both containers and host work folder, including the failure path. Keys are not evidence artifacts.
- The complete backend harness calls this stage before ordinary suites and retains original startup refusal, security,
  financial/erasure controls and export recovery/closure modes. No frontend was changed.

Runtime used only in disposable tests:
`python@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df`
(official Python 3.12 slim Bookworm manifest, pulled once; runs use `--pull never`). No host SDK/package installation.

## Evidence and remaining work

Initial full run `usapp-e1-e7a5953d8a` stopped during compilation of the new harness helper because it omitted
`<iostream>`. It reached no backend suites/CTest. The fix only adds the missing test include; no assertion, timeout or
permission was relaxed. A scoped continuation on that owned compiler passed host Python **91 tests** (17 new service
cases + 74 existing foundation tests), pinned Python 3.12 **91 tests**, and actual C++/Python HTTPS **14 assertions**.
The fixture ended with **2 operations / 4 batches**, both drains true and `serving_allowed=false`; synthetic private
keys were removed and containers stopped. This targeted continuation is not a passing full backend run.
It also predates the final pre-admission deadline check and its new expired-authentication regression; it is retained
as intermediate evidence only.

Final run **`usapp-e1-35fb10e7d8`** passed the entire owned disposable harness (exit 0):

- Host Python **3.8.7: 92 tests**, container Python **3.12.16: 92 tests** (18 new service cases + 74 foundation cases).
  The extra final regression delays authentication past the budget and proves publication never starts.
- Actual C++ -> Python HTTPS **14 assertions**, **2 operations / 4 batches**, both drains true,
  `serving_allowed=false`. Existing file transcript **12 requests / 15 assertions** also passed independently.
- Existing HTTPS client **74 assertions / 9 cases** passed targeted and in ordinary CTest.
- Full **CTest 36/36 in 105.64 s**, real isolated API **1465 assertions**, zero failures. All **619** ordinary case names
  and their order exactly match the prior final run `07bc76b458`, with no omissions, additions or duplicates. New Python
  tests and the integration helper are explicit harness stages, not silently omitted from claimed verification.
- **54** migration checksums/files unchanged; eraser/privilege checks zero after apply/repeat/CTest; ledger-chain and
  early/reversed erasure checks zero. Existing two-role security/financial/memory/push/clock controls passed.
- Actual missing-provider media/purge and recovery-closed restart refusal/port probes passed. Export loss/large/closure
  **8/6/23**, closed album/memory **7/8**, closed startup **9** passed; API exported-content markers zero. Ordinary export
  loss recovery (~1009 ms here) does not prove the known half-open/reconnect availability limit is fixed.
- Compiler/test and runtime Python source hashes were checked against the host before use. See the evidence source
  manifest and source-binding records. Exact synthetic-key removal succeeded and was checked in both containers; host
  key absence checked separately. All three owned containers are stopped/retained; closure-mode database is CLOSED.

Evidence allowlist: `Claude outputs/journal-service-20261116` (summary, compact CTest output, Python test summaries,
non-serving report, source hashes/binding and migration hashes). No private keys, credentials, environment files, raw
API/PostgreSQL logs or dumps archived. No frontend/worker/independent backup rerun because their code was unchanged.

This completes the injected HTTPS service component only. Real cloud backends/credentials, independent recovery
Source/head/incarnation/restore binding, production main wiring, J3c/J4/J5 completion, physical network faults,
deployment and browser/device acceptance remain unverified. No deployment, commit or production/cloud-resource access.
