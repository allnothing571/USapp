// =====================================================================
// services/api/src/services/MediaService.cc
// =====================================================================
#include "services/MediaService.h"
#include "services/MediaEligibility.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AlbumService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

const char *kMemberSql = "SELECT m.couple_id::text AS couple_id FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";
const char *kCoupleLockSql = "SELECT pg_advisory_xact_lock(33019::integer, hashtext($1::text))";
const char *kMediaLockSql = "SELECT pg_advisory_xact_lock(33018::integer, hashtext($1::text))";
const char *kProblemSql = "SELECT usapp_media_grant_problem($1::uuid, $2::uuid, $3::bigint) AS problem";

const char *kByKeySql =
    "SELECT o.id::text AS id, o.request_fingerprint FROM media_objects o WHERE o.uploader_id = $1::uuid AND o.idempotency_key = $2";

const char *kInsertSql =
    "WITH n AS (SELECT gen_random_uuid() AS id) "
    "INSERT INTO media_objects (id, couple_id, uploader_id, staging_key, declared_type, declared_size, expires_at, "
    "idempotency_key, request_fingerprint) "
    "SELECT n.id, $1::uuid, $2::uuid, 'staging/' || $1::uuid::text || '/' || n.id::text || '/upload', $3::text, $4::bigint, "
    "clock_timestamp() + make_interval(secs => $5::double precision), $6::text, $7::text FROM n "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";

// Everything about one object, reached only through the caller's own couple. No row: not theirs, or absent.
const char *kFactsSql =
    "SELECT o.id::text AS id, o.uploader_id::text AS uploader_id, o.staging_key, o.declared_type, o.declared_size, "
    "floor(extract(epoch FROM o.expires_at))::bigint AS expires_epoch, " USAPP_TS("o.expires_at") " AS expires_at, "
    USAPP_TS("o.created_at") " AS created_at, t.outcome, t.reason, t.real_format, t.width, t.height, a.final_key, "
    "EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id) AS planned, "
    "EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything') AS removing, "
    "usapp_media_shared_now(o.id) AS shared, "
    "usapp_media_pending_reference(o.id) IS NOT NULL AS reserved, "
    "usapp_media_deleted_now(o.id) AS deleted, "
    "clock_timestamp() >= o.expires_at AS grant_ended, clock_timestamp() > o.expires_at + interval '15 minutes' AS window_ended "
    "FROM couple_members m JOIN media_objects o ON o.couple_id = m.couple_id AND o.id = $2::uuid "
    "LEFT JOIN media_upload_outcomes t ON t.media_id = o.id "
    "LEFT JOIN media_promotion_attempts a ON a.media_id = t.media_id AND a.attempt_no = t.attempt_no "
    "WHERE m.user_id = $1::uuid";

const char *kStateSql =
    "SELECT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = $1::uuid) AS decided, "
    "EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = $1::uuid) AS planned, "
    "(SELECT count(*) FROM media_promotion_attempts a WHERE a.media_id = $1::uuid)::integer AS attempts";

const char *kAttemptSql =
    "INSERT INTO media_promotion_attempts (media_id, attempt_no, couple_id, final_key) "
    "SELECT o.id, (SELECT COALESCE(max(a.attempt_no), 0) + 1 FROM media_promotion_attempts a WHERE a.media_id = o.id), o.couple_id, "
    "'media/' || o.couple_id::text || '/' || o.id::text || '/' || gen_random_uuid()::text || '/original' "
    "FROM media_objects o WHERE o.id = $1::uuid RETURNING attempt_no, final_key";

const char *kOutcomeSql =
    "INSERT INTO media_upload_outcomes (media_id, couple_id, outcome, reason, attempt_no, real_format, width, height, stored_size, etag, crc64) "
    "VALUES ($1::uuid, $2::uuid, $3, NULLIF($4, ''), NULLIF($5, '')::integer, NULLIF($6, ''), NULLIF($7, '')::integer, "
    "NULLIF($8, '')::integer, NULLIF($9, '')::bigint, NULLIF($10, ''), NULLIF($11, ''))";

// Late writes are expected until the last attempt and the grant have both had their grace.
const char *kPlanSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) "
    "SELECT o.id, $2::text, $3::text, GREATEST(o.expires_at, COALESCE((SELECT max(a.created_at) FROM media_promotion_attempts a "
    "WHERE a.media_id = o.id), o.created_at)) + make_interval(secs => $4::double precision) "
    "FROM media_objects o WHERE o.id = $1::uuid";

// Ids and fixed labels only.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, 'media.upload_decided', 'media', $3::text, $4::jsonb)";

const char *kDispositionSql = "SELECT usapp_media_key_disposition($1) AS state";

const char *kLateSql =
    "INSERT INTO media_late_objects (object_key, disposition, source) VALUES ($1, $2, $3) ON CONFLICT DO NOTHING";

// The job. Candidates are read, locked in sorted order, then re-evaluated in a new statement, so a commit that
// finished before the locks were granted drops a row instead of failing the tick.
#define USAPP_ABANDONED_WHERE                                                                                    \
    "clock_timestamp() >= o.expires_at + make_interval(secs => $1::double precision) "                           \
    "AND NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = o.id) "                            \
    "AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything')"
#define USAPP_UNATTACHED_WHERE                                                                                   \
    "t.outcome = 'ready' AND clock_timestamp() >= usapp_media_released_at(o.id) + make_interval(secs => $1::double precision) " \
    "AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything')"

const char *kAbandonedCandidatesSql =
    "SELECT COALESCE(jsonb_agg(c.id), '[]'::jsonb)::text AS ids FROM (SELECT o.id FROM media_objects o WHERE " USAPP_ABANDONED_WHERE
    " ORDER BY o.id LIMIT $2::integer) c";
const char *kUnattachedCandidatesSql =
    "SELECT COALESCE(jsonb_agg(c.id), '[]'::jsonb)::text AS ids FROM (SELECT o.id FROM media_objects o "
    "JOIN media_upload_outcomes t ON t.media_id = o.id WHERE " USAPP_UNATTACHED_WHERE " ORDER BY o.id LIMIT $2::integer) c";
const char *kLockCandidatesSql =
    "SELECT pg_advisory_xact_lock(33018::integer, k.h) FROM (SELECT DISTINCT hashtext(x) AS h "
    "FROM jsonb_array_elements_text($1::jsonb) x ORDER BY 1) k";
const char *kPlanAbandonedSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) "
    "SELECT o.id, 'everything', 'abandoned', GREATEST(o.expires_at, COALESCE((SELECT max(a.created_at) FROM media_promotion_attempts a "
    "WHERE a.media_id = o.id), o.created_at)) + make_interval(secs => $3::double precision) "
    "FROM media_objects o WHERE o.id IN (SELECT x::uuid FROM jsonb_array_elements_text($2::jsonb) x) AND " USAPP_ABANDONED_WHERE;
const char *kPlanUnattachedSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before) "
    "SELECT o.id, 'everything', 'unattached', GREATEST(o.expires_at, COALESCE((SELECT max(a.created_at) FROM media_promotion_attempts a "
    "WHERE a.media_id = o.id), o.created_at)) + make_interval(secs => $3::double precision) "
    "FROM media_objects o JOIN media_upload_outcomes t ON t.media_id = o.id "
    "WHERE o.id IN (SELECT x::uuid FROM jsonb_array_elements_text($2::jsonb) x) AND " USAPP_UNATTACHED_WHERE;

// The READY key is never in a leftovers list: the attempt filter excludes it.
// D6. The exact cycle, on the database clock. Read as a hint, then again in the plan's own statement, after the locks.
#define USAPP_PURGE_WHERE                                                                                        \
    "NOT usapp_media_undo_open(d.purge_after, clock_timestamp()) "                                               \
    "AND NOT EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = d.id) "                      \
    "AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = d.media_id AND p.scope = 'everything')"
const char *kPurgeCandidatesSql =
    "SELECT COALESCE(jsonb_agg(c.id), '[]'::jsonb)::text AS ids FROM (SELECT d.media_id AS id FROM media_deletions d WHERE " USAPP_PURGE_WHERE
    " ORDER BY d.purge_after, d.media_id LIMIT $1::integer) c";
const char *kPlanPurgeSql =
    "INSERT INTO media_cleanup_plans (media_id, scope, reason, sweep_not_before, deletion_id) "
    "SELECT d.media_id, 'everything', 'user_deleted', GREATEST(o.expires_at, COALESCE((SELECT max(a.created_at) FROM media_promotion_attempts a "
    "WHERE a.media_id = o.id), o.created_at)) + make_interval(secs => $2::double precision), d.id "
    "FROM media_deletions d JOIN media_objects o ON o.id = d.media_id "
    "WHERE d.media_id IN (SELECT x::uuid FROM jsonb_array_elements_text($1::jsonb) x) AND " USAPP_PURGE_WHERE;

// ---- D6 deletion and undo. Ids and fixed labels only.
const char *kKeyLockSql = "SELECT pg_advisory_xact_lock(33025::integer, hashtext(jsonb_build_array($1::text, $2::text, $3::text)::text))";
const char *kDeletionByKeySql =
    "SELECT d.id::text AS id, d.request_fingerprint FROM media_deletions d WHERE d.deleted_by = $1::uuid AND d.idempotency_key = $2";
const char *kUndoByKeySql =
    "SELECT r.deletion_id::text AS id, r.request_fingerprint FROM media_deletion_reversals r WHERE r.reversed_by = $1::uuid AND r.idempotency_key = $2";
// Read after 33018. No row: absent, or in another couple.
const char *kDeleteStateSql =
    "SELECT o.uploader_id::text AS uploader_id, EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = o.id AND t.outcome = 'ready') AS ready, "
    "EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything') AS removing, "
    "usapp_media_deleted_now(o.id) AS deleted FROM media_objects o WHERE o.id = $1::uuid AND o.couple_id = $2::uuid";
const char *kUndoStateSql =
    "SELECT d.deleted_by::text AS deleted_by, EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = d.id) AS reversed, "
    "EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = d.media_id AND p.scope = 'everything') AS removing, "
    "EXISTS (SELECT 1 FROM memory_deletion_photo_cycles l WHERE l.media_deletion_id = d.id) AS linked, "
    "usapp_media_undo_open(d.purge_after, clock_timestamp()) AS open "
    "FROM media_deletions d WHERE d.id = $1::uuid AND d.media_id = $2::uuid AND d.couple_id = $3::uuid";
// The deadline is computed once, from the database clock.
const char *kDeletionInsertSql =
    "INSERT INTO media_deletions (id, media_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
    "SELECT COALESCE(NULLIF($6::text, '')::uuid, gen_random_uuid()), $1::uuid, $2::uuid, $3::uuid, "
    "1 + COALESCE((SELECT max(x.cycle_no) FROM media_deletions x WHERE x.media_id = $1::uuid), 0), $4::text, $5::text, n.t, n.t + interval '168 hours' "
    "FROM (SELECT clock_timestamp() AS t) n RETURNING id::text AS id";
const char *kReversalInsertSql =
    "INSERT INTO media_deletion_reversals (deletion_id, media_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text)";
const char *kDeletionAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'media', $4::text, $5::jsonb)";
const char *kDeletionFactsSql =
    "SELECT json_build_object('deletion_id', d.id, 'media_id', d.media_id, 'cycle_no', d.cycle_no, "
    "'deleted_at', " USAPP_TS("d.deleted_at") ", 'purge_after', " USAPP_TS("d.purge_after") ", 'undone_at', " USAPP_TS("r.reversed_at") ", "
    "'state', CASE WHEN r.deletion_id IS NOT NULL THEN 'undone' "
    "WHEN EXISTS (SELECT 1 FROM media_cleanup_passes s WHERE s.media_id = d.media_id AND s.scope = 'everything' AND s.kind = 'sweep') THEN 'purged' "
    "WHEN EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.deletion_id = d.id) THEN 'purging' ELSE 'deleted' END)::text AS deletion_json "
    "FROM media_deletions d LEFT JOIN media_deletion_reversals r ON r.deletion_id = d.id WHERE d.id = $1::uuid";

// The caller's own undoable cycles, newest first. Exactly the undo's own conditions, on the database clock. No row: not in a space.
// One row of NULLs: nothing to undo. Nothing here reads a key, and one statement answers the whole page.
//
// The context is the photo's one binding, read through the same rule the album and memory reads use: the parent belongs to the caller's
// couple, and is not deleted. A parent that fails it, or has no current text, is 'unavailable', with no id, title or even its kind. A pending request is never
// context, because it is not a binding. A position exists only where the memory's current revision states it: an album's order shifts as
// photos come and go, so it has none.
#define USAPP_UNDOABLE_SQL(filter, limit)                                                                        \
    "SELECT x.* FROM couple_members c LEFT JOIN LATERAL (SELECT json_build_object('deletion_id', d.id, 'media_id', d.media_id, " \
    "'deleted_at', " USAPP_TS("d.deleted_at") ", 'purge_after', " USAPP_TS("d.purge_after") ", "                 \
    "'uploaded_at', " USAPP_TS("o.created_at") ", 'context', json_build_object("                                 \
    "'kind', CASE WHEN b.media_id IS NULL THEN 'none' WHEN mp.label IS NOT NULL THEN 'memory' "                  \
    "             WHEN ap.label IS NOT NULL THEN 'album' ELSE 'unavailable' END, "                               \
    "'relation', CASE WHEN mp.label IS NOT NULL THEN CASE WHEN cur.position IS NOT NULL THEN 'current' ELSE 'former' END " \
    "                 WHEN ap.label IS NOT NULL THEN CASE WHEN EXISTS (SELECT 1 FROM album_items i WHERE i.album_id = b.target_id AND i.media_id = d.media_id " \
    "                      AND NOT EXISTS (SELECT 1 FROM album_item_removals rm WHERE rm.album_id = i.album_id AND rm.media_id = i.media_id)) " \
    "                      THEN 'current' ELSE 'former' END END, "                                               \
    "'target_id', CASE WHEN mp.label IS NOT NULL OR ap.label IS NOT NULL THEN b.target_id END, "                 \
    "'label', COALESCE(mp.label, ap.label), 'position', cur.position))::text AS deletion_json, "                 \
    USAPP_TS("d.deleted_at") " AS sort_at, d.id::text AS sort_id FROM media_deletions d "                        \
    "JOIN media_objects o ON o.id = d.media_id LEFT JOIN media_bindings b ON b.media_id = d.media_id "           \
    "LEFT JOIN LATERAL (SELECT tx.title AS label, rv.version FROM memories pm "                                  \
    "  JOIN LATERAL (SELECT r.version, r.text_id FROM memory_revisions r WHERE r.memory_id = pm.id ORDER BY r.version DESC LIMIT 1) rv ON true " \
    "  JOIN memory_texts tx ON tx.id = rv.text_id "                                                              \
    "  WHERE b.target_kind = 'memory' AND pm.id = b.target_id AND pm.couple_id = c.couple_id "                   \
    "    AND NOT usapp_memory_deleted_now(pm.id)) mp ON true "                                                   \
    "LEFT JOIN LATERAL (SELECT tx.name AS label FROM albums pa "                                                 \
    "  JOIN LATERAL (SELECT r.text_id FROM album_revisions r WHERE r.album_id = pa.id ORDER BY r.version DESC LIMIT 1) rv ON true " \
    "  JOIN album_texts tx ON tx.id = rv.text_id "                                                               \
    "  WHERE b.target_kind = 'album' AND pa.id = b.target_id AND pa.couple_id = c.couple_id) ap ON true "        \
    "LEFT JOIN LATERAL (SELECT ph.position FROM memory_revision_photos ph "                                      \
    "  WHERE ph.memory_id = b.target_id AND ph.version = mp.version AND ph.media_id = d.media_id) cur ON true "  \
    "WHERE d.deleted_by = c.user_id AND d.couple_id = c.couple_id "                                              \
    "AND usapp_media_undo_open(d.purge_after, clock_timestamp()) "                                               \
    "AND NOT EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = d.id) "                      \
    "AND NOT EXISTS (SELECT 1 FROM memory_deletion_photo_cycles l WHERE l.media_deletion_id = d.id) "            \
    "AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = d.media_id AND p.scope = 'everything')" filter \
    " ORDER BY d.deleted_at DESC, d.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kUndoableFirstSql = USAPP_UNDOABLE_SQL("", "$2::integer");
const char *kUndoableAfterSql = USAPP_UNDOABLE_SQL(" AND (d.deleted_at, d.id) < ($2::timestamptz, $3::uuid)", "$4::integer");
#undef USAPP_UNDOABLE_SQL

// D6 M1c. The caller's own READY, undeleted photos, bound in the caller's couple to a memory that is deleted now. No parent id or text.
// No row: not in a space. One row of NULLs: none. A photo leaves the list when its uploader deletes it, or its memory comes back.
#define USAPP_RETAINED_SQL(filter, limit)                                                                        \
    "SELECT x.* FROM couple_members c LEFT JOIN LATERAL (SELECT json_build_object('media_id', o.id, "            \
    "'uploaded_at', " USAPP_TS("o.created_at") ", 'width', t.width, 'height', t.height)::text AS deletion_json, " \
    USAPP_TS("o.created_at") " AS sort_at, o.id::text AS sort_id FROM media_objects o "                          \
    "JOIN media_upload_outcomes t ON t.media_id = o.id AND t.outcome = 'ready' "                                 \
    "JOIN media_bindings b ON b.media_id = o.id AND b.couple_id = c.couple_id AND b.target_kind = 'memory' "     \
    "JOIN memories pm ON pm.id = b.target_id AND pm.couple_id = c.couple_id "                                    \
    "WHERE o.uploader_id = c.user_id AND o.couple_id = c.couple_id AND usapp_memory_deleted_now(pm.id) "         \
    "AND NOT usapp_media_deleted_now(o.id) "                                                                     \
    "AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything')" filter \
    " ORDER BY o.created_at DESC, o.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kRetainedFirstSql = USAPP_RETAINED_SQL("", "$2::integer");
const char *kRetainedAfterSql = USAPP_RETAINED_SQL(" AND (o.created_at, o.id) < ($2::timestamptz, $3::uuid)", "$4::integer");
#undef USAPP_RETAINED_SQL

// The caller's own READY objects that may take their first home now, by the one shared definition. No row: not in a space. One row of
// NULLs: none. A snapshot: cleanup, deletion or another attachment may win before the caller's attachment, whose guards decide.
#define USAPP_UNATTACHED_SQL(filter, limit)                                                                      \
    "SELECT x.* FROM couple_members c LEFT JOIN LATERAL (SELECT json_build_object('media_id', o.id, "            \
    "'uploaded_at', " USAPP_TS("o.created_at") ", 'width', t.width, 'height', t.height)::text AS deletion_json, " \
    USAPP_TS("o.created_at") " AS sort_at, o.id::text AS sort_id FROM media_objects o "                          \
    "JOIN media_upload_outcomes t ON t.media_id = o.id AND t.outcome = 'ready' "                                 \
    "WHERE o.uploader_id = c.user_id AND o.couple_id = c.couple_id AND " USAPP_MEDIA_UNHOMED("o") filter        \
    " ORDER BY o.created_at DESC, o.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kUnattachedFirstSql = USAPP_UNATTACHED_SQL("", "$2::integer");
const char *kUnattachedAfterSql = USAPP_UNATTACHED_SQL(" AND (o.created_at, o.id) < ($2::timestamptz, $3::uuid)", "$4::integer");
#undef USAPP_UNATTACHED_SQL

const char *kWorkSql =
    "SELECT p.media_id::text AS media_id, p.scope, clock_timestamp() >= p.sweep_not_before AS sweep_due, "
    "(SELECT COALESCE(jsonb_agg(k.key), '[]'::jsonb)::text FROM ("
    "  SELECT o.staging_key AS key UNION ALL SELECT a.final_key FROM media_promotion_attempts a "
    "  WHERE a.media_id = p.media_id AND (p.scope = 'everything' OR a.attempt_no IS DISTINCT FROM t.attempt_no)) k) AS keys "
    "FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id "
    "LEFT JOIN media_upload_outcomes t ON t.media_id = p.media_id "
    "WHERE NOT EXISTS (SELECT 1 FROM media_cleanup_passes s WHERE s.media_id = p.media_id AND s.scope = p.scope AND s.kind = 'sweep') "
    "AND (clock_timestamp() >= p.sweep_not_before OR NOT EXISTS (SELECT 1 FROM media_cleanup_passes f "
    "     WHERE f.media_id = p.media_id AND f.scope = p.scope AND f.kind = 'first')) "
    "ORDER BY p.created_at, p.media_id, p.scope LIMIT $1::integer";
// J2c2: the same rule, after a cursor. Journaled work can stay unresolved (an unjournaled deletion, a journal outage), and in creation
// order such items would hold the whole window forever and starve every later plan. The cursor is memory only.
const char *kWorkAfterSql =
    "SELECT p.media_id::text AS media_id, p.scope, clock_timestamp() >= p.sweep_not_before AS sweep_due, p.created_at::text AS created_text, "
    "(SELECT COALESCE(jsonb_agg(k.key), '[]'::jsonb)::text FROM ("
    "  SELECT o.staging_key AS key UNION ALL SELECT a.final_key FROM media_promotion_attempts a "
    "  WHERE a.media_id = p.media_id AND (p.scope = 'everything' OR a.attempt_no IS DISTINCT FROM t.attempt_no)) k) AS keys "
    "FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id "
    "LEFT JOIN media_upload_outcomes t ON t.media_id = p.media_id "
    "WHERE NOT EXISTS (SELECT 1 FROM media_cleanup_passes s WHERE s.media_id = p.media_id AND s.scope = p.scope AND s.kind = 'sweep') "
    "AND (clock_timestamp() >= p.sweep_not_before OR NOT EXISTS (SELECT 1 FROM media_cleanup_passes f "
    "     WHERE f.media_id = p.media_id AND f.scope = p.scope AND f.kind = 'first')) "
    "AND (p.created_at, p.media_id, p.scope) > ($2::timestamptz, $3::uuid, $4::text) "
    "ORDER BY p.created_at, p.media_id, p.scope LIMIT $1::integer";

const char *kPassSql =
    "INSERT INTO media_cleanup_passes (media_id, scope, kind) VALUES ($1::uuid, $2, $3) ON CONFLICT DO NOTHING";

#undef USAPP_PURGE_WHERE
#undef USAPP_ABANDONED_WHERE
#undef USAPP_UNATTACHED_WHERE
#undef USAPP_TS

using Outcome = MediaService::Outcome;
using Status = MediaService::Status;
using media::KeyPage;
using media::ImageFacts;
using media::ObjectFacts;
using media::ObjectStore;
using media::StoreStatus;

Status make(Outcome outcome, const char *message = "") {
    Status status;
    status.outcome = outcome;
    status.message = message;
    return status;
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::vector<std::string> parseStrings(const std::string &text) {
    Json::CharReaderBuilder builder;
    std::string errors;
    Json::Value out;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::vector<std::string> values;
    if (reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isArray()) {
        for (const auto &item : out) values.push_back(item.asString());
    }
    return values;
}

std::string stateOf(const drogon::orm::Row &row) {
    if (!row["outcome"].isNull() && row["outcome"].as<std::string>() == "rejected") return "rejected";
    if (row["removing"].as<bool>()) return "removed";
    if (!row["outcome"].isNull()) return row["outcome"].as<std::string>();
    if (row["planned"].as<bool>()) return "removed";
    return row["window_ended"].as<bool>() ? "expired" : "granted";
}

Json::Value factsJson(const drogon::orm::Row &row) {
    Json::Value out(Json::objectValue);
    out["media_id"] = row["id"].as<std::string>();
    out["state"] = stateOf(row);
    out["uploader_id"] = row["uploader_id"].as<std::string>();
    out["content_type"] = row["declared_type"].as<std::string>();
    out["size_bytes"] = static_cast<Json::Int64>(row["declared_size"].as<long long>());
    out["created_at"] = row["created_at"].as<std::string>();
    out["reason"] = row["reason"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["reason"].as<std::string>());
    out["width"] = row["width"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["width"].as<int>());
    out["height"] = row["height"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["height"].as<int>());
    return out;
}

// Only the uploader sees an object before it is shared or proposed; a pending proposal shows it to the partner who must review
// it, and a rejection or withdrawal takes that back. Shared means bound to a memory, for good, or an active album item: a removal
// from an album takes the partner's access back after its commit. Once removing, nobody but the uploader sees even its facts.
// D6: once deleted, nobody sees anything, the uploader included.
bool visibleTo(const drogon::orm::Row &row, const std::string &userId) {
    if (row["deleted"].as<bool>()) return false;
    if (row["uploader_id"].as<std::string>() == userId) return true;
    return (row["shared"].as<bool>() || row["reserved"].as<bool>()) && !row["removing"].isNull() && !row["removing"].as<bool>() && !row["outcome"].isNull() &&
           row["outcome"].as<std::string>() == "ready";
}

Json::Value uploadJson(const media::UploadForm &form, const std::string &expiresAt) {
    Json::Value out(Json::objectValue);
    out["url"] = form.url;
    Json::Value fields(Json::objectValue);
    for (const auto &[name, value] : form.fields) fields[name] = value;
    out["fields"] = fields;
    out["expires_at"] = expiresAt;
    return out;
}

template <typename Ctx>
void finish(const std::shared_ptr<Ctx> &ctx, Status status) {
    if (ctx->completed) return;
    ctx->completed = true;
    if (status.outcome == Outcome::Error) LOG_ERROR << "media request failed detail=" << status.detail;
    if (ctx->done) ctx->done(std::move(status));
}

template <typename Ctx>
void failAt(const std::shared_ptr<Ctx> &ctx, const char *stage) {
    ctx->failDetail = std::string("media stage=") + stage;
    if (ctx->scope && !ctx->scope->settled()) {
        ctx->scope->fail(ctx->failDetail);
    } else {
        Status status = make(Outcome::Error);
        status.detail = ctx->failDetail;
        finish(ctx, std::move(status));
    }
}

template <typename Ctx, typename... Args>
void inTx(const std::shared_ptr<Ctx> &ctx, const char *sql, const char *stage,
          std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    ctx->scope->tx().execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                failAt(ctx, stage);
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) { failAt(ctx, stage); }, std::forward<Args>(args)...);
}

template <typename Ctx, typename... Args>
void onPool(const std::shared_ptr<Ctx> &ctx, const char *sql, const char *stage,
            std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    db::Pool::instance().client()->execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                failAt(ctx, stage);
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) { failAt(ctx, stage); }, std::forward<Args>(args)...);
}

template <typename Ctx>
void beginTx(const std::shared_ptr<Ctx> &ctx, std::function<void()> next) {
    std::weak_ptr<Ctx> weak = ctx;
    db::TxScope::beginAsync(
        [ctx, next = std::move(next)](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            next();
        },
        [weak](const std::string &message) {
            auto strong = weak.lock();
            if (!strong) return;
            Status status = make(strong->commitRequested ? Outcome::ResultUnknown : Outcome::Error);
            status.detail = strong->failDetail.empty() ? message : strong->failDetail;
            finish(strong, std::move(status));
        });
}

// ---------------------------------------------------------------- grant
struct GrantCtx {
    std::shared_ptr<db::TxScope> scope;
    std::shared_ptr<ObjectStore> store;
    MediaService::Policy policy;
    std::string userId, key, contentType, fingerprint, coupleId, mediaId;
    int64_t size = 0;
    Outcome outcome = Outcome::Created;
    MediaService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};
using GrantPtr = std::shared_ptr<GrantCtx>;

// A new or replayed grant reads its own row. A form is issued only while the grant can still be used.
void answerGrant(const GrantPtr &ctx) {
    onPool(ctx, kFactsSql, "grant_read",
           [ctx](const drogon::orm::Result &rows) {
               if (rows.size() != 1u) { failAt(ctx, "grant_read"); return; }
               const auto &row = rows[0];
               // D6: a replay of the grant is a read like any other.
               if (row["deleted"].as<bool>()) {
                   finish(ctx, make(Outcome::NotFound, "图片不存在"));
                   return;
               }
               Status status = make(ctx->outcome);
               status.media = factsJson(row);
               status.upload = Json::Value(Json::nullValue);
               if (row["outcome"].isNull() && !row["planned"].as<bool>() && !row["grant_ended"].as<bool>()) {
                   const auto form = ctx->store->signUpload(row["staging_key"].as<std::string>(), row["declared_type"].as<std::string>(),
                                                            row["declared_size"].as<long long>(), row["expires_epoch"].as<long long>());
                   status.upload = uploadJson(form, row["expires_at"].as<std::string>());
               }
               finish(ctx, std::move(status));
           },
           ctx->userId, ctx->mediaId);
}

void classifyGrant(const GrantPtr &ctx, std::function<void()> onMissing) {
    inTx(ctx, kByKeySql, "classify",
         [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onMissing(); return; }
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 ctx->scope->rollback([ctx]() { finish(ctx, make(Outcome::KeyReused, "该请求标识已用于另一项操作")); });
                 return;
             }
             ctx->mediaId = rows[0]["id"].as<std::string>();
             ctx->outcome = Outcome::Replayed;
             ctx->scope->rollback([ctx]() { answerGrant(ctx); });
         },
         ctx->userId, ctx->key);
}

void insertGrant(const GrantPtr &ctx) {
    inTx(ctx, kProblemSql, "limits",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "limits"); return; }
             if (!rows[0]["problem"].isNull()) {
                 ctx->scope->rollback([ctx]() { finish(ctx, make(Outcome::LimitExceeded, "上传数量或空间已达上限")); });
                 return;
             }
             inTx(ctx, kInsertSql, "insert",
                  [ctx](const drogon::orm::Result &inserted) {
                      if (inserted.size() == 0u) {
                          // A concurrent same-key grant committed first.
                          classifyGrant(ctx, [ctx]() { failAt(ctx, "insert"); });
                          return;
                      }
                      ctx->mediaId = inserted[0]["id"].as<std::string>();
                      ctx->commitRequested = true;
                      ctx->scope->commit([ctx]() { answerGrant(ctx); });
                  },
                  ctx->coupleId, ctx->userId, ctx->contentType, std::to_string(ctx->size),
                  std::to_string(ctx->policy.grantSeconds), ctx->key, ctx->fingerprint);
         },
         ctx->userId, ctx->coupleId, std::to_string(ctx->size));
}

// ---------------------------------------------------------------- complete
struct CompleteCtx {
    std::shared_ptr<db::TxScope> scope;
    std::shared_ptr<ObjectStore> store;
    MediaService::Policy policy;
    MediaService::Fault fault = MediaService::Fault::None;
    std::shared_ptr<deletionjournal::Journal> journal;  // J2c2: null keeps the unjournaled removal
    std::string userId, mediaId, coupleId, stagingKey, declaredType, finalKey;
    int64_t declaredSize = 0;
    int attemptNo = 0;
    ObjectFacts objectFacts;
    ImageFacts imageFacts;
    std::string rejection;
    MediaService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};
using CompletePtr = std::shared_ptr<CompleteCtx>;

// The stored truth. Used by the first read, and by every path that found the object already decided.
void answerStored(const CompletePtr &ctx, bool first);

void storeDown(const CompletePtr &ctx) {
    finish(ctx, make(Outcome::StoreUnavailable, "存储暂时不可用，可以重试"));
}

void journaledLateRemoval(std::shared_ptr<deletionjournal::Journal> journal, std::shared_ptr<ObjectStore> store, const std::string &key,
                          const std::string &source, std::function<void()> next);

// This attempt may not publish. Its own key is removed if it is dead. Never another attempt's, and never a key that is
// READY or still live; the reconciler is the backstop if this process stops before getting here.
void abandonAttempt(const CompletePtr &ctx) {
    onPool(ctx, kDispositionSql, "resume_state",
           [ctx](const drogon::orm::Result &rows) {
               const std::string state = rows.size() == 1u ? rows[0]["state"].as<std::string>() : std::string();
               if (state != "late" && state != "pending_sweep") {
                   answerStored(ctx, false);
                   return;
               }
               // J2c2: journaled, and the state is read again after the intent.
               if (ctx->journal) {
                   journaledLateRemoval(ctx->journal, ctx->store, ctx->finalKey, "attempt_resume", [ctx]() { answerStored(ctx, false); });
                   return;
               }
               ctx->store->remove(ctx->finalKey, [ctx](StoreStatus removed) {
                   if (removed != StoreStatus::Ok) {
                       LOG_ERROR << "media resumed attempt could not remove its key; the reconciler will. media=" << ctx->mediaId;
                       answerStored(ctx, false);
                       return;
                   }
                   onPool(ctx, kLateSql, "resume_record",
                          [ctx](const drogon::orm::Result &) { answerStored(ctx, false); }, ctx->finalKey,
                          std::string("removed"), std::string("attempt_resume"));
               });
           },
           ctx->finalKey);
}

// Step 7. Under the lock: if an outcome or a plan appeared, this attempt publishes nothing.
void commitOutcome(const CompletePtr &ctx) {
    beginTx(ctx, [ctx]() {
        inTx(ctx, kMemberSql, "outcome_member",
             [ctx](const drogon::orm::Result &members) {
                 if (members.size() != 1u) { failAt(ctx, "outcome_member"); return; }
                 inTx(ctx, kMediaLockSql, "outcome_lock",
                      [ctx](const drogon::orm::Result &) {
                          inTx(ctx, kStateSql, "outcome_state",
                               [ctx](const drogon::orm::Result &rows) {
                                   if (rows.size() != 1u) { failAt(ctx, "outcome_state"); return; }
                                   if (rows[0]["decided"].as<bool>() || rows[0]["planned"].as<bool>()) {
                                       ctx->scope->rollback([ctx]() { abandonAttempt(ctx); });
                                       return;
                                   }
                                   const bool ready = ctx->rejection.empty();
                                   auto &o = ctx->objectFacts;
                                   auto &i = ctx->imageFacts;
                                   inTx(ctx, kOutcomeSql, "outcome",
                                        [ctx, ready](const drogon::orm::Result &) {
                                            inTx(ctx, kPlanSql, "plan",
                                                 [ctx, ready](const drogon::orm::Result &) {
                                                     Json::Value detail(Json::objectValue);
                                                     detail["media_id"] = ctx->mediaId;
                                                     detail["outcome"] = ready ? "ready" : "rejected";
                                                     detail["reason"] = ready ? Json::Value(Json::nullValue) : Json::Value(ctx->rejection);
                                                     inTx(ctx, kAuditSql, "audit",
                                                          [ctx](const drogon::orm::Result &) {
                                                              if (ctx->fault == MediaService::Fault::FailBeforeOutcomeCommit) {
                                                                  failAt(ctx, "forced_fault");
                                                                  return;
                                                              }
                                                              ctx->commitRequested = true;
                                                              ctx->scope->commit([ctx]() { answerStored(ctx, false); });
                                                          },
                                                          ctx->userId, ctx->coupleId, ctx->mediaId, compactJson(detail));
                                                 },
                                                 ctx->mediaId, std::string(ready ? "leftovers" : "everything"),
                                                 std::string(ready ? "ready" : "rejected"),
                                                 std::to_string(ctx->policy.sweepGraceSeconds));
                                        },
                                        ctx->mediaId, ctx->coupleId, std::string(ready ? "ready" : "rejected"), ctx->rejection,
                                        ready ? std::to_string(ctx->attemptNo) : std::string(), ready ? i.format : std::string(),
                                        ready ? std::to_string(i.width) : std::string(), ready ? std::to_string(i.height) : std::string(),
                                        ready ? std::to_string(o.size) : std::string(), ready ? o.etag : std::string(),
                                        ready ? o.crc64 : std::string());
                               },
                               ctx->mediaId);
                      },
                      ctx->mediaId);
             },
             ctx->userId);
    });
}

// Step 6: both HEADs and the decode read this attempt's own write-once key. A disagreement means a guarantee was
// broken, which is never READY and never a rejection of the user's photo.
void inspect(const CompletePtr &ctx) {
    ctx->store->head(ctx->finalKey, [ctx](StoreStatus first, ObjectFacts before) {
        if (first != StoreStatus::Ok) { storeDown(ctx); return; }
        ctx->store->imageInfo(ctx->finalKey, [ctx, before](StoreStatus decoded, ImageFacts image) {
            if (decoded != StoreStatus::Ok && decoded != StoreStatus::Invalid) { storeDown(ctx); return; }
            ctx->store->head(ctx->finalKey, [ctx, before, decoded, image](StoreStatus second, ObjectFacts after) {
                if (second != StoreStatus::Ok) { storeDown(ctx); return; }
                if (before.etag != after.etag || before.size != after.size) {
                    LOG_ERROR << "media final key changed during inspection; a store guarantee was broken. media=" << ctx->mediaId;
                    storeDown(ctx);
                    return;
                }
                ctx->objectFacts = after;
                ctx->imageFacts = image;
                ctx->rejection = decoded == StoreStatus::Invalid
                                     ? std::string("not_image")
                                     : MediaService::rejectionFor(after.size, ctx->declaredSize, image.format, ctx->declaredType,
                                                                  image.width, image.height);
                commitOutcome(ctx);
            });
        });
    });
}

// Step 5: no store work is spent on an attempt that has already lost.
void fenceThenInspect(const CompletePtr &ctx) {
    onPool(ctx, kStateSql, "fence",
           [ctx](const drogon::orm::Result &rows) {
               if (rows.size() != 1u) { failAt(ctx, "fence"); return; }
               if (rows[0]["decided"].as<bool>() || rows[0]["planned"].as<bool>()) {
                   abandonAttempt(ctx);
                   return;
               }
               inspect(ctx);
           },
           ctx->mediaId);
}

// Step 4. Sent once. A failure report may be untrue, so the key is left to the sweep rather than removed here: a copy
// still on its way could land after any removal.
void promote(const CompletePtr &ctx) {
    ctx->store->copy(ctx->stagingKey, ctx->finalKey, [ctx](StoreStatus copied) {
        if (copied == StoreStatus::Ok) {
            fenceThenInspect(ctx);
        } else if (copied == StoreStatus::NotFound) {
            // The source went away after the HEAD: only cleanup removes it.
            answerStored(ctx, true);
        } else {
            storeDown(ctx);
        }
    });
}

// Step 3. Every key that could exist is known before it can exist. The attempt cap ends a loop of failures.
void startAttempt(const CompletePtr &ctx) {
    beginTx(ctx, [ctx]() {
        inTx(ctx, kMemberSql, "attempt_member",
             [ctx](const drogon::orm::Result &members) {
                 if (members.size() != 1u) { failAt(ctx, "attempt_member"); return; }
                 inTx(ctx, kMediaLockSql, "attempt_lock",
                      [ctx](const drogon::orm::Result &) {
                          inTx(ctx, kStateSql, "attempt_state",
                               [ctx](const drogon::orm::Result &rows) {
                                   if (rows.size() != 1u) { failAt(ctx, "attempt_state"); return; }
                                   if (rows[0]["decided"].as<bool>() || rows[0]["planned"].as<bool>()) {
                                       ctx->scope->rollback([ctx]() { answerStored(ctx, false); });
                                       return;
                                   }
                                   if (rows[0]["attempts"].as<int>() >= ctx->policy.maxAttempts) {
                                       ctx->rejection = "attempts_exhausted";
                                       ctx->scope->rollback([ctx]() { commitOutcome(ctx); });
                                       return;
                                   }
                                   inTx(ctx, kAttemptSql, "attempt",
                                        [ctx](const drogon::orm::Result &inserted) {
                                            if (inserted.size() != 1u) { failAt(ctx, "attempt"); return; }
                                            ctx->attemptNo = inserted[0]["attempt_no"].as<int>();
                                            ctx->finalKey = inserted[0]["final_key"].as<std::string>();
                                            ctx->commitRequested = true;
                                            ctx->scope->commit([ctx]() {
                                                ctx->commitRequested = false;
                                                promote(ctx);
                                            });
                                        },
                                        ctx->mediaId);
                               },
                               ctx->mediaId);
                      },
                      ctx->mediaId);
             },
             ctx->userId);
    });
}

void answerStored(const CompletePtr &ctx, bool first) {
    onPool(ctx, kFactsSql, "read",
           [ctx, first](const drogon::orm::Result &rows) {
               // Anyone else's object, or an absent one, looks the same, and no store call was made.
               if (rows.size() != 1u || rows[0]["uploader_id"].as<std::string>() != ctx->userId || rows[0]["deleted"].as<bool>()) {
                   finish(ctx, make(Outcome::NotFound, "图片不存在"));
                   return;
               }
               const auto &row = rows[0];
               if (!row["outcome"].isNull()) {
                   Status status = make(Outcome::Ok);
                   status.media = factsJson(row);
                   status.upload = Json::Value(Json::nullValue);
                   finish(ctx, std::move(status));
                   return;
               }
               if (row["planned"].as<bool>() || row["window_ended"].as<bool>()) {
                   finish(ctx, make(Outcome::GrantExpired, "上传已过期，请重新上传"));
                   return;
               }
               if (!first) {
                   finish(ctx, make(Outcome::NotUploaded, "图片尚未上传完成"));
                   return;
               }
               ctx->stagingKey = row["staging_key"].as<std::string>();
               ctx->declaredType = row["declared_type"].as<std::string>();
               ctx->declaredSize = row["declared_size"].as<long long>();
               onPool(ctx, "SELECT o.couple_id::text AS couple_id FROM media_objects o WHERE o.id = $1::uuid", "couple",
                      [ctx](const drogon::orm::Result &couple) {
                          if (couple.size() != 1u) { failAt(ctx, "couple"); return; }
                          ctx->coupleId = couple[0]["couple_id"].as<std::string>();
                          // Step 2: outages and an absent upload use no attempt.
                          ctx->store->head(ctx->stagingKey, [ctx](StoreStatus present, ObjectFacts) {
                              if (present == StoreStatus::Ok) {
                                  startAttempt(ctx);
                              } else if (present == StoreStatus::NotFound) {
                                  finish(ctx, make(Outcome::NotUploaded, "图片尚未上传完成"));
                              } else {
                                  storeDown(ctx);
                              }
                          });
                      },
                      ctx->mediaId);
           },
           ctx->userId, ctx->mediaId);
}

// ---------------------------------------------------------------- reads
struct ReadCtx {
    std::shared_ptr<db::TxScope> scope;
    MediaService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

// ---------------------------------------------------------------- deletion and undo
struct DeletionCtx {
    std::shared_ptr<db::TxScope> scope;
    bool isUndo = false;
    MediaService::Fault fault = MediaService::Fault::None;
    std::string userId, mediaId, deletionId, key, fingerprint, coupleId;
    Outcome outcome = Outcome::Created;
    Status result;
    MediaService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};
using DeletionPtr = std::shared_ptr<DeletionCtx>;

void refuseDeletion(const DeletionPtr &ctx, Outcome outcome, const char *message) {
    auto status = std::make_shared<Status>(make(outcome, message));
    ctx->scope->rollback([ctx, status]() { finish(ctx, std::move(*status)); });
}

void readDeletionFacts(const DeletionPtr &ctx, const std::string &deletionId, Outcome outcome, std::function<void(Status)> next) {
    inTx(ctx, kDeletionFactsSql, "deletion_read",
         [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "deletion_read"); return; }
             Json::CharReaderBuilder builder;
             std::string errors;
             const std::string text = rows[0]["deletion_json"].as<std::string>();
             const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
             Status status = make(outcome);
             if (!reader->parse(text.data(), text.data() + text.size(), &status.deletion, &errors)) { failAt(ctx, "deletion_read"); return; }
             next(std::move(status));
         },
         deletionId);
}

void auditAndCommitDeletion(const DeletionPtr &ctx, const std::string &deletionId, const char *action) {
    Json::Value detail(Json::objectValue);
    detail["media_id"] = ctx->mediaId;
    detail["deletion_id"] = deletionId;
    inTx(ctx, kDeletionAuditSql, "deletion_audit",
         [ctx, deletionId](const drogon::orm::Result &) {
             readDeletionFacts(ctx, deletionId, ctx->outcome, [ctx](Status status) {
                 ctx->result = std::move(status);
                 ctx->commitRequested = true;
                 ctx->scope->commit([ctx]() { finish(ctx, std::move(ctx->result)); });
             });
         },
         ctx->userId, ctx->coupleId, std::string(action), ctx->mediaId, compactJson(detail));
}

// Member, the request's key, classification. A replay answers before any state is looked at, so it can never see a state error,
// move a deadline or touch a later cycle.
void afterClassification(const DeletionPtr &ctx, std::function<void()> onNew) {
    inTx(ctx, kMemberSql, "deletion_member",
         [ctx, onNew = std::move(onNew)](const drogon::orm::Result &members) {
             if (members.size() != 1u) { refuseDeletion(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             ctx->coupleId = members[0]["couple_id"].as<std::string>();
             inTx(ctx, kKeyLockSql, "deletion_key_lock",
                  [ctx, onNew](const drogon::orm::Result &) {
                      inTx(ctx, ctx->isUndo ? kUndoByKeySql : kDeletionByKeySql, "deletion_classify",
                           [ctx, onNew](const drogon::orm::Result &rows) {
                               if (rows.size() == 0u) { onNew(); return; }
                               if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                                   refuseDeletion(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                                   return;
                               }
                               readDeletionFacts(ctx, rows[0]["id"].as<std::string>(), Outcome::Replayed, [ctx](Status status) {
                                   auto shared = std::make_shared<Status>(std::move(status));
                                   ctx->scope->rollback([ctx, shared]() { finish(ctx, std::move(*shared)); });
                               });
                           },
                           ctx->userId, ctx->key);
                  },
                  ctx->userId, std::string(ctx->isUndo ? "undo" : "delete"), ctx->key);
         },
         ctx->userId);
}

void deleteStart(const DeletionPtr &ctx) {
    afterClassification(ctx, [ctx]() {
        inTx(ctx, kMediaLockSql, "deletion_lock",
             [ctx](const drogon::orm::Result &) {
                 inTx(ctx, kDeleteStateSql, "deletion_state",
                      [ctx](const drogon::orm::Result &rows) {
                          // The partner, another couple and an absent id look the same. So does anything not READY, or on its way out.
                          if (rows.size() != 1u || rows[0]["uploader_id"].as<std::string>() != ctx->userId || !rows[0]["ready"].as<bool>()) {
                              refuseDeletion(ctx, Outcome::NotFound, "图片不存在");
                              return;
                          }
                          // Its own deleter learns this cycle stands, right through a purge.
                          if (rows[0]["deleted"].as<bool>()) {
                              refuseDeletion(ctx, Outcome::AlreadyDeleted, "这张照片已经删除了");
                              return;
                          }
                          if (rows[0]["removing"].as<bool>()) {
                              refuseDeletion(ctx, Outcome::NotFound, "图片不存在");
                              return;
                          }
                          inTx(ctx, kDeletionInsertSql, "deletion_insert",
                               [ctx](const drogon::orm::Result &inserted) {
                                   if (inserted.size() != 1u) { failAt(ctx, "deletion_insert"); return; }
                                   ctx->outcome = Outcome::Created;
                                   auditAndCommitDeletion(ctx, inserted[0]["id"].as<std::string>(), "media.deleted");
                               },
                               ctx->mediaId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint, ctx->deletionId);
                      },
                      ctx->mediaId, ctx->coupleId);
             },
             ctx->mediaId);
    });
}

void undoStart(const DeletionPtr &ctx) {
    afterClassification(ctx, [ctx]() {
        inTx(ctx, kMediaLockSql, "undo_lock",
             [ctx](const drogon::orm::Result &) {
                 inTx(ctx, kUndoStateSql, "undo_state",
                      [ctx](const drogon::orm::Result &rows) {
                          if (rows.size() != 1u || rows[0]["deleted_by"].as<std::string>() != ctx->userId) {
                              refuseDeletion(ctx, Outcome::NotFound, "图片不存在");
                              return;
                          }
                          if (rows[0]["reversed"].as<bool>()) {
                              refuseDeletion(ctx, Outcome::DeletionNotActive, "这次删除已经撤销了");
                              return;
                          }
                          // It came with a memory's deletion, and goes back only with that memory.
                          if (rows[0]["linked"].as<bool>()) {
                              refuseDeletion(ctx, Outcome::DeletionLinked, "这张照片是随回忆一起删除的，请撤销删除这条回忆");
                              return;
                          }
                          if (ctx->fault != MediaService::Fault::IgnoreUndoPrecheck && (rows[0]["removing"].as<bool>() || !rows[0]["open"].as<bool>())) {
                              refuseDeletion(ctx, Outcome::UndoClosed, "已超过可撤销的时间");
                              return;
                          }
                          // The clock keeps moving after that read, and the guard reads it again. Its refusal is the same answer: the cycle
                          // closed, and the failed statement has ended the transaction. Any other failure stays a failure.
                          ctx->scope->tx().execSqlAsync(
                              kReversalInsertSql,
                              [ctx](const drogon::orm::Result &) {
                                  try {
                                      ctx->outcome = Outcome::Ok;
                                      auditAndCommitDeletion(ctx, ctx->deletionId, "media.deletion_undone");
                                  } catch (const std::exception &) {
                                      failAt(ctx, "undo_insert");
                                  }
                              },
                              [ctx](const drogon::orm::DrogonDbException &failure) {
                                  const std::string text = failure.base().what();
                                  if (text.find("USAPP_MEDIA_REVERSAL_WINDOW") != std::string::npos ||
                                      text.find("USAPP_MEDIA_REVERSAL_PURGE_PLANNED") != std::string::npos) {
                                      refuseDeletion(ctx, Outcome::UndoClosed, "已超过可撤销的时间");
                                      return;
                                  }
                                  failAt(ctx, "undo_insert");
                              },
                              ctx->deletionId, ctx->mediaId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
                      },
                      ctx->deletionId, ctx->mediaId, ctx->coupleId);
             },
             ctx->mediaId);
    });
}

std::string lowerId(std::string id) {
    for (auto &ch : id) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return id;
}

// ---------------------------------------------------------------- the job
struct TickCtx {
    drogon::orm::Transaction *tx = nullptr;
    MediaService::Policy policy;
    std::shared_ptr<ObjectStore> store;
    std::function<void(MediaService::TickReport)> done;
    size_t planned = 0;
    bool finished = false;
};
using TickPtr = std::shared_ptr<TickCtx>;

void finishTick(const TickPtr &ctx, MediaService::TickReport report) {
    if (ctx->finished) return;
    ctx->finished = true;
    ctx->done(std::move(report));
}

void failTick(const TickPtr &ctx, const char *stage) {
    MediaService::TickReport report;
    report.detail = std::string("media_cleanup stage=") + stage;
    finishTick(ctx, std::move(report));
}

template <typename... Args>
void tickStep(const TickPtr &ctx, const char *sql, const char *stage,
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

// J2b1: prepare without a transaction, journal, then use the real service's
// locked classification/state checks. Only confirmed SQL effects get a journal
// terminal. A refusal/rollback REQUEST is deliberately not an abort proof.
namespace dj = deletionjournal;
struct JournalMediaCtx {
    std::shared_ptr<dj::Journal> journal;
    std::string userId, mediaId, key, deletionId, fingerprint, coupleId;
    bool undo = false, classified = false;
    MediaService::Fault fault = MediaService::Fault::None;
    MediaService::Callback done;
    std::optional<Status> status;
    dj::Operation operation;
};
using JournalMediaPtr = std::shared_ptr<JournalMediaCtx>;

std::string journalUuid(const std::string &seed, const char *label) {
    auto hash = crypto::sha256Hex(std::string("usapp.media-journal.v1:") + label + ":" + seed);
    hash[12] = '8';
    hash[16] = 'a';
    return hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-" + hash.substr(12, 4) + "-" + hash.substr(16, 4) + "-" + hash.substr(20, 12);
}

// A post-effect read transaction acquires the same key fence and commits before
// acknowledging externally. This also waits out a replay's requested rollback.
void verifyJournalEffect(const JournalMediaPtr &ctx, std::function<void(dj::EffectState)> done) {
    auto answer = std::make_shared<std::function<void(dj::EffectState)>>(std::move(done));
    db::TxScope::beginAsync(
        [ctx, answer](std::shared_ptr<db::TxScope> scope) {
            scope->tx().execSqlAsync(kKeyLockSql,
                [ctx, answer, scope](const drogon::orm::Result &) {
                    scope->tx().execSqlAsync(ctx->undo ? kUndoByKeySql : kDeletionByKeySql,
                        [ctx, answer, scope](const drogon::orm::Result &rows) {
                            bool matches = false;
                            try { matches = rows.size() == 1u && rows[0]["id"].as<std::string>() == ctx->deletionId &&
                                            rows[0]["request_fingerprint"].as<std::string>() == ctx->fingerprint; }
                            catch (const std::exception &) {}
                            if (!matches) { scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return; }
                            scope->commit([answer]() { (*answer)(dj::EffectState::Committed); });
                        },
                        [scope](const drogon::orm::DrogonDbException &) { scope->fail("media journal verify"); }, ctx->userId, ctx->key);
                },
                [scope](const drogon::orm::DrogonDbException &) { scope->fail("media journal fence"); },
                ctx->userId, std::string(ctx->undo ? "undo" : "delete"), ctx->key);
        },
        [answer](const std::string &) { (*answer)(dj::EffectState::Unknown); });
}

struct JournalMediaEffect : dj::Effect {
    JournalMediaPtr ctx;
    explicit JournalMediaEffect(JournalMediaPtr c) : ctx(std::move(c)) {}
    void applyOrClassify(const dj::Operation &, std::function<void(dj::EffectState)> done) override {
        if (ctx->classified) { verifyJournalEffect(ctx, std::move(done)); return; }
        auto raw = std::make_shared<DeletionCtx>();
        raw->isUndo = ctx->undo;
        raw->fault = ctx->fault;
        raw->userId = ctx->userId;
        raw->mediaId = ctx->mediaId;
        raw->deletionId = ctx->deletionId;
        raw->key = ctx->key;
        raw->fingerprint = ctx->fingerprint;
        raw->done = [c = ctx, done = std::move(done)](Status status) {
            const bool success = status.success() && status.deletion["deletion_id"].isString() &&
                                 status.deletion["deletion_id"].asString() == c->deletionId;
            c->status = std::move(status);
            if (success) verifyJournalEffect(c, done);
            else done(dj::EffectState::Unknown);
        };
        beginTx(raw, [raw]() { if (raw->isUndo) undoStart(raw); else deleteStart(raw); });
    }
};

void runJournalMedia(const JournalMediaPtr &ctx) {
    Json::Value seed(Json::arrayValue);
    seed.append(ctx->userId); seed.append(ctx->undo ? "undo" : "delete"); seed.append(ctx->key);
    const auto identity = compactJson(seed);
    ctx->operation.id = journalUuid(identity, "operation");
    if (!ctx->undo && !ctx->classified) ctx->deletionId = journalUuid(identity, "cycle");
    ctx->operation.action = ctx->undo ? dj::Action::Undo : dj::Action::Delete;
    ctx->operation.subjects = {{dj::Kind::Media, ctx->mediaId, ctx->deletionId, journalUuid(identity, "intent")}};
    dj::Coordinator::run(ctx->operation, ctx->journal, std::make_shared<JournalMediaEffect>(ctx), [ctx](dj::Result result) {
        if (result == dj::Result::Committed && ctx->status && ctx->status->success()) {
            ctx->done(std::move(*ctx->status)); return;
        }
        if (result == dj::Result::Conflict) { ctx->done(make(Outcome::KeyReused, "该请求标识已用于另一项操作")); return; }
        // A confirmed state refusal may be returned as before, but is NOT an
        // aborted external intent: neither rollback request nor absence proves
        // every in-flight same-key attempt can no longer commit.
        if (result == dj::Result::Unknown && ctx->status && !ctx->status->success() &&
            ctx->status->outcome != Outcome::Error && ctx->status->outcome != Outcome::ResultUnknown) {
            ctx->done(std::move(*ctx->status)); return;
        }
        auto status = make(Outcome::ResultUnknown, "结果未确认，请使用原请求重试");
        status.detail = "media journal=unconfirmed";
        ctx->done(std::move(status));
    });
}

void readJournalReplay(const JournalMediaPtr &ctx) {
    db::Pool::instance().client()->execSqlAsync(kDeletionFactsSql,
        [ctx](const drogon::orm::Result &rows) {
            try {
                if (rows.size() != 1u) throw std::runtime_error("row_count");
                Json::CharReaderBuilder builder;
                const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                const auto text = rows[0]["deletion_json"].as<std::string>();
                std::string errors;
                Status status = make(Outcome::Replayed);
                if (!reader->parse(text.data(), text.data() + text.size(), &status.deletion, &errors)) throw std::runtime_error("json");
                ctx->status = std::move(status);
                runJournalMedia(ctx);
            } catch (const std::exception &) { ctx->done(make(Outcome::Error)); }
        },
        [ctx](const drogon::orm::DrogonDbException &) { ctx->done(make(Outcome::Error)); }, ctx->deletionId);
}

void prepareJournalMedia(const JournalMediaPtr &ctx) {
    const std::string sql = std::string("SELECT m.couple_id::text AS couple_id, k.id::text AS id, k.request_fingerprint "
        "FROM couple_members m LEFT JOIN (") + (ctx->undo ? kUndoByKeySql : kDeletionByKeySql) +
        ") k ON true WHERE m.user_id = $1::uuid";
    db::Pool::instance().client()->execSqlAsync(sql,
        [ctx](const drogon::orm::Result &members) {
            try {
                if (members.size() != 1u) { ctx->done(make(Outcome::SpaceNotFound, "尚未加入空间")); return; }
                ctx->coupleId = members[0]["couple_id"].as<std::string>();
                if (!members[0]["id"].isNull()) {
                    if (members[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                        ctx->done(make(Outcome::KeyReused, "该请求标识已用于另一项操作")); return;
                    }
                    ctx->classified = true;
                    ctx->deletionId = members[0]["id"].as<std::string>();
                    readJournalReplay(ctx); return;
                }
                const char *sql = ctx->undo ?
                    "SELECT d.deleted_by::text AS owner FROM media_deletions d WHERE d.id = $3::uuid AND d.media_id = $1::uuid AND d.couple_id = $2::uuid" :
                    "SELECT o.uploader_id::text AS owner FROM media_objects o WHERE o.id = $1::uuid AND o.couple_id = $2::uuid AND "
                    "EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = o.id AND t.outcome = 'ready') AND $3::text IS NOT NULL";
                db::Pool::instance().client()->execSqlAsync(sql,
                    [ctx](const drogon::orm::Result &rows) {
                        try {
                            if (rows.size() != 1u || rows[0]["owner"].as<std::string>() != ctx->userId) {
                                ctx->done(make(Outcome::NotFound, "图片不存在")); return;
                            }
                            runJournalMedia(ctx);
                        } catch (const std::exception &) { ctx->done(make(Outcome::Error)); }
                    },
                    [ctx](const drogon::orm::DrogonDbException &) { ctx->done(make(Outcome::Error)); },
                    ctx->mediaId, ctx->coupleId, ctx->deletionId);
            } catch (const std::exception &) { ctx->done(make(Outcome::Error)); }
        },
        [ctx](const drogon::orm::DrogonDbException &) { ctx->done(make(Outcome::Error)); }, ctx->userId, ctx->key);
}

struct WorkItem {
    std::string mediaId, scope, kind;
    std::vector<std::string> keys;
};

// Only after every key of a pass is gone is the pass recorded, so a failure simply repeats it next tick.
void runWork(const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<WorkItem>> work, size_t index,
             std::function<void()> finished);

void removeKeys(const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<WorkItem>> work, size_t index, size_t keyIndex,
                bool allGone, std::function<void()> finished) {
    const auto &item = (*work)[index];
    if (keyIndex >= item.keys.size()) {
        if (!allGone) {
            runWork(store, work, index + 1, std::move(finished));
            return;
        }
        db::Pool::instance().client()->execSqlAsync(
            kPassSql,
            [store, work, index, finished](const drogon::orm::Result &) { runWork(store, work, index + 1, finished); },
            [store, work, index, finished](const drogon::orm::DrogonDbException &) {
                LOG_ERROR << "media_cleanup could not record a pass; it repeats next tick";
                runWork(store, work, index + 1, finished);
            },
            item.mediaId, item.scope, item.kind);
        return;
    }
    // Removed, or confirmed already absent. Anything else leaves the pass unrecorded, and the same keys are tried next tick.
    store->remove(item.keys[keyIndex], [store, work, index, keyIndex, allGone, finished](StoreStatus removed) {
        removeKeys(store, work, index, keyIndex + 1, allGone && (removed == StoreStatus::Ok || removed == StoreStatus::NotFound), finished);
    });
}

void runWork(const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<WorkItem>> work, size_t index,
             std::function<void()> finished) {
    if (index >= work->size()) {
        finished();
        return;
    }
    removeKeys(store, work, index, 0, true, std::move(finished));
}

// The reconciler. Only a key the database calls late is removed, and only a key nothing names is reported.
struct Scan {
    std::mutex mutex;
    std::shared_ptr<ObjectStore> store;  // markers belong to one store
    std::string stagingMarker, mediaMarker;
};
Scan &scanState() {
    static Scan scan;
    return scan;
}
std::atomic<bool> &backgroundBusy() {
    static std::atomic<bool> busy{false};
    return busy;
}

void classifyKeys(const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<std::string>> keys, size_t index,
                  std::function<void()> finished) {
    if (index >= keys->size()) {
        finished();
        return;
    }
    const std::string key = (*keys)[index];
    auto next = [store, keys, index, finished]() { classifyKeys(store, keys, index + 1, finished); };
    db::Pool::instance().client()->execSqlAsync(
        kDispositionSql,
        [store, key, next](const drogon::orm::Result &rows) {
            const std::string state = rows.size() == 1u ? rows[0]["state"].as<std::string>() : std::string();
            auto record = [key, next](const char *disposition) {
                db::Pool::instance().client()->execSqlAsync(
                    kLateSql, [next](const drogon::orm::Result &) { next(); },
                    [next](const drogon::orm::DrogonDbException &) {
                        LOG_ERROR << "media_cleanup could not record a late object";
                        next();
                    },
                    key, std::string(disposition), std::string("reconcile"));
            };
            if (state == "late") {
                store->remove(key, [record, next](StoreStatus removed) {
                    if (removed == StoreStatus::Ok) record("removed");
                    else next();  // still listed next time
                });
            } else if (state == "unknown") {
                LOG_ERROR << "media_cleanup found an object no row names; reported, not removed";
                record("report_only");
            } else {
                next();
            }
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, key);
}

void scanPrefix(const std::shared_ptr<ObjectStore> &store, const std::string &prefix, int pageSize, bool isStaging,
                std::function<void()> finished) {
    auto &scan = scanState();
    std::string marker;
    {
        std::lock_guard<std::mutex> guard(scan.mutex);
        if (scan.store != store) {
            scan.store = store;
            scan.stagingMarker.clear();
            scan.mediaMarker.clear();
        }
        marker = isStaging ? scan.stagingMarker : scan.mediaMarker;
    }
    store->list(prefix, marker, pageSize, [store, isStaging, finished](StoreStatus listed, KeyPage page) {
        if (listed != StoreStatus::Ok) {
            finished();
            return;
        }
        {
            auto &state = scanState();
            std::lock_guard<std::mutex> guard(state.mutex);
            (isStaging ? state.stagingMarker : state.mediaMarker) = page.nextMarker;
        }
        classifyKeys(store, std::make_shared<std::vector<std::string>>(std::move(page.keys)), 0, finished);
    });
}

// =====================================================================
// J2c2: journaled object cleanup (docs/phase3-deletion-journal.md Section 11). With a journal configured, every
// ObjectStore::remove of a cleanup pass, a late object or a resumed attempt's own key runs only after the journal has
// confirmed a durable intent for exactly that removal. Nothing here holds a transaction or an advisory lock: each read
// is one pooled statement, and every journal and store call is between them.
//
// Identity. One operation per (plan, scope, pass kind) or per (late key, source), and an epoch. A retry reuses its
// epoch, so a lost receipt or a partial failure repeats the same operation. A terminal epoch never stands for work the
// database still needs: if the database still selects that pass or still calls that key late, the next epoch is a new
// operation, so a key that lands again is removed again. The payload binds the plan's reason and exact deletion, the
// source and a digest of the exact keys: a changed target set under the same identity is a conflict, not an effect.
// A user deletion's purge is action Purge on that deletion's cycle; every other cleanup is action Cleanup on a cycle
// named by its plan, never an invented deletion.
//
// No cleanup is ever acknowledged as aborted: a refusal after an earlier attempt of the same operation cannot prove
// that attempt removed nothing. A refused operation stays unresolved, which the restore plan treats as a barrier.
// =====================================================================
constexpr int kCleanupEpochs = 16;
constexpr int kCleanupAckBatch = 50;

const char *kPassFactsSql =
    "SELECT p.reason, COALESCE(p.deletion_id::text, '') AS deletion_id, clock_timestamp() >= p.sweep_not_before AS sweep_due, "
    "EXISTS (SELECT 1 FROM media_cleanup_passes s WHERE s.media_id = p.media_id AND s.scope = p.scope AND s.kind = $3) AS recorded, "
    "(SELECT COALESCE(jsonb_agg(k.key ORDER BY k.key), '[]'::jsonb)::text FROM ("
    "  SELECT o.staging_key AS key UNION ALL SELECT a.final_key FROM media_promotion_attempts a "
    "  WHERE a.media_id = p.media_id AND (p.scope = 'everything' OR a.attempt_no IS DISTINCT FROM t.attempt_no)) k) AS keys "
    "FROM media_cleanup_plans p JOIN media_objects o ON o.id = p.media_id "
    "LEFT JOIN media_upload_outcomes t ON t.media_id = p.media_id "
    "WHERE p.media_id = $1::uuid AND p.scope = $2";
const char *kDispositionsSql =
    "SELECT COALESCE(bool_and(usapp_media_key_disposition(k) IN ('pending_sweep', 'late')), true) AS removable "
    "FROM jsonb_array_elements_text($1::jsonb) k";
// The key's media, its state, and the plan that state comes from: everything if there is one, else leftovers.
const char *kKeyFactsSql =
    "SELECT k.media_id::text AS media_id, usapp_media_key_disposition($1) AS state, COALESCE(p.scope, '') AS scope, "
    "COALESCE(p.reason, '') AS reason, COALESCE(p.deletion_id::text, '') AS deletion_id "
    "FROM (SELECT a.media_id FROM media_promotion_attempts a WHERE a.final_key = $1 "
    "      UNION ALL SELECT o.id FROM media_objects o WHERE o.staging_key = $1) k "
    "LEFT JOIN LATERAL (SELECT x.scope, x.reason, x.deletion_id FROM media_cleanup_plans x WHERE x.media_id = k.media_id "
    "  ORDER BY (x.scope = 'everything') DESC LIMIT 1) p ON true LIMIT 1";
// Completed work whose final acknowledgement may be missing. Normal selection no longer returns it.
const char *kPassAckSql =
    "SELECT media_id::text AS media_id, scope, kind FROM media_cleanup_passes "
    "WHERE (media_id::text, scope, kind) > ($1, $2, $3) ORDER BY media_id::text, scope, kind LIMIT $4::integer";
const char *kLateAckSql =
    "SELECT object_key FROM media_late_objects WHERE disposition = 'removed' AND object_key > $1 "
    "ORDER BY object_key LIMIT $2::integer";

struct CleanupTarget {
    bool late = false;
    std::string mediaId, scope, pass, source, reason, deletionId;
    std::vector<std::string> keys;  // sorted; a late target has exactly one
};

struct CleanupProgress {
    std::map<std::string, uint64_t> workEpoch, ackEpoch;
    std::map<std::string, std::chrono::steady_clock::time_point> backoff;
};
struct CleanupState {
    std::mutex mutex;
    MediaService::CleanupJournalReport report;
    std::function<bool(const std::string &)> fault;  // tests only
    std::string passMedia, passScope, passKind, lateKey;
    std::string workCreated = "-infinity", workMedia = "00000000-0000-0000-0000-000000000000", workScope;
    // Operation/acknowledgement cursors and refusals are memory only. A restart walks the durable history again,
    // with a fresh bounded window per round; an unenrolled cycle is backed off without inventing new epochs.
    // Scoped by provider ownership: switching journals must not skip that journal's earlier operations.
    std::map<std::weak_ptr<dj::Journal>, std::shared_ptr<CleanupProgress>, std::owner_less<std::weak_ptr<dj::Journal>>> progress;
};
CleanupState &cleanupState() {
    static CleanupState state;
    return state;
}
std::shared_ptr<CleanupProgress> cleanupProgress(const std::shared_ptr<dj::Journal> &journal) {
    auto &state = cleanupState();
    std::lock_guard<std::mutex> guard(state.mutex);
    for (auto it = state.progress.begin(); it != state.progress.end();) {
        if (it->first.expired()) it = state.progress.erase(it);
        else ++it;
    }
    auto &progress = state.progress[journal];
    if (!progress) progress = std::make_shared<CleanupProgress>();
    return progress;
}
bool cleanupFault(const std::string &stage) {
    std::function<bool(const std::string &)> fault;
    {
        std::lock_guard<std::mutex> guard(cleanupState().mutex);
        fault = cleanupState().fault;
    }
    return fault && fault(stage);
}
void countCleanup(long long MediaService::CleanupJournalReport::*field) {
    std::lock_guard<std::mutex> guard(cleanupState().mutex);
    ++(cleanupState().report.*field);
}

std::string cleanupFingerprint(const CleanupTarget &t) {
    std::string text = std::string("usapp.media-cleanup.v1\n") + (t.late ? "late" : "pass") + "\n" + t.mediaId + "\n" + t.scope + "\n" +
                       t.pass + "\n" + t.source + "\n" + t.reason + "\n" + t.deletionId;
    for (const auto &key : t.keys) text += "\n" + key;
    return crypto::sha256Hex(text);
}
dj::Operation cleanupOperation(const CleanupTarget &t, uint64_t epoch) {
    const std::string seed = (t.late ? "late|" + t.source + "|" + t.keys.front() : "pass|" + t.mediaId + "|" + t.scope + "|" + t.pass) +
                             "|epoch=" + std::to_string(epoch);
    const bool userDeleted = t.reason == "user_deleted";
    dj::Operation op;
    op.id = journalUuid(seed, "cleanup-operation");
    op.action = userDeleted ? dj::Action::Purge : dj::Action::Cleanup;
    op.subjects = {{dj::Kind::Media, t.mediaId, userDeleted ? t.deletionId : journalUuid(t.mediaId + "|" + t.scope, "cleanup-plan"),
                    journalUuid(seed, "cleanup-intent")}};
    op.requestFingerprint = cleanupFingerprint(t);
    return op;
}
// A user deletion's plan must name its deletion: no cycle is invented, and no such plan is cleaned unjournaled.
bool plausible(const CleanupTarget &t) {
    if (t.mediaId.empty() || t.scope.empty() || t.reason.empty() || t.keys.empty()) return false;
    return t.reason != "user_deleted" || !t.deletionId.empty();
}
bool passTarget(const drogon::orm::Result &rows, const std::string &mediaId, const std::string &scope, const std::string &pass,
                CleanupTarget &out) {
    if (rows.size() != 1u) return false;
    out = CleanupTarget{};
    out.mediaId = mediaId;
    out.scope = scope;
    out.pass = pass;
    out.reason = rows[0]["reason"].as<std::string>();
    out.deletionId = rows[0]["deletion_id"].as<std::string>();
    out.keys = parseStrings(rows[0]["keys"].as<std::string>());
    std::sort(out.keys.begin(), out.keys.end());
    return plausible(out);
}
bool lateTarget(const drogon::orm::Result &rows, const std::string &key, const std::string &source, CleanupTarget &out) {
    if (rows.size() != 1u) return false;
    out = CleanupTarget{};
    out.late = true;
    out.mediaId = rows[0]["media_id"].as<std::string>();
    out.scope = rows[0]["scope"].as<std::string>();
    out.source = source;
    out.reason = rows[0]["reason"].as<std::string>();
    out.deletionId = rows[0]["deletion_id"].as<std::string>();
    out.keys = {key};
    return plausible(out);
}

// Each key in turn. Removed or confirmed absent counts; anything else leaves the operation unresolved.
void removeEach(const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<std::string>> keys, size_t index, bool allGone,
                std::function<void(bool)> done) {
    if (index >= keys->size()) {
        done(allGone);
        return;
    }
    store->remove((*keys)[index], [store, keys, index, allGone, done](StoreStatus removed) {
        removeEach(store, keys, index + 1, allGone && (removed == StoreStatus::Ok || removed == StoreStatus::NotFound), done);
    });
}

struct CleanupEffect : dj::Effect, std::enable_shared_from_this<CleanupEffect> {
    std::shared_ptr<ObjectStore> store;
    CleanupTarget target;
    bool confirmOnly = false;
    std::shared_ptr<bool> ran = std::make_shared<bool>(false), refused = std::make_shared<bool>(false);

    void applyOrClassify(const dj::Operation &op, std::function<void(dj::EffectState)> done) override {
        *ran = true;
        if (target.late) late(op, std::move(done));
        else pass(op, std::move(done));
    }
    void refuse(const std::function<void(dj::EffectState)> &done) {
        *refused = true;
        done(dj::EffectState::Unknown);
    }
    void pass(const dj::Operation &op, std::function<void(dj::EffectState)> done) {
        auto self = shared_from_this();
        db::Pool::instance().client()->execSqlAsync(
            kPassFactsSql,
            [self, op, done](const drogon::orm::Result &rows) {
                CleanupTarget now;
                // The authoritative facts again, after the intent: the same plan, deletion and keys, or nothing happens.
                if (!passTarget(rows, self->target.mediaId, self->target.scope, self->target.pass, now) ||
                    cleanupFingerprint(now) != op.requestFingerprint) { self->refuse(done); return; }
                // Recorded only after every key was confirmed absent, by this operation or an earlier attempt of it.
                if (rows[0]["recorded"].as<bool>()) { done(dj::EffectState::Committed); return; }
                if (self->confirmOnly || (now.pass == "sweep" && !rows[0]["sweep_due"].as<bool>())) { self->refuse(done); return; }
                Json::Value keys(Json::arrayValue);
                for (const auto &key : now.keys) keys.append(key);
                db::Pool::instance().client()->execSqlAsync(
                    kDispositionsSql,
                    [self, now, done](const drogon::orm::Result &states) {
                        // A key the database now calls live, keep or unknown is never removed.
                        if (states.size() != 1u || !states[0]["removable"].as<bool>()) { self->refuse(done); return; }
                        removeEach(self->store, std::make_shared<std::vector<std::string>>(now.keys), 0, true, [self, now, done](bool allGone) {
                            if (!allGone || cleanupFault("pass_record")) { done(dj::EffectState::Unknown); return; }
                            db::Pool::instance().client()->execSqlAsync(
                                kPassSql, [done](const drogon::orm::Result &) { done(dj::EffectState::Committed); },
                                [done](const drogon::orm::DrogonDbException &) {
                                    LOG_ERROR << "media_cleanup could not record a journaled pass; it repeats next tick";
                                    done(dj::EffectState::Unknown);
                                },
                                now.mediaId, now.scope, now.pass);
                        });
                    },
                    [done](const drogon::orm::DrogonDbException &) { done(dj::EffectState::Unknown); }, compactJson(keys));
            },
            [done](const drogon::orm::DrogonDbException &) { done(dj::EffectState::Unknown); }, target.mediaId, target.scope, target.pass);
    }
    void late(const dj::Operation &op, std::function<void(dj::EffectState)> done) {
        auto self = shared_from_this();
        const std::string key = target.keys.front();
        db::Pool::instance().client()->execSqlAsync(
            kKeyFactsSql,
            [self, op, key, done](const drogon::orm::Result &rows) {
                CleanupTarget now;
                if (!lateTarget(rows, key, self->target.source, now) || cleanupFingerprint(now) != op.requestFingerprint) {
                    self->refuse(done);
                    return;
                }
                if (self->confirmOnly) {
                    // Only the store can say the key is gone now. Present again is the reconciler's next removal, not this one's.
                    self->store->head(key, [done](StoreStatus seen, ObjectFacts) {
                        done(seen == StoreStatus::NotFound ? dj::EffectState::Committed : dj::EffectState::Unknown);
                    });
                    return;
                }
                const std::string state = rows[0]["state"].as<std::string>();
                const bool removable = state == "late" || (state == "pending_sweep" && now.source == "attempt_resume");
                if (!removable) { self->refuse(done); return; }
                self->store->remove(key, [self, key, done](StoreStatus removed) {
                    if (removed == StoreStatus::NotFound) { done(dj::EffectState::Committed); return; }
                    if (removed != StoreStatus::Ok) { done(dj::EffectState::Unknown); return; }
                    if (cleanupFault("late_record")) { done(dj::EffectState::Committed); return; }
                    db::Pool::instance().client()->execSqlAsync(
                        kLateSql, [done](const drogon::orm::Result &) { done(dj::EffectState::Committed); },
                        [done](const drogon::orm::DrogonDbException &) {
                            // The key is confirmed gone; only its record is missing.
                            LOG_ERROR << "media_cleanup could not record a journaled late removal";
                            done(dj::EffectState::Committed);
                        },
                        key, std::string("removed"), self->target.source);
                });
            },
            [done](const drogon::orm::DrogonDbException &) { done(dj::EffectState::Unknown); }, key);
    }
};

constexpr int kCleanupBackoffSeconds = 300;
std::string backoffKey(const CleanupTarget &t) { return t.late ? "late|" + t.source + "|" + t.keys.front() : "pass|" + t.mediaId + "|" + t.scope + "|" + t.pass; }

// Epoch by epoch until one operation is this run's: a terminal or conflicting epoch that this run did not execute is
// earlier work, and the database still asks for this removal.
void runCleanupEpochs(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, CleanupTarget target, uint64_t epoch,
                      std::function<void()> next, int remaining = kCleanupEpochs) {
    const auto progress = cleanupProgress(journal);
    const auto key = backoffKey(target);
    if (remaining == kCleanupEpochs) {
        bool skip = false;
        {
            std::lock_guard<std::mutex> guard(cleanupState().mutex);
            epoch = progress->workEpoch[key];
            auto &backoff = progress->backoff;
            const auto found = backoff.find(backoffKey(target));
            if (found != backoff.end()) {
                if (std::chrono::steady_clock::now() < found->second) {
                    ++cleanupState().report.backedOff;
                    skip = true;
                } else {
                    backoff.erase(found);
                }
            }
        }
        // Never under that lock: next() can run the rest of the round synchronously, which takes it again.
        if (skip) {
            next();
            return;
        }
    }
    if (remaining == 0) {
        {
            std::lock_guard<std::mutex> guard(cleanupState().mutex);
            ++cleanupState().report.exhausted;
        }
        // The cursor was advanced after each classified terminal operation. Next round resumes here, not at zero.
        next();
        return;
    }
    auto effect = std::make_shared<CleanupEffect>();
    effect->store = store;
    effect->target = target;
    const auto op = cleanupOperation(target, epoch);
    dj::Coordinator::run(op, journal, effect, [journal, store, target, epoch, remaining, next, effect, progress, key, op](dj::Result result) {
        if (!*effect->ran && (result == dj::Result::Committed || result == dj::Result::ProvenNotApplied || result == dj::Result::Conflict)) {
            if (result == dj::Result::Conflict) countCleanup(&MediaService::CleanupJournalReport::conflicts);
            auto advance = [journal, store, target, epoch, remaining, next, progress, key]() {
                if (epoch == std::numeric_limits<uint64_t>::max()) { next(); return; } // never reuse an identity on overflow
                {
                    std::lock_guard<std::mutex> guard(cleanupState().mutex);
                    progress->workEpoch[key] = epoch + 1;
                }
                runCleanupEpochs(journal, store, target, epoch + 1, next, remaining - 1);
            };
            if (result != dj::Result::Conflict) { advance(); return; }
            // A conflict can be an immutable occupied identity, or a refusal to create any intent (e.g. an
            // unenrolled legacy purge). Only the former justifies advancing. Unknown stays on the same epoch.
            journal->lookupOperation(op.id, [advance, progress, key, next](dj::LookupState state, dj::Operation) {
                if (state == dj::LookupState::Found) { advance(); return; }
                if (state == dj::LookupState::Missing) {
                    std::lock_guard<std::mutex> guard(cleanupState().mutex);
                    progress->backoff[key] = std::chrono::steady_clock::now() + std::chrono::seconds(kCleanupBackoffSeconds);
                }
                next();
            });
            return;
        }
        if (result == dj::Result::Committed) countCleanup(&MediaService::CleanupJournalReport::confirmed);
        else if (*effect->refused) countCleanup(&MediaService::CleanupJournalReport::refused);
        else countCleanup(&MediaService::CleanupJournalReport::unknown);
        next();
    });
}

// Missing final acknowledgements, by the frozen payload the journal holds. Never a removal: completed passes are
// confirmed from their recorded pass, late removals from the store saying the key is gone.
void confirmCleanupEpochs(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, CleanupTarget target, uint64_t epoch,
                          std::function<void()> next, int remaining = kCleanupEpochs) {
    const auto progress = cleanupProgress(journal);
    const auto key = backoffKey(target);
    if (remaining == kCleanupEpochs) {
        std::lock_guard<std::mutex> guard(cleanupState().mutex);
        epoch = progress->ackEpoch[key];
    }
    if (remaining == 0) { next(); return; }
    const auto id = cleanupOperation(target, epoch).id;
    journal->lookupOperation(id, [journal, store, target, epoch, remaining, next, progress, key](dj::LookupState state, dj::Operation found) {
        if (state != dj::LookupState::Found) {
            if (state == dj::LookupState::Missing) {
                std::lock_guard<std::mutex> guard(cleanupState().mutex);
                progress->ackEpoch[key] = 0; // wrap, so older unresolved acknowledgements are revisited
            }
            next(); return;
        }
        auto effect = std::make_shared<CleanupEffect>();
        effect->store = store;
        effect->target = target;
        effect->confirmOnly = true;
        dj::Coordinator::run(found, journal, effect, [journal, store, target, epoch, remaining, next, effect, progress, key](dj::Result result) {
            if (*effect->ran && result == dj::Result::Committed) countCleanup(&MediaService::CleanupJournalReport::acknowledged);
            {
                std::lock_guard<std::mutex> guard(cleanupState().mutex);
                progress->ackEpoch[key] = epoch == std::numeric_limits<uint64_t>::max() ? 0 : epoch + 1;
            }
            if (epoch == std::numeric_limits<uint64_t>::max()) { next(); return; }
            confirmCleanupEpochs(journal, store, target, epoch + 1, next, remaining - 1);
        });
    });
}

// A late or resumed key, prepared from the database, then journaled.
void journaledLateRemoval(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, const std::string &key,
                          const std::string &source, std::function<void()> next) {
    db::Pool::instance().client()->execSqlAsync(
        kKeyFactsSql,
        [journal, store, key, source, next](const drogon::orm::Result &rows) {
            CleanupTarget target;
            if (!lateTarget(rows, key, source, target)) {
                countCleanup(&MediaService::CleanupJournalReport::refused);
                next();
                return;
            }
            runCleanupEpochs(journal, store, target, 0, next);
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, key);
}

void runJournaledWork(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, std::shared_ptr<std::vector<WorkItem>> work,
                      size_t index, std::function<void()> finished) {
    if (index >= work->size()) { finished(); return; }
    const auto item = (*work)[index];
    auto next = [journal, store, work, index, finished]() { runJournaledWork(journal, store, work, index + 1, finished); };
    db::Pool::instance().client()->execSqlAsync(
        kPassFactsSql,
        [journal, store, item, next](const drogon::orm::Result &rows) {
            CleanupTarget target;
            if (!passTarget(rows, item.mediaId, item.scope, item.kind, target)) {
                countCleanup(&MediaService::CleanupJournalReport::refused);
                next();
                return;
            }
            runCleanupEpochs(journal, store, target, 0, next);
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, item.mediaId, item.scope, item.kind);
}

void ackPassRows(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, std::shared_ptr<drogon::orm::Result> rows, size_t index,
                 std::function<void()> finished) {
    if (index >= rows->size()) { finished(); return; }
    const auto row = (*rows)[index];
    const std::string media = row["media_id"].as<std::string>(), scope = row["scope"].as<std::string>(), kind = row["kind"].as<std::string>();
    auto next = [journal, store, rows, index, finished]() { ackPassRows(journal, store, rows, index + 1, finished); };
    db::Pool::instance().client()->execSqlAsync(
        kPassFactsSql,
        [journal, store, media, scope, kind, next](const drogon::orm::Result &facts) {
            CleanupTarget target;
            if (!passTarget(facts, media, scope, kind, target)) { next(); return; }
            confirmCleanupEpochs(journal, store, target, 0, next);
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, media, scope, kind);
}
void ackLateRows(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, std::shared_ptr<drogon::orm::Result> rows, size_t index,
                 std::function<void()> finished) {
    if (index >= rows->size()) { finished(); return; }
    const auto row = (*rows)[index];
    const std::string key = row["object_key"].as<std::string>();
    auto next = [journal, store, rows, index, finished]() { ackLateRows(journal, store, rows, index + 1, finished); };
    db::Pool::instance().client()->execSqlAsync(
        kKeyFactsSql,
        [journal, store, key, next](const drogon::orm::Result &facts) {
            CleanupTarget target;
            if (!lateTarget(facts, key, "attempt_resume", target)) { next(); return; }
            // The append-only removed row preserves only the first source. Both operation namespaces may exist.
            confirmCleanupEpochs(journal, store, target, 0, [journal, store, target, next]() mutable {
                target.source = "reconcile";
                confirmCleanupEpochs(journal, store, target, 0, next);
            });
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, key);
}
// A bounded page per tick, cycling through all completed work. The position is memory only: a restart begins again.
void reconcileAcknowledgements(std::shared_ptr<dj::Journal> journal, std::shared_ptr<ObjectStore> store, std::function<void()> finished) {
    std::string media, scope, kind, lateKey;
    {
        std::lock_guard<std::mutex> guard(cleanupState().mutex);
        media = cleanupState().passMedia; scope = cleanupState().passScope; kind = cleanupState().passKind; lateKey = cleanupState().lateKey;
    }
    db::Pool::instance().client()->execSqlAsync(
        kPassAckSql,
        [journal, store, lateKey, finished](const drogon::orm::Result &passes) {
            auto rows = std::make_shared<drogon::orm::Result>(passes);
            {
                std::lock_guard<std::mutex> guard(cleanupState().mutex);
                auto &state = cleanupState();
                if (passes.size() < static_cast<size_t>(kCleanupAckBatch)) { state.passMedia.clear(); state.passScope.clear(); state.passKind.clear(); }
                else {
                    const auto last = passes[passes.size() - 1];
                    state.passMedia = last["media_id"].as<std::string>(); state.passScope = last["scope"].as<std::string>(); state.passKind = last["kind"].as<std::string>();
                }
            }
            ackPassRows(journal, store, rows, 0, [journal, store, lateKey, finished]() {
                db::Pool::instance().client()->execSqlAsync(
                    kLateAckSql,
                    [journal, store, finished](const drogon::orm::Result &late) {
                        auto lateRows = std::make_shared<drogon::orm::Result>(late);
                        {
                            std::lock_guard<std::mutex> guard(cleanupState().mutex);
                            cleanupState().lateKey = late.size() < static_cast<size_t>(kCleanupAckBatch) ? std::string()
                                                                                                         : late[late.size() - 1]["object_key"].as<std::string>();
                        }
                        ackLateRows(journal, store, lateRows, 0, finished);
                    },
                    [finished](const drogon::orm::DrogonDbException &) { finished(); }, lateKey, std::to_string(kCleanupAckBatch));
            });
        },
        [finished](const drogon::orm::DrogonDbException &) { finished(); }, media, scope, kind, std::to_string(kCleanupAckBatch));
}

// The reconciler's classification, journaled. Unknown keys stay report-only, with no store call.
void classifyKeysJournaled(std::shared_ptr<dj::Journal> journal, const std::shared_ptr<ObjectStore> &store, std::shared_ptr<std::vector<std::string>> keys,
                           size_t index, std::function<void()> finished) {
    if (index >= keys->size()) { finished(); return; }
    const std::string key = (*keys)[index];
    auto next = [journal, store, keys, index, finished]() { classifyKeysJournaled(journal, store, keys, index + 1, finished); };
    db::Pool::instance().client()->execSqlAsync(
        kDispositionSql,
        [journal, store, key, next](const drogon::orm::Result &rows) {
            const std::string state = rows.size() == 1u ? rows[0]["state"].as<std::string>() : std::string();
            if (state == "late") {
                journaledLateRemoval(journal, store, key, "reconcile", next);
            } else if (state == "unknown") {
                LOG_ERROR << "media_cleanup found an object no row names; reported, not removed";
                db::Pool::instance().client()->execSqlAsync(
                    kLateSql, [next](const drogon::orm::Result &) { next(); },
                    [next](const drogon::orm::DrogonDbException &) { next(); }, key, std::string("report_only"), std::string("reconcile"));
            } else {
                next();
            }
        },
        [next](const drogon::orm::DrogonDbException &) { next(); }, key);
}
void scanPrefixJournaled(std::shared_ptr<dj::Journal> journal, const std::shared_ptr<ObjectStore> &store, const std::string &prefix, int pageSize,
                         bool isStaging, std::function<void()> finished) {
    auto &scan = scanState();
    std::string marker;
    {
        std::lock_guard<std::mutex> guard(scan.mutex);
        if (scan.store != store) {
            scan.store = store;
            scan.stagingMarker.clear();
            scan.mediaMarker.clear();
        }
        marker = isStaging ? scan.stagingMarker : scan.mediaMarker;
    }
    store->list(prefix, marker, pageSize, [journal, store, isStaging, finished](StoreStatus listed, KeyPage page) {
        if (listed != StoreStatus::Ok) { finished(); return; }
        {
            auto &state = scanState();
            std::lock_guard<std::mutex> guard(state.mutex);
            (isStaging ? state.stagingMarker : state.mediaMarker) = page.nextMarker;
        }
        classifyKeysJournaled(journal, store, std::make_shared<std::vector<std::string>>(std::move(page.keys)), 0, finished);
    });
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

MediaService &MediaService::instance() {
    static MediaService service;
    return service;
}

bool MediaService::allowedType(const std::string &t) {
    return t == "image/jpeg" || t == "image/png" || t == "image/webp";
}

std::string MediaService::fingerprint(const std::string &contentType, int64_t size) {
    return crypto::sha256Hex("media-upload:v1:type=" + std::to_string(contentType.size()) + ":" + contentType +
                             ";size=" + std::to_string(size) + ";");
}

bool MediaService::parseUploadRequest(const Json::Value &body, std::string &contentType, int64_t &size, std::string &message) {
    if (!body.isObject() || body.size() != 2u || !body["content_type"].isString() || !body["size_bytes"].isIntegral() ||
        body["size_bytes"].isBool()) {
        message = "请求必须恰好包含 content_type 和 size_bytes";
        return false;
    }
    contentType = body["content_type"].asString();
    if (!allowedType(contentType)) {
        message = "只支持 JPEG、PNG 和 WebP 图片";
        return false;
    }
    size = body["size_bytes"].asInt64();
    if (size < 1 || size > 20000000) {
        message = "图片大小必须在 1 字节到 20 MB 之间";
        return false;
    }
    return true;
}

std::string MediaService::rejectionFor(int64_t storedSize, int64_t declaredSize, const std::string &realFormat,
                                       const std::string &declaredType, int width, int height) {
    if (storedSize != declaredSize) return "size_mismatch";
    if (!allowedType(realFormat)) return "not_image";
    if (realFormat != declaredType) return "format_mismatch";
    if (width < 1 || height < 1 || width > 30000 || height > 30000 ||
        static_cast<int64_t>(width) * static_cast<int64_t>(height) > 250000000) {
        return "dimensions";
    }
    return "";
}

ErrorCode MediaService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Disabled: return ErrorCode::MediaDisabled;
        case Outcome::NotFound: return ErrorCode::MediaNotFound;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::LimitExceeded: return ErrorCode::MediaLimitExceeded;
        case Outcome::NotUploaded: return ErrorCode::MediaNotUploaded;
        case Outcome::GrantExpired: return ErrorCode::MediaGrantExpired;
        case Outcome::StoreUnavailable: return ErrorCode::MediaStoreUnavailable;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::AlreadyDeleted: return ErrorCode::MediaAlreadyDeleted;
        case Outcome::UndoClosed: return ErrorCode::MediaUndoClosed;
        case Outcome::DeletionNotActive: return ErrorCode::MediaDeletionNotActive;
        case Outcome::DeletionLinked: return ErrorCode::MediaDeletionLinked;
        default: return ErrorCode::InternalError;
    }
}

int MediaService::httpStatusFor(Outcome outcome) { return outcome == Outcome::Created ? 201 : 200; }

Json::Value MediaService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    if (status.deletion.isObject()) {
        body["deletion"] = status.deletion;
        return body;
    }
    body["media"] = status.media;
    body["upload"] = status.upload;
    return body;
}

std::string MediaService::deletionFingerprint(const std::string &mediaId) {
    return crypto::sha256Hex("media-delete:v1:media=" + std::to_string(mediaId.size()) + ":" + mediaId + ";");
}

std::string MediaService::undoFingerprint(const std::string &mediaId, const std::string &deletionId) {
    return crypto::sha256Hex("media-undo:v1:media=" + std::to_string(mediaId.size()) + ":" + mediaId + ";deletion=" +
                             std::to_string(deletionId.size()) + ":" + deletionId + ";");
}

void MediaService::configure(Policy policy, std::shared_ptr<media::ObjectStore> store) {
    std::lock_guard<std::mutex> guard(mutex_);
    policy_ = std::move(policy);
    store_ = std::move(store);
}

void MediaService::configureDeletionJournal(std::shared_ptr<deletionjournal::Journal> journal) {
    std::lock_guard<std::mutex> guard(mutex_);
    deletionJournal_ = std::move(journal);
}

MediaService::CleanupJournalReport MediaService::cleanupJournalReport() {
    std::lock_guard<std::mutex> guard(cleanupState().mutex);
    return cleanupState().report;
}

bool MediaService::cleanupBusy() { return backgroundBusy().load(); }

void MediaService::setCleanupFaultForTest(std::function<bool(const std::string &stage)> fault) {
    std::lock_guard<std::mutex> guard(cleanupState().mutex);
    cleanupState().fault = std::move(fault);
}

std::pair<MediaService::Policy, std::shared_ptr<media::ObjectStore>> MediaService::current() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return {policy_, store_};
}

// =====================================================================
// Actions
// =====================================================================

void MediaService::createUpload(const std::string &userId, const std::string &key, const std::string &contentType, int64_t size, Callback cb) {
    const auto config = current();
    const Policy &policy = config.first;
    const auto &store = config.second;
    if (!store) {
        cb(make(Outcome::Disabled, "图片上传暂未开启"));
        return;
    }
    auto ctx = std::make_shared<GrantCtx>();
    ctx->store = store;
    ctx->policy = policy;
    ctx->userId = userId;
    ctx->key = key;
    ctx->contentType = contentType;
    ctx->size = size;
    ctx->fingerprint = fingerprint(contentType, size);
    ctx->done = std::move(cb);
    beginTx(ctx, [ctx]() {
        inTx(ctx, kMemberSql, "member",
             [ctx](const drogon::orm::Result &rows) {
                 if (rows.size() != 1u) {
                     ctx->scope->rollback([ctx]() { finish(ctx, make(Outcome::SpaceNotFound, "尚未加入空间")); });
                     return;
                 }
                 ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                 inTx(ctx, kCoupleLockSql, "lock",
                      [ctx](const drogon::orm::Result &) { classifyGrant(ctx, [ctx]() { insertGrant(ctx); }); }, ctx->coupleId);
             },
             ctx->userId);
    });
}

void MediaService::complete(const std::string &userId, const std::string &mediaId, Callback cb, Fault fault) {
    const auto config = current();
    const Policy &policy = config.first;
    const auto &store = config.second;
    if (!store) {
        cb(make(Outcome::Disabled, "图片上传暂未开启"));
        return;
    }
    auto ctx = std::make_shared<CompleteCtx>();
    ctx->store = store;
    ctx->policy = policy;
    ctx->fault = fault;
    { std::lock_guard<std::mutex> guard(mutex_); ctx->journal = deletionJournal_; }
    ctx->userId = userId;
    ctx->mediaId = mediaId;
    ctx->done = std::move(cb);
    answerStored(ctx, true);
}

void MediaService::facts(const std::string &userId, const std::string &mediaId, Callback cb) {
    auto ctx = std::make_shared<ReadCtx>();
    ctx->done = std::move(cb);
    onPool(ctx, kFactsSql, "facts",
           [ctx, userId](const drogon::orm::Result &rows) {
               if (rows.size() != 1u || !visibleTo(rows[0], userId)) {
                   finish(ctx, make(Outcome::NotFound, "图片不存在"));
                   return;
               }
               Status status = make(Outcome::Ok);
               status.media = factsJson(rows[0]);
               status.upload = Json::Value(Json::nullValue);
               finish(ctx, std::move(status));
           },
           userId, mediaId);
}

void MediaService::content(const std::string &userId, const std::string &mediaId, const std::string &variant, Callback cb) {
    const auto config = current();
    const Policy &policy = config.first;
    const std::shared_ptr<media::ObjectStore> store = config.second;
    if (!store) {
        cb(make(Outcome::Disabled, "图片暂时无法显示"));
        return;
    }
    auto ctx = std::make_shared<ReadCtx>();
    ctx->done = std::move(cb);
    const int url = policy.readUrlSeconds, bucket = policy.readBucketSeconds;
    const std::string id = lowerId(mediaId);
    // D6: the state is read after 33018 and the URL is made while holding it, so a deletion is entirely before this read or entirely
    // after this URL. Signing makes no network call. The transaction writes nothing and holds no other lock.
    beginTx(ctx, [ctx, userId, variant, store, url, bucket, id]() {
        inTx(ctx, kMediaLockSql, "content_lock",
             [ctx, userId, variant, store, url, bucket, id](const drogon::orm::Result &) {
                 inTx(ctx, kFactsSql, "content",
                      [ctx, userId, variant, store, url, bucket](const drogon::orm::Result &rows) {
                          auto refuse = [ctx]() {
                              ctx->scope->rollback([ctx]() { finish(ctx, make(Outcome::NotFound, "图片不存在")); });
                          };
                          // The only readable key is the READY attempt's. Staging is never signed for reading.
                          if (rows.size() != 1u || !visibleTo(rows[0], userId) || rows[0]["outcome"].isNull() ||
                              rows[0]["outcome"].as<std::string>() != "ready" || rows[0]["removing"].as<bool>() || rows[0]["final_key"].isNull()) {
                              refuse();
                              return;
                          }
                          // O8: the original can carry location data, so only its uploader may download it. Anyone else entitled to the photo
                          // gets variants. The answer is the same 404 as an absent image, and no URL is signed.
                          if (variant != "thumb" && variant != "view" && rows[0]["uploader_id"].as<std::string>() != userId) {
                              refuse();
                              return;
                          }
                          const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                          const int64_t floored = now - now % bucket;
                          auto status = std::make_shared<Status>(make(Outcome::Ok));
                          status->redirectUrl = store->signRead(rows[0]["final_key"].as<std::string>(), variant,
                                                                rows[0]["real_format"].as<std::string>(), floored + url);
                          ctx->scope->rollback([ctx, status]() { finish(ctx, std::move(*status)); });
                      },
                      userId, id);
             },
             id);
    });
}

void MediaService::deletePhoto(const std::string &userId, const std::string &mediaId, const std::string &key, Callback cb) {
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> guard(mutex_); journal = deletionJournal_; }
    if (journal) {
        auto ctx = std::make_shared<JournalMediaCtx>();
        ctx->journal = std::move(journal);
        ctx->userId = userId; ctx->mediaId = lowerId(mediaId); ctx->key = key;
        ctx->fingerprint = deletionFingerprint(ctx->mediaId); ctx->done = std::move(cb);
        prepareJournalMedia(ctx); return;
    }
    auto ctx = std::make_shared<DeletionCtx>();
    ctx->userId = userId;
    ctx->mediaId = lowerId(mediaId);
    ctx->key = key;
    ctx->fingerprint = deletionFingerprint(ctx->mediaId);
    ctx->done = std::move(cb);
    beginTx(ctx, [ctx]() { deleteStart(ctx); });
}

namespace {
// One statement answers a whole page of either list: the position, the items as stored, and one more row to say whether more follow.
void readPage(const char *firstSql, const char *afterSql, const char *label, const std::string &userId, int limit,
              const std::optional<std::pair<std::string, std::string>> &after, std::function<void(MediaService::DeletionPage)> cb) {
    using DeletionPage = MediaService::DeletionPage;
    const std::string detail = std::string("media stage=") + label;
    auto done = std::make_shared<std::function<void(DeletionPage)>>(std::move(cb));
    auto onRows = [done, limit, detail](const drogon::orm::Result &rows) {
        DeletionPage page;
        if (rows.size() == 0u) {
            page.outcome = Outcome::SpaceNotFound;
            page.message = "尚未加入空间";
            (*done)(std::move(page));
            return;
        }
        try {
            Json::CharReaderBuilder builder;
            const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            for (const auto &row : rows) {
                if (row["deletion_json"].isNull()) continue;
                if (static_cast<int>(page.items.size()) == limit) {
                    const auto &last = rows[page.items.size() - 1];
                    page.nextCursor = AlbumService::encodeCursor({last["sort_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                const std::string text = row["deletion_json"].as<std::string>();
                Json::Value item;
                std::string errors;
                if (!reader->parse(text.data(), text.data() + text.size(), &item, &errors)) throw std::runtime_error("json");
                page.items.push_back(std::move(item));
            }
            page.outcome = Outcome::Ok;
        } catch (const std::exception &) {
            DeletionPage failed;
            failed.detail = detail;
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(page));
    };
    auto onError = [done, detail](const drogon::orm::DrogonDbException &) {
        DeletionPage failed;
        failed.detail = detail;
        (*done)(std::move(failed));
    };
    const auto &client = db::Pool::instance().client();
    if (after) {
        client->execSqlAsync(afterSql, std::move(onRows), std::move(onError), userId, after->first, after->second, std::to_string(limit + 1));
    } else {
        client->execSqlAsync(firstSql, std::move(onRows), std::move(onError), userId, std::to_string(limit + 1));
    }
}
}  // namespace

void MediaService::listUndoable(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                                std::function<void(DeletionPage)> cb) {
    readPage(kUndoableFirstSql, kUndoableAfterSql, "undoable_read", userId, limit, after, std::move(cb));
}

void MediaService::listRetained(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                                std::function<void(DeletionPage)> cb) {
    readPage(kRetainedFirstSql, kRetainedAfterSql, "retained_read", userId, limit, after, std::move(cb));
}

void MediaService::capabilities(const std::string &userId, std::function<void(Capabilities)> cb) {
    auto done = std::make_shared<std::function<void(Capabilities)>>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        "SELECT 1 AS member FROM couple_members c WHERE c.user_id = $1::uuid",
        [done](const drogon::orm::Result &rows) {
            Capabilities result;
            if (rows.size() == 0u) {
                result.outcome = Outcome::SpaceNotFound;
                result.message = "尚未加入空间";
            } else {
                result.outcome = Outcome::Ok;
                // The same predicate as every uploads_enabled field, read once here.
                result.uploadsEnabled = MediaService::instance().uploadsEnabled();
            }
            (*done)(std::move(result));
        },
        [done](const drogon::orm::DrogonDbException &) { (*done)(Capabilities{}); },
        userId);
}

void MediaService::listUnattached(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after,
                                  std::function<void(DeletionPage)> cb) {
    readPage(kUnattachedFirstSql, kUnattachedAfterSql, "unattached_read", userId, limit, after, std::move(cb));
}

void MediaService::undoDeletion(const std::string &userId, const std::string &mediaId, const std::string &deletionId, const std::string &key, Callback cb,
                                Fault fault) {
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> guard(mutex_); journal = deletionJournal_; }
    if (journal) {
        auto ctx = std::make_shared<JournalMediaCtx>();
        ctx->journal = std::move(journal); ctx->undo = true; ctx->fault = fault;
        ctx->userId = userId; ctx->mediaId = lowerId(mediaId); ctx->deletionId = lowerId(deletionId); ctx->key = key;
        ctx->fingerprint = undoFingerprint(ctx->mediaId, ctx->deletionId); ctx->done = std::move(cb);
        prepareJournalMedia(ctx); return;
    }
    auto ctx = std::make_shared<DeletionCtx>();
    ctx->isUndo = true;
    ctx->fault = fault;
    ctx->userId = userId;
    ctx->mediaId = lowerId(mediaId);
    ctx->deletionId = lowerId(deletionId);
    ctx->key = key;
    ctx->fingerprint = undoFingerprint(ctx->mediaId, ctx->deletionId);
    ctx->done = std::move(cb);
    beginTx(ctx, [ctx]() { undoStart(ctx); });
}

void MediaService::runTick(drogon::orm::Transaction &tx, std::function<void(TickReport)> done) {
    const auto config = current();
    std::shared_ptr<deletionjournal::Journal> journal;
    { std::lock_guard<std::mutex> guard(mutex_); journal = deletionJournal_; }
    const Policy &policy = config.first;
    const auto &store = config.second;
    if (!store) {
        TickReport report;
        report.ok = true;
        report.detail = "media dispatch=off";
        done(std::move(report));
        return;
    }
    auto ctx = std::make_shared<TickCtx>();
    ctx->tx = &tx;
    ctx->policy = policy;
    ctx->store = store;
    ctx->done = std::move(done);

    auto chooseWork = [ctx, journal]() {
        std::string created, media, scope;
        if (journal) {
            std::lock_guard<std::mutex> guard(cleanupState().mutex);
            created = cleanupState().workCreated; media = cleanupState().workMedia; scope = cleanupState().workScope;
        }
        auto chosen = [ctx, journal](const drogon::orm::Result &rows) {
                     if (journal) {
                         std::lock_guard<std::mutex> guard(cleanupState().mutex);
                         auto &state = cleanupState();
                         if (rows.size() < static_cast<size_t>(ctx->policy.planLimit)) {
                             state.workCreated = "-infinity"; state.workMedia = "00000000-0000-0000-0000-000000000000"; state.workScope.clear();
                         } else {
                             const auto last = rows[rows.size() - 1];
                             state.workCreated = last["created_text"].as<std::string>(); state.workMedia = last["media_id"].as<std::string>();
                             state.workScope = last["scope"].as<std::string>();
                         }
                     }
                     auto work = std::make_shared<std::vector<WorkItem>>();
                     for (const auto &row : rows) {
                         work->push_back({row["media_id"].as<std::string>(), row["scope"].as<std::string>(),
                                          row["sweep_due"].as<bool>() ? "sweep" : "first", parseStrings(row["keys"].as<std::string>())});
                     }
                     MediaService::TickReport report;
                     report.ok = true;
                     std::ostringstream detail;
                     detail << "media planned=" << ctx->planned << " passes=" << work->size();
                     report.detail = detail.str();
                     auto store = ctx->store;
                     const int pageSize = ctx->policy.scanPageSize;
                     // Ticks do not overlap this work: at most one store-side round runs at a time.
                     report.afterCommit = [store, work, pageSize, journal]() {
                         bool expected = false;
                         if (!backgroundBusy().compare_exchange_strong(expected, true)) return;
                         // J2c2: the journaled round. Passes, then missing acknowledgements, then the reconciler.
                         if (journal) {
                             runJournaledWork(journal, store, work, 0, [journal, store, pageSize]() {
                                 reconcileAcknowledgements(journal, store, [journal, store, pageSize]() {
                                     scanPrefixJournaled(journal, store, "staging/", pageSize, true, [journal, store, pageSize]() {
                                         scanPrefixJournaled(journal, store, "media/", pageSize, false, []() { backgroundBusy().store(false); });
                                     });
                                 });
                             });
                             return;
                         }
                         runWork(store, work, 0, [store, pageSize]() {
                             scanPrefix(store, "staging/", pageSize, true, [store, pageSize]() {
                                 scanPrefix(store, "media/", pageSize, false, []() { backgroundBusy().store(false); });
                             });
                         });
                     };
                     finishTick(ctx, std::move(report));
        };
        if (journal) tickStep(ctx, kWorkAfterSql, "work", chosen, std::to_string(ctx->policy.planLimit), created, media, scope);
        else tickStep(ctx, kWorkSql, "work", chosen, std::to_string(ctx->policy.planLimit));
    };

    // All three candidate sets are read, then locked in one sorted statement, the order every multi-photo write uses. Locking them
    // set by set could wait against such a write once a deleted photo and an unattached one share a request. Each plan statement then
    // re-evaluates its own rule, so a commit that finished before the locks were granted drops a row instead of failing the tick.
    auto planAll = [ctx, chooseWork](const std::string &abandoned, const std::string &unattached, const std::string &purges) {
        auto all = parseStrings(abandoned);
        for (auto &id : parseStrings(unattached)) all.push_back(std::move(id));
        for (auto &id : parseStrings(purges)) all.push_back(std::move(id));
        if (all.empty()) { chooseWork(); return; }
        Json::Value merged(Json::arrayValue);
        for (const auto &id : all) merged.append(id);
        const std::string grace = std::to_string(ctx->policy.sweepGraceSeconds);
        tickStep(ctx, kLockCandidatesSql, "cleanup_lock",
                 [ctx, chooseWork, abandoned, unattached, purges, grace](const drogon::orm::Result &) {
                     tickStep(ctx, kPlanAbandonedSql, "abandoned_plan",
                              [ctx, chooseWork, unattached, purges, grace](const drogon::orm::Result &first) {
                                  ctx->planned += first.affectedRows();
                                  tickStep(ctx, kPlanUnattachedSql, "unattached_plan",
                                           [ctx, chooseWork, purges, grace](const drogon::orm::Result &second) {
                                               ctx->planned += second.affectedRows();
                                               tickStep(ctx, kPlanPurgeSql, "purge_plan",
                                                        [ctx, chooseWork](const drogon::orm::Result &third) {
                                                            ctx->planned += third.affectedRows();
                                                            chooseWork();
                                                        },
                                                        purges, grace);
                                           },
                                           std::to_string(ctx->policy.unattachedSeconds), unattached, grace);
                              },
                              std::to_string(ctx->policy.abandonAfterSeconds), abandoned, grace);
                 },
                 compactJson(merged));
    };

    const std::string limit = std::to_string(ctx->policy.planLimit);
    tickStep(ctx, kAbandonedCandidatesSql, "abandoned",
             [ctx, planAll, limit](const drogon::orm::Result &abandoned) {
                 const std::string abandonedIds = abandoned[0]["ids"].as<std::string>();
                 tickStep(ctx, kUnattachedCandidatesSql, "unattached",
                          [ctx, planAll, limit, abandonedIds](const drogon::orm::Result &unattached) {
                              const std::string unattachedIds = unattached[0]["ids"].as<std::string>();
                              tickStep(ctx, kPurgeCandidatesSql, "purge",
                                       [planAll, abandonedIds, unattachedIds](const drogon::orm::Result &purges) {
                                           planAll(abandonedIds, unattachedIds, purges[0]["ids"].as<std::string>());
                                       },
                                       limit);
                          },
                          std::to_string(ctx->policy.unattachedSeconds), limit);
             },
             std::to_string(ctx->policy.abandonAfterSeconds), limit);
}

}  // namespace usapp
