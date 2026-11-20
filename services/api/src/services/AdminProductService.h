#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
class AdminProductService {
 public:
    enum class Outcome {Ok,SpaceNotFound,NotFound,Error};
    struct Result {Outcome outcome=Outcome::Error;Json::Value body;};
    struct Cursor {std::int64_t createdMicros=0;std::string productId;};
    using Callback=std::function<void(Result)>;
    static std::string encodeCursor(const std::string &coupleId,const Cursor &cursor);
    static bool decodeCursor(const std::string &raw,const std::string &coupleId,Cursor &cursor);
    static void list(const std::string &coupleId,int limit,const Cursor *cursor,Callback cb);
    static void detail(const std::string &coupleId,const std::string &productId,Callback cb);
};
}
