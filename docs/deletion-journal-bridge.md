# Injected C++ / Python journal wire bridge (2026-11-15)

Subsequent injected service (2026-11-16): bounded Python TLS listener and actual C++/Python HTTPS integration are locally
verified. Production backend/credential factory, main wiring and independent recovery release remain absent. The
historical component scope/results below are unchanged; see [current service evidence](journal-https-service.md).

Subsequent client component (2026-11-15): an optional bounded HTTPS Exchange is now locally verified; no Python service/backend factory or production wiring. The injected wire component below remains separate. See [client evidence and limits](journal-https-transport.md).

## Implemented component and explicit scope

The existing J2 Journal interface now has an optional **injected** `WireJournal` adapter. It encodes ensure-intent,
settle and lookup calls; authenticates and validates their replies; maps them to the existing coordinator states. A
Python `Dispatcher` binds the same protocol to an injected J4 reference `Publisher`. There is no HTTP server, socket
transport, SDK/credential factory, environment reader or production main wiring in this component. Exchange is an
asynchronous interface, not an implementation of bounded remote I/O. The existing startup refusal gate still refuses
real media/purge requests because the actual provider/independent recovery authority is missing.

This is a wire/adapter compatibility batch, **not completed real journal integration, J3c or a recovery Source**. No
independent checkpoint, restore incarnation, fresh-head attestation or serving-release message exists in this protocol.
There is no `serving_allowed` response field; unknown/extra fields are rejected. Receiving a valid writer receipt
does not authorize opening restored content. The known lost-CLOSED-marker case remains unsolved by this component.

## Wire v1

Requests have exactly `version`, `journal_id`, `nonce`, `method`, `operation_id`, `operation`, `phase`.
Version is integer 1 (not true or 1.0). The journal UUID is pinned at each end. Nonce is 32 lowercase hex characters;
C++ defaults to 16 cryptographically random bytes per request. Deterministic nonce injection is used only by tests.
No transport configuration/secret or nonce generator is available from the production environment in this batch.

- ensure_intent: complete operation and phase intent.
- settle: the same complete operation and phase committed or aborted. C++ sends aborted only for the existing
  `EffectState::ProvenNotApplied`; an unknown effect is rejected locally without an exchange. Transport errors never
  become proof of rollback.
- lookup: canonical operation ID, null operation and null phase. Found carries the full frozen operation binding;
  it does not imply that the operation is pending or may be executed again.

Operations have the existing exact action/fingerprint/subject schema, with no raw keys, content or object URLs.
Targets/intents are unique, UUIDs and fingerprints retain existing validation, cleanup is media only. Requests are
normalized in C++; Python rejects unsorted or altered wire lists. Found bindings must also be normalized and refer to
the exact requested ID. Frames are capped at 65536 bytes and 256 subjects (prototype safeguards inherited from the
reference model, not approved production quotas). These validation caps are not a process-memory or queue bound.

Authentication uses HMAC（Hash-based Message Authentication Code，基于哈希的消息认证码）with SHA-256 and a pinned
64-character lowercase hexadecimal key string. The key is used as its ASCII bytes at both ends. Input is the exact raw
body bytes prefixed with `usapp-journal-bridge-v1:request\n` or `...:response\n`; domain separation prevents request/
response reflection. Comparisons use constant-time helpers. Authentication precedes Python parsing/backend work.
No key, signature or body is logged by the library. Test artifacts use only the literal synthetic key and fixed UUIDs.
The MAC provides integrity/authentication, **not confidentiality**. A future real transport must provide confidentiality,
private deployment and operational key provisioning/rotation; it must not reuse cloud credentials or recovery-reader
authority as this writer key.

Responses have exactly `version`, `journal_id`, `nonce`, `request_sha256`, `state`, `operation`. The hash binds the raw
original request; fresh nonce/journal binding rejects wrong-call/wrong-journal and replayed response bytes. A MAC alone
does not make an otherwise contradictory frame valid. Non-200, missing/bad authentication, malformed/duplicate/trailing
JSON, oversized/unknown fields, wrong nonce/hash, wrong operation or inappropriate states map conservatively to unknown
(or false for terminal confirmation). Valid explicit conflict remains conflict. Only the matching committed/aborted
terminal acknowledges settlement. Each adapter call answers at most once, including duplicate/throwing exchanges.

The transport contract requires bounded queues, streamed response bytes, absolute deadlines/cancellation and a serialized
callback executor. **None is implemented by the injected Exchange itself**. An exchange that never calls back can leave
the caller waiting; the bridge cannot manufacture a ready/abort/absence receipt. No network or transport liveness result
is claimed here. These requirements must be implemented and fault-tested before constructing a production adapter.

Replaying a signed request is not a new grant: original stable operation identity and immutable payload/terminal rules
still apply. There is no replay-cache or wall-clock lease in this protocol. Fresh nonces bind replies for new calls;
shared-key compromise, a dishonest authenticated service or a stale backend are not solved by authentication.

## Publication lookup

`Publisher.lookup` uses the existing complete `_load` path: validate referenced batch bytes, logical chain/checkpoint,
and derived operation/cycle indexes against the metadata snapshot. A failed/corrupt/unavailable snapshot gives unknown,
never missing. Found returns a defensive copy. This relies on Metadata.read's authoritative-current-state contract:
a self-consistent stale/truncated checkpoint still needs the independent freshness proof required by J4/J3c. It is not
a shortcut to recovery validation. Publish ordering, object/metadata transaction boundaries and terminal rules unchanged.

## Evidence so far

Python foundation regression: **74 tests passed**, including eight new dispatcher tests, unchanged J1/batches/
publication suites. The first focused run used a nonexistent fixture fault name (`after_commit`); corrected to the
existing `after` fault so the same strict lost-receipt assertion exercises an actual unknown commit. No production
behavior or assertion was weakened. Authentication-before-backend, strict/duplicate/scope/phase/size rejection,
corrupt indexes, lost receipt recovery and no duplicate publication are covered with in-memory adapters only.

The real compiled C++ protocol runner emitted 12 authenticated requests. Python processed them through injected fake
objects/metadata; C++ verified the actual returned bytes/MACs and adapter states: **15 assertions, zero failures**.
Two operations, four expected batches; repeated intent/terminal calls and conflicting terminal/payload created no
extra batch. Structural fake-only execution constructs no network transport; its printed network_calls=0 label is not
vendor/network instrumentation. The transcript is synthetic, not a durable storage or cloud acknowledgement.

Complete backend regression passed in `usapp-e1-169cf84da7`: **35/35 CTest (108.95 s)** and **1465 real HTTP assertions**. New C++ unit cases cover request MACs, enum mapping,
forgery/reflection/correlation, malformed/oversized/duplicate replies, lookup binding, invalid local input, terminal
mismatch, exception/duplicate callbacks and rejection of an unknown effect before transport. Existing startup refusal
and real fault boundaries remain unchanged. No public contract, numbered migration, grant, dependency or frontend
change (0.60.0, 54 frozen through 0053; next 0054). No deployment, commit, SDK/cloud/resource operation, real keys,
browser/device or production recovery/serving acceptance.

Next required component: a bounded authenticated confidential transport plus protected service factory using the real
publication adapters; separately implement independent recovery Source/head/restore binding and release proofs. Keep
the production startup refusal in place until those gates have actual evidence.


## Final combined proof and remaining integration

The full harness exited successfully: all 54 migration hashes unchanged, eraser/privilege deviations zero after apply,
repeat apply and CTest, early/reversed erasure and ledger chain breaks zero. Existing SQL ledgers and both-role clock
controls passed. All **605** prior ordinary cases retained, **five** new bridge unit cases added (610 total); no old case
removed. Cross-language driver passed 12 real byte exchanges and 15 C++ assertions against the injected Python model.
Fake stores remain process memory; no durability, real network or production-provider result is inferred.

Existing startup real-media/purge/missing-provider and closed-restart refusal processes/port probes passed again. The
prior independently tested query-timeout/function-missing startup faults were not repeated this batch; their separate
record stands. Export loss/large/closure 8/6/23 and closed album/memory 7/8 passed. API export-content markers zero.
Owned containers stopped/retained, closure test database CLOSED by design. Export half-open/reconnect limits and other
legacy clock issues remain open. No frontend/worker-provider/independent backup/browser suites repeated because those
implementations were unchanged. Evidence allowlist: `Claude outputs/journal-bridge-20261115`; no real secrets, env
files, raw server logs or dumps. The synthetic transcript includes its literal test key by design; not production data.

This completes the injected wire/adapter component only. The next production integration still needs the bounded,
confidential transport/service factory, real-adapter fault proof and independent recovery authority. No new main
provider configuration or serving exception was introduced. V1, full J3c/J4/J5 and production acceptance remain open.
