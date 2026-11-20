#include "services/DeletionJournalBridge.h"
#include "core/Crypto.h"
#include <json/json.h>
#include <openssl/hmac.h>
#include <atomic>
#include <set>
#include <stdexcept>

namespace usapp::deletionjournal::bridge {
namespace {
bool hex(const std::string &v,size_t size) {
    return v.size()==size&&std::all_of(v.begin(),v.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool keys(const Json::Value &v,const std::set<std::string> &expected) {
    if(!v.isObject())return false;
    const auto names=v.getMemberNames();return std::set<std::string>(names.begin(),names.end())==expected;
}
std::string compact(const Json::Value &v){Json::StreamWriterBuilder w;w["indentation"]="";return Json::writeString(w,v);}
const char *actionName(Action a){switch(a){case Action::Delete:return "delete";case Action::Undo:return "undo";case Action::Purge:return "purge";case Action::Cleanup:return "cleanup";}return "invalid";}
Json::Value encode(const Operation &op) {
    Json::Value v(Json::objectValue);v["operation_id"]=op.id;v["action"]=actionName(op.action);v["request_fingerprint"]=op.requestFingerprint;
    v["subjects"]=Json::Value(Json::arrayValue);
    for(const auto &s:op.subjects){Json::Value row(Json::objectValue);row["target_kind"]=s.kind==Kind::Memory?"memory":"media";row["target_id"]=s.targetId;row["cycle_id"]=s.cycleId;row["intent_id"]=s.intentId;v["subjects"].append(row);}
    return v;
}
bool decode(const Json::Value &v,Operation &op) {
    if(!keys(v,{"operation_id","action","request_fingerprint","subjects"})||!v["operation_id"].isString()||!v["action"].isString()||!v["request_fingerprint"].isString()||!v["subjects"].isArray()||v["subjects"].empty()||v["subjects"].size()>kMaxSubjects)return false;
    op.id=v["operation_id"].asString();op.requestFingerprint=v["request_fingerprint"].asString();const auto a=v["action"].asString();
    if(a=="delete")op.action=Action::Delete;else if(a=="undo")op.action=Action::Undo;else if(a=="purge")op.action=Action::Purge;else if(a=="cleanup")op.action=Action::Cleanup;else return false;
    for(const auto &row:v["subjects"]){
        if(!keys(row,{"target_kind","target_id","cycle_id","intent_id"}))return false;
        for(const auto *field:{"target_kind","target_id","cycle_id","intent_id"})if(!row[field].isString())return false;
        const auto kind=row["target_kind"].asString();if(kind!="memory"&&kind!="media")return false;
        op.subjects.push_back({kind=="memory"?Kind::Memory:Kind::Media,row["target_id"].asString(),row["cycle_id"].asString(),row["intent_id"].asString()});
    }
    auto sorted=op;return normalize(sorted)&&sorted==op;
}
bool parse(const std::string &body,Json::Value &value){
    if(body.find('\0')!=std::string::npos)return false; // JsonCpp may otherwise treat an embedded NUL as end-of-input.
    Json::CharReaderBuilder b;b["rejectDupKeys"]=true;b["strictRoot"]=true;b["failIfExtra"]=true;b["stackLimit"]=12;
    auto r=std::unique_ptr<Json::CharReader>(b.newCharReader());std::string errors;
    return r->parse(body.data(),body.data()+body.size(),&value,&errors);
}
}
std::string mac(const std::string &key,const std::string &direction,const std::string &body) {
    if(direction!="request"&&direction!="response")throw std::invalid_argument("invalid_bridge_direction");
    const auto input="usapp-journal-bridge-v1:"+direction+"\n"+body;
    unsigned char digest[EVP_MAX_MD_SIZE];unsigned int length=0;
    if(!HMAC(EVP_sha256(),key.data(),static_cast<int>(key.size()),reinterpret_cast<const unsigned char *>(input.data()),input.size(),digest,&length)||length!=32)throw std::runtime_error("bridge_mac_failed");
    static const char *chars="0123456789abcdef";std::string out;out.reserve(64);
    for(unsigned i=0;i<length;++i){out.push_back(chars[digest[i]>>4]);out.push_back(chars[digest[i]&15]);}return out;
}
WireJournal::WireJournal(std::string id,std::string key,Exchange exchange,Nonce nonce):journalId_(std::move(id)),key_(std::move(key)),exchange_(std::move(exchange)),nonce_(std::move(nonce)) {
    if(!uuidShape(journalId_)||!hex(key_,64)||!exchange_)throw std::invalid_argument("invalid_bridge_configuration");
    if(!nonce_)nonce_=[](){return crypto::randomHex(16);};
}
void WireJournal::call(const std::string &method,const Operation *input,const std::string &id,const std::string &phase,std::function<void(std::string,Operation)> done) {
    auto delivered=std::make_shared<std::atomic<bool>>(false);
    auto finish=[delivered,done=std::move(done)](std::string state,Operation op={}){if(!delivered->exchange(true))done(std::move(state),std::move(op));};
    try {
        if(!uuidShape(id)){finish("unknown");return;}
        Operation op;if(input){op=*input;if(!normalize(op)||op.subjects.size()>kMaxSubjects){finish("unknown");return;}}
        const auto nonce=nonce_();if(!hex(nonce,32)){finish("unknown");return;}
        Json::Value v(Json::objectValue);v["version"]=1;v["journal_id"]=journalId_;v["nonce"]=nonce;v["method"]=method;v["operation_id"]=id;
        v["operation"]=input?encode(op):Json::Value(Json::nullValue);v["phase"]=phase.empty()?Json::Value(Json::nullValue):Json::Value(phase);
        const auto body=compact(v);if(body.size()>kMaxWireBytes){finish("unknown");return;}
        const auto digest=crypto::sha256Hex(body),journalId=journalId_,key=key_;
        exchange_({body,mac(key_,"request",body)},[finish,nonce,digest,journalId,key,method,id](Reply reply){
            try {
                Json::Value value;Operation found;
                if(reply.status!=200||reply.body.size()>kMaxWireBytes||!hex(reply.signature,64)||!crypto::constantTimeEquals(reply.signature,mac(key,"response",reply.body))||!parse(reply.body,value)||
                   !keys(value,{"version","journal_id","nonce","request_sha256","state","operation"})||(value["version"].type()!=Json::intValue&&value["version"].type()!=Json::uintValue)||!value["version"].isUInt64()||value["version"].asUInt64()!=1||
                   !value["journal_id"].isString()||value["journal_id"].asString()!=journalId||!value["nonce"].isString()||value["nonce"].asString()!=nonce||
                   !value["request_sha256"].isString()||value["request_sha256"].asString()!=digest||!value["state"].isString()){finish("unknown");return;}
                const auto state=value["state"].asString();
                if(method=="lookup"){
                    if(state=="found"&&decode(value["operation"],found)&&found.id==id){finish(state,std::move(found));return;}
                    if((state=="missing"||state=="unknown")&&value["operation"].isNull()){finish(state);return;}
                } else if(value["operation"].isNull()&&(state=="ready"||state=="committed"||state=="aborted"||state=="conflict"||state=="unknown"||state=="unavailable"||state=="invalid")){finish(state);return;}
                finish("unknown");
            } catch (...) {finish("unknown");}
        });
    } catch (...) {finish("unknown");}
}
void WireJournal::ensureIntent(const Operation &op,std::function<void(IntentState)> done) {
    call("ensure_intent",&op,op.id,"intent",[done](std::string s,Operation){
        done(s=="ready"?IntentState::Ready:s=="committed"?IntentState::Committed:s=="aborted"?IntentState::Aborted:s=="conflict"?IntentState::Conflict:IntentState::Unknown);
    });
}
void WireJournal::settle(const Operation &op,EffectState state,std::function<void(bool)> done) {
    if(state!=EffectState::Committed&&state!=EffectState::ProvenNotApplied){done(false);return;}
    const std::string phase=state==EffectState::Committed?"committed":"aborted";
    call("settle",&op,op.id,phase,[done,phase](std::string s,Operation){done(s==phase);});
}
void WireJournal::lookupOperation(const std::string &id,std::function<void(LookupState,Operation)> done) {
    call("lookup",nullptr,id,"",[done](std::string s,Operation op){done(s=="found"?LookupState::Found:s=="missing"?LookupState::Missing:LookupState::Unknown,std::move(op));});
}
} // namespace usapp::deletionjournal::bridge
