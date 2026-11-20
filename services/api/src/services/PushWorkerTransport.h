// =====================================================================
// services/api/src/services/PushWorkerTransport.h
//
// Phase 2 P2-5c (docs/phase2-web-push.md Section 16): the real Transport, a call to the private
// push worker. The worker encrypts and talks to the provider; this process keeps every decision and
// every durable fact. Off unless PUSH_ENABLED, PUSH_WORKER_URL and PUSH_WORKER_TOKEN are all set.
//
// Asynchronous on its own event loop: nothing here blocks an event-loop or ORM thread. One absolute
// deadline (15 s) per call and at most one result per call. A response counts only if its job equals
// the attempt that was sent and its outcome, status and Retry-After agree. What a result may claim is
// honest about what is known:
//   no request bytes handed to a connection        transport_unavailable (could not connect, or the
//                                                  deadline passed first; the request is withdrawn)
//   worker refused before starting (400/408/413)   transport_unavailable
//   worker said 401 / 503                          worker_auth_failed / busy
//   anything after the request may have been read  unknown (lost response, deadline, 5xx, malformed)
// Worker acceptance is never device display, and "accepted" means only that a provider accepted.
//
// Cancellation: Drogon v1.9.11 has no cancel call, and a queued request keeps its client alive, so
// releasing a client reference does NOT stop a request; it is sent when the connection succeeds. The
// deadline is therefore Drogon's per-request timeout, whose timer erases a request still in the queue.
// Once the request was written, the deadline shuts the socket down so that the connection ends. That
// cannot recall what the worker already read, which is why that case stays unknown.
// =====================================================================
#pragma once

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

#include <functional>
#include <optional>
#include <string>

#include "services/PushDispatch.h"

namespace usapp::push {

constexpr size_t kMinWorkerTokenLength = 32;
constexpr size_t kMaxWorkerResponseBytes = 1024;

struct WorkerConfig {
    std::string url;  // http://host[:port] on the private container network
    std::string token;
    double deadlineSeconds = 15.0;
};

// Nullopt unless explicitly enabled with a complete, well-formed configuration. Never logged.
std::optional<WorkerConfig> workerConfigFrom(bool enabled, const std::string &url, const std::string &token);
std::optional<WorkerConfig> workerConfigFromEnv();

// ---------------------------------------------------------------- pure
std::string workerRequestBody(const SendRequest &request);
// True only for a combination the worker can produce: a contradictory one proves nothing.
bool consistentWorkerResult(const SendResult &result);
// An HTTP response from the worker, for the attempt that was sent.
SendResult classifyWorkerResponse(const AttemptKey &sent, int httpStatus, const std::string &body);
// No HTTP response. bytesSent is the client's count of bytes handed to a connection: only zero proves
// that nothing was sent, whichever failure was reported.
SendResult classifyWorkerFailure(size_t bytesSent);

class WorkerTransport : public Transport {
  public:
    explicit WorkerTransport(WorkerConfig config);

    void send(const SendRequest &request, std::function<void(SendResult)> done) override;

  private:
    WorkerConfig config_;
    trantor::EventLoopThread loop_;
};

}  // namespace usapp::push
