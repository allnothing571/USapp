#include "services/NotificationWriter.h"
namespace usapp {
void writeNotification(const std::shared_ptr<db::TxScope> &scope, std::string family,
                       std::string sourceId, std::function<void()> next, std::function<void()> failed) {
    scope->tx().execSqlAsync(
        "INSERT INTO notifications(couple_id,actor_id,recipient_id,event_type,target_kind,target_id,"
        "redemption_id,redemption_event_id,task_change_proposal_id,anniversary_proposal_id) "
        "SELECT couple_id,actor_id,recipient_id,event_type,target_kind,target_id,"
        "CASE WHEN $1 IN ('redemption','fulfillment') THEN $2::uuid END,"
        "CASE WHEN $1 = 'redemption_event' THEN $2::uuid END,"
        "CASE WHEN $1 IN ('task_proposal','task_decision') THEN $2::uuid END,"
        "CASE WHEN $1 IN ('anniversary_proposal','anniversary_decision') THEN $2::uuid END "
        "FROM usapp_notification_source($1,$2::uuid) RETURNING id",
        [scope, next, failed](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failed(); return; }
            next();
        },
        [scope, failed](const drogon::orm::DrogonDbException &) { failed(); }, family, sourceId);
}
}
