// =====================================================================
// services/api/src/services/PushWorkerTransport.cc
// =====================================================================
#include "services/PushWorkerTransport.h"

#include <json/json.h>
#include <sys/socket.h>

#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "core/Env.h"

namespace usapp::push {

namespace {

bool validWorkerUrl(const std::string &url) {
    static const std::string kScheme = "http://";
    if (url.compare(0, kScheme.size(), kScheme) != 0 || url.size() == kScheme.size() || url.size() > 256u) return false;
    for (size_t i = kScheme.size(); i < url.size(); ++i) {
        const char c = url[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

// Exactly these keys, or the response is not one this worker sends.
bool hasExactKeys(const Json::Value &value, const std::set<std::string> &keys) {
    if (!value.isObject()) return false;
    const auto names = value.getMemberNames();
    return std::set<std::string>(names.begin(), names.end()) == keys;
}

const SendResult kUnknown{"unknown", std::nullopt, std::nullopt};
const SendResult kUnavailable{"transport_unavailable", std::nullopt, std::nullopt};

// jsoncpp's isInt also accepts 201.0: only a number written as an integer counts here.
bool isWholeNumber(const Json::Value &value) {
    return (value.type() == Json::intValue || value.type() == Json::uintValue) && value.isInt();
}

bool inRange(int value, int low, int high) { return value >= low && value <= high; }

// The result of one call, on the loop thread only.
struct Call {
    AttemptKey key;
    std::function<void(SendResult)> done;
    drogon::HttpClientPtr client;
    int socketFd = -1;  // the latest socket the client created
    bool finished = false;
};

void finishCall(const std::shared_ptr<Call> &call, trantor::EventLoop *loop, SendResult result) {
    if (call->finished) return;
    call->finished = true;
    // The client may be inside its own callback here: it is released after this returns.
    loop->queueInLoop([client = std::move(call->client)]() {});
    auto done = std::move(call->done);
    if (done) done(std::move(result));
}

}  // namespace

std::optional<WorkerConfig> workerConfigFrom(bool enabled, const std::string &url, const std::string &token) {
    if (!enabled || !validWorkerUrl(url) || token.size() < kMinWorkerTokenLength) return std::nullopt;
    WorkerConfig config;
    config.url = url;
    config.token = token;
    return config;
}

std::optional<WorkerConfig> workerConfigFromEnv() {
    return workerConfigFrom(env::getBool("PUSH_ENABLED", false), env::get("PUSH_WORKER_URL"), env::get("PUSH_WORKER_TOKEN"));
}

std::string workerRequestBody(const SendRequest &request) {
    Json::Value body(Json::objectValue);
    body["job"]["notification_id"] = request.key.notificationId;
    body["job"]["generation_id"] = request.key.generationId;
    body["job"]["attempt_no"] = request.key.attemptNo;
    body["subscription"]["endpoint"] = request.endpoint;
    body["subscription"]["keys"]["p256dh"] = request.p256dh;
    body["subscription"]["keys"]["auth"] = request.auth;
    body["payload"]["v"] = 1;
    body["payload"]["nid"] = request.key.notificationId;
    body["payload"]["event_type"] = request.eventType;
    body["payload"]["target_kind"] = request.targetKind;
    body["payload"]["target_id"] = request.targetId;
    body["payload"]["marker"] = request.marker;
    body["ttl_seconds"] = request.ttlSeconds;
    body["expires_at_ms"] = static_cast<Json::Int64>(request.expiresAtMs);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, body);
}

SendResult classifyWorkerResponse(const AttemptKey &sent, int httpStatus, const std::string &body) {
    // Refused by the worker before it began anything.
    if (httpStatus == 401) return {"worker_auth_failed", std::nullopt, std::nullopt};
    if (httpStatus == 503) return {"busy", std::nullopt, std::nullopt};
    if (httpStatus == 400 || httpStatus == 408 || httpStatus == 413) return kUnavailable;
    if (httpStatus != 200 || body.size() > kMaxWorkerResponseBytes) return kUnknown;

    Json::Value value;
    Json::CharReaderBuilder builder;
    builder["strictRoot"] = true;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["stackLimit"] = 8;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &value, &errors)) return kUnknown;
    if (!hasExactKeys(value, {"job", "outcome", "http_status", "retry_after_s"})) return kUnknown;

    // Only the exact attempt that was sent: another job's result is not this attempt's.
    const Json::Value &job = value["job"];
    if (!hasExactKeys(job, {"notification_id", "generation_id", "attempt_no"}) || !job["notification_id"].isString() ||
        !job["generation_id"].isString() || !isWholeNumber(job["attempt_no"]) ||
        job["notification_id"].asString() != sent.notificationId || job["generation_id"].asString() != sent.generationId ||
        job["attempt_no"].asInt() != sent.attemptNo) {
        return kUnknown;
    }

    if (!value["outcome"].isString()) return kUnknown;

    SendResult result;
    result.outcome = value["outcome"].asString();
    const Json::Value &status = value["http_status"];
    if (isWholeNumber(status)) result.httpStatus = status.asInt();
    else if (!status.isNull()) return kUnknown;
    const Json::Value &retry = value["retry_after_s"];
    if (isWholeNumber(retry)) result.retryAfterSeconds = retry.asInt();
    else if (!retry.isNull()) return kUnknown;
    // A contradiction could revoke a generation or pause dispatch: it proves nothing, so unknown.
    return consistentWorkerResult(result) ? result : kUnknown;
}

bool consistentWorkerResult(const SendResult &result) {
    const auto &outcome = result.outcome;
    const std::optional<int> &status = result.httpStatus;
    const std::optional<int> &retry = result.retryAfterSeconds;
    if (retry && !inRange(*retry, 0, 86400)) return false;
    // Only a provider's "try later" carries a delay.
    if (outcome == "transient") {
        return status && (*status == 408 || *status == 425 || *status == 429 || inRange(*status, 500, 599));
    }
    if (retry) return false;
    if (outcome == "accepted") return status && inRange(*status, 200, 299);
    if (outcome == "gone") return status && (*status == 404 || *status == 410);
    if (outcome == "provider_auth_failed") return status && (*status == 401 || *status == 403);
    if (outcome == "rejected") {
        if (!status || !inRange(*status, 300, 499)) return false;
        for (int other : {401, 403, 404, 408, 410, 425, 429}) {
            if (*status == other) return false;
        }
        return true;
    }
    // No provider status exists for these, and they are the only other outcomes a worker may report.
    return !status && (outcome == "unknown" || outcome == "transport_unavailable" || outcome == "ttl_skipped");
}

SendResult classifyWorkerFailure(size_t bytesSent) {
    return bytesSent == 0u ? kUnavailable : kUnknown;
}

WorkerTransport::WorkerTransport(WorkerConfig config) : config_(std::move(config)), loop_("push-transport") {
    loop_.run();
}

void WorkerTransport::send(const SendRequest &request, std::function<void(SendResult)> done) {
    auto call = std::make_shared<Call>();
    call->key = request.key;
    call->done = std::move(done);
    auto body = workerRequestBody(request);
    trantor::EventLoop *loop = loop_.getLoop();
    loop->queueInLoop([this, call, loop, body = std::move(body)]() mutable {
        call->client = drogon::HttpClient::newHttpClient(config_.url, loop);
        std::weak_ptr<Call> weakCall = call;
        call->client->setSockOptCallback([weakCall](int fd) {
            if (auto live = weakCall.lock()) live->socketFd = fd;
        });
        auto http = drogon::HttpRequest::newHttpRequest();
        http->setMethod(drogon::Post);
        http->setPath("/v1/deliver");
        http->addHeader("Authorization", "Bearer " + config_.token);
        http->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        http->setBody(std::move(body));
        // The timeout is the one deadline: its timer also erases a request that is still queued.
        call->client->sendRequest(http, [call, loop](drogon::ReqResult result, const drogon::HttpResponsePtr &response) {
            if (call->finished) return;
            if (result != drogon::ReqResult::Ok || !response) {
                const size_t bytesSent = call->client->bytesSent();
                // Written already, so the client holds the connection until it ends. This runs on the
                // loop thread and any earlier close would have ended the call first, so the socket is
                // still this connection's.
                if (result == drogon::ReqResult::Timeout && bytesSent > 0u && call->socketFd >= 0) {
                    ::shutdown(call->socketFd, SHUT_RDWR);
                }
                finishCall(call, loop, classifyWorkerFailure(bytesSent));
                return;
            }
            finishCall(call, loop, classifyWorkerResponse(call->key, static_cast<int>(response->statusCode()),
                                                          std::string(response->body())));
        }, config_.deadlineSeconds);
    });
}

}  // namespace usapp::push
