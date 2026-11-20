#include "services/AdminRedemptionCancellationService.h"
#include <drogon/drogon.h>
#include <atomic>
#include <array>
#include <climits>
#include <memory>
#include <regex>
#include "db/Db.h"
namespace usapp {
namespace {
using A=AdminRedemptionCancellationService;
bool json(const std::string &s,Json::Value &v){Json::CharReaderBuilder b;std::unique_ptr<Json::CharReader> r(b.newCharReader());std::string e;return r->parse(s.data(),s.data()+s.size(),&v,&e)&&v.isObject();}
A::Result fail(A::Outcome o){A::Result r;r.outcome=o;return r;}
A::Outcome refusal(const std::string &s){
 if(s=="validation")return A::Outcome::Validation;
 if(s=="admin_forbidden")return A::Outcome::Forbidden;
 if(s=="not_found"||s=="account_unavailable")return A::Outcome::NotFound;
 if(s=="conflict")return A::Outcome::Conflict;
 if(s=="balance_bound")return A::Outcome::BalanceBound;
 if(s=="key_reused")return A::Outcome::KeyReused;
 return A::Outcome::Error;
}
}
bool A::parse(const Json::Value &body,Request &out){
 if(!body.isObject()||body.size()!=2u||!body["expected_status"].isString()||!body["reason"].isString())return false;
 out={body["expected_status"].asString(),body["reason"].asString()};
 if((out.expectedStatus!="pending_confirmation"&&out.expectedStatus!="pending_fulfillment")||out.reason.empty()||out.reason.size()>2000u)return false;
 for(unsigned char c:out.reason)if(c<32||c==127)return false;
 return true; // SQL also checks Unicode character count, controls and exact trimmed terms.
}
void A::cancel(const std::string &admin,const std::string &space,const std::string &redemption,const std::string &key,const Request &r,Callback cb,Fault fault){
 auto done=std::make_shared<Callback>(std::move(cb));auto answered=std::make_shared<std::atomic<bool>>(false);
 auto finish=[done,answered](Result value){if(!answered->exchange(true))(*done)(std::move(value));};
 const auto args=std::make_shared<std::array<std::string,6>>(std::array<std::string,6>{admin,space,redemption,r.expectedStatus,r.reason,key});
 db::TxScope::beginAsync([args,finish,fault](std::shared_ptr<db::TxScope> tx){
  tx->tx().execSqlAsync("SELECT usapp_admin_redemption_cancel($1::uuid,$2::uuid,$3::uuid,$4,$5,$6)::text AS r",
   [tx,finish,fault](const drogon::orm::Result &rows){
    Json::Value receipt;
    if(rows.size()!=1u||!json(rows[0]["r"].as<std::string>(),receipt)){finish(fail(Outcome::Error));tx->fail("admin cancellation unreadable");return;}
    if(receipt.isMember("error")){finish(fail(refusal(receipt["error"].asString())));tx->fail("admin cancellation refused");return;}
    if(!receipt["cancellation"].isObject()){finish(fail(Outcome::Error));tx->fail("admin cancellation missing receipt");return;}
    Result result;result.outcome=Outcome::Ok;result.body=std::move(receipt);
    tx->commit([finish,result,fault]{if(fault==Fault::DiscardConfirmedCommitReply)finish(fail(Outcome::Unknown));else finish(result);});
   },[tx,finish](const drogon::orm::DrogonDbException &){LOG_ERROR<<"admin cancellation statement failed";finish(fail(Outcome::Error));tx->fail("admin cancellation statement failed");},
   (*args)[0],(*args)[1],(*args)[2],(*args)[3],(*args)[4],(*args)[5]);
 },[finish](const std::string &){finish(fail(Outcome::Unknown));});
}
}
