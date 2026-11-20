// =====================================================================
// services/api/src/services/PushDispatch.cc
// =====================================================================
#include "services/PushDispatch.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <atomic>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

#include "db/Db.h"
#include "services/PushService.h"

namespace usapp::push {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_UNRESOLVED(a)                                                                                     \
    "NOT EXISTS (SELECT 1 FROM push_attempt_observations o WHERE o.notification_id = " a ".notification_id "    \
    "AND o.generation_id = " a ".generation_id AND o.attempt_no = " a ".attempt_no) "                           \
    "AND NOT EXISTS (SELECT 1 FROM push_attempt_expiries x WHERE x.notification_id = " a ".notification_id "    \
    "AND x.generation_id = " a ".generation_id AND x.attempt_no = " a ".attempt_no)"
#define USAPP_NO_OUTCOME                                                                                        \
    "NOT EXISTS (SELECT 1 FROM push_delivery_outcomes t WHERE t.notification_id = d.notification_id "           \
    "AND t.generation_id = d.generation_id)"
#define USAPP_NO_LIVE_ATTEMPT                                                                                   \
    "NOT EXISTS (SELECT 1 FROM push_attempts l WHERE l.notification_id = d.notification_id "                    \
    "AND l.generation_id = d.generation_id AND " USAPP_UNRESOLVED("l") ")"
#define USAPP_GENERATION_ACTIVE(keyParam)                                                                       \
    "(NOT EXISTS (SELECT 1 FROM push_subscription_revocations r WHERE r.subscription_id = g.id) "               \
    "AND s.is_authenticated AND s.revoked_at IS NULL AND s.expires_at > clock_timestamp() "                     \
    "AND g.vapid_key_id = " keyParam ")"

// R5: every pre-send below calls the 0028 function, so an active tick without it would claim, fail each check and let
// leases expire into provider budget. The migration is checked here, before anything is written.
const char *kControlSql =
    "SELECT c.state, EXISTS (SELECT 1 FROM schema_migrations WHERE version = '0028_reminder_push_stale') AS source_validity "
    "FROM usapp_push_control_state() c";

const char *kExpireSql =
    "INSERT INTO push_attempt_expiries (notification_id, generation_id, attempt_no) "
    "SELECT a.notification_id, a.generation_id, a.attempt_no FROM push_attempts a "
    "WHERE a.lease_until <= clock_timestamp() AND " USAPP_UNRESOLVED("a")
    " ORDER BY a.lease_until LIMIT $1::integer ON CONFLICT DO NOTHING RETURNING attempt_no";

// A budget spent by expiries: the recording path decides this for actual observations.
const char *kExhaustedSql =
    "INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) "
    "SELECT d.notification_id, d.generation_id, 'exhausted', "
    "(SELECT max(a.attempt_no) FROM push_attempts a WHERE a.notification_id = d.notification_id "
    " AND a.generation_id = d.generation_id) "
    "FROM push_deliveries d WHERE " USAPP_NO_OUTCOME " AND " USAPP_NO_LIVE_ATTEMPT
    " AND usapp_push_provider_attempts(d.notification_id, d.generation_id) >= 5 "
    "LIMIT $1::integer ON CONFLICT DO NOTHING RETURNING attempt_no";

// TTL bounds claimed and retrying work too; an in-flight attempt may still be observed later.
const char *kExpiredSql =
    "INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) "
    "SELECT d.notification_id, d.generation_id, 'expired' FROM push_deliveries d "
    "JOIN notifications n ON n.id = d.notification_id "
    "WHERE " USAPP_NO_OUTCOME " AND n.created_at <= clock_timestamp() - make_interval(secs => $1::double precision) "
    "LIMIT $2::integer ON CONFLICT DO NOTHING RETURNING notification_id";

const char *kCutoverSql =
    "INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) "
    "SELECT d.notification_id, d.generation_id, 'cutover_skipped' FROM push_deliveries d "
    "JOIN notifications n ON n.id = d.notification_id "
    "WHERE " USAPP_NO_OUTCOME " AND n.created_at < (SELECT c.cutover FROM usapp_push_control_state() c) "
    "LIMIT $1::integer ON CONFLICT DO NOTHING RETURNING notification_id";

// Bounded TTL scan, not a watermark: a late commit inside the window is still found. Insert-time
// rule: created_at compared with activated_at and the cutover, whatever the transaction's begin.
// A notice ahead of the database clock (0041) is deferred, not rejected: this scan finds it again on a later tick.
const char *kFanoutSql =
    "INSERT INTO push_deliveries (notification_id, generation_id, recipient_id, couple_id) "
    "SELECT n.id, g.id, n.recipient_id, n.couple_id FROM notifications n "
    "JOIN push_subscriptions g ON g.user_id = n.recipient_id AND g.couple_id = n.couple_id "
    "AND n.created_at >= g.activated_at "
    "JOIN sessions s ON s.id = g.session_id "
    "WHERE n.created_at >= (SELECT c.cutover FROM usapp_push_control_state() c) "
    "AND n.created_at > clock_timestamp() - make_interval(secs => $1::double precision) "
    "AND n.created_at <= clock_timestamp() "
    "AND " USAPP_GENERATION_ACTIVE("$2") " "
    "AND NOT EXISTS (SELECT 1 FROM push_deliveries e WHERE e.notification_id = n.id AND e.generation_id = g.id) "
    "ORDER BY n.created_at, n.id, g.id LIMIT $3::integer ON CONFLICT DO NOTHING RETURNING notification_id";

const char *kPreClaimInactiveSql =
    "INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state) "
    "SELECT d.notification_id, d.generation_id, 'inactive' FROM push_deliveries d "
    "JOIN push_subscriptions g ON g.id = d.generation_id JOIN sessions s ON s.id = g.session_id "
    "WHERE " USAPP_NO_OUTCOME " AND " USAPP_NO_LIVE_ATTEMPT " AND NOT " USAPP_GENERATION_ACTIVE("$1")
    " LIMIT $2::integer ON CONFLICT DO NOTHING RETURNING notification_id";

// Fair selection (Section 6.5): fewest provider attempts, oldest notice, generation id; at most
// $6 per recipient per tick; live leases bounded globally by $5. The backoff is due from the last
// result: Retry-After if longer; an internal result uses the internal backoff and no budget.
const char *kClaimSql =
    "WITH live AS (SELECT count(*)::integer AS n FROM push_attempts a WHERE a.lease_until > clock_timestamp() AND "
    USAPP_UNRESOLVED("a") "), "
    "cand AS (SELECT d.notification_id, d.generation_id, d.recipient_id, n.created_at AS notice_at, "
    "usapp_push_provider_attempts(d.notification_id, d.generation_id) AS used, last.attempt_no "
    "FROM push_deliveries d JOIN notifications n ON n.id = d.notification_id "
    "LEFT JOIN LATERAL (SELECT a.attempt_no, o.class, o.retry_after_s, COALESCE(o.observed_at, x.expired_at) AS result_at "
    "FROM push_attempts a LEFT JOIN push_attempt_observations o ON o.notification_id = a.notification_id "
    "AND o.generation_id = a.generation_id AND o.attempt_no = a.attempt_no "
    "LEFT JOIN push_attempt_expiries x ON x.notification_id = a.notification_id "
    "AND x.generation_id = a.generation_id AND x.attempt_no = a.attempt_no "
    "WHERE a.notification_id = d.notification_id AND a.generation_id = d.generation_id "
    "ORDER BY a.attempt_no DESC LIMIT 1) last ON true "
    "WHERE " USAPP_NO_OUTCOME " AND usapp_push_provider_attempts(d.notification_id, d.generation_id) < 5 "
    // Backlog beyond this tick's maintenance limits is simply not claimable yet.
    "AND n.created_at >= (SELECT c.cutover FROM usapp_push_control_state() c) "
    "AND n.created_at > clock_timestamp() - make_interval(secs => $7::double precision) "
    // Ahead of the clock (0041): no claim, so no attempt, until a later tick finds the clock has passed it.
    "AND n.created_at <= clock_timestamp() AND "
    "(last.attempt_no IS NULL OR (last.result_at IS NOT NULL AND clock_timestamp() >= last.result_at + make_interval(secs => "
    "GREATEST(COALESCE(last.retry_after_s, 0)::double precision, CASE WHEN last.class = 'internal' THEN $3::double precision "
    "ELSE ($4::double precision[])[GREATEST(1, LEAST(usapp_push_provider_attempts(d.notification_id, d.generation_id), "
    "array_length($4::double precision[], 1)))] END))))), "
    "ranked AS (SELECT cand.*, row_number() OVER (PARTITION BY recipient_id ORDER BY used, notice_at, generation_id) AS rn "
    "FROM cand), "
    "pick AS (SELECT * FROM ranked WHERE rn <= $6::integer ORDER BY used, notice_at, generation_id "
    "LIMIT GREATEST(0, $5::integer - (SELECT n FROM live))) "
    "INSERT INTO push_attempts (notification_id, generation_id, attempt_no, instance_id, claimed_at, lease_until) "
    "SELECT p.notification_id, p.generation_id, COALESCE(p.attempt_no, 0) + 1, $1::text, t.ts, "
    "t.ts + make_interval(secs => $2::double precision) "
    "FROM pick p CROSS JOIN LATERAL (SELECT clock_timestamp() AS ts) t "
    "RETURNING notification_id::text AS notification_id, generation_id::text AS generation_id, attempt_no";

// Fences the exact attempt, not only its delivery. No row: no such attempt. R5: LEAST ignores a NULL stale_at, so what a
// non-reminder sends is unchanged; delivery_remaining is the lifetime alone, and the source is read at one instant.
const char *kPreSendSql =
    "SELECT c.state AS control_state, (c.cutover IS NULL OR n.created_at < c.cutover) AS before_cutover, "
    "(n.created_at <= clock_timestamp() - make_interval(secs => $4::double precision)) AS ttl_reached, "
    "floor(extract(epoch FROM n.created_at + make_interval(secs => $4::double precision) - clock_timestamp()))::bigint "
    "AS delivery_remaining, usapp_reminder_occurrence_valid(n.id, src.at_time) AS source_valid, "
    "(n.created_at > src.at_time) AS notice_ahead, "
    "EXISTS (SELECT 1 FROM push_delivery_outcomes t WHERE t.notification_id = d.notification_id "
    "AND t.generation_id = d.generation_id) AS has_outcome, " USAPP_GENERATION_ACTIVE("$3") " AS generation_active, "
    "(a.instance_id = $6::text) AS own_claim, (a.lease_until > clock_timestamp()) AS lease_valid, "
    "EXISTS (SELECT 1 FROM push_attempt_observations o WHERE o.notification_id = a.notification_id "
    "AND o.generation_id = a.generation_id AND o.attempt_no = a.attempt_no) AS observed, "
    "EXISTS (SELECT 1 FROM push_attempt_expiries x WHERE x.notification_id = a.notification_id "
    "AND x.generation_id = a.generation_id AND x.attempt_no = a.attempt_no) AS expired, "
    "EXISTS (SELECT 1 FROM push_attempts w WHERE w.notification_id = a.notification_id "
    "AND w.generation_id = a.generation_id AND w.attempt_no > a.attempt_no) AS superseded, "
    "floor(extract(epoch FROM LEAST(n.created_at + make_interval(secs => $4::double precision), src.stale_at) - src.at_time))::bigint "
    "AS remaining_ttl, floor(extract(epoch FROM LEAST(n.created_at + make_interval(secs => $4::double precision), src.stale_at)) * 1000)::bigint "
    "AS expires_at_ms, n.event_type, n.target_kind, n.target_id::text AS target_id, g.endpoint, g.p256dh, g.auth "
    "FROM push_attempts a JOIN push_deliveries d ON d.notification_id = a.notification_id "
    "AND d.generation_id = a.generation_id JOIN notifications n ON n.id = d.notification_id "
    "JOIN push_subscriptions g ON g.id = d.generation_id JOIN sessions s ON s.id = g.session_id "
    "CROSS JOIN usapp_push_control_state() c "
    "CROSS JOIN LATERAL (SELECT clock_timestamp() AS at_time, (SELECT o.stale_at FROM reminder_occurrences o "
    "WHERE o.id = n.reminder_occurrence_id) AS stale_at) src "
    "WHERE a.notification_id = $1::uuid AND a.generation_id = $2::uuid AND a.attempt_no = $5::integer";

const char *kObservationSql =
    "INSERT INTO push_attempt_observations (notification_id, generation_id, attempt_no, outcome, class, http_status, "
    "retry_after_s) VALUES ($1::uuid, $2::uuid, $3::integer, $4, $5, NULLIF($6, '')::integer, NULLIF($7, '')::integer) "
    "ON CONFLICT DO NOTHING RETURNING attempt_no";

const char *kStoredSql =
    "SELECT o.outcome, usapp_push_provider_attempts(o.notification_id, o.generation_id) AS used, "
    "d.recipient_id::text AS recipient_id, d.couple_id::text AS couple_id "
    "FROM push_attempt_observations o JOIN push_deliveries d ON d.notification_id = o.notification_id "
    "AND d.generation_id = o.generation_id "
    "WHERE o.notification_id = $1::uuid AND o.generation_id = $2::uuid AND o.attempt_no = $3::integer";

const char *kOutcomeSql =
    "INSERT INTO push_delivery_outcomes (notification_id, generation_id, final_state, attempt_no) "
    "VALUES ($1::uuid, $2::uuid, $3, NULLIF($4, '')::integer) ON CONFLICT DO NOTHING";

// Only this attempt's own immutable generation; a replacement on the same endpoint is another row.
const char *kGoneSql =
    "INSERT INTO push_subscription_revocations (subscription_id, user_id, couple_id, reason, gone_notification_id, "
    "gone_attempt_no) VALUES ($1::uuid, $2::uuid, $3::uuid, 'gone', $4::uuid, $5::integer) ON CONFLICT DO NOTHING";

// The lock first, so the conditional insert below reads the state its guard will see: an owner
// transition in between cannot abort the transaction that holds the observation.
// Provenance rule: pause only if this attempt was claimed under the latest control revision. While
// the state is enabled, the latest revision is the owner's enable or resume; any owner action after
// the claim makes this attempt's revision older, so its failure is stale and cannot pause. A NULL
// revision (claimed before 0024) never matches.
const char *kPauseLockSql = "SELECT pg_advisory_xact_lock(33015::integer, 0)";
const char *kPauseSql =
    "INSERT INTO push_dispatch_control (kind, actor, reason_code) SELECT 'pause', 'dispatcher', $1::text "
    "FROM push_attempts a WHERE a.notification_id = $2::uuid AND a.generation_id = $3::uuid "
    "AND a.attempt_no = $4::integer "
    "AND a.control_revision = (SELECT max(r.revision) FROM push_dispatch_control r) "
    "AND (SELECT c.state FROM usapp_push_control_state() c) = 'enabled'";

const char *kFinalSql =
    "SELECT t.final_state FROM push_delivery_outcomes t WHERE t.notification_id = $1::uuid AND t.generation_id = $2::uuid";

#undef USAPP_GENERATION_ACTIVE
#undef USAPP_NO_LIVE_ATTEMPT
#undef USAPP_NO_OUTCOME
#undef USAPP_UNRESOLVED

std::string numberText(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string arrayText(const std::vector<double> &values) {
    std::string out = "{";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += ",";
        out += numberText(values[i]);
    }
    return out + "}";
}

// ---------------------------------------------------------------- the tick
struct TickCtx {
    drogon::orm::Transaction *tx = nullptr;
    Policy policy;
    std::function<void(TickReport)> done;
    TickPlan plan = TickPlan::Idle;
    std::string state;
    size_t expired = 0, exhausted = 0, ttl = 0, cutover = 0, fanout = 0, inactive = 0;
    bool finished = false;
};
using TickPtr = std::shared_ptr<TickCtx>;

void finishTick(const TickPtr &ctx, TickReport report) {
    if (ctx->finished) return;
    ctx->finished = true;
    ctx->done(std::move(report));
}

void failTick(const TickPtr &ctx, const char *stage) {
    TickReport report;
    report.ok = false;
    // Fixed label: database text may echo values.
    report.detail = std::string("push_dispatch stage=") + stage;
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

std::string countsDetail(const TickCtx &c, size_t claimed) {
    std::ostringstream out;
    out << "control=" << c.state << " expired_leases=" << c.expired << " exhausted=" << c.exhausted
        << " ttl_expired=" << c.ttl << " cutover_skipped=" << c.cutover << " fanout=" << c.fanout
        << " inactive=" << c.inactive << " claimed=" << claimed;
    return out.str();
}

void claimStep(const TickPtr &ctx) {
    if (ctx->plan != TickPlan::Dispatch) {
        TickReport report;
        report.ok = true;
        report.detail = countsDetail(*ctx, 0);
        finishTick(ctx, std::move(report));
        return;
    }
    const auto &p = ctx->policy;
    step(ctx, kClaimSql, "claim",
         [ctx](const drogon::orm::Result &rows) {
             TickReport report;
             report.ok = true;
             for (const auto &row : rows) {
                 report.claims.push_back({row["notification_id"].as<std::string>(),
                                          row["generation_id"].as<std::string>(), row["attempt_no"].as<int>()});
             }
             report.detail = countsDetail(*ctx, report.claims.size());
             finishTick(ctx, std::move(report));
         },
         p.instanceId, numberText(p.leaseSeconds), numberText(p.internalBackoffSeconds), arrayText(p.backoffSeconds),
         std::to_string(p.capacity), std::to_string(p.perRecipient), numberText(p.ttlSeconds));
}

void preClaimStep(const TickPtr &ctx) {
    step(ctx, kPreClaimInactiveSql, "pre_claim",
         [ctx](const drogon::orm::Result &rows) {
             ctx->inactive = rows.size();
             claimStep(ctx);
         },
         ctx->policy.vapidKeyId, std::to_string(ctx->policy.maintenanceLimit));
}

void fanoutStep(const TickPtr &ctx) {
    step(ctx, kFanoutSql, "fanout",
         [ctx](const drogon::orm::Result &rows) {
             ctx->fanout = rows.size();
             preClaimStep(ctx);
         },
         numberText(ctx->policy.ttlSeconds), ctx->policy.vapidKeyId, std::to_string(ctx->policy.fanoutLimit));
}

void cutoverStep(const TickPtr &ctx) {
    step(ctx, kCutoverSql, "cutover",
         [ctx](const drogon::orm::Result &rows) {
             ctx->cutover = rows.size();
             fanoutStep(ctx);
         },
         std::to_string(ctx->policy.maintenanceLimit));
}

void ttlStep(const TickPtr &ctx) {
    step(ctx, kExpiredSql, "ttl",
         [ctx](const drogon::orm::Result &rows) {
             ctx->ttl = rows.size();
             cutoverStep(ctx);
         },
         numberText(ctx->policy.ttlSeconds), std::to_string(ctx->policy.maintenanceLimit));
}

void exhaustedStep(const TickPtr &ctx) {
    step(ctx, kExhaustedSql, "exhausted",
         [ctx](const drogon::orm::Result &rows) {
             ctx->exhausted = rows.size();
             ttlStep(ctx);
         },
         std::to_string(ctx->policy.maintenanceLimit));
}

void expireStep(const TickPtr &ctx) {
    step(ctx, kExpireSql, "lease_expiry",
         [ctx](const drogon::orm::Result &rows) {
             ctx->expired = rows.size();
             exhaustedStep(ctx);
         },
         std::to_string(ctx->policy.maintenanceLimit));
}

// ---------------------------------------------------------------- recording
struct RecordCtx {
    std::shared_ptr<db::TxScope> scope;
    AttemptKey key;
    SendResult result;
    RecordStatus status;
    std::string recipientId;
    std::string coupleId;
    std::function<void(RecordStatus)> done;
    bool completed = false;
};
using RecordPtr = std::shared_ptr<RecordCtx>;

void completeRecord(const RecordPtr &ctx, RecordStatus status) {
    if (ctx->completed) return;
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

template <typename... Args>
void recordStep(const RecordPtr &ctx, const char *sql, const char *stage,
                std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    ctx->scope->tx().execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                ctx->scope->fail(std::string("push_record stage=") + stage);
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) {
            ctx->scope->fail(std::string("push_record stage=") + stage);
        },
        std::forward<Args>(args)...);
}

void finishRecord(const RecordPtr &ctx) {
    recordStep(ctx, kFinalSql, "final",
               [ctx](const drogon::orm::Result &rows) {
                   if (rows.size() == 1u) ctx->status.finalState = rows[0]["final_state"].as<std::string>();
                   ctx->status.ok = true;
                   ctx->scope->commit([ctx]() { completeRecord(ctx, ctx->status); });
               },
               ctx->key.notificationId, ctx->key.generationId);
}

// A pause only on the first recording (a duplicate must not pause again) and only for a claim of the
// current control revision (kPauseSql). The observation is already stored either way.
void pauseIfNeeded(const RecordPtr &ctx) {
    const std::string reason = pauseReasonForOutcome(ctx->status.storedOutcome);
    if (reason.empty() || !ctx->status.inserted) {
        finishRecord(ctx);
        return;
    }
    recordStep(ctx, kPauseLockSql, "pause_lock",
               [ctx, reason](const drogon::orm::Result &) {
                   recordStep(ctx, kPauseSql, "pause",
                              [ctx](const drogon::orm::Result &) { finishRecord(ctx); }, reason,
                              ctx->key.notificationId, ctx->key.generationId, std::to_string(ctx->key.attemptNo));
               });
}

void insertOutcome(const RecordPtr &ctx, const std::string &state, bool withAttempt) {
    recordStep(ctx, kOutcomeSql, "outcome",
               [ctx](const drogon::orm::Result &) { pauseIfNeeded(ctx); }, ctx->key.notificationId,
               ctx->key.generationId, state, withAttempt ? std::to_string(ctx->key.attemptNo) : std::string());
}

void applySideEffects(const RecordPtr &ctx) {
    const std::string &stored = ctx->status.storedOutcome;
    const std::string terminal = terminalForOutcome(stored);
    if (stored == "gone") {
        recordStep(ctx, kGoneSql, "gone",
                   [ctx](const drogon::orm::Result &) { insertOutcome(ctx, "gone", true); }, ctx->key.generationId,
                   ctx->recipientId, ctx->coupleId, ctx->key.notificationId, std::to_string(ctx->key.attemptNo));
        return;
    }
    if (!terminal.empty()) {
        insertOutcome(ctx, terminal, stored == "accepted" || stored == "rejected" || stored == "source_stale_skipped");
        return;
    }
    if (outcomeClass(stored) == "provider" && ctx->status.providerAttempts >= kProviderBudget) {
        insertOutcome(ctx, "exhausted", true);
        return;
    }
    pauseIfNeeded(ctx);
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

std::string outcomeClass(const std::string &outcome) {
    for (const char *o : {"accepted", "gone", "rejected", "transient", "unknown", "provider_auth_failed"}) {
        if (outcome == o) return "provider";
    }
    for (const char *o : {"worker_auth_failed", "transport_unavailable", "busy", "inactive_skipped", "ttl_skipped",
                          "cutover_skipped", "control_skipped", "terminal_skipped", "lease_lost_skipped",
                          "source_stale_skipped", "notice_ahead_skipped"}) {
        if (outcome == o) return "internal";
    }
    return "";
}

bool validResult(const SendResult &result) {
    if (outcomeClass(result.outcome).empty()) return false;
    if (result.httpStatus && (*result.httpStatus < 100 || *result.httpStatus > 599)) return false;
    return !result.retryAfterSeconds || (*result.retryAfterSeconds >= 0 && *result.retryAfterSeconds <= 86400);
}

std::string terminalForOutcome(const std::string &outcome) {
    if (outcome == "accepted" || outcome == "gone" || outcome == "rejected") return outcome;
    if (outcome == "inactive_skipped") return "inactive";
    if (outcome == "ttl_skipped") return "expired";
    if (outcome == "cutover_skipped") return "cutover_skipped";
    if (outcome == "source_stale_skipped") return "source_stale";
    return "";
}

std::string pauseReasonForOutcome(const std::string &outcome) {
    return outcome == "worker_auth_failed" || outcome == "provider_auth_failed" ? outcome : std::string();
}

TickPlan planTick(const std::string &controlState, bool capable) {
    if (controlState == "enabled") return capable ? TickPlan::Dispatch : TickPlan::FailClosed;
    if (controlState == "paused") return capable ? TickPlan::Maintain : TickPlan::FailClosed;
    return TickPlan::Idle;
}

PreSend decidePreSend(const PreSendFacts &f) {
    // The exact attempt first: a claim known to have lost its lease never starts a request.
    if (!f.ownClaim) return PreSend::NotOwnClaim;
    if (f.observed) return PreSend::AlreadyObserved;
    if (f.expired || f.superseded || !f.leaseValid) return PreSend::LeaseLost;
    if (f.hasOutcome) return PreSend::Terminal;
    if (f.ttlReached) return PreSend::Ttl;
    if (f.beforeCutover) return PreSend::Cutover;
    if (f.controlState != "enabled") return PreSend::Control;
    if (!f.generationActive) return PreSend::Inactive;
    // Not terminal: the attempt ends with an internal observation and the delivery is judged again at its retry. Before
    // the source check, because the source's validity is read from the same clock that is behind.
    if (f.noticeAhead) return PreSend::NoticeAhead;
    // Last, so every fence above answers exactly as it does for any other notification: the delivery's own lifetime
    // wins over a stale source, and a pause keeps the attempt pending, to be judged against the source at its retry.
    if (f.sourceStale) return PreSend::SourceStale;
    if (f.windowUnderASecond) return PreSend::Ttl;
    return PreSend::Send;
}

int boundedTtlSeconds(long long remainingSeconds, double lifetimeSeconds) {
    long long limit = kMaxWorkerTtlSeconds;
    if (lifetimeSeconds < static_cast<double>(limit)) limit = lifetimeSeconds < 0.0 ? 0 : static_cast<long long>(lifetimeSeconds);
    if (remainingSeconds > limit) return static_cast<int>(limit);
    return remainingSeconds < 0 ? 0 : static_cast<int>(remainingSeconds);
}

std::string skipOutcome(PreSend decision) {
    switch (decision) {
        case PreSend::LeaseLost: return "lease_lost_skipped";
        case PreSend::Terminal: return "terminal_skipped";
        case PreSend::Ttl: return "ttl_skipped";
        case PreSend::Cutover: return "cutover_skipped";
        case PreSend::Control: return "control_skipped";
        case PreSend::Inactive: return "inactive_skipped";
        case PreSend::SourceStale: return "source_stale_skipped";
        case PreSend::NoticeAhead: return "notice_ahead_skipped";
        case PreSend::Send:
        case PreSend::NotOwnClaim:
        case PreSend::AlreadyObserved:
            break;
    }
    return "";
}

// =====================================================================
// Dispatcher
// =====================================================================

Dispatcher::Dispatcher(Policy policy, std::shared_ptr<Transport> transport)
    : policy_(std::move(policy)), transport_(std::move(transport)) {}

bool Dispatcher::capable() const {
    return policy_.localEnabled && transport_ != nullptr && !policy_.vapidKeyId.empty() &&
           !policy_.markerSecret.empty();
}

void Dispatcher::runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done) {
    auto ctx = std::make_shared<TickCtx>();
    ctx->tx = &tx;
    ctx->policy = policy_;
    ctx->done = std::move(done);
    const bool isCapable = capable();
    step(ctx, kControlSql, "control",
         [ctx, isCapable](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failTick(ctx, "control"); return; }
             ctx->state = rows[0]["state"].as<std::string>();
             ctx->plan = planTick(ctx->state, isCapable);
             // Off stays off and writes nothing, so a default-off rollout is unaffected.
             if ((ctx->plan == TickPlan::Dispatch || ctx->plan == TickPlan::Maintain) && !rows[0]["source_validity"].as<bool>()) {
                 LOG_ERROR << "push_dispatch: migration 0028_reminder_push_stale is absent; no dispatch";
                 failTick(ctx, "preflight_missing_0028");
                 return;
             }
             if (ctx->plan == TickPlan::Idle) {
                 TickReport report;
                 report.ok = true;
                 report.detail = "control=" + ctx->state + " dispatch=off";
                 finishTick(ctx, std::move(report));
                 return;
             }
             if (ctx->plan == TickPlan::FailClosed) {
                 // A mismatch never writes: it cannot enable, disable or move the cutover.
                 LOG_ERROR << "push_dispatch mismatch: control=" << ctx->state << " local_capability=incomplete; "
                           << "no dispatch";
                 TickReport report;
                 report.ok = true;
                 report.detail = "control=" + ctx->state + " dispatch=fail_closed local_capability=incomplete";
                 finishTick(ctx, std::move(report));
                 return;
             }
             expireStep(ctx);
         });
}

void Dispatcher::sendClaims(const std::vector<AttemptKey> &claims) {
    for (const auto &key : claims) preSend(key);
}

void Dispatcher::preSend(const AttemptKey &key) {
    auto self = shared_from_this();
    db::Pool::instance().client()->execSqlAsync(
        kPreSendSql,
        [self, key](const drogon::orm::Result &rows) {
            auto recordQuietly = [self, key](const SendResult &result) {
                self->recordResult(key, result, [key](RecordStatus status) {
                    if (!status.ok) {
                        LOG_ERROR << "push_dispatch recording failed; the lease will expire. generation="
                                  << key.generationId << " attempt=" << key.attemptNo;
                    }
                });
            };
            try {
                if (rows.size() != 1u) {
                    // No such attempt: nothing to send and nothing an observation could reference.
                    LOG_ERROR << "push_dispatch pre-send found no attempt; not sent. generation="
                              << key.generationId << " attempt=" << key.attemptNo;
                    return;
                }
                const auto &row = rows[0];
                PreSendFacts facts;
                facts.controlState = row["control_state"].as<std::string>();
                facts.beforeCutover = row["before_cutover"].as<bool>();
                // Within the worker's range and never past the lifetime, whatever the clock did. The expiry below is untouched.
                const int remainingTtl = boundedTtlSeconds(row["remaining_ttl"].as<long long>(), self->policy_.ttlSeconds);
                // Under a second left is nothing a provider could use.
                facts.ttlReached = row["ttl_reached"].as<bool>() || row["delivery_remaining"].as<long long>() < 1;
                facts.noticeAhead = row["notice_ahead"].as<bool>();
                facts.sourceStale = !row["source_valid"].as<bool>();
                facts.windowUnderASecond = remainingTtl < 1;
                facts.hasOutcome = row["has_outcome"].as<bool>();
                facts.generationActive = row["generation_active"].as<bool>();
                facts.ownClaim = row["own_claim"].as<bool>();
                facts.leaseValid = row["lease_valid"].as<bool>();
                facts.observed = row["observed"].as<bool>();
                facts.expired = row["expired"].as<bool>();
                facts.superseded = row["superseded"].as<bool>();
                const PreSend decision = decidePreSend(facts);
                if (decision != PreSend::Send) {
                    const std::string skip = skipOutcome(decision);
                    if (!skip.empty()) recordQuietly({skip, std::nullopt, std::nullopt});
                    return;
                }
                SendRequest request;
                request.key = key;
                request.eventType = row["event_type"].as<std::string>();
                request.targetKind = row["target_kind"].as<std::string>();
                request.targetId = row["target_id"].as<std::string>();
                request.endpoint = row["endpoint"].as<std::string>();
                request.p256dh = row["p256dh"].as<std::string>();
                request.auth = row["auth"].as<std::string>();
                request.marker = PushService::markerFor(self->policy_.markerSecret, key.generationId);
                request.ttlSeconds = remainingTtl;
                request.expiresAtMs = row["expires_at_ms"].as<long long>();
                self->transport_->send(request, recordQuietly);
            } catch (const std::exception &) {
                recordQuietly({"transport_unavailable", std::nullopt, std::nullopt});
            }
        },
        [key](const drogon::orm::DrogonDbException &) {
            // Nothing was sent. The lease expires and the next tick accounts for it conservatively.
            LOG_ERROR << "push_dispatch pre-send check failed; not sent. generation=" << key.generationId
                      << " attempt=" << key.attemptNo;
        },
        key.notificationId, key.generationId, policy_.vapidKeyId, numberText(policy_.ttlSeconds),
        std::to_string(key.attemptNo), policy_.instanceId);
}

void Dispatcher::recordResult(const AttemptKey &key, const SendResult &result, std::function<void(RecordStatus)> done) {
    // Invalid input from a transport is an unknown provider result, never a dropped attempt.
    SendResult normalized = result;
    if (!validResult(normalized)) normalized = {"unknown", std::nullopt, std::nullopt};
    auto ctx = std::make_shared<RecordCtx>();
    ctx->key = key;
    ctx->result = normalized;
    ctx->done = std::move(done);
    std::weak_ptr<RecordCtx> weak = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            const auto &k = ctx->key;
            const auto &r = ctx->result;
            recordStep(ctx, kObservationSql, "observation",
                       [ctx](const drogon::orm::Result &inserted) {
                           ctx->status.inserted = inserted.size() == 1u;
                           const auto &key2 = ctx->key;
                           recordStep(ctx, kStoredSql, "stored",
                                      [ctx](const drogon::orm::Result &rows) {
                                          if (rows.size() != 1u) {
                                              ctx->scope->fail("push_record stage=stored");
                                              return;
                                          }
                                          ctx->status.storedOutcome = rows[0]["outcome"].as<std::string>();
                                          ctx->status.providerAttempts = rows[0]["used"].as<int>();
                                          ctx->recipientId = rows[0]["recipient_id"].as<std::string>();
                                          ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                                          applySideEffects(ctx);
                                      },
                                      key2.notificationId, key2.generationId, std::to_string(key2.attemptNo));
                       },
                       k.notificationId, k.generationId, std::to_string(k.attemptNo), r.outcome, outcomeClass(r.outcome),
                       r.httpStatus ? std::to_string(*r.httpStatus) : std::string(),
                       r.retryAfterSeconds ? std::to_string(*r.retryAfterSeconds) : std::string());
        },
        [weak](const std::string &) {
            // Fixed labels only. Before the commit callback nothing is recorded as confirmed.
            if (auto strong = weak.lock()) {
                RecordStatus failed;
                failed.ok = false;
                completeRecord(strong, failed);
            }
        });
}

}  // namespace usapp::push
