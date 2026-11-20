// =====================================================================
// services/api/src/services/MemoryService.cc
// =====================================================================
#include "services/MemoryService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <cctype>
#include <exception>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AlbumService.h"
#include "services/AnniversaryCalendar.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
#define USAPP_DATE(expr) "to_char(" expr ", 'YYYY-MM-DD')"

// One revision's or proposal's ordered photos. Never an object key. D6: a deleted photo keeps its position as a placeholder: it says
// so, has no dimensions, and its content route answers 404. An undo restores it.
#define USAPP_PHOTOS(table, where)                                                                               \
    "(SELECT COALESCE(json_agg(json_build_object('position', ph.position, 'media_id', ph.media_id, "             \
    "'uploader_id', mo.uploader_id, 'width', mt.width, 'height', mt.height, "                                    \
    "'deleted', usapp_media_deleted_now(ph.media_id)) ORDER BY ph.position), '[]'::json) "                       \
    "FROM " table " ph JOIN media_objects mo ON mo.id = ph.media_id "                                            \
    "LEFT JOIN media_upload_outcomes mt ON mt.media_id = ph.media_id AND NOT usapp_media_deleted_now(ph.media_id) WHERE " where ")"

// The current state of memory m with its latest revision r. Both members read it.
#define USAPP_MEMORY_JSON                                                                                        \
    "json_build_object('id', m.id, 'creator_id', m.creator_id, 'version', r.version, "                           \
    "'date', " USAPP_DATE("r.memory_date") ", 'title', tx.title, 'description', tx.description, "                \
    "'created_at', " USAPP_TS("m.created_at") ", 'updated_at', " USAPP_TS("r.created_at") ", "                   \
    "'photos', " USAPP_PHOTOS("memory_revision_photos", "ph.memory_id = m.id AND ph.version = r.version") ", "   \
    "'pending_proposal', (SELECT json_build_object('id', p.id, 'proposer_id', p.proposer_id, "                   \
    "   'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ") "                            \
    "   FROM memory_edit_proposals p WHERE p.memory_id = m.id "                                                  \
    "   AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = p.id) LIMIT 1), "            \
    "'pending_delete_request', (SELECT json_build_object('id', q.id, 'requester_id', q.requester_id, "           \
    "   'created_at', " USAPP_TS("q.created_at") ") FROM memory_delete_requests q "                              \
    "   WHERE q.id = usapp_memory_pending_delete_request(m.id)))"
#define USAPP_MEMORY_FROM                                                                                       \
    "memories m JOIN LATERAL (SELECT x.* FROM memory_revisions x WHERE x.memory_id = m.id "                      \
    "ORDER BY x.version DESC LIMIT 1) r ON true JOIN memory_texts tx ON tx.id = r.text_id"

const char *kGetSql =
    "SELECT " USAPP_MEMORY_JSON "::text AS memory_json FROM couple_members c JOIN " USAPP_MEMORY_FROM
    " ON m.couple_id = c.couple_id WHERE c.user_id = $1::uuid AND m.id = $2::uuid AND NOT usapp_memory_deleted_now(m.id)";

// Pages by (current date, created_at, id) descending. One row per memory, or one row of NULLs when the space has none.
#define USAPP_LIST_SQL(filter, limit)                                                                            \
    "SELECT c.couple_id::text AS couple_id, x.* FROM couple_members c LEFT JOIN LATERAL ("                       \
    "SELECT " USAPP_MEMORY_JSON "::text AS memory_json, " USAPP_DATE("r.memory_date") " AS sort_date, "          \
    USAPP_TS("m.created_at") " AS sort_created_at, m.id::text AS sort_id FROM " USAPP_MEMORY_FROM                \
    " WHERE m.couple_id = c.couple_id AND NOT usapp_memory_deleted_now(m.id)" filter " ORDER BY r.memory_date DESC, m.created_at DESC, m.id DESC LIMIT " limit ") x ON true " \
    "WHERE c.user_id = $1::uuid"
const char *kListFirstSql = USAPP_LIST_SQL("", "$2::integer");
const char *kListAfterSql = USAPP_LIST_SQL(
    " AND (r.memory_date, m.created_at, m.id) < ($2::date, $3::timestamptz, $4::uuid)", "$5::integer");

// V1 section 30: the immutable original and suggestion, with the requester, time and status.
const char *kProposalSql =
    "SELECT json_build_object('id', p.id, 'memory_id', p.memory_id, 'proposer_id', p.proposer_id, 'base_version', p.base_version, "
    "'created_at', " USAPP_TS("p.created_at") ", "
    "'status', COALESCE(d.outcome, 'pending'), 'decided_by', d.decided_by, 'decided_at', " USAPP_TS("d.decided_at") ", "
    "'approved_version', ar.version, "
    "'original', json_build_object('version', b.version, 'date', " USAPP_DATE("b.memory_date") ", 'title', bt.title, 'description', bt.description, "
    "  'photos', " USAPP_PHOTOS("memory_revision_photos", "ph.memory_id = b.memory_id AND ph.version = b.version") "), "
    "'proposed', json_build_object('date', " USAPP_DATE("p.memory_date") ", 'title', pt.title, 'description', pt.description, "
    "  'photos', " USAPP_PHOTOS("memory_proposal_photos", "ph.proposal_id = p.id") "))::text AS proposal_json "
    "FROM couple_members c JOIN memories m ON m.couple_id = c.couple_id AND m.id = $2::uuid "
    "JOIN memory_edit_proposals p ON p.memory_id = m.id AND p.id = $3::uuid "
    "JOIN memory_texts pt ON pt.id = p.text_id "
    "JOIN memory_revisions b ON b.memory_id = p.memory_id AND b.version = p.base_version "
    "JOIN memory_texts bt ON bt.id = b.text_id "
    "LEFT JOIN memory_edit_decisions d ON d.proposal_id = p.id "
    "LEFT JOIN memory_revisions ar ON ar.proposal_id = p.id "
    "WHERE c.user_id = $1::uuid AND NOT usapp_memory_deleted_now(m.id)";

// One memory's proposals as summaries, newest first. No row: not in a space. memory_id NULL: absent, deleted, purged or another space's.
// Otherwise one row per proposal, or one row of NULLs when there is none. Served by memory_edit_proposals_memory_idx.
#define USAPP_PROPOSAL_LIST_SQL(filter, limit)                                                                   \
    "SELECT m.id::text AS memory_id, (SELECT max(v.version) FROM memory_revisions v WHERE v.memory_id = m.id) AS current_version, x.* " \
    "FROM couple_members c LEFT JOIN memories m ON m.couple_id = c.couple_id AND m.id = $2::uuid AND NOT usapp_memory_deleted_now(m.id) " \
    "LEFT JOIN LATERAL (SELECT json_build_object('id', p.id, 'memory_id', p.memory_id, 'proposer_id', p.proposer_id, "            \
    "'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ", 'status', COALESCE(d.outcome, 'pending'), "     \
    "'decided_by', d.decided_by, 'decided_at', " USAPP_TS("d.decided_at") ", 'approved_version', ar.version)::text AS proposal_json, " \
    USAPP_TS("p.created_at") " AS sort_at, p.id::text AS sort_id FROM memory_edit_proposals p "                 \
    "LEFT JOIN memory_edit_decisions d ON d.proposal_id = p.id LEFT JOIN memory_revisions ar ON ar.proposal_id = p.id " \
    "WHERE p.memory_id = m.id" filter " ORDER BY p.created_at DESC, p.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kProposalsFirstSql = USAPP_PROPOSAL_LIST_SQL("", "$3::integer");
const char *kProposalsAfterSql = USAPP_PROPOSAL_LIST_SQL(" AND (p.created_at, p.id) < ($3::timestamptz, $4::uuid)", "$5::integer");
#undef USAPP_PROPOSAL_LIST_SQL

const char *kMembershipSql = "SELECT c.couple_id::text AS couple_id FROM couple_members c WHERE c.user_id = $1::uuid FOR SHARE OF c";
const char *kTargetSql =
    "SELECT c.couple_id::text AS couple_id, m.id::text AS memory_id, m.creator_id::text AS creator_id "
    "FROM couple_members c LEFT JOIN memories m ON m.couple_id = c.couple_id AND m.id = $2::uuid "
    "WHERE c.user_id = $1::uuid FOR SHARE OF c";
const char *kDecisionTargetSql =
    "SELECT c.couple_id::text AS couple_id, m.id::text AS memory_id, m.creator_id::text AS creator_id, "
    "p.id::text AS proposal_id, p.proposer_id::text AS proposer_id FROM couple_members c "
    "LEFT JOIN memories m ON m.couple_id = c.couple_id AND m.id = $2::uuid "
    "LEFT JOIN memory_edit_proposals p ON p.memory_id = m.id AND p.id = $3::uuid "
    "WHERE c.user_id = $1::uuid FOR SHARE OF c";

const char *kMemoryLockSql = "SELECT pg_advisory_xact_lock(33020::integer, hashtext($1::text))";
// Sorted within one statement, the same order the cleanup job uses.
const char *kPhotoLockSql =
    "SELECT pg_advisory_xact_lock(33018::integer, k.h) FROM (SELECT DISTINCT hashtext(x) AS h "
    "FROM jsonb_array_elements_text($1::jsonb) x ORDER BY 1) k";

const char *kStateSql =
    "SELECT r.version, tx.title, tx.description, " USAPP_DATE("r.memory_date") " AS memory_date, "
    "(SELECT COALESCE(jsonb_agg(ph.media_id ORDER BY ph.position), '[]'::jsonb)::text FROM memory_revision_photos ph "
    " WHERE ph.memory_id = r.memory_id AND ph.version = r.version) AS photos, "
    "EXISTS (SELECT 1 FROM memory_edit_proposals q WHERE q.memory_id = r.memory_id "
    "AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = q.id)) AS has_pending, "
    "usapp_memory_pending_delete_request(r.memory_id) IS NOT NULL AS has_delete_request, "
    "usapp_memory_deleted_now(r.memory_id) AS deleted "
    "FROM memory_revisions r JOIN memory_texts tx ON tx.id = r.text_id WHERE r.memory_id = $1::uuid ORDER BY r.version DESC LIMIT 1";
const char *kDecisionStateSql =
    "SELECT (SELECT max(r.version) FROM memory_revisions r WHERE r.memory_id = p.memory_id) AS current_version, p.base_version, "
    "EXISTS (SELECT 1 FROM memory_edit_decisions d WHERE d.proposal_id = p.id) AS decided, usapp_memory_deleted_now(p.memory_id) AS deleted, "
    "(SELECT COALESCE(jsonb_agg(ph.media_id::text), '[]'::jsonb)::text FROM memory_proposal_photos ph WHERE ph.proposal_id = p.id) AS photos "
    "FROM memory_edit_proposals p WHERE p.id = $1::uuid";

const char *kTodaySql =
    "SELECT $1::date > (clock_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS in_future";

// Read after the photo locks. The caller's own unbound object, or one already this memory's: READY, not planned, not reserved.
const char *kPhotoCheckSql =
    "SELECT count(*)::integer AS eligible FROM jsonb_array_elements_text($1::jsonb) w "
    "JOIN media_objects o ON o.id = w::uuid AND o.couple_id = $2::uuid "
    "JOIN media_upload_outcomes t ON t.media_id = o.id AND t.outcome = 'ready' "
    "LEFT JOIN media_bindings b ON b.media_id = o.id "
    "WHERE NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = o.id AND p.scope = 'everything') "
    "AND NOT usapp_media_deleted_now(o.id) "
    "AND ((b.media_id IS NOT NULL AND b.target_kind = 'memory' AND b.target_id = NULLIF($3::text, '')::uuid) "
    "  OR (b.media_id IS NULL AND o.uploader_id = $4::uuid AND usapp_media_pending_reference(o.id) IS NULL))";
// At approval: everything the proposal named is still usable. Its uploader may have deleted a reserved or kept photo since; that
// refuses, and only their undo brings it back: an undo approves nothing.
const char *kApprovalPhotoCheckSql =
    "SELECT count(*)::integer AS total, count(*) FILTER (WHERE t.outcome = 'ready' AND NOT usapp_media_deleted_now(x.media_id) AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p "
    "WHERE p.media_id = x.media_id AND p.scope = 'everything'))::integer AS eligible FROM memory_proposal_photos x "
    "LEFT JOIN media_upload_outcomes t ON t.media_id = x.media_id WHERE x.proposal_id = $1::uuid";

const char *kRevisionByKeySql =
    "SELECT memory_id::text AS ref, version::text AS ref2, request_fingerprint FROM memory_revisions WHERE actor_id = $1::uuid AND idempotency_key = $2";
const char *kProposalByKeySql =
    "SELECT id::text AS ref, '' AS ref2, request_fingerprint FROM memory_edit_proposals WHERE proposer_id = $1::uuid AND idempotency_key = $2";
const char *kDecisionByKeySql =
    "SELECT proposal_id::text AS ref, '' AS ref2, request_fingerprint FROM memory_edit_decisions WHERE decided_by = $1::uuid AND idempotency_key = $2";

const char *kRevisionFactsSql =
    "SELECT r.memory_id::text AS memory_id, json_build_object('action', r.action, 'memory_id', r.memory_id, 'version', r.version, "
    "'actor_id', r.actor_id, 'proposal_id', r.proposal_id, 'created_at', " USAPP_TS("r.created_at") ")::text AS mutation_json "
    "FROM memory_revisions r WHERE r.memory_id = $1::uuid AND r.version = $2::integer";
const char *kProposalFactsSql =
    "SELECT p.memory_id::text AS memory_id, json_build_object('action', 'propose', 'memory_id', p.memory_id, 'proposal_id', p.id, "
    "'proposer_id', p.proposer_id, 'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ")::text AS mutation_json "
    "FROM memory_edit_proposals p WHERE p.id = $1::uuid";
const char *kDecisionFactsSql =
    "SELECT p.memory_id::text AS memory_id, json_build_object('action', 'decide', 'memory_id', p.memory_id, 'proposal_id', d.proposal_id, "
    "'proposer_id', p.proposer_id, 'outcome', d.outcome, 'decided_by', d.decided_by, 'base_version', p.base_version, "
    "'version', r.version, 'decided_at', " USAPP_TS("d.decided_at") ")::text AS mutation_json "
    "FROM memory_edit_decisions d JOIN memory_edit_proposals p ON p.id = d.proposal_id "
    "LEFT JOIN memory_revisions r ON r.proposal_id = d.proposal_id WHERE d.proposal_id = $1::uuid";

const char *kCurrentInTxSql =
    "SELECT " USAPP_MEMORY_JSON "::text AS memory_json FROM " USAPP_MEMORY_FROM " WHERE m.id = $1::uuid AND NOT usapp_memory_deleted_now(m.id)";

const char *kIdentityInsertSql = "INSERT INTO memories (id, couple_id, creator_id) VALUES ($1::uuid, $2::uuid, $3::uuid)";
const char *kTextInsertSql =
    "INSERT INTO memory_texts (couple_id, title, description) VALUES ($1::uuid, $2::text, $3::text) RETURNING id::text AS id";
const char *kRevisionInsertSql =
    "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, memory_date, text_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::integer, $4::uuid, $5::text, $6::date, $7::uuid, $8::text, $9::text) ON CONFLICT DO NOTHING RETURNING version";
// The guard refuses anything not READY, planned, another's, or reserved elsewhere. Already bound here: nothing to do.
const char *kBindSql =
    "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
    "SELECT x::uuid, $2::uuid, 'memory', $3::uuid, $4::uuid FROM jsonb_array_elements_text($1::jsonb) x "
    "WHERE NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = x::uuid)";
const char *kRevisionPhotosSql =
    "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) "
    "SELECT $1::uuid, $2::integer, o.n, o.v::uuid, $3::uuid FROM jsonb_array_elements_text($4::jsonb) WITH ORDINALITY o(v, n)";
const char *kProposalInsertSql =
    "INSERT INTO memory_edit_proposals (id, couple_id, memory_id, proposer_id, base_version, memory_date, text_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::integer, $6::date, $7::uuid, $8::text, $9::text) ON CONFLICT DO NOTHING RETURNING id::text AS id";
const char *kProposalPhotosSql =
    "INSERT INTO memory_proposal_photos (proposal_id, position, media_id, couple_id) "
    "SELECT $1::uuid, o.n, o.v::uuid, $2::uuid FROM jsonb_array_elements_text($3::jsonb) WITH ORDINALITY o(v, n)";
const char *kDecisionInsertSql =
    "INSERT INTO memory_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING proposal_id::text AS id";
// Approval: exactly the proposal's date, text and photos, as base_version + 1.
const char *kApproveRevisionSql =
    "INSERT INTO memory_revisions (memory_id, couple_id, version, actor_id, action, proposal_id, memory_date, text_id) "
    "SELECT p.memory_id, p.couple_id, p.base_version + 1, $2::uuid, 'approve', p.id, p.memory_date, p.text_id "
    "FROM memory_edit_proposals p WHERE p.id = $1::uuid RETURNING version";
const char *kApproveBindSql =
    "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
    "SELECT x.media_id, x.couple_id, 'memory', p.memory_id, $2::uuid FROM memory_proposal_photos x "
    "JOIN memory_edit_proposals p ON p.id = x.proposal_id WHERE x.proposal_id = $1::uuid "
    "AND NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = x.media_id) ORDER BY x.media_id";
const char *kApprovePhotosSql =
    "INSERT INTO memory_revision_photos (memory_id, version, position, media_id, couple_id) "
    "SELECT p.memory_id, $2::integer, x.position, x.media_id, x.couple_id FROM memory_proposal_photos x "
    "JOIN memory_edit_proposals p ON p.id = x.proposal_id WHERE x.proposal_id = $1::uuid";

// Ids, version and outcome only: never text.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'memory', $4::text, $5::jsonb)";

// ---- D6 M1a. Facts of one exact cycle, never text or photos. No row in the state reads: absent, or in another couple.
const char *kDeletionByKeySql =
    "SELECT d.id::text AS id, d.request_fingerprint FROM memory_deletions d WHERE d.deleted_by = $1::uuid AND d.idempotency_key = $2";
const char *kUndoByKeySql =
    "SELECT r.deletion_id::text AS id, r.request_fingerprint FROM memory_deletion_reversals r WHERE r.reversed_by = $1::uuid AND r.idempotency_key = $2";
const char *kDeleteStateSql =
    "SELECT usapp_memory_deleted_now($1::uuid) AS deleted, (SELECT q.id::text FROM memory_edit_proposals q WHERE q.memory_id = $1::uuid "
    "AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions x WHERE x.proposal_id = q.id) LIMIT 1) AS pending_proposal, "
    "usapp_memory_pending_delete_request($1::uuid)::text AS pending_delete_request";
const char *kUndoStateSql =
    "SELECT d.deleted_by::text AS deleted_by, EXISTS (SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id = d.id) AS reversed, "
    "usapp_media_undo_open(d.purge_after, clock_timestamp()) AS open "
    "FROM memory_deletions d WHERE d.id = $1::uuid AND d.memory_id = $2::uuid AND d.couple_id = $3::uuid";
// The deadline is computed once, from the database clock.
const char *kDeletionInsertSql =
    "INSERT INTO memory_deletions (id, memory_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
    "SELECT $6::uuid, $1::uuid, $2::uuid, $3::uuid, 1 + COALESCE((SELECT max(x.cycle_no) FROM memory_deletions x WHERE x.memory_id = $1::uuid), 0), "
    "$4::text, $5::text, n.t, n.t + interval '168 hours' FROM (SELECT clock_timestamp() AS t) n "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";
const char *kReversalInsertSql =
    "INSERT INTO memory_deletion_reversals (deletion_id, memory_id, couple_id, reversed_by, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING deletion_id::text AS id";
const char *kDeletionFactsSql =
    "SELECT json_build_object('deletion_id', d.id, 'memory_id', d.memory_id, 'cycle_no', d.cycle_no, "
    "'deleted_at', " USAPP_TS("d.deleted_at") ", 'purge_after', " USAPP_TS("d.purge_after") ", 'undone_at', " USAPP_TS("r.reversed_at") ", "
    "'state', CASE WHEN r.deletion_id IS NOT NULL THEN 'undone' ELSE 'deleted' END)::text AS deletion_json "
    "FROM memory_deletions d LEFT JOIN memory_deletion_reversals r ON r.deletion_id = d.id WHERE d.id = $1::uuid";

// The caller's own undoable cycles, newest first, with the title the creator can still read. No row: not in a space. One row of NULLs: none.
#define USAPP_MEMORY_DELETIONS_SQL(filter, limit)                                                                \
    "SELECT x.* FROM couple_members c LEFT JOIN LATERAL (SELECT json_build_object('deletion_id', d.id, 'memory_id', d.memory_id, 'title', tx.title, " \
    "'deleted_at', " USAPP_TS("d.deleted_at") ", 'purge_after', " USAPP_TS("d.purge_after") ")::text AS deletion_json, "         \
    USAPP_TS("d.deleted_at") " AS sort_at, d.id::text AS sort_id FROM memory_deletions d "                       \
    "JOIN LATERAL (SELECT r.text_id FROM memory_revisions r WHERE r.memory_id = d.memory_id ORDER BY r.version DESC LIMIT 1) rv ON true " \
    "JOIN memory_texts tx ON tx.id = rv.text_id "                                                                \
    "WHERE d.deleted_by = c.user_id AND d.couple_id = c.couple_id "                                              \
    "AND usapp_media_undo_open(d.purge_after, clock_timestamp()) "                                               \
    "AND NOT EXISTS (SELECT 1 FROM memory_deletion_reversals k WHERE k.deletion_id = d.id)" filter               \
    " ORDER BY d.deleted_at DESC, d.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kDeletionsFirstSql = USAPP_MEMORY_DELETIONS_SQL("", "$2::integer");
const char *kDeletionsAfterSql = USAPP_MEMORY_DELETIONS_SQL(" AND (d.deleted_at, d.id) < ($2::timestamptz, $3::uuid)", "$4::integer");
#undef USAPP_MEMORY_DELETIONS_SQL

// ---- D6 M1c. The creator's photos bound to this memory, sorted, before the cycle's row: its time then follows any undo of them.
const char *kCascadeLockSql =
    "SELECT pg_advisory_xact_lock(33018::integer, k.h) FROM (SELECT DISTINCT hashtext(b.media_id::text) AS h "
    "FROM media_bindings b JOIN media_objects o ON o.id = b.media_id "
    "WHERE b.target_kind = 'memory' AND b.target_id = $1::uuid AND o.uploader_id = $2::uuid ORDER BY 1) k";
const char *kCascadeSql = "SELECT usapp_memory_cascade_photos($1::uuid) AS n";
const char *kJournalCandidatesSql =
    "SELECT b.media_id::text AS media_id FROM media_bindings b JOIN media_objects o ON o.id = b.media_id "
    "WHERE b.target_kind = 'memory' AND b.target_id = $1::uuid AND o.uploader_id = $2::uuid "
    "AND NOT usapp_media_deleted_now(b.media_id) AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p "
    "WHERE p.media_id = b.media_id AND p.scope = 'everything') ORDER BY b.media_id";
const char *kJournalLinksSql =
    "SELECT media_id::text AS media_id, media_deletion_id::text AS cycle_id FROM memory_deletion_photo_cycles "
    "WHERE memory_deletion_id = $1::uuid ORDER BY media_id";
// The same M1c rows/keys/fingerprints/times, with prepared IDs. The existing
// row/deferred guards still require this transaction and complete coverage.
const char *kJournalCascadeSql =
    "WITH x AS (SELECT * FROM memory_deletions WHERE id = $1::uuid), "
    "e AS (SELECT * FROM jsonb_to_recordset($2::jsonb) AS e(media_id uuid, cycle_id uuid)), "
    "made AS (INSERT INTO media_deletions (id, media_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after) "
    "SELECT e.cycle_id, e.media_id, x.couple_id, x.deleted_by, "
    "1 + COALESCE((SELECT max(d.cycle_no) FROM media_deletions d WHERE d.media_id = e.media_id), 0), "
    "'mc-' || x.id::text || '-' || e.media_id::text, "
    "encode(sha256(convert_to('memory-cascade:v1:' || x.id::text || ':' || e.media_id::text, 'UTF8')), 'hex'), "
    "x.deleted_at, x.purge_after FROM x CROSS JOIN e ORDER BY hashtext(e.media_id::text) RETURNING id) SELECT id FROM made";
const char *kJournalLinkSql =
    "WITH x AS (SELECT * FROM memory_deletions WHERE id = $1::uuid), "
    "e AS (SELECT * FROM jsonb_to_recordset($2::jsonb) AS e(media_id uuid, cycle_id uuid)), "
    "links AS (INSERT INTO memory_deletion_photo_cycles (media_deletion_id, media_id, memory_deletion_id, memory_id, couple_id) "
    "SELECT e.cycle_id, e.media_id, x.id, x.memory_id, x.couple_id FROM e CROSS JOIN x RETURNING 1) "
    "SELECT count(*)::integer AS n FROM links";
const char *kLinkedLockSql =
    "SELECT pg_advisory_xact_lock(33018::integer, k.h) FROM (SELECT DISTINCT hashtext(l.media_id::text) AS h "
    "FROM memory_deletion_photo_cycles l WHERE l.memory_deletion_id = $1::uuid ORDER BY 1) k";
// Read after those locks: a purge plan on any linked photo closes the whole undo.
const char *kLinkedStateSql =
    "SELECT count(*)::integer AS n FROM memory_deletion_photo_cycles l JOIN media_cleanup_plans p ON p.media_id = l.media_id AND p.scope = 'everything' "
    "WHERE l.memory_deletion_id = $1::uuid";
const char *kCascadeUndoSql = "SELECT usapp_memory_cascade_undo($1::uuid) AS n";

// ---- D6 M1b. Facts of one delete request, never text or photos, and never the cycle: only the approver's answer adds that.
#define USAPP_DELETE_REQUEST_JSON                                                                                \
    "json_build_object('request_id', q.id, 'memory_id', q.memory_id, 'requester_id', q.requester_id, "           \
    "'created_at', " USAPP_TS("q.created_at") ", 'status', COALESCE(d.outcome, 'pending'), "                     \
    "'decided_by', d.decided_by, 'decided_at', " USAPP_TS("d.decided_at") ")::text AS request_json "             \
    "FROM memory_delete_requests q LEFT JOIN memory_delete_decisions d ON d.request_id = q.id "
const char *kDeleteRequestFactsSql = "SELECT " USAPP_DELETE_REQUEST_JSON "WHERE q.id = $1::uuid";
// Both members, while the memory is visible; otherwise the same answer as a request of an absent memory.
const char *kDeleteRequestReadSql =
    "SELECT " USAPP_DELETE_REQUEST_JSON
    "JOIN couple_members c ON c.couple_id = q.couple_id AND c.user_id = $1::uuid "
    "WHERE q.memory_id = $2::uuid AND q.id = $3::uuid AND NOT usapp_memory_deleted_now(q.memory_id)";
#undef USAPP_DELETE_REQUEST_JSON
const char *kDeleteRequestTargetSql =
    "SELECT c.couple_id::text AS couple_id, m.id::text AS memory_id, m.creator_id::text AS creator_id, "
    "q.id::text AS request_id, q.requester_id::text AS requester_id FROM couple_members c "
    "LEFT JOIN memories m ON m.couple_id = c.couple_id AND m.id = $2::uuid "
    "LEFT JOIN memory_delete_requests q ON q.memory_id = m.id AND q.id = $3::uuid "
    "WHERE c.user_id = $1::uuid FOR SHARE OF c";
const char *kDeleteRequestByKeySql =
    "SELECT q.id::text AS id, q.request_fingerprint, '' AS deletion_id FROM memory_delete_requests q WHERE q.requester_id = $1::uuid AND q.idempotency_key = $2";
const char *kDeleteDecisionByKeySql =
    "SELECT d.request_id::text AS id, d.request_fingerprint, COALESCE(d.deletion_id::text, '') AS deletion_id "
    "FROM memory_delete_decisions d WHERE d.decided_by = $1::uuid AND d.idempotency_key = $2";
const char *kDeleteRequestStateSql =
    "SELECT usapp_memory_deleted_now($1::uuid) AS deleted, usapp_memory_pending_delete_request($1::uuid) IS NOT NULL OR EXISTS ("
    "SELECT 1 FROM memory_edit_proposals p WHERE p.memory_id = $1::uuid "
    "AND NOT EXISTS (SELECT 1 FROM memory_edit_decisions x WHERE x.proposal_id = p.id)) AS has_pending";
const char *kDeleteDecisionStateSql =
    "SELECT usapp_memory_deleted_now($1::uuid) AS deleted, EXISTS (SELECT 1 FROM memory_delete_decisions d WHERE d.request_id = $2::uuid) AS decided";
const char *kDeleteRequestInsertSql =
    "INSERT INTO memory_delete_requests (id, memory_id, couple_id, requester_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING id::text AS id";
// An approval names its cycle before the cycle exists: a cycle cannot start while the request is pending. The key is deferred.
const char *kDeleteDecisionInsertSql =
    "INSERT INTO memory_delete_decisions (request_id, memory_id, couple_id, decided_by, outcome, deletion_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, NULLIF($6::text, '')::uuid, $7::text, $8::text) "
    "ON CONFLICT DO NOTHING RETURNING request_id::text AS id";

#undef USAPP_LIST_SQL
#undef USAPP_MEMORY_FROM
#undef USAPP_MEMORY_JSON
#undef USAPP_PHOTOS
#undef USAPP_DATE
#undef USAPP_TS

using Outcome = MemoryService::Outcome;
using Terms = MemoryService::Terms;
using Decision = MemoryService::Decision;
using Status = MemoryService::Status;

enum class Op { Create, Edit, Propose, Decide, Delete, Undo, RequestDelete, DecideDelete };

std::string newUuid() {
    std::string hex = crypto::randomHex(16);
    hex[12] = '4';
    const char variant[] = {'8', '9', 'a', 'b'};
    hex[16] = variant[static_cast<unsigned char>(hex[16]) % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

bool parseObject(const std::string &text, Json::Value &out) {
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errors) && out.isObject();
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::string jsonIds(const std::vector<std::string> &ids) {
    Json::Value array(Json::arrayValue);
    for (const auto &id : ids) array.append(id);
    return compactJson(array);
}

std::string field(const char *name, const std::string &value) {
    return std::string(name) + "=" + std::to_string(value.size()) + ":" + value + ";";
}

std::string termsCanonical(const Terms &t) {
    std::string out = field("date", t.date) + field("title", t.title) + field("description", t.description);
    for (const auto &id : t.mediaIds) out += field("media", id);
    return out;
}

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool onlyFields(const Json::Value &body, std::initializer_list<const char *> required,
                std::initializer_list<const char *> optional, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是 JSON 对象";
        return false;
    }
    std::set<std::string> allowed;
    for (const char *name : required) allowed.insert(name);
    for (const char *name : optional) allowed.insert(name);
    for (const auto &name : body.getMemberNames()) {
        if (!allowed.count(name)) {
            message = "请求体包含不允许的字段";
            return false;
        }
    }
    for (const char *name : required) {
        if (!body.isMember(name)) {
            message = std::string("缺少字段 ") + name;
            return false;
        }
    }
    return true;
}

bool parseTerms(const Json::Value &body, Terms &out, std::string &message) {
    if (!body["date"].isString()) {
        message = "日期必须是 YYYY-MM-DD 字符串";
        return false;
    }
    const auto date = AnniversaryCalendar::parse(body["date"].asString());
    if (!date || date->year < 1900) {
        message = "日期必须是 1900-01-01 之后的有效日期";
        return false;
    }
    out.date = AnniversaryCalendar::format(*date);
    if (!body["title"].isString()) {
        message = "标题必须是字符串";
        return false;
    }
    switch (TaskService::normalizeTitle(body["title"].asString(), out.title, MemoryService::kMaxTitleCodePoints)) {
        case TaskService::TextError::None: break;
        case TaskService::TextError::InvalidUtf8: message = "标题包含无法识别的字符"; return false;
        case TaskService::TextError::ControlCharacter: message = "标题只能是一行，且不能包含控制字符"; return false;
        case TaskService::TextError::Empty: message = "标题不能为空"; return false;
        case TaskService::TextError::TooLong: message = "标题最多 60 个字符"; return false;
    }
    out.description.clear();
    if (body.isMember("description")) {
        if (!body["description"].isString()) {
            message = "描述必须是字符串";
            return false;
        }
        switch (TaskService::normalizeDescription(body["description"].asString(), out.description, MemoryService::kMaxDescriptionCodePoints)) {
            case TaskService::TextError::None:
            case TaskService::TextError::Empty:
                break;
            case TaskService::TextError::InvalidUtf8: message = "描述包含无法识别的字符"; return false;
            case TaskService::TextError::ControlCharacter: message = "描述不能包含控制字符"; return false;
            case TaskService::TextError::TooLong: message = "描述最多 2000 个字符"; return false;
        }
    }
    const auto &photos = body["media_ids"];
    if (!photos.isArray() || photos.empty() || photos.size() > MemoryService::kMaxPhotos) {
        message = "每条回忆需要 1 到 9 张照片";
        return false;
    }
    std::set<std::string> seen;
    out.mediaIds.clear();
    for (const auto &v : photos) {
        const std::string id = v.isString() ? lower(v.asString()) : std::string();
        if (!TaskService::isCanonicalUuid(id) || !seen.insert(id).second) {
            message = "media_ids 必须是不重复的 uuid";
            return false;
        }
        out.mediaIds.push_back(id);
    }
    return true;
}

// ---------------------------------------------------------------- the write context
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    Op op = Op::Create;
    std::string userId, coupleId, memoryId, creatorId, proposalId, proposerId, key, fingerprint, textId, deletionId;
    int cascaded = 0;  // photo cycles written or undone by this transaction
    Terms terms;
    int expectedVersion = 0;
    Decision decision = Decision::Approve;
    MemoryService::Fault fault = MemoryService::Fault::None;
    std::string ref, ref2;
    std::string planId, planKey;
    intent::Choice choice;  // D20: a direct create only
    Status result;
    MemoryService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
    std::optional<deletionjournal::Operation> journalOperation;
};
using CtxPtr = std::shared_ptr<Ctx>;

Status makeStatus(Outcome outcome, const char *message = "") {
    Status status;
    status.outcome = outcome;
    status.message = message;
    return status;
}

const char *opName(Op op) {
    switch (op) {
        case Op::Create: return "memory_create";
        case Op::Edit: return "memory_edit";
        case Op::Propose: return "memory_propose";
        case Op::Delete: return "memory_delete";
        case Op::Undo: return "memory_undo";
        case Op::RequestDelete: return "memory_delete_request";
        case Op::DecideDelete: return "memory_delete_decide";
        case Op::Decide: break;
    }
    return "memory_decide";
}

void complete(const CtxPtr &ctx, Status status) {
    if (ctx->completed) return;
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = TaskService::failureDetail(opName(ctx->op), stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void reject(const CtxPtr &ctx, Outcome outcome, const char *message) {
    auto status = std::make_shared<Status>(makeStatus(outcome, message));
    ctx->scope->rollback([ctx, status]() { complete(ctx, std::move(*status)); });
}

// A statement whose only failures are unexpected.
template <typename... Args>
void step(const CtxPtr &ctx, const char *sql, const char *stage, std::function<void(const drogon::orm::Result &)> next, Args &&...args) {
    ctx->scope->tx().execSqlAsync(
        sql,
        [ctx, stage, next = std::move(next)](const drogon::orm::Result &rows) {
            try {
                next(rows);
            } catch (const std::exception &) {
                failAt(ctx, stage, "column_format");
            }
        },
        [ctx, stage](const drogon::orm::DrogonDbException &) { failAt(ctx, stage, "db_error"); }, std::forward<Args>(args)...);
}

void readFacts(const CtxPtr &ctx, Outcome outcome, std::function<void(Status)> next) {
    auto onRows = [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
        if (rows.size() != 1u) { failAt(ctx, "read_mutation", "row_count"); return; }
        auto status = std::make_shared<Status>(makeStatus(outcome));
        std::string memoryId;
        try {
            if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) { failAt(ctx, "read_mutation", "json_format"); return; }
            memoryId = rows[0]["memory_id"].as<std::string>();
        } catch (const std::exception &) { failAt(ctx, "read_mutation", "column_format"); return; }
        step(ctx, kCurrentInTxSql, "read_current",
             [ctx, status, next](const drogon::orm::Result &current) {
                 // D6: a replay reads today's memory, and a deleted one is absent. A new write cannot get here: its state check is under this lock.
                 if (current.size() == 0u) {
                     reject(ctx, Outcome::NotFound, "回忆不存在");
                     return;
                 }
                 if (current.size() != 1u || !parseObject(current[0]["memory_json"].as<std::string>(), status->memory)) {
                     failAt(ctx, "read_current", "row_count");
                     return;
                 }
                 next(std::move(*status));
             },
             memoryId);
    };
    auto onError = [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_mutation", "db_error"); };
    if (ctx->op == Op::Propose) {
        ctx->scope->tx().execSqlAsync(kProposalFactsSql, std::move(onRows), std::move(onError), ctx->ref);
    } else if (ctx->op == Op::Decide) {
        ctx->scope->tx().execSqlAsync(kDecisionFactsSql, std::move(onRows), std::move(onError), ctx->ref);
    } else {
        ctx->scope->tx().execSqlAsync(kRevisionFactsSql, std::move(onRows), std::move(onError), ctx->ref, ctx->ref2);
    }
}

// Same actor and key: an identical request replays its committed write, read-only, with no new revision or binding. A different
// fingerprint is IDEMPOTENCY_KEY_REUSED. Runs before any state check.
void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    const char *sql = ctx->op == Op::Propose ? kProposalByKeySql : ctx->op == Op::Decide ? kDecisionByKeySql : kRevisionByKeySql;
    step(ctx, sql, "classify",
         [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onMissing(); return; }
             ctx->ref = rows[0]["ref"].as<std::string>();
             ctx->ref2 = rows[0]["ref2"].as<std::string>();
             // D6 M1d (O5): erased. The key cannot be compared or replayed any more, and it says nothing of what it was for.
             if (rows[0]["request_fingerprint"].as<std::string>() == std::string(64, '0')) {
                 reject(ctx, Outcome::NotFound, "回忆不存在");
                 return;
             }
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                 return;
             }
             readFacts(ctx, Outcome::Replayed, [ctx](Status status) {
                 auto shared = std::make_shared<Status>(std::move(status));
                 if (ctx->op != Op::Create || !ctx->planId.empty()) {
                     ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
                     return;
                 }
                 // D20: a replay only reads the original decision (null for a create from before 0043); it never writes one.
                 intent::Source source;
                 source.memoryId = ctx->ref;
                 intent::readOriginal(
                     ctx->scope, intent::Operation::MemoryCreate, source,
                     [ctx, shared](Json::Value facts) {
                         shared->hasNotification = true;
                         shared->notification = std::move(facts);
                         ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
                     },
                     [ctx]() { failAt(ctx, "classify", "intent_read_failed"); });
             });
         },
         ctx->userId, ctx->key);
}

const char *auditAction(const CtxPtr &ctx) {
    switch (ctx->op) {
        case Op::Create: return "memory.created";
        case Op::Edit: return "memory.edited";
        case Op::Propose: return "memory.edit_proposed";
        case Op::Delete: return "memory.deleted";
        case Op::Undo: return "memory.deletion_undone";
        case Op::RequestDelete: return "memory.delete_requested";
        case Op::DecideDelete:
            if (ctx->decision == Decision::Approve) return "memory.delete_request_approved";
            return ctx->decision == Decision::Reject ? "memory.delete_request_rejected" : "memory.delete_request_withdrawn";
        case Op::Decide: break;
    }
    if (ctx->decision == Decision::Approve) return "memory.edit_approved";
    return ctx->decision == Decision::Reject ? "memory.edit_rejected" : "memory.edit_withdrawn";
}

void auditAndCommit(const CtxPtr &ctx, Outcome outcome, Json::Value detail) {
    step(ctx, kAuditSql, "audit",
         [ctx, outcome](const drogon::orm::Result &) {
             auto commitWith = [ctx, outcome](bool hasNotification, Json::Value notification) {
                 readFacts(ctx, outcome, [ctx, hasNotification, notification](Status status) {
                     status.hasNotification = hasNotification;
                     status.notification = notification;
                     ctx->result = std::move(status);
                     ctx->commitRequested = true;
                     ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
                 });
             };
             if (ctx->op != Op::Create || !ctx->planId.empty()) {
                 commitWith(false, Json::Value());
                 return;
             }
             // D20: the intent (and, when requested with a partner, the one notice) in this same transaction.
             intent::Source source;
             source.memoryId = ctx->memoryId;
             intent::record(
                 ctx->scope, intent::Operation::MemoryCreate, source, ctx->choice,
                 [commitWith](Json::Value facts) { commitWith(true, std::move(facts)); },
                 [ctx]() { failAt(ctx, "notification_intent", "write_failed"); });
         },
         ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->memoryId, compactJson(detail));
}

void finishRevision(const CtxPtr &ctx, Json::Value detail) {
    if (ctx->planId.empty()) {
        auditAndCommit(ctx, Outcome::Created, std::move(detail));
        return;
    }
    step(ctx,
         "WITH linked AS (INSERT INTO shared_plan_memory_links (plan_id,couple_id,memory_id,actor_id,mode,request_key) "
         "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,'create',$5) RETURNING *) "
         "INSERT INTO audit_logs (actor_type,actor_id,couple_id,action,target_type,target_id,detail) "
         "SELECT 'user',actor_id,couple_id,'shared_plan.memory_linked','shared_plan',plan_id::text,"
         "jsonb_build_object('memory_id',memory_id) FROM linked",
         "link_plan", [ctx, detail = std::move(detail)](const drogon::orm::Result &) {
             if (ctx->fault == MemoryService::Fault::FailPlanLink) {
                 failAt(ctx, "link_plan", "injected_failure");
                 return;
             }
             auditAndCommit(ctx, Outcome::Created, detail);
         }, ctx->planId, ctx->coupleId, ctx->memoryId, ctx->userId, ctx->planKey);
}

// Locks in sorted order, then decides on what those waits let it see.
void withEligiblePhotos(const CtxPtr &ctx, std::function<void()> next) {
    const std::string ids = jsonIds(ctx->terms.mediaIds);
    step(ctx, kPhotoLockSql, "photo_lock",
         [ctx, ids, next = std::move(next)](const drogon::orm::Result &) {
             step(ctx, kPhotoCheckSql, "photo_check",
                  [ctx, next](const drogon::orm::Result &rows) {
                      if (rows.size() != 1u) { failAt(ctx, "photo_check", "row_count"); return; }
                      if (rows[0]["eligible"].as<size_t>() != ctx->terms.mediaIds.size()) {
                          // A concurrent create with this key may be what took the photos: its replay wins over the refusal.
                          classify(ctx, [ctx]() { reject(ctx, Outcome::PhotoUnavailable, "有照片已不可用，请重新选择"); });
                          return;
                      }
                      next();
                  },
                  ids, ctx->coupleId, ctx->memoryId, ctx->userId);
         },
         ids);
}

// ---- create and edit: text, then the revision, bindings, photos.
void writeRevision(const CtxPtr &ctx, int version, const char *action) {
    step(ctx, kTextInsertSql, "insert_text",
         [ctx, version, action](const drogon::orm::Result &texts) {
             ctx->textId = texts[0]["id"].as<std::string>();
             step(ctx, kRevisionInsertSql, "insert_revision",
                  [ctx, version](const drogon::orm::Result &rows) {
                      if (rows.size() == 0u) {
                          // A concurrent same-key write committed first; this transaction's rows roll back.
                          classify(ctx, [ctx]() { failAt(ctx, "insert_revision", "conflict_unexplained"); });
                          return;
                      }
                      const std::string ids = jsonIds(ctx->terms.mediaIds);
                      step(ctx, kBindSql, "bind",
                           [ctx, version, ids](const drogon::orm::Result &) {
                               step(ctx, kRevisionPhotosSql, "insert_photos",
                                    [ctx, version](const drogon::orm::Result &) {
                                        ctx->ref = ctx->memoryId;
                                        ctx->ref2 = std::to_string(version);
                                        Json::Value detail(Json::objectValue);
                                        detail["memory_id"] = ctx->memoryId;
                                        detail["version"] = version;
                                        detail["photos"] = static_cast<Json::UInt>(ctx->terms.mediaIds.size());
                                        finishRevision(ctx, std::move(detail));
                                    },
                                    ctx->memoryId, std::to_string(version), ctx->coupleId, ids);
                           },
                           ids, ctx->coupleId, ctx->memoryId, ctx->userId);
                  },
                  ctx->memoryId, ctx->coupleId, std::to_string(version), ctx->userId, std::string(action), ctx->terms.date,
                  ctx->textId, ctx->key, ctx->fingerprint);
         },
         ctx->coupleId, ctx->terms.title, ctx->terms.description);
}

void rejectFutureThen(const CtxPtr &ctx, std::function<void()> next) {
    step(ctx, kTodaySql, "date",
         [ctx, next = std::move(next)](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "date", "row_count"); return; }
             if (rows[0]["in_future"].as<bool>()) {
                 reject(ctx, Outcome::Validation, "回忆的日期不能晚于今天");
                 return;
             }
             next();
         },
         ctx->terms.date);
}

void createStart(const CtxPtr &ctx) {
    step(ctx, kMembershipSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             auto create = [ctx]() {
                 rejectFutureThen(ctx, [ctx]() {
                     // A brand new memory has no photos of its own yet: the check's memory id is empty.
                     withEligiblePhotos(ctx, [ctx]() {
                         ctx->memoryId = newUuid();
                         step(ctx, kIdentityInsertSql, "insert_identity",
                              [ctx](const drogon::orm::Result &) { writeRevision(ctx, 1, "create"); },
                              ctx->memoryId, ctx->coupleId, ctx->userId);
                     });
                 });
             };
             if (ctx->planId.empty()) {
                 classify(ctx, std::move(create));
                 return;
             }
             step(ctx, "SELECT usapp_plan_memory_preflight($1::uuid,$2::uuid,'create',$3) AS outcome",
                  "plan_preflight", [ctx, create = std::move(create)](const drogon::orm::Result &rows) {
                      if (rows.size() != 1u) { failAt(ctx, "plan_preflight", "row_count"); return; }
                      const auto outcome = rows[0]["outcome"].as<std::string>();
                      if (outcome == "new") classify(ctx, create);
                      else if (outcome == "replay") classify(ctx, [ctx]() { failAt(ctx, "plan_preflight", "missing_receipt"); });
                      else if (outcome == "not_found") reject(ctx, Outcome::PlanNotFound, "计划不存在");
                      else if (outcome == "key_reused") reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                      else if (outcome == "conflict") reject(ctx, Outcome::PlanConflict, "计划未完成或已关联回忆");
                      else failAt(ctx, "plan_preflight", "invalid_outcome");
                  }, ctx->userId, ctx->planId, ctx->planKey);
         },
         ctx->userId);
}

// ---- edit and propose
bool sameAsBase(const Terms &t, const drogon::orm::Row &row) {
    if (t.date != row["memory_date"].as<std::string>() || t.title != row["title"].as<std::string>() ||
        t.description != row["description"].as<std::string>()) {
        return false;
    }
    Json::CharReaderBuilder builder;
    std::string errors;
    Json::Value photos;
    const std::string text = row["photos"].as<std::string>();
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &photos, &errors) || !photos.isArray() || photos.size() != t.mediaIds.size()) {
        return false;
    }
    for (Json::ArrayIndex i = 0; i < photos.size(); ++i) {
        if (photos[i].asString() != t.mediaIds[i]) return false;
    }
    return true;
}

void writeProposal(const CtxPtr &ctx) {
    step(ctx, kTextInsertSql, "insert_text",
         [ctx](const drogon::orm::Result &texts) {
             ctx->textId = texts[0]["id"].as<std::string>();
             ctx->proposalId = newUuid();
             step(ctx, kProposalInsertSql, "insert_proposal",
                  [ctx](const drogon::orm::Result &rows) {
                      if (rows.size() == 0u) {
                          classify(ctx, [ctx]() { failAt(ctx, "insert_proposal", "conflict_unexplained"); });
                          return;
                      }
                      step(ctx, kProposalPhotosSql, "insert_photos",
                           [ctx](const drogon::orm::Result &) {
                               ctx->ref = ctx->proposalId;
                               ctx->ref2.clear();
                               Json::Value detail(Json::objectValue);
                               detail["memory_id"] = ctx->memoryId;
                               detail["proposal_id"] = ctx->proposalId;
                               detail["base_version"] = ctx->expectedVersion;
                               detail["photos"] = static_cast<Json::UInt>(ctx->terms.mediaIds.size());
                               auditAndCommit(ctx, Outcome::Created, detail);
                           },
                           ctx->proposalId, ctx->coupleId, jsonIds(ctx->terms.mediaIds));
                  },
                  ctx->proposalId, ctx->coupleId, ctx->memoryId, ctx->userId, std::to_string(ctx->expectedVersion), ctx->terms.date,
                  ctx->textId, ctx->key, ctx->fingerprint);
         },
         ctx->coupleId, ctx->terms.title, ctx->terms.description);
}

void revisionChecks(const CtxPtr &ctx) {
    step(ctx, kStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             // D6: read after 33020, so a deletion is entirely before this write or entirely after it.
             if (rows[0]["deleted"].as<bool>()) {
                 reject(ctx, Outcome::NotFound, "回忆不存在");
                 return;
             }
             const bool isCreator = ctx->creatorId == ctx->userId;
             if (ctx->op == Op::Edit && !isCreator) {
                 reject(ctx, Outcome::Forbidden, "只有创建者可以直接修改，你可以提交修改申请");
                 return;
             }
             if (ctx->op == Op::Propose && isCreator) {
                 reject(ctx, Outcome::Forbidden, "创建者请直接修改");
                 return;
             }
             if (ctx->expectedVersion != rows[0]["version"].as<int>()) {
                 reject(ctx, Outcome::VersionConflict, "内容已经更新，请刷新后再修改");
                 return;
             }
             if (rows[0]["has_pending"].as<bool>()) {
                 reject(ctx, Outcome::ProposalPending, ctx->op == Op::Edit ? "有待处理的修改申请，请先同意或拒绝" : "已有待处理的修改申请");
                 return;
             }
             if (rows[0]["has_delete_request"].as<bool>()) {
                 reject(ctx, Outcome::ProposalPending, ctx->op == Op::Edit ? "有待处理的删除申请，请先同意或拒绝" : "已有待处理的删除申请");
                 return;
             }
             if (sameAsBase(ctx->terms, rows[0])) {
                 reject(ctx, Outcome::Validation, "内容没有变化");
                 return;
             }
             rejectFutureThen(ctx, [ctx]() {
                 withEligiblePhotos(ctx, [ctx]() {
                     if (ctx->op == Op::Edit) writeRevision(ctx, ctx->expectedVersion + 1, "edit");
                     else writeProposal(ctx);
                 });
             });
         },
         ctx->memoryId);
}

void revisionTarget(const CtxPtr &ctx) {
    const std::string requested = ctx->memoryId;
    step(ctx, kTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["memory_id"].isNull()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->memoryId = rows[0]["memory_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->fingerprint = ctx->op == Op::Edit ? MemoryService::editFingerprint(ctx->memoryId, ctx->expectedVersion, ctx->terms)
                                                    : MemoryService::proposeFingerprint(ctx->memoryId, ctx->expectedVersion, ctx->terms);
             step(ctx, kMemoryLockSql, "lock",
                  [ctx](const drogon::orm::Result &) { classify(ctx, [ctx]() { revisionChecks(ctx); }); }, ctx->memoryId);
         },
         ctx->userId, requested);
}

// ---- decide. Approval, its revision and every binding commit together, or none of them do.
void finishDecision(const CtxPtr &ctx, int version) {
    Json::Value detail(Json::objectValue);
    detail["memory_id"] = ctx->memoryId;
    detail["proposal_id"] = ctx->proposalId;
    detail["outcome"] = ctx->decision == Decision::Approve ? "approved" : ctx->decision == Decision::Reject ? "rejected" : "withdrawn";
    detail["version"] = version > 0 ? Json::Value(version) : Json::Value(Json::nullValue);
    auditAndCommit(ctx, Outcome::Ok, detail);
}

void approveRevision(const CtxPtr &ctx) {
    step(ctx, kApproveRevisionSql, "insert_revision",
         [ctx](const drogon::orm::Result &revisions) {
             if (revisions.size() != 1u) { failAt(ctx, "insert_revision", "row_count"); return; }
             const int version = revisions[0]["version"].as<int>();
             if (ctx->fault == MemoryService::Fault::FailPhotoBinding) {
                 failAt(ctx, "bind", "forced_fault");
                 return;
             }
             step(ctx, kApproveBindSql, "bind",
                  [ctx, version](const drogon::orm::Result &) {
                      step(ctx, kApprovePhotosSql, "insert_photos",
                           [ctx, version](const drogon::orm::Result &) { finishDecision(ctx, version); },
                           ctx->proposalId, std::to_string(version));
                  },
                  ctx->proposalId, ctx->userId);
         },
         ctx->proposalId, ctx->userId);
}

void decisionInsert(const CtxPtr &ctx) {
    const std::string outcome = ctx->decision == Decision::Approve ? "approved" : ctx->decision == Decision::Reject ? "rejected" : "withdrawn";
    step(ctx, kDecisionInsertSql, "insert_decision",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) {
                 classify(ctx, [ctx]() { reject(ctx, Outcome::ProposalDecided, "这条修改申请已经处理过了"); });
                 return;
             }
             ctx->ref = ctx->proposalId;
             ctx->ref2.clear();
             if (ctx->decision == Decision::Approve) approveRevision(ctx);
             else finishDecision(ctx, 0);  // Deciding is what releases the proposal's reservations.
         },
         ctx->proposalId, ctx->coupleId, ctx->userId, outcome, ctx->key, ctx->fingerprint);
}

void decisionChecks(const CtxPtr &ctx) {
    step(ctx, kDecisionStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             if (rows[0]["deleted"].as<bool>()) {
                 reject(ctx, Outcome::NotFound, "回忆不存在");
                 return;
             }
             if (ctx->decision == Decision::Withdraw ? ctx->userId != ctx->proposerId : ctx->userId != ctx->creatorId) {
                 reject(ctx, Outcome::Forbidden, ctx->decision == Decision::Withdraw ? "只有提出申请的一方可以撤回" : "只有创建者可以处理修改申请");
                 return;
             }
             if (rows[0]["decided"].as<bool>()) { reject(ctx, Outcome::ProposalDecided, "这条修改申请已经处理过了"); return; }
             if (ctx->decision != Decision::Approve) { decisionInsert(ctx); return; }
             if (rows[0]["base_version"].as<int>() != rows[0]["current_version"].as<int>()) {
                 reject(ctx, Outcome::ProposalStale, "内容已经更新，这条修改申请不能再同意");
                 return;
             }
             const std::string ids = rows[0]["photos"].as<std::string>();
             step(ctx, kPhotoLockSql, "photo_lock",
                  [ctx](const drogon::orm::Result &) {
                      step(ctx, kApprovalPhotoCheckSql, "photo_check",
                           [ctx](const drogon::orm::Result &counts) {
                               if (counts.size() != 1u) { failAt(ctx, "photo_check", "row_count"); return; }
                               if (counts[0]["eligible"].as<int>() != counts[0]["total"].as<int>()) {
                                   reject(ctx, Outcome::PhotoUnavailable, "有照片已不可用，这条修改申请不能再同意");
                                   return;
                               }
                               decisionInsert(ctx);
                           },
                           ctx->proposalId);
                  },
                  ids);
         },
         ctx->proposalId);
}

void decisionTarget(const CtxPtr &ctx) {
    const std::string requested = ctx->memoryId, proposal = ctx->proposalId;
    step(ctx, kDecisionTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["memory_id"].isNull()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             if (rows[0]["proposal_id"].isNull()) { reject(ctx, Outcome::ProposalNotFound, "修改申请不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->memoryId = rows[0]["memory_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->proposalId = rows[0]["proposal_id"].as<std::string>();
             ctx->proposerId = rows[0]["proposer_id"].as<std::string>();
             ctx->fingerprint = MemoryService::decisionFingerprint(ctx->memoryId, ctx->proposalId, ctx->decision);
             step(ctx, kMemoryLockSql, "lock",
                  [ctx](const drogon::orm::Result &) { classify(ctx, [ctx]() { decisionChecks(ctx); }); }, ctx->memoryId);
         },
         ctx->userId, requested, proposal);
}

// ---- D6 M1a: deletion and undo. Member and target, 33020, classification, then state: the same order as every memory write.
void readDeletionFacts(const CtxPtr &ctx, const std::string &deletionId, Outcome outcome, std::function<void(Status)> next) {
    step(ctx, kDeletionFactsSql, "read_deletion",
         [ctx, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "read_deletion", "row_count"); return; }
             Status status = makeStatus(outcome);
             if (!parseObject(rows[0]["deletion_json"].as<std::string>(), status.deletion)) { failAt(ctx, "read_deletion", "json_format"); return; }
             next(std::move(status));
         },
         deletionId);
}

// A replay answers about its own cycle before any state is looked at, so it never sees a state error and never reads the memory.
void classifyDeletion(const CtxPtr &ctx, std::function<void()> onMissing) {
    step(ctx, ctx->op == Op::Undo ? kUndoByKeySql : kDeletionByKeySql, "classify",
         [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onMissing(); return; }
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                 return;
             }
             readDeletionFacts(ctx, rows[0]["id"].as<std::string>(), Outcome::Replayed, [ctx](Status status) {
                 auto shared = std::make_shared<Status>(std::move(status));
                 ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
             });
         },
         ctx->userId, ctx->key);
}

void finishDeletion(const CtxPtr &ctx, const std::string &deletionId, Outcome outcome) {
    Json::Value detail(Json::objectValue);
    detail["memory_id"] = ctx->memoryId;
    detail["deletion_id"] = deletionId;
    detail["photos"] = ctx->cascaded;
    step(ctx, kAuditSql, "audit",
         [ctx, deletionId, outcome](const drogon::orm::Result &) {
             readDeletionFacts(ctx, deletionId, outcome, [ctx](Status status) {
                 ctx->result = std::move(status);
                 ctx->commitRequested = true;
                 ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
             });
         },
         ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->memoryId, compactJson(detail));
}

Json::Value journalPhotoJson(const deletionjournal::Operation &operation) {
    Json::Value photos(Json::arrayValue);
    for (const auto &subject : operation.subjects) if (subject.kind == deletionjournal::Kind::Media) {
        Json::Value photo(Json::objectValue);
        photo["media_id"] = subject.targetId; photo["cycle_id"] = subject.cycleId;
        photos.append(std::move(photo));
    }
    return photos;
}

bool journalPhotosMatch(const drogon::orm::Result &rows, const deletionjournal::Operation &operation, bool cycles) {
    std::vector<std::pair<std::string, std::string>> expected, actual;
    for (const auto &subject : operation.subjects) if (subject.kind == deletionjournal::Kind::Media)
        expected.emplace_back(subject.targetId, cycles ? subject.cycleId : "");
    for (const auto &row : rows) actual.emplace_back(row["media_id"].as<std::string>(), cycles ? row["cycle_id"].as<std::string>() : "");
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    return expected == actual;
}

void writeCycleRows(const CtxPtr &ctx, std::function<void()> onConflict, std::function<void()> onWritten) {
    step(ctx, kDeletionInsertSql, "insert_deletion",
         [ctx, onConflict, onWritten](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onConflict(); return; }
             auto made = [ctx, onWritten](const drogon::orm::Result &result) {
                 if (result.size() != 1u) { failAt(ctx, "cascade", "row_count"); return; }
                 ctx->cascaded = result[0]["n"].as<int>();
                 onWritten();
             };
             if (ctx->journalOperation) {
                 const auto photos = journalPhotoJson(*ctx->journalOperation);
                 const auto text = compactJson(photos);
                 step(ctx, kJournalCascadeSql, "cascade",
                      [ctx, made, text, expected = photos.size()](const drogon::orm::Result &rows) {
                          if (rows.size() != expected) { failAt(ctx, "cascade", "row_count"); return; }
                          step(ctx, kJournalLinkSql, "cascade_links", made, ctx->deletionId, text);
                      }, ctx->deletionId, text);
             }
             else step(ctx, kCascadeSql, "cascade", made, ctx->deletionId);
         },
         ctx->memoryId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint, ctx->deletionId);
}

// Re-evaluate the exact target set AFTER sorted photo locks, BEFORE the cycle.
// Any prior decision/edit settlement in this transaction rolls back on mismatch.
void writeCycle(const CtxPtr &ctx, std::function<void()> onConflict, std::function<void()> onWritten) {
    step(ctx, kCascadeLockSql, "cascade_lock",
         [ctx, onConflict, onWritten](const drogon::orm::Result &) {
             if (!ctx->journalOperation) { writeCycleRows(ctx, onConflict, onWritten); return; }
             step(ctx, kJournalCandidatesSql, "journal_revalidate",
                  [ctx, onConflict, onWritten](const drogon::orm::Result &rows) {
                      if (!journalPhotosMatch(rows, *ctx->journalOperation, false)) {
                          reject(ctx, Outcome::ResultUnknown, "照片清单已变化，本次未确认，请保留原请求"); return;
                      }
                      writeCycleRows(ctx, onConflict, onWritten);
                  }, ctx->memoryId, ctx->creatorId);
         }, ctx->memoryId, ctx->creatorId);
}

void insertDeletion(const CtxPtr &ctx) {
    writeCycle(ctx,
               // A concurrent write with this key committed first, on another memory; this transaction's rows roll back.
               [ctx]() { classifyDeletion(ctx, [ctx]() { failAt(ctx, "insert_deletion", "conflict_unexplained"); }); },
               [ctx]() {
                   if (ctx->fault == MemoryService::Fault::FailAfterCascade) { failAt(ctx, "cascade", "forced_fault"); return; }
                   finishDeletion(ctx, ctx->deletionId, Outcome::Created);
               });
}

// A pending delete request is what this deletion asked for: it is approved, naming this cycle, in the same transaction.
void settleDeleteRequest(const CtxPtr &ctx, const std::string &request) {
    step(ctx, kDeleteDecisionInsertSql, "settle_request",
         [ctx, request](const drogon::orm::Result &decided) {
             if (decided.size() != 1u) { failAt(ctx, "settle_request", "row_count"); return; }
             Json::Value detail(Json::objectValue);
             detail["memory_id"] = ctx->memoryId;
             detail["request_id"] = request;
             detail["deletion_id"] = ctx->deletionId;
             detail["outcome"] = "approved";
             detail["by_deletion"] = true;
             step(ctx, kAuditSql, "audit",
                  [ctx](const drogon::orm::Result &) { insertDeletion(ctx); },
                  ctx->userId, ctx->coupleId, std::string("memory.delete_request_approved"), ctx->memoryId, compactJson(detail));
         },
         request, ctx->memoryId, ctx->coupleId, ctx->userId, std::string("approved"), ctx->deletionId, std::string("deletion-settle-") + newUuid(),
         MemoryService::deleteDecisionFingerprint(ctx->memoryId, request, Decision::Approve));
}

void deleteChecks(const CtxPtr &ctx) {
    step(ctx, kDeleteStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             const bool isCreator = ctx->creatorId == ctx->userId;
             if (ctx->deletionId.empty()) ctx->deletionId = newUuid();
             if (rows[0]["deleted"].as<bool>()) {
                 // To the partner it is simply absent.
                 if (isCreator) reject(ctx, Outcome::AlreadyDeleted, "这条回忆已经删除了");
                 else reject(ctx, Outcome::NotFound, "回忆不存在");
                 return;
             }
             if (!isCreator) { reject(ctx, Outcome::Forbidden, "只有创建者可以删除这条回忆"); return; }
             if (!rows[0]["pending_delete_request"].isNull()) { settleDeleteRequest(ctx, rows[0]["pending_delete_request"].as<std::string>()); return; }
             if (rows[0]["pending_proposal"].isNull()) { insertDeletion(ctx); return; }
             // Decided here, in this transaction: its reservations are released at commit, and an undo does not bring it back.
             const std::string proposal = rows[0]["pending_proposal"].as<std::string>();
             step(ctx, kDecisionInsertSql, "reject_pending",
                  [ctx, proposal](const drogon::orm::Result &decided) {
                      if (decided.size() != 1u) { failAt(ctx, "reject_pending", "row_count"); return; }
                      Json::Value detail(Json::objectValue);
                      detail["memory_id"] = ctx->memoryId;
                      detail["proposal_id"] = proposal;
                      detail["outcome"] = "rejected";
                      detail["by_deletion"] = true;
                      step(ctx, kAuditSql, "audit",
                           [ctx](const drogon::orm::Result &) { insertDeletion(ctx); },
                           ctx->userId, ctx->coupleId, std::string("memory.edit_rejected"), ctx->memoryId, compactJson(detail));
                  },
                  proposal, ctx->coupleId, ctx->userId, std::string("rejected"), std::string("deletion-reject-") + newUuid(),
                  MemoryService::decisionFingerprint(ctx->memoryId, proposal, Decision::Reject));
         },
         ctx->memoryId);
}

// The clock keeps moving after the prechecks, and each guard reads it again. A refusal from any of them is the same answer, and it fails the whole
// undo: the memory reversal and every linked reversal commit together or not at all.
void refuseOrFailUndo(const CtxPtr &ctx, const drogon::orm::DrogonDbException &failure, const char *stage) {
    const std::string text = failure.base().what();
    if (text.find("USAPP_MEMORY_REVERSAL_WINDOW") != std::string::npos || text.find("USAPP_MEMORY_REVERSAL_PURGED") != std::string::npos ||
        text.find("USAPP_MEDIA_REVERSAL_WINDOW") != std::string::npos ||
        text.find("USAPP_MEDIA_REVERSAL_PURGE_PLANNED") != std::string::npos) {
        reject(ctx, Outcome::UndoClosed, "已超过可撤销的时间");
        return;
    }
    failAt(ctx, stage, "db_error");
}

void insertReversalRows(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kReversalInsertSql,
        [ctx](const drogon::orm::Result &inserted) {
            try {
                if (inserted.size() == 0u) {
                    classifyDeletion(ctx, [ctx]() { failAt(ctx, "insert_reversal", "conflict_unexplained"); });
                    return;
                }
                ctx->scope->tx().execSqlAsync(
                    kCascadeUndoSql,
                    [ctx](const drogon::orm::Result &undone) {
                        try {
                            if (undone.size() != 1u) { failAt(ctx, "cascade_undo", "row_count"); return; }
                            ctx->cascaded = undone[0]["n"].as<int>();
                            finishDeletion(ctx, ctx->deletionId, Outcome::Ok);
                        } catch (const std::exception &) {
                            failAt(ctx, "cascade_undo", "column_format");
                        }
                    },
                    [ctx](const drogon::orm::DrogonDbException &failure) { refuseOrFailUndo(ctx, failure, "cascade_undo"); },
                    ctx->deletionId);
            } catch (const std::exception &) {
                failAt(ctx, "insert_reversal", "column_format");
            }
        },
        [ctx](const drogon::orm::DrogonDbException &failure) { refuseOrFailUndo(ctx, failure, "insert_reversal"); },
        ctx->deletionId, ctx->memoryId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
}

void insertReversal(const CtxPtr &ctx) {
    if (!ctx->journalOperation) { insertReversalRows(ctx); return; }
    step(ctx, kJournalLinksSql, "journal_undo_revalidate",
         [ctx](const drogon::orm::Result &rows) {
             if (!journalPhotosMatch(rows, *ctx->journalOperation, true)) {
                 reject(ctx, Outcome::ResultUnknown, "关联照片清单未确认，请保留原请求"); return;
             }
             insertReversalRows(ctx);
         }, ctx->deletionId);
}

void undoChecks(const CtxPtr &ctx) {
    step(ctx, kUndoStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u || rows[0]["deleted_by"].as<std::string>() != ctx->userId) {
                 reject(ctx, Outcome::NotFound, "回忆不存在");
                 return;
             }
             if (rows[0]["reversed"].as<bool>()) { reject(ctx, Outcome::DeletionNotActive, "这次删除已经撤销了"); return; }
             if (ctx->fault != MemoryService::Fault::IgnoreUndoPrecheck && !rows[0]["open"].as<bool>()) {
                 reject(ctx, Outcome::UndoClosed, "已超过可撤销的时间");
                 return;
             }
             step(ctx, kLinkedLockSql, "linked_lock",
                  [ctx](const drogon::orm::Result &) {
                      step(ctx, kLinkedStateSql, "linked_state",
                           [ctx](const drogon::orm::Result &planned) {
                               if (planned.size() != 1u) { failAt(ctx, "linked_state", "row_count"); return; }
                               // One photo that can no longer come back keeps the whole memory deleted.
                               if (ctx->fault != MemoryService::Fault::IgnoreUndoPrecheck && planned[0]["n"].as<int>() > 0) {
                                   reject(ctx, Outcome::UndoClosed, "已超过可撤销的时间");
                                   return;
                               }
                               insertReversal(ctx);
                           },
                           ctx->deletionId);
                  },
                  ctx->deletionId);
         },
         ctx->deletionId, ctx->memoryId, ctx->coupleId);
}

void deletionTarget(const CtxPtr &ctx) {
    const std::string requested = ctx->memoryId;
    step(ctx, kTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["memory_id"].isNull()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->memoryId = rows[0]["memory_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->fingerprint = ctx->op == Op::Undo ? MemoryService::undoFingerprint(ctx->memoryId, ctx->deletionId) : MemoryService::deletionFingerprint(ctx->memoryId);
             step(ctx, kMemoryLockSql, "lock",
                  [ctx](const drogon::orm::Result &) {
                      classifyDeletion(ctx, [ctx]() {
                          if (ctx->op == Op::Undo) undoChecks(ctx);
                          else deleteChecks(ctx);
                      });
                  },
                  ctx->memoryId);
         },
         ctx->userId, requested);
}

// ---- D6 M1b: delete requests. Member and target, 33020, classification, then state, as every memory write.
void readRequestFacts(const CtxPtr &ctx, const std::string &requestId, const std::string &deletionId, Outcome outcome, std::function<void(Status)> next) {
    step(ctx, kDeleteRequestFactsSql, "read_request",
         [ctx, deletionId, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "read_request", "row_count"); return; }
             auto status = std::make_shared<Status>(makeStatus(outcome));
             if (!parseObject(rows[0]["request_json"].as<std::string>(), status->request)) { failAt(ctx, "read_request", "json_format"); return; }
             if (deletionId.empty()) { next(std::move(*status)); return; }
             step(ctx, kDeletionFactsSql, "read_deletion",
                  [ctx, status, next](const drogon::orm::Result &cycle) {
                      if (cycle.size() != 1u) { failAt(ctx, "read_deletion", "row_count"); return; }
                      if (!parseObject(cycle[0]["deletion_json"].as<std::string>(), status->deletion)) { failAt(ctx, "read_deletion", "json_format"); return; }
                      next(std::move(*status));
                  },
                  deletionId);
         },
         requestId);
}

void commitRequestFacts(const CtxPtr &ctx, const std::string &requestId, const std::string &deletionId, Outcome outcome) {
    readRequestFacts(ctx, requestId, deletionId, outcome, [ctx](Status status) {
        ctx->result = std::move(status);
        ctx->commitRequested = true;
        ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
    });
}

// A replay answers about its own request, and its own cycle for an approver, before any state is looked at. It never reads the memory.
void classifyDeleteRequest(const CtxPtr &ctx, std::function<void()> onMissing) {
    step(ctx, ctx->op == Op::RequestDelete ? kDeleteRequestByKeySql : kDeleteDecisionByKeySql, "classify",
         [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onMissing(); return; }
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                 return;
             }
             readRequestFacts(ctx, rows[0]["id"].as<std::string>(), rows[0]["deletion_id"].as<std::string>(), Outcome::Replayed, [ctx](Status status) {
                 auto shared = std::make_shared<Status>(std::move(status));
                 ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
             });
         },
         ctx->userId, ctx->key);
}

void requestChecks(const CtxPtr &ctx) {
    step(ctx, kDeleteRequestStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             if (rows[0]["deleted"].as<bool>()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             if (ctx->creatorId == ctx->userId) { reject(ctx, Outcome::Forbidden, "创建者请直接删除"); return; }
             if (rows[0]["has_pending"].as<bool>()) { reject(ctx, Outcome::ProposalPending, "这条回忆已有待处理的申请"); return; }
             ctx->proposalId = newUuid();
             step(ctx, kDeleteRequestInsertSql, "insert_request",
                  [ctx](const drogon::orm::Result &inserted) {
                      if (inserted.size() == 0u) {
                          // A concurrent write with this key committed first, on another memory; this transaction's rows roll back.
                          classifyDeleteRequest(ctx, [ctx]() { failAt(ctx, "insert_request", "conflict_unexplained"); });
                          return;
                      }
                      Json::Value detail(Json::objectValue);
                      detail["memory_id"] = ctx->memoryId;
                      detail["request_id"] = ctx->proposalId;
                      step(ctx, kAuditSql, "audit",
                           [ctx](const drogon::orm::Result &) { commitRequestFacts(ctx, ctx->proposalId, "", Outcome::Created); },
                           ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->memoryId, compactJson(detail));
                  },
                  ctx->proposalId, ctx->memoryId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
         },
         ctx->memoryId);
}

void requestTarget(const CtxPtr &ctx) {
    const std::string requested = ctx->memoryId;
    step(ctx, kTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["memory_id"].isNull()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->memoryId = rows[0]["memory_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->fingerprint = MemoryService::deleteRequestFingerprint(ctx->memoryId);
             step(ctx, kMemoryLockSql, "lock",
                  [ctx](const drogon::orm::Result &) { classifyDeleteRequest(ctx, [ctx]() { requestChecks(ctx); }); },
                  ctx->memoryId);
         },
         ctx->userId, requested);
}

// The decision, then the cycle under the same key, then both audits: all of it commits, or none of it does.
void approveDeleteRequest(const CtxPtr &ctx) {
    writeCycle(ctx,
               // The caller already spent this key on a direct deletion. The decision rolls back with it.
               [ctx]() { reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作"); },
               [ctx]() {
                   if (ctx->fault == MemoryService::Fault::FailDeleteApproval) { failAt(ctx, "insert_deletion", "forced_fault"); return; }
                   Json::Value detail(Json::objectValue);
                   detail["memory_id"] = ctx->memoryId;
                   detail["request_id"] = ctx->proposalId;
                   detail["deletion_id"] = ctx->deletionId;
                   detail["outcome"] = "approved";
                   step(ctx, kAuditSql, "audit",
                        [ctx](const drogon::orm::Result &) {
                            Json::Value deleted(Json::objectValue);
                            deleted["memory_id"] = ctx->memoryId;
                            deleted["deletion_id"] = ctx->deletionId;
                            deleted["request_id"] = ctx->proposalId;
                            deleted["photos"] = ctx->cascaded;
                            step(ctx, kAuditSql, "audit",
                                 [ctx](const drogon::orm::Result &) { commitRequestFacts(ctx, ctx->proposalId, ctx->deletionId, Outcome::Ok); },
                                 ctx->userId, ctx->coupleId, std::string("memory.deleted"), ctx->memoryId, compactJson(deleted));
                        },
                        ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->memoryId, compactJson(detail));
               });
}

void deleteDecisionInsert(const CtxPtr &ctx) {
    const bool approve = ctx->decision == Decision::Approve;
    if (approve && ctx->deletionId.empty()) ctx->deletionId = newUuid();
    step(ctx, kDeleteDecisionInsertSql, "insert_decision",
         [ctx, approve](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) {
                 classifyDeleteRequest(ctx, [ctx]() { reject(ctx, Outcome::ProposalDecided, "这条删除申请已经处理过了"); });
                 return;
             }
             if (approve) { approveDeleteRequest(ctx); return; }
             Json::Value detail(Json::objectValue);
             detail["memory_id"] = ctx->memoryId;
             detail["request_id"] = ctx->proposalId;
             detail["outcome"] = ctx->decision == Decision::Reject ? "rejected" : "withdrawn";
             step(ctx, kAuditSql, "audit",
                  [ctx](const drogon::orm::Result &) { commitRequestFacts(ctx, ctx->proposalId, "", Outcome::Ok); },
                  ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->memoryId, compactJson(detail));
         },
         ctx->proposalId, ctx->memoryId, ctx->coupleId, ctx->userId,
         std::string(approve ? "approved" : ctx->decision == Decision::Reject ? "rejected" : "withdrawn"), ctx->deletionId, ctx->key, ctx->fingerprint);
}

void deleteDecisionChecks(const CtxPtr &ctx) {
    step(ctx, kDeleteDecisionStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             if (rows[0]["deleted"].as<bool>()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             if (ctx->decision == Decision::Withdraw ? ctx->userId != ctx->proposerId : ctx->userId != ctx->creatorId) {
                 reject(ctx, Outcome::Forbidden, ctx->decision == Decision::Withdraw ? "只有提出申请的一方可以撤回" : "只有创建者可以处理删除申请");
                 return;
             }
             if (rows[0]["decided"].as<bool>()) { reject(ctx, Outcome::ProposalDecided, "这条删除申请已经处理过了"); return; }
             deleteDecisionInsert(ctx);
         },
         ctx->memoryId, ctx->proposalId);
}

void deleteDecisionTarget(const CtxPtr &ctx) {
    const std::string requested = ctx->memoryId, request = ctx->proposalId;
    step(ctx, kDeleteRequestTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["memory_id"].isNull()) { reject(ctx, Outcome::NotFound, "回忆不存在"); return; }
             if (rows[0]["request_id"].isNull()) { reject(ctx, Outcome::ProposalNotFound, "删除申请不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->memoryId = rows[0]["memory_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->proposalId = rows[0]["request_id"].as<std::string>();
             ctx->proposerId = rows[0]["requester_id"].as<std::string>();
             ctx->fingerprint = MemoryService::deleteDecisionFingerprint(ctx->memoryId, ctx->proposalId, ctx->decision);
             step(ctx, kMemoryLockSql, "lock",
                  [ctx](const drogon::orm::Result &) { classifyDeleteRequest(ctx, [ctx]() { deleteDecisionChecks(ctx); }); },
                  ctx->memoryId);
         },
         ctx->userId, requested, request);
}

void run(const CtxPtr &ctx, void (*first)(const CtxPtr &)) {
    if (!db::Pool::instance().ready()) {
        Status status = makeStatus(Outcome::Error);
        status.detail = TaskService::failureDetail(opName(ctx->op), "pool", "not_ready");
        complete(ctx, std::move(status));
        return;
    }
    std::weak_ptr<Ctx> weak = ctx;
    db::TxScope::beginAsync(
        [ctx, first](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            first(ctx);
        },
        [weak](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            auto strong = weak.lock();
            if (!strong) return;
            Status status = makeStatus(strong->commitRequested ? Outcome::ResultUnknown : Outcome::Error);
            status.detail = strong->failDetail.empty() ? TaskService::failureDetail(opName(strong->op), "transaction", "failed") : strong->failDetail;
            complete(strong, std::move(status));
        });
}

CtxPtr makeCtx(Op op, const std::string &userId, const std::string &key, MemoryService::Callback cb) {
    auto ctx = std::make_shared<Ctx>();
    ctx->op = op;
    ctx->userId = userId;
    ctx->key = key;
    ctx->done = std::move(cb);
    return ctx;
}

// J2b2: all external calls live outside database transactions. Preparation is
// read-only; the existing locked path revalidates the frozen complete batch.
namespace dj = deletionjournal;
struct JournalMemoryCtx {
    std::shared_ptr<dj::Journal> journal;
    Op op;
    std::string userId, memoryId, key, proposalId, deletionId, fingerprint, creatorId, coupleId;
    bool classified = false, deleted = false;
    bool hasRequest = false, lookupDelivered = false;
    MemoryService::Fault fault = MemoryService::Fault::None;
    MemoryService::Callback done;
    std::optional<Status> status;
    dj::Operation operation;
};
using JM = std::shared_ptr<JournalMemoryCtx>;
const char *journalKeySql(Op op) {
    return op == Op::Undo ? kUndoByKeySql : op == Op::DecideDelete ? kDeleteDecisionByKeySql : kDeletionByKeySql;
}
std::string memoryJournalUuid(const std::string &seed, const std::string &label) {
    auto h = crypto::sha256Hex("usapp.memory-journal.v1:" + label + ":" + seed);
    h[12] = '8'; h[16] = 'a';
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

// A committed read fence is essential: TxScope rollback callbacks acknowledge
// only the request, so a replay cannot directly settle the external log there.
void verifyJournalMemory(const JM &ctx, std::function<void(dj::EffectState)> callback) {
    auto answer = std::make_shared<std::function<void(dj::EffectState)>>(std::move(callback));
    db::TxScope::beginAsync(
        [ctx, answer](std::shared_ptr<db::TxScope> scope) {
            scope->tx().execSqlAsync(kMemoryLockSql,
                [ctx, answer, scope](const drogon::orm::Result &) {
                    scope->tx().execSqlAsync(journalKeySql(ctx->op),
                        [ctx, answer, scope](const drogon::orm::Result &rows) {
                            bool matches = false;
                            try {
                                matches = rows.size() == 1u && rows[0]["request_fingerprint"].as<std::string>() == ctx->fingerprint &&
                                    (ctx->op == Op::DecideDelete ? rows[0]["id"].as<std::string>() == ctx->proposalId &&
                                      rows[0]["deletion_id"].as<std::string>() == ctx->deletionId : rows[0]["id"].as<std::string>() == ctx->deletionId);
                            } catch (const std::exception &) {}
                            if (!matches) { scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return; }
                            scope->tx().execSqlAsync(kJournalLinksSql,
                                [ctx, answer, scope](const drogon::orm::Result &links) {
                                    try {
                                        if (!journalPhotosMatch(links, ctx->operation, true)) {
                                            scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return;
                                        }
                                    } catch (const std::exception &) { scope->fail("memory journal links"); return; }
                                    if (ctx->op != Op::Undo) { scope->commit([answer]() { (*answer)(dj::EffectState::Committed); }); return; }
                                    scope->tx().execSqlAsync(
                                        "SELECT NOT EXISTS (SELECT 1 FROM memory_deletion_photo_cycles l "
                                        "LEFT JOIN media_deletion_reversals r ON r.deletion_id = l.media_deletion_id "
                                        "WHERE l.memory_deletion_id = $1::uuid AND r.deletion_id IS NULL) AS complete",
                                        [answer, scope](const drogon::orm::Result &rows) {
                                            try {
                                                if (rows.size() != 1u || !rows[0]["complete"].as<bool>()) {
                                                    scope->rollback([answer]() { (*answer)(dj::EffectState::Unknown); }); return;
                                                }
                                                scope->commit([answer]() { (*answer)(dj::EffectState::Committed); });
                                            } catch (const std::exception &) { scope->fail("memory journal undo"); }
                                        }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory journal undo"); }, ctx->deletionId);
                                }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory journal links"); }, ctx->deletionId);
                        }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory journal classify"); }, ctx->userId, ctx->key);
                }, [scope](const drogon::orm::DrogonDbException &) { scope->fail("memory journal fence"); }, ctx->memoryId);
        }, [answer](const std::string &) { (*answer)(dj::EffectState::Unknown); });
}

struct JournalMemoryEffect : dj::Effect {
    JM ctx;
    explicit JournalMemoryEffect(JM c) : ctx(std::move(c)) {}
    void applyOrClassify(const dj::Operation &, std::function<void(dj::EffectState)> callback) override {
        // Classified replay was already verified before the external call.
        if (ctx->classified) { callback(dj::EffectState::Committed); return; }
        auto raw = makeCtx(ctx->op, ctx->userId, ctx->key, [c = ctx, callback](Status status) {
            const bool success = status.success() && status.deletion["deletion_id"].isString() &&
                                 status.deletion["deletion_id"].asString() == c->deletionId;
            c->status = std::move(status);
            if (success) verifyJournalMemory(c, callback);
            else callback(dj::EffectState::Unknown);  // NEVER infer abort from a rollback request
        });
        raw->memoryId = ctx->memoryId; raw->proposalId = ctx->proposalId; raw->deletionId = ctx->deletionId;
        raw->decision = Decision::Approve; raw->fault = ctx->fault; raw->journalOperation = ctx->operation;
        run(raw, ctx->op == Op::DecideDelete ? deleteDecisionTarget : deletionTarget);
    }
};

void coordinateJournalMemory(const JM &ctx) {
    dj::Coordinator::run(ctx->operation, ctx->journal, std::make_shared<JournalMemoryEffect>(ctx), [ctx](dj::Result result) {
        if (result == dj::Result::Committed && ctx->status && ctx->status->success()) { ctx->done(std::move(*ctx->status)); return; }
        if (result == dj::Result::Conflict) { ctx->done(makeStatus(Outcome::KeyReused, "该请求标识已用于另一项操作")); return; }
        if (result == dj::Result::Unknown && ctx->status && !ctx->status->success() &&
            ctx->status->outcome != Outcome::Error && ctx->status->outcome != Outcome::ResultUnknown) {
            ctx->done(std::move(*ctx->status)); return;
        }
        ctx->done(makeStatus(Outcome::ResultUnknown, "结果未确认，请保留原请求重试"));
    });
}

void readJournalMemoryReplay(const JM &ctx) {
    db::Pool::instance().client()->execSqlAsync(kDeletionFactsSql,
        [ctx](const drogon::orm::Result &rows) {
            try {
                Status status = makeStatus(Outcome::Replayed);
                if (rows.size() != 1u || !parseObject(rows[0]["deletion_json"].as<std::string>(), status.deletion)) throw std::runtime_error("facts");
                ctx->status = std::move(status);
                auto verified = [ctx]() {
                    verifyJournalMemory(ctx, [ctx](dj::EffectState state) {
                        if (state != dj::EffectState::Committed) { ctx->done(makeStatus(Outcome::ResultUnknown)); return; }
                        coordinateJournalMemory(ctx);
                    });
                };
                if (ctx->op != Op::DecideDelete) { verified(); return; }
                db::Pool::instance().client()->execSqlAsync(kDeleteRequestFactsSql,
                    [ctx, verified](const drogon::orm::Result &requests) {
                        try {
                            if (requests.size() != 1u || !parseObject(requests[0]["request_json"].as<std::string>(), ctx->status->request)) throw std::runtime_error("request");
                            verified();
                        } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
                    }, [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); }, ctx->proposalId);
            } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
        }, [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); }, ctx->deletionId);
}

void completeJournalPreparation(const JM &ctx) {
    if (ctx->classified) readJournalMemoryReplay(ctx);
    else coordinateJournalMemory(ctx);
}

void prepareJournalMemoryBatch(const JM &ctx) {
    Json::Value seed(Json::arrayValue);
    seed.append(ctx->userId); seed.append(ctx->op == Op::Undo ? "undo" : "delete"); seed.append(ctx->key);
    ctx->operation.id = memoryJournalUuid(compactJson(seed), "operation");
    ctx->operation.action = ctx->op == Op::Undo ? dj::Action::Undo : dj::Action::Delete;
    ctx->operation.requestFingerprint = ctx->fingerprint;
    try {
        ctx->journal->lookupOperation(ctx->operation.id, [ctx](dj::LookupState state, dj::Operation frozen) {
            if (ctx->lookupDelivered) return;
            ctx->lookupDelivered = true;
            if (state == dj::LookupState::Unknown) { ctx->done(makeStatus(Outcome::ResultUnknown)); return; }
            if (state == dj::LookupState::Found) {
                if (!dj::normalize(frozen) || frozen.id != ctx->operation.id || frozen.action != ctx->operation.action ||
                    frozen.requestFingerprint != ctx->fingerprint) {
                    ctx->done(makeStatus(Outcome::KeyReused, "该请求标识已用于另一项操作")); return;
                }
                int roots = 0;
                std::string root;
                for (const auto &s : frozen.subjects) if (s.kind == dj::Kind::Memory) {
                    if (s.targetId == ctx->memoryId) { roots++; root = s.cycleId; } else roots += 2;
                }
                if (roots != 1 || ((!ctx->deletionId.empty()) && root != ctx->deletionId)) {
                    ctx->done(makeStatus(Outcome::KeyReused, "该请求标识已用于另一项操作")); return;
                }
                ctx->deletionId = root;
                ctx->operation = std::move(frozen);
                completeJournalPreparation(ctx); return;
            }
            if (state != dj::LookupState::Missing) { ctx->done(makeStatus(Outcome::ResultUnknown)); return; }
            if (ctx->deletionId.empty()) ctx->deletionId = memoryJournalUuid(ctx->operation.id, "cycle");
            ctx->operation.subjects = {{dj::Kind::Memory, ctx->memoryId, ctx->deletionId,
                                       memoryJournalUuid(ctx->operation.id, "memory-intent")}};
            const bool linked = ctx->classified || ctx->op == Op::Undo;
            auto onPhotos = [ctx, linked](const drogon::orm::Result &rows) {
                try {
                    for (const auto &row : rows) {
                        const auto photo = row["media_id"].as<std::string>();
                        const auto cycle = linked ? row["cycle_id"].as<std::string>() : memoryJournalUuid(ctx->deletionId, "photo-cycle:" + photo);
                        ctx->operation.subjects.push_back({dj::Kind::Media, photo, cycle,
                            memoryJournalUuid(ctx->operation.id, "photo-intent:" + photo)});
                    }
                    if (!dj::normalize(ctx->operation)) throw std::runtime_error("batch");
                    completeJournalPreparation(ctx);
                } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
            };
            auto error = [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); };
            if (linked) db::Pool::instance().client()->execSqlAsync(kJournalLinksSql, onPhotos, error, ctx->deletionId);
            else db::Pool::instance().client()->execSqlAsync(kJournalCandidatesSql, onPhotos, error, ctx->memoryId, ctx->creatorId);
        });
    } catch (const std::exception &) { if (!ctx->lookupDelivered) ctx->done(makeStatus(Outcome::ResultUnknown)); }
}

void prepareJournalMemory(const JM &ctx) {
    const char *target =
        "SELECT c.couple_id::text AS couple_id, m.id::text AS memory_id, m.creator_id::text AS creator_id, "
        "usapp_memory_deleted_now(m.id) AS deleted, q.id::text AS request_id "
        "FROM couple_members c LEFT JOIN memories m ON m.id = $2::uuid AND m.couple_id = c.couple_id "
        "LEFT JOIN memory_delete_requests q ON q.memory_id = m.id AND q.id = NULLIF($3::text, '')::uuid WHERE c.user_id = $1::uuid";
    db::Pool::instance().client()->execSqlAsync(target,
        [ctx](const drogon::orm::Result &rows) {
            try {
                if (rows.size() != 1u) { ctx->done(makeStatus(Outcome::SpaceNotFound, "尚未加入空间")); return; }
                if (rows[0]["memory_id"].isNull()) { ctx->done(makeStatus(Outcome::NotFound, "回忆不存在")); return; }
                ctx->creatorId = rows[0]["creator_id"].as<std::string>();
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->deleted = rows[0]["deleted"].as<bool>();
                ctx->hasRequest = !rows[0]["request_id"].isNull();
                ctx->fingerprint = ctx->op == Op::Undo ? MemoryService::undoFingerprint(ctx->memoryId, ctx->deletionId) :
                    ctx->op == Op::DecideDelete ? MemoryService::deleteDecisionFingerprint(ctx->memoryId, ctx->proposalId, Decision::Approve) :
                    MemoryService::deletionFingerprint(ctx->memoryId);
                db::Pool::instance().client()->execSqlAsync(journalKeySql(ctx->op),
                    [ctx](const drogon::orm::Result &keys) {
                        try {
                            if (!keys.empty()) {
                                if (keys[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                                    ctx->done(makeStatus(Outcome::KeyReused, "该请求标识已用于另一项操作")); return;
                                }
                                ctx->classified = true;
                                ctx->deletionId = keys[0][ctx->op == Op::DecideDelete ? "deletion_id" : "id"].as<std::string>();
                                prepareJournalMemoryBatch(ctx); return;
                            }
                            if (ctx->op == Op::DecideDelete && !ctx->hasRequest) {
                                ctx->done(makeStatus(Outcome::ProposalNotFound, "删除申请不存在")); return;
                            }
                            if (ctx->creatorId != ctx->userId) {
                                ctx->done(makeStatus(ctx->op == Op::Undo || ctx->deleted ? Outcome::NotFound : Outcome::Forbidden, "只有创建者可以删除或撤销")); return;
                            }
                            if (ctx->op != Op::Undo) { prepareJournalMemoryBatch(ctx); return; }
                            db::Pool::instance().client()->execSqlAsync(
                                "SELECT deleted_by::text AS owner FROM memory_deletions WHERE id = $1::uuid AND memory_id = $2::uuid AND couple_id = $3::uuid",
                                [ctx](const drogon::orm::Result &cycles) {
                                    try {
                                        if (cycles.size() != 1u || cycles[0]["owner"].as<std::string>() != ctx->userId) {
                                            ctx->done(makeStatus(Outcome::NotFound, "删除轮次不存在")); return;
                                        }
                                        prepareJournalMemoryBatch(ctx);
                                    } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
                                }, [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); },
                                ctx->deletionId, ctx->memoryId, ctx->coupleId);
                        } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
                    }, [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); }, ctx->userId, ctx->key);
            } catch (const std::exception &) { ctx->done(makeStatus(Outcome::Error)); }
        }, [ctx](const drogon::orm::DrogonDbException &) { ctx->done(makeStatus(Outcome::Error)); }, ctx->userId, ctx->memoryId, ctx->proposalId);
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

MemoryService &MemoryService::instance() {
    static MemoryService service;
    return service;
}

void MemoryService::configureDeletionJournal(std::shared_ptr<deletionjournal::Journal> journal) {
    std::lock_guard<std::mutex> guard(journalMutex_);
    deletionJournal_ = std::move(journal);
}

bool MemoryService::parseCreateRequest(const Json::Value &body, Terms &terms, std::string &message) {
    if (!onlyFields(body, {"date", "title", "media_ids"}, {"description"}, message)) return false;
    return parseTerms(body, terms, message);
}

bool MemoryService::parseRevisionRequest(const Json::Value &body, int &expectedVersion, Terms &terms, std::string &message) {
    if (!onlyFields(body, {"expected_version", "date", "title", "media_ids"}, {"description"}, message)) return false;
    const auto &v = body["expected_version"];
    if (!((v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt()) || v.asInt() < 1) {
        message = "expected_version 必须是正整数";
        return false;
    }
    expectedVersion = v.asInt();
    return parseTerms(body, terms, message);
}

bool MemoryService::parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message) {
    if (!onlyFields(body, {"outcome"}, {}, message)) return false;
    const auto &v = body["outcome"];
    if (v.isString() && v.asString() == "approve") decision = Decision::Approve;
    else if (v.isString() && v.asString() == "reject") decision = Decision::Reject;
    else if (v.isString() && v.asString() == "withdraw") decision = Decision::Withdraw;
    else {
        message = "outcome 只能是 approve、reject 或 withdraw";
        return false;
    }
    return true;
}

std::string MemoryService::createFingerprint(const Terms &terms) {
    return crypto::sha256Hex("memory-create:v1:" + termsCanonical(terms));
}

std::string MemoryService::editFingerprint(const std::string &memoryId, int expectedVersion, const Terms &terms) {
    return crypto::sha256Hex("memory-edit:v1:" + field("memory_id", memoryId) + field("expected_version", std::to_string(expectedVersion)) + termsCanonical(terms));
}

std::string MemoryService::proposeFingerprint(const std::string &memoryId, int expectedVersion, const Terms &terms) {
    return crypto::sha256Hex("memory-propose:v1:" + field("memory_id", memoryId) + field("expected_version", std::to_string(expectedVersion)) + termsCanonical(terms));
}

std::string MemoryService::decisionFingerprint(const std::string &memoryId, const std::string &proposalId, Decision decision) {
    const char *name = decision == Decision::Approve ? "approve" : decision == Decision::Reject ? "reject" : "withdraw";
    return crypto::sha256Hex("memory-decide:v1:" + field("memory_id", memoryId) + field("proposal_id", proposalId) + field("outcome", name));
}

std::string MemoryService::deletionFingerprint(const std::string &memoryId) {
    return crypto::sha256Hex("memory-delete:v1:" + field("memory_id", memoryId));
}

std::string MemoryService::undoFingerprint(const std::string &memoryId, const std::string &deletionId) {
    return crypto::sha256Hex("memory-undo:v1:" + field("memory_id", memoryId) + field("deletion_id", deletionId));
}

std::string MemoryService::deleteRequestFingerprint(const std::string &memoryId) {
    return crypto::sha256Hex("memory-delete-request:v1:" + field("memory_id", memoryId));
}

std::string MemoryService::deleteDecisionFingerprint(const std::string &memoryId, const std::string &requestId, Decision decision) {
    const char *name = decision == Decision::Approve ? "approve" : decision == Decision::Reject ? "reject" : "withdraw";
    return crypto::sha256Hex("memory-delete-decide:v1:" + field("memory_id", memoryId) + field("request_id", requestId) + field("outcome", name));
}

std::string MemoryService::encodeCursor(const Cursor &cursor) { return cursor.date + "_" + cursor.createdAt + "_" + cursor.id; }

// Strictly the server's own form: date_timestamp_uuid, so nothing else reaches the statement.
std::optional<MemoryService::Cursor> MemoryService::decodeCursor(const std::string &raw) {
    if (raw.size() > TaskService::kMaxCursorLength) return std::nullopt;
    const auto first = raw.find('_');
    const auto second = first == std::string::npos ? std::string::npos : raw.find('_', first + 1);
    if (second == std::string::npos) return std::nullopt;
    Cursor cursor{raw.substr(0, first), raw.substr(first + 1, second - first - 1), lower(raw.substr(second + 1))};
    const auto date = AnniversaryCalendar::parse(cursor.date);
    if (!date || AnniversaryCalendar::format(*date) != cursor.date || !TaskService::isCanonicalUuid(cursor.id)) return std::nullopt;
    // YYYY-MM-DDTHH:MM:SS.ffffffZ, as this service writes it.
    const auto &t = cursor.createdAt;
    if (t.size() != 27u || t[10] != 'T' || t[19] != '.' || t[26] != 'Z' || !AnniversaryCalendar::parse(t.substr(0, 10))) return std::nullopt;
    for (size_t i = 11; i < 26; ++i) {
        const bool punct = i == 13 || i == 16 || i == 19;
        if (punct ? (i == 19 ? t[i] != '.' : t[i] != ':') : !std::isdigit(static_cast<unsigned char>(t[i]))) return std::nullopt;
    }
    if (t.substr(11, 2) > "23" || t.substr(14, 2) > "59" || t.substr(17, 2) > "60") return std::nullopt;
    return cursor;
}

Json::Value MemoryService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    if (status.request.isObject()) {
        body["request"] = status.request;
        if (status.deletion.isObject()) body["deletion"] = status.deletion;
        return body;
    }
    if (status.deletion.isObject()) {
        body["deletion"] = status.deletion;
        return body;
    }
    if (status.proposal.isObject()) {
        body["proposal"] = status.proposal;
        return body;
    }
    if (status.mutation.isObject()) body["mutation"] = status.mutation;
    body["memory"] = status.memory;
    if (status.hasNotification) body["notification"] = status.notification;
    return body;
}

ErrorCode MemoryService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::NotFound: return ErrorCode::MemoryNotFound;
        case Outcome::Forbidden: return ErrorCode::MemoryEditForbidden;
        case Outcome::VersionConflict: return ErrorCode::MemoryVersionConflict;
        case Outcome::ProposalPending: return ErrorCode::MemoryProposalPending;
        case Outcome::ProposalNotFound: return ErrorCode::MemoryProposalNotFound;
        case Outcome::ProposalDecided: return ErrorCode::MemoryProposalDecided;
        case Outcome::ProposalStale: return ErrorCode::MemoryProposalStale;
        case Outcome::PhotoUnavailable: return ErrorCode::MemoryPhotoUnavailable;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::PlanNotFound: return ErrorCode::NotFound;
        case Outcome::PlanConflict: return ErrorCode::PlanConflict;
        case Outcome::AlreadyDeleted: return ErrorCode::MemoryAlreadyDeleted;
        case Outcome::UndoClosed: return ErrorCode::MemoryUndoClosed;
        case Outcome::DeletionNotActive: return ErrorCode::MemoryDeletionNotActive;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        default: return ErrorCode::InternalError;
    }
}

int MemoryService::httpStatusFor(Outcome outcome) { return outcome == Outcome::Created ? 201 : 200; }

// =====================================================================
// Actions
// =====================================================================

void MemoryService::create(const std::string &userId, const std::string &key, const Terms &terms, Callback cb, intent::Choice choice) {
    auto ctx = makeCtx(Op::Create, userId, key, std::move(cb));
    ctx->terms = terms;
    ctx->choice = choice;
    // Omitted and false keep the legacy bytes; true is domain-separated, so a changed choice under one key is key reuse.
    ctx->fingerprint = intent::fingerprint(createFingerprint(terms), choice);
    run(ctx, createStart);
}

void MemoryService::createForPlan(const std::string &userId, const std::string &planId, const std::string &key,
                                  const Terms &terms, Callback cb, Fault fault) {
    auto ctx = makeCtx(Op::Create, userId, crypto::sha256Hex("plan-memory-key:v1:" + key), std::move(cb));
    ctx->planId = lower(planId);
    ctx->planKey = key;
    ctx->terms = terms;
    ctx->fault = fault;
    ctx->fingerprint = crypto::sha256Hex("plan-memory-create:v1:" + ctx->planId + ":" + createFingerprint(terms));
    run(ctx, createStart);
}

void MemoryService::edit(const std::string &userId, const std::string &memoryId, const std::string &key, int expectedVersion, const Terms &terms, Callback cb) {
    auto ctx = makeCtx(Op::Edit, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->expectedVersion = expectedVersion;
    ctx->terms = terms;
    run(ctx, revisionTarget);
}

void MemoryService::propose(const std::string &userId, const std::string &memoryId, const std::string &key, int expectedVersion, const Terms &terms, Callback cb) {
    auto ctx = makeCtx(Op::Propose, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->expectedVersion = expectedVersion;
    ctx->terms = terms;
    run(ctx, revisionTarget);
}

void MemoryService::decide(const std::string &userId, const std::string &memoryId, const std::string &proposalId, const std::string &key, Decision decision,
                           Callback cb, Fault fault) {
    auto ctx = makeCtx(Op::Decide, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->proposalId = proposalId;
    ctx->decision = decision;
    ctx->fault = fault;
    run(ctx, decisionTarget);
}

void MemoryService::deleteMemory(const std::string &userId, const std::string &memoryId, const std::string &key, Callback cb, Fault fault) {
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> guard(journalMutex_); journal = deletionJournal_; }
    if (journal) {
        auto ctx = std::make_shared<JournalMemoryCtx>();
        ctx->journal = std::move(journal); ctx->op = Op::Delete;
        ctx->userId = userId; ctx->memoryId = lower(memoryId); ctx->key = key; ctx->fault = fault; ctx->done = std::move(cb);
        prepareJournalMemory(ctx); return;
    }
    auto ctx = makeCtx(Op::Delete, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->fault = fault;
    run(ctx, deletionTarget);
}

void MemoryService::undoDeletion(const std::string &userId, const std::string &memoryId, const std::string &deletionId, const std::string &key, Callback cb, Fault fault) {
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> guard(journalMutex_); journal = deletionJournal_; }
    if (journal) {
        auto ctx = std::make_shared<JournalMemoryCtx>();
        ctx->journal = std::move(journal); ctx->op = Op::Undo;
        ctx->userId = userId; ctx->memoryId = lower(memoryId); ctx->key = key; ctx->deletionId = lower(deletionId);
        ctx->fault = fault; ctx->done = std::move(cb); prepareJournalMemory(ctx); return;
    }
    auto ctx = makeCtx(Op::Undo, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->deletionId = lower(deletionId);
    ctx->fault = fault;
    run(ctx, deletionTarget);
}

void MemoryService::requestDeletion(const std::string &userId, const std::string &memoryId, const std::string &key, Callback cb) {
    auto ctx = makeCtx(Op::RequestDelete, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    run(ctx, requestTarget);
}

void MemoryService::decideDeleteRequest(const std::string &userId, const std::string &memoryId, const std::string &requestId, const std::string &key, Decision decision,
                                        Callback cb, Fault fault) {
    std::shared_ptr<dj::Journal> journal;
    { std::lock_guard<std::mutex> guard(journalMutex_); journal = deletionJournal_; }
    if (journal && decision == Decision::Approve) {
        auto ctx = std::make_shared<JournalMemoryCtx>();
        ctx->journal = std::move(journal); ctx->op = Op::DecideDelete;
        ctx->userId = userId; ctx->memoryId = lower(memoryId); ctx->key = key; ctx->proposalId = lower(requestId);
        ctx->fault = fault; ctx->done = std::move(cb); prepareJournalMemory(ctx); return;
    }
    auto ctx = makeCtx(Op::DecideDelete, userId, key, std::move(cb));
    ctx->memoryId = memoryId;
    ctx->proposalId = requestId;
    ctx->decision = decision;
    ctx->fault = fault;
    run(ctx, deleteDecisionTarget);
}

void MemoryService::getDeleteRequest(const std::string &userId, const std::string &memoryId, const std::string &requestId, Callback cb) {
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kDeleteRequestReadSql,
        [done](const drogon::orm::Result &rows) {
            Status status = makeStatus(Outcome::ProposalNotFound, "删除申请不存在");
            if (rows.size() == 1u && parseObject(rows[0]["request_json"].as<std::string>(), status.request)) status.outcome = Outcome::Ok;
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) { (*done)(makeStatus(Outcome::Error)); },
        userId, memoryId, requestId);
}

void MemoryService::listDeletions(const std::string &userId, int limit, const std::optional<std::pair<std::string, std::string>> &after, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::SpaceNotFound;
            result.message = "尚未加入空间";
            (*done)(std::move(result));
            return;
        }
        try {
            result.outcome = Outcome::Ok;
            size_t kept = 0;
            for (const auto &row : rows) {
                if (row["deletion_json"].isNull()) continue;
                if (kept == static_cast<size_t>(limit)) {
                    const auto &last = rows[kept - 1];
                    result.nextCursor = AlbumService::encodeCursor({last["sort_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                Json::Value item;
                if (!parseObject(row["deletion_json"].as<std::string>(), item)) throw std::runtime_error("deletion json");
                result.memories.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("memory_deletions", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("memory_deletions", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (after) {
        client->execSqlAsync(kDeletionsAfterSql, std::move(onRows), std::move(onError), userId, after->first, after->second, std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kDeletionsFirstSql, std::move(onRows), std::move(onError), userId, std::to_string(limit + 1));
    }
}

void MemoryService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::SpaceNotFound;
            result.message = "尚未加入空间";
            (*done)(std::move(result));
            return;
        }
        try {
            result.outcome = Outcome::Ok;
            size_t kept = 0;
            for (const auto &row : rows) {
                if (row["memory_json"].isNull()) continue;
                if (kept == static_cast<size_t>(limit)) {
                    const auto &last = rows[kept - 1];
                    result.nextCursor = encodeCursor({last["sort_date"].as<std::string>(), last["sort_created_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                Json::Value item;
                if (!parseObject(row["memory_json"].as<std::string>(), item)) throw std::runtime_error("memory json");
                result.memories.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("memory_list", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("memory_list", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (cursor) {
        client->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId, cursor->date, cursor->createdAt, cursor->id, std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId, std::to_string(limit + 1));
    }
}

void MemoryService::get(const std::string &userId, const std::string &memoryId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            Status status = makeStatus(Outcome::NotFound, "回忆不存在");
            if (rows.size() == 1u && parseObject(rows[0]["memory_json"].as<std::string>(), status.memory)) status.outcome = Outcome::Ok;
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            Status status = makeStatus(Outcome::Error);
            status.detail = TaskService::failureDetail("memory_get", "read", "db_error");
            (*done)(std::move(status));
        },
        userId, memoryId);
}

void MemoryService::getProposal(const std::string &userId, const std::string &memoryId, const std::string &proposalId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kProposalSql,
        [done](const drogon::orm::Result &rows) {
            Status status = makeStatus(Outcome::ProposalNotFound, "修改申请不存在");
            if (rows.size() == 1u && parseObject(rows[0]["proposal_json"].as<std::string>(), status.proposal)) status.outcome = Outcome::Ok;
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            Status status = makeStatus(Outcome::Error);
            status.detail = TaskService::failureDetail("memory_proposal", "read", "db_error");
            (*done)(std::move(status));
        },
        userId, memoryId, proposalId);
}

void MemoryService::listProposals(const std::string &userId, const std::string &memoryId, int limit,
                                  const std::optional<std::pair<std::string, std::string>> &after, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::SpaceNotFound;
            result.message = "尚未加入空间";
            (*done)(std::move(result));
            return;
        }
        if (rows[0]["memory_id"].isNull()) {
            result.outcome = Outcome::NotFound;
            result.message = "回忆不存在";
            (*done)(std::move(result));
            return;
        }
        try {
            result.outcome = Outcome::Ok;
            result.currentVersion = rows[0]["current_version"].as<long long>();
            size_t kept = 0;
            for (const auto &row : rows) {
                if (row["proposal_json"].isNull()) continue;
                if (kept == static_cast<size_t>(limit)) {
                    const auto &last = rows[kept - 1];
                    result.nextCursor = AlbumService::encodeCursor({last["sort_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                Json::Value item;
                if (!parseObject(row["proposal_json"].as<std::string>(), item)) throw std::runtime_error("proposal json");
                result.memories.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("memory_proposals", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("memory_proposals", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (after) {
        client->execSqlAsync(kProposalsAfterSql, std::move(onRows), std::move(onError), userId, memoryId, after->first, after->second,
                             std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kProposalsFirstSql, std::move(onRows), std::move(onError), userId, memoryId, std::to_string(limit + 1));
    }
}

}  // namespace usapp
