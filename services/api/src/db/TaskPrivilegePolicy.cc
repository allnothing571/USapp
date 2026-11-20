// =====================================================================
// services/api/src/db/TaskPrivilegePolicy.cc
// =====================================================================
#include "db/TaskPrivilegePolicy.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace usapp::db {

namespace {

// 与钱包矩阵查询结构相同，只把表清单换成任务表。只返回实际值。
const char *kTaskMatrixSql = R"SQL(
WITH
task_tables(table_name) AS (
    VALUES ('tasks'), ('task_completions'), ('task_progress_events'), ('task_read_model'),
           ('task_change_proposals'), ('task_change_decisions'), ('task_closures'),
           ('reward_products'), ('reward_product_revisions'),
           ('reward_redemptions'), ('reward_redemption_fulfillments'),
           ('reward_redemption_events'), ('reward_redemption_state'),
           ('messages'), ('message_replies'),
           ('anniversaries'), ('anniversary_revisions'), ('anniversary_edit_proposals'),
           ('anniversary_edit_decisions'), ('anniversary_home_pins'), ('notifications'), ('notification_reads'),
           ('notification_intents'), ('task_reactions'),
           ('push_subscriptions'), ('push_subscription_revocations'), ('push_registration_commands'),
           ('push_dispatch_control'), ('push_deliveries'), ('push_attempts'), ('push_attempt_observations'),
           ('push_attempt_expiries'), ('push_delivery_outcomes'),
           ('reminder_rules'), ('reminder_occurrences'),
           ('media_objects'), ('media_promotion_attempts'), ('media_upload_outcomes'), ('media_cleanup_plans'),
           ('media_cleanup_passes'), ('media_bindings'), ('media_late_objects'),
           ('memories'), ('memory_texts'), ('memory_revisions'), ('memory_revision_photos'),
           ('memory_edit_proposals'), ('memory_proposal_photos'), ('memory_edit_decisions'),
           ('albums'), ('album_texts'), ('album_revisions'), ('album_photo_batches'), ('album_items'),
           ('album_item_removals'), ('album_cover_events'), ('album_edit_proposals'), ('album_edit_decisions'), ('shared_plans'), ('shared_plan_events'), ('shared_plan_memory_links'),
           ('media_deletions'), ('media_deletion_reversals'),
           ('memory_deletions'), ('memory_deletion_reversals'),
           ('memory_delete_requests'), ('memory_delete_decisions'),
           ('memory_deletion_photo_cycles'), ('memory_purges'),
           ('deletion_recovery_attempts'), ('deletion_recovery_manifests'), ('deletion_recovery_quarantine'), ('deletion_recovery_verifications'),
           ('avatar_assets'), ('avatar_items'), ('avatar_item_revisions'), ('avatar_starter_configs'), ('avatar_purchases'), ('avatar_loadout_events'), ('achievement_activation'), ('admin_wallet_adjustments'), ('admin_redemption_cancellations'), ('account_login_events')
),
resolved AS (
    SELECT w.table_name, to_regclass('public.' || w.table_name)::oid AS rel
      FROM task_tables w
),
subjects(subject, role_name) AS (
    VALUES ('current', current_user::text), ('usapp_push', 'usapp_push'), ('public', 'public')
),
subject_state AS (
    SELECT s.subject, s.role_name,
           (s.subject = 'public'
            OR EXISTS (SELECT 1 FROM pg_roles r WHERE r.rolname = s.role_name)) AS role_exists
      FROM subjects s
),
table_privs(privilege) AS (
    VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'),
           ('TRUNCATE'), ('REFERENCES'), ('TRIGGER'), ('MAINTAIN')
),
column_privs(privilege) AS (
    VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')
)
SELECT 'identity'::text AS kind, 'current'::text AS subject, current_user::text AS object,
       ''::text AS column_name, 'is_app_role'::text AS privilege,
       (current_user = 'usapp_api' AND NOT COALESCE((SELECT rolbypassrls FROM pg_roles WHERE rolname=current_user), true)) AS actual
UNION ALL
SELECT 'identity', 'current', '', '', 'rolsuper',
       (SELECT r.rolsuper FROM pg_roles r WHERE r.rolname = current_user)
UNION ALL
SELECT 'identity', 'current', '', '', 'has_role_memberships',
       EXISTS (SELECT 1
                 FROM pg_auth_members m
                 JOIN pg_roles r ON r.oid = m.member
                WHERE r.rolname = current_user)
UNION ALL
SELECT 'identity', 'usapp_push', '', '', 'role_exists', s.role_exists
  FROM subject_state s
 WHERE s.subject = 'usapp_push'
UNION ALL
SELECT 'identity', 'usapp_memory_eraser', '', '', 'no_violations',
       NOT EXISTS (SELECT 1 FROM public.usapp_memory_eraser_violations())
UNION ALL
SELECT 'identity', 'usapp_memory_eraser', '', '', 'violation_' || v, true
  FROM public.usapp_memory_eraser_violations() v
UNION ALL
SELECT 'identity', 'timestamp_guards', '', '', 'no_column_defaults',
       NOT EXISTS (SELECT 1
                     FROM (VALUES ('public.messages', 'created_at'), ('public.message_replies', 'created_at'),
                                  ('public.reward_redemptions', 'created_at'), ('public.reward_redemption_events', 'created_at'),
                                  ('public.reward_redemption_fulfillments', 'fulfilled_at'), ('public.notifications', 'created_at'),
                                  ('public.notification_intents', 'created_at'), ('public.push_deliveries', 'created_at')) w(rel, col)
                     JOIN pg_attribute a ON a.attrelid = to_regclass(w.rel) AND a.attname = w.col
                    WHERE a.atthasdef)
UNION ALL
SELECT 'identity', 'phase3_notifications', '', '', 'sources_present',
       (SELECT count(*) FROM pg_attribute a
         WHERE a.attrelid IN (to_regclass('public.notifications'), to_regclass('public.notification_intents'))
           AND a.attname IN ('memory_id', 'shared_plan_id', 'album_batch_id') AND NOT a.attisdropped) = 6
UNION ALL
SELECT 'table', 'current', t.table_name, '', 'exists', t.rel IS NOT NULL
  FROM resolved t
UNION ALL
SELECT 'table', 'current', t.table_name, '', 'owned_by_current_user',
       (SELECT c.relowner = (SELECT r.oid FROM pg_roles r WHERE r.rolname = current_user)
          FROM pg_class c
         WHERE c.oid = t.rel)
  FROM resolved t
UNION ALL
SELECT 'table_privilege', s.subject, t.table_name, '', p.privilege,
       CASE WHEN t.rel IS NULL OR NOT s.role_exists THEN NULL
            ELSE has_table_privilege(s.role_name, t.rel, p.privilege) END
  FROM subject_state s CROSS JOIN resolved t CROSS JOIN table_privs p
UNION ALL
SELECT 'any_column_privilege', s.subject, t.table_name, '', p.privilege,
       CASE WHEN t.rel IS NULL OR NOT s.role_exists THEN NULL
            ELSE has_any_column_privilege(s.role_name, t.rel, p.privilege) END
  FROM subject_state s CROSS JOIN resolved t CROSS JOIN column_privs p
UNION ALL
SELECT 'column_privilege', s.subject, t.table_name, a.attname::text, 'UPDATE',
       CASE WHEN NOT s.role_exists THEN NULL
            ELSE has_column_privilege(s.role_name, t.rel, a.attnum, 'UPDATE') END
  FROM subject_state s
 CROSS JOIN resolved t
  JOIN pg_attribute a ON a.attrelid = t.rel AND a.attnum > 0 AND NOT a.attisdropped
)SQL";

// 每张表至少有一个必然存在的列要出现在列级结果里，防止列枚举为空时静默通过。
const char *requiredColumnFor(const std::string &table) {
    if (table == "tasks" || table == "task_read_model" || table == "task_change_proposals" ||
        table == "reward_products") {
        return "id";
    }
    if (table == "reward_product_revisions" || table == "reward_redemptions") return "product_id";
    if (table == "reward_redemption_state") return "id";
    if (table == "reward_redemption_events") return "redemption_id";
    if (table == "reward_redemption_fulfillments") return "redemption_id";
    if (table == "task_reactions") return "id";
    if (table == "push_subscriptions") return "id";
    if (table == "push_subscription_revocations") return "subscription_id";
    if (table == "push_registration_commands") return "user_id";
    if (table == "push_dispatch_control") return "revision";
    if (table == "push_deliveries" || table == "push_attempts" || table == "push_attempt_observations" ||
        table == "push_attempt_expiries" || table == "push_delivery_outcomes") {
        return "notification_id";
    }
    if (table == "reminder_rules" || table == "reminder_occurrences") return "id";
    if (table == "media_objects") return "id";
    if (table == "media_late_objects") return "object_key";
    if (table.rfind("deletion_recovery_", 0) == 0) return "generation";
    if (table.rfind("media_", 0) == 0) return "media_id";
    if (table == "memories" || table == "memory_texts" || table == "memory_edit_proposals") return "id";
    if (table == "memory_revisions" || table == "memory_revision_photos" || table == "memory_deletions" ||
        table == "memory_deletion_reversals" || table == "memory_delete_requests" || table == "memory_delete_decisions" ||
        table == "memory_deletion_photo_cycles" || table == "memory_purges") {
        return "memory_id";
    }
    if (table == "memory_proposal_photos" || table == "memory_edit_decisions") return "proposal_id";
    if (table == "albums" || table == "album_texts" || table == "album_photo_batches" || table == "album_edit_proposals") return "id";
    if (table == "album_edit_decisions") return "proposal_id";
    if (table == "shared_plan_memory_links") return "plan_id";
    if (table == "shared_plans" || table == "shared_plan_events") return "id";
    if (table.rfind("album_", 0) == 0) return "album_id";
    if (table == "notifications") return "id";
    if (table == "notification_reads") return "notification_id";
    if (table == "notification_intents") return "id";
    if (table == "messages") return "id";
    if (table == "message_replies") return "message_id";
    if (table == "anniversaries" || table == "anniversary_edit_proposals") return "id";
    if (table == "anniversary_revisions") return "anniversary_id";
    if (table == "anniversary_edit_decisions") return "proposal_id";
    if (table == "anniversary_home_pins") return "user_id";
    if (table == "avatar_assets") return "asset_key";
    if (table == "avatar_items") return "id";
    if (table == "avatar_item_revisions") return "item_id";
    if (table == "avatar_starter_configs") return "version";
    if (table == "avatar_purchases") return "id";
    if (table == "avatar_loadout_events") return "id";
    if (table == "achievement_activation") return "singleton";
    if (table == "admin_wallet_adjustments" || table == "admin_redemption_cancellations" || table == "account_login_events") return "id";
    return table == "task_change_decisions" ? "proposal_id" : "task_id";
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string keyOf(const std::string &kind,
                  const std::string &subject,
                  const std::string &object,
                  const std::string &column,
                  const std::string &privilege) {
    std::string key = kind + "/" + subject + "/";
    if (kind != "identity") {
        key += object;
        if (!column.empty()) {
            key += "." + column;
        }
        key += "/";
    }
    key += privilege;
    return key;
}

std::string keyOf(const WalletPrivilegeRow &row) {
    return keyOf(row.kind, row.subject, row.object, row.column, row.privilege);
}

std::set<std::string> requiredKeys() {
    std::set<std::string> keys;
    keys.insert(keyOf("identity", "current", "", "", "is_app_role"));
    keys.insert(keyOf("identity", "current", "", "", "rolsuper"));
    keys.insert(keyOf("identity", "current", "", "", "has_role_memberships"));
    keys.insert(keyOf("identity", "usapp_push", "", "", "role_exists"));
    keys.insert(keyOf("identity", "usapp_memory_eraser", "", "", "no_violations"));
    keys.insert(keyOf("identity", "timestamp_guards", "", "", "no_column_defaults"));
    for (const auto &table : TaskPrivilegePolicy::taskTables()) {
        keys.insert(keyOf("table", "current", table, "", "exists"));
        keys.insert(keyOf("table", "current", table, "", "owned_by_current_user"));
        for (const auto &subject : WalletPrivilegePolicy::subjects()) {
            for (const auto &privilege : WalletPrivilegePolicy::tablePrivileges()) {
                keys.insert(keyOf("table_privilege", subject, table, "", privilege));
            }
            for (const auto &privilege : WalletPrivilegePolicy::anyColumnPrivileges()) {
                keys.insert(keyOf("any_column_privilege", subject, table, "", privilege));
            }
            keys.insert(keyOf("column_privilege", subject, table, requiredColumnFor(table), "UPDATE"));
        }
    }
    return keys;
}

std::string describe(const WalletPrivilegeCheck &check) {
    const auto &row = check.row;
    std::ostringstream os;
    os << WalletPrivilegePolicy::safeName(row.kind) << " " << WalletPrivilegePolicy::safeName(row.subject);
    if (row.kind != "identity") {
        os << " " << WalletPrivilegePolicy::safeName(row.object);
        if (!row.column.empty()) {
            os << "." << WalletPrivilegePolicy::safeName(row.column);
        }
    }
    os << " " << WalletPrivilegePolicy::safeName(row.privilege) << " expected="
       << (check.expected ? "true" : "false") << " actual=";
    if (row.actual.has_value()) {
        os << (*row.actual ? "true" : "false");
    } else {
        os << "null";
    }
    return os.str();
}

}  // namespace

const std::vector<std::string> &TaskPrivilegePolicy::taskTables() {
    static const std::vector<std::string> tables = {"tasks", "task_completions", "task_progress_events", "task_read_model",
                                                    "task_change_proposals", "task_change_decisions", "task_closures",
                                                    // M7: append-only reward products share the same SELECT/INSERT matrix.
                                                    "reward_products", "reward_product_revisions",
                                                    // M8: append-only redemptions and fulfillments.
                                                    "reward_redemptions", "reward_redemption_fulfillments", "reward_redemption_events", "reward_redemption_state",
                                                    // P2-1: append-only messages and replies.
                                                    "messages", "message_replies",
                                                    // P2-2: append-only anniversaries, proposals, decisions and pins.
                                                    "anniversaries", "anniversary_revisions", "anniversary_edit_proposals",
                                                    "anniversary_edit_decisions", "anniversary_home_pins", "notifications", "notification_reads",
                                                    // P2-3b: append-only notification intents.
                                                    "notification_intents", "task_reactions",
                                                    // P2-5a: append-only push subscription lifecycle.
                                                    "push_subscriptions", "push_subscription_revocations",
                                                    "push_registration_commands",
                                                    // P2-5b: append-only dispatch facts and control log.
                                                    "push_dispatch_control", "push_deliveries", "push_attempts",
                                                    "push_attempt_observations", "push_attempt_expiries",
                                                    "push_delivery_outcomes",
                                                    // P2-6: append-only reminder rules and occurrences.
                                                    "reminder_rules", "reminder_occurrences",
                                                    // P3-1a: append-only media facts.
                                                    "media_objects", "media_promotion_attempts", "media_upload_outcomes",
                                                    "media_cleanup_plans", "media_cleanup_passes", "media_bindings",
                                                    "media_late_objects",
                                                    // P3-1b: append-only photo memories.
                                                    "memories", "memory_texts", "memory_revisions", "memory_revision_photos",
                                                    "memory_edit_proposals", "memory_proposal_photos", "memory_edit_decisions",
                                                    // P3-2a: append-only shared albums.
                                                    "albums", "album_texts", "album_revisions", "album_photo_batches",
                                                    "album_items", "album_item_removals", "album_cover_events",
                                                    // P3-2b: append-only album rename requests.
                                                    "album_edit_proposals", "album_edit_decisions", "shared_plans", "shared_plan_events", "shared_plan_memory_links",
                                                    // D6: append-only media deletion cycles and reversals.
                                                    "media_deletions", "media_deletion_reversals",
                                                    // D6 M1a: append-only memory deletion cycles and reversals.
                                                    "memory_deletions", "memory_deletion_reversals",
                                                    // D6 M1b: append-only memory delete requests and decisions.
                                                    "memory_delete_requests", "memory_delete_decisions",
                                                    // D6 M1c: append-only links from a memory cycle to its media cycles.
                                                    "memory_deletion_photo_cycles",
                                                    // D6 M1d: read-only to the application; only usapp_memory_erase writes it.
                                                    "memory_purges", "deletion_recovery_attempts", "deletion_recovery_manifests",
                                                    "deletion_recovery_quarantine", "deletion_recovery_verifications",
                                                    // Phase 4 B1 (0044): the asset registry is read-only; the catalog is append-only.
                                                    "avatar_assets", "avatar_items", "avatar_item_revisions", "avatar_starter_configs",
                                                    // Phase 4 B2 (0045): append-only purchases, also the ownership record.
                                                    "avatar_purchases",
                                                    // Phase 4 B3 (0046): append-only saved loadouts.
                                                    "avatar_loadout_events", "achievement_activation", "admin_wallet_adjustments", "admin_redemption_cancellations", "account_login_events"};
    return tables;
}

const char *TaskPrivilegePolicy::matrixSql() {
    return kTaskMatrixSql;
}

std::optional<bool> TaskPrivilegePolicy::expectedFor(const WalletPrivilegeRow &row) {
    if (row.kind == "identity") {
        if (row.subject == "current") {
            if (row.privilege == "is_app_role") return true;
            if (row.privilege == "rolsuper") return false;
            if (row.privilege == "has_role_memberships") return false;
        }
        if (row.subject == "usapp_push" && row.privilege == "role_exists") return true;
        // D6 M1d: the validator's own answer. A violation row has no expected value, so it fails startup by name.
        if (row.subject == "usapp_memory_eraser" && row.privilege == "no_violations") return true;
        // 0041: the guards assign these times, so a default left on a column means the migration is absent.
        if (row.subject == "timestamp_guards" && row.privilege == "no_column_defaults") return true;
        // 0043: the three D20 notification sources. A build that writes them must not start on an older schema.
        if (row.subject == "phase3_notifications" && row.privilege == "sources_present") return true;
        return std::nullopt;
    }

    if (!contains(taskTables(), row.object)) {
        return std::nullopt;
    }

    if (row.kind == "table") {
        if (row.subject != "current") return std::nullopt;
        if (row.privilege == "exists") return true;
        if (row.privilege == "owned_by_current_user") return false;
        return std::nullopt;
    }

    if (!contains(WalletPrivilegePolicy::subjects(), row.subject)) {
        return std::nullopt;
    }
    const bool isApp = row.subject == "current";

    const bool readOnly = row.object == "task_read_model" || row.object == "reward_redemption_state" || row.object == "memory_purges" || row.object.rfind("deletion_recovery_", 0) == 0 ||
                          row.object == "avatar_assets" || row.object == "achievement_activation";

    if (row.kind == "table_privilege") {
        if (!contains(WalletPrivilegePolicy::tablePrivileges(), row.privilege)) return std::nullopt;
        return isApp && (row.privilege == "SELECT" || (row.privilege == "INSERT" && !readOnly));
    }

    if (row.kind == "any_column_privilege") {
        if (!contains(WalletPrivilegePolicy::anyColumnPrivileges(), row.privilege)) return std::nullopt;
        return isApp && (row.privilege == "SELECT" || (row.privilege == "INSERT" && !readOnly));
    }

    if (row.kind == "column_privilege") {
        if (row.privilege != "UPDATE" || row.column.empty()) return std::nullopt;
        return false;  // 任务表没有任何可更新的列
    }

    return std::nullopt;
}

WalletPrivilegeReport TaskPrivilegePolicy::evaluate(const std::vector<WalletPrivilegeRow> &rows) {
    WalletPrivilegeReport report;
    std::set<std::string> seen;
    for (const auto &row : rows) {
        if (row.kind == "identity" && row.privilege == "is_app_role") {
            report.currentRole = WalletPrivilegePolicy::safeName(row.object);
        }
        const std::optional<bool> expected = expectedFor(row);
        if (!expected.has_value()) {
            report.unexpected.push_back(WalletPrivilegePolicy::safeName(keyOf(row)));
            continue;
        }
        seen.insert(keyOf(row));
        WalletPrivilegeCheck check;
        check.row = row;
        check.expected = *expected;
        report.checks.push_back(std::move(check));
    }
    for (const auto &key : requiredKeys()) {
        if (seen.find(key) == seen.end()) {
            report.missing.push_back(WalletPrivilegePolicy::safeName(key));
        }
    }
    return report;
}

std::string TaskPrivilegePolicy::summary(const WalletPrivilegeReport &report) {
    const auto bad = report.mismatches();
    std::ostringstream os;
    os << "task privilege policy: role=" << (report.currentRole.empty() ? "?" : report.currentRole)
       << " ok=" << (report.ok() ? "true" : "false") << " checks=" << report.checks.size()
       << " mismatches=" << bad.size() << " missing=" << report.missing.size()
       << " unexpected=" << report.unexpected.size();
    for (const auto &check : bad) {
        os << "\n  mismatch: " << describe(check);
    }
    for (const auto &key : report.missing) {
        os << "\n  missing: " << key;
    }
    for (const auto &key : report.unexpected) {
        os << "\n  unexpected: " << key;
    }
    return os.str();
}

void TaskPrivilegePolicy::check(const ClientPtr &client,
                                WalletPrivilegePolicy::ReportCallback onReport,
                                WalletPrivilegePolicy::ErrorCallback onError) {
    if (!client) {
        onError("任务权限矩阵检查失败：数据库客户端未初始化");
        return;
    }
    client->execSqlAsync(
        kTaskMatrixSql,
        [onReport](const drogon::orm::Result &result) {
            onReport(evaluate(WalletPrivilegePolicy::rowsFromResult(result)));
        },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(describePgError("任务权限矩阵查询失败", e.base()));
        });
}

}  // namespace usapp::db
