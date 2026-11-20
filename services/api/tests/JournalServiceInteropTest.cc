// Synthetic TLS material + actual HTTPS C++ -> Python dispatcher -> injected stores interoperability.
// Not a provider factory or live resource client. Private test key files are removed by the owned harness.
#include "DeletionJournalBridgeUnitTests.h"
#include "OssStubServer.h"
#include "services/JournalHttpsTransport.h"
#include "RecoverySourceBridgeUnitTests.h"
#include "RecoveryAuthorityFixture.h"
#include "services/PostgresRecoveryStore.h"
#include <charconv>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sys/stat.h>

namespace b=usapp::deletionjournal::bridge;
namespace u=usapptest::journalbridgeunit;
namespace s=usapptest::ossstub;
using namespace usapp::deletionjournal;
namespace {
void pki(const std::string &directory){
    if(directory.rfind("/tmp/usapp-journal-service-",0)!=0||directory.find("..")!=std::string::npos||std::filesystem::exists(directory))throw std::runtime_error("fixture_directory_refused");
    std::filesystem::create_directory(directory);std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
    const auto ca=s::makeCertificate("journal service synthetic CA",nullptr,"");
    const auto leaf=s::makeCertificate("journal.test.invalid",&ca,"journal.test.invalid");
    const auto write=[&](const std::string &name,const std::function<bool(FILE *)> &writer){
        FILE *file=std::fopen((directory+"/"+name).c_str(),"wx");if(!file)throw std::runtime_error("fixture_file_refused");
        ::fchmod(::fileno(file),0600);const bool ok=writer(file);std::fclose(file);if(!ok)throw std::runtime_error("fixture_pki_failed");
    };
    write("ca.crt",[&](FILE *f){return PEM_write_X509(f,ca.cert.get())==1;});
    write("server.crt",[&](FILE *f){return PEM_write_X509(f,leaf.cert.get())==1;});
    write("server.key",[&](FILE *f){return PEM_write_PrivateKey(f,leaf.key.get(),nullptr,nullptr,0,nullptr,nullptr)==1;});
    std::ofstream marker(directory+"/synthetic-only.json");marker<<"{\"synthetic_only\":true}\n";if(!marker)throw std::runtime_error("fixture_marker_failed");
}
template<typename T,typename F>T await(F start){
    auto p=std::make_shared<std::promise<T>>();auto f=p->get_future();start([p](T value){p->set_value(std::move(value));});
    if(f.wait_for(std::chrono::seconds(4))!=std::future_status::ready)throw std::runtime_error("interop_callback_timeout");return f.get();
}

namespace authority = usapp::deletionrecovery::authority;
struct ExternalProof {
    authority::PublicKey key{};
    authority::Signature signature{};
    int64_t issued = 0;
    int64_t expires = 0;
};

template<size_t N>
void decodeProofHex(const std::string &input, std::array<unsigned char, N> &output) {
    if (input.size() != 2 * N || input.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("external_proof_hex_refused");
    for (size_t i = 0; i < output.size(); ++i) {
        unsigned value = 0;
        std::from_chars(input.data() + 2 * i, input.data() + 2 * i + 2, value, 16);
        output[i] = static_cast<unsigned char>(value);
    }
}

int64_t proofTime(const std::string &input) {
    int64_t value = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || value <= 0)
        throw std::runtime_error("external_proof_time_refused");
    return value;
}

void recoverOnPostgres(const std::shared_ptr<usapp::deletionrecovery::bridge::WireSource> &source,
                       const usapp::deletionrecovery::Request &request, bool restored,
                       const b::Exchange &exchange, const std::string &instanceToken,
                       const std::string &archiveSha256, const std::string &expectedAddress,
                       const ExternalProof &proof) {
    namespace r = usapp::deletionrecovery;
    const char *conninfo = std::getenv("USAPP_TEST_MAINT_CONNINFO");
    const std::string prefix = restored
        ? "host=restore port=5432 dbname=usapp_restore_probe user=postgres password="
        : "host=postgres port=5432 dbname=usapp_test user=postgres password=";
    const std::string connection = conninfo ? conninfo : "";
    if (connection.compare(0, prefix.size(), prefix) != 0 ||
        connection.size() == prefix.size() ||
        connection.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos) {
        throw std::runtime_error("recovery_drill_connection_refused");
    }
    auto db = drogon::orm::DbClient::newPgClient(connection, 1);
    auto store = std::make_shared<r::PostgresStore>(db);
    const auto identity = db->execSqlSync(
        "SELECT current_database() AS database, current_user='postgres' AS maintenance, "
        "host(inet_server_addr()) AS server_address, "
        "public.usapp_recovery_closed() AS closed, "
        "(has_database_privilege('usapp_api',current_database(),'CONNECT') OR "
        "has_database_privilege('usapp_push',current_database(),'CONNECT')) AS app_connect");
    if (!identity[0]["maintenance"].as<bool>() ||
        identity[0]["database"].as<std::string>() != (restored ? "usapp_restore_probe" : "usapp_test") ||
        identity[0]["closed"].as<bool>() == restored ||
        (restored && (identity[0]["app_connect"].as<bool>() ||
                      identity[0]["server_address"].as<std::string>() != expectedAddress))) {
        throw std::runtime_error("recovery_drill_target_state_refused");
    }
    const auto run = [&](const r::Request &attempt) {
        return await<r::Report>([&](auto done) { r::Coordinator::run(attempt, source, store, done); });
    };
    const auto observed = await<std::pair<bool, r::Snapshot>>([&](auto done) {
        source->snapshot(request.journalId,
            [done](bool trusted, r::Snapshot snapshot) { done({trusted, std::move(snapshot)}); });
    });
    if (!observed.first) throw std::runtime_error("recovery_drill_snapshot_unavailable");
    const auto &snapshot = observed.second;
    const r::Head pin{request.journalId, 4, "d91bd2e896f34eca5fc1e38172a23b1c436d06391755ede5221b999dcfd09064"};
    const auto runPinned = [&](const r::Head &expected, const std::shared_ptr<r::Source> &reader) {
        return await<r::Report>([&](auto done) { r::Coordinator::runPinned(request, expected, reader, store, done); });
    };
    auto verified = r::Report{r::Outcome::Blocked};
    if (restored) {
        // Checkpoints are fixed from the deterministic publication fixture, independently of its reader reply.
        const r::Head stale{request.journalId, 3, "4c73cd78c87fdca7ea65d7a66ef095c27cf4f0c6cca28646d4877497695a092c"};
        const auto checkClosedWithoutManifest = [&] {
            const auto state = db->execSqlSync(
                "SELECT public.usapp_recovery_closed() AND "
                "NOT has_database_privilege('usapp_api',current_database(),'CONNECT') AND "
                "NOT has_database_privilege('usapp_push',current_database(),'CONNECT') AND "
                "NOT EXISTS (SELECT 1 FROM deletion_recovery_manifests WHERE generation=a.generation) AND "
                "NOT EXISTS (SELECT 1 FROM deletion_recovery_verifications WHERE generation=a.generation) AS safe "
                "FROM deletion_recovery_attempts a ORDER BY generation DESC LIMIT 1");
            CHECK_EQ(state.size(), 1u);
            if (state.size() == 1) CHECK(state[0]["safe"].as<bool>());
        };
        CHECK(runPinned(stale, source).outcome == r::Outcome::HeadChanged);
        checkClosedWithoutManifest();
        auto wrongKey = std::make_shared<r::bridge::WireSource>(
            r::bridge::Context{request.journalId, request.databaseId, request.attemptId}, std::string(64, 'e'), exchange);
        CHECK(runPinned(pin, wrongKey).outcome == r::Outcome::Blocked);
        checkClosedWithoutManifest();
        namespace a = r::authority;
        const a::TargetBinding actual{request, instanceToken, archiveSha256};
        constexpr int64_t now = 1800000000;
        const a::Attestation accepted{actual, pin, now - 1, now + 60};
        usapptest::recoveryauthority::Signer signer;
        const auto runAttested = [&](const a::Attestation &attestation, const a::Signature &signature) {
            return await<r::Report>([&](auto done) {
                a::prepare(actual, signer.publicKey, attestation, signature, source, store, done, [] { return now; });
            });
        };
        const auto before = db->execSqlSync("SELECT count(*) AS n FROM deletion_recovery_attempts")[0]["n"].as<int64_t>();
        auto wrongTarget = accepted;
        wrongTarget.target.instanceToken = instanceToken == std::string(64, 'c') ? std::string(64, 'b') : std::string(64, 'c');
        CHECK(runAttested(wrongTarget, signer.sign(wrongTarget)).outcome == r::Outcome::Blocked);
        auto wrongArchive = accepted;
        wrongArchive.target.archiveSha256 = archiveSha256 == std::string(64, 'a') ? std::string(64, 'b') : std::string(64, 'a');
        CHECK(runAttested(wrongArchive, signer.sign(wrongArchive)).outcome == r::Outcome::Blocked);
        auto expired = accepted;
        expired.expiresAt = now;
        CHECK(runAttested(expired, signer.sign(expired)).outcome == r::Outcome::Blocked);
        auto forged = signer.sign(accepted);
        forged[0] ^= 1;
        CHECK(runAttested(accepted, forged).outcome == r::Outcome::Blocked);
        CHECK_EQ(db->execSqlSync("SELECT count(*) AS n FROM deletion_recovery_attempts")[0]["n"].as<int64_t>(), before);
        const a::Attestation signedExternally{actual, pin, proof.issued, proof.expires};
        verified = await<r::Report>([&](auto done) {
            a::prepare(actual, proof.key, signedExternally, proof.signature, source, store, done);
        });
    } else {
        verified = run(request);
    }
    CHECK(verified.outcome == r::Outcome::VerifiedClosed);
    CHECK_FALSE(verified.servingAllowed);
    const auto rows = db->execSqlSync(
        "SELECT a.generation, v.head_seq, v.head_hash, m.payload->'head'->>'hash' AS manifest_hash "
        "FROM deletion_recovery_attempts a JOIN deletion_recovery_manifests m USING(generation) "
        "JOIN deletion_recovery_verifications v USING(generation) "
        "WHERE a.database_id=$1::uuid AND a.attempt_id=$2::uuid AND a.journal_id=$3::uuid",
        request.databaseId, request.attemptId, request.journalId);
    if (rows.size() != 1) throw std::runtime_error("recovery_drill_verification_missing");
    const auto generation = rows[0]["generation"].as<uint64_t>();
    CHECK_EQ(rows[0]["head_seq"].as<uint64_t>(), snapshot.head.seq);
    CHECK_EQ(rows[0]["head_hash"].as<std::string>(), snapshot.head.hash);
    CHECK_EQ(rows[0]["manifest_hash"].as<std::string>(), snapshot.head.hash);
    const auto quarantine = db->execSqlSync(
        "SELECT target_kind, target_id, reason FROM deletion_recovery_quarantine WHERE generation=$1::bigint",
        std::to_string(generation));
    CHECK_EQ(quarantine.size(), 1u);
    if (quarantine.size() == 1) {
        CHECK_EQ(quarantine[0]["target_kind"].as<std::string>(), "memory");
        CHECK_EQ(quarantine[0]["target_id"].as<std::string>(), usapptest::deletionjournalunit::id(1));
        CHECK_EQ(quarantine[0]["reason"].as<std::string>(), "hidden");
    }
    if (restored) {
        const auto gate = db->execSqlSync(
            "SELECT public.usapp_recovery_closed() AND "
            "NOT has_database_privilege('usapp_api',current_database(),'CONNECT') AND "
            "NOT has_database_privilege('usapp_push',current_database(),'CONNECT') AS closed");
        CHECK(gate[0]["closed"].as<bool>());
        db->closeAll();
        return;
    }

    auto mismatch = request;
    mismatch.attemptId = usapptest::deletionjournalunit::id(99);
    CHECK(run(mismatch).outcome == r::Outcome::Blocked);
    const auto latest = db->execSqlSync(
        "SELECT generation, attempt_id, public.usapp_recovery_closed() AS closed, "
        "EXISTS (SELECT 1 FROM deletion_recovery_manifests m WHERE m.generation=a.generation) AS staged, "
        "EXISTS (SELECT 1 FROM deletion_recovery_verifications v WHERE v.generation=a.generation) AS verified "
        "FROM deletion_recovery_attempts a ORDER BY generation DESC LIMIT 1");
    if (latest.size() != 1) throw std::runtime_error("recovery_drill_fence_missing");
    CHECK(latest[0]["generation"].as<uint64_t>() > generation);
    CHECK_EQ(latest[0]["attempt_id"].as<std::string>(), mismatch.attemptId);
    CHECK(latest[0]["closed"].as<bool>());
    CHECK_FALSE(latest[0]["staged"].as<bool>());
    CHECK_FALSE(latest[0]["verified"].as<bool>());
    const r::Fence stale{request.databaseId, request.attemptId, generation};
    CHECK(await<r::Receipt>([&](auto done) { store->stage(stale, snapshot, done); }) == r::Receipt::Conflict);
    CHECK(await<r::Receipt>([&](auto done) { store->recordVerified(stale, snapshot.head, done); }) == r::Receipt::Conflict);
    db->closeAll();
}
}
int main(int argc,char **argv){
    try {
        if(argc==3&&std::string(argv[1])=="pki"){pki(argv[2]);std::cout<<"JOURNAL_SERVICE_SYNTHETIC_PKI_CREATED\n";return 0;}
        if (argc < 2) return 2;
        const std::string mode = argv[1];
        const bool restored = mode == "recover-restored";
        if (argc != (restored ? 11 : 4) || (!restored && mode != "run" && mode != "recover" && mode != "recover-db")) return 2;
        ExternalProof proof;
        if (restored) {
            decodeProofHex(argv[7], proof.key);
            decodeProofHex(argv[8], proof.signature);
            proof.issued = proofTime(argv[9]);
            proof.expires = proofTime(argv[10]);
        }
        const std::string port=argv[2];if(port.empty()||port.size()>5||!std::all_of(port.begin(),port.end(),[](char c){return c>='0'&&c<='9';})||std::stoi(port)<1||std::stoi(port)>65535)return 2;
        b::HttpsConfig config;config.origin="https://journal.test.invalid:"+port;config.caFile=argv[3];config.testConnectTo="127.0.0.1:"+port;config.deadlineMs=2500;
        b::HttpsTransport transport(config);
        if(std::string(argv[1])!="run"){
            namespace r=usapp::deletionrecovery;namespace rb=r::bridge;namespace model=usapptest::deletionrecoveryunit;
            const rb::Context context{u::journal,usapptest::deletionjournalunit::id(45),usapptest::deletionjournalunit::id(46)};
            auto source=std::make_shared<rb::WireSource>(context,std::string(64,'d'),transport.exchange());
            if (std::string(argv[1]) == "recover-db" || std::string(argv[1]) == "recover-restored") {
                TEST_CASE("Recovery Source HTTPS and PostgreSQL: persist quarantine and preserve closure") {
                    recoverOnPostgres(source, {context.databaseId, context.attemptId, context.journalId},
                                      restored, transport.exchange(), restored ? argv[4] : "", restored ? argv[5] : "",
                                      restored ? argv[6] : "", proof);
                }
                return usapptest::finish();
            }
            TEST_CASE("Recovery Source HTTPS: complete external snapshot, batch head and coordinator remain non-serving"){
                const auto result=await<std::pair<bool,r::Snapshot>>([&](auto done){source->snapshot(u::journal,[done](bool trusted,r::Snapshot value){done({trusted,std::move(value)});});});
                CHECK(result.first);CHECK_EQ(result.second.head.seq,4u);CHECK_EQ(result.second.hidden.size(),1u);CHECK(result.second.purgeBarriers.empty());CHECK(result.second.unresolved.empty());
                if(!result.second.hidden.empty()){CHECK(result.second.hidden[0].kind==r::Kind::Memory);CHECK_EQ(result.second.hidden[0].id,usapptest::deletionjournalunit::id(1));}
                const auto head=await<std::pair<bool,r::Head>>([&](auto done){source->currentHead(u::journal,[done](bool trusted,r::Head value){done({trusted,std::move(value)});});});CHECK(head.first);CHECK(head.second==result.second.head);
                std::vector<std::string> trace;auto store=std::make_shared<model::Store>(trace);
                const r::Request request{context.databaseId,context.attemptId,context.journalId};CHECK(source->binds(request));
                auto report=await<r::Report>([&](auto done){r::Coordinator::run(request,source,store,done);});CHECK(report.outcome==r::Outcome::VerifiedClosed);CHECK_FALSE(report.servingAllowed);CHECK(store->closed);CHECK(store->verified.has_value());CHECK(store->staged.head==result.second.head);
                auto foreign=context;foreign.attemptId=usapptest::deletionjournalunit::id(99);rb::WireSource wrong(foreign,std::string(64,'d'),transport.exchange());
                CHECK_FALSE(await<bool>([&](auto done){wrong.snapshot(u::journal,[done](bool trusted,r::Snapshot){done(trusted);});}));
                rb::WireSource badKey(context,std::string(64,'b'),transport.exchange());CHECK_FALSE(await<bool>([&](auto done){badKey.currentHead(u::journal,[done](bool trusted,r::Head){done(trusted);});}));
                auto mismatch=request;mismatch.databaseId=usapptest::deletionjournalunit::id(99);auto blocked=await<r::Report>([&](auto done){r::Coordinator::run(mismatch,source,store,done);});CHECK(blocked.outcome==r::Outcome::Blocked);CHECK_FALSE(blocked.servingAllowed);CHECK(store->closed);CHECK_FALSE(store->verified.has_value());
            }
            return usapptest::finish();
        }
        b::WireJournal journal(u::journal,u::key,transport.exchange());
        auto a=usapptest::deletionjournalunit::sample(401);auto changed=a;changed.requestFingerprint=std::string(64,'c');
        auto cleanup=usapptest::deletionjournalunit::sample(402,Action::Cleanup);cleanup.subjects[0].kind=Kind::Media;
        auto intent=[&](const Operation &op,IntentState expected){CHECK(await<IntentState>([&](auto done){journal.ensureIntent(op,done);})==expected);};
        auto settle=[&](const Operation &op,EffectState state,bool expected){CHECK_EQ(await<bool>([&](auto done){journal.settle(op,state,done);}),expected);};
        auto lookup=[&](const std::string &id,LookupState expected,const Operation &op){
            const auto result=await<std::pair<LookupState,Operation>>([&](auto done){journal.lookupOperation(id,[done](LookupState state,Operation value){done({state,std::move(value)});});});
            CHECK(result.first==expected);if(result.first==LookupState::Found)CHECK(result.second==op);
        };
        TEST_CASE("Journal service HTTPS interop: signed immutable publication, read, terminal replay and conflicts"){
            intent(a,IntentState::Ready);lookup(a.id,LookupState::Found,a);settle(a,EffectState::Committed,true);intent(a,IntentState::Committed);
            lookup(usapptest::deletionjournalunit::id(999),LookupState::Missing,{});settle(a,EffectState::ProvenNotApplied,false);intent(changed,IntentState::Conflict);
            intent(cleanup,IntentState::Ready);settle(cleanup,EffectState::ProvenNotApplied,true);intent(cleanup,IntentState::Aborted);lookup(cleanup.id,LookupState::Found,cleanup);settle(cleanup,EffectState::Committed,false);
        }
        return usapptest::finish();
    } catch (...) {std::cerr<<"JOURNAL_SERVICE_INTEROP_FAILED\n";return 1;}
}
