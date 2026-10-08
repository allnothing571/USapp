#include <drogon/HttpController.h>
#include <functional>
#include <string>

#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/WalletService.h"

namespace usapp::controllers {

class WalletController : public drogon::HttpController<WalletController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(WalletController::getWallet, "/api/v1/wallet", drogon::Get,
                  AuthFilter::classTypeName());
    METHOD_LIST_END

    void getWallet(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        std::string userId;
        if (!ctrl::requireUser(req, callback, userId)) return;
        WalletService::instance().getOwnWallet(
            userId, [req, callback](WalletService::Outcome outcome, WalletBalances balances) {
                if (outcome == WalletService::Outcome::SpaceNotFound) {
                    callback(ctrl::fail(ErrorCode::SpaceNotFound, "尚未加入空间", req));
                } else if (outcome != WalletService::Outcome::Ok) {
                    callback(ctrl::fail(ErrorCode::InternalError, "资产暂时无法加载，请稍后重试", req));
                } else {
                    callback(jsonResponse(WalletService::responseBody(balances), 200));
                }
            });
    }
};

}  // namespace usapp::controllers
