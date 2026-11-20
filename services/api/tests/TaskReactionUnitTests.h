#pragma once
#include "services/TaskReactionService.h"
#include "TestSupport.h"
namespace usapptest::reactionunit {
inline void run() {
 using S=usapp::TaskReactionService; S::Input in; std::string error;
 TEST_CASE("P2-4: strict reaction parser, code points, periods and fingerprints") {
  Json::Value b;b["kind"]="emoji";b["emoji_code"]="heart";
  CHECK(S::parse(b,in,error));auto first=S::fingerprint("root",in);
  b["emoji_code"]="clap";CHECK(S::parse(b,in,error));CHECK(first!=S::fingerprint("root",in));
  b["body"]="extra";CHECK_FALSE(S::parse(b,in,error));b.removeMember("body");
  b["emoji_code"]="unknown";CHECK_FALSE(S::parse(b,in,error));
  b["emoji_code"]="flower";b["period_date"]="2026-02-30";CHECK_FALSE(S::parse(b,in,error));
  b["period_date"]="2026-10-26";CHECK(S::parse(b,in,error));
  b=Json::Value(Json::objectValue);b["kind"]="text";b["body"]="  好棒  ";CHECK(S::parse(b,in,error));CHECK_EQ(in.body,std::string("好棒"));
  b["body"]="one\ntwo";CHECK_FALSE(S::parse(b,in,error));
  b["body"]=" ";CHECK_FALSE(S::parse(b,in,error));
  std::string forty;for(int n=0;n<40;++n) forty+="🌸";
  b["body"]=forty;CHECK(S::parse(b,in,error));b["body"]=forty+"🌸";CHECK_FALSE(S::parse(b,in,error));
  CHECK(S::errorCode(S::Outcome::NotCompleted)==usapp::ErrorCode::TaskNotCompleted);
  CHECK(S::errorCode(S::Outcome::Forbidden)==usapp::ErrorCode::TaskReactionForbidden);
  CHECK(S::errorCode(S::Outcome::AlreadyReacted)==usapp::ErrorCode::TaskAlreadyReacted);
 }
}
}
