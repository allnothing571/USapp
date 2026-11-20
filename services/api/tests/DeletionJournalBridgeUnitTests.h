#pragma once
#include "DeletionJournalUnitTests.h"
#include "services/DeletionJournalBridge.h"
#include "core/Crypto.h"
#include <json/json.h>

namespace usapptest::journalbridgeunit {
namespace b=usapp::deletionjournal::bridge;
using namespace usapp::deletionjournal;
inline Json::Value json(const std::string &raw){Json::Value v;Json::CharReaderBuilder r;std::string e;auto reader=std::unique_ptr<Json::CharReader>(r.newCharReader());if(!reader->parse(raw.data(),raw.data()+raw.size(),&v,&e))throw std::runtime_error("fixture_json");return v;}
inline std::string compact(const Json::Value &v){Json::StreamWriterBuilder w;w["indentation"]="";return Json::writeString(w,v);}
inline const std::string key(64,'b');
inline const std::string journal=deletionjournalunit::id(44);
inline b::Reply response(const b::Request &request,const std::string &state,Json::Value op=Json::Value(Json::nullValue)) {
    const auto sent=json(request.body);Json::Value v(Json::objectValue);
    v["version"]=1;v["journal_id"]=sent["journal_id"];v["nonce"]=sent["nonce"];v["request_sha256"]=usapp::crypto::sha256Hex(request.body);v["state"]=state;v["operation"]=std::move(op);
    const auto raw=compact(v);return {200,raw,b::mac(key,"response",raw)};
}
inline void run() {
    const auto op=deletionjournalunit::sample();
    TEST_CASE("Journal bridge: authenticated pinned replies map intent states without inventing absence or abort") {
        for(const auto &entry:std::vector<std::pair<std::string,IntentState>>{{"ready",IntentState::Ready},{"committed",IntentState::Committed},{"aborted",IntentState::Aborted},{"conflict",IntentState::Conflict},{"unknown",IntentState::Unknown},{"unavailable",IntentState::Unknown},{"invalid",IntentState::Unknown},{"missing",IntentState::Unknown},{"garbage",IntentState::Unknown}}){
            int callbacks=0;
            b::WireJournal bridge(journal,key,[&](b::Request req,auto done){CHECK_EQ(req.signature,b::mac(key,"request",req.body));done(response(req,entry.first));});
            bridge.ensureIntent(op,[&](IntentState got){callbacks++;CHECK(got==entry.second);});CHECK_EQ(callbacks,1);
        }
    }
    TEST_CASE("Journal bridge: forged, misbound, oversized, duplicate and contradictory response fields remain unknown") {
        for(int fault=0;fault<13;fault++){
            b::WireJournal bridge(journal,key,[&](b::Request req,auto done){
                auto reply=response(req,"ready");auto v=json(reply.body);
                if(fault==0)v["nonce"]=std::string(32,'0');
                if(fault==1)v["journal_id"]=deletionjournalunit::id(45);
                if(fault==2)v["request_sha256"]=std::string(64,'0');
                if(fault==3)v["operation"]=json(req.body)["operation"];
                if(fault==4)v["version"]=1.0;
                if(fault==5)v["private"]=true;
                reply.body=compact(v);reply.signature=b::mac(key,"response",reply.body);
                if(fault==6)reply.signature=std::string(64,'0');
                if(fault==7)reply.signature=b::mac(key,"request",reply.body);
                if(fault==8)reply.status=503;
                if(fault==9){reply.body=std::string(b::kMaxWireBytes+1,'x');reply.signature=b::mac(key,"response",reply.body);}
                if(fault==10){reply.body.insert(1,"\"version\":1,");reply.signature=b::mac(key,"response",reply.body);}
                if(fault==11){reply.body+=" {}";reply.signature=b::mac(key,"response",reply.body);}
                if(fault==12){reply.body.push_back('\0');reply.body+="{}";reply.signature=b::mac(key,"response",reply.body);}
                done(reply);
            });
            int callbacks=0;bridge.ensureIntent(op,[&](IntentState got){callbacks++;CHECK(got==IntentState::Unknown);});CHECK_EQ(callbacks,1);
        }
    }
    TEST_CASE("Journal bridge: matching terminal only, unknown effect never sent, callback or transport faults never double-answer") {
        int exchanges=0;
        b::WireJournal bridge(journal,key,[&](b::Request req,auto done){exchanges++;done(response(req,"aborted"));done(response(req,"committed"));throw std::runtime_error("private_fault");});
        int callbacks=0;bridge.settle(op,EffectState::ProvenNotApplied,[&](bool yes){callbacks++;CHECK(yes);});CHECK_EQ(callbacks,1);
        bridge.settle(op,EffectState::Committed,[&](bool yes){CHECK_FALSE(yes);});
        bridge.settle(op,EffectState::Unknown,[&](bool yes){CHECK_FALSE(yes);});CHECK_EQ(exchanges,2);
        b::WireJournal throwing(journal,key,[](b::Request,auto){throw std::runtime_error("private_fault");});
        throwing.ensureIntent(op,[](IntentState got){CHECK(got==IntentState::Unknown);});
        int answered=0;
        b::WireJournal unknownFirst(journal,key,[](b::Request req,auto done){done({});done(response(req,"ready"));});
        unknownFirst.ensureIntent(op,[&](IntentState got){answered++;CHECK(got==IntentState::Unknown);});CHECK_EQ(answered,1);
    }
    TEST_CASE("Journal bridge: found is a complete normalized same-operation binding; missing needs a valid signed read") {
        for(int mode=0;mode<7;mode++){
            b::WireJournal bridge(journal,key,[&](b::Request req,auto done){
                b::Request temp=req;auto v=json(temp.body);v["operation"]=Json::Value(Json::objectValue);
                v["operation"]["operation_id"]=op.id;v["operation"]["action"]="delete";v["operation"]["request_fingerprint"]=op.requestFingerprint;
                Json::Value row(Json::objectValue);row["target_kind"]="memory";row["target_id"]=op.subjects[0].targetId;row["cycle_id"]=op.subjects[0].cycleId;row["intent_id"]=op.subjects[0].intentId;v["operation"]["subjects"].append(row);
                auto binding=v["operation"];
                if(mode==2)binding["operation_id"]=deletionjournalunit::id(999);
                if(mode==3)binding["subjects"][0]["target_id"]="invalid";
                if(mode==4)binding["subjects"].append(row);
                auto reply=response(req,mode==1||mode==5?"missing":"found",mode==1?Json::Value(Json::nullValue):binding);
                if(mode==6)reply.signature.clear();done(reply);
            });
            int calls=0;bridge.lookupOperation(op.id,[&](LookupState got,Operation found){calls++;CHECK(got==(mode==0?LookupState::Found:mode==1?LookupState::Missing:LookupState::Unknown));if(got==LookupState::Found)CHECK(found==op);});CHECK_EQ(calls,1);
        }
    }
    TEST_CASE("Journal bridge: invalid local inputs and nonce cannot invoke the exchange") {
        int calls=0;
        b::WireJournal bridge(journal,key,[&](b::Request,auto){calls++;});
        auto invalid=op;invalid.subjects.push_back(invalid.subjects[0]);bridge.ensureIntent(invalid,[](IntentState got){CHECK(got==IntentState::Unknown);});
        bridge.lookupOperation("invalid",[](LookupState got,Operation){CHECK(got==LookupState::Unknown);});
        b::WireJournal badNonce(journal,key,[&](b::Request,auto){calls++;},[]{return "invalid";});badNonce.ensureIntent(op,[](IntentState got){CHECK(got==IntentState::Unknown);});
        CHECK_EQ(calls,0);
        bool refused=false;try{b::WireJournal bad(journal,"bad",[](b::Request,auto){});}catch(const std::invalid_argument &){refused=true;}CHECK(refused);
    }
}
} // namespace usapptest::journalbridgeunit
