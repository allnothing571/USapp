#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
// Admin-only read model; each response is one statement. No writes or financial mutations.
class AdminRedemptionService {
  public:
    enum class Outcome { Ok, SpaceNotFound, RedemptionNotFound, Error };
    struct Result { Outcome outcome=Outcome::Error; Json::Value body; };
    using Callback=std::function<void(Result)>;
    struct Cursor { std::int64_t createdMicros=0; std::string redemptionId; };
    static bool validStatus(const std::string &status);
    static std::string encodeCursor(const std::string &coupleId,const std::string &status,const Cursor &cursor);
    static bool decodeCursor(const std::string &raw,const std::string &coupleId,const std::string &status,Cursor &out);
    static void list(const std::string &coupleId,const std::string &status,int limit,const Cursor *cursor,Callback cb);
    static void detail(const std::string &coupleId,const std::string &redemptionId,Callback cb);
    static constexpr int kEventItems=100;
};
}
