#pragma once
#include <functional>
#include <memory>
#include <string>
#include "db/Db.h"

namespace usapp {
// Called ONLY for a newly inserted immutable source, after the caller's audit insert.
// No independent transaction, commit, locks, Push or business-state mutation.
void writeNotification(const std::shared_ptr<db::TxScope> &scope, std::string family,
                       std::string sourceId, std::function<void()> next,
                       std::function<void()> failed);
}
