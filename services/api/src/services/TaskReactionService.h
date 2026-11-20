#pragma once
#include <functional>
#include <string>
#include <json/json.h>
#include "core/Errors.h"
#include "services/TaskService.h"

namespace usapp {
class TaskReactionService {
 public:
    struct Input { std::string kind, emojiCode, body, periodDate; };
    enum class Outcome { Created, Replayed, Validation, NotFound, NotCompleted, Forbidden,
                         AlreadyReacted, KeyReused, ResultUnknown, Error };
    struct Status {
        Outcome outcome = Outcome::Error;
        Json::Value mutation;
        TaskService::TaskData task;
    };
    using Callback = std::function<void(Status)>;
    static bool parse(const Json::Value &body, Input &out, std::string &message);
    static std::string fingerprint(const std::string &rootId, const Input &input);
    static ErrorCode errorCode(Outcome outcome);
    static void react(const std::string &userId, const std::string &rootId,
                      const std::string &key, const Input &input, Callback cb);
};
}
