#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
class AdminAlbumService {
 public:
    enum class Outcome { Ok, SpaceNotFound, AlbumNotFound, Error };
    struct Result { Outcome outcome=Outcome::Error; Json::Value body; };
    struct Cursor { std::int64_t createdMicros=0; std::string albumId; };
    using Callback=std::function<void(Result)>;
    static std::string encodeCursor(const std::string &,const Cursor &);
    static bool decodeCursor(const std::string &,const std::string &,Cursor &);
    static std::string encodeItemCursor(const std::string &,const std::string &,bool,int);
    static bool decodeItemCursor(const std::string &,const std::string &,const std::string &,bool,int &);
    static void list(const std::string &,int,const Cursor *,Callback);
    static void detail(const std::string &,const std::string &,Callback);
    static void items(const std::string &,const std::string &,bool,int,int,Callback);
};
}
