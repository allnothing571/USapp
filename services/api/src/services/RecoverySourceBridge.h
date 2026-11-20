#pragma once
#include "services/DeletionRecovery.h"
#include "services/DeletionJournalBridge.h"

namespace usapp::deletionrecovery::bridge {
struct Context { std::string journalId, databaseId, attemptId; };
// Separate MAC domain from publication; ASCII 64-hex key bytes match the Python reader protocol.
std::string mac(const std::string &key, const std::string &direction, const std::string &body);
// Authenticated material consumer only. Caller must independently pin the reader endpoint/key/current restore context
// and guarantee the reader's external authoritative-current-read contract. No environment/main/release factory.
class WireSource final : public Source {
  public:
    using Exchange = deletionjournal::bridge::Exchange;
    using Nonce = deletionjournal::bridge::Nonce;
    WireSource(Context, std::string key, Exchange, Nonce = {});
    bool binds(const Request &) const noexcept override;
    void snapshot(const std::string &, std::function<void(bool, Snapshot)>) override;
    void currentHead(const std::string &, std::function<void(bool, Head)>) override;
  private:
    Context context_;
    std::string key_;
    Exchange exchange_;
    Nonce nonce_;
    void call(const std::string &, const std::string &, std::function<void(bool, Snapshot)>);
};
} // namespace usapp::deletionrecovery::bridge
