#pragma once

// P3-2b rollback compatibility: the P3-2A API binary, started by the isolated verification on a database with 0032, through its real
// AuthFilter. Fixtures with an approved rename and a pending request are written by this build's service; only the HTTP answers come
// from the older binary. Runs only when USAPP_TEST_ALBUM_OLD_API_URL is set, against literal usapp_test, and is not part of CTest.
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "AlbumRenameDbTests.h"
#include "ReminderLiveApiTest.h"

namespace usapptest::albumoldapi {

namespace al = usapptest::album;
namespace ar = usapptest::albumrename;
namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace live = usapptest::reminderliveapi;
using AO = usapp::AlbumService::Outcome;
using Decision = usapp::AlbumService::Decision;

inline void run(const c::ClientPtr &pool, const std::string &dsn, const std::string &base) {
    md::prepareEnv(pool, dsn);
    TEST_CASE("P3-2b rollback: the P3-2a binary starts on 0032, reads renamed albums, and its writes leave the new state intact") {
        try {
            const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (live::get(base + "/api/v1/health").status != 200) {
                if (std::chrono::steady_clock::now() >= ready) throw std::runtime_error("API not ready");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            md::useStore();
            const auto s = md::space();
            const auto id = al::idOf(al::create(s.userA, "原名"));
            const auto p1 = md::agedReady(pool, s), p2 = md::agedReady(pool, s), p3 = md::agedReady(pool, s);
            CHECK(al::is(al::add(s.userA, id, {p1, p2}), AO::Created));
            const auto approved = ar::proposalOf(ar::propose(s.userB, id, 1, "已批准"));
            CHECK(ar::is(ar::decide(s.userA, id, approved, Decision::Approve), AO::Ok));
            const auto pendingId = ar::proposalOf(ar::propose(s.userB, id, 2, "待处理"));

            const std::string csrf = usapp::crypto::randomHex(32);
            const auto token = live::commitToken(pool, s.userA, csrf);
            const auto headers = [&](const std::string &key) {
                return std::vector<std::string>{"Origin: https://localhost:8443", "X-CSRF-Token: " + csrf, "Content-Type: application/json", "Idempotency-Key: " + key};
            };
            const std::string albums = base + "/api/v1/albums";
            const std::string thisAlbum = albums + "/" + id;

            // Reads: the current name, from an 'approve' revision whose key is NULL. The older shape has no version or request.
            const auto read = live::get(thisAlbum, token);
            CHECK_EQ(read.status, 200L);
            CHECK_EQ(live::parsed(read)["album"]["name"].asString(), std::string("已批准"));
            CHECK(!live::parsed(read)["album"].isMember("version"));
            CHECK(!live::parsed(read)["album"].isMember("pending_proposal"));
            const auto listed = live::get(albums, token);
            CHECK_EQ(listed.status, 200L);
            CHECK_EQ(live::parsed(listed)["albums"][0]["name"].asString(), std::string("已批准"));
            CHECK_EQ(live::parsed(live::get(thisAlbum + "/photos", token))["photos"].size(), 2u);

            // Its writes still work, under the pending request.
            const std::string nameBody = R"({"name":"旧版新建"})";
            const auto made = live::call(albums, token, headers(c::newUuid()), &nameBody);
            CHECK_EQ(made.status, 201L);
            const std::string addBody = R"({"media_ids":[")" + p3 + R"("]})";
            CHECK_EQ(live::call(thisAlbum + "/photos", token, headers(c::newUuid()), &addBody).status, 201L);
            const std::string coverBody = R"({"media_id":")" + p3 + R"("})";
            CHECK_EQ(live::call(thisAlbum + "/cover", token, headers(c::newUuid()), &coverBody).status, 201L);
            const std::string empty;
            CHECK_EQ(live::call(thisAlbum + "/photos/" + p1 + "/removal", token, headers(c::newUuid()), &empty).status, 200L);

            // The older binary has no rename routes, so a pending request can be neither seen nor decided through it.
            const std::string renameBody = R"({"expected_version":3,"name":"x"})";
            const auto noRename = live::call(thisAlbum + "/revisions", token, headers(c::newUuid()), &renameBody);
            CHECK(noRename.status == 404L || noRename.status == 405L);
            const auto noRead = live::get(thisAlbum + "/proposals/" + pendingId, token);
            CHECK(noRead.status == 404L || noRead.status == 405L);
            const std::string decisionBody = R"({"outcome":"approve"})";
            const auto noDecision = live::call(thisAlbum + "/proposals/" + pendingId + "/decision", token, headers(c::newUuid()), &decisionBody);
            CHECK(noDecision.status == 404L || noDecision.status == 405L);

            // Back on this build, the request survived, the base is still current, and everything the older binary wrote is there.
            const auto now = al::getAlbum(s.userA, id);
            CHECK_EQ(ar::versionOf(now), 2);
            if (now) {
                CHECK_EQ(now->album["pending_proposal"]["id"].asString(), pendingId);
                CHECK_EQ(now->album["photo_count"].asInt(), 2);
            }
            CHECK_EQ(al::coverOf(now), p3);
            const auto after = ar::decide(s.userA, id, pendingId, Decision::Approve);
            CHECK(ar::is(after, AO::Ok));
            CHECK_EQ(ar::nameOf(after), std::string("待处理"));
            CHECK_EQ(ar::versionOf(after), 3);
            // And the older binary reads that too.
            CHECK_EQ(live::parsed(live::get(thisAlbum, token))["album"]["name"].asString(), std::string("待处理"));
        } catch (const std::exception &) {
            c::expectThat(false, "album-old-api", "case failed (details withheld)");
        }
    }
}

}  // namespace usapptest::albumoldapi
