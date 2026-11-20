// Synthetic file transcript: C++ emits requests, Python injected dispatcher replies, C++ validates them.
#include "DeletionJournalBridgeUnitTests.h"
#include <fstream>
#include <iomanip>
#include <sstream>
using namespace usapp::deletionjournal;
namespace u=usapptest::journalbridgeunit;
int main(int argc,char **argv){
    if(argc!=3||(std::string(argv[1])!="emit"&&std::string(argv[1])!="verify"))return 2;
    const bool emit=std::string(argv[1])=="emit";
    Json::Value transcript(Json::objectValue),responses;
    if(!emit){std::ifstream in(argv[2]);std::string text((std::istreambuf_iterator<char>(in)),{});responses=u::json(text);if(!responses.isArray()||responses.size()!=12)return 2;}
    transcript["synthetic_only"]=true;transcript["journal_id"]=u::journal;transcript["key"]=u::key;transcript["requests"]=Json::Value(Json::arrayValue);
    unsigned index=0,nonce=0;
    auto a=usapptest::deletionjournalunit::sample(401);auto changed=a;changed.requestFingerprint=std::string(64,'c');
    auto cleanup=usapptest::deletionjournalunit::sample(402,Action::Cleanup);cleanup.subjects[0].kind=Kind::Media;
    auto exchange=[&](u::b::Request req,auto done){
        Json::Value row(Json::objectValue);row["body"]=req.body;row["signature"]=req.signature;transcript["requests"].append(row);
        if(emit){done({});}else{const auto r=responses[index];done({r["status"].asInt(),r["body"].asString(),r["signature"].asString()});}index++;
    };
    u::b::WireJournal bridge(u::journal,u::key,exchange,[&](){std::ostringstream n;n<<std::hex<<std::setw(32)<<std::setfill('0')<<++nonce;return n.str();});
    auto intent=[&](const Operation &op,IntentState expected){bridge.ensureIntent(op,[&](IntentState got){if(!emit)CHECK(got==expected);});};
    auto settle=[&](const Operation &op,EffectState effect,bool expected){bridge.settle(op,effect,[&](bool got){if(!emit)CHECK_EQ(got,expected);});};
    auto lookup=[&](const std::string &id,LookupState expected,const Operation &op){bridge.lookupOperation(id,[&](LookupState got,Operation found){if(!emit){CHECK(got==expected);if(got==LookupState::Found)CHECK(found==op);}});};
    TEST_CASE("Journal bridge cross-language: immutable intent, full binding, terminal replay, missing and conflicts") {
        intent(a,IntentState::Ready);lookup(a.id,LookupState::Found,a);settle(a,EffectState::Committed,true);intent(a,IntentState::Committed);
        lookup(usapptest::deletionjournalunit::id(999),LookupState::Missing,{});settle(a,EffectState::ProvenNotApplied,false);intent(changed,IntentState::Conflict);
        intent(cleanup,IntentState::Ready);settle(cleanup,EffectState::ProvenNotApplied,true);intent(cleanup,IntentState::Aborted);lookup(cleanup.id,LookupState::Found,cleanup);settle(cleanup,EffectState::Committed,false);
        CHECK_EQ(index,12u);
    }
    if(emit){std::ofstream out(argv[2]);out<<u::compact(transcript)<<"\n";if(!out)return 2;}
    return usapptest::finish();
}
