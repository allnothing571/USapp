#pragma once
#include <functional>
#include <optional>
#include <string>
#include <json/json.h>
#include "services/MessageService.h"

namespace usapp {
class NotificationService {
 public:
    struct Result {
        int status = 500;
        Json::Value body;
    };
    using Callback = std::function<void(Result)>;
    using Cursor = MessageService::Cursor;
    static void list(const std::string &user, int limit, const std::optional<Cursor> &cursor,
                     bool unreadOnly, Callback cb);
    static void unreadCount(const std::string &user, Callback cb);
    // Naturally idempotent by notification id. No client-supplied recipient or timestamp.
    static void markRead(const std::string &user, const std::string &id, Callback cb);
};
}
