#pragma once

// Phase 4 B1 (0044_avatar_catalog, docs/phase4-avatar-shop-design.md): the avatar catalog and starter appearance through the real
// AvatarService on literal usapp_test. Fake registry rows only: the maintenance connection adds them, as the reviewed-art step
// would; no real asset exists. Runs as its own CTest target (avatar_catalog_db), outside db_gated. The HTTP boundary (both
// listeners, admin session, Origin and CSRF) is covered through the real binary in ReminderLiveApiTest.h.
#include <json/json.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MediaDbTests.h"
#include "core/Crypto.h"
#include "services/AvatarService.h"

namespace usapptest::avatar {

namespace md = usapptest::media;
namespace c = usapptest::taskcompletion;
namespace ts = usapptest::taskservice;
using S = usapp::AvatarService;
using SO = S::Outcome;

inline const c::Observer &obs() { return md::obs(); }
inline long long count(const std::string &sql, const std::string &a = "", const std::string &b = "") {
    if (a.empty()) return obs().client->execSqlSync(sql)[0]["n"].as<long long>();
    return c::scalar(obs(), sql, a, b);
}

// The maintenance connection: only it may add registry rows, as after art review.
inline const drogon::orm::DbClientPtr &maint() {
    static const auto *client = []() {
        const char *info = std::getenv("USAPP_TEST_MAINT_CONNINFO");
        if (!info || !*info) throw std::runtime_error("avatar_catalog_db needs USAPP_TEST_MAINT_CONNINFO");
        auto *made = new drogon::orm::DbClientPtr(drogon::orm::DbClient::newPgClient(info, 1));
        (*made)->setTimeout(5.0);
        return made;
    }();
    return *client;
}
// The connection may still be coming up: wait for it once, then run each statement exactly once, so a refusal is the answer.
inline void asMaintenance(const std::string &sql) {
    static bool ready = false;
    for (int tries = 0; !ready; ++tries) {
        try {
            maint()->execSqlSync("SELECT 1");
            ready = true;
        } catch (const std::exception &) {
            if (tries >= 100) throw;
            md::sleepMs(20);
        }
    }
    maint()->execSqlSync(sql);
}
// The refusal text of one statement, or empty when it went through.
inline std::string refusal(const std::function<void()> &statement) {
    try {
        statement();
        return std::string();
    } catch (const std::exception &e) {
        return e.what();
    }
}
inline bool refusedWith(const std::string &message, const std::string &needle) { return message.find(needle) != std::string::npos; }

inline std::string hex(char c) { return std::string(64, c); }
// A fake registry asset with valid metadata; nothing exists on disk.
inline std::string layersFor(const std::string &category) {
    return category == "hair" ? "{\"back\":\"" + hex('a') + "\",\"front\":\"" + hex('b') + "\",\"front_under_hat\":\"" + hex('c') + "\"}"
                              : "{\"main\":\"" + hex('d') + "\"}";
}
inline std::string addAsset(const std::string &family, int revision, const std::string &format = "svg") {
    const auto category = family.substr(0, family.find('.'));
    const std::string hat = category == "hat" ? "true" : "NULL";
    const std::string position = category == "accessory" ? "'neck'" : "NULL";
    asMaintenance("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top, accessory_position) "
                  "VALUES ('" + family + ".v" + std::to_string(revision) + "', '" + category + "', '" + family + "', " +
                  std::to_string(revision) + ", '" + format + "', '" + layersFor(category) + "'::jsonb, " + hat + ", " + position + ")");
    return family + ".v" + std::to_string(revision);
}

inline std::string newAdmin(const c::ClientPtr &pool) {
    return pool->execSqlSync("INSERT INTO admin_users (username, password_hash, status) VALUES ($1, $2, 'active') RETURNING id::text AS id",
                             "t_avatar_" + usapp::crypto::randomHex(5), usapp::crypto::pbkdf2Hash(usapp::crypto::randomHex(16), 1000))[0]["id"]
        .as<std::string>();
}
inline std::string key() { return "avatar-" + usapp::crypto::randomHex(12); }

inline S::ItemInput item(const std::string &category, bool starter, const std::string &asset, const std::string &name = "测试装扮",
                         std::optional<int> price = std::nullopt, bool listed = true) {
    S::ItemInput in;
    in.category = category;
    in.starter = starter;
    in.name = name;
    in.assetKey = asset;
    in.price = starter ? std::nullopt : (price ? price : std::optional<int>(100));
    in.listed = listed;
    return in;
}
inline S::RevisionInput revision(int expected, const std::string &asset, const std::string &name, std::optional<int> price, bool listed) {
    S::RevisionInput in;
    in.expectedVersion = expected;
    in.name = name;
    in.assetKey = asset;
    in.price = price;
    in.listed = listed;
    return in;
}

using Pending = ts::Pending<S::Result>;
inline std::optional<S::Result> create(const std::string &admin, const std::string &k, const S::ItemInput &in, S::Fault f = S::Fault::None) {
    auto p = std::make_shared<Pending>();
    S::instance().createItem(admin, k, in, [p](S::Result r) { p->deliver(std::move(r)); }, f);
    return p->wait();
}
inline std::shared_ptr<Pending> startRevise(const std::string &admin, const std::string &k, const std::string &id, const S::RevisionInput &in) {
    auto p = std::make_shared<Pending>();
    S::instance().reviseItem(admin, k, id, in, [p](S::Result r) { p->deliver(std::move(r)); });
    return p;
}
inline std::optional<S::Result> revise(const std::string &admin, const std::string &k, const std::string &id, const S::RevisionInput &in) {
    return startRevise(admin, k, id, in)->wait();
}
inline std::optional<S::Result> starter(const std::string &admin, const std::string &k, int expected, const std::string &hair,
                                        const std::string &clothes, const std::string &expression, S::Fault f = S::Fault::None) {
    S::StarterInput in;
    in.expectedVersion = expected;
    in.hair = hair;
    in.clothes = clothes;
    in.expression = expression;
    auto p = std::make_shared<Pending>();
    S::instance().setStarter(admin, k, in, [p](S::Result r) { p->deliver(std::move(r)); }, f);
    return p->wait();
}
inline std::optional<S::Result> readWith(const std::function<void(S::Callback)> &call) {
    auto p = std::make_shared<Pending>();
    call([p](S::Result r) { p->deliver(std::move(r)); });
    return p->wait();
}
inline bool is(const std::optional<S::Result> &r, SO o) { return r.has_value() && r->outcome == o; }
inline std::string idOf(const std::optional<S::Result> &r) { return r ? r->body["item"]["id"].asString() : std::string(); }
inline int starterVersion() { return static_cast<int>(count("SELECT COALESCE(max(version), 0)::bigint AS n FROM avatar_starter_configs")); }
inline bool listedInCatalog(const std::string &id) {
    const auto r = readWith([](S::Callback cb) { S::instance().catalog(std::move(cb)); });
    if (!r || r->outcome != SO::Ok) throw std::runtime_error("catalog");
    for (const auto &i : r->body["items"]) {
        if (i["id"].asString() == id) return true;
    }
    return false;
}
inline std::string written(const Json::Value &v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

inline void runAvatarCatalogDbTests(const c::ClientPtr &pool, const std::string &dsn) {
    md::prepareEnv(pool, dsn);
    const std::string run = usapp::crypto::randomHex(4);
    const auto fam = [&run](const std::string &category, const std::string &name) { return category + "." + name + "_" + run; };

    TEST_CASE("Avatar B1: with no starter configuration the appearance is incomplete, never invented") {
        try {
            if (starterVersion() != 0) {
                std::printf("AVATAR_INCOMPLETE_CASE skipped: a configuration already exists\n");
            } else {
                const auto r = readWith([](S::Callback cb) { S::instance().appearance("00000000-0000-4000-8000-000000000001", std::move(cb)); });
                CHECK(is(r, SO::Ok));
                if (r) {
                    const auto &a = r->body["appearance"];
                    CHECK_EQ(a["source"].asString(), std::string("starter_defaults"));
                    CHECK(a["complete"].isBool() && !a["complete"].asBool());
                    CHECK(a["starter_version"].isNull());
                    for (const char *slot : {"hair", "clothes", "expression", "hat", "handheld", "accessory"}) CHECK(a["slots"][slot].isNull());
                    CHECK_EQ(r->body.size(), 1u);
                    // B1 fields plus loadout_version (B3, additive): 0 before any save.
                    CHECK_EQ(a.size(), 5u);
                    CHECK(a["loadout_version"].isInt() && a["loadout_version"].asInt() == 0);
                }
                std::printf("AVATAR_INCOMPLETE_CASE covered\n");
            }
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: the registry is maintenance-only, append-only and shape-checked") {
        try {
            const auto family = fam("hat", "cap");
            addAsset(family, 1);
            // The application role can read the registry but never add to it.
            CHECK(refusedWith(refusal([&] {
                pool->execSqlSync("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) "
                                  "VALUES ($1 || '.v2', 'hat', $1, 2, 'svg', $2::jsonb, true)", family, layersFor("hat"));
            }), "permission denied"));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_assets WHERE family = $1", family), 1);
            // Revisions are contiguous; a family keeps its placement hint and format; layers match the category.
            CHECK(refusedWith(refusal([&] { addAsset(family, 3); }), "USAPP_AVATAR_ASSET_REVISION"));
            CHECK(refusedWith(refusal([&] { addAsset(family, 2, "webp"); }), "USAPP_AVATAR_ASSET_FAMILY"));
            CHECK(refusedWith(refusal([&] {
                asMaintenance("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES ('" +
                              family + ".v2', 'hat', '" + family + "', 2, 'svg', '" + layersFor("hat") + "'::jsonb, false)");
            }), "USAPP_AVATAR_ASSET_FAMILY"));
            const auto hair = fam("hair", "bad");
            CHECK(!refusal([&] {
                asMaintenance("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers) VALUES ('" + hair + ".v1', 'hair', '" +
                              hair + "', 1, 'svg', '{\"main\":\"" + hex('e') + "\"}'::jsonb)");
            }).empty());
            CHECK(!refusal([&] {
                asMaintenance("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top) VALUES ('https://x/a.v1', 'hat', '" +
                              fam("hat", "url") + "', 1, 'svg', '" + layersFor("hat") + "'::jsonb, true)");
            }).empty());
            CHECK(!refusal([&] {
                asMaintenance("INSERT INTO avatar_assets (asset_key, category, family, revision, format, layers) VALUES ('" + fam("clothes", "png") +
                              ".v1', 'clothes', '" + fam("clothes", "png") + "', 1, 'png', '" + layersFor("clothes") + "'::jsonb)");
            }).empty());
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_assets WHERE family IN ($1, $2)", hair, fam("clothes", "png")), 0);
            // Never updated or deleted, by anyone.
            CHECK(refusedWith(refusal([&] { pool->execSqlSync("DELETE FROM avatar_assets WHERE family = $1", family); }), "permission denied"));
            CHECK(refusedWith(refusal([&] { asMaintenance("UPDATE avatar_assets SET format = 'webp' WHERE family = '" + family + "'"); }),
                              "USAPP_AVATAR_APPEND_ONLY"));
            CHECK(refusedWith(refusal([&] { asMaintenance("DELETE FROM avatar_assets WHERE family = '" + family + "'"); }), "USAPP_AVATAR_APPEND_ONLY"));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: create, replay, changed payload and an atomic audit") {
        try {
            const auto admin = newAdmin(pool);
            const auto asset = addAsset(fam("handheld", "balloon"), 1);
            const auto k = key();
            const auto before = count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin);
            const auto made = create(admin, k, item("handheld", false, asset, "气球", 120, true));
            CHECK(is(made, SO::Created));
            if (!is(made, SO::Created)) return;
            const auto id = idOf(made);
            CHECK(!made->body["replayed"].asBool());
            CHECK_EQ(made->body["revision_version"].asInt(), 1);
            CHECK_EQ(made->body["item"]["price_sunshine"].asInt(), 120);
            CHECK_EQ(made->body["item"]["asset"]["key"].asString(), asset);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_type = 'admin' AND actor_id = $1::uuid AND action = 'avatar.item_created' "
                           "AND target_id = $2", admin, id), 1);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin), before + 1);
            // Same key and body: the original item and revision, nothing new.
            const auto again = create(admin, k, item("handheld", false, asset, "气球", 120, true));
            CHECK(is(again, SO::Replayed));
            CHECK(again && again->body["replayed"].asBool() && idOf(again) == id);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin), before + 1);
            // Same key, another body or another kind of write.
            CHECK(is(create(admin, k, item("handheld", false, asset, "气球", 121, true)), SO::KeyReused));
            CHECK(is(revise(admin, k, id, revision(1, asset, "气球二", 120, true)), SO::KeyReused));
            CHECK(is(starter(admin, k, starterVersion(), id, id, id), SO::KeyReused));
            // Another admin's identical key is a separate request: here, the family is already an item.
            CHECK(is(create(newAdmin(pool), k, item("handheld", false, asset, "气球", 120, true)), SO::AssetInvalid));
            // Unknown asset; prices and categories are checked before anything is written.
            CHECK(is(create(admin, key(), item("handheld", false, fam("handheld", "none") + ".v1")), SO::AssetInvalid));
            S::ItemInput bad;
            std::string message;
            Json::Value body;
            Json::Reader().parse(R"({"category":"hat","starter":true,"name":"x","asset_key":"hat.cap.v1","price_sunshine":5,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"hat.cap.v1","price_sunshine":10000,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"hair.cap.v1","price_sunshine":5,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"https://evil/hat.v1","price_sunshine":5,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"hat/../../etc.v1","price_sunshine":5,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":" x","asset_key":"hat.cap.v1","price_sunshine":5,"listed":true})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"hat.cap.v1","price_sunshine":5,"listed":true,"owner":"u"})", body);
            CHECK(!S::parseCreate(body, bad, message));
            Json::Reader().parse(R"({"category":"hat","starter":false,"name":"x","asset_key":"hat.cap.v1","price_sunshine":5,"listed":true})", body);
            CHECK(S::parseCreate(body, bad, message));
            CHECK(!S::validName(std::string("a\x01")));
            CHECK(!S::validName(std::string(21, 'a')));
            CHECK(S::validName("二十个字以内的名字"));
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: a failure before commit leaves no item, revision or audit; the retry writes once") {
        try {
            const auto admin = newAdmin(pool);
            const auto asset = addAsset(fam("accessory", "scarf"), 1);
            const auto k = key();
            const auto in = item("accessory", false, asset, "围巾", 300, true);
            CHECK(is(create(admin, k, in, S::Fault::FailBeforeCommit), SO::ResultUnknown));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_items WHERE family = $1", asset.substr(0, asset.rfind('.'))), 0);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_item_revisions WHERE admin_user_id = $1::uuid", admin), 0);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin), 0);
            const auto made = create(admin, k, in);
            CHECK(is(made, SO::Created));
            CHECK(is(create(admin, k, in), SO::Replayed));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin), 1);
            // The same for a valid starter configuration: the fault comes after the function accepted it.
            const auto hair = idOf(create(admin, key(), item("hair", true, addAsset(fam("hair", "fault"), 1), "发")));
            const auto clothes = idOf(create(admin, key(), item("clothes", true, addAsset(fam("clothes", "fault"), 1), "衣")));
            const auto face = idOf(create(admin, key(), item("expression", true, addAsset(fam("expression", "fault"), 1), "脸")));
            CHECK(!hair.empty() && !clothes.empty() && !face.empty());
            const auto version = starterVersion();
            const auto audits = count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.starter_configured'");
            CHECK(is(starter(admin, key(), version, hair, clothes, face, S::Fault::FailBeforeCommit), SO::ResultUnknown));
            CHECK_EQ(starterVersion(), version);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.starter_configured'"), audits);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: revisions rename, correct the asset, reprice and unlist; versions conflict; category and family never change") {
        try {
            const auto admin = newAdmin(pool);
            const auto family = fam("clothes", "coat");
            const auto v1 = addAsset(family, 1);
            const auto other = addAsset(fam("clothes", "dress"), 1);
            const auto made = create(admin, key(), item("clothes", false, v1, "外套", 500, true));
            CHECK(is(made, SO::Created));
            if (!is(made, SO::Created)) return;
            const auto id = idOf(made);
            CHECK(listedInCatalog(id));
            // Rename.
            const auto rk = key();
            const auto renamed = revise(admin, rk, id, revision(1, v1, "大衣", 500, true));
            CHECK(is(renamed, SO::Created));
            CHECK(renamed && renamed->body["item"]["version"].asInt() == 2 && renamed->body["item"]["name"].asString() == "大衣");
            CHECK(is(revise(admin, rk, id, revision(1, v1, "大衣", 500, true)), SO::Replayed));
            CHECK(is(revise(admin, rk, id, revision(1, v1, "大衣2", 500, true)), SO::KeyReused));
            // Stale version; unchanged content; another family; a starter-style price on a paid item.
            CHECK(is(revise(admin, key(), id, revision(1, v1, "长外套", 500, true)), SO::VersionConflict));
            CHECK(is(revise(admin, key(), id, revision(2, v1, "大衣", 500, true)), SO::Validation));
            CHECK(is(revise(admin, key(), id, revision(2, other, "大衣", 500, true)), SO::AssetInvalid));
            CHECK(is(revise(admin, key(), id, revision(2, v1, "大衣", std::nullopt, true)), SO::Validation));
            // A reviewed defect correction: the next revision of the same family. Never back to the old one.
            const auto v2 = addAsset(family, 2);
            CHECK(is(revise(admin, key(), id, revision(2, v2, "大衣", 600, true)), SO::Created));
            CHECK(is(revise(admin, key(), id, revision(3, v1, "大衣", 600, true)), SO::AssetInvalid));
            // Unlisting hides it from the catalog, not from the admin list; listing again shows it.
            CHECK(is(revise(admin, key(), id, revision(3, v2, "大衣", 600, false)), SO::Created));
            CHECK(!listedInCatalog(id));
            const auto all = readWith([](S::Callback cb) { S::instance().adminItems(std::move(cb)); });
            CHECK(all && written(all->body).find(id) != std::string::npos);
            const auto detail = readWith([&id](S::Callback cb) { S::instance().adminItem(id, std::move(cb)); });
            CHECK(is(detail, SO::Ok));
            CHECK(detail && detail->body["revisions"].size() == 4u && detail->body["item"]["category"].asString() == "clothes");
            CHECK(detail && detail->body["revisions"][0]["name"].asString() == "外套" && detail->body["revisions"][0]["price_sunshine"].asInt() == 500);
            CHECK(is(revise(admin, key(), id, revision(4, v2, "大衣", 600, true)), SO::Created));
            CHECK(listedInCatalog(id));
            CHECK(is(revise(admin, key(), c::newUuid(), revision(1, v2, "x", 1, true)), SO::NotFound));
            CHECK(is(readWith([](S::Callback cb) { S::instance().adminItem(c::newUuid(), std::move(cb)); }), SO::NotFound));
            CHECK(is(readWith([](S::Callback cb) { S::instance().adminItem("not-a-uuid", std::move(cb)); }), SO::NotFound));
            // The database refuses what the API never sends: a version gap, another category's asset, and any update or delete.
            CHECK(refusedWith(refusal([&] {
                pool->execSqlSync("INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id, idempotency_key, "
                                  "request_fingerprint) VALUES ($1::uuid, 9, 'x', $2, 5, true, $3::uuid, $4, repeat('a', 64))", id, v2, admin, key());
            }), "USAPP_AVATAR_VERSION"));
            CHECK(refusedWith(refusal([&] {
                pool->execSqlSync("INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id, idempotency_key, "
                                  "request_fingerprint) VALUES ($1::uuid, 6, 'x', $2, 5, true, $3::uuid, $4, repeat('a', 64))", id, other, admin, key());
            }), "USAPP_AVATAR_ASSET"));
            for (const char *sql : {"UPDATE avatar_items SET category = 'hat' WHERE id = $1::uuid", "DELETE FROM avatar_items WHERE id = $1::uuid",
                                    "UPDATE avatar_item_revisions SET price_sunshine = 1 WHERE item_id = $1::uuid",
                                    "DELETE FROM avatar_item_revisions WHERE item_id = $1::uuid"}) {
                CHECK(refusedWith(refusal([&] { pool->execSqlSync(sql, id); }), "permission denied"));
            }
            CHECK(refusedWith(refusal([&] { asMaintenance("UPDATE avatar_items SET starter = true WHERE id = '" + id + "'"); }), "USAPP_AVATAR_APPEND_ONLY"));
            CHECK(refusedWith(refusal([&] { asMaintenance("DELETE FROM avatar_item_revisions WHERE item_id = '" + id + "'"); }), "USAPP_AVATAR_APPEND_ONLY"));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_item_revisions WHERE item_id = $1::uuid", id), 5);
            // Every revision has its audit row.
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE target_id = $1 AND action IN ('avatar.item_created', 'avatar.item_revised')", id), 5);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: two revisions against the same version: exactly one wins") {
        try {
            const auto admin = newAdmin(pool);
            const auto asset = addAsset(fam("hat", "beret"), 1);
            const auto id = idOf(create(admin, key(), item("hat", false, asset, "贝雷帽", 200, true)));
            CHECK(!id.empty());
            auto left = startRevise(admin, key(), id, revision(1, asset, "左", 200, true));
            auto right = startRevise(admin, key(), id, revision(1, asset, "右", 200, true));
            const auto a = left->wait();
            const auto b = right->wait();
            CHECK(a && b);
            if (!a || !b) return;
            CHECK((a->outcome == SO::Created) != (b->outcome == SO::Created));
            CHECK(a->outcome == SO::VersionConflict || b->outcome == SO::VersionConflict);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_item_revisions WHERE item_id = $1::uuid", id), 2);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: the starter configuration is explicit and deterministic, and only starters of the slot may fill it") {
        try {
            const auto admin = newAdmin(pool);
            const auto hairA = idOf(create(admin, key(), item("hair", true, addAsset(fam("hair", "short"), 1), "短发")));
            const auto clothes = idOf(create(admin, key(), item("clothes", true, addAsset(fam("clothes", "tee"), 1), "T恤")));
            const auto smile = idOf(create(admin, key(), item("expression", true, addAsset(fam("expression", "smile"), 1), "微笑")));
            const auto calm = idOf(create(admin, key(), item("expression", true, addAsset(fam("expression", "calm"), 1), "平静")));
            const auto paidHair = idOf(create(admin, key(), item("hair", false, addAsset(fam("hair", "long"), 1), "长发", 50)));
            CHECK(!hairA.empty() && !clothes.empty() && !smile.empty() && !calm.empty() && !paidHair.empty());
            // A paid item, a wrong category, an unknown id: all refused, nothing written.
            const auto version = starterVersion();
            CHECK(is(starter(admin, key(), version, paidHair, clothes, smile), SO::StarterInvalid));
            CHECK(is(starter(admin, key(), version, hairA, smile, clothes), SO::StarterInvalid));
            CHECK(is(starter(admin, key(), version, c::newUuid(), clothes, smile), SO::StarterInvalid));
            CHECK(is(starter(admin, key(), version + 1, hairA, clothes, smile), SO::VersionConflict));
            CHECK_EQ(starterVersion(), version);
            CHECK(refusedWith(refusal([&] {
                pool->execSqlSync("INSERT INTO avatar_starter_configs (version, hair_item_id, clothes_item_id, expression_item_id, admin_user_id, idempotency_key, "
                                  "request_fingerprint) VALUES ($1::integer, $2::uuid, $3::uuid, $4::uuid, $5::uuid, $6, repeat('a', 64))",
                                  std::to_string(version + 1), paidHair, clothes, smile, admin, key());
            }), "USAPP_AVATAR_STARTER_ITEM"));
            // Two free expressions exist; the configuration names one.
            const auto k = key();
            const auto set = starter(admin, k, version, hairA, clothes, smile);
            CHECK(is(set, SO::Created));
            CHECK(set && set->body["starter"]["version"].asInt() == version + 1 && set->body["starter"]["expression"].asString() == smile);
            CHECK(is(starter(admin, k, version, hairA, clothes, smile), SO::Replayed));
            CHECK(is(starter(admin, k, version, hairA, clothes, calm), SO::KeyReused));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE action = 'avatar.starter_configured' AND target_id = $1",
                           std::to_string(version + 1)), 1);
            // A starter created later, and unlisting the configured one, change nothing: the defaults are exactly what is configured.
            const auto newerHair = idOf(create(admin, key(), item("hair", true, addAsset(fam("hair", "newer"), 1), "新发型")));
            CHECK(!newerHair.empty());
            const auto hairAsset = readWith([&hairA](S::Callback cb) { S::instance().adminItem(hairA, std::move(cb)); });
            CHECK(is(revise(admin, key(), hairA, revision(1, hairAsset->body["item"]["asset"]["key"].asString(), "短发", std::nullopt, false)), SO::Created));
            for (const auto &user : {c::newUuid(), c::newUuid()}) {
                const auto r = readWith([&user](S::Callback cb) { S::instance().appearance(user, std::move(cb)); });
                CHECK(is(r, SO::Ok));
                if (!r) continue;
                const auto &a = r->body["appearance"];
                CHECK(a["complete"].asBool());
                CHECK_EQ(a["source"].asString(), std::string("starter_defaults"));
                CHECK_EQ(a["starter_version"].asInt(), version + 1);
                CHECK_EQ(a["slots"]["hair"]["id"].asString(), hairA);
                CHECK_EQ(a["slots"]["clothes"]["id"].asString(), clothes);
                CHECK_EQ(a["slots"]["expression"]["id"].asString(), smile);
                CHECK(a["slots"]["hair"]["starter"].asBool() && a["slots"]["hair"]["price_sunshine"].isNull());
                for (const char *slot : {"hat", "handheld", "accessory"}) CHECK(a["slots"][slot].isNull());
                // Nothing about ownership, purchases or any user appears.
                const auto text = written(r->body);
                for (const char *word : {"owned", "purchase", "user", "wallet", "sunshine_balance"}) CHECK(text.find(word) == std::string::npos);
            }
            // A starter can never gain a price; a paid item can never become a starter.
            CHECK(is(revise(admin, key(), smile, revision(1, readWith([&smile](S::Callback cb) { S::instance().adminItem(smile, std::move(cb)); })
                                                                ->body["item"]["asset"]["key"].asString(), "微笑", 5, true)),
                     SO::Validation));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_items WHERE id = $1::uuid AND starter", paidHair), 0);
            // Starter items never touch the wallet: no purchase, and so no AVATAR_PURCHASE operation, ever names one. (Written for B1 as "no
            // avatar wallet operation at all"; B2 purchases of paid items in the same database are legitimate, so the invariant is now exact.)
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_purchases p JOIN avatar_items i ON i.id = p.item_id WHERE i.starter"), 0);
            for (const auto &id : {hairA, clothes, smile, calm, newerHair}) {
                CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_purchases WHERE item_id = $1::uuid", id), 0);
            }
        } catch (const std::exception &) {
            CHECK(false);
        }
    }

    TEST_CASE("Avatar B1: a disabled administrator writes nothing") {
        try {
            const auto admin = newAdmin(pool);
            const auto asset = addAsset(fam("handheld", "flower"), 1);
            asMaintenance("UPDATE admin_users SET status = 'disabled' WHERE id = '" + admin + "'");
            CHECK(is(create(admin, key(), item("handheld", false, asset, "花", 10, true)), SO::Error));
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM avatar_item_revisions WHERE admin_user_id = $1::uuid", admin), 0);
            CHECK_EQ(count("SELECT count(*)::bigint AS n FROM audit_logs WHERE actor_id = $1::uuid", admin), 0);
        } catch (const std::exception &) {
            CHECK(false);
        }
    }
}

}  // namespace usapptest::avatar
