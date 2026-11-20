// =====================================================================
// services/api/src/services/ReminderGeneration.cc
// =====================================================================
#include "services/ReminderGeneration.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

namespace usapp::reminder {

namespace {

// The one definition of "due now", used by the scan and again after the locks. Mirrors
// usapp_reminder_occurrence_guard (0025), which re-checks each row. The window comes first, so the effective-time
// functions run only for rows inside it. Asia/Shanghai throughout; $1 is read once per statement.
#define USAPP_REMINDER_ELIGIBLE_CTES                                                                                     \
    "clock AS MATERIALIZED (SELECT x.now_at, (x.now_at AT TIME ZONE 'Asia/Shanghai')::date AS today "                    \
    "  FROM (SELECT COALESCE(NULLIF($1::text, '')::timestamptz, clock_timestamp()) AS now_at) x), "                      \
    "latest AS MATERIALIZED (SELECT DISTINCT ON (r.owner_id, r.task_id, r.anniversary_id) r.* FROM reminder_rules r "    \
    "  ORDER BY r.owner_id, r.task_id, r.anniversary_id, r.seq DESC), "                                                  \
    /* Tasks: the period containing now and nothing else. No period row is read to find it, or created. */               \
    "task_window AS MATERIALIZED (SELECT r.id AS rule_id, r.couple_id, r.owner_id, t.id AS task_id, t.created_at AS source_at, " \
    "    p.period_date, f.fire_at, s.stale_at "                                                                          \
    "  FROM latest r CROSS JOIN clock "                                                                                  \
    "  JOIN tasks t ON t.id = r.task_id AND t.assignee_id = r.owner_id AND t.recurrence_id IS NULL "                     \
    "  CROSS JOIN LATERAL (SELECT CASE t.schedule_type WHEN 'weekly' THEN 7 WHEN 'daily' THEN 1 END AS span) sp "        \
    "  CROSS JOIN LATERAL (SELECT CASE sp.span WHEN 7 THEN clock.today - (EXTRACT(ISODOW FROM clock.today)::integer - 1) " \
    "      WHEN 1 THEN clock.today END AS period_date) p "                                                               \
    "  CROSS JOIN LATERAL (SELECT CASE WHEN sp.span IS NULL THEN t.deadline_local_date ELSE p.period_date + (sp.span - 1) END AS last_date, " \
    "      CASE WHEN sp.span IS NULL THEN t.deadline_at "                                                                \
    "      ELSE (p.period_date + sp.span)::timestamp AT TIME ZONE 'Asia/Shanghai' END AS deadline) d "                   \
    "  CROSS JOIN LATERAL (SELECT usapp_reminder_task_fire_at(r.mode, r.before_minutes, r.days_before, r.at_time, "      \
    "      r.at_local_date, d.last_date, d.deadline) AS fire_at) f "                                                     \
    "  CROSS JOIN LATERAL (SELECT CASE WHEN d.deadline IS NULL THEN f.fire_at + interval '2 hours' "                     \
    "      ELSE LEAST(d.deadline, f.fire_at + interval '2 hours') END AS stale_at) s "                                   \
    "  WHERE r.enabled AND f.fire_at <= clock.now_at AND clock.now_at < s.stale_at), "                                   \
    "task_due AS MATERIALIZED (SELECT 0 AS kind_rank, 'task' AS kind, w.rule_id, w.couple_id, w.owner_id, w.task_id AS target_id, " \
    "    w.period_date, NULL::date AS target_date, NULL::integer AS days_before, NULL::integer AS anniversary_version, " \
    "    NULL::integer AS schedule_version, w.fire_at, w.stale_at "                                                      \
    "  FROM task_window w "                                                                                              \
    "  WHERE NOT EXISTS (SELECT 1 FROM task_closures k WHERE k.task_id = w.task_id) "                                    \
    /* Partial progress has no task_completions row, so it never suppresses. */                                          \
    "    AND NOT EXISTS (SELECT 1 FROM task_completions c WHERE w.period_date IS NULL AND c.task_id = w.task_id) "       \
    "    AND NOT EXISTS (SELECT 1 FROM tasks q JOIN task_completions c ON c.task_id = q.id "                             \
    "                     WHERE q.recurrence_id = w.task_id AND q.period_date = w.period_date) "                         \
    "    AND GREATEST(usapp_reminder_task_effective_at(w.owner_id, w.task_id), w.source_at) <= w.fire_at "               \
    /* An inherited rule that no longer fits generates nothing (0027). */                                                \
    "    AND NOT usapp_reminder_rule_needs_reset(w.rule_id) "                                                            \
    /* Same keys as the 0025 partial unique indexes. */                                                                  \
    "    AND NOT EXISTS (SELECT 1 FROM reminder_occurrences o WHERE o.owner_id = w.owner_id AND o.task_id = w.task_id "  \
    "                     AND o.period_date IS NOT DISTINCT FROM w.period_date)), "                                      \
    /* Anniversaries: from today's fire date, so year boundaries and leap years need no special case. */                 \
    "anniversary_window AS MATERIALIZED (SELECT r.id AS rule_id, r.couple_id, r.owner_id, r.anniversary_id, o.days_before, " \
    "    clock.today + o.days_before AS target_date, (clock.today + r.at_time) AT TIME ZONE 'Asia/Shanghai' AS fire_at, " \
    "    (clock.today + 1)::timestamp AT TIME ZONE 'Asia/Shanghai' AS stale_at "                                         \
    "  FROM latest r CROSS JOIN clock "                                                                                  \
    "  CROSS JOIN LATERAL (SELECT d::integer AS days_before FROM unnest(r.days_before_set) d) o "                        \
    "  WHERE r.enabled AND r.anniversary_id IS NOT NULL "                                                                \
    "    AND ((clock.today + r.at_time) AT TIME ZONE 'Asia/Shanghai') <= clock.now_at), "                                \
    "anniversary_due AS MATERIALIZED (SELECT 1 AS kind_rank, 'anniversary' AS kind, w.rule_id, w.couple_id, w.owner_id, " \
    "    w.anniversary_id AS target_id, NULL::date AS period_date, w.target_date, w.days_before, "                       \
    "    sched.latest_version AS anniversary_version, sched.schedule_version, w.fire_at, w.stale_at "                    \
    "  FROM anniversary_window w "                                                                                       \
    "  CROSS JOIN LATERAL usapp_anniversary_schedule_effective(w.anniversary_id) sched "                                 \
    "  JOIN anniversary_revisions rev ON rev.anniversary_id = w.anniversary_id AND rev.version = sched.latest_version "  \
    "  CROSS JOIN LATERAL (SELECT EXTRACT(YEAR FROM w.target_date)::integer - EXTRACT(YEAR FROM rev.event_date)::integer AS year_gap) g " \
    "  WHERE ((rev.kind = 'countdown' AND w.target_date = rev.event_date) "                                              \
    "         OR (rev.kind = 'anniversary' AND g.year_gap >= 0 "                                                         \
    "             AND (rev.event_date + make_interval(years => g.year_gap))::date = w.target_date)) "                    \
    "    AND GREATEST(usapp_reminder_offset_effective_at(w.owner_id, w.anniversary_id, w.days_before), sched.effective_at) <= w.fire_at " \
    "    AND NOT EXISTS (SELECT 1 FROM reminder_occurrences o WHERE o.owner_id = w.owner_id "                            \
    "                     AND o.anniversary_id = w.anniversary_id AND o.target_date = w.target_date "                    \
    "                     AND o.days_before = w.days_before)), "                                                         \
    "eligible AS (SELECT * FROM task_due UNION ALL SELECT * FROM anniversary_due) "

// The keys cover the whole identity, so no two candidates tie (design Section 9.1).
#define USAPP_REMINDER_ORDER(a) \
    a ".fire_at, " a ".owner_id, " a ".kind_rank, " a ".target_id, " a ".days_before, COALESCE(" a ".period_date, " a ".target_date)"

const char *kPreflightSql =
    "SELECT set_config('lock_timeout', $1::text, true) AS lock_timeout, "
    "current_setting('transaction_isolation') = 'read committed' AS fresh_snapshots, "
    "EXISTS (SELECT 1 FROM schema_migrations WHERE version = '0026_reminder_next_fire') AS registered, "
    // R4: the statements below use the needs-reset definition from 0027.
    "EXISTS (SELECT 1 FROM schema_migrations WHERE version = '0027_reminder_successor_rules') AS successor_rules, "
    // The behaviour itself, not only its registration: both repaired functions exclude fired identities.
    "(SELECT count(*) FROM pg_proc p WHERE p.pronamespace = 'public'::regnamespace "
    "  AND p.proname IN ('usapp_reminder_task_next_fire', 'usapp_reminder_anniversary_next_fire') "
    "  AND p.prosrc LIKE '%reminder_occurrences%')::bigint AS repaired";

// Sorted within one statement: every other path holds at most one existing root, and takes its domain lock
// before 33017 (design Section 11).
const char *kLockSql =
    "WITH c AS (SELECT * FROM jsonb_to_recordset($1::jsonb) AS x(kind text, owner_id text, target_id text)), "
    "keys AS (SELECT 0 AS phase, CASE c.kind WHEN 'task' THEN 33004 ELSE 33010 END AS ns, hashtext(c.target_id) AS k FROM c "
    "         UNION SELECT 1, 33017, hashtext(c.owner_id || ':' || c.target_id) FROM c) "
    "SELECT pg_advisory_xact_lock(o.ns, o.k) FROM (SELECT ns, k FROM keys ORDER BY phase, ns, k) o";

// A new statement, so a new snapshot: it sees whatever committed before the locks were granted. Facts come from
// this evaluation; the candidate only selects. ON CONFLICT is the race backstop.
const char *kInsertSql =
    "WITH " USAPP_REMINDER_ELIGIBLE_CTES ", "
    "picked AS (SELECT * FROM jsonb_to_recordset($2::jsonb) AS x(rule_id uuid, target_id uuid, period_date date, "
    "    target_date date, days_before integer, fire_at timestamptz)) "
    "INSERT INTO reminder_occurrences (couple_id, owner_id, rule_id, event_kind, task_id, period_date, anniversary_id, "
    "    anniversary_version, schedule_version, target_date, days_before, fire_at, stale_at) "
    "SELECT e.couple_id, e.owner_id, e.rule_id, CASE e.kind WHEN 'task' THEN 'reminder.task' ELSE 'reminder.anniversary' END, "
    "    CASE e.kind WHEN 'task' THEN e.target_id END, e.period_date, CASE e.kind WHEN 'anniversary' THEN e.target_id END, "
    "    e.anniversary_version, e.schedule_version, e.target_date, e.days_before, e.fire_at, e.stale_at "
    "FROM eligible e JOIN picked p ON p.rule_id = e.rule_id AND p.target_id = e.target_id AND p.fire_at = e.fire_at "
    "    AND p.period_date IS NOT DISTINCT FROM e.period_date AND p.target_date IS NOT DISTINCT FROM e.target_date "
    "    AND p.days_before IS NOT DISTINCT FROM e.days_before "
    "ORDER BY " USAPP_REMINDER_ORDER("e") " "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

const char *kNotifySql =
    "INSERT INTO notifications (couple_id, actor_id, recipient_id, event_type, target_kind, target_id, reminder_occurrence_id) "
    "SELECT o.couple_id, NULL::uuid, o.owner_id, o.event_kind, "
    "    CASE o.event_kind WHEN 'reminder.task' THEN 'task' ELSE 'anniversary' END, COALESCE(o.task_id, o.anniversary_id), o.id "
    "FROM reminder_occurrences o WHERE o.id = ANY($1::uuid[])";

struct TickCtx {
    drogon::orm::Transaction *tx = nullptr;
    Policy policy;
    std::function<void(TickReport)> done;
    std::string candidates;
    size_t scanned = 0;
    std::chrono::steady_clock::time_point lockStart;
};
using TickPtr = std::shared_ptr<TickCtx>;

void finishTick(const TickPtr &ctx, TickReport report) {
    auto done = std::move(ctx->done);
    ctx->done = nullptr;
    if (done) done(std::move(report));
}

void failTick(const TickPtr &ctx, const char *stage) {
    TickReport report;
    report.ok = false;
    // Fixed label: database text may echo values.
    report.detail = std::string("reminder_generate stage=") + stage;
    finishTick(ctx, std::move(report));
}

template <typename... Args>
void step(const TickPtr &ctx, const char *sql, const char *stage,
          std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    ctx->tx->execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                failTick(ctx, stage);
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) { failTick(ctx, stage); }, std::forward<Args>(args)...);
}

void finishCounts(const TickPtr &ctx, size_t created) {
    TickReport report;
    report.ok = true;
    report.scanned = ctx->scanned;
    report.created = created;
    if (ctx->scanned > 0) {
        report.lockHeldMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx->lockStart).count();
    }
    std::ostringstream out;
    out << "reminders scanned=" << report.scanned << " created=" << report.created
        << " dropped=" << (report.scanned - report.created) << " lock_held_ms=" << report.lockHeldMs;
    report.detail = out.str();
    finishTick(ctx, std::move(report));
}

void notifyStep(const TickPtr &ctx, const drogon::orm::Result &inserted) {
    const size_t created = inserted.size();
    if (created == 0) {
        finishCounts(ctx, 0);
        return;
    }
    std::string ids = "{";
    for (size_t i = 0; i < created; ++i) {
        if (i > 0) ids += ",";
        ids += inserted[i]["id"].as<std::string>();
    }
    ids += "}";
    step(ctx, kNotifySql, "notify",
         [ctx, created](const drogon::orm::Result &rows) {
             if (rows.affectedRows() != created) {
                 failTick(ctx, "notify");
                 return;
             }
             finishCounts(ctx, created);
         },
         ids);
}

void lockAndInsert(const TickPtr &ctx) {
    ctx->lockStart = std::chrono::steady_clock::now();
    step(ctx, kLockSql, "lock",
         [ctx](const drogon::orm::Result &) {
             step(ctx, kInsertSql, "insert", [ctx](const drogon::orm::Result &inserted) { notifyStep(ctx, inserted); },
                  std::string(), ctx->candidates);
         },
         ctx->candidates);
}

}  // namespace

const std::string kScanSql =
    "WITH " USAPP_REMINDER_ELIGIBLE_CTES ", "
    "chosen AS (SELECT e.* FROM eligible e ORDER BY " USAPP_REMINDER_ORDER("e") " LIMIT $2::integer) "
    "SELECT COALESCE(jsonb_agg(jsonb_build_object('kind', c.kind, 'rule_id', c.rule_id, 'owner_id', c.owner_id, "
    "    'target_id', c.target_id, 'period_date', c.period_date, 'target_date', c.target_date, "
    "    'days_before', c.days_before, 'fire_at', c.fire_at) ORDER BY " USAPP_REMINDER_ORDER("c") "), '[]'::jsonb)::text AS candidates, "
    "count(*)::bigint AS n FROM chosen c";

Generator::Generator(Policy policy) : policy_(std::move(policy)) {}

void Generator::runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done) const {
    auto ctx = std::make_shared<TickCtx>();
    ctx->tx = &tx;
    ctx->policy = policy_;
    ctx->done = std::move(done);
    if (!ctx->policy.enabled) {
        TickReport report;
        report.ok = true;
        report.detail = "reminders dispatch=off";
        finishTick(ctx, std::move(report));
        return;
    }
    step(ctx, kPreflightSql, "preflight",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u || !rows[0]["fresh_snapshots"].as<bool>()) {
                 failTick(ctx, "preflight");
                 return;
             }
             // Fail closed: a build on a database without the repair would report next_fire_at values the
             // no-refire rule never generates. Nothing has been read or written yet.
             if (!rows[0]["registered"].as<bool>() || rows[0]["repaired"].as<long long>() != 2) {
                 LOG_ERROR << "reminder_generate: migration 0026_reminder_next_fire is absent; no generation";
                 failTick(ctx, "preflight_missing_0026");
                 return;
             }
             if (!rows[0]["successor_rules"].as<bool>()) {
                 LOG_ERROR << "reminder_generate: migration 0027_reminder_successor_rules is absent; no generation";
                 failTick(ctx, "preflight_missing_0027");
                 return;
             }
             step(ctx, kScanSql.c_str(), "scan",
                  [ctx](const drogon::orm::Result &scan) {
                      if (scan.size() != 1u) {
                          failTick(ctx, "scan");
                          return;
                      }
                      ctx->scanned = static_cast<size_t>(scan[0]["n"].as<long long>());
                      if (ctx->scanned == 0) {
                          finishCounts(ctx, 0);
                          return;
                      }
                      ctx->candidates = scan[0]["candidates"].as<std::string>();
                      lockAndInsert(ctx);
                  },
                  ctx->policy.scanAtForTest, std::to_string(ctx->policy.limit));
         },
         std::to_string(ctx->policy.lockTimeoutMs));
}

}  // namespace usapp::reminder
