// Local TLS only. Uses synthetic MAC key; no cloud, DB, environment factory or production wiring.
#include "DeletionJournalBridgeUnitTests.h"
#include "OssStubServer.h"
#include "services/JournalHttpsTransport.h"
#include <csignal>
#include <cstdlib>
#include <future>
#include <pthread.h>

namespace b=usapp::deletionjournal::bridge;
namespace u=usapptest::journalbridgeunit;
namespace s=usapptest::ossstub;
using namespace usapp::deletionjournal;
namespace {
const std::string host="journal.test.invalid";
struct Pki {
    s::Certificate ca=s::makeCertificate("journal test CA",nullptr,"");
    s::Certificate good=s::makeCertificate(host,&ca,host);
    s::Certificate wrong=s::makeCertificate("wrong",&ca,"wrong.test.invalid");
    s::Certificate expired=s::makeCertificate(host,&ca,host,-1);
    s::Certificate other=s::makeCertificate("other test CA",nullptr,"");
    s::Certificate untrusted=s::makeCertificate(host,&other,host);
    std::string caFile=s::writeCaFile(ca,"journal");
};
Pki &pki(){static Pki value;return value;}
b::HttpsConfig config(const s::StubServer &server){b::HttpsConfig c;c.origin="https://"+host+":"+std::to_string(server.port());c.caFile=pki().caFile;c.testConnectTo=server.connectTo();return c;}
struct Pending {
    std::promise<b::Reply> promise;std::future<b::Reply> future=promise.get_future();std::atomic<int> calls{0};
    void deliver(b::Reply reply){++calls;try{promise.set_value(std::move(reply));}catch(...){}}
    b::Reply wait(int ms=3000){if(future.wait_for(std::chrono::milliseconds(ms))!=std::future_status::ready)throw std::runtime_error("fixture_callback_timeout");return future.get();}
};
b::Request request(){b::Request result;b::WireJournal journal(u::journal,u::key,[&](b::Request req,auto done){result=std::move(req);done({});});journal.ensureIntent(usapptest::deletionjournalunit::sample(),[](IntentState){});return result;}
std::shared_ptr<Pending> send(const b::Exchange &exchange,b::Request req){auto p=std::make_shared<Pending>();exchange(std::move(req),[p](b::Reply reply){p->deliver(std::move(reply));});return p;}
s::Action signedResponse(const s::Recorded &r,const std::string &state="ready"){
    const auto signature=r.headers.find("x-usapp-journal-mac");
    if(signature==r.headers.end()||signature->second!=b::mac(u::key,"request",r.body)){s::Action a;a.status=401;return a;}
    auto reply=u::response({r.body,signature->second},state);s::Action a;a.body=reply.body;a.headers["X-Usapp-Journal-Mac"]=reply.signature;return a;
}
IntentState ensure(b::WireJournal &journal){auto p=std::make_shared<std::promise<IntentState>>();auto f=p->get_future();journal.ensureIntent(usapptest::deletionjournalunit::sample(),[p](IntentState v){p->set_value(v);});if(f.wait_for(std::chrono::seconds(3))!=std::future_status::ready)throw std::runtime_error("fixture_intent_timeout");return f.get();}
bool until(const std::function<bool()> &predicate,int ms=1000){const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);while(std::chrono::steady_clock::now()<end){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return predicate();}
}
int main(){
    std::signal(SIGPIPE,+[](int){}); // Benign fixture handler; transport must not replace this process policy.
    TEST_CASE("Journal HTTPS: config has no plaintext, arbitrary URL, disabled verification or unbounded admission"){
        b::HttpsConfig c;c.origin="https://journal.test.invalid";CHECK(b::validHttpsConfig(c));
        for(const auto *url:{"http://journal.test.invalid","https://user@journal.test.invalid","https://journal.test.invalid/path","https://journal.test.invalid?x=1","https://journal.test.invalid:0","https://journal.test.invalid:65536","https://journal.test.invalid:01","https://journal.test.invalid."}){auto bad=c;bad.origin=url;CHECK_FALSE(b::validHttpsConfig(bad));}
        auto bad=c;bad.deadlineMs=0;CHECK_FALSE(b::validHttpsConfig(bad));bad=c;bad.maxOutstanding=9;CHECK_FALSE(b::validHttpsConfig(bad));bad=c;bad.maxConnections=0;CHECK_FALSE(b::validHttpsConfig(bad));bad=c;bad.testConnectTo="8.8.8.8:443";CHECK_FALSE(b::validHttpsConfig(bad));
    }
    TEST_CASE("Journal HTTPS: actual verified TLS carries signed bytes on the fixed POST path, ignoring proxy environment"){
        s::StubServer server(pki().good,[](const s::Recorded &r){return signedResponse(r);});
        const char *before=std::getenv("HTTPS_PROXY");const std::string saved=before?before:"";const bool had=before!=nullptr;
        ::setenv("HTTPS_PROXY","http://127.0.0.1:1",1);
        {b::HttpsTransport transport(config(server));b::WireJournal journal(u::journal,u::key,transport.exchange());CHECK(ensure(journal)==IntentState::Ready);}
        if(had)::setenv("HTTPS_PROXY",saved.c_str(),1);else ::unsetenv("HTTPS_PROXY");
        CHECK_EQ(server.requestCount(),1u);const auto r=server.requests()[0];CHECK_EQ(r.method,std::string("POST"));CHECK_EQ(r.path,std::string("/v1/journal"));CHECK(r.query.empty());CHECK_EQ(r.headers.at("content-type"),std::string("application/json"));
    }
    TEST_CASE("Journal HTTPS: untrusted, expired and wrong-name certificates receive no application request"){
        for(const auto &leaf:{pki().wrong,pki().expired,pki().untrusted}){
            s::StubServer server(leaf,[](const s::Recorded &r){return signedResponse(r);});b::HttpsTransport transport(config(server));b::WireJournal journal(u::journal,u::key,transport.exchange());CHECK(ensure(journal)==IntentState::Unknown);CHECK_EQ(server.requestCount(),0u);
        }
        s::StubServer server(pki().good,[](const s::Recorded &r){return signedResponse(r);});auto c=config(server);c.caFile.clear();b::HttpsTransport transport(c);b::WireJournal journal(u::journal,u::key,transport.exchange());CHECK(ensure(journal)==IntentState::Unknown);CHECK_EQ(server.requestCount(),0u);
    }
    TEST_CASE("Journal HTTPS: redirects, lost replies, oversized headers, duplicate MAC and encoding tricks remain unknown without retry"){
        for(int mode=0;mode<6;mode++){
            s::StubServer server(pki().good,[mode](const s::Recorded &r){auto a=signedResponse(r);if(mode==0){a.status=302;a.headers["Location"]="/must-not-follow";}if(mode==1)a.kind=s::Action::Kind::CloseWithoutResponse;if(mode==2)a.headers["X-Padding"]=std::string(8192,'x');if(mode==3)a.headers["x-usapp-journal-mac"]=a.headers["X-Usapp-Journal-Mac"];if(mode==4)a.headers["Content-Encoding"]="gzip";if(mode==5)a.headers["X-Usapp-Journal-Mac"]=std::string(64,'0');return a;});
            b::HttpsTransport transport(config(server));b::WireJournal journal(u::journal,u::key,transport.exchange());CHECK(ensure(journal)==IntentState::Unknown);CHECK_EQ(server.requestCount(),1u);CHECK_EQ(server.connections.load(),1);
        }
    }
    TEST_CASE("Journal HTTPS: exact body cap succeeds, unknown-length over-cap aborts while receiving"){
        for(const auto size:{b::kMaxWireBytes,b::kMaxWireBytes+1}){
            s::StubServer server(pki().good,[size](const s::Recorded &){s::Action a;a.omitContentLength=true;a.body=std::string(size,'x');a.headers["X-Usapp-Journal-Mac"]=b::mac(u::key,"response",a.body);return a;});
            b::HttpsTransport transport(config(server));auto p=send(transport.exchange(),request());const auto reply=p->wait();CHECK_EQ(reply.status,size==b::kMaxWireBytes?200:0);if(reply.status==200)CHECK_EQ(reply.body.size(),b::kMaxWireBytes);CHECK_EQ(p->calls.load(),1);
        }
    }
    TEST_CASE("Journal HTTPS: slow TLS and continuously progressing responses obey total deadline with no late application send"){
        for(int mode=0;mode<2;mode++){
            s::StubServer server(pki().good,[](const s::Recorded &){s::Action a;a.kind=s::Action::Kind::Trickle;a.trickleContentLength=60000;return a;});
            if(mode==0)server.stallHandshakeMs=400;
            auto c=config(server);c.deadlineMs=120;b::HttpsTransport transport(c);const auto start=std::chrono::steady_clock::now();auto p=send(transport.exchange(),request());CHECK_EQ(p->wait().status,0);CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(450));CHECK_EQ(p->calls.load(),1);if(mode==0){CHECK_EQ(server.requestCount(),0u);CHECK_EQ(server.bytesAfterStall.load(),0);}
        }
    }
    TEST_CASE("Journal HTTPS: bounded admission, destruction cancels queued calls, weak exchange cannot resurrect transport"){
        s::StubServer server(pki().good,[](const s::Recorded &r){return signedResponse(r);});server.stallHandshakeMs=400;
        auto c=config(server);c.maxOutstanding=2;c.maxConnections=1;
        auto transport=std::make_unique<b::HttpsTransport>(c);const auto exchange=transport->exchange();auto a=send(exchange,request()),d=send(exchange,request()),overflow=send(exchange,request());
        CHECK_EQ(overflow->wait().status,0);CHECK(until([&]{return server.connections.load()>0;}));
        transport.reset();CHECK_EQ(a->wait().status,0);CHECK_EQ(d->wait().status,0);CHECK_EQ(send(exchange,request())->wait().status,0);
        std::this_thread::sleep_for(std::chrono::milliseconds(450));CHECK_EQ(server.requestCount(),0u);CHECK_EQ(server.bytesAfterStall.load(),0);CHECK_EQ(a->calls.load(),1);CHECK_EQ(d->calls.load(),1);
    }
    TEST_CASE("Journal HTTPS: callbacks can release the final transport owner without self-join or a lost answer"){
        s::StubServer server(pki().good,[](const s::Recorded &r){return signedResponse(r);});
        auto transport=std::make_shared<b::HttpsTransport>(config(server));auto exchange=transport->exchange();auto p=std::make_shared<Pending>();
        exchange(request(),[&transport,p](b::Reply r){transport.reset();p->deliver(std::move(r));});CHECK_EQ(p->wait().status,200);CHECK_FALSE(transport);CHECK_EQ(send(exchange,request())->wait().status,0);
    }
    TEST_CASE("Journal HTTPS: SIGPIPE is blocked only on the owned callback thread"){
        sigset_t before,after;pthread_sigmask(SIG_SETMASK,nullptr,&before);
        struct sigaction globalBefore{},globalAfter{};::sigaction(SIGPIPE,nullptr,&globalBefore);
        s::StubServer server(pki().good,[](const s::Recorded &r){return signedResponse(r);});
        b::HttpsTransport transport(config(server));auto p=std::make_shared<std::promise<bool>>();auto f=p->get_future();
        transport.exchange()(request(),[p](b::Reply reply){sigset_t mask;pthread_sigmask(SIG_SETMASK,nullptr,&mask);p->set_value(reply.status==200&&sigismember(&mask,SIGPIPE)==1);});
        const bool ready=f.wait_for(std::chrono::seconds(3))==std::future_status::ready;CHECK(ready);if(ready)CHECK(f.get());
        pthread_sigmask(SIG_SETMASK,nullptr,&after);CHECK_EQ(sigismember(&before,SIGPIPE),sigismember(&after,SIGPIPE));
        ::sigaction(SIGPIPE,nullptr,&globalAfter);CHECK(globalBefore.sa_handler==globalAfter.sa_handler);
    }
    return usapptest::finish();
}
