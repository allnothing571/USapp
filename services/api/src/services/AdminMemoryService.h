#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
class AdminMemoryService {
 public:
 enum class Outcome { Ok, SpaceNotFound, NotFound, Error };
 struct Result { Outcome outcome=Outcome::Error; Json::Value body; };
 struct Cursor { std::int64_t createdMicros=0; std::string id; };
 using Callback=std::function<void(Result)>;
 static std::string encodeCursor(const std::string &,const Cursor &,const std::string &memory="");
 static bool decodeCursor(const std::string &,const std::string &,Cursor &,const std::string &memory="");
 static void list(const std::string &,int,const Cursor *,Callback);
 static void detail(const std::string &,const std::string &,Callback);
 static void proposals(const std::string &,const std::string &,int,const Cursor *,Callback);
 static void comparison(const std::string &,const std::string &,const std::string &,Callback);
};
}
