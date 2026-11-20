// =====================================================================
// services/api/src/services/PushDispatch.h
//
// Phase 2 P2-5b durable push dispatch (docs/phase2-web-push.md Sections 5, 6 and 13; migration
// 0023). Committed notifications are the durable source; nothing here scans audit logs or writes
// wallets, tasks or notifications. The Transport interface is implemented by PushWorkerTransport
// (P2-5c, off by default) and by the tests' asynchronous fake.
//
// A tick (under the scheduler's transaction and lock, no other advisory lock except the 33016 of
// the claim guard): read control -> expire leases -> exhausted -> TTL -> cutover skip -> fan-out
// -> pre-claim inactive -> claim. Network work starts only in afterCommit, after the commit that
// released the lock, and uses neither the tick's Transaction nor its TxScope.
// After commit each claim gets a pre-send check of that EXACT attempt (its own claim, lease still
// valid, no observation, no expiry, no newer attempt) and of control, cutover, TTL and subscription,
// then Transport::send (asynchronous; no blocking wait on an event-loop or ORM thread). A race remains
// between that check and the send: a lease ending, a disable or a logout right after the check can
// still be sent, and a lease cannot physically stop a stalled sender. Delivery is at-least-once within bounds, can duplicate and can deliver nothing;
// provider acceptance never means the device displayed anything.
// P2-6 R5 (0028): each attempt also rechecks a reminder's source, after every other fence, and a stale one ends as
// source_stale with no request, no budget and no pause. A reminder's TTL and absolute expiry stop at its stale_at.
// The same race applies: a source changing right after the check can still be sent.
// 0041: a guard-assigned time can be ahead of a clock that moved backwards. Such a notice is deferred, never rejected: fan-out and claim
// skip it and find it again on a later tick. If the clock moves back after a claim, pre-send records notice_ahead_skipped, which is
// internal, spends no budget and decides nothing, because a claim left to expire would spend budget. Its retry is judged again.
// Every result is recorded in its own transaction. The observation is first-writer-wins; side
// effects are derived from the STORED observation and are idempotent, so a duplicate recording
// repeats nothing. An automatic pause needs the first recording AND a claim made under the current
// control revision, checked under the control lock 33015, so a stale failure never pauses a state
// the owner restored after the claim. The observation is stored either way.
// =====================================================================
#pragma once

#include <drogon/orm/DbClient.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace usapp::push {

constexpr int kProviderBudget = 5;  // also enforced by the 0023 guards
constexpr int kMaxWorkerTtlSeconds = 86400;  // the worker refuses anything above this (parseDeliverRequest)

struct Policy {
    // The local capability. Never moves the cutover and never writes control rows.
    bool localEnabled = false;
    std::string vapidKeyId;    // SHA-256 of the configured public key
    std::string markerSecret;  // derives the opaque marker of the payload
    std::string instanceId = "unconfigured";
    double leaseSeconds = 60.0;     // the 0023 CHECK bounds it at 60 s
    double ttlSeconds = 86400.0;    // the 0023 guards bound it at 24 h
    std::vector<double> backoffSeconds{60.0, 300.0, 1800.0, 7200.0};  // by provider attempts used
    double internalBackoffSeconds = 60.0;
    int capacity = 4;      // live leases globally; the 0023 guard bounds it at 4
    int perRecipient = 2;  // claims per recipient per tick
    int fanoutLimit = 100;       // deliveries inserted per tick
    int maintenanceLimit = 200;  // expiries and outcomes per statement per tick
};

struct AttemptKey {
    std::string notificationId;
    std::string generationId;
    int attemptNo = 0;
};

// What a transport needs. Endpoint and keys never reach a log, a response or a tick detail.
struct SendRequest {
    AttemptKey key;
    std::string eventType;
    std::string targetKind;
    std::string targetId;
    std::string endpoint;
    std::string p256dh;
    std::string auth;
    std::string marker;
    // What remains of the delivery's fixed lifetime, or of a reminder's stale_at if that is earlier, read from the
    // database clock at pre-send. A retry never gets a fresh TTL, and the absolute expiry stops a request delayed on its way.
    int ttlSeconds = 0;
    long long expiresAtMs = 0;
};

// Closed outcomes (0023 class rule): provider accepted | gone | rejected | transient | unknown |
// provider_auth_failed; internal worker_auth_failed | transport_unavailable | busy and the
// pre-send skips, including lease_lost_skipped and source_stale_skipped.
struct SendResult {
    std::string outcome;
    std::optional<int> httpStatus;
    std::optional<int> retryAfterSeconds;
};

class Transport {
  public:
    virtual ~Transport() = default;
    // Asynchronous. Must not block; done may run on any thread, at most once is expected and a
    // repeat is harmless (recording is idempotent).
    virtual void send(const SendRequest &request, std::function<void(SendResult)> done) = 0;
};

// ---------------------------------------------------------------- pure
// "provider", "internal", or empty for an unknown outcome.
std::string outcomeClass(const std::string &outcome);
bool validResult(const SendResult &result);
// The terminal state a stored observation decides by itself ("" = none; exhausted is budget-based).
std::string terminalForOutcome(const std::string &outcome);
// The pause reason a stored observation causes ("" = none). The two auth failures stay distinct.
std::string pauseReasonForOutcome(const std::string &outcome);

enum class TickPlan { Idle, FailClosed, Maintain, Dispatch };
// disabled: nothing, whatever the local flags. enabled/paused without a complete capability: fail
// closed. paused: maintenance and fan-out, no claims. enabled: everything.
TickPlan planTick(const std::string &controlState, bool capable);

// NotOwnClaim and AlreadyObserved write nothing: the attempt is another instance's, or its result
// already stands. LeaseLost: this claim lost its lease or was superseded before anything was sent.
// SourceStale: a reminder whose source no longer warrants it (usapp_reminder_occurrence_valid).
// NoticeAhead: the notice's time is ahead of the database clock (0041). Not terminal: judged again at the retry.
enum class PreSend { Send, NotOwnClaim, AlreadyObserved, LeaseLost, Terminal, Ttl, Cutover, Control, Inactive, SourceStale, NoticeAhead };
struct PreSendFacts {
    std::string controlState;
    bool beforeCutover = false;
    bool ttlReached = false;
    bool hasOutcome = false;
    bool generationActive = false;
    bool ownClaim = true;
    bool leaseValid = true;
    bool observed = false;
    bool expired = false;
    bool superseded = false;
    bool sourceStale = false;
    bool windowUnderASecond = false;  // under a second before the bounded expiry, source still valid
    bool noticeAhead = false;         // created_at is after the clock reading of this check
};
PreSend decidePreSend(const PreSendFacts &facts);
// The TTL a worker may be sent: at most the remaining time, the whole-second lifetime and the worker's limit. 0 means nothing to send.
int boundedTtlSeconds(long long remainingSeconds, double lifetimeSeconds);
// The internal observation written instead of a send ("" for Send and for the no-write decisions).
std::string skipOutcome(PreSend decision);

struct TickReport {
    bool ok = false;
    std::string detail;  // fixed labels and counts only
    std::vector<AttemptKey> claims;
};

struct RecordStatus {
    bool ok = false;
    bool inserted = false;       // false: an earlier observation for this attempt stands
    std::string storedOutcome;   // the first observation
    std::string finalState;      // the delivery's terminal state, if any
    int providerAttempts = 0;
};

class Dispatcher : public std::enable_shared_from_this<Dispatcher> {
  public:
    Dispatcher(Policy policy, std::shared_ptr<Transport> transport);

    bool capable() const;
    const Policy &policy() const { return policy_; }

    // Inside the scheduler transaction. `done` runs after the last statement; tx is not used later.
    void runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done);

    // After commit only. Pre-send check, then Transport::send or an explicit skip observation.
    void sendClaims(const std::vector<AttemptKey> &claims);

    // Own transaction; safe for late and duplicate results.
    void recordResult(const AttemptKey &key, const SendResult &result, std::function<void(RecordStatus)> done);

  private:
    void preSend(const AttemptKey &key);

    Policy policy_;
    std::shared_ptr<Transport> transport_;
};

}  // namespace usapp::push
