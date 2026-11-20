#pragma once
#include "services/DeletionJournalBridge.h"

namespace usapp::deletionjournal::bridge {
struct HttpsConfig {
    std::string origin;             // https://host[:port], fixed /v1/journal path; no user-controlled URL.
    std::string caFile;             // empty uses system trust; neither peer nor name verification can be disabled.
    long deadlineMs = 5000;         // admission through complete response, steady clock, not idle timeout.
    unsigned maxOutstanding = 8;    // active plus queued calls; admission over this bound fails immediately.
    unsigned maxConnections = 2;
    // Hermetic tests only: preserves URL/SNI/name validation; never read from environment.
    std::string testConnectTo;
};
bool validHttpsConfig(const HttpsConfig &);
// Owns one serialized I/O thread. Admitted callbacks run there and must be nonblocking.
// Invalid/admission/stopped rejection is immediate on the caller, outside locks. Never retries.
// Keep this owner alive while in use; destruction cancels pending work with an unknown reply.
class HttpsTransport final {
  public:
    explicit HttpsTransport(HttpsConfig);
    ~HttpsTransport();
    HttpsTransport(const HttpsTransport &) = delete;
    HttpsTransport &operator=(const HttpsTransport &) = delete;
    Exchange exchange() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace usapp::deletionjournal::bridge
