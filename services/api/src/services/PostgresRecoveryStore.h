#pragma once
#include "services/DeletionRecovery.h"
#include "db/Db.h"

namespace usapp::deletionrecovery {
// J3b maintenance adapter. No default client, configuration, route, or main wiring.
// usapp_api has no write/EXECUTE permission: using it here fails closed.
class PostgresStore final : public Store {
  public:
    explicit PostgresStore(db::ClientPtr client) : client_(std::move(client)) {}
    void close(const Request &, std::function<void(Receipt, Fence)>) override;
    void stage(const Fence &, const Snapshot &, std::function<void(Receipt)>) override;
    void verify(const Fence &, const Snapshot &, std::function<void(Receipt)>) override;
    void recordVerified(const Fence &, const Head &, std::function<void(Receipt)>) override;
  private:
    db::ClientPtr client_;
};
} // namespace usapp::deletionrecovery
