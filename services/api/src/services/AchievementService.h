#pragma once
#include <functional>
#include <json/json.h>
#include <string>
namespace usapp {
class AchievementService {
  public:
    enum class Outcome { Ok, SpaceNotFound, Error };
    struct Result { Outcome outcome = Outcome::Error; Json::Value body; };
    using Callback = std::function<void(Result)>;
    static void read(const std::string &userId, Callback cb);
};
}
