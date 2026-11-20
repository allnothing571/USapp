#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
namespace usapp {
class AdminRedemptionCancellationService {
 public:
 enum class Outcome {Ok,Validation,Forbidden,NotFound,Conflict,BalanceBound,KeyReused,Unknown,Error};
 enum class Fault {None,DiscardConfirmedCommitReply};
 struct Request {std::string expectedStatus,reason;};
 struct Result {Outcome outcome=Outcome::Error;Json::Value body;};
 using Callback=std::function<void(Result)>;
 static bool parse(const Json::Value &,Request &);
 static void cancel(const std::string &admin,const std::string &space,const std::string &redemption,const std::string &key,const Request &,Callback,Fault=Fault::None);
};
}
