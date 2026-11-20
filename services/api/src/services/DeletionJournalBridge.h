#pragma once
#include "services/DeletionJournal.h"

namespace usapp::deletionjournal::bridge {
constexpr size_t kMaxWireBytes = 65536;
constexpr size_t kMaxSubjects = 256;
struct Request { std::string body, signature; };
struct Reply { int status = 0; std::string body, signature; };
// Injected transport only: must bound queues, wall time and streamed bytes, keep confidentiality and serialize callbacks.
// There is deliberately no network/config/env factory or production main wiring in this batch.
using Exchange = std::function<void(Request, std::function<void(Reply)>)>;
using Nonce = std::function<std::string()>;
std::string mac(const std::string &key, const std::string &direction, const std::string &body);

class WireJournal final : public Journal {
  public:
    WireJournal(std::string journalId, std::string key, Exchange, Nonce = {});
    void ensureIntent(const Operation &, std::function<void(IntentState)>) override;
    void settle(const Operation &, EffectState, std::function<void(bool)>) override;
    void lookupOperation(const std::string &, std::function<void(LookupState, Operation)>) override;
  private:
    std::string journalId_, key_;
    Exchange exchange_;
    Nonce nonce_;
    void call(const std::string &method, const Operation *operation, const std::string &operationId,
              const std::string &phase, std::function<void(std::string, Operation)>);
};
} // namespace usapp::deletionjournal::bridge
