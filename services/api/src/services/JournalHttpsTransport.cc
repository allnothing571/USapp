#include "services/JournalHttpsTransport.h"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <pthread.h>
#include <signal.h>

namespace usapp::deletionjournal::bridge {
namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t kMaxHeaders=8192;
bool hexadecimal(const std::string &s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool port(const std::string &s){if(s.empty()||s.size()>5||s[0]=='0')return false;unsigned n=0;for(char c:s){if(c<'0'||c>'9')return false;n=n*10+static_cast<unsigned>(c-'0');}return n>0&&n<=65535;}
bool origin(const std::string &s){
    if(s.compare(0,8,"https://")||s.size()>256)return false;
    auto host=s.substr(8);const auto colon=host.find(':');
    if(colon!=std::string::npos){if(!port(host.substr(colon+1)))return false;host.resize(colon);}
    if(host.empty()||host.size()>253)return false;
    size_t begin=0;
    while(begin<host.size()){
        const auto end=host.find('.',begin);const auto part=host.substr(begin,end==std::string::npos?std::string::npos:end-begin);
        if(part.empty()||part.size()>63||part.front()=='-'||part.back()=='-'||!std::all_of(part.begin(),part.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';}))return false;
        if(end==std::string::npos)return true;begin=end+1;
    }
    return false;
}
struct Call {
    Request request; std::function<void(Reply)> done;Clock::time_point deadline;
    CURL *easy=nullptr;curl_slist *headers=nullptr,*mapping=nullptr;
    std::string body,signature;size_t headerBytes=0;unsigned headerLines=0;
    bool signatureSeen=false,lengthSeen=false,finished=false;
};
struct State {
    HttpsConfig config;std::mutex mutex;std::atomic<bool> stopping{false};
    CURLM *multi=nullptr;unsigned outstanding=0;std::deque<std::shared_ptr<Call>> queued;
    std::map<CURL *,std::shared_ptr<Call>> active;
};
void answer(const std::function<void(Reply)> &done,Reply reply={}){try{if(done)done(std::move(reply));}catch(...){/* No callback exception escapes the I/O thread. */}}
void dispose(const std::shared_ptr<Call> &c){if(c->easy){curl_easy_cleanup(c->easy);c->easy=nullptr;}curl_slist_free_all(c->headers);c->headers=nullptr;curl_slist_free_all(c->mapping);c->mapping=nullptr;}
void finish(const std::shared_ptr<State> &s,const std::shared_ptr<Call> &c,Reply reply={}){
    if(c->finished)return;c->finished=true;dispose(c);
    {std::lock_guard<std::mutex> lock(s->mutex);--s->outstanding;}
    auto done=std::move(c->done);answer(done,std::move(reply));
}
size_t body(char *data,size_t size,size_t count,void *user){
    try {
    auto &c=*static_cast<Call *>(user);
    if(size&&count>std::numeric_limits<size_t>::max()/size)return 0;
    const auto length=size*count;if(length>kMaxWireBytes-c.body.size())return 0;
    c.body.append(data,length);return length;
    } catch (...) { return 0; }
}
size_t header(char *data,size_t size,size_t count,void *user){
    try {
    auto &c=*static_cast<Call *>(user);
    if(size&&count>std::numeric_limits<size_t>::max()/size)return 0;
    const auto length=size*count;if(length>kMaxHeaders-c.headerBytes||++c.headerLines>64)return 0;c.headerBytes+=length;
    const std::string line(data,length);
    if(line.compare(0,5,"HTTP/")==0){c.signatureSeen=c.lengthSeen=false;c.signature.clear();return length;}
    const auto colon=line.find(':');if(colon==std::string::npos)return length;
    auto name=line.substr(0,colon);for(char &v:name)if(v>='A'&&v<='Z')v=static_cast<char>(v-'A'+'a');
    auto value=line.substr(colon+1);const auto a=value.find_first_not_of(" \t\r\n"),b=value.find_last_not_of(" \t\r\n");value=a==std::string::npos?"":value.substr(a,b-a+1);
    if(name=="x-usapp-journal-mac"){if(c.signatureSeen||!hexadecimal(value))return 0;c.signatureSeen=true;c.signature=value;}
    if(name=="content-length"){
        if(c.lengthSeen||value.empty()||value.size()>8)return 0;c.lengthSeen=true;size_t n=0;
        for(char v:value){if(v<'0'||v>'9')return 0;n=n*10+static_cast<size_t>(v-'0');if(n>kMaxWireBytes)return 0;}
    }
    if(name=="content-encoding"&&value!="identity")return 0;
    return length;
    } catch (...) { return 0; }
}
int progress(void *user,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
    const auto *c=static_cast<Call *>(user);return Clock::now()>=c->deadline?1:0;
}
bool start(const std::shared_ptr<State> &s,const std::shared_ptr<Call> &c){
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(c->deadline-Clock::now()).count();if(remaining<=0)return false;
    c->easy=curl_easy_init();if(!c->easy)return false;
    const auto add=[&](const std::string &v){auto *next=curl_slist_append(c->headers,v.c_str());if(!next)return false;c->headers=next;return true;};
    if(!add("Content-Type: application/json")||!add("Accept: application/json")||!add("X-Usapp-Journal-Mac: "+c->request.signature)||!add("Expect:")||!add("Connection: close"))return false;
    CURL *e=c->easy;
#define JOURNAL_SET(option,value) do { if(curl_easy_setopt(e,option,value)!=CURLE_OK)return false; } while(false)
    JOURNAL_SET(CURLOPT_URL,(s->config.origin+"/v1/journal").c_str());
    JOURNAL_SET(CURLOPT_POST,1L);JOURNAL_SET(CURLOPT_POSTFIELDS,c->request.body.data());JOURNAL_SET(CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(c->request.body.size()));
    JOURNAL_SET(CURLOPT_HTTPHEADER,c->headers);JOURNAL_SET(CURLOPT_HTTP_VERSION,static_cast<long>(CURL_HTTP_VERSION_1_1));
    JOURNAL_SET(CURLOPT_PROTOCOLS_STR,"https");JOURNAL_SET(CURLOPT_FOLLOWLOCATION,0L);
    JOURNAL_SET(CURLOPT_PROXY,"");JOURNAL_SET(CURLOPT_NOPROXY,"*");JOURNAL_SET(CURLOPT_NOSIGNAL,1L);
    JOURNAL_SET(CURLOPT_SSL_VERIFYPEER,1L);JOURNAL_SET(CURLOPT_SSL_VERIFYHOST,2L);JOURNAL_SET(CURLOPT_SSLVERSION,static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    if(!s->config.caFile.empty())JOURNAL_SET(CURLOPT_CAINFO,s->config.caFile.c_str());
    if(!s->config.testConnectTo.empty()){c->mapping=curl_slist_append(nullptr,("::"+s->config.testConnectTo).c_str());if(!c->mapping)return false;JOURNAL_SET(CURLOPT_CONNECT_TO,c->mapping);}
    JOURNAL_SET(CURLOPT_TIMEOUT_MS,static_cast<long>(remaining));JOURNAL_SET(CURLOPT_CONNECTTIMEOUT_MS,static_cast<long>(remaining));
    JOURNAL_SET(CURLOPT_FRESH_CONNECT,1L);JOURNAL_SET(CURLOPT_FORBID_REUSE,1L);
    JOURNAL_SET(CURLOPT_HTTP_CONTENT_DECODING,0L);JOURNAL_SET(CURLOPT_ACCEPT_ENCODING,"identity");
    JOURNAL_SET(CURLOPT_WRITEFUNCTION,body);JOURNAL_SET(CURLOPT_WRITEDATA,c.get());JOURNAL_SET(CURLOPT_HEADERFUNCTION,header);JOURNAL_SET(CURLOPT_HEADERDATA,c.get());
    JOURNAL_SET(CURLOPT_NOPROGRESS,0L);JOURNAL_SET(CURLOPT_XFERINFOFUNCTION,progress);JOURNAL_SET(CURLOPT_XFERINFODATA,c.get());

#undef JOURNAL_SET
    if(curl_multi_add_handle(s->multi,e)!=CURLM_OK)return false;s->active.emplace(e,c);return true;
}
void run(const std::shared_ptr<State> &s){
    // NOSIGNAL does not suppress every SIGPIPE corner case. Scope the policy to this owned I/O thread only.
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);
    if(pthread_sigmask(SIG_BLOCK,&blocked,nullptr)!=0)s->stopping=true;
    for(;;){
        std::deque<std::shared_ptr<Call>> waiting;
        {std::lock_guard<std::mutex> lock(s->mutex);waiting.swap(s->queued);}
        for(auto &c:waiting){if(s->stopping||Clock::now()>=c->deadline||!start(s,c))finish(s,c);}
        if(s->stopping){
            auto active=std::move(s->active);s->active.clear();
            for(auto &entry:active){curl_multi_remove_handle(s->multi,entry.first);finish(s,entry.second);}
            break;
        }
        int running=0;const auto performed=curl_multi_perform(s->multi,&running);
        if(performed!=CURLM_OK){s->stopping=true;continue;}
        int left=0;
        while(auto *message=curl_multi_info_read(s->multi,&left)){
            if(message->msg!=CURLMSG_DONE)continue;
            const auto found=s->active.find(message->easy_handle);if(found==s->active.end())continue;
            auto c=found->second;Reply reply;
            if(message->data.result==CURLE_OK&&Clock::now()<c->deadline&&c->signatureSeen){long code=0;curl_easy_getinfo(c->easy,CURLINFO_RESPONSE_CODE,&code);reply={static_cast<int>(code),std::move(c->body),std::move(c->signature)};}
            curl_multi_remove_handle(s->multi,c->easy);s->active.erase(found);finish(s,c,std::move(reply));
        }
        for(auto it=s->active.begin();it!=s->active.end();){
            if(Clock::now()<it->second->deadline){++it;continue;}
            auto c=it->second;curl_multi_remove_handle(s->multi,it->first);it=s->active.erase(it);finish(s,c);
        }
        if(curl_multi_poll(s->multi,nullptr,0,20,nullptr)!=CURLM_OK)s->stopping=true;
    }
    // Stop admission and drain anything enqueued just before stopping was observed.
    std::deque<std::shared_ptr<Call>> waiting;
    {std::lock_guard<std::mutex> lock(s->mutex);s->stopping=true;waiting.swap(s->queued);auto *multi=s->multi;s->multi=nullptr;curl_multi_cleanup(multi);}
    for(auto &c:waiting)finish(s,c);
}
void enqueue(const std::shared_ptr<State> &s,Request request,std::function<void(Reply)> done){
    if(request.body.size()>kMaxWireBytes||!hexadecimal(request.signature)){answer(done);return;}
    auto c=std::make_shared<Call>();c->request=std::move(request);c->done=std::move(done);c->deadline=Clock::now()+std::chrono::milliseconds(s->config.deadlineMs);
    bool admitted=false;
    {std::lock_guard<std::mutex> lock(s->mutex);if(!s->stopping&&s->outstanding<s->config.maxOutstanding){++s->outstanding;s->queued.push_back(c);admitted=true;if(s->multi)curl_multi_wakeup(s->multi);}}
    if(!admitted)answer(c->done);
}
} // namespace
bool validHttpsConfig(const HttpsConfig &c){
    if(!origin(c.origin)||c.caFile.find('\0')!=std::string::npos||c.deadlineMs<1||c.deadlineMs>20000||c.maxOutstanding<1||c.maxOutstanding>8||c.maxConnections<1||c.maxConnections>c.maxOutstanding)return false;
    return c.testConnectTo.empty()||(c.testConnectTo.compare(0,10,"127.0.0.1:")==0&&port(c.testConnectTo.substr(10)));
}
struct HttpsTransport::Impl {
    std::shared_ptr<State> state;std::thread worker;
    explicit Impl(HttpsConfig config):state(std::make_shared<State>()){
        if(!validHttpsConfig(config))throw std::invalid_argument("invalid_journal_https_configuration");
        static std::once_flag once;static CURLcode initialized=CURLE_FAILED_INIT;
        std::call_once(once,[]{initialized=curl_global_init(CURL_GLOBAL_DEFAULT);});
        if(initialized!=CURLE_OK||!(curl_version_info(CURLVERSION_NOW)->features&CURL_VERSION_ASYNCHDNS))throw std::runtime_error("journal_https_async_resolver_required");
        state->config=std::move(config);state->multi=curl_multi_init();if(!state->multi)throw std::runtime_error("journal_https_init_failed");
        if(curl_multi_setopt(state->multi,CURLMOPT_MAX_TOTAL_CONNECTIONS,static_cast<long>(state->config.maxConnections))!=CURLM_OK||
           curl_multi_setopt(state->multi,CURLMOPT_MAX_HOST_CONNECTIONS,static_cast<long>(state->config.maxConnections))!=CURLM_OK){
            curl_multi_cleanup(state->multi);state->multi=nullptr;throw std::runtime_error("journal_https_limits_unavailable");
        }
        try{worker=std::thread([s=state]{run(s);});}catch(...){curl_multi_cleanup(state->multi);throw;}
    }
    ~Impl(){
        {std::lock_guard<std::mutex> lock(state->mutex);state->stopping=true;if(state->multi)curl_multi_wakeup(state->multi);}
        if(worker.joinable()){if(worker.get_id()==std::this_thread::get_id())worker.detach();else worker.join();}
    }
};
HttpsTransport::HttpsTransport(HttpsConfig config):impl_(std::make_unique<Impl>(std::move(config))){}
HttpsTransport::~HttpsTransport()=default;
Exchange HttpsTransport::exchange() const {
    std::weak_ptr<State> weak=impl_->state;
    return [weak](Request request,std::function<void(Reply)> done){if(auto state=weak.lock())enqueue(state,std::move(request),std::move(done));else answer(done);};
}
} // namespace usapp::deletionjournal::bridge
