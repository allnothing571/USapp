#include <drogon/HttpController.h>
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "services/AdminWalletAdjustmentService.h"
#include "services/TaskService.h"
namespace usapp::controllers {
namespace {
using Callback=std::function<void(const drogon::HttpResponsePtr &)>;
using A=AdminWalletAdjustmentService;
void respond(const drogon::HttpRequestPtr &req,const Callback &cb,const A::Result &r){
 switch(r.outcome){
  case A::Outcome::Ok:cb(jsonResponse(r.body,200));return;
  case A::Outcome::Validation:cb(ctrl::fail(ErrorCode::ValidationError,"纠错参数或原因无效",req));return;
  case A::Outcome::Forbidden:cb(ctrl::fail(ErrorCode::AdminAdjustmentForbidden,"管理员当前无权执行纠错",req));return;
  case A::Outcome::NotFound:cb(ctrl::fail(ErrorCode::NotFound,"目标用户或有效空间不可用",req));return;
  case A::Outcome::Conflict:cb(ctrl::fail(ErrorCode::AdminAdjustmentConflict,"账户流水已变化，请重新读取并确认",req));return;
  case A::Outcome::BalanceBound:cb(ctrl::fail(ErrorCode::WalletBalanceLimit,"纠错后余额超出允许范围",req));return;
  case A::Outcome::KeyReused:cb(ctrl::fail(ErrorCode::IdempotencyKeyReused,"请求标识已用于不同的纠错",req));return;
  case A::Outcome::Unknown:cb(ctrl::fail(ErrorCode::ResultUnknown,"结果未确认，请保留原请求并使用相同标识重试",req));return;
  case A::Outcome::Error:break;
 }
 cb(ctrl::fail(ErrorCode::InternalError,"服务内部错误",req));
}
}
class AdminWalletAdjustmentController:public drogon::HttpController<AdminWalletAdjustmentController>{
 public:
 METHOD_LIST_BEGIN
 ADD_METHOD_TO(AdminWalletAdjustmentController::create,"/api/admin/v1/users/{userId}/wallet/adjustments",drogon::Post,AdminFilter::classTypeName());
 METHOD_LIST_END
 void create(const drogon::HttpRequestPtr &req,Callback &&cb,std::string user){
  const auto admin=adminctx::userId(req);
  if(admin.empty()||!adminctx::isAuthenticated(req)){cb(ctrl::fail(ErrorCode::Unauthenticated,"管理员未登录",req));return;}
  for(char &c:user)if(c>='A'&&c<='F')c=static_cast<char>(c-'A'+'a');
  if(!ctrl::looksLikeUuid(user)){cb(ctrl::fail(ErrorCode::ValidationError,"用户标识无效",req));return;}
  const auto key=req->getHeader("idempotency-key");
  if(!TaskService::validIdempotencyKey(key)){cb(ctrl::fail(ErrorCode::ValidationError,"缺少或无效的 Idempotency-Key",req));return;}
  Json::Value body;A::Request command;
  if(!ctrl::parseBody(req,cb,body))return;
  if(!A::parse(body,command)){cb(ctrl::fail(ErrorCode::ValidationError,"请填写资产、非零整数差额、账户序号和原因",req));return;}
  A::adjust(admin,user,key,command,[req,cb](A::Result r){respond(req,cb,r);});
 }
};
}
