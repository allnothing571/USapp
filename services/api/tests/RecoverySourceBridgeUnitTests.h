#pragma once
#include "DeletionRecoveryUnitTests.h"
#include "DeletionJournalBridgeUnitTests.h"
#include "services/RecoverySourceBridge.h"

namespace usapptest::recoverysourceunit {
namespace r=usapp::deletionrecovery;
namespace b=r::bridge;
namespace j=usapp::deletionjournal::bridge;
namespace old=usapptest::deletionrecoveryunit;
using usapptest::journalbridgeunit::json;
using usapptest::journalbridgeunit::compact;
inline const std::string key(64,'d');
inline b::Context context(){return {old::id(1),old::id(2),old::id(3)};}
inline Json::Value payload(const j::Request &request,bool zero=false){
    const auto sent=json(request.body);Json::Value value(Json::objectValue);
    for(const auto *name:{"journal_id","database_id","attempt_id","nonce"})value[name]=sent[name];
    value["version"]=1;value["request_sha256"]=usapp::crypto::sha256Hex(request.body);value["state"]="verified";value["serving_allowed"]=false;
    auto &cp=value["checkpoint"];cp["version"]=2;cp["journal_id"]=sent["journal_id"];cp["batch_seq"]=zero?0:8;cp["batch_head"]=std::string(64,zero?'0':'a');cp["event_seq"]=zero?0:12;cp["event_head"]=std::string(64,zero?'0':'b');
    value["snapshot"]=Json::nullValue;
    if(sent["method"]=="snapshot"){
        auto &snapshot=value["snapshot"];snapshot["head"]["journal_id"]=sent["journal_id"];snapshot["head"]["seq"]=cp["batch_seq"];snapshot["head"]["hash"]=cp["batch_head"];
        snapshot["hidden"]=Json::arrayValue;snapshot["purge_barriers"]=Json::arrayValue;snapshot["unresolved"]=Json::arrayValue;
        if(!zero){
            for(auto entry:{std::make_pair("memory",10u),std::make_pair("media",11u)}){Json::Value t;t["kind"]=entry.first;t["id"]=old::id(entry.second);snapshot["hidden"].append(t);}
            Json::Value barrier;barrier["kind"]="media";barrier["id"]=old::id(12);snapshot["purge_barriers"].append(barrier);snapshot["unresolved"].append(old::id(30));
        }
    }
    return value;
}
inline j::Reply response(const Json::Value &value){const auto raw=compact(value);return {200,raw,b::mac(key,"response",raw)};}
inline void run(){
    TEST_CASE("Recovery source: authenticated context, batch checkpoint and complete canonical projection"){
        for(bool zero:{false,true}){
            int callbacks=0,calls=0;
            b::WireSource source(context(),key,[&](j::Request request,auto done){++calls;CHECK_EQ(request.signature,b::mac(key,"request",request.body));auto v=json(request.body);CHECK_EQ(v["database_id"].asString(),context().databaseId);CHECK_EQ(v["attempt_id"].asString(),context().attemptId);CHECK_EQ(v.getMemberNames().size(),6u);done(response(payload(request,zero)));});
            source.snapshot(context().journalId,[&](bool trusted,r::Snapshot value){++callbacks;CHECK(trusted);CHECK_EQ(value.head.seq,zero?0u:8u);CHECK_EQ(value.head.hash,std::string(64,zero?'0':'a'));CHECK_EQ(value.hidden.size(),zero?0u:2u);CHECK_EQ(value.purgeBarriers.size(),zero?0u:1u);CHECK_EQ(value.unresolved.size(),zero?0u:1u);});
            source.currentHead(context().journalId,[&](bool trusted,r::Head value){++callbacks;CHECK(trusted);CHECK_EQ(value.seq,zero?0u:8u);});CHECK_EQ(callbacks,2);CHECK_EQ(calls,2);
        }
    }
    TEST_CASE("Recovery source: forged, cross-context, malformed and contradictory frames always fail closed"){
        for(int fault=0;fault<39;++fault){
            int callbacks=0;
            b::WireSource source(context(),key,[&](j::Request request,auto done){
                auto value=payload(request);auto &cp=value["checkpoint"];auto &snap=value["snapshot"];
                switch(fault){
                    case 0:value["journal_id"]=old::id(99);break;case 1:value["database_id"]=old::id(99);break;case 2:value["attempt_id"]=old::id(99);break;
                    case 3:value["nonce"]=std::string(32,'f');break;case 4:value["request_sha256"]=std::string(64,'0');break;
                    case 5:value["version"]=1.0;break;case 6:value["serving_allowed"]=true;break;case 7:value["serving_allowed"]=0;break;
                    case 8:value["state"]="missing";break;case 9:value["extra"]=true;break;case 10:cp["version"]=true;break;
                    case 11:cp["journal_id"]=old::id(99);break;case 12:cp["batch_seq"]=-1;break;case 13:cp["batch_seq"]=1025;break;
                    case 14:cp["event_seq"]=7;break;case 15:cp["event_seq"]=Json::UInt64(std::numeric_limits<uint64_t>::max());break;
                    case 16:cp["batch_head"]=std::string(64,'0');break;case 17:cp["event_head"]=std::string(64,'A');break;
                    case 18:snap["head"]["seq"]=12;break;case 19:snap["head"]["hash"]=std::string(64,'b');break;
                    case 20:snap["hidden"].append(snap["hidden"][0]);break;case 21:std::swap(snap["hidden"][0],snap["hidden"][1]);break;
                    case 22:snap["hidden"][0]["kind"]="album";break;case 23:snap["purge_barriers"][0]["id"]="bad";break;
                    case 24:snap["unresolved"].append(snap["unresolved"][0]);break;case 25:snap["unresolved"][0]=false;break;
                    case 26:snap["hidden"]=Json::nullValue;break;case 27:value["state"]="unknown";value["checkpoint"]=Json::nullValue;value["snapshot"]=Json::nullValue;break;
                    default:break;
                }
                auto reply=response(value);
                if(fault==28)reply.signature=std::string(64,'0');
                if(fault==29)reply.signature=j::mac(key,"response",reply.body);
                if(fault==30)reply.signature=b::mac(key,"request",reply.body);
                if(fault==31)reply.status=503;
                if(fault==32)reply.body=std::string(65537,'x');
                if(fault==33){reply.body="{\"version\":1,"+reply.body.substr(1);reply.signature=b::mac(key,"response",reply.body);}
                if(fault==34){reply.body+=" {}";reply.signature=b::mac(key,"response",reply.body);}
                if(fault==35){reply.body="/* no */"+reply.body;reply.signature=b::mac(key,"response",reply.body);}
                if(fault==36){reply.body="\xef\xbb\xbf"+reply.body;reply.signature=b::mac(key,"response",reply.body);}
                if(fault==37){reply.body=reply.body.substr(0,reply.body.size()-1)+",}";reply.signature=b::mac(key,"response",reply.body);}
                if(fault==38){reply.body.push_back('\0');reply.body+="{}";reply.signature=b::mac(key,"response",reply.body);}
                done(reply);
            });
            source.snapshot(context().journalId,[&](bool trusted,r::Snapshot value){++callbacks;CHECK_FALSE(trusted);CHECK(value.hidden.empty());CHECK(value.head.journalId.empty());});CHECK_EQ(callbacks,1);
        }
    }
    TEST_CASE("Recovery source: current-head replies cannot carry projections or inconsistent zero checkpoints"){
        for(int fault=0;fault<6;++fault){
            b::WireSource source(context(),key,[&](j::Request request,auto done){auto value=payload(request,true);switch(fault){case 0:value["snapshot"]=Json::objectValue;break;case 1:value["checkpoint"]["event_seq"]=1;break;case 2:value["checkpoint"]["event_head"]=std::string(64,'a');break;case 3:value["checkpoint"]["batch_head"]=std::string(64,'a');break;case 4:value["checkpoint"]["event_seq"]=0.0;break;case 5:value["checkpoint"]["extra"]=true;break;}done(response(value));});
            source.currentHead(context().journalId,[](bool trusted,r::Head head){CHECK_FALSE(trusted);CHECK(head.journalId.empty());});
        }
    }
    TEST_CASE("Recovery source: invalid context, foreign journal and bad nonce never dispatch"){
        int calls=0;auto exchange=[&](j::Request,auto){++calls;};
        bool threw=false;try{b::WireSource source({"bad",old::id(2),old::id(3)},key,exchange);}catch(const std::invalid_argument &){threw=true;}CHECK(threw);
        b::WireSource source(context(),key,exchange);source.snapshot(old::id(99),[](bool trusted,r::Snapshot){CHECK_FALSE(trusted);});
        b::WireSource bad(context(),key,exchange,[]{return "bad";});bad.currentHead(context().journalId,[](bool trusted,r::Head){CHECK_FALSE(trusted);});CHECK_EQ(calls,0);
        CHECK(source.binds({context().databaseId,context().attemptId,context().journalId}));CHECK_FALSE(source.binds({old::id(99),context().attemptId,context().journalId}));
    }
    TEST_CASE("Recovery source: duplicate and late replies, exchange throws and destroyed owner deliver only once"){
        std::function<void(j::Reply)> waiting; j::Request sent;int answers=0;
        {b::WireSource source(context(),key,[&](j::Request request,auto done){sent=std::move(request);waiting=done;});source.snapshot(context().journalId,[&](bool trusted,r::Snapshot){++answers;CHECK(trusted);});}
        waiting(response(payload(sent)));waiting({503,"",""});CHECK_EQ(answers,1);
        b::WireSource source(context(),key,[](j::Request request,auto done){done(response(payload(request)));throw std::runtime_error("fixed");});
        source.currentHead(context().journalId,[&](bool trusted,r::Head){++answers;CHECK(trusted);});CHECK_EQ(answers,2);
        b::WireSource failed(context(),key,[](j::Request,auto){throw std::runtime_error("fixed");});failed.snapshot(context().journalId,[&](bool trusted,r::Snapshot){++answers;CHECK_FALSE(trusted);});CHECK_EQ(answers,3);
    }
    TEST_CASE("Recovery source: fresh default nonces reject replay across otherwise identical calls"){
        j::Reply previous;std::string nonce;int calls=0,answers=0;
        b::WireSource source(context(),key,[&](j::Request request,auto done){++calls;const auto current=json(request.body)["nonce"].asString();CHECK_EQ(current.size(),32u);if(calls==1){nonce=current;previous=response(payload(request));}else CHECK_FALSE(current==nonce);done(previous);});
        source.snapshot(context().journalId,[&](bool trusted,r::Snapshot){++answers;CHECK(trusted);});
        source.snapshot(context().journalId,[&](bool trusted,r::Snapshot){++answers;CHECK_FALSE(trusted);});CHECK_EQ(answers,2);
    }
    TEST_CASE("Recovery source: coordinator closes before rejecting restore-context mismatch and never records moving heads"){
        for(int mode=0;mode<3;++mode){
            std::vector<std::string> trace;auto store=std::make_shared<old::Store>(trace);int calls=0,answers=0;r::Outcome outcome=r::Outcome::Unknown;
            auto source=std::make_shared<b::WireSource>(context(),key,[&](j::Request request,auto done){++calls;auto value=payload(request);if(mode==1&&json(request.body)["method"]=="current_head")value["checkpoint"]["batch_head"]=std::string(64,'c');done(response(value));});
            r::Request request{context().databaseId,context().attemptId,context().journalId};if(mode==0)request.attemptId=old::id(99);
            r::Coordinator::run(request,source,store,[&](r::Report report){++answers;outcome=report.outcome;CHECK_FALSE(report.servingAllowed);});
            CHECK_EQ(answers,1);CHECK(store->closed);
            if(mode==0){CHECK(outcome==r::Outcome::Blocked);CHECK_EQ(calls,0);CHECK(trace==std::vector<std::string>({"close"}));CHECK_FALSE(store->verified.has_value());}
            if(mode==1){CHECK(outcome==r::Outcome::HeadChanged);CHECK_FALSE(store->verified.has_value());CHECK_EQ(calls,2);}
            if(mode==2){CHECK(outcome==r::Outcome::VerifiedClosed);CHECK(store->verified.has_value());CHECK_EQ(calls,2);CHECK_EQ(store->staged.head.seq,8u);}
        }
    }
    TEST_CASE("Recovery source: coordinator rechecks a revoked pin before final head and keeps database closed"){
        struct Revoked : old::Source {mutable int checks=0;explicit Revoked(std::vector<std::string> &trace):old::Source(trace){}bool binds(const r::Request &)const noexcept override{return ++checks==1;}};
        std::vector<std::string> trace;auto source=std::make_shared<Revoked>(trace);auto store=std::make_shared<old::Store>(trace);int answers=0;
        r::Coordinator::run({old::id(2),old::id(3),old::id(1)},source,store,[&](r::Report report){++answers;CHECK(report.outcome==r::Outcome::Blocked);CHECK_FALSE(report.servingAllowed);});
        CHECK_EQ(answers,1);CHECK_EQ(source->checks,2);CHECK_EQ(source->heads,0);CHECK(store->closed);CHECK_FALSE(store->verified.has_value());
    }
}
} // namespace usapptest::recoverysourceunit
