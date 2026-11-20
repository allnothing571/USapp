#pragma once

// P3-1d: the real MediaService and OssObjectStore together, against the local TLS model, on literal usapp_test. It shows how the workflow
// behaves through the real adapter. It proves nothing about the vendor. Isolated committed fixtures; no credentials; no sleeps past a grant.
#include <memory>
#include <string>

#include "MediaDbTests.h"
#include "OssStubClient.h"
#include "OssStubServer.h"
#include "services/OssObjectStore.h"

namespace usapptest::ossservice {

namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace st = usapptest::ossstub;
using M = usapp::MediaService;
using MO = M::Outcome;

constexpr const char *kBucket = "usapp-stub";
constexpr const char *kEndpoint = "oss-stub.example.test";
constexpr const char *kHost = "usapp-stub.oss-stub.example.test";

struct Rig {
    st::Certificate ca = st::makeCertificate("usapp service stub ca", nullptr, "");
    st::Certificate leaf = st::makeCertificate(kHost, &ca, kHost);
    std::string caFile = st::writeCaFile(ca, "service");
    std::shared_ptr<st::StubOss> model = std::make_shared<st::StubOss>(st::StubOss::Identities{
        {{"UPLOADKEYID", "UPLOADSECRET"}, {"READKEYID", "READSECRET"}, {"SERVERKEYID", "SERVERSECRET"}}, "cn-stub", kBucket, kHost});
    std::unique_ptr<st::StubServer> server;
    std::shared_ptr<usapp::media::OssObjectStore> store;

    Rig() {
        auto held = model;
        server = std::make_unique<st::StubServer>(leaf, [held](const st::Recorded &request) { return held->handle(request); });
        usapp::media::OssConfig config;
        config.endpoint = kEndpoint;
        config.region = "cn-stub";
        config.bucket = kBucket;
        config.uploadSigner = {"UPLOADKEYID", "UPLOADSECRET"};
        config.readSigner = {"READKEYID", "READSECRET"};
        config.server = {"SERVERKEYID", "SERVERSECRET"};
        config.testCaFile = caFile;
        config.testConnectTo = server->connectTo();
        config.headDeadlineMs = config.infoDeadlineMs = config.removeDeadlineMs = config.listDeadlineMs = config.copyDeadlineMs = 3000;
        store = std::make_shared<usapp::media::OssObjectStore>(std::move(config));
        M::instance().configure(M::Policy{}, store);
    }
    ~Rig() { M::instance().configure(M::Policy{}, nullptr); }

    // A browser's grant and upload, with whatever extras a test adds. Returns the media id.
    std::string upload(const std::string &user, const std::string &type, const std::string &bytes, const std::map<std::string, std::string> &extras = {}) {
        const auto made = md::grant(user, type, static_cast<int64_t>(bytes.size()));
        if (!md::is(made, MO::Created)) throw std::runtime_error("no grant");
        const auto form = md::formOf(made);
        if (st::clientPost(form, bytes, caFile, server->connectTo(), extras).status != 204) throw std::runtime_error("upload refused");
        return md::idOf(made);
    }
    std::string finalOf(const std::string &id) { return md::readyKey(id); }
    st::ClientResult follow(const std::optional<M::Status> &read) { return st::clientFetch(read ? read->redirectUrl : std::string(), caFile, server->connectTo()); }
    std::vector<std::string> copyDestinations(const std::string &id) {
        std::vector<std::string> out;
        for (const auto &request : server->requests()) {
            if (request.method == "PUT" && request.path.find(id) != std::string::npos) out.push_back(request.path);
        }
        return out;
    }
};

inline void runOssServiceDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);

    TEST_CASE("P3-1d service: grant, upload, complete and read through the real adapter, with O8 and clean read headers") {
        try {
            Rig rig;
            const auto s = md::space();
            const auto other = md::space();
            const std::string bytes = st::syntheticJpeg(true);
            const auto id = rig.upload(s.userA, "image/jpeg", bytes, {{"Content-Disposition", "inline; filename=evil.html"}, {"x-oss-meta-evil", "1"}});
            const auto done = md::complete(s.userA, id);
            CHECK_EQ(md::stateOf(done), std::string("ready"));
            CHECK_EQ(rig.model->refusedSignatures, 0);
            const auto key = rig.finalOf(id);
            CHECK(key.rfind("media/" + s.couple + "/" + id + "/", 0) == 0u);
            // The final object carries none of what the upload set, and the copy was sent once.
            const auto &stored = rig.model->objects.at(key).metadata;
            CHECK_EQ(stored.size(), 1u);
            CHECK_EQ(stored.at("Content-Type"), std::string("application/octet-stream"));
            CHECK_EQ(rig.copyDestinations(id).size(), 1u);

            // The uploader downloads the original, as a download, with its metadata (it is theirs).
            const auto original = rig.follow(md::content(s.userA, id, "original"));
            CHECK_EQ(original.status, 200L);
            CHECK_EQ(original.headers.at("content-disposition"), std::string("attachment"));
            CHECK_EQ(original.headers.at("content-type"), std::string("application/octet-stream"));
            CHECK(st::scanMetadata(original.body).gps);
            // Not attached: nobody else gets anything.
            CHECK(md::is(md::content(s.userB, id, "view"), MO::NotFound));
            const auto anchor = md::anchorMemory(pool, s);
            pool->execSqlSync("INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
                              "VALUES ($1::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid)", id, s.couple, anchor, s.userA);
            // Attached: the partner reads a variant through the store, and never the original.
            const auto variant = rig.follow(md::content(s.userB, id, "view"));
            CHECK_EQ(variant.status, 200L);
            CHECK_EQ(variant.headers.at("content-type"), std::string("image/webp"));
            CHECK_EQ(variant.headers.at("content-disposition"), std::string("inline"));
            const auto calls = rig.server->requestCount();
            const auto refused = md::content(s.userB, id, "original");
            CHECK(md::is(refused, MO::NotFound));
            CHECK(md::is(md::content(other.userA, id, "view"), MO::NotFound));
            CHECK(md::is(md::content(other.userA, id, "original"), MO::NotFound));
            CHECK_EQ(rig.server->requestCount(), calls);
        } catch (const std::exception &) {
            c::expectThat(false, "oss-service-read", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1d service: a copy that succeeds but loses its response is never resent to that final key") {
        try {
            Rig rig;
            const auto s = md::space();
            const std::string bytes = st::syntheticPng(false);
            const auto id = rig.upload(s.userA, "image/png", bytes);
            rig.model->loseNextCopyResponse = true;
            const auto first = md::complete(s.userA, id);
            CHECK(md::is(first, MO::StoreUnavailable));
            CHECK_EQ(md::attempts(id), 1);
            const auto lost = md::attemptKey(id, 1);
            CHECK_EQ(rig.model->copiesPerDestination.at(lost), 1);  // it did take effect
            CHECK_EQ(md::disposition(lost), std::string("live"));   // and is known, not READY

            const auto second = md::complete(s.userA, id);
            CHECK_EQ(md::stateOf(second), std::string("ready"));
            const auto winner = md::readyKey(id);
            CHECK(winner != lost);
            CHECK_EQ(md::attemptKey(id, 2), winner);
            // Each final key received exactly one copy, ever.
            const auto sent = rig.copyDestinations(id);
            CHECK_EQ(sent.size(), 2u);
            CHECK(sent[0] != sent[1]);
            CHECK_EQ(rig.model->copiesPerDestination.at(lost), 1);
            CHECK_EQ(rig.model->copiesPerDestination.at(winner), 1);
            CHECK_EQ(md::disposition(winner), std::string("keep"));
            CHECK_EQ(md::disposition(lost), std::string("pending_sweep"));
            // A later retry changes nothing: no third copy.
            CHECK_EQ(md::stateOf(md::complete(s.userA, id)), std::string("ready"));
            CHECK_EQ(rig.copyDestinations(id).size(), 2u);
        } catch (const std::exception &) {
            c::expectThat(false, "oss-service-lost-response", "case failed (details withheld)");
        }
    }

    TEST_CASE("P3-1d service: a malformed copy success never reaches READY, and a store outage is never a rejection") {
        try {
            Rig rig;
            const auto s = md::space();
            const auto id = rig.upload(s.userA, "image/webp", st::syntheticWebp(false));
            rig.model->malformNextCopy = "<html>200 from something in the way</html>";
            const auto first = md::complete(s.userA, id);
            CHECK(md::is(first, MO::StoreUnavailable));
            CHECK_EQ(md::readyKey(id), std::string());
            CHECK_EQ(c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM media_upload_outcomes WHERE media_id = $1::uuid", id), 0);
            CHECK_EQ(md::stateOf(md::complete(s.userA, id)), std::string("ready"));
            const auto sent = rig.copyDestinations(id);
            CHECK_EQ(sent.size(), 2u);
            CHECK(sent[0] != sent[1]);

            // Bytes that are not an image: the model refuses image/info, and the adapter will not call that a verdict on the photo.
            const auto bad = rig.upload(s.userA, "image/png", "not an image at all");
            const auto answer = md::complete(s.userA, bad);
            CHECK(md::is(answer, MO::StoreUnavailable));
            CHECK_EQ(c::scalar(md::obs(), "SELECT count(*)::bigint AS n FROM media_upload_outcomes WHERE media_id = $1::uuid", bad), 0);
        } catch (const std::exception &) {
            c::expectThat(false, "oss-service-malformed", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::ossservice
