#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
class AdminNotificationService {
  public:
    enum class Outcome {Ok,UserNotFound,Error};
    struct Result {Outcome outcome=Outcome::Error;Json::Value body;};
    struct Cursor {std::int64_t createdMicros=0;std::string notificationId;};
    using Callback=std::function<void(Result)>;
    static std::string encodeCursor(const std::string &userId,const Cursor &cursor);
    static bool decodeCursor(const std::string &raw,const std::string &userId,Cursor &cursor);
    static void list(const std::string &userId,int limit,const Cursor *cursor,Callback cb);
};
}
