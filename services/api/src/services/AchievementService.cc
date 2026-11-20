#include "services/AchievementService.h"
#include <drogon/drogon.h>
#include <memory>
#include "db/Db.h"
namespace usapp {
void AchievementService::read(const std::string &userId, Callback cb) {
    // One snapshot for membership, activation, completions and all five outcomes.
    // No writes, rewards, catalog/user-controlled selectors or server clock dependency.
    static const char *sql = R"SQL(
        WITH member AS (SELECT couple_id FROM couple_members WHERE user_id = $1::uuid),
        activation AS (SELECT enabled_at FROM achievement_activation WHERE singleton)
        SELECT (usapp_achievement_facts(
            ARRAY(SELECT c.completed_at FROM task_completions c
                  WHERE c.couple_id = m.couple_id AND c.completed_by = $1::uuid
                    AND c.completed_at >= a.enabled_at), a.enabled_at)
            || jsonb_build_object('enabled', a.enabled_at IS NOT NULL,
                'enabled_at', to_char(a.enabled_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
                'timezone', 'Asia/Shanghai'))::text AS body
        FROM member m LEFT JOIN activation a ON true
    )SQL";
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(sql, [done](const drogon::orm::Result &rows) {
        Result r;
        if (rows.empty()) r.outcome = Outcome::SpaceNotFound;
        else {
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            std::string errors, text = rows[0]["body"].as<std::string>();
            if (reader->parse(text.data(), text.data() + text.size(), &r.body, &errors)) r.outcome = Outcome::Ok;
        }
        (*done)(std::move(r));
    }, [done](const drogon::orm::DrogonDbException &) { (*done)(Result{}); }, userId);
}
}
