// =====================================================================
// services/api/src/services/ExportService.cc
// E1 structured user-data export. Design: docs/user-data-export-design.md Sections 4-7.
// =====================================================================
#include "services/ExportService.h"

#include "core/Crypto.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>


namespace usapp {

namespace {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ SQL builder
std::string ts(const std::string &e) { return "to_char((" + e + ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"; }
// A text sort key equal to ORDER BY (time, id...): fixed-width UTC time, then the identifiers.
std::string tk(const std::string &e) { return "to_char((" + e + ") AT TIME ZONE 'UTC', 'YYYYMMDDHH24MISSUS')"; }
std::string pad(const std::string &e) { return "lpad((" + e + ")::text, 19, '0')"; }

// One capped section: the inner query passes base rows only (ORDER BY ... LIMIT cap + 1), the outer builds each row's JSON,
// and the CTE records each row's serialized bytes. Nothing is aggregated into an array here.
std::string section(const std::string &name, const std::string &key, const std::string &json, const std::string &inner,
                    const std::string &joins = "") {
    return name + " AS MATERIALIZED (SELECT y.k, y.j, octet_length(y.j::text)::bigint AS b FROM (SELECT " + key + " AS k, " + json +
           " AS j FROM (" + inner + " LIMIT $2::integer + 1) s " + joins + ") y)";
}

// The one photo-reference predicate (design 6.3), mirroring MediaService::visibleTo for the caller ($1).
const std::string kVisible =
    "NOT usapp_media_deleted_now(o.id) AND (o.uploader_id = $1::uuid OR ((usapp_media_shared_now(o.id) OR usapp_media_pending_reference(o.id) IS NOT NULL) "
    "AND t.outcome = 'ready' AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans cp WHERE cp.media_id = o.id AND cp.scope = 'everything')))";

std::string photoRef() {
    return "(SELECT json_build_object('media_id', o.id, 'uploader_id', o.uploader_id, 'width', t.width, 'height', t.height) "
           "FROM media_objects o JOIN media_upload_outcomes t ON t.media_id = o.id WHERE o.id = ph.media_id AND " + kVisible + ")";
}

// Ordered photo positions; an invisible photo keeps its position as a withheld marker without any id.
std::string photos(const std::string &table, const std::string &where) {
    return "(SELECT COALESCE(json_agg(CASE WHEN v.ref IS NULL THEN json_build_object('position', ph.position, 'photo', NULL, 'withheld', 'unavailable') "
           "ELSE json_build_object('position', ph.position, 'photo', v.ref) END ORDER BY ph.position), '[]'::json) FROM " + table +
           " ph CROSS JOIN LATERAL (SELECT " + photoRef() + " AS ref) v WHERE " + where + ")";
}

std::string decision(const std::string &table, const std::string &extra = "") {
    return "(SELECT json_build_object('decided_by', d.decided_by, 'outcome', d.outcome" + extra + ", 'decided_at', " + ts("d.decided_at") +
           ") FROM " + table + " d WHERE d.proposal_id = s.id)";
}

// Plan bodies are rebuilt from the five validated keys only (0033).
std::string planBody(const std::string &e) {
    return "CASE WHEN " + e + " IS NULL THEN NULL ELSE json_build_object('title', " + e + "->'title', 'date', " + e + "->'date', 'time', " + e +
           "->'time', 'place', " + e + "->'place', 'note', " + e + "->'note') END";
}

std::string buildSql() {
    const std::string kc = tk("s.created_at") + " || s.id::text";
    std::vector<std::pair<std::string, std::string>> sections;  // name, CTE
    auto add = [&](const std::string &name, const std::string &cte) { sections.emplace_back(name, cte); };

    add("tasks", section("s_tasks", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'assignee_id', s.assignee_id, 'title', s.title, 'description', s.description, "
        "'reward_flowers', s.reward_flowers, 'reward_sunshine', s.reward_sunshine, 'timezone', s.timezone, 'deadline_local_date', s.deadline_local_date, "
        "'deadline_local_time', s.deadline_local_time, 'deadline_at', " + ts("s.deadline_at") + ", 'completion_mode', s.completion_mode, "
        "'target_count', s.target_count, 'schedule_type', s.schedule_type, 'recurrence_id', s.recurrence_id, 'period_date', s.period_date, "
        "'start_local_date', s.start_local_date, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM tasks x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("task_completions", section("s_completions", tk("s.completed_at") + " || s.task_id::text",
        "json_build_object('task_id', s.task_id, 'completed_by', s.completed_by, 'reward_flowers', s.reward_flowers, 'reward_sunshine', s.reward_sunshine, "
        "'completed_at', " + ts("s.completed_at") + ")",
        "SELECT x.* FROM task_completions x JOIN me ON x.couple_id = me.cid ORDER BY x.completed_at, x.task_id"));
    add("task_progress", section("s_progress", tk("s.recorded_at") + " || s.id::text",
        "json_build_object('id', s.id, 'task_id', s.task_id, 'user_id', s.user_id, 'progress_count', s.progress_count, 'target_count', s.target_count, "
        "'recorded_at', " + ts("s.recorded_at") + ")",
        "SELECT x.* FROM task_progress_events x JOIN me ON x.couple_id = me.cid ORDER BY x.recorded_at, x.id"));
    add("task_changes", section("s_changes", kc,
        "json_build_object('id', s.id, 'task_id', s.task_id, 'proposer_id', s.proposer_id, 'kind', s.kind, 'title', s.title, 'description', s.description, "
        "'reward_flowers', s.reward_flowers, 'reward_sunshine', s.reward_sunshine, 'completion_mode', s.completion_mode, 'target_count', s.target_count, "
        "'deadline_local_date', s.deadline_local_date, 'deadline_local_time', s.deadline_local_time, 'start_local_date', s.start_local_date, "
        "'created_at', " + ts("s.created_at") + ", 'decision', " + decision("task_change_decisions", ", 'successor_task_id', d.successor_task_id") + ")",
        "SELECT x.* FROM task_change_proposals x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("task_closures", section("s_closures", tk("s.closed_at") + " || s.task_id::text",
        "json_build_object('task_id', s.task_id, 'reason', s.reason, 'proposal_id', s.proposal_id, 'closed_at', " + ts("s.closed_at") + ")",
        "SELECT x.* FROM task_closures x JOIN me ON x.couple_id = me.cid ORDER BY x.closed_at, x.task_id"));
    add("task_reactions", section("s_reactions", kc,
        "json_build_object('id', s.id, 'completion_task_id', s.completion_task_id, 'author_id', s.author_id, 'kind', s.kind, 'emoji_code', s.emoji_code, "
        "'body', s.body, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM task_reactions x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("reminder_rules", section("s_reminders", kc,
        "json_build_object('id', s.id, 'task_id', s.task_id, 'anniversary_id', s.anniversary_id, 'seq', s.seq, 'enabled', s.enabled, 'mode', s.mode, "
        "'before_minutes', s.before_minutes, 'days_before', s.days_before, 'at_time', s.at_time, 'at_local_date', s.at_local_date, "
        "'days_before_set', s.days_before_set, 'origin', s.origin, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM reminder_rules x JOIN me ON x.couple_id = me.cid AND x.owner_id = me.uid ORDER BY x.created_at, x.id"));
    add("wallet_ledger", section("s_ledger", "s.asset_type || " + pad("s.seq"),
        "json_build_object('id', s.id, 'asset_type', s.asset_type, 'seq', s.seq, 'amount', s.amount, 'balance_after', s.balance_after, "
        "'created_at', " + ts("s.created_at") + ", 'operation_id', s.operation_id, 'operation_type', o.operation_type, 'source_type', o.source_type, "
        "'source_id', o.source_id)",
        "SELECT x.* FROM wallet_ledger_entries x JOIN me ON x.user_id = me.uid ORDER BY x.asset_type, x.seq",
        "JOIN wallet_operations o ON o.id = s.operation_id"));
    // D14: the creator sees every product; the owner only active ones (latest revision).
    const std::string visibleProduct =
        "(p.creator_id = $1::uuid OR (p.owner_id = $1::uuid AND (SELECT r.status FROM reward_product_revisions r WHERE r.product_id = p.id "
        "ORDER BY r.version DESC LIMIT 1) = 'active'))";
    add("reward_products", section("s_products", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'owner_id', s.owner_id, 'created_at', " + ts("s.created_at") + ")",
        "SELECT p.* FROM reward_products p JOIN me ON p.couple_id = me.cid WHERE " + visibleProduct + " ORDER BY p.created_at, p.id"));
    add("reward_product_revisions", section("s_product_revisions", tk("s.created_at") + " || s.product_id::text || " + pad("s.version"),
        "json_build_object('product_id', s.product_id, 'version', s.version, 'actor_id', s.actor_id, 'action', s.action, 'title', s.title, "
        "'description', s.description, 'price_flowers', s.price_flowers, 'redemption_mode', s.redemption_mode, 'status', s.status, "
        "'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM reward_product_revisions x JOIN me ON x.couple_id = me.cid JOIN reward_products p ON p.id = x.product_id WHERE " + visibleProduct +
        " ORDER BY x.created_at, x.product_id, x.version"));
    const std::string party = "(x.creator_id = me.uid OR x.redeemer_id = me.uid)";
    add("reward_redemptions", section("s_redemptions", kc,
        "json_build_object('id', s.id, 'product_id', s.product_id, 'product_version', s.product_version, 'creator_id', s.creator_id, "
        "'redeemer_id', s.redeemer_id, 'title', s.title, 'description', s.description, 'price_flowers', s.price_flowers, "
        "'redemption_mode', s.redemption_mode, 'created_at', " + ts("s.created_at") + ", 'fulfillment', (SELECT json_build_object('fulfilled_by', "
        "f.fulfilled_by, 'fulfilled_at', " + ts("f.fulfilled_at") + ") FROM reward_redemption_fulfillments f WHERE f.redemption_id = s.id))",
        "SELECT x.* FROM reward_redemptions x JOIN me ON x.couple_id = me.cid WHERE " + party + " ORDER BY x.created_at, x.id"));
    add("reward_redemption_events", section("s_redemption_events", kc,
        "json_build_object('id', s.id, 'redemption_id', s.redemption_id, 'actor_id', s.actor_id, 'action', s.action, 'proposal_id', s.proposal_id, "
        "'created_at', " + ts("s.created_at") + ")",
        "SELECT e.* FROM reward_redemption_events e JOIN me ON e.couple_id = me.cid JOIN reward_redemptions x ON x.id = e.redemption_id WHERE " + party +
        " ORDER BY e.created_at, e.id"));
    add("messages", section("s_messages", kc,
        "json_build_object('id', s.id, 'author_id', s.author_id, 'recipient_id', s.recipient_id, 'body', s.body, 'created_at', " + ts("s.created_at") +
        ", 'reply', (SELECT json_build_object('author_id', r.author_id, 'body', r.body, 'created_at', " + ts("r.created_at") +
        ") FROM message_replies r WHERE r.message_id = s.id))",
        "SELECT x.* FROM messages x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("anniversaries", section("s_anniversaries", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM anniversaries x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("anniversary_revisions", section("s_anniversary_revisions", tk("s.created_at") + " || s.anniversary_id::text || " + pad("s.version"),
        "json_build_object('anniversary_id', s.anniversary_id, 'version', s.version, 'actor_id', s.actor_id, 'action', s.action, "
        "'proposal_id', s.proposal_id, 'title', s.title, 'event_date', s.event_date, 'kind', s.kind, 'note', s.note, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM anniversary_revisions x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.anniversary_id, x.version"));
    add("anniversary_proposals", section("s_anniversary_proposals", kc,
        "json_build_object('id', s.id, 'anniversary_id', s.anniversary_id, 'proposer_id', s.proposer_id, 'base_version', s.base_version, "
        "'title', s.title, 'event_date', s.event_date, 'kind', s.kind, 'note', s.note, 'created_at', " + ts("s.created_at") +
        ", 'decision', " + decision("anniversary_edit_decisions") + ")",
        "SELECT x.* FROM anniversary_edit_proposals x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    // Q2: every memory section is restricted to memories that are not deleted now (which is also true of every memory under a hold).
    add("memories", section("s_memories", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM memories x JOIN me ON x.couple_id = me.cid WHERE NOT usapp_memory_deleted_now(x.id) ORDER BY x.created_at, x.id"));
    add("memory_revisions", section("s_memory_revisions", tk("s.created_at") + " || s.memory_id::text || " + pad("s.version"),
        "json_build_object('memory_id', s.memory_id, 'version', s.version, 'actor_id', s.actor_id, 'action', s.action, 'proposal_id', s.proposal_id, "
        "'memory_date', s.memory_date, 'title', mt.title, 'description', mt.description, 'created_at', " + ts("s.created_at") + ", 'photos', " +
        photos("memory_revision_photos", "ph.memory_id = s.memory_id AND ph.version = s.version") + ")",
        "SELECT x.* FROM memory_revisions x JOIN me ON x.couple_id = me.cid WHERE NOT usapp_memory_deleted_now(x.memory_id) "
        "ORDER BY x.created_at, x.memory_id, x.version",
        "JOIN memory_texts mt ON mt.id = s.text_id"));
    add("memory_proposals", section("s_memory_proposals", kc,
        "json_build_object('id', s.id, 'memory_id', s.memory_id, 'proposer_id', s.proposer_id, 'base_version', s.base_version, "
        "'memory_date', s.memory_date, 'title', mt.title, 'description', mt.description, 'created_at', " + ts("s.created_at") + ", 'photos', " +
        photos("memory_proposal_photos", "ph.proposal_id = s.id") + ", 'decision', " + decision("memory_edit_decisions") + ")",
        "SELECT x.* FROM memory_edit_proposals x JOIN me ON x.couple_id = me.cid WHERE NOT usapp_memory_deleted_now(x.memory_id) "
        "ORDER BY x.created_at, x.id",
        "JOIN memory_texts mt ON mt.id = s.text_id"));
    add("albums", section("s_albums", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM albums x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id"));
    add("album_revisions", section("s_album_revisions", tk("s.created_at") + " || s.album_id::text || " + pad("s.version"),
        "json_build_object('album_id', s.album_id, 'version', s.version, 'actor_id', s.actor_id, 'action', s.action, 'proposal_id', s.proposal_id, "
        "'name', at.name, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM album_revisions x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.album_id, x.version",
        "JOIN album_texts at ON at.id = s.text_id"));
    add("album_proposals", section("s_album_proposals", kc,
        "json_build_object('id', s.id, 'album_id', s.album_id, 'proposer_id', s.proposer_id, 'base_version', s.base_version, 'name', at.name, "
        "'created_at', " + ts("s.created_at") + ", 'decision', " + decision("album_edit_decisions") + ")",
        "SELECT x.* FROM album_edit_proposals x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id",
        "JOIN album_texts at ON at.id = s.text_id"));
    // Active items only (as the album API): not removed, not deleted, and visible to the caller; others are omitted.
    add("album_items", section("s_album_items", tk("s.created_at") + " || s.album_id::text || " + pad("s.seq"),
        "json_build_object('album_id', s.album_id, 'seq', s.seq, 'added_by', s.added_by, 'created_at', " + ts("s.created_at") +
        ", 'photo', json_build_object('media_id', s.media_id, 'uploader_id', s.uploader_id, 'width', s.width, 'height', s.height))",
        "SELECT i.album_id, i.seq, i.added_by, i.created_at, i.media_id, o.uploader_id, t.width, t.height FROM album_items i JOIN me ON i.couple_id = me.cid "
        "JOIN media_objects o ON o.id = i.media_id JOIN media_upload_outcomes t ON t.media_id = o.id "
        "WHERE NOT EXISTS (SELECT 1 FROM album_item_removals rm WHERE rm.album_id = i.album_id AND rm.media_id = i.media_id) AND " + kVisible +
        " ORDER BY i.created_at, i.album_id, i.seq"));
    add("shared_plans", section("s_plans", kc,
        "json_build_object('id', s.id, 'creator_id', s.creator_id, 'created_at', " + ts("s.created_at") + ", 'body', " + planBody("b.body") +
        ", 'body_version', b.body_version, 'status', st.status, 'status_version', st.status_version, 'memory_link', "
        "CASE WHEN (SELECT closed FROM hold) THEN json_build_object('memory_id', NULL, 'withheld', 'unavailable') "
        "WHEN l.plan_id IS NULL THEN NULL "
        "WHEN usapp_memory_deleted_now(l.memory_id) THEN json_build_object('mode', l.mode, 'memory_id', NULL, 'withheld', 'unavailable') "
        "ELSE json_build_object('mode', l.mode, 'memory_id', l.memory_id, 'created_at', " + ts("l.created_at") + ") END)",
        "SELECT x.* FROM shared_plans x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.id",
        "LEFT JOIN LATERAL (SELECT e.body, e.body_version FROM shared_plan_events e WHERE e.plan_id = s.id AND e.body_version IS NOT NULL "
        "ORDER BY e.body_version DESC LIMIT 1) b ON true "
        "LEFT JOIN LATERAL (SELECT e.status, e.status_version FROM shared_plan_events e WHERE e.plan_id = s.id AND e.status_version IS NOT NULL "
        "ORDER BY e.status_version DESC LIMIT 1) st ON true "
        "LEFT JOIN shared_plan_memory_links l ON l.plan_id = s.id"));
    add("shared_plan_events", section("s_plan_events", tk("s.created_at") + " || s.plan_id::text || " + pad("s.seq"),
        "json_build_object('id', s.id, 'plan_id', s.plan_id, 'seq', s.seq, 'actor_id', s.actor_id, 'operation', s.operation, "
        "'body_version', s.body_version, 'status_version', s.status_version, 'status', s.status, 'body', " + planBody("s.body") +
        ", 'expected_version', CASE WHEN jsonb_typeof(s.request->'expected_version') = 'number' THEN (s.request->>'expected_version')::integer END, "
        "'proposal_id', s.proposal_id, 'outcome', s.outcome, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM shared_plan_events x JOIN me ON x.couple_id = me.cid ORDER BY x.created_at, x.plan_id, x.seq"));
    add("avatar_purchases", section("s_purchases", kc,
        "json_build_object('id', s.id, 'item_id', s.item_id, 'item_version', s.item_version, 'name', s.name, 'category', s.category, "
        "'asset_key', s.asset_key, 'price_sunshine', s.price_sunshine, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM avatar_purchases x JOIN me ON x.user_id = me.uid ORDER BY x.created_at, x.id"));
    add("avatar_loadouts", section("s_loadouts", pad("s.version"),
        "json_build_object('version', s.version, 'slots', json_build_object('hair', s.hair_item_id, 'clothes', s.clothes_item_id, "
        "'expression', s.expression_item_id, 'hat', s.hat_item_id, 'handheld', s.handheld_item_id, 'accessory', s.accessory_item_id), "
        "'requested', s.requested, 'starter_version', s.starter_version, 'created_at', " + ts("s.created_at") + ")",
        "SELECT x.* FROM avatar_loadout_events x JOIN me ON x.user_id = me.uid ORDER BY x.version"));

    std::string sql = "WITH me AS MATERIALIZED (SELECT m.user_id AS uid, m.couple_id AS cid FROM couple_members m WHERE m.user_id = $1::uuid), "
                      "hold AS MATERIALIZED (SELECT usapp_recovery_closed() AS closed)";
    for (const auto &[name, cte] : sections) sql += ",\n" + cte;
    sql += ",\ncounts AS MATERIALIZED (";
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto cteName = sections[i].second.substr(0, sections[i].second.find(' '));
        if (i) sql += " UNION ALL ";
        sql += "SELECT '" + sections[i].first + "'::text AS name, count(*)::bigint AS n, COALESCE(sum(b), 0)::bigint AS b FROM " + cteName;
    }
    sql += "),\nverdict AS MATERIALIZED (SELECT c.*, CASE WHEN c.members = 0 THEN 'no_space' WHEN c.over_sections IS NOT NULL THEN 'section_rows' "
           "WHEN c.row_total > $3::bigint THEN 'total_rows' WHEN c.byte_total > $4::bigint THEN 'input_bytes' ELSE 'ok' END AS status FROM (SELECT "
           "(SELECT count(*) FROM me) AS members, (SELECT string_agg(name, ',' ORDER BY name) FROM counts WHERE n > $2::integer) AS over_sections, "
           "(SELECT sum(n) FROM counts)::bigint AS row_total, (SELECT sum(b) FROM counts)::bigint AS byte_total) c)\n";

    auto agg = [](const std::string &cte) { return "(SELECT COALESCE(json_agg(j ORDER BY k), '[]'::json) FROM " + cte + ")"; };
    auto ofName = [&](const std::string &name) {
        for (const auto &[n, cte] : sections) if (n == name) return cte.substr(0, cte.find(' '));
        return std::string();
    };
    auto held = [&](const std::string &name) {
        return "json_build_object('state', CASE WHEN h.closed THEN 'unavailable' ELSE 'available' END, 'items', " + agg(ofName(name)) + ")";
    };
    const std::string doc =
        "json_build_object("
        "'export', json_build_object('format', 'usapp-user-export', 'schema_version', 1, 'generated_at', " + ts("statement_timestamp()") +
        ", 'subject_user_id', me.uid, 'scope', 'own_and_shared_space', 'photos_included', false, 'complete', true), "
        "'account', (SELECT json_build_object('id', u.id, 'email', u.email_display, 'display_name', u.display_name, 'status', u.status, "
        "'created_at', " + ts("u.created_at") + ") FROM users u WHERE u.id = me.uid), "
        "'space', (SELECT json_build_object('id', cp.id, 'name', cp.name, 'timezone', cp.timezone, 'created_by_user_id', cp.created_by_user_id, "
        "'created_at', " + ts("cp.created_at") + ", 'members', (SELECT json_agg(json_build_object('user_id', cm.user_id, 'slot', cm.slot, "
        "'display_name', mu.display_name, 'joined_at', " + ts("cm.joined_at") + ") ORDER BY cm.slot) FROM couple_members cm JOIN users mu ON mu.id = cm.user_id "
        "WHERE cm.couple_id = cp.id)) FROM couples cp WHERE cp.id = me.cid), "
        "'tasks', " + agg("s_tasks") + ", 'task_completions', " + agg("s_completions") + ", 'task_progress', " + agg("s_progress") +
        ", 'task_changes', " + agg("s_changes") + ", 'task_closures', " + agg("s_closures") + ", 'task_reactions', " + agg("s_reactions") +
        ", 'reminder_rules', " + agg("s_reminders") +
        ", 'wallet', json_build_object('balances', json_build_object("
        "'flower', COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e WHERE e.user_id = me.uid AND e.asset_type = 'flower' ORDER BY e.seq DESC LIMIT 1), 0), "
        "'sunshine', COALESCE((SELECT e.balance_after FROM wallet_ledger_entries e WHERE e.user_id = me.uid AND e.asset_type = 'sunshine' ORDER BY e.seq DESC LIMIT 1), 0)), "
        "'ledger', " + agg("s_ledger") + "), "
        "'reward_products', " + agg("s_products") + ", 'reward_product_revisions', " + agg("s_product_revisions") +
        ", 'reward_redemptions', " + agg("s_redemptions") + ", 'reward_redemption_events', " + agg("s_redemption_events") +
        ", 'messages', " + agg("s_messages") +
        ", 'anniversaries', " + agg("s_anniversaries") + ", 'anniversary_revisions', " + agg("s_anniversary_revisions") +
        ", 'anniversary_proposals', " + agg("s_anniversary_proposals") +
        ", 'anniversary_home_pin', (SELECT json_build_object('anniversary_id', hp.anniversary_id, 'seq', hp.seq, 'created_at', " + ts("hp.created_at") +
        ") FROM anniversary_home_pins hp WHERE hp.user_id = me.uid ORDER BY hp.seq DESC LIMIT 1), "
        "'memories', " + held("memories") + ", 'memory_revisions', " + agg("s_memory_revisions") + ", 'memory_proposals', " + agg("s_memory_proposals") +
        ", 'albums', " + held("albums") + ", 'album_revisions', " + agg("s_album_revisions") + ", 'album_proposals', " + agg("s_album_proposals") +
        ", 'album_items', " + agg("s_album_items") +
        ", 'shared_plans', " + agg("s_plans") + ", 'shared_plan_events', " + agg("s_plan_events") +
        ", 'avatar_purchases', " + agg("s_purchases") + ", 'avatar_loadouts', " + agg("s_loadouts") +
        ", 'achievements', (SELECT (usapp_achievement_facts(ARRAY(SELECT c.completed_at FROM task_completions c WHERE c.couple_id = me.cid "
        "AND c.completed_by = me.uid AND c.completed_at >= a.enabled_at), a.enabled_at) || jsonb_build_object('derived', true, 'timezone', 'Asia/Shanghai', 'enabled', "
        "a.enabled_at IS NOT NULL, 'enabled_at', " + ts("a.enabled_at") + "))::json FROM (SELECT 1) one "
        "LEFT JOIN achievement_activation a ON a.singleton))";
    // The gate. The document is built once, in a materialized CTE, inside a CASE branch taken only when the verdict is 'ok':
    // PostgreSQL evaluates only the needed CASE branch, so no section array aggregation runs on refusal. (A lateral WHERE alone was
    // planned as a join filter and the document expression was duplicated for octet_length, runs usapp-e1-5f800e9659/-b50fdcd440.)
    sql += ",\ngated AS MATERIALIZED (SELECT CASE WHEN (SELECT status FROM verdict) = 'ok' THEN (" + doc + ")::text END AS doc, "
           "to_char(statement_timestamp() AT TIME ZONE 'UTC', 'YYYYMMDD\"T\"HH24MISS\"Z\"') AS stamp FROM me, hold h)\n";
    // session_tag / session_ro let the caller confirm this ran on the tagged session, inside the read-only transaction holding the lock.
    sql += "SELECT v.status, v.over_sections, v.row_total, v.byte_total, g.doc, octet_length(g.doc)::bigint AS doc_bytes, g.stamp, "
           "pg_backend_pid() AS session_pid, current_setting('transaction_read_only') AS session_ro, "
           "current_setting('application_name') AS session_tag "
           "FROM verdict v LEFT JOIN gated g ON true";
    return sql;
}

// ------------------------------------------------------------------ admission and dedicated sessions (process-local)
//
// Each admission slot owns one long-lived single-connection Drogon client: a dedicated PostgreSQL session on which this service
// issues its statements strictly one after another. Drogon reconnects a broken connection by itself, so a later statement may
// run on a NEW session; nothing about the old one can be learned from the client.
//
// Identity. Before BEGIN, each export tags its slot session with a fresh random token as application_name (session-level SET).
// Every later statement checks it still runs on the tagged session (and, after BEGIN, inside the read-only transaction). A lost
// BEGIN reply therefore still leaves the original session findable by its token. The captured backend pid is diagnostic only:
// pids are reused, and no decision depends on one.
//
// End. The export ends with this service's own ROLLBACK. The slot is released only when a verification on the slot's session
// shows: this session is outside any transaction, no OTHER backend carrying the token is in any state but idle, and no backend
// carrying the token holds the 33026 lock. Otherwise the slot stays held ("unconfirmed") and verification keeps polling, never
// blocking a thread. This service never terminates any backend. Clients are never destroyed (see init()).
struct Slot {
    int index = 0;
    drogon::orm::DbClientPtr client;
    bool busy = false;
    std::string state = "idle";  // idle, working, end_requested, unconfirmed (diagnostics)
};

std::mutex gMutex;
ExportService::Limits gLimits;
// Deliberately never freed: dedicated clients must not be destroyed at runtime (where a last reference could be dropped on the
// client's own event-loop thread) nor during static destruction at exit (docs: the DbClient teardown crash record).
std::vector<std::shared_ptr<Slot>> *gSlots = new std::vector<std::shared_ptr<Slot>>();
std::set<std::string> gActive;
std::map<std::string, Clock::time_point> gLastAdmission;
std::function<void(std::function<void()>)> gAfterLock;
std::function<void(std::function<void()>)> gBeforeEnd;
std::function<void(const ExportService::Release &)> gReleaseObserver;
std::function<void(const std::string &)> gTokenObserver;
int gVerifyFaults = 0;
bool gBeginReplyLoss = false;
bool gSilentBeginReplyLoss = false;
bool gGenerationReplyLoss = false;
int gPidOverride = 0;

trantor::EventLoop *timerLoop() {
    static trantor::EventLoopThread thread("export-deadline");
    static std::once_flag once;
    std::call_once(once, [] { thread.run(); });
    return thread.getLoop();
}

void setSlotState(const std::shared_ptr<Slot> &slot, const char *state) {
    std::lock_guard<std::mutex> lock(gMutex);
    slot->state = state;
}

struct Ctx {
    std::string user;
    std::string token;                 // application_name of this export's session
    ExportService::Limits limits;
    ExportService::Callback cb;
    Clock::time_point started;
    std::shared_ptr<Slot> slot;
    std::atomic<int> pid{0};           // diagnostic only; 0 when BEGIN's reply was never seen
    std::atomic<bool> answered{false};
    std::atomic<bool> ending{false};
    std::atomic<bool> released{false};
    int verifyAttempts = 0;
    bool warnedUnconfirmed = false;

    void answer(ExportService::Result r) {
        if (answered.exchange(true)) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        // Q5: one process log line, no content.
        LOG_INFO << "user data export user=" << user << " outcome=" << static_cast<int>(r.outcome) << " reason=" << r.reason
                 << " rows=" << r.rows << " input_bytes=" << r.inputBytes << " output_bytes=" << r.body.size() << " ms=" << ms;
        cb(std::move(r));
    }
    // Exactly once, after the end was confirmed. The observer runs outside the admission mutex.
    void release(const std::string &cleanup) {
        if (released.exchange(true)) return;
        std::function<void(const ExportService::Release &)> observer;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            slot->busy = false;
            slot->state = "idle";
            gActive.erase(user);
            observer = gReleaseObserver;
        }
        LOG_INFO << "user data export cleanup user=" << user << " slot=" << slot->index << " cleanup=" << cleanup
                 << " attempts=" << verifyAttempts;
        if (observer) observer(ExportService::Release{user, slot->index, pid.load(), cleanup, verifyAttempts});
    }
};

ExportService::Result failure(ExportService::Outcome outcome, const std::string &reason) {
    ExportService::Result r;
    r.outcome = outcome;
    r.reason = reason;
    return r;
}

// A label for logs and tests only; every database failure fails closed the same way. Drogon's PostgreSQL client may not carry
// SQLSTATE (observed in run usapp-e1-ea5ada7186), so the fixed server messages are the fallback; anything else is "database".
std::string reasonOf(const drogon::orm::DrogonDbException &e) {
    if (const auto *sql = dynamic_cast<const drogon::orm::SqlError *>(&e.base())) {
        if (sql->sqlState() == "55P03") return "lock_timeout";
        if (sql->sqlState() == "57014") return "statement_timeout";
    }
    const std::string text = e.base().what();
    if (text.find("canceling statement due to lock timeout") != std::string::npos) return "lock_timeout";
    if (text.find("canceling statement due to statement timeout") != std::string::npos) return "statement_timeout";
    return "database";
}

void verifyEnded(const std::shared_ptr<Ctx> &ctx);

// Schedule every export statement on the same submission loop as the first ROLLBACK. Once
// ending is set, late callbacks cannot submit another stage, even after the slot is reused.
template<typename... Args>
void submit(const std::shared_ptr<Ctx> &ctx, Args&&... args) {
    timerLoop()->queueInLoop([ctx, statement = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        if (ctx->ending) return;
        std::apply([&](auto&&... values) {
            ctx->slot->client->execSqlAsync(std::forward<decltype(values)>(values)...);
        }, std::move(statement));
    });
}

// Retry later on the event-loop timer, never by blocking. Backoff 50 ms doubling to 2 s; the slot stays held meanwhile.
void verifyLater(const std::shared_ptr<Ctx> &ctx) {
    const double delay = std::min(2.0, 0.05 * static_cast<double>(1 << std::min(ctx->verifyAttempts, 6)));
    timerLoop()->runAfter(delay, [ctx]() { verifyEnded(ctx); });
}

void unconfirmed(const std::shared_ptr<Ctx> &ctx, const char *why) {
    setSlotState(ctx->slot, "unconfirmed");
    if (!ctx->warnedUnconfirmed) {
        ctx->warnedUnconfirmed = true;
        LOG_WARN << "user data export slot held: end not confirmed user=" << ctx->user << " slot=" << ctx->slot->index << " why=" << why
                 << " (released automatically once the original session is gone or idle; see design Section 7)";
    }
    verifyLater(ctx);
}

void checkEnded(const std::shared_ptr<Ctx> &ctx);

// One verification round on the slot's single connection: ROLLBACK, then the token check. If the check runs on the tagged
// session, the ROLLBACK just before it ran on that same session (a reconnect yields an untagged session, which is never
// re-tagged), so its success is the server's confirmation that this session has no transaction. ROLLBACK with none open only warns.
void verifyEnded(const std::shared_ptr<Ctx> &ctx) {
    ++ctx->verifyAttempts;
    ctx->slot->client->execSqlAsync(
        "ROLLBACK", [ctx](const drogon::orm::Result &) { checkEnded(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { unconfirmed(ctx, "rollback_unacknowledged"); });
}

void checkEnded(const std::shared_ptr<Ctx> &ctx) {
    ctx->slot->client->execSqlAsync(
        "SELECT current_setting('application_name') = $1 AS mine, "
        "(SELECT count(*) FROM pg_stat_activity a WHERE a.application_name = $1 AND a.pid <> pg_backend_pid() "
        " AND a.state IS DISTINCT FROM 'idle')::bigint AS open_elsewhere, "
        "EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid WHERE a.application_name = $1 "
        " AND l.locktype = 'advisory' AND l.classid = 33026 AND l.objid = 0) AS token_locked",
        [ctx](const drogon::orm::Result &rows) {
            bool faulted = false;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (gVerifyFaults > 0) { --gVerifyFaults; faulted = true; }
            }
            if (faulted || rows.size() != 1u) { unconfirmed(ctx, "verification_failed"); return; }
            const auto &row = rows[0];
            const bool mine = row["mine"].as<bool>();
            const long long openElsewhere = row["open_elsewhere"].as<long long>();
            const bool tokenLocked = row["token_locked"].as<bool>();
            if (openElsewhere > 0 || tokenLocked) {
                unconfirmed(ctx, openElsewhere > 0 ? "original_session_open" : "lock_held");
                return;
            }
            ctx->release(mine ? "ended" : "session_replaced");
        },
        [ctx](const drogon::orm::DrogonDbException &) { unconfirmed(ctx, "verification_failed"); }, ctx->token);
}

// The single end of every admitted export: answer the caller (a no-op after the deadline answered), then end the transaction with
// this service's own ROLLBACK and confirm it. Only the confirmation releases the slot.
void endWith(const std::shared_ptr<Ctx> &ctx, ExportService::Result r) {
    if (ctx->ending.exchange(true)) return;
    ctx->answer(std::move(r));
    setSlotState(ctx->slot, "end_requested");
    // The end itself: this service's ROLLBACK and its token-based confirmation (verifyEnded), retried until confirmed.
    auto send = [ctx]() { timerLoop()->queueInLoop([ctx]() { verifyEnded(ctx); }); };
    std::function<void(std::function<void()>)> hook;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        hook = gBeforeEnd;
    }
    if (hook) hook(send);
    else send();
}

void generate(const std::shared_ptr<Ctx> &ctx) {
    if (ctx->answered) { endWith(ctx, failure(ExportService::Outcome::Unavailable, "deadline")); return; }
    const auto &l = ctx->limits;
    bool lost = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        lost = gGenerationReplyLoss;
        gGenerationReplyLoss = false;
    }
    submit(ctx,
        ExportService::generationSql(),
        [ctx, lost](const drogon::orm::Result &rows) {
            if (lost) return;  // test fault: generation ran, neither reply reaches the lifecycle
            using O = ExportService::Outcome;
            if (rows.size() != 1u) { endWith(ctx, failure(O::Unavailable, "database")); return; }
            const auto &row = rows[0];
            // The tagged session, still inside the read-only transaction that holds the lock.
            if (row["session_tag"].as<std::string>() != ctx->token || row["session_ro"].as<std::string>() != "on") {
                endWith(ctx, failure(O::Unavailable, "session_lost"));
                return;
            }
            const auto status = row["status"].as<std::string>();
            ExportService::Result r;
            r.rows = row["row_total"].isNull() ? 0 : row["row_total"].as<long long>();
            r.inputBytes = row["byte_total"].isNull() ? 0 : row["byte_total"].as<long long>();
            if (status == "no_space") { r.outcome = O::SpaceNotFound; r.reason = "no_space"; }
            else if (status != "ok") { r.outcome = O::TooLarge; r.reason = status; }
            else if (row["doc"].isNull()) { r.outcome = O::Unavailable; r.reason = "database"; }
            else {
                auto doc = row["doc"].as<std::string>();
                // The exact transmitted bytes: the body is sent as this string, unchanged.
                if (static_cast<long long>(doc.size()) != row["doc_bytes"].as<long long>()) { r.outcome = O::Unavailable; r.reason = "database"; }
                else if (static_cast<long long>(doc.size()) > ctx->limits.maxOutputBytes) { r.outcome = O::TooLarge; r.reason = "output_bytes"; }
                else {
                    r.outcome = O::Ok;
                    r.reason = "ok";
                    r.filename = "usapp-export-" + row["stamp"].as<std::string>() + ".json";
                    r.body = std::move(doc);
                }
            }
            endWith(ctx, std::move(r));
        },
        [ctx, lost](const drogon::orm::DrogonDbException &e) {
            if (!lost) endWith(ctx, failure(ExportService::Outcome::Unavailable, reasonOf(e)));
        },
        ctx->user, l.maxSectionRows, static_cast<long long>(l.maxTotalRows), static_cast<long long>(l.maxInputBytes));
}

void begin(const std::shared_ptr<Ctx> &ctx) {
    using O = ExportService::Outcome;
    auto onError = [ctx](const drogon::orm::DrogonDbException &e) { endWith(ctx, failure(O::Unavailable, reasonOf(e))); };
    // 1. Tag the session (autocommit, session-level), before any transaction can exist.
    submit(ctx,
        "SELECT set_config('application_name', $1, false) AS tag",
        [ctx, onError](const drogon::orm::Result &) {
            // 2. BEGIN. If its reply is lost, the transaction may exist on the tagged session; only verification can tell.
            submit(ctx,
                "BEGIN ISOLATION LEVEL READ COMMITTED, READ ONLY",
                [ctx, onError](const drogon::orm::Result &) {
                    bool lost = false;
                    bool silent = false;
                    {
                        std::lock_guard<std::mutex> lock(gMutex);
                        lost = gBeginReplyLoss;
                        silent = gSilentBeginReplyLoss;
                        gBeginReplyLoss = gSilentBeginReplyLoss = false;
                    }
                    if (lost) {  // test fault: BEGIN was applied, its reply treated as never received
                        if (!silent) endWith(ctx, failure(O::Unavailable, "session_uncertain"));
                        return;
                    }
                    // 3. Identity, settings: still the tagged session, now inside the read-only transaction.
                    submit(ctx,
                        "SELECT pg_backend_pid() AS pid, current_setting('application_name') = $3 AS tagged, "
                        "current_setting('transaction_read_only') AS ro, "
                        "set_config('lock_timeout', $1, true) AS l, set_config('statement_timeout', $2, true) AS s",
                        [ctx, onError](const drogon::orm::Result &rows) {
                            if (rows.size() != 1u || !rows[0]["tagged"].as<bool>() || rows[0]["ro"].as<std::string>() != "on") {
                                endWith(ctx, failure(O::Unavailable, "session_lost"));
                                return;
                            }
                            int override = 0;
                            {
                                std::lock_guard<std::mutex> lock(gMutex);
                                override = gPidOverride;
                            }
                            ctx->pid = override != 0 ? override : rows[0]["pid"].as<int>();
                            if (ctx->answered) { endWith(ctx, failure(O::Unavailable, "deadline")); return; }
                            // 4. The recovery lock, in its own statement: generation's snapshot starts after it is held.
                            submit(ctx,
                                "SELECT pg_advisory_xact_lock_shared(33026, 0), current_setting('application_name') = $1 AS tagged",
                                [ctx](const drogon::orm::Result &locked) {
                                    if (locked.size() != 1u || !locked[0]["tagged"].as<bool>()) {
                                        endWith(ctx, failure(O::Unavailable, "session_lost"));
                                        return;
                                    }
                                    std::function<void(std::function<void()>)> hook;
                                    {
                                        std::lock_guard<std::mutex> lock(gMutex);
                                        hook = gAfterLock;
                                    }
                                    if (hook) hook([ctx]() { generate(ctx); });
                                    else generate(ctx);
                                },
                                onError, ctx->token);
                        },
                        onError, std::to_string(ctx->limits.lockTimeoutMs) + "ms", std::to_string(ctx->limits.statementTimeoutMs) + "ms",
                        ctx->token);
                },
                // No reply to BEGIN: it may or may not have been applied. The token-based verification decides.
                [ctx](const drogon::orm::DrogonDbException &e) {
                    endWith(ctx, failure(O::Unavailable, reasonOf(e)));
                });
        },
        onError, ctx->token);
}

}  // namespace

const std::string &ExportService::generationSql() {
    static const std::string sql = buildSql();
    return sql;
}

void ExportService::init(const std::string &connectionString, int slots) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gSlots->empty()) return;
    for (int i = 0; i < slots; ++i) {
        auto slot = std::make_shared<Slot>();
        slot->index = i;
        slot->client = drogon::orm::DbClient::newPgClient(connectionString, 1);
        // A client-side bound on any one statement, above the server-side lock and statement timeouts, for a dead connection.
        slot->client->setTimeout(static_cast<double>(gLimits.deadlineMs + gLimits.statementTimeoutMs) / 1000.0);
        gSlots->push_back(slot);
    }
}

ExportService::Limits ExportService::limits() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gLimits;
}

void ExportService::setLimitsForTesting(const Limits &limits) {
    std::lock_guard<std::mutex> lock(gMutex);
    gLimits = limits;
}

void ExportService::resetLimitsForTesting() { setLimitsForTesting(Limits{}); }

void ExportService::setAfterLockHookForTesting(std::function<void(std::function<void()>)> hook) {
    std::lock_guard<std::mutex> lock(gMutex);
    gAfterLock = std::move(hook);
}

void ExportService::setBeforeEndHookForTesting(std::function<void(std::function<void()>)> hook) {
    std::lock_guard<std::mutex> lock(gMutex);
    gBeforeEnd = std::move(hook);
}

void ExportService::setReleaseObserverForTesting(std::function<void(const Release &)> observer) {
    std::lock_guard<std::mutex> lock(gMutex);
    gReleaseObserver = std::move(observer);
}

void ExportService::setTokenObserverForTesting(std::function<void(const std::string &)> observer) {
    std::lock_guard<std::mutex> lock(gMutex);
    gTokenObserver = std::move(observer);
}

void ExportService::setVerifyFaultsForTesting(int failures) {
    std::lock_guard<std::mutex> lock(gMutex);
    gVerifyFaults = failures;
}

void ExportService::setBeginReplyLossForTesting(bool once, bool silent) {
    std::lock_guard<std::mutex> lock(gMutex);
    gBeginReplyLoss = once;
    gSilentBeginReplyLoss = once && silent;
}

void ExportService::setGenerationReplyLossForTesting(bool once) {
    std::lock_guard<std::mutex> lock(gMutex);
    gGenerationReplyLoss = once;
}

void ExportService::setCapturedPidOverrideForTesting(int pid) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPidOverride = pid;
}

void ExportService::clearAdmissionsForTesting() {
    std::lock_guard<std::mutex> lock(gMutex);
    gLastAdmission.clear();
}

int ExportService::busySlotsForTesting() {
    std::lock_guard<std::mutex> lock(gMutex);
    int busy = 0;
    for (const auto &slot : *gSlots) busy += slot->busy ? 1 : 0;
    return busy;
}

std::vector<std::string> ExportService::slotStatesForTesting() {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<std::string> states;
    for (const auto &slot : *gSlots) states.push_back(slot->state);
    return states;
}

void ExportService::exportFor(const std::string &userId, Callback cb) {
    auto ctx = std::make_shared<Ctx>();
    ctx->user = userId;
    ctx->cb = std::move(cb);
    ctx->started = Clock::now();
    ctx->token = "usapp-export-" + crypto::randomHex(16);
    Result refused;
    refused.outcome = Outcome::Busy;
    std::function<void(const std::string &)> tokenObserver;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        ctx->limits = gLimits;
        tokenObserver = gTokenObserver;
        const auto now = Clock::now();
        const auto last = gLastAdmission.find(userId);
        const int capacity = std::min(ctx->limits.maxGlobal, static_cast<int>(gSlots->size()));
        std::shared_ptr<Slot> free;
        for (int i = 0; i < capacity && !free; ++i) {
            if (!(*gSlots)[static_cast<size_t>(i)]->busy) free = (*gSlots)[static_cast<size_t>(i)];
        }
        if (gSlots->empty()) {
            refused.outcome = Outcome::Unavailable;
            refused.reason = "not_initialized";
        } else if (gActive.count(userId)) {
            refused.reason = "concurrent";
            refused.retryAfterSeconds = 1;
        } else if (!free) {
            refused.reason = "global";
            refused.retryAfterSeconds = 1;
        } else if (last != gLastAdmission.end() && now - last->second < std::chrono::seconds(ctx->limits.frequencySeconds)) {
            refused.reason = "frequency";
            const auto left = std::chrono::seconds(ctx->limits.frequencySeconds) - (now - last->second);
            refused.retryAfterSeconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(left).count()) + 1;
        } else {
            free->busy = true;
            free->state = "working";
            ctx->slot = free;
            gActive.insert(userId);
            gLastAdmission[userId] = now;
        }
    }
    if (!ctx->slot) {
        // Outside the admission mutex: the callback is the caller's.
        ctx->released = true;
        ctx->answered = true;
        LOG_INFO << "user data export user=" << userId << " outcome=refused reason=" << refused.reason;
        ctx->cb(std::move(refused));
        return;
    }
    if (tokenObserver) tokenObserver(ctx->token);
    // The end-to-end deadline answers the caller; the slot is held until the transaction's end is confirmed.
    timerLoop()->runAfter(static_cast<double>(ctx->limits.deadlineMs) / 1000.0,
                          [ctx]() {
                              endWith(ctx, failure(Outcome::Unavailable, "deadline"));
                          });
    begin(ctx);
}

}  // namespace usapp
