#pragma once

// P2-5c: the worker transport without a database: pure classification, then the real WorkerTransport
// against a loopback socket that scripts what the worker does. No provider and no worker process.
#include <arpa/inet.h>
#include <drogon/HttpClient.h>
#include <fcntl.h>
#include <json/json.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "services/PushWorkerTransport.h"

namespace usapptest::pushworkerunit {

namespace d = usapp::push;

inline const char *kToken = "unit-test-only-worker-token-000000000";

inline d::AttemptKey attempt() { return {"11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222", 3}; }

inline d::SendRequest sendRequest() {
    d::SendRequest r;
    r.key = attempt();
    r.eventType = "message.created";
    r.targetKind = "message";
    r.targetId = "33333333-3333-4333-8333-333333333333";
    r.endpoint = "https://fcm.googleapis.com/fcm/send/unit";
    r.p256dh = "p";
    r.auth = "a";
    r.marker = "m";
    r.ttlSeconds = 90;
    r.expiresAtMs = 1800000000123LL;
    return r;
}

inline std::string jobJson(const d::AttemptKey &k) {
    return "{\"notification_id\":\"" + k.notificationId + "\",\"generation_id\":\"" + k.generationId + "\",\"attempt_no\":" +
           std::to_string(k.attemptNo) + "}";
}
inline std::string resultJson(const d::AttemptKey &k, const std::string &outcome, const std::string &status = "null",
                              const std::string &retry = "null") {
    return "{\"job\":" + jobJson(k) + ",\"outcome\":\"" + outcome + "\",\"http_status\":" + status + ",\"retry_after_s\":" + retry + "}";
}

// One connection at a time on loopback. The script owns the accepted socket, and is given the whole
// request text it read.
class StubWorker {
  public:
    using Script = std::function<void(int, const std::string &)>;
    explicit StubWorker(Script script) : script_(std::move(script)) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listen_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        ::listen(listen_, 4);
        socklen_t length = sizeof(address);
        ::getsockname(listen_, reinterpret_cast<sockaddr *>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this]() {
            const int fd = ::accept(listen_, nullptr, nullptr);
            if (fd < 0) return;
            std::string text;
            char buffer[4096];
            size_t want = std::string::npos;
            while (want == std::string::npos || text.size() < want) {
                const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                text.append(buffer, static_cast<size_t>(n));
                const size_t head = text.find("\r\n\r\n");
                if (want == std::string::npos && head != std::string::npos) {
                    std::string lower = text;
                    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    const size_t at = lower.find("content-length: ");
                    want = head + 4 + (at == std::string::npos ? 0u : static_cast<size_t>(std::stoul(text.substr(at + 16))));
                }
            }
            script_(fd, text);
            ::close(fd);
        });
    }
    ~StubWorker() {
        ::shutdown(listen_, SHUT_RDWR);
        ::close(listen_);
        if (thread_.joinable()) thread_.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    Script script_;
    int listen_ = -1;
    int port_ = 0;
    std::thread thread_;
};

// A listener whose accept queue is full, so a new connection's SYN is dropped and it stays in progress
// until release() drains the queue and the kernel retransmits. This holds connection establishment past a
// deadline without any sleep inside the code under test. Linux loopback only, which is where these tests run.
class SaturatedListener {
  public:
    SaturatedListener() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        ::listen(listen_, 0);
        socklen_t length = sizeof(address);
        ::getsockname(listen_, reinterpret_cast<sockaddr *>(&address), &length);
        port_ = ntohs(address.sin_port);
        for (int i = 0; i < 16 && !saturated_; ++i) {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            (void)!::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
            pollfd waiting{fd, POLLOUT, 0};
            if (::poll(&waiting, 1, 200) == 0) {
                saturated_ = true;  // this one could not complete: the queue is full
                ::close(fd);
            } else {
                fillers_.push_back(fd);
            }
        }
        ::fcntl(listen_, F_SETFL, O_NONBLOCK);
    }
    ~SaturatedListener() {
        for (int fd : fillers_) ::close(fd);
        ::close(listen_);
    }
    bool saturated() const { return saturated_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    // Drains the queue, then watches for the window: bytes that reach this listener, excluding the empty
    // fillers. Returns early once any byte has arrived.
    size_t releaseAndWatch(double seconds) {
        std::vector<int> open;
        size_t bytes = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (bytes == 0 && std::chrono::steady_clock::now() < end) {
            const int fd = ::accept(listen_, nullptr, nullptr);
            if (fd >= 0) {
                ::fcntl(fd, F_SETFL, O_NONBLOCK);
                open.push_back(fd);
            }
            char buffer[4096];
            for (int conn : open) {
                const ssize_t n = ::recv(conn, buffer, sizeof(buffer), 0);
                if (n > 0) bytes += static_cast<size_t>(n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int fd : open) ::close(fd);
        return bytes;
    }

  private:
    int listen_ = -1;
    int port_ = 0;
    bool saturated_ = false;
    std::vector<int> fillers_;
};

inline std::string httpResponse(int status, const std::string &body) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
inline void writeAll(int fd, const std::string &text) { (void)!::send(fd, text.data(), text.size(), MSG_NOSIGNAL); }

struct Sent {
    std::optional<d::SendResult> result;
    int calls = 0;
    double seconds = 0;
};

// The real transport, once. The waiting happens on this test thread only.
inline Sent sendVia(d::WorkerTransport &transport, double settleSeconds = 0.0) {
    auto mutex = std::make_shared<std::mutex>();
    auto ready = std::make_shared<std::condition_variable>();
    auto out = std::make_shared<Sent>();
    const auto started = std::chrono::steady_clock::now();
    transport.send(sendRequest(), [=](d::SendResult result) {
        std::lock_guard<std::mutex> guard(*mutex);
        out->calls += 1;
        out->result = std::move(result);
        ready->notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(*mutex);
        ready->wait_for(lock, std::chrono::seconds(8), [&]() { return out->calls > 0; });
    }
    Sent copy;
    {
        std::lock_guard<std::mutex> guard(*mutex);
        copy.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
    if (settleSeconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(settleSeconds));
    std::lock_guard<std::mutex> guard(*mutex);
    copy.result = out->result;
    copy.calls = out->calls;
    return copy;
}

inline d::WorkerConfig configFor(const std::string &url, double deadline = 1.0) {
    d::WorkerConfig config;
    config.url = url;
    config.token = kToken;
    config.deadlineSeconds = deadline;
    return config;
}

inline Sent scripted(StubWorker::Script script, double settleSeconds = 0.0) {
    StubWorker stub(std::move(script));
    d::WorkerTransport transport(configFor(stub.url()));
    return sendVia(transport, settleSeconds);
}

inline void runPushWorkerUnitTests() {
    TEST_CASE("P2-5c: the worker transport is configured only when explicitly enabled and complete") {
        const std::string token(32, 't');
        CHECK(d::workerConfigFrom(true, "http://push-worker:8090", token).has_value());
        CHECK_FALSE(d::workerConfigFrom(false, "http://push-worker:8090", token).has_value());
        CHECK_FALSE(d::workerConfigFrom(true, "", token).has_value());
        CHECK_FALSE(d::workerConfigFrom(true, "http://push-worker:8090", "").has_value());
        CHECK_FALSE(d::workerConfigFrom(true, "http://push-worker:8090", std::string(31, 't')).has_value());
        for (const char *bad : {"https://push-worker:8090", "push-worker:8090", "http://", "http://a/b", "http://u@a", "http://a?b=c"}) {
            CHECK_FALSE(d::workerConfigFrom(true, bad, token).has_value());
        }
        CHECK(d::workerConfigFrom(true, "http://push-worker:8090", token)->deadlineSeconds == 15.0);
    }

    TEST_CASE("P2-5c: the worker request holds the exact fields, the remaining TTL and the absolute expiry") {
        Json::Value body;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::string text = d::workerRequestBody(sendRequest());
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        CHECK(reader->parse(text.data(), text.data() + text.size(), &body, &errors));
        CHECK_EQ(body.size(), 5u);
        CHECK_EQ(body["ttl_seconds"].asInt(), 90);
        CHECK_EQ(body["expires_at_ms"].asInt64(), 1800000000123LL);
        CHECK_EQ(body["job"]["attempt_no"].asInt(), 3);
        CHECK_EQ(body["payload"].size(), 6u);
        CHECK_EQ(body["payload"]["v"].asInt(), 1);
        CHECK_EQ(body["payload"]["nid"].asString(), attempt().notificationId);
        CHECK_EQ(body["subscription"]["endpoint"].asString(), std::string("https://fcm.googleapis.com/fcm/send/unit"));
    }

    TEST_CASE("0041: a bounded TTL reaches the worker request unchanged at both ends of its range, with the expiry untouched") {
        for (const int ttl : {1, 86400}) {
            auto request = sendRequest();
            request.ttlSeconds = d::boundedTtlSeconds(ttl, 86400.0);
            Json::Value body;
            Json::CharReaderBuilder builder;
            std::string errors;
            const std::string text = d::workerRequestBody(request);
            const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            CHECK(reader->parse(text.data(), text.data() + text.size(), &body, &errors));
            CHECK_EQ(body["ttl_seconds"].asInt(), ttl);
            CHECK_EQ(body["expires_at_ms"].asInt64(), 1800000000123LL);
        }
        auto over = sendRequest();
        over.ttlSeconds = d::boundedTtlSeconds(86401, 86400.0);
        CHECK(d::workerRequestBody(over).find("\"ttl_seconds\":86400") != std::string::npos);
    }

    TEST_CASE("P2-5c: worker responses are classified strictly, for this exact attempt only") {
        const auto k = attempt();
        auto outcome = [&](int status, const std::string &body) { return d::classifyWorkerResponse(k, status, body).outcome; };
        const auto accepted = d::classifyWorkerResponse(k, 200, resultJson(k, "accepted", "201"));
        CHECK_EQ(accepted.outcome, std::string("accepted"));
        CHECK_EQ(*accepted.httpStatus, 201);
        const auto later = d::classifyWorkerResponse(k, 200, resultJson(k, "transient", "429", "120"));
        CHECK_EQ(later.outcome, std::string("transient"));
        CHECK_EQ(*later.retryAfterSeconds, 120);
        CHECK_EQ(outcome(200, resultJson(k, "transport_unavailable")), std::string("transport_unavailable"));
        CHECK_EQ(outcome(200, resultJson(k, "ttl_skipped")), std::string("ttl_skipped"));
        CHECK_EQ(outcome(200, resultJson(k, "unknown")), std::string("unknown"));

        // Refused before anything began.
        CHECK_EQ(outcome(401, "{}"), std::string("worker_auth_failed"));
        CHECK_EQ(outcome(503, "{}"), std::string("busy"));
        for (int status : {400, 408, 413}) CHECK_EQ(outcome(status, "{}"), std::string("transport_unavailable"));
        // Anything else may have followed a send.
        for (int status : {201, 204, 302, 404, 500, 502}) CHECK_EQ(outcome(status, resultJson(k, "accepted", "201")), std::string("unknown"));

        // Another attempt, generation or notification is not this attempt's result.
        auto other = k;
        other.attemptNo = 2;
        CHECK_EQ(outcome(200, resultJson(other, "accepted", "201")), std::string("unknown"));
        other = k;
        other.generationId = "44444444-4444-4444-8444-444444444444";
        CHECK_EQ(outcome(200, resultJson(other, "gone", "410")), std::string("unknown"));
        other = k;
        other.notificationId = other.generationId;
        CHECK_EQ(outcome(200, resultJson(other, "gone", "410")), std::string("unknown"));

        // Every outcome against the statuses and delays it may and may not carry.
        struct Row {
            const char *outcome;
            const char *status;
            const char *retry;
            bool valid;
        };
        const Row matrix[] = {
            {"accepted", "200", "null", true}, {"accepted", "201", "null", true}, {"accepted", "299", "null", true},
            {"accepted", "null", "null", false}, {"accepted", "199", "null", false}, {"accepted", "300", "null", false},
            {"accepted", "404", "null", false}, {"accepted", "201", "0", false}, {"accepted", "201", "30", false},
            {"gone", "404", "null", true}, {"gone", "410", "null", true}, {"gone", "201", "null", false},
            {"gone", "null", "null", false}, {"gone", "403", "null", false}, {"gone", "500", "null", false},
            {"gone", "410", "30", false},
            {"provider_auth_failed", "401", "null", true}, {"provider_auth_failed", "403", "null", true},
            {"provider_auth_failed", "410", "null", false}, {"provider_auth_failed", "201", "null", false},
            {"provider_auth_failed", "500", "null", false}, {"provider_auth_failed", "null", "null", false},
            {"provider_auth_failed", "403", "30", false},
            {"transient", "408", "null", true}, {"transient", "425", "null", true}, {"transient", "429", "null", true},
            {"transient", "429", "0", true}, {"transient", "429", "86400", true}, {"transient", "500", "null", true},
            {"transient", "599", "120", true}, {"transient", "201", "null", false}, {"transient", "404", "null", false},
            {"transient", "410", "null", false}, {"transient", "400", "null", false}, {"transient", "null", "null", false},
            {"transient", "null", "30", false}, {"transient", "429", "86401", false}, {"transient", "429", "-1", false},
            {"rejected", "300", "null", true}, {"rejected", "302", "null", true}, {"rejected", "400", "null", true},
            {"rejected", "413", "null", true}, {"rejected", "499", "null", true}, {"rejected", "201", "null", false},
            {"rejected", "401", "null", false}, {"rejected", "403", "null", false}, {"rejected", "404", "null", false},
            {"rejected", "408", "null", false}, {"rejected", "410", "null", false}, {"rejected", "425", "null", false},
            {"rejected", "429", "null", false}, {"rejected", "500", "null", false}, {"rejected", "null", "null", false},
            {"rejected", "400", "30", false},
            {"unknown", "null", "null", true}, {"unknown", "201", "null", false}, {"unknown", "null", "30", false},
            {"transport_unavailable", "null", "null", true}, {"transport_unavailable", "503", "null", false},
            {"transport_unavailable", "null", "30", false},
            {"ttl_skipped", "null", "null", true}, {"ttl_skipped", "200", "null", false}, {"ttl_skipped", "null", "30", false},
            // Internal results the worker never reports in a 200.
            {"worker_auth_failed", "null", "null", false}, {"busy", "null", "null", false},
            {"inactive_skipped", "null", "null", false}, {"control_skipped", "null", "null", false},
            {"cutover_skipped", "null", "null", false}, {"terminal_skipped", "null", "null", false},
            {"lease_lost_skipped", "null", "null", false}, {"displayed", "null", "null", false},
        };
        for (const Row &row : matrix) {
            const auto result = d::classifyWorkerResponse(k, 200, resultJson(k, row.outcome, row.status, row.retry));
            const std::string label = std::string(row.outcome) + "/" + row.status + "/" + row.retry;
            CHECK_EQ(label + "=" + result.outcome, label + "=" + (row.valid ? row.outcome : "unknown"));
            if (row.valid) {
                CHECK_EQ(result.httpStatus.has_value(), std::string(row.status) != "null");
                CHECK_EQ(result.retryAfterSeconds.has_value(), std::string(row.retry) != "null");
            } else {
                // A refused response carries nothing forward.
                CHECK_FALSE(result.httpStatus.has_value());
                CHECK_FALSE(result.retryAfterSeconds.has_value());
            }
        }
        // A number must be written as an integer.
        CHECK_EQ(outcome(200, resultJson(k, "gone", "410.0")), std::string("unknown"));
        CHECK_EQ(outcome(200, resultJson(k, "gone", "true")), std::string("unknown"));
        const auto delayed = d::classifyWorkerResponse(k, 200, resultJson(k, "transient", "503", "120"));
        CHECK_EQ(delayed.outcome, std::string("transient"));
        CHECK_EQ(*delayed.httpStatus, 503);
        CHECK_EQ(*delayed.retryAfterSeconds, 120);
        for (const char *badDelay : {"120.0", "true"}) {
            const auto refused = d::classifyWorkerResponse(k, 200, resultJson(k, "transient", "503", badDelay));
            CHECK_EQ(refused.outcome, std::string("unknown"));
            CHECK_FALSE(refused.httpStatus.has_value());
            CHECK_FALSE(refused.retryAfterSeconds.has_value());
        }

        // Malformed, extra, or claiming more than the worker can know.
        for (const std::string &bad : std::vector<std::string>{
                 "", "not json", "[]", "null", "{}", resultJson(k, "displayed"), resultJson(k, "busy"), resultJson(k, "worker_auth_failed"),
                 resultJson(k, "inactive_skipped"), resultJson(k, "accepted"), resultJson(k, "accepted", "500"), resultJson(k, "gone"),
                 resultJson(k, "accepted", "\"201\""), resultJson(k, "accepted", "99"), resultJson(k, "transient", "503", "86401"),
                 resultJson(k, "transient", "503", "-1"), resultJson(k, "accepted", "201") + "trailing",
                 "{\"job\":" + jobJson(k) + ",\"outcome\":\"accepted\",\"http_status\":201}",
                 "{\"job\":" + jobJson(k) + ",\"outcome\":\"accepted\",\"http_status\":201,\"retry_after_s\":null,\"x\":1}",
                 "{\"job\":" + jobJson(k) + ",\"job\":" + jobJson(k) + ",\"outcome\":\"accepted\",\"http_status\":201,\"retry_after_s\":null}",
                 std::string(d::kMaxWorkerResponseBytes + 1, ' ') + resultJson(k, "accepted", "201")}) {
            CHECK_EQ(outcome(200, bad), std::string("unknown"));
        }
        // Only the absence of written bytes proves that nothing was sent.
        CHECK_EQ(d::classifyWorkerFailure(0u).outcome, std::string("transport_unavailable"));
        CHECK_EQ(d::classifyWorkerFailure(1u).outcome, std::string("unknown"));
    }

    TEST_CASE("P2-5c: the real transport authenticates and carries the request; a result arrives once") {
        std::string seen;
        const auto sent = scripted([&seen](int fd, const std::string &text) {
            seen = text;
            writeAll(fd, httpResponse(200, resultJson(attempt(), "accepted", "201")));
        }, 0.1);
        CHECK_EQ(sent.calls, 1);
        CHECK(sent.result.has_value());
        if (sent.result) CHECK_EQ(sent.result->outcome, std::string("accepted"));
        CHECK(seen.rfind("POST /v1/deliver HTTP/1.1\r\n", 0) == 0u);
        CHECK(seen.find(std::string("Bearer ") + kToken) != std::string::npos);
        CHECK(seen.find("\"ttl_seconds\":90") != std::string::npos);
        CHECK(seen.find("\"expires_at_ms\":1800000000123") != std::string::npos);
    }

    TEST_CASE("P2-5c: the real transport maps the worker's refusals and a mismatched job") {
        // A copy of the outcome: CHECK_EQ must not hold a reference into a temporary.
        const auto reply = [](int status, const std::string &body) -> std::string {
            const auto sent = scripted([=](int fd, const std::string &) { writeAll(fd, httpResponse(status, body)); });
            return sent.result ? sent.result->outcome : std::string("no result");
        };
        CHECK_EQ(reply(401, "{}"), std::string("worker_auth_failed"));
        CHECK_EQ(reply(503, "{}"), std::string("busy"));
        CHECK_EQ(reply(400, "{}"), std::string("transport_unavailable"));
        CHECK_EQ(reply(500, "{}"), std::string("unknown"));
        auto wrong = attempt();
        wrong.attemptNo = 4;
        CHECK_EQ(reply(200, resultJson(wrong, "accepted", "201")), std::string("unknown"));
        CHECK_EQ(reply(200, "{broken"), std::string("unknown"));
        CHECK_EQ(reply(200, std::string(4096, 'x')), std::string("unknown"));
    }

    TEST_CASE("P2-5c: a lost response is unknown, a refused connection is not sent") {
        // The worker read the request and the connection then ended: it may have sent.
        const auto lost = scripted([](int, const std::string &) {});
        CHECK(lost.result.has_value());
        if (lost.result) CHECK_EQ(lost.result->outcome, std::string("unknown"));

        // A reserved port that was listening a moment ago and is closed now.
        int port = 0;
        {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
            socklen_t length = sizeof(address);
            ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
            port = ntohs(address.sin_port);
            ::close(fd);
        }
        d::WorkerTransport refused(configFor("http://127.0.0.1:" + std::to_string(port)));
        const auto notSent = sendVia(refused);
        CHECK(notSent.result.has_value());
        if (notSent.result) CHECK_EQ(notSent.result->outcome, std::string("transport_unavailable"));
    }

    TEST_CASE("P2-5c: an absolute deadline ends a stalled worker, and a late response changes nothing") {
        const auto sent = scripted([](int fd, const std::string &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // past the 1 s test deadline
            writeAll(fd, httpResponse(200, resultJson(attempt(), "accepted", "201")));
        }, 0.8);
        CHECK(sent.result.has_value());
        if (sent.result) CHECK_EQ(sent.result->outcome, std::string("unknown"));
        CHECK_EQ(sent.calls, 1);
        CHECK(sent.seconds >= 0.9);
        CHECK(sent.seconds < 1.4);

        // A worker that keeps dribbling a response never extends it.
        const auto dribble = scripted([](int fd, const std::string &) {
            writeAll(fd, "HTTP/1.1 200 OK\r\n");
            for (int i = 0; i < 30; ++i) {
                writeAll(fd, "X-Pad: y\r\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }, 0.0);
        CHECK(dribble.result.has_value());
        if (dribble.result) CHECK_EQ(dribble.result->outcome, std::string("unknown"));
        CHECK(dribble.seconds < 1.4);
    }

    TEST_CASE("P2-5c: Drogon v1.9.11 does not cancel a queued request when its client reference is dropped") {
        // Why the transport cannot rely on releasing its client: this characterizes the dependency.
        SaturatedListener listener;
        CHECK(listener.saturated());
        trantor::EventLoopThread loop("drop-reference");
        loop.run();
        {
            auto client = drogon::HttpClient::newHttpClient(listener.url(), loop.getLoop());
            auto http = drogon::HttpRequest::newHttpRequest();
            http->setMethod(drogon::Post);
            http->setPath("/v1/deliver");
            http->setBody("{}");
            client->sendRequest(http, [](drogon::ReqResult, const drogon::HttpResponsePtr &) {});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(listener.releaseAndWatch(8.0) > 0u);
    }

    TEST_CASE("P2-5c: a deadline during connection establishment sends nothing, then or later") {
        SaturatedListener listener;
        CHECK(listener.saturated());
        d::WorkerTransport transport(configFor(listener.url(), 0.3));
        const auto sent = sendVia(transport);
        CHECK_EQ(sent.calls, 1);
        CHECK(sent.seconds < 1.0);
        CHECK(sent.result.has_value());
        // Provably never sent, so not a possibly-sent attempt.
        if (sent.result) CHECK_EQ(sent.result->outcome, std::string("transport_unavailable"));
        CHECK_EQ(listener.releaseAndWatch(5.0), 0u);
    }

    TEST_CASE("P2-5c: a deadline after the request was written is unknown, and the connection is closed") {
        std::atomic<bool> sawEnd{false};
        const auto sent = scripted([&sawEnd](int fd, const std::string &) {
            pollfd waiting{fd, POLLIN, 0};
            char buffer[64];
            // The worker stalls. Our deadline is 1 s; the socket must end well before this 3 s wait does.
            if (::poll(&waiting, 1, 3000) > 0 && ::recv(fd, buffer, sizeof(buffer), 0) == 0) sawEnd = true;
        });
        CHECK(sent.result.has_value());
        if (sent.result) CHECK_EQ(sent.result->outcome, std::string("unknown"));
        CHECK(sent.seconds < 1.4);
        CHECK(sawEnd.load());
    }
}

}  // namespace usapptest::pushworkerunit
