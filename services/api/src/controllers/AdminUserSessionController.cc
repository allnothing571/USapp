#include <drogon/HttpController.h>
#include "http/AdminFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AdminUserSessionService.h"
#include "services/TaskService.h"
namespace usapp::controllers {
class AdminUserSessionController:public drogon::HttpController<AdminUserSessionController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminUserSessionController::list,"/api/admin/v1/users/{userId}/sessions",drogon::Get,AdminFilter::classTypeName());
    METHOD_LIST_END
    void list(const drogon::HttpRequestPtr &req,std::function<void(const drogon::HttpResponsePtr &)> &&cb,std::string userId){
        if(adminctx::userId(req).empty()){cb(ctrl::fail(ErrorCode::Unauthenticated,"管理员未登录",req));return;}
        for(auto &ch:userId)if(ch>='A'&&ch<='F')ch=static_cast<char>(ch-'A'+'a');
        if(!ctrl::looksLikeUuid(userId)){cb(ctrl::fail(ErrorCode::ValidationError,"用户 id 格式无效",req));return;}
        const auto limit=TaskService::parseLimit(req->getParameter("limit"));
        if(!limit){cb(ctrl::fail(ErrorCode::ValidationError,"limit 必须是 1-100 的整数",req));return;}
        const auto raw=req->getParameter("cursor");AdminUserSessionService::Cursor cursor;
        if(!raw.empty()&&!AdminUserSessionService::decodeCursor(raw,userId,cursor)){
            cb(ctrl::fail(ErrorCode::ValidationError,"游标无效或不属于该用户",req));return;
        }
        AdminUserSessionService::list(userId,*limit,raw.empty()?nullptr:&cursor,[req,cb](AdminUserSessionService::Result r){
            switch(r.outcome){
                case AdminUserSessionService::Outcome::Ok:cb(jsonResponse(r.body,200));return;
                case AdminUserSessionService::Outcome::UserNotFound:cb(ctrl::fail(ErrorCode::NotFound,"用户不存在",req));return;
                case AdminUserSessionService::Outcome::Error:cb(ctrl::fail(ErrorCode::InternalError,"服务内部错误",req));return;
            }
        });
    }
};
}
