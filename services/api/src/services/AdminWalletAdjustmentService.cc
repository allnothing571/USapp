#include "services/AdminWalletAdjustmentService.h"
#include <drogon/drogon.h>
#include <atomic>
#include <array>
#include <climits>
#include <memory>
#include <regex>
#include "db/Db.h"
namespace usapp {
namespace {
using A=AdminWalletAdjustmentService;
bool decimal(const std::string &s){
 if(!std::regex_match(s,std::regex("^(0|[1-9][0-9]{0,18})$")))return false;
 std::uint64_t n=0;for(char c:s)n=n*10+static_cast<unsigned>(c-'0');return n<=static_cast<std::uint64_t>(INT64_MAX);
}
bool json(const std::string &s,Json::Value &v){Json::CharReaderBuilder b;std::unique_ptr<Json::CharReader> r(b.newCharReader());std::string e;return r->parse(s.data(),s.data()+s.size(),&v,&e)&&v.isObject();}
A::Result fail(A::Outcome o){A::Result r;r.outcome=o;return r;}
A::Outcome refusal(const std::string &s){
 if(s=="validation")return A::Outcome::Validation;
 if(s=="admin_forbidden")return A::Outcome::Forbidden;
 if(s=="target_unavailable")return A::Outcome::NotFound;
 if(s=="conflict")return A::Outcome::Conflict;
 if(s=="balance_bound")return A::Outcome::BalanceBound;
 if(s=="key_reused")return A::Outcome::KeyReused;
 return A::Outcome::Error;
}
}
bool A::parse(const Json::Value &body,Request &out){
 if(!body.isObject()||body.size()!=4u)return false;
 for(const char *k:{"asset","delta","expected_seq","reason"})if(!body.isMember(k)||!body[k].isString())return false;
 out={body["asset"].asString(),body["delta"].asString(),body["expected_seq"].asString(),body["reason"].asString()};
 if((out.asset!="flower"&&out.asset!="sunshine")||!std::regex_match(out.delta,std::regex("^-?[1-9][0-9]{0,3}$"))||!decimal(out.expectedSeq)||out.reason.empty()||out.reason.size()>2000u)return false;
 for(unsigned char c:out.reason)if(c<32||c==127)return false;
 return true; // SQL validates Unicode character count and exact trimmed terms as well.
}
void A::adjust(const std::string &admin,const std::string &user,const std::string &key,const Request &r,Callback cb,Fault fault){
 auto done=std::make_shared<Callback>(std::move(cb));auto answered=std::make_shared<std::atomic<bool>>(false);
 auto finish=[done,answered](Result value){if(!answered->exchange(true))(*done)(std::move(value));};
 const auto args=std::make_shared<std::array<std::string,7>>(std::array<std::string,7>{admin,user,r.asset,r.delta,r.expectedSeq,r.reason,key});
 db::TxScope::beginAsync([args,finish,fault](std::shared_ptr<db::TxScope> tx){
  tx->tx().execSqlAsync("SELECT usapp_admin_wallet_adjust($1::uuid,$2::uuid,$3,$4::integer,$5::bigint,$6,$7)::text AS r",
   [tx,finish,fault](const drogon::orm::Result &rows){
    Json::Value receipt;
    if(rows.size()!=1u||!json(rows[0]["r"].as<std::string>(),receipt)){finish(fail(Outcome::Error));tx->fail("admin adjustment unreadable");return;}
    if(receipt.isMember("error")){finish(fail(refusal(receipt["error"].asString())));tx->fail("admin adjustment refused");return;}
    if(!receipt["adjustment"].isObject()){finish(fail(Outcome::Error));tx->fail("admin adjustment missing receipt");return;}
    Result result;result.outcome=Outcome::Ok;result.body=std::move(receipt);
    tx->commit([finish,result,fault]{if(fault==Fault::DiscardConfirmedCommitReply)finish(fail(Outcome::Unknown));else finish(result);});
   },[tx,finish](const drogon::orm::DrogonDbException &){LOG_ERROR<<"admin adjustment statement failed";finish(fail(Outcome::Error));tx->fail("admin adjustment statement failed");},
   (*args)[0],(*args)[1],(*args)[2],(*args)[3],(*args)[4],(*args)[5],(*args)[6]);
 },[finish](const std::string &){finish(fail(Outcome::Unknown));});
}
}
