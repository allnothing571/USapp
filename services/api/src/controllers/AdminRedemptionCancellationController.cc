#include <drogon/HttpController.h>
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "services/AdminRedemptionCancellationService.h"
#include "services/TaskService.h"
namespace usapp::controllers {
namespace {
using Callback=std::function<void(const drogon::HttpResponsePtr &)>;
using A=AdminRedemptionCancellationService;
void respond(const drogon::HttpRequestPtr &req,const Callback &cb,const A::Result &r){
 switch(r.outcome){
  case A::Outcome::Ok:cb(jsonResponse(r.body,200));return;
  case A::Outcome::Validation:cb(ctrl::fail(ErrorCode::ValidationError,"取消状态或原因无效",req));return;
  case A::Outcome::Forbidden:cb(ctrl::fail(ErrorCode::RedemptionForbidden,"管理员当前无权取消兑换",req));return;
  case A::Outcome::NotFound:cb(ctrl::fail(ErrorCode::RedemptionNotFound,"兑换或退款账户不可用",req));return;
  case A::Outcome::Conflict:cb(ctrl::fail(ErrorCode::RedemptionStateConflict,"兑换状态已变化，请重新读取并确认",req));return;
  case A::Outcome::BalanceBound:cb(ctrl::fail(ErrorCode::WalletBalanceLimit,"退款后余额超出允许范围，未取消兑换",req));return;
  case A::Outcome::KeyReused:cb(ctrl::fail(ErrorCode::IdempotencyKeyReused,"请求标识已用于不同的取消操作",req));return;
  case A::Outcome::Unknown:cb(ctrl::fail(ErrorCode::ResultUnknown,"结果未确认，请保留原请求并使用相同标识重试",req));return;
  case A::Outcome::Error:break;
 }
 cb(ctrl::fail(ErrorCode::InternalError,"服务内部错误",req));
}
}
class AdminRedemptionCancellationController:public drogon::HttpController<AdminRedemptionCancellationController>{
 public:
 METHOD_LIST_BEGIN
 ADD_METHOD_TO(AdminRedemptionCancellationController::create,"/api/admin/v1/couples/{coupleId}/redemptions/{redemptionId}/cancellations",drogon::Post,AdminFilter::classTypeName());
 METHOD_LIST_END
 void create(const drogon::HttpRequestPtr &req,Callback &&cb,std::string space,std::string redemption){
  const auto admin=adminctx::userId(req);
  if(admin.empty()||!adminctx::isAuthenticated(req)){cb(ctrl::fail(ErrorCode::Unauthenticated,"管理员未登录",req));return;}
  for(auto *id:{&space,&redemption})for(char &c:*id)if(c>='A'&&c<='F')c=static_cast<char>(c-'A'+'a');
  if(!ctrl::looksLikeUuid(space)||!ctrl::looksLikeUuid(redemption)){cb(ctrl::fail(ErrorCode::ValidationError,"空间或兑换标识无效",req));return;}
  const auto key=req->getHeader("idempotency-key");
  if(!TaskService::validIdempotencyKey(key)){cb(ctrl::fail(ErrorCode::ValidationError,"缺少或无效的 Idempotency-Key",req));return;}
  Json::Value body;A::Request command;
  if(!ctrl::parseBody(req,cb,body))return;
  if(!A::parse(body,command)){cb(ctrl::fail(ErrorCode::ValidationError,"请填写已核对的待处理状态和完整原因",req));return;}
  A::cancel(admin,space,redemption,key,command,[req,cb](A::Result r){respond(req,cb,r);});
 }
};
}
