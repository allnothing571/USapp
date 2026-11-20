// =====================================================================
// services/api/src/services/AlbumService.cc
// =====================================================================
#include "services/AlbumService.h"
#include "services/MediaEligibility.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <cctype>
#include <exception>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/AnniversaryCalendar.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"
// D6: a deleted photo is out of the list, the count and the cover until an undo, and an undo brings back its old item and any choice.
#define USAPP_ACTIVE(item)                                                                                       \
    "NOT EXISTS (SELECT 1 FROM album_item_removals rm WHERE rm.album_id = " item ".album_id AND rm.media_id = " item ".media_id) " \
    "AND NOT usapp_media_deleted_now(" item ".media_id)"
#define USAPP_CHOSEN "(SELECT e.media_id FROM album_cover_events e WHERE e.album_id = a.id ORDER BY e.event_no DESC LIMIT 1)"

// The current state of album a with its latest revision r. Both members read it. Never an object key. The chosen cover while it
// is active, otherwise the highest active seq.
#define USAPP_ALBUM_JSON                                                                                         \
    "json_build_object('id', a.id, 'creator_id', a.creator_id, 'name', tx.name, 'version', r.version, "          \
    "'created_at', " USAPP_TS("a.created_at") ", "                                                               \
    "'pending_proposal', (SELECT json_build_object('id', p.id, 'proposer_id', p.proposer_id, "                   \
    "   'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ") "                            \
    "   FROM album_edit_proposals p WHERE p.album_id = a.id "                                                    \
    "   AND NOT EXISTS (SELECT 1 FROM album_edit_decisions d WHERE d.proposal_id = p.id) LIMIT 1), "             \
    "'photo_count', (SELECT count(*) FROM album_items i WHERE i.album_id = a.id AND " USAPP_ACTIVE("i") "), "    \
    "'cover', (SELECT json_build_object('media_id', i.media_id, 'uploader_id', mo.uploader_id, "                 \
    "   'width', mt.width, 'height', mt.height, 'chosen', i.media_id IS NOT DISTINCT FROM " USAPP_CHOSEN ") "    \
    "   FROM album_items i JOIN media_objects mo ON mo.id = i.media_id "                                         \
    "   LEFT JOIN media_upload_outcomes mt ON mt.media_id = i.media_id "                                         \
    "   WHERE i.album_id = a.id AND " USAPP_ACTIVE("i")                                                          \
    "   ORDER BY (i.media_id = " USAPP_CHOSEN ") DESC NULLS LAST, i.seq DESC LIMIT 1))"
#define USAPP_ALBUM_FROM                                                                                         \
    "albums a JOIN LATERAL (SELECT x.* FROM album_revisions x WHERE x.album_id = a.id "                          \
    "ORDER BY x.version DESC LIMIT 1) r ON true JOIN album_texts tx ON tx.id = r.text_id"

const char *kGetSql =
    "SELECT " USAPP_ALBUM_JSON "::text AS album_json FROM couple_members c JOIN " USAPP_ALBUM_FROM
    " ON a.couple_id = c.couple_id WHERE c.user_id = $1::uuid AND a.id = $2::uuid";

// Pages by (created_at, id) descending. One row per album, or one row of NULLs when the space has none.
#define USAPP_LIST_SQL(filter, limit)                                                                            \
    "SELECT c.couple_id::text AS couple_id, x.* FROM couple_members c LEFT JOIN LATERAL ("                       \
    "SELECT " USAPP_ALBUM_JSON "::text AS album_json, " USAPP_TS("a.created_at") " AS sort_created_at, "         \
    "a.id::text AS sort_id FROM " USAPP_ALBUM_FROM                                                               \
    " WHERE a.couple_id = c.couple_id" filter " ORDER BY a.created_at DESC, a.id DESC LIMIT " limit ") x ON true " \
    "WHERE c.user_id = $1::uuid"
const char *kListFirstSql = USAPP_LIST_SQL("", "$2::integer");
const char *kListAfterSql = USAPP_LIST_SQL(" AND (a.created_at, a.id) < ($2::timestamptz, $3::uuid)", "$4::integer");

// The album's active photos by seq descending. No row: not this member's album. One row of NULLs: none active.
#define USAPP_PHOTOS_SQL(filter, limit)                                                                          \
    "SELECT a.id::text AS album_id, x.* FROM couple_members c JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid " \
    "LEFT JOIN LATERAL (SELECT json_build_object('seq', i.seq, 'media_id', i.media_id, 'uploader_id', mo.uploader_id, "        \
    "'width', mt.width, 'height', mt.height, 'added_at', " USAPP_TS("i.created_at") ")::text AS photo_json, i.seq AS sort_seq " \
    "FROM album_items i JOIN media_objects mo ON mo.id = i.media_id LEFT JOIN media_upload_outcomes mt ON mt.media_id = i.media_id " \
    "WHERE i.album_id = a.id AND " USAPP_ACTIVE("i") filter " ORDER BY i.seq DESC LIMIT " limit ") x ON true "   \
    "WHERE c.user_id = $1::uuid"
const char *kPhotosFirstSql = USAPP_PHOTOS_SQL("", "$3::integer");
const char *kPhotosAfterSql = USAPP_PHOTOS_SQL(" AND i.seq < $3::integer", "$4::integer");

// V1 section 30: the immutable original and suggestion, with the requester, time and status.
const char *kProposalSql =
    "SELECT json_build_object('id', p.id, 'album_id', p.album_id, 'proposer_id', p.proposer_id, 'base_version', p.base_version, "
    "'created_at', " USAPP_TS("p.created_at") ", 'status', COALESCE(d.outcome, 'pending'), 'decided_by', d.decided_by, "
    "'decided_at', " USAPP_TS("d.decided_at") ", 'approved_version', ar.version, 'original_name', bt.name, 'proposed_name', pt.name)::text AS proposal_json "
    "FROM couple_members c JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid "
    "JOIN album_edit_proposals p ON p.album_id = a.id AND p.id = $3::uuid "
    "JOIN album_texts pt ON pt.id = p.text_id "
    "JOIN album_revisions b ON b.album_id = p.album_id AND b.version = p.base_version "
    "JOIN album_texts bt ON bt.id = b.text_id "
    "LEFT JOIN album_edit_decisions d ON d.proposal_id = p.id "
    "LEFT JOIN album_revisions ar ON ar.proposal_id = p.id "
    "WHERE c.user_id = $1::uuid";

// One album's rename requests as summaries, newest first. No row: not this member's album. Otherwise one row per request, or one row of
// NULLs when there is none. Served by album_edit_proposals_album_idx. Names, keys and fingerprints are not read.
#define USAPP_PROPOSAL_LIST_SQL(filter, limit)                                                                   \
    "SELECT a.id::text AS album_id, (SELECT max(v.version) FROM album_revisions v WHERE v.album_id = a.id) AS current_version, x.* " \
    "FROM couple_members c JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid "                     \
    "LEFT JOIN LATERAL (SELECT json_build_object('id', p.id, 'album_id', p.album_id, 'proposer_id', p.proposer_id, "             \
    "'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ", 'status', COALESCE(d.outcome, 'pending'), "     \
    "'decided_by', d.decided_by, 'decided_at', " USAPP_TS("d.decided_at") ", 'approved_version', ar.version)::text AS proposal_json, " \
    USAPP_TS("p.created_at") " AS sort_at, p.id::text AS sort_id FROM album_edit_proposals p "                  \
    "LEFT JOIN album_edit_decisions d ON d.proposal_id = p.id LEFT JOIN album_revisions ar ON ar.proposal_id = p.id " \
    "WHERE p.album_id = a.id" filter " ORDER BY p.created_at DESC, p.id DESC LIMIT " limit ") x ON true WHERE c.user_id = $1::uuid"
const char *kProposalsFirstSql = USAPP_PROPOSAL_LIST_SQL("", "$3::integer");
const char *kProposalsAfterSql = USAPP_PROPOSAL_LIST_SQL(" AND (p.created_at, p.id) < ($3::timestamptz, $4::uuid)", "$5::integer");
#undef USAPP_PROPOSAL_LIST_SQL

const char *kMembershipSql = "SELECT c.couple_id::text AS couple_id FROM couple_members c WHERE c.user_id = $1::uuid FOR SHARE OF c";
const char *kTargetSql =
    "SELECT c.couple_id::text AS couple_id, a.id::text AS album_id, a.creator_id::text AS creator_id "
    "FROM couple_members c LEFT JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid "
    "WHERE c.user_id = $1::uuid FOR SHARE OF c";
const char *kDecisionTargetSql =
    "SELECT c.couple_id::text AS couple_id, a.id::text AS album_id, a.creator_id::text AS creator_id, "
    "p.id::text AS proposal_id, p.proposer_id::text AS proposer_id FROM couple_members c "
    "LEFT JOIN albums a ON a.couple_id = c.couple_id AND a.id = $2::uuid "
    "LEFT JOIN album_edit_proposals p ON p.album_id = a.id AND p.id = $3::uuid "
    "WHERE c.user_id = $1::uuid FOR SHARE OF c";

const char *kNameStateSql =
    "SELECT r.version, tx.name, EXISTS (SELECT 1 FROM album_edit_proposals q WHERE q.album_id = r.album_id "
    "AND NOT EXISTS (SELECT 1 FROM album_edit_decisions d WHERE d.proposal_id = q.id)) AS has_pending "
    "FROM album_revisions r JOIN album_texts tx ON tx.id = r.text_id WHERE r.album_id = $1::uuid ORDER BY r.version DESC LIMIT 1";
const char *kDecidedSql = "SELECT EXISTS (SELECT 1 FROM album_edit_decisions d WHERE d.proposal_id = $1::uuid) AS decided";

const char *kAlbumLockSql = "SELECT pg_advisory_xact_lock(33021::integer, hashtext($1::text))";
// Sorted within one statement, the same order the cleanup job and memories use.
const char *kPhotoLockSql =
    "SELECT pg_advisory_xact_lock(33018::integer, k.h) FROM (SELECT DISTINCT hashtext(x) AS h "
    "FROM jsonb_array_elements_text($1::jsonb) x ORDER BY 1) k";

// Read after the photo locks. The caller's own object: READY, not planned, not reserved, and with no home of any kind. The conditions
// are MediaEligibility.h's, the same text as before it existed.
const char *kPhotoCheckSql =
    "SELECT count(*)::integer AS eligible FROM jsonb_array_elements_text($1::jsonb) w "
    "JOIN media_objects o ON o.id = w::uuid AND o.couple_id = $2::uuid AND o.uploader_id = $3::uuid "
    "JOIN media_upload_outcomes t ON t.media_id = o.id AND t.outcome = 'ready' "
    "WHERE " USAPP_MEDIA_UNHOMED("o");

const char *kItemStateSql =
    "SELECT i.added_by::text AS added_by, EXISTS (SELECT 1 FROM album_item_removals r WHERE r.album_id = i.album_id AND r.media_id = i.media_id) AS removed, "
    "usapp_media_deleted_now(i.media_id) AS deleted "
    "FROM album_items i WHERE i.album_id = $1::uuid AND i.media_id = $2::uuid";

const char *kCreateByKeySql =
    "SELECT album_id::text AS ref, version::text AS ref2, request_fingerprint FROM album_revisions WHERE actor_id = $1::uuid AND idempotency_key = $2";
const char *kProposalByKeySql =
    "SELECT id::text AS ref, '' AS ref2, request_fingerprint FROM album_edit_proposals WHERE proposer_id = $1::uuid AND idempotency_key = $2";
const char *kDecisionByKeySql =
    "SELECT proposal_id::text AS ref, '' AS ref2, request_fingerprint FROM album_edit_decisions WHERE decided_by = $1::uuid AND idempotency_key = $2";
const char *kAddByKeySql =
    "SELECT id::text AS ref, '' AS ref2, request_fingerprint FROM album_photo_batches WHERE added_by = $1::uuid AND idempotency_key = $2";
const char *kRemoveByKeySql =
    "SELECT album_id::text AS ref, media_id::text AS ref2, request_fingerprint FROM album_item_removals WHERE removed_by = $1::uuid AND idempotency_key = $2";
const char *kCoverByKeySql =
    "SELECT album_id::text AS ref, event_no::text AS ref2, request_fingerprint FROM album_cover_events WHERE actor_id = $1::uuid AND idempotency_key = $2";

const char *kRevisionFactsSql =
    "SELECT r.album_id::text AS album_id, json_build_object('action', r.action, 'album_id', r.album_id, 'actor_id', r.actor_id, 'version', r.version, "
    "'created_at', " USAPP_TS("r.created_at") ")::text AS mutation_json FROM album_revisions r WHERE r.album_id = $1::uuid AND r.version = $2::integer";
const char *kProposalFactsSql =
    "SELECT p.album_id::text AS album_id, json_build_object('action', 'propose', 'album_id', p.album_id, 'proposal_id', p.id, 'actor_id', p.proposer_id, "
    "'base_version', p.base_version, 'created_at', " USAPP_TS("p.created_at") ")::text AS mutation_json FROM album_edit_proposals p WHERE p.id = $1::uuid";
const char *kDecisionFactsSql =
    "SELECT p.album_id::text AS album_id, json_build_object('action', 'decide', 'album_id', p.album_id, 'proposal_id', d.proposal_id, 'actor_id', d.decided_by, "
    "'outcome', d.outcome, 'base_version', p.base_version, 'version', r.version, 'created_at', " USAPP_TS("d.decided_at") ")::text AS mutation_json "
    "FROM album_edit_decisions d JOIN album_edit_proposals p ON p.id = d.proposal_id "
    "LEFT JOIN album_revisions r ON r.proposal_id = d.proposal_id WHERE d.proposal_id = $1::uuid";
const char *kAddFactsSql =
    "SELECT b.album_id::text AS album_id, json_build_object('action', 'add_photos', 'album_id', b.album_id, 'batch_id', b.id, 'actor_id', b.added_by, "
    "'media_ids', (SELECT json_agg(i.media_id ORDER BY i.seq DESC) FROM album_items i WHERE i.batch_id = b.id), "
    "'created_at', " USAPP_TS("b.created_at") ")::text AS mutation_json FROM album_photo_batches b WHERE b.id = $1::uuid";
const char *kRemoveFactsSql =
    "SELECT r.album_id::text AS album_id, json_build_object('action', 'remove_photo', 'album_id', r.album_id, 'media_id', r.media_id, "
    "'actor_id', r.removed_by, 'created_at', " USAPP_TS("r.removed_at") ")::text AS mutation_json "
    "FROM album_item_removals r WHERE r.album_id = $1::uuid AND r.media_id = $2::uuid";
const char *kCoverFactsSql =
    "SELECT e.album_id::text AS album_id, json_build_object('action', 'set_cover', 'album_id', e.album_id, 'media_id', e.media_id, "
    "'actor_id', e.actor_id, 'event_no', e.event_no, 'created_at', " USAPP_TS("e.created_at") ")::text AS mutation_json "
    "FROM album_cover_events e WHERE e.album_id = $1::uuid AND e.event_no = $2::integer";

const char *kCurrentInTxSql =
    "SELECT " USAPP_ALBUM_JSON "::text AS album_json FROM " USAPP_ALBUM_FROM " WHERE a.id = $1::uuid";

const char *kIdentityInsertSql = "INSERT INTO albums (id, couple_id, creator_id) VALUES ($1::uuid, $2::uuid, $3::uuid)";
const char *kTextInsertSql = "INSERT INTO album_texts (couple_id, name) VALUES ($1::uuid, $2::text) RETURNING id::text AS id";
const char *kRevisionInsertSql =
    "INSERT INTO album_revisions (album_id, couple_id, version, actor_id, action, text_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $7::integer, $3::uuid, $8::text, $4::uuid, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING version";
const char *kProposalInsertSql =
    "INSERT INTO album_edit_proposals (id, couple_id, album_id, proposer_id, base_version, text_id, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::integer, $6::uuid, $7::text, $8::text) ON CONFLICT DO NOTHING RETURNING id::text AS id";
const char *kDecisionInsertSql =
    "INSERT INTO album_edit_decisions (proposal_id, couple_id, decided_by, outcome, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING proposal_id::text AS id";
// Approval: exactly the proposal's text, as base_version + 1. The guard refuses anything else.
const char *kApproveRevisionSql =
    "INSERT INTO album_revisions (album_id, couple_id, version, actor_id, action, proposal_id, text_id) "
    "SELECT p.album_id, p.couple_id, p.base_version + 1, $2::uuid, 'approve', p.id, p.text_id "
    "FROM album_edit_proposals p WHERE p.id = $1::uuid RETURNING version";
const char *kBatchInsertSql =
    "INSERT INTO album_photo_batches (id, album_id, couple_id, added_by, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING id::text AS id";
// The guard refuses anything not READY, planned, another's, or reserved.
const char *kBindSql =
    "INSERT INTO media_bindings (media_id, couple_id, target_kind, target_id, bound_by) "
    "SELECT x::uuid, $2::uuid, 'album', $3::uuid, $4::uuid FROM jsonb_array_elements_text($1::jsonb) x";
// The next N values, the first photo chosen highest, inserted lowest first so the guard's max + 1 holds.
const char *kItemsInsertSql =
    "WITH base AS (SELECT COALESCE(max(i.seq), 0) AS top FROM album_items i WHERE i.album_id = $1::uuid), "
    "req AS (SELECT o.v::uuid AS media_id, o.n, count(*) OVER () AS total FROM jsonb_array_elements_text($5::jsonb) WITH ORDINALITY o(v, n)) "
    "INSERT INTO album_items (album_id, seq, couple_id, media_id, batch_id, added_by) "
    "SELECT $1::uuid, (base.top + req.total - req.n + 1)::integer, $2::uuid, req.media_id, $3::uuid, $4::uuid "
    "FROM req CROSS JOIN base ORDER BY req.n DESC";
const char *kRemovalInsertSql =
    "INSERT INTO album_item_removals (album_id, media_id, couple_id, removed_by, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text) ON CONFLICT DO NOTHING RETURNING media_id::text AS id";
const char *kCoverInsertSql =
    "INSERT INTO album_cover_events (album_id, event_no, couple_id, media_id, actor_id, idempotency_key, request_fingerprint) "
    "SELECT $1::uuid, (SELECT COALESCE(max(e.event_no), 0) + 1 FROM album_cover_events e WHERE e.album_id = $1::uuid), "
    "$2::uuid, $3::uuid, $4::uuid, $5::text, $6::text ON CONFLICT DO NOTHING RETURNING event_no::text AS event_no";

// Ids and counts only: never text.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'album', $4::text, $5::jsonb)";

#undef USAPP_PHOTOS_SQL
#undef USAPP_LIST_SQL
#undef USAPP_ALBUM_FROM
#undef USAPP_ALBUM_JSON
#undef USAPP_CHOSEN
#undef USAPP_ACTIVE
#undef USAPP_TS

using Outcome = AlbumService::Outcome;
using Status = AlbumService::Status;

using Decision = AlbumService::Decision;

enum class Op { Create, Add, Remove, Cover, Rename, Propose, Decide };

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

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool normalizeName(const Json::Value &value, std::string &name, std::string &message) {
    if (!value.isString()) {
        message = "相册名称必须是字符串";
        return false;
    }
    switch (TaskService::normalizeTitle(value.asString(), name, AlbumService::kMaxNameCodePoints)) {
        case TaskService::TextError::None: return true;
        case TaskService::TextError::InvalidUtf8: message = "相册名称包含无法识别的字符"; return false;
        case TaskService::TextError::ControlCharacter: message = "相册名称只能是一行，且不能包含控制字符"; return false;
        case TaskService::TextError::Empty: message = "相册名称不能为空"; return false;
        case TaskService::TextError::TooLong: message = "相册名称最多 40 个字符"; return false;
    }
    return false;
}

bool exactFields(const Json::Value &body, const char *name, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是 JSON 对象";
        return false;
    }
    for (const auto &member : body.getMemberNames()) {
        if (member != name) {
            message = "请求体包含不允许的字段";
            return false;
        }
    }
    if (!body.isMember(name)) {
        message = std::string("缺少字段 ") + name;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- the write context
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    Op op = Op::Create;
    std::string userId, coupleId, albumId, creatorId, batchId, mediaId, proposalId, proposerId, key, fingerprint, name;
    std::vector<std::string> mediaIds;
    int expectedVersion = 0;
    Decision decision = Decision::Reject;
    AlbumService::Fault fault = AlbumService::Fault::None;
    std::string ref, ref2;
    intent::Choice choice;  // D20: an add batch only
    Status result;
    AlbumService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
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
        case Op::Create: return "album_create";
        case Op::Add: return "album_add";
        case Op::Remove: return "album_remove";
        case Op::Cover: return "album_cover";
        case Op::Rename: return "album_rename";
        case Op::Propose: return "album_propose";
        case Op::Decide: break;
    }
    return "album_decide";
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
        std::string albumId;
        try {
            if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) { failAt(ctx, "read_mutation", "json_format"); return; }
            albumId = rows[0]["album_id"].as<std::string>();
        } catch (const std::exception &) { failAt(ctx, "read_mutation", "column_format"); return; }
        step(ctx, kCurrentInTxSql, "read_current",
             [ctx, status, next](const drogon::orm::Result &current) {
                 if (current.size() != 1u || !parseObject(current[0]["album_json"].as<std::string>(), status->album)) {
                     failAt(ctx, "read_current", "row_count");
                     return;
                 }
                 next(std::move(*status));
             },
             albumId);
    };
    auto onError = [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_mutation", "db_error"); };
    switch (ctx->op) {
        case Op::Create:
        case Op::Rename: ctx->scope->tx().execSqlAsync(kRevisionFactsSql, std::move(onRows), std::move(onError), ctx->ref, ctx->ref2); break;
        case Op::Propose: ctx->scope->tx().execSqlAsync(kProposalFactsSql, std::move(onRows), std::move(onError), ctx->ref); break;
        case Op::Decide: ctx->scope->tx().execSqlAsync(kDecisionFactsSql, std::move(onRows), std::move(onError), ctx->ref); break;
        case Op::Add: ctx->scope->tx().execSqlAsync(kAddFactsSql, std::move(onRows), std::move(onError), ctx->ref); break;
        case Op::Remove: ctx->scope->tx().execSqlAsync(kRemoveFactsSql, std::move(onRows), std::move(onError), ctx->ref, ctx->ref2); break;
        case Op::Cover: ctx->scope->tx().execSqlAsync(kCoverFactsSql, std::move(onRows), std::move(onError), ctx->ref, ctx->ref2); break;
    }
}

// Same actor and key: an identical request replays its committed write, read-only, with no new row. A different fingerprint is
// IDEMPOTENCY_KEY_REUSED. Runs before any state check, so a replay never sees a state error.
void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    const char *sql = kCoverByKeySql;
    switch (ctx->op) {
        case Op::Create:
        case Op::Rename: sql = kCreateByKeySql; break;
        case Op::Add: sql = kAddByKeySql; break;
        case Op::Remove: sql = kRemoveByKeySql; break;
        case Op::Propose: sql = kProposalByKeySql; break;
        case Op::Decide: sql = kDecisionByKeySql; break;
        case Op::Cover: break;
    }
    step(ctx, sql, "classify",
         [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) { onMissing(); return; }
             ctx->ref = rows[0]["ref"].as<std::string>();
             ctx->ref2 = rows[0]["ref2"].as<std::string>();
             if (rows[0]["request_fingerprint"].as<std::string>() != ctx->fingerprint) {
                 reject(ctx, Outcome::KeyReused, "该请求标识已用于另一项操作");
                 return;
             }
             readFacts(ctx, Outcome::Replayed, [ctx](Status status) {
                 auto shared = std::make_shared<Status>(std::move(status));
                 if (ctx->op != Op::Add) {
                     ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
                     return;
                 }
                 // D20: a replay only reads the original decision (null for a batch from before 0043); it never writes one.
                 intent::Source source;
                 source.albumBatchId = ctx->ref;
                 intent::readOriginal(
                     ctx->scope, intent::Operation::AlbumPhotosAdd, source,
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
        case Op::Create: return "album.created";
        case Op::Add: return "album.photos_added";
        case Op::Remove: return "album.photo_removed";
        case Op::Cover: return "album.cover_set";
        case Op::Rename: return "album.renamed";
        case Op::Propose: return "album.rename_proposed";
        case Op::Decide: break;
    }
    if (ctx->decision == Decision::Approve) return "album.rename_approved";
    return ctx->decision == Decision::Reject ? "album.rename_rejected" : "album.rename_withdrawn";
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
             if (ctx->op != Op::Add) {
                 commitWith(false, Json::Value());
                 return;
             }
             // D20: one intent per batch (and at most one notice) in this same transaction, after every binding and item.
             intent::Source source;
             source.albumBatchId = ctx->batchId;
             intent::record(
                 ctx->scope, intent::Operation::AlbumPhotosAdd, source, ctx->choice,
                 [commitWith](Json::Value facts) { commitWith(true, std::move(facts)); },
                 [ctx]() { failAt(ctx, "notification_intent", "write_failed"); });
         },
         ctx->userId, ctx->coupleId, std::string(auditAction(ctx)), ctx->albumId, compactJson(detail));
}

// ---- create: identity, text, revision 1.
void createStart(const CtxPtr &ctx) {
    step(ctx, kMembershipSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             classify(ctx, [ctx]() {
                 ctx->albumId = newUuid();
                 step(ctx, kIdentityInsertSql, "insert_identity",
                      [ctx](const drogon::orm::Result &) {
                          step(ctx, kTextInsertSql, "insert_text",
                               [ctx](const drogon::orm::Result &texts) {
                                   step(ctx, kRevisionInsertSql, "insert_revision",
                                        [ctx](const drogon::orm::Result &revisions) {
                                            if (revisions.size() == 0u) {
                                                // A concurrent same-key write committed first; this transaction's rows roll back.
                                                classify(ctx, [ctx]() { failAt(ctx, "insert_revision", "conflict_unexplained"); });
                                                return;
                                            }
                                            ctx->ref = ctx->albumId;
                                            ctx->ref2 = "1";
                                            Json::Value detail(Json::objectValue);
                                            detail["album_id"] = ctx->albumId;
                                            auditAndCommit(ctx, Outcome::Created, detail);
                                        },
                                        ctx->albumId, ctx->coupleId, ctx->userId, texts[0]["id"].as<std::string>(), ctx->key, ctx->fingerprint,
                                        std::string("1"), std::string("create"));
                               },
                               ctx->coupleId, ctx->name);
                      },
                      ctx->albumId, ctx->coupleId, ctx->userId);
             });
         },
         ctx->userId);
}

// ---- add: batch, bindings, items.
void writeAdd(const CtxPtr &ctx) {
    const std::string ids = jsonIds(ctx->mediaIds);
    ctx->batchId = newUuid();
    step(ctx, kBatchInsertSql, "insert_batch",
         [ctx, ids](const drogon::orm::Result &batches) {
             if (batches.size() == 0u) {
                 classify(ctx, [ctx]() { failAt(ctx, "insert_batch", "conflict_unexplained"); });
                 return;
             }
             step(ctx, kBindSql, "bind",
                  [ctx, ids](const drogon::orm::Result &) {
                      if (ctx->fault == AlbumService::Fault::FailAfterBinding) {
                          failAt(ctx, "bind", "forced_fault");
                          return;
                      }
                      step(ctx, kItemsInsertSql, "insert_items",
                           [ctx](const drogon::orm::Result &) {
                               ctx->ref = ctx->batchId;
                               Json::Value detail(Json::objectValue);
                               detail["album_id"] = ctx->albumId;
                               detail["batch_id"] = ctx->batchId;
                               detail["photos"] = static_cast<Json::UInt>(ctx->mediaIds.size());
                               auditAndCommit(ctx, Outcome::Created, detail);
                           },
                           ctx->albumId, ctx->coupleId, ctx->batchId, ctx->userId, ids);
                  },
                  ids, ctx->coupleId, ctx->albumId, ctx->userId);
         },
         ctx->batchId, ctx->albumId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
}

void addChecks(const CtxPtr &ctx) {
    const std::string ids = jsonIds(ctx->mediaIds);
    step(ctx, kPhotoLockSql, "photo_lock",
         [ctx, ids](const drogon::orm::Result &) {
             step(ctx, kPhotoCheckSql, "photo_check",
                  [ctx](const drogon::orm::Result &rows) {
                      if (rows.size() != 1u) { failAt(ctx, "photo_check", "row_count"); return; }
                      if (rows[0]["eligible"].as<size_t>() != ctx->mediaIds.size()) {
                          // A concurrent add with this key may be what took the photos: its replay wins over the refusal.
                          classify(ctx, [ctx]() { reject(ctx, Outcome::PhotoUnavailable, "有照片已不可用，请重新选择"); });
                          return;
                      }
                      writeAdd(ctx);
                  },
                  ids, ctx->coupleId, ctx->userId);
         },
         ids);
}

// ---- remove: design Section 7.4, after classification.
void removeChecks(const CtxPtr &ctx) {
    step(ctx, kItemStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::PhotoNotFound, "相册里没有这张照片"); return; }
             const bool isUploader = rows[0]["added_by"].as<std::string>() == ctx->userId;
             const bool removed = rows[0]["removed"].as<bool>();
             // D6: to the partner a deleted photo is not here. Its uploader may still remove the item, and an undo then respects that.
             if ((removed || rows[0]["deleted"].as<bool>()) && !isUploader) { reject(ctx, Outcome::PhotoNotFound, "相册里没有这张照片"); return; }
             if (!isUploader) { reject(ctx, Outcome::PhotoForbidden, "只有上传者可以移除这张照片"); return; }
             if (removed) { reject(ctx, Outcome::PhotoRemoved, "这张照片已经移除"); return; }
             step(ctx, kRemovalInsertSql, "insert_removal",
                  [ctx](const drogon::orm::Result &inserted) {
                      if (inserted.size() == 0u) {
                          classify(ctx, [ctx]() { failAt(ctx, "insert_removal", "conflict_unexplained"); });
                          return;
                      }
                      ctx->ref = ctx->albumId;
                      ctx->ref2 = ctx->mediaId;
                      Json::Value detail(Json::objectValue);
                      detail["album_id"] = ctx->albumId;
                      detail["media_id"] = ctx->mediaId;
                      auditAndCommit(ctx, Outcome::Ok, detail);
                  },
                  ctx->albumId, ctx->mediaId, ctx->coupleId, ctx->userId, ctx->key, ctx->fingerprint);
         },
         ctx->albumId, ctx->mediaId);
}

// ---- cover: either member, an active item only.
void coverChecks(const CtxPtr &ctx) {
    // D6: 33018 after 33021, so a deletion is entirely before this read or entirely after this cover.
    step(ctx, kPhotoLockSql, "photo_lock",
         [ctx](const drogon::orm::Result &) {
             step(ctx, kItemStateSql, "state",
                  [ctx](const drogon::orm::Result &rows) {
                      if (rows.size() != 1u || rows[0]["removed"].as<bool>() || rows[0]["deleted"].as<bool>()) {
                          reject(ctx, Outcome::PhotoNotFound, "相册里没有这张照片");
                          return;
                      }
                      step(ctx, kCoverInsertSql, "insert_cover",
                           [ctx](const drogon::orm::Result &inserted) {
                               if (inserted.size() == 0u) {
                                   classify(ctx, [ctx]() { failAt(ctx, "insert_cover", "conflict_unexplained"); });
                                   return;
                               }
                               ctx->ref = ctx->albumId;
                               ctx->ref2 = inserted[0]["event_no"].as<std::string>();
                               Json::Value detail(Json::objectValue);
                               detail["album_id"] = ctx->albumId;
                               detail["media_id"] = ctx->mediaId;
                               auditAndCommit(ctx, Outcome::Created, detail);
                           },
                           ctx->albumId, ctx->coupleId, ctx->mediaId, ctx->userId, ctx->key, ctx->fingerprint);
                  },
                  ctx->albumId, ctx->mediaId);
         },
         jsonIds({ctx->mediaId}));
}

// ---- rename and request: design Section 3.1, after classification. A pending request blocks both, which keeps its base current.
void nameChecks(const CtxPtr &ctx) {
    step(ctx, kNameStateSql, "state",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
             const bool isCreator = ctx->creatorId == ctx->userId;
             if (ctx->op == Op::Rename && !isCreator) { reject(ctx, Outcome::EditForbidden, "只有创建者可以直接改名，你可以提交改名申请"); return; }
             if (ctx->op == Op::Propose && isCreator) { reject(ctx, Outcome::EditForbidden, "创建者请直接改名"); return; }
             if (ctx->expectedVersion != rows[0]["version"].as<int>()) { reject(ctx, Outcome::VersionConflict, "相册已经更新，请刷新后再试"); return; }
             if (rows[0]["has_pending"].as<bool>()) {
                 reject(ctx, Outcome::ProposalPending, ctx->op == Op::Rename ? "有待处理的改名申请，请先同意或拒绝" : "已有待处理的改名申请");
                 return;
             }
             if (ctx->name == rows[0]["name"].as<std::string>()) { reject(ctx, Outcome::Validation, "名称没有变化"); return; }
             step(ctx, kTextInsertSql, "insert_text",
                  [ctx](const drogon::orm::Result &texts) {
                      const std::string textId = texts[0]["id"].as<std::string>();
                      if (ctx->op == Op::Rename) {
                          const std::string version = std::to_string(ctx->expectedVersion + 1);
                          step(ctx, kRevisionInsertSql, "insert_revision",
                               [ctx, version](const drogon::orm::Result &revisions) {
                                   if (revisions.size() == 0u) {
                                       classify(ctx, [ctx]() { failAt(ctx, "insert_revision", "conflict_unexplained"); });
                                       return;
                                   }
                                   ctx->ref = ctx->albumId;
                                   ctx->ref2 = version;
                                   Json::Value detail(Json::objectValue);
                                   detail["album_id"] = ctx->albumId;
                                   detail["version"] = ctx->expectedVersion + 1;
                                   auditAndCommit(ctx, Outcome::Created, detail);
                               },
                               ctx->albumId, ctx->coupleId, ctx->userId, textId, ctx->key, ctx->fingerprint, version, std::string("edit"));
                          return;
                      }
                      ctx->proposalId = newUuid();
                      step(ctx, kProposalInsertSql, "insert_proposal",
                           [ctx](const drogon::orm::Result &proposals) {
                               if (proposals.size() == 0u) {
                                   classify(ctx, [ctx]() { failAt(ctx, "insert_proposal", "conflict_unexplained"); });
                                   return;
                               }
                               ctx->ref = ctx->proposalId;
                               ctx->ref2.clear();
                               Json::Value detail(Json::objectValue);
                               detail["album_id"] = ctx->albumId;
                               detail["proposal_id"] = ctx->proposalId;
                               detail["base_version"] = ctx->expectedVersion;
                               auditAndCommit(ctx, Outcome::Created, detail);
                           },
                           ctx->proposalId, ctx->coupleId, ctx->albumId, ctx->userId, std::to_string(ctx->expectedVersion), textId, ctx->key, ctx->fingerprint);
                  },
                  ctx->coupleId, ctx->name);
         },
         ctx->albumId);
}

// Member with album, 33021, classification, then the operation's own checks.
void targetStart(const CtxPtr &ctx) {
    const std::string requested = ctx->albumId;
    step(ctx, kTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["album_id"].isNull()) { reject(ctx, Outcome::NotFound, "相册不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->albumId = rows[0]["album_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             switch (ctx->op) {
                 case Op::Add: ctx->fingerprint = intent::fingerprint(AlbumService::addFingerprint(ctx->albumId, ctx->mediaIds), ctx->choice); break;
                 case Op::Remove: ctx->fingerprint = AlbumService::removeFingerprint(ctx->albumId, ctx->mediaId); break;
                 case Op::Rename: ctx->fingerprint = AlbumService::renameFingerprint(ctx->albumId, ctx->expectedVersion, ctx->name); break;
                 case Op::Propose: ctx->fingerprint = AlbumService::proposeFingerprint(ctx->albumId, ctx->expectedVersion, ctx->name); break;
                 default: ctx->fingerprint = AlbumService::coverFingerprint(ctx->albumId, ctx->mediaId); break;
             }
             step(ctx, kAlbumLockSql, "lock",
                  [ctx](const drogon::orm::Result &) {
                      classify(ctx, [ctx]() {
                          if (ctx->op == Op::Add) addChecks(ctx);
                          else if (ctx->op == Op::Remove) removeChecks(ctx);
                          else if (ctx->op == Op::Rename || ctx->op == Op::Propose) nameChecks(ctx);
                          else coverChecks(ctx);
                      });
                  },
                  ctx->albumId);
         },
         ctx->userId, requested);
}

// ---- decide. Approval and its revision commit together, or neither does. A stale base cannot be reached here, because a pending
// request blocks every revision; the decision guard refuses it if that ever stops being true, and this reports a failure.
void finishDecision(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["album_id"] = ctx->albumId;
    detail["proposal_id"] = ctx->proposalId;
    detail["outcome"] = ctx->decision == Decision::Approve ? "approved" : ctx->decision == Decision::Reject ? "rejected" : "withdrawn";
    auditAndCommit(ctx, Outcome::Ok, detail);
}

void decisionInsert(const CtxPtr &ctx) {
    const std::string outcome = ctx->decision == Decision::Approve ? "approved" : ctx->decision == Decision::Reject ? "rejected" : "withdrawn";
    step(ctx, kDecisionInsertSql, "insert_decision",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() == 0u) {
                 classify(ctx, [ctx]() { reject(ctx, Outcome::ProposalDecided, "这条改名申请已经处理过了"); });
                 return;
             }
             ctx->ref = ctx->proposalId;
             ctx->ref2.clear();
             if (ctx->decision != Decision::Approve) { finishDecision(ctx); return; }
             if (ctx->fault == AlbumService::Fault::FailAfterDecision) {
                 failAt(ctx, "insert_revision", "forced_fault");
                 return;
             }
             step(ctx, kApproveRevisionSql, "insert_revision",
                  [ctx](const drogon::orm::Result &revisions) {
                      if (revisions.size() != 1u) { failAt(ctx, "insert_revision", "row_count"); return; }
                      finishDecision(ctx);
                  },
                  ctx->proposalId, ctx->userId);
         },
         ctx->proposalId, ctx->coupleId, ctx->userId, outcome, ctx->key, ctx->fingerprint);
}

void decisionStart(const CtxPtr &ctx) {
    const std::string requested = ctx->albumId, proposal = ctx->proposalId;
    step(ctx, kDecisionTargetSql, "membership",
         [ctx](const drogon::orm::Result &rows) {
             if (rows.size() != 1u) { reject(ctx, Outcome::SpaceNotFound, "尚未加入空间"); return; }
             if (rows[0]["album_id"].isNull()) { reject(ctx, Outcome::NotFound, "相册不存在"); return; }
             if (rows[0]["proposal_id"].isNull()) { reject(ctx, Outcome::ProposalNotFound, "改名申请不存在"); return; }
             ctx->coupleId = rows[0]["couple_id"].as<std::string>();
             ctx->albumId = rows[0]["album_id"].as<std::string>();
             ctx->creatorId = rows[0]["creator_id"].as<std::string>();
             ctx->proposalId = rows[0]["proposal_id"].as<std::string>();
             ctx->proposerId = rows[0]["proposer_id"].as<std::string>();
             ctx->fingerprint = AlbumService::decisionFingerprint(ctx->albumId, ctx->proposalId, ctx->decision);
             step(ctx, kAlbumLockSql, "lock",
                  [ctx](const drogon::orm::Result &) {
                      classify(ctx, [ctx]() {
                          if (ctx->decision == Decision::Withdraw ? ctx->userId != ctx->proposerId : ctx->userId != ctx->creatorId) {
                              reject(ctx, Outcome::EditForbidden, ctx->decision == Decision::Withdraw ? "只有提出申请的一方可以撤回" : "只有创建者可以处理改名申请");
                              return;
                          }
                          step(ctx, kDecidedSql, "state",
                               [ctx](const drogon::orm::Result &decided) {
                                   if (decided.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
                                   if (decided[0]["decided"].as<bool>()) { reject(ctx, Outcome::ProposalDecided, "这条改名申请已经处理过了"); return; }
                                   decisionInsert(ctx);
                               },
                               ctx->proposalId);
                      });
                  },
                  ctx->albumId);
         },
         ctx->userId, requested, proposal);
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

CtxPtr makeCtx(Op op, const std::string &userId, const std::string &key, AlbumService::Callback cb) {
    auto ctx = std::make_shared<Ctx>();
    ctx->op = op;
    ctx->userId = userId;
    ctx->key = key;
    ctx->done = std::move(cb);
    return ctx;
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

AlbumService &AlbumService::instance() {
    static AlbumService service;
    return service;
}

bool AlbumService::parseCreateRequest(const Json::Value &body, std::string &name, std::string &message) {
    if (!exactFields(body, "name", message)) return false;
    return normalizeName(body["name"], name, message);
}

bool AlbumService::parseRevisionRequest(const Json::Value &body, int &expectedVersion, std::string &name, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是 JSON 对象";
        return false;
    }
    for (const auto &member : body.getMemberNames()) {
        if (member != "expected_version" && member != "name") {
            message = "请求体包含不允许的字段";
            return false;
        }
    }
    if (!body.isMember("expected_version") || !body.isMember("name")) {
        message = "缺少字段 expected_version 或 name";
        return false;
    }
    const auto &v = body["expected_version"];
    if (!((v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt()) || v.asInt() < 1) {
        message = "expected_version 必须是正整数";
        return false;
    }
    expectedVersion = v.asInt();
    return normalizeName(body["name"], name, message);
}

bool AlbumService::parseDecisionRequest(const Json::Value &body, Decision &decision, std::string &message) {
    if (!exactFields(body, "outcome", message)) return false;
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

std::string AlbumService::renameFingerprint(const std::string &albumId, int expectedVersion, const std::string &name) {
    return crypto::sha256Hex("album-rename:v1:" + field("album_id", albumId) + field("expected_version", std::to_string(expectedVersion)) + field("name", name));
}

std::string AlbumService::proposeFingerprint(const std::string &albumId, int expectedVersion, const std::string &name) {
    return crypto::sha256Hex("album-propose:v1:" + field("album_id", albumId) + field("expected_version", std::to_string(expectedVersion)) + field("name", name));
}

std::string AlbumService::decisionFingerprint(const std::string &albumId, const std::string &proposalId, Decision decision) {
    const char *name = decision == Decision::Approve ? "approve" : decision == Decision::Reject ? "reject" : "withdraw";
    return crypto::sha256Hex("album-decide:v1:" + field("album_id", albumId) + field("proposal_id", proposalId) + field("outcome", name));
}

bool AlbumService::parseAddRequest(const Json::Value &body, std::vector<std::string> &mediaIds, std::string &message) {
    if (!exactFields(body, "media_ids", message)) return false;
    const auto &photos = body["media_ids"];
    if (!photos.isArray() || photos.empty() || photos.size() > kMaxPhotosPerAdd) {
        message = "每次需要添加 1 到 20 张照片";
        return false;
    }
    std::set<std::string> seen;
    mediaIds.clear();
    for (const auto &v : photos) {
        const std::string id = v.isString() ? lower(v.asString()) : std::string();
        if (!TaskService::isCanonicalUuid(id) || !seen.insert(id).second) {
            message = "media_ids 必须是不重复的 uuid";
            return false;
        }
        mediaIds.push_back(id);
    }
    return true;
}

bool AlbumService::parseCoverRequest(const Json::Value &body, std::string &mediaId, std::string &message) {
    if (!exactFields(body, "media_id", message)) return false;
    const auto &v = body["media_id"];
    mediaId = v.isString() ? lower(v.asString()) : std::string();
    if (!TaskService::isCanonicalUuid(mediaId)) {
        message = "media_id 必须是 uuid";
        return false;
    }
    return true;
}

std::string AlbumService::createFingerprint(const std::string &name) { return crypto::sha256Hex("album-create:v1:" + field("name", name)); }

std::string AlbumService::addFingerprint(const std::string &albumId, const std::vector<std::string> &mediaIds) {
    std::string canonical = field("album_id", albumId);
    for (const auto &id : mediaIds) canonical += field("media", id);
    return crypto::sha256Hex("album-add:v1:" + canonical);
}

std::string AlbumService::removeFingerprint(const std::string &albumId, const std::string &mediaId) {
    return crypto::sha256Hex("album-remove:v1:" + field("album_id", albumId) + field("media", mediaId));
}

std::string AlbumService::coverFingerprint(const std::string &albumId, const std::string &mediaId) {
    return crypto::sha256Hex("album-cover:v1:" + field("album_id", albumId) + field("media", mediaId));
}

std::string AlbumService::encodeCursor(const Cursor &cursor) { return cursor.createdAt + "_" + cursor.id; }

// Strictly the server's own form: timestamp_uuid, so nothing else reaches the statement.
std::optional<AlbumService::Cursor> AlbumService::decodeCursor(const std::string &raw) {
    if (raw.size() > TaskService::kMaxCursorLength) return std::nullopt;
    const auto split = raw.find('_');
    if (split == std::string::npos) return std::nullopt;
    Cursor cursor{raw.substr(0, split), lower(raw.substr(split + 1))};
    if (!TaskService::isCanonicalUuid(cursor.id)) return std::nullopt;
    // YYYY-MM-DDTHH:MM:SS.ffffffZ, as this service writes it.
    const auto &t = cursor.createdAt;
    if (t.size() != 27u || t[10] != 'T' || t[19] != '.' || t[26] != 'Z') return std::nullopt;
    const auto date = AnniversaryCalendar::parse(t.substr(0, 10));
    if (!date || AnniversaryCalendar::format(*date) != t.substr(0, 10)) return std::nullopt;
    for (size_t i = 11; i < 26; ++i) {
        const bool punct = i == 13 || i == 16 || i == 19;
        if (punct ? (i == 19 ? t[i] != '.' : t[i] != ':') : !std::isdigit(static_cast<unsigned char>(t[i]))) return std::nullopt;
    }
    if (t.substr(11, 2) > "23" || t.substr(14, 2) > "59" || t.substr(17, 2) > "60") return std::nullopt;
    return cursor;
}

// A positive decimal seq with no sign, spaces or leading zero.
std::optional<int> AlbumService::decodePhotoCursor(const std::string &raw) {
    if (raw.empty() || raw.size() > 10u || raw[0] == '0') return std::nullopt;
    long long value = 0;
    for (const char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    if (value > 2147483647LL) return std::nullopt;
    return static_cast<int>(value);
}

Json::Value AlbumService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    if (status.proposal.isObject()) {
        body["proposal"] = status.proposal;
        return body;
    }
    if (status.mutation.isObject()) body["mutation"] = status.mutation;
    body["album"] = status.album;
    if (status.hasNotification) body["notification"] = status.notification;
    return body;
}

ErrorCode AlbumService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::NotFound: return ErrorCode::AlbumNotFound;
        case Outcome::PhotoUnavailable: return ErrorCode::AlbumPhotoUnavailable;
        case Outcome::PhotoNotFound: return ErrorCode::AlbumPhotoNotFound;
        case Outcome::PhotoForbidden: return ErrorCode::AlbumPhotoForbidden;
        case Outcome::PhotoRemoved: return ErrorCode::AlbumPhotoRemoved;
        case Outcome::EditForbidden: return ErrorCode::AlbumEditForbidden;
        case Outcome::VersionConflict: return ErrorCode::AlbumVersionConflict;
        case Outcome::ProposalPending: return ErrorCode::AlbumProposalPending;
        case Outcome::ProposalNotFound: return ErrorCode::AlbumProposalNotFound;
        case Outcome::ProposalDecided: return ErrorCode::AlbumProposalDecided;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        default: return ErrorCode::InternalError;
    }
}

int AlbumService::httpStatusFor(Outcome outcome) { return outcome == Outcome::Created ? 201 : 200; }

// =====================================================================
// Actions
// =====================================================================

void AlbumService::create(const std::string &userId, const std::string &key, const std::string &name, Callback cb) {
    auto ctx = makeCtx(Op::Create, userId, key, std::move(cb));
    ctx->name = name;
    ctx->fingerprint = createFingerprint(name);
    run(ctx, createStart);
}

void AlbumService::addPhotos(const std::string &userId, const std::string &albumId, const std::string &key, const std::vector<std::string> &mediaIds, Callback cb,
                             Fault fault, intent::Choice choice) {
    auto ctx = makeCtx(Op::Add, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->mediaIds = mediaIds;
    ctx->fault = fault;
    ctx->choice = choice;
    run(ctx, targetStart);
}

void AlbumService::removePhoto(const std::string &userId, const std::string &albumId, const std::string &mediaId, const std::string &key, Callback cb) {
    auto ctx = makeCtx(Op::Remove, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->mediaId = lower(mediaId);
    run(ctx, targetStart);
}

void AlbumService::setCover(const std::string &userId, const std::string &albumId, const std::string &mediaId, const std::string &key, Callback cb) {
    auto ctx = makeCtx(Op::Cover, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->mediaId = lower(mediaId);
    run(ctx, targetStart);
}

void AlbumService::rename(const std::string &userId, const std::string &albumId, const std::string &key, int expectedVersion, const std::string &name, Callback cb) {
    auto ctx = makeCtx(Op::Rename, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->expectedVersion = expectedVersion;
    ctx->name = name;
    run(ctx, targetStart);
}

void AlbumService::propose(const std::string &userId, const std::string &albumId, const std::string &key, int expectedVersion, const std::string &name, Callback cb) {
    auto ctx = makeCtx(Op::Propose, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->expectedVersion = expectedVersion;
    ctx->name = name;
    run(ctx, targetStart);
}

void AlbumService::decide(const std::string &userId, const std::string &albumId, const std::string &proposalId, const std::string &key, Decision decision, Callback cb,
                          Fault fault) {
    auto ctx = makeCtx(Op::Decide, userId, key, std::move(cb));
    ctx->albumId = albumId;
    ctx->proposalId = proposalId;
    ctx->decision = decision;
    ctx->fault = fault;
    run(ctx, decisionStart);
}

void AlbumService::getProposal(const std::string &userId, const std::string &albumId, const std::string &proposalId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kProposalSql,
        [done](const drogon::orm::Result &rows) {
            Status status = makeStatus(Outcome::ProposalNotFound, "改名申请不存在");
            if (rows.size() == 1u && parseObject(rows[0]["proposal_json"].as<std::string>(), status.proposal)) status.outcome = Outcome::Ok;
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            Status status = makeStatus(Outcome::Error);
            status.detail = TaskService::failureDetail("album_proposal", "read", "db_error");
            (*done)(std::move(status));
        },
        userId, albumId, proposalId);
}

void AlbumService::listProposals(const std::string &userId, const std::string &albumId, int limit, const std::optional<Cursor> &after, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::NotFound;
            result.message = "相册不存在";
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
                    result.nextCursor = encodeCursor({last["sort_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                Json::Value item;
                if (!parseObject(row["proposal_json"].as<std::string>(), item)) throw std::runtime_error("proposal json");
                result.items.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("album_proposals", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("album_proposals", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (after) {
        client->execSqlAsync(kProposalsAfterSql, std::move(onRows), std::move(onError), userId, albumId, after->createdAt, after->id,
                             std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kProposalsFirstSql, std::move(onRows), std::move(onError), userId, albumId, std::to_string(limit + 1));
    }
}

void AlbumService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb) {
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
                if (row["album_json"].isNull()) continue;
                if (kept == static_cast<size_t>(limit)) {
                    const auto &last = rows[kept - 1];
                    result.nextCursor = encodeCursor({last["sort_created_at"].as<std::string>(), last["sort_id"].as<std::string>()});
                    break;
                }
                Json::Value item;
                if (!parseObject(row["album_json"].as<std::string>(), item)) throw std::runtime_error("album json");
                result.items.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("album_list", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("album_list", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (cursor) {
        client->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId, cursor->createdAt, cursor->id, std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId, std::to_string(limit + 1));
    }
}

void AlbumService::get(const std::string &userId, const std::string &albumId, Callback cb) {
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            Status status = makeStatus(Outcome::NotFound, "相册不存在");
            if (rows.size() == 1u && parseObject(rows[0]["album_json"].as<std::string>(), status.album)) status.outcome = Outcome::Ok;
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            Status status = makeStatus(Outcome::Error);
            status.detail = TaskService::failureDetail("album_get", "read", "db_error");
            (*done)(std::move(status));
        },
        userId, albumId);
}

void AlbumService::listPhotos(const std::string &userId, const std::string &albumId, int limit, const std::optional<int> &beforeSeq, ListCallback cb) {
    auto done = std::make_shared<ListCallback>(std::move(cb));
    auto onRows = [done, limit](const drogon::orm::Result &rows) {
        ListResult result;
        if (rows.size() == 0u) {
            result.outcome = Outcome::NotFound;
            result.message = "相册不存在";
            (*done)(std::move(result));
            return;
        }
        try {
            result.outcome = Outcome::Ok;
            size_t kept = 0;
            for (const auto &row : rows) {
                if (row["photo_json"].isNull()) continue;
                if (kept == static_cast<size_t>(limit)) {
                    result.nextCursor = std::to_string(rows[kept - 1]["sort_seq"].as<int>());
                    break;
                }
                Json::Value item;
                if (!parseObject(row["photo_json"].as<std::string>(), item)) throw std::runtime_error("photo json");
                result.items.push_back(std::move(item));
                ++kept;
            }
        } catch (const std::exception &) {
            ListResult failed;
            failed.detail = TaskService::failureDetail("album_photos", "read", "column_format");
            (*done)(std::move(failed));
            return;
        }
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult failed;
        failed.detail = TaskService::failureDetail("album_photos", "read", "db_error");
        (*done)(std::move(failed));
    };
    const auto client = db::Pool::instance().client();
    if (beforeSeq) {
        client->execSqlAsync(kPhotosAfterSql, std::move(onRows), std::move(onError), userId, albumId, std::to_string(*beforeSeq), std::to_string(limit + 1));
    } else {
        client->execSqlAsync(kPhotosFirstSql, std::move(onRows), std::move(onError), userId, albumId, std::to_string(limit + 1));
    }
}

}  // namespace usapp
