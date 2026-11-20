#include "services/AdminRedemptionService.h"
#include <drogon/drogon.h>
#include <memory>
#include "db/Db.h"
namespace usapp {
namespace {

#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// Explicit allowlist from the authoritative state view; never select its key/fingerprint/operation columns.
#define USAPP_ADMIN_REDEMPTION_CORE \
    "'id', t.id, 'product_id', t.product_id, 'product_version', t.product_version, " \
    "'creator_id', t.creator_id, 'redeemer_id', t.redeemer_id, 'title', t.title, " \
    "'price_flowers', t.price_flowers::text, 'redemption_mode', t.redemption_mode, 'status', t.status, " \
    "'created_at', " USAPP_TS("t.created_at") ", 'fulfilled_at', " USAPP_TS("t.fulfilled_at") ", " \
    "'fulfilled_by', t.fulfilled_by, 'cancel_reason', t.cancel_reason, 'cancelled_at', " USAPP_TS("t.cancelled_at") ", " \
    "'cancellation_id', t.cancellation_id, 'cancellation_by', t.cancellation_by"
#define USAPP_ADMIN_REDEMPTION_KEY "(extract(epoch FROM t.created_at) * 1000000)::bigint"
const char *kListSql =
    "WITH page AS (SELECT json_build_object(" USAPP_ADMIN_REDEMPTION_CORE ") AS item, "
    USAPP_ADMIN_REDEMPTION_KEY "::text AS ck, t.created_at AS sk, t.id AS si "
    "FROM reward_redemption_state t WHERE t.couple_id = $1::uuid AND ($2::text = '' OR t.status = $2::text) "
    "AND ($3::bigint = 0 OR (" USAPP_ADMIN_REDEMPTION_KEY ", t.id) < ($3::bigint, $4::uuid)) "
    "ORDER BY t.created_at DESC, t.id DESC LIMIT $5::integer + 1) "
    "SELECT (SELECT status FROM couples WHERE id = $1::uuid) AS space_status, "
    "(SELECT COALESCE(json_agg(json_build_object('item', p.item, 'ck', p.ck) ORDER BY p.sk DESC, p.si DESC), '[]'::json) "
    "FROM page p)::text AS rows, " USAPP_TS("statement_timestamp()") " AS read_at";
const char *kDetailSql =
    "WITH target AS (SELECT t.id,t.couple_id,t.product_id,t.product_version,t.creator_id,t.redeemer_id,"
    "t.title,t.description,t.price_flowers,t.redemption_mode,t.status,t.created_at,t.fulfilled_at,t.fulfilled_by,"
    "t.cancel_reason,t.cancelled_at,t.cancellation_id,t.cancellation_by FROM reward_redemption_state t "
    "WHERE t.couple_id=$1::uuid AND t.id=$2::uuid) "
    "SELECT (SELECT status FROM couples WHERE id=$1::uuid) AS space_status, "
    "(SELECT json_build_object(" USAPP_ADMIN_REDEMPTION_CORE ", 'description', t.description) FROM target t)::text AS redemption, "
    "(SELECT count(*) FROM reward_redemption_events e JOIN target t ON e.redemption_id=t.id AND e.couple_id=t.couple_id) AS event_total, "
    "(SELECT COALESCE(json_agg(json_build_object('id', e.id, 'actor_id', e.actor_id, 'action', e.action, "
    "'proposal_id', e.proposal_id, 'created_at', " USAPP_TS("e.created_at") ") ORDER BY e.created_at DESC,e.id DESC), '[]'::json) "
    "FROM (SELECT e.id,e.actor_id,e.action,e.proposal_id,e.created_at FROM reward_redemption_events e "
    "JOIN target t ON e.redemption_id=t.id AND e.couple_id=t.couple_id ORDER BY e.created_at DESC,e.id DESC LIMIT $3::integer) e)::text AS events, "
    "(SELECT json_build_object('id',ac.id,'admin_id',ac.admin_id,'reason',ac.reason,"
    "'refunded_flowers',ac.refunded_flowers::text,'created_at'," USAPP_TS("ac.created_at") ") FROM admin_redemption_cancellations ac "
    "JOIN target t ON ac.redemption_id=t.id AND ac.couple_id=t.couple_id)::text AS admin_cancellation, "
    USAPP_TS("statement_timestamp()") " AS read_at";
bool parseJson(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

bool optionalJson(const drogon::orm::Row &row, const char *column, Json::Value &out) {
    if (row[column].isNull()) {
        out = Json::nullValue;
        return true;
    }
    return parseJson(row[column].as<std::string>(), out) && out.isObject();
}

bool positiveDecimal(const std::string &digits, std::int64_t &out) {
    if (digits.empty() || digits.size() > 19 || digits[0] == '0') return false;
    std::uint64_t value = 0;
    for (char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

// Canonical lowercase uuid only (the controller lowercases path ids; cursors carry what the server encoded).
bool lowerUuid(const std::string &s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

Json::Value space(const std::string &coupleId, const drogon::orm::Row &row) {
    Json::Value s;
    s["id"] = coupleId;
    s["status"] = row["space_status"].as<std::string>();
    return s;
}

} // namespace
bool AdminRedemptionService::validStatus(const std::string &status) {
    return status.empty() || status == "pending_confirmation" || status == "pending_fulfillment" || status == "fulfilled" || status == "cancelled";
}
std::string AdminRedemptionService::encodeCursor(const std::string &coupleId, const std::string &status, const Cursor &c) {
    return "r1." + (status.empty() ? std::string("all") : status) + "." + std::to_string(c.createdMicros) + "." + c.redemptionId + "." +
           coupleId;
}

bool AdminRedemptionService::decodeCursor(const std::string &raw, const std::string &coupleId, const std::string &status, Cursor &out) {
    if (raw.size() > 160) return false;
    const std::string prefix = "r1." + (status.empty() ? std::string("all") : status) + ".";
    if (raw.compare(0, prefix.size(), prefix) != 0) return false;
    const auto dot = raw.find('.', prefix.size());
    if (dot == std::string::npos) return false;
    const std::string rest = raw.substr(dot + 1);  // <redemptionId>.<coupleId>
    if (rest.size() != 36 + 1 + 36 || rest[36] != '.') return false;
    const std::string redemptionId = rest.substr(0, 36);
    if (!lowerUuid(redemptionId) || rest.substr(37) != coupleId) return false;
    std::int64_t micros = 0;
    if (!positiveDecimal(raw.substr(prefix.size(), dot - prefix.size()), micros)) return false;
    out.createdMicros = micros;
    out.redemptionId = redemptionId;
    return true;
}

void AdminRedemptionService::list(const std::string &coupleId, const std::string &status, int limit, const Cursor *cursor, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    const long long micros = cursor ? static_cast<long long>(cursor->createdMicros) : 0;
    const std::string after = cursor ? cursor->redemptionId : std::string("00000000-0000-0000-0000-000000000000");
    db::Pool::instance().client()->execSqlAsync(
        kListSql,
        [done, coupleId, status, limit](const drogon::orm::Result &rows) {
            Result r;
            if (rows.size() != 1u) { (*done)(Result{}); return; }
            const auto &row = rows[0];
            if (row["space_status"].isNull()) { r.outcome = Outcome::SpaceNotFound; (*done)(std::move(r)); return; }
            Json::Value fetched;
            if (!parseJson(row["rows"].as<std::string>(), fetched) || !fetched.isArray()) { (*done)(Result{}); return; }
            Json::Value items(Json::arrayValue);
            std::string lastKey, lastId;
            for (Json::ArrayIndex i = 0; i < fetched.size() && static_cast<int>(i) < limit; ++i) {
                items.append(fetched[i]["item"]);
                lastKey = fetched[i]["ck"].asString();
                lastId = fetched[i]["item"]["id"].asString();
            }
            r.body["space"] = space(coupleId, row);
            r.body["status"] = status.empty() ? Json::Value(Json::nullValue) : Json::Value(status);
            r.body["items"] = items;
            r.body["read_at"] = row["read_at"].as<std::string>();
            if (static_cast<int>(fetched.size()) > limit && !items.empty()) {
                Cursor next;
                if (!positiveDecimal(lastKey, next.createdMicros) || !lowerUuid(lastId)) { (*done)(Result{}); return; }
                next.redemptionId = lastId;
                r.body["next_cursor"] = encodeCursor(coupleId, status, next);
            } else {
                r.body["next_cursor"] = Json::nullValue;
            }
            r.outcome = Outcome::Ok;
            (*done)(std::move(r));
        },
        [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin redemption list failed", e.base());
            (*done)(Result{});
        },
        coupleId, status, micros, after, limit);
}


void AdminRedemptionService::detail(const std::string &coupleId, const std::string &redemptionId, Callback cb) {
    auto done=std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(kDetailSql,
        [done,coupleId](const drogon::orm::Result &rows) {
            Result r;
            if (rows.size()!=1u) { (*done)(std::move(r)); return; }
            const auto &row=rows[0];
            if (row["space_status"].isNull()) { r.outcome=Outcome::SpaceNotFound; (*done)(std::move(r)); return; }
            if (row["redemption"].isNull()) { r.outcome=Outcome::RedemptionNotFound; (*done)(std::move(r)); return; }
            Json::Value redemption,events,adminCancellation;
            if (!optionalJson(row,"redemption",redemption) || !parseJson(row["events"].as<std::string>(),events) || !events.isArray() || !optionalJson(row,"admin_cancellation",adminCancellation)) { (*done)(std::move(r)); return; }
            r.body["space"]=space(coupleId,row);
            r.body["redemption"]=redemption;
            r.body["admin_cancellation"]=adminCancellation;
            r.body["events"]["total"]=static_cast<Json::Int64>(row["event_total"].as<std::int64_t>());
            r.body["events"]["items"]=events;
            r.body["read_at"]=row["read_at"].as<std::string>();
            r.outcome=Outcome::Ok; (*done)(std::move(r));
        }, [done](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << db::describePgError("admin redemption detail failed",e.base()); (*done)(Result{});
        }, coupleId,redemptionId,kEventItems);
}
} // namespace usapp
