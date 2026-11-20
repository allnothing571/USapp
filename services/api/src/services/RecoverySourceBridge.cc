#include "services/RecoverySourceBridge.h"
#include "core/Crypto.h"
#include <json/json.h>
#include <openssl/hmac.h>
#include <atomic>
#include <set>
#include <stdexcept>

namespace usapp::deletionrecovery::bridge {
namespace {
constexpr size_t maxItems=2048;
bool hex(const std::string &v,size_t length){return v.size()==length&&std::all_of(v.begin(),v.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool keys(const Json::Value &v,const std::set<std::string> &expected){if(!v.isObject())return false;const auto names=v.getMemberNames();return std::set<std::string>(names.begin(),names.end())==expected;}
bool integer(const Json::Value &v){return(v.type()==Json::intValue||v.type()==Json::uintValue)&&v.isUInt64()&&v.asUInt64()<=static_cast<uint64_t>(std::numeric_limits<int64_t>::max());}
bool text(const Json::Value &v,const std::string &expected){return v.isString()&&v.asString()==expected;}
std::string compact(const Json::Value &v){Json::StreamWriterBuilder w;w["indentation"]="";return Json::writeString(w,v);}
bool parse(const std::string &body,Json::Value &v){if(body.find('\0')!=std::string::npos)return false;Json::CharReaderBuilder b;b["rejectDupKeys"]=true;b["strictRoot"]=true;b["failIfExtra"]=true;b["allowComments"]=false;b["allowTrailingCommas"]=false;b["skipBom"]=false;b["stackLimit"]=12;auto reader=std::unique_ptr<Json::CharReader>(b.newCharReader());std::string error;return reader->parse(body.data(),body.data()+body.size(),&v,&error);}
bool checkpoint(const Json::Value &v,const std::string &id,Head &head){
    if(!keys(v,{"version","journal_id","batch_seq","batch_head","event_seq","event_head"})||!integer(v["version"])||v["version"].asUInt64()!=2||!text(v["journal_id"],id)||!integer(v["batch_seq"])||!integer(v["event_seq"])||
       !v["batch_head"].isString()||!v["event_head"].isString()||!hex(v["batch_head"].asString(),64)||!hex(v["event_head"].asString(),64))return false;
    const auto batch=v["batch_seq"].asUInt64(),event=v["event_seq"].asUInt64();const auto batchHash=v["batch_head"].asString(),eventHash=v["event_head"].asString();const std::string zero(64,'0');
    if(batch>1024||event<batch||(batch==0?(event!=0||batchHash!=zero||eventHash!=zero):(batchHash==zero||eventHash==zero)))return false;
    head={id,batch,batchHash};return validHead(head);
}
bool targets(const Json::Value &v,std::vector<Target> &out){
    if(!v.isArray()||v.size()>maxItems)return false;
    for(const auto &item:v){if(!keys(item,{"kind","id"})||!item["kind"].isString()||!item["id"].isString()||!deletionjournal::uuidShape(item["id"].asString()))return false;const auto kind=item["kind"].asString();if(kind!="memory"&&kind!="media")return false;out.push_back({kind=="memory"?Kind::Memory:Kind::Media,item["id"].asString()});}return true;
}
bool snapshotValue(const Json::Value &v,const Head &head,Snapshot &out){
    if(!keys(v,{"head","hidden","purge_barriers","unresolved"})||!keys(v["head"],{"journal_id","seq","hash"})||!text(v["head"]["journal_id"],head.journalId)||!integer(v["head"]["seq"])||v["head"]["seq"].asUInt64()!=head.seq||!text(v["head"]["hash"],head.hash)||
       !targets(v["hidden"],out.hidden)||!targets(v["purge_barriers"],out.purgeBarriers)||!v["unresolved"].isArray()||v["unresolved"].size()>maxItems||out.hidden.size()+out.purgeBarriers.size()+v["unresolved"].size()>maxItems)return false;
    for(const auto &id:v["unresolved"]){if(!id.isString())return false;out.unresolved.push_back(id.asString());}
    out.head=head;auto normalized=out;
    return normalize(normalized)&&normalized.hidden==out.hidden&&normalized.purgeBarriers==out.purgeBarriers&&normalized.unresolved==out.unresolved;
}
}
std::string mac(const std::string &key,const std::string &direction,const std::string &body){
    if(direction!="request"&&direction!="response")throw std::invalid_argument("invalid_recovery_direction");
    const auto input="usapp-journal-recovery-v1:"+direction+"\n"+body;unsigned char digest[EVP_MAX_MD_SIZE];unsigned int length=0;
    if(!HMAC(EVP_sha256(),key.data(),static_cast<int>(key.size()),reinterpret_cast<const unsigned char *>(input.data()),input.size(),digest,&length)||length!=32)throw std::runtime_error("recovery_mac_failed");
    static const char *chars="0123456789abcdef";std::string out;out.reserve(64);for(unsigned i=0;i<length;++i){out.push_back(chars[digest[i]>>4]);out.push_back(chars[digest[i]&15]);}return out;
}
WireSource::WireSource(Context context,std::string key,Exchange exchange,Nonce nonce):context_(std::move(context)),key_(std::move(key)),exchange_(std::move(exchange)),nonce_(std::move(nonce)){
    if(!deletionjournal::uuidShape(context_.journalId)||!deletionjournal::uuidShape(context_.databaseId)||!deletionjournal::uuidShape(context_.attemptId)||!hex(key_,64)||!exchange_)throw std::invalid_argument("invalid_recovery_source_configuration");
    if(!nonce_)nonce_=[](){return crypto::randomHex(16);};
}
bool WireSource::binds(const Request &request)const noexcept{return request.journalId==context_.journalId&&request.databaseId==context_.databaseId&&request.attemptId==context_.attemptId;}
void WireSource::call(const std::string &id,const std::string &method,std::function<void(bool,Snapshot)> done){
    auto delivered=std::make_shared<std::atomic<bool>>(false);
    auto finish=[delivered,done=std::move(done)](bool trusted,Snapshot data={}){if(!delivered->exchange(true))done(trusted,std::move(data));};
    try{
        if(id!=context_.journalId){finish(false);return;}
        const auto nonce=nonce_();if(!hex(nonce,32)){finish(false);return;}
        Json::Value value(Json::objectValue);value["version"]=1;value["journal_id"]=context_.journalId;value["database_id"]=context_.databaseId;value["attempt_id"]=context_.attemptId;value["nonce"]=nonce;value["method"]=method;
        const auto body=compact(value),digest=crypto::sha256Hex(body),key=key_;const auto context=context_;
        exchange_({body,mac(key,"request",body)},[finish,digest,nonce,key,context,method](deletionjournal::bridge::Reply reply){
            try{
                Json::Value value;Head head;Snapshot result;
                if(reply.status!=200||reply.body.size()>deletionjournal::bridge::kMaxWireBytes||!hex(reply.signature,64)||!crypto::constantTimeEquals(reply.signature,mac(key,"response",reply.body))||!parse(reply.body,value)||
                   !keys(value,{"version","journal_id","database_id","attempt_id","nonce","request_sha256","state","checkpoint","snapshot","serving_allowed"})||!integer(value["version"])||value["version"].asUInt64()!=1||
                   !text(value["journal_id"],context.journalId)||!text(value["database_id"],context.databaseId)||!text(value["attempt_id"],context.attemptId)||!text(value["nonce"],nonce)||!text(value["request_sha256"],digest)||
                   !value["serving_allowed"].isBool()||value["serving_allowed"].asBool()||!text(value["state"],"verified")||!checkpoint(value["checkpoint"],context.journalId,head)){finish(false);return;}
                if(method=="snapshot"){
                    if(!snapshotValue(value["snapshot"],head,result)){finish(false);return;}
                }else{
                    if(!value["snapshot"].isNull()){finish(false);return;}result.head=head;
                }
                finish(true,std::move(result));
            }catch(...){finish(false);}
        });
    }catch(...){finish(false);}
}
void WireSource::snapshot(const std::string &id,std::function<void(bool,Snapshot)> done){call(id,"snapshot",std::move(done));}
void WireSource::currentHead(const std::string &id,std::function<void(bool,Head)> done){call(id,"current_head",[done=std::move(done)](bool trusted,Snapshot value){done(trusted,std::move(value.head));});}
} // namespace usapp::deletionrecovery::bridge
