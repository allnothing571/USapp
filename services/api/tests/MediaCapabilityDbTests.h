#pragma once

// Upload capability: GET /api/v1/media/capabilities through the real MediaService, on literal usapp_test with FakeObjectStore. Runs as its
// own CTest target (media_capability_db), outside db_gated. The route and its HTTP answers are covered through the real API binary in
// ReminderLiveApiTest.h, where no store is configured.
#include <memory>
#include <optional>
#include <string>

#include "MediaDbTests.h"

namespace usapptest::mediacapability {

namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using M = usapp::MediaService;
using MO = M::Outcome;

inline std::optional<M::Capabilities> read(const std::string &user) {
    auto p = std::make_shared<ts::Pending<M::Capabilities>>();
    M::instance().capabilities(user, [p](M::Capabilities r) { p->deliver(std::move(r)); });
    return p->wait();
}
// What a read could plausibly change for a couple, and the store's own counters.
inline std::string footprint(const std::string &couple) {
    return md::obs().client->execSqlSync(
        "SELECT concat_ws('|', (SELECT count(*) FROM media_objects WHERE couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_upload_outcomes WHERE couple_id = $1::uuid), "
        "(SELECT count(*) FROM media_promotion_attempts WHERE couple_id = $1::uuid), (SELECT count(*) FROM audit_logs)) AS v",
        couple)[0]["v"].as<std::string>();
}

inline void runMediaCapabilityDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);

    TEST_CASE("Media capability: the same predicate as uploads_enabled, for members only, with no store call and no write") {
        try {
            auto store = md::useStore();
            const auto s = md::space();
            const auto beforeRows = footprint(s.couple);
            const int calls = store->calls.load(), signings = store->readSignings.load(), copies = store->copies.load();
            for (const auto &user : {s.userA, s.userB}) {
                const auto on = read(user);
                CHECK(on && on->outcome == MO::Ok);
                CHECK(on && on->uploadsEnabled == M::instance().uploadsEnabled());
                CHECK(on && on->uploadsEnabled);
            }
            CHECK_EQ(store->calls.load(), calls);
            CHECK_EQ(store->readSignings.load(), signings);
            CHECK_EQ(store->copies.load(), copies);
            CHECK_EQ(footprint(s.couple), beforeRows);

            // Disabled: no store configured. The capability says so, and the upload route still refuses by itself.
            M::instance().configure(md::env.policy, nullptr);
            const auto off = read(s.userA);
            CHECK(off && off->outcome == MO::Ok && !off->uploadsEnabled);
            CHECK(off && off->uploadsEnabled == M::instance().uploadsEnabled());
            CHECK(md::is(md::grant(s.userA), MO::Disabled));
            CHECK_EQ(footprint(s.couple), beforeRows);

            // An enabled answer is a snapshot: configured again it reads true, then a store removed later still refuses the grant.
            md::useStore();
            const auto again = read(s.userA);
            CHECK(again && again->uploadsEnabled);
            M::instance().configure(md::env.policy, nullptr);
            CHECK(md::is(md::grant(s.userA), MO::Disabled));
            md::useStore();

            // A user in no space.
            const auto lonely = c::newUuid();
            ts::commitUser(pool, lonely, "cap_lonely");
            const auto none = read(lonely);
            CHECK(none && none->outcome == MO::SpaceNotFound);
        } catch (const std::exception &failure) {
            md::useStore();
            c::expectThat(false, "capability", md::exceptionCategory(failure));
        }
    }
}

}  // namespace usapptest::mediacapability
