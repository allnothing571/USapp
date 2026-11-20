// =====================================================================
// services/api/src/services/MessageService.cc
// =====================================================================
#include "services/MessageService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// Current message with its reply (if any).
#define USAPP_MESSAGE_COLUMNS                                                                            \
    "d.id::text AS id, d.author_id::text AS author_id, d.recipient_id::text AS recipient_id, d.body, "  \
    USAPP_TS("d.created_at") " AS created_at, r.author_id::text AS reply_author_id, "                   \
    "r.body AS reply_body, " USAPP_TS("r.created_at") " AS reply_created_at"
#define USAPP_MESSAGE_FROM "messages d LEFT JOIN message_replies r ON r.message_id = d.id"

// Both members see every message of their space. $2 = partner only (messages written to
// the caller). Pages in (created_at DESC, id DESC). First page: $1 user, $2 partnerOnly,
// $3 limit+1. Next pages: $3 created_at, $4 id, $5 limit+1.
#define USAPP_LIST_SQL(predicate, limit)                                                               \
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "                               \
    "LEFT JOIN LATERAL (SELECT " USAPP_MESSAGE_COLUMNS ", d.created_at AS sort_ca, d.id AS sort_id "  \
    "FROM " USAPP_MESSAGE_FROM " WHERE d.couple_id = m.couple_id "                                     \
    "AND (NOT $2::boolean OR d.recipient_id = $1::uuid)" predicate                                    \
    " ORDER BY d.created_at DESC, d.id DESC LIMIT " limit "::integer) x ON true "                      \
    "WHERE m.user_id = $1::uuid ORDER BY x.sort_ca DESC, x.sort_id DESC"

const char *kListFirstSql = USAPP_LIST_SQL("", "$3");
const char *kListAfterSql = USAPP_LIST_SQL(" AND (d.created_at, d.id) < ($3::timestamptz, $4::uuid)", "$5");

#undef USAPP_LIST_SQL

const char *kGetSql =
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "
    "LEFT JOIN LATERAL (SELECT " USAPP_MESSAGE_COLUMNS " FROM " USAPP_MESSAGE_FROM
    " WHERE d.couple_id = m.couple_id AND d.id = $2::uuid) x ON true "
    "WHERE m.user_id = $1::uuid";

const char *kMessageByIdSql = "SELECT " USAPP_MESSAGE_COLUMNS " FROM " USAPP_MESSAGE_FROM " WHERE d.id = $1::uuid";

// Immutable facts of a write.
const char *kCreateFactsSql =
    "SELECT json_build_object('action', 'create', 'message_id', d.id, 'author_id', d.author_id, "
    "'recipient_id', d.recipient_id, 'body', d.body, 'created_at', " USAPP_TS("d.created_at") ")::text "
    "AS mutation_json FROM messages d WHERE d.id = $1::uuid";
const char *kReplyFactsSql =
    "SELECT json_build_object('action', 'reply', 'message_id', r.message_id, 'author_id', r.author_id, "
    "'body', r.body, 'created_at', " USAPP_TS("r.created_at") ")::text "
    "AS mutation_json FROM message_replies r WHERE r.message_id = $1::uuid";

// Create step 1: caller and partner; the caller's member row FOR SHARE.
const char *kMembershipSql =
    "SELECT m.couple_id::text AS couple_id, "
    "       (SELECT q.user_id::text FROM couple_members q "
    "         WHERE q.couple_id = m.couple_id AND q.user_id <> m.user_id ORDER BY q.slot LIMIT 1) AS partner_id "
    "FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";

// Reply step 1: membership FOR SHARE joined to a message of the same space.
const char *kReplyTargetSql =
    "SELECT m.couple_id::text AS couple_id, d.id::text AS message_id, d.author_id::text AS author_id, "
    "d.recipient_id::text AS recipient_id FROM couple_members m "
    "LEFT JOIN messages d ON d.couple_id = m.couple_id AND d.id = $2::uuid "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kCreateByKeySql =
    "SELECT id::text AS id, request_fingerprint FROM messages WHERE author_id = $1::uuid AND idempotency_key = $2";
const char *kReplyByKeySql =
    "SELECT message_id::text AS id, request_fingerprint FROM message_replies "
    "WHERE author_id = $1::uuid AND idempotency_key = $2";

const char *kMessageInsertSql =
    "INSERT INTO messages (id, couple_id, author_id, recipient_id, body, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::text, $6::text, $7::text) "
    "ON CONFLICT DO NOTHING RETURNING id::text AS id";
const char *kReplyInsertSql =
    "INSERT INTO message_replies (message_id, couple_id, author_id, body, idempotency_key, request_fingerprint) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::text, $5::text, $6::text) "
    "ON CONFLICT DO NOTHING RETURNING message_id::text AS id";

// The audit detail holds identifiers and the length only, never the text.
const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'message', $4::text, $5::jsonb)";

#undef USAPP_MESSAGE_FROM
#undef USAPP_MESSAGE_COLUMNS
#undef USAPP_TS

std::string newUuid() {
    std::string hex = crypto::randomHex(16);
    hex[12] = '4';
    const char variant[] = {'8', '9', 'a', 'b'};
    hex[16] = variant[static_cast<unsigned char>(hex[16]) % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20, 12);
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

size_t codePoints(const std::string &utf8) {
    size_t n = 0;
    for (unsigned char c : utf8) n += (c & 0xC0u) != 0x80u ? 1u : 0u;
    return n;
}

MessageService::MessageData messageFromRow(const drogon::orm::Row &row) {
    MessageService::MessageData m;
    m.id = row["id"].as<std::string>();
    m.authorId = row["author_id"].as<std::string>();
    m.recipientId = row["recipient_id"].as<std::string>();
    m.body = row["body"].as<std::string>();
    m.createdAt = row["created_at"].as<std::string>();
    m.hasReply = !row["reply_author_id"].isNull();
    if (m.hasReply) {
        m.replyAuthorId = row["reply_author_id"].as<std::string>();
        m.replyBody = row["reply_body"].as<std::string>();
        m.replyCreatedAt = row["reply_created_at"].as<std::string>();
    }
    return m;
}

MessageService::Status makeStatus(MessageService::Outcome outcome, std::string message = "",
                                  std::string detail = "") {
    MessageService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

// Shared body parsing: the request is exactly {"body": string}; description rules apply.
bool parseText(const Json::Value &body, bool reply, std::string &out, std::string &message) {
    if (!body.isObject() || body.size() != 1u || !body.isMember("body") || !body["body"].isString()) {
        message = "请求体只能包含字符串字段 body";
        return false;
    }
    const char *what = reply ? "回复" : "留言";
    switch (TaskService::normalizeDescription(body["body"].asString(), out)) {
        case TaskService::TextError::None:
        case TaskService::TextError::Empty:
            break;
        case TaskService::TextError::InvalidUtf8:
            message = std::string(what) + "包含无法识别的字符";
            return false;
        case TaskService::TextError::ControlCharacter:
            message = std::string(what) + "不能包含控制字符";
            return false;
        case TaskService::TextError::TooLong:
            message = reply ? "回复最多 100 个字符" : "留言最多 500 个字符";
            return false;
    }
    if (out.empty()) {
        message = std::string(what) + "不能为空";
        return false;
    }
    if (reply && out.find('\n') != std::string::npos) {
        message = "回复只能是一行";
        return false;
    }
    const size_t limit = reply ? MessageService::kMaxReplyCodePoints : MessageService::kMaxBodyCodePoints;
    if (codePoints(out) > limit) {
        message = reply ? "回复最多 100 个字符" : "留言最多 500 个字符";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Lifetime: Ctx --(shared)--> TxScope --(onError_, weak)--> Ctx, as in the other
// services. Every end goes through complete() exactly once; commitRequested is set
// before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
struct Ctx {
    std::shared_ptr<db::TxScope> scope;
    bool replying = false;
    std::string userId;
    std::string coupleId;
    std::string partnerId;
    std::string messageId;
    std::string key;
    std::string text;
    std::string fingerprint;
    intent::Choice choice;     // P2-3b
    Json::Value notification;  // original notification facts of this write
    MessageService::Status result;
    MessageService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<Ctx>;

std::string label(bool replying, const char *stage, const char *reason) {
    return TaskService::failureDetail(replying ? "message_reply" : "message_create", stage, reason);
}

void complete(const CtxPtr &ctx, MessageService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "MessageService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->replying, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, MessageService::Status status) {
    auto shared = std::make_shared<MessageService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

// Immutable facts of the write plus the current message, inside the transaction.
void readFacts(const CtxPtr &ctx, const std::string &messageId, MessageService::Outcome outcome,
               std::function<void(MessageService::Status)> next) {
    ctx->scope->tx().execSqlAsync(
        ctx->replying ? kReplyFactsSql : kCreateFactsSql,
        [ctx, messageId, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_mutation", "row_count"); return; }
            auto status = std::make_shared<MessageService::Status>(makeStatus(outcome));
            try {
                if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) {
                    failAt(ctx, "read_mutation", "json_format");
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "read_mutation", "column_format"); return; }
            std::string id = messageId;
            ctx->scope->tx().execSqlAsync(
                kMessageByIdSql,
                [ctx, status, next](const drogon::orm::Result &messages) {
                    if (messages.size() != 1u) { failAt(ctx, "read_message", "row_count"); return; }
                    try { status->data = messageFromRow(messages[0]); }
                    catch (const std::exception &) { failAt(ctx, "read_message", "column_format"); return; }
                    next(std::move(*status));
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_message", "db_error"); },
                id);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_mutation", "db_error"); },
        messageId);
}

// Same author and key: an identical request replays its committed write (read-only).
// P2-3b: the typed intent source of this write (the message, or its reply row).
intent::Operation intentOperation(const CtxPtr &ctx) {
    return ctx->replying ? intent::Operation::MessageReply : intent::Operation::MessageCreate;
}

intent::Source intentSource(const CtxPtr &ctx, const std::string &messageId) {
    intent::Source source;
    if (ctx->replying) source.replyMessageId = messageId;
    else source.messageId = messageId;
    return source;
}

void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        ctx->replying ? kReplyByKeySql : kCreateByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string id;
            std::string stored;
            try {
                id = rows[0]["id"].as<std::string>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(MessageService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            readFacts(ctx, id, MessageService::Outcome::Replayed, [ctx, id](MessageService::Status status) {
                // A replay only reads the original decision; it never appends an intent or notice.
                auto shared = std::make_shared<MessageService::Status>(std::move(status));
                intent::readOriginal(
                    ctx->scope, intentOperation(ctx), intentSource(ctx, id),
                    [ctx, shared](Json::Value facts) {
                        shared->hasNotification = true;
                        shared->notification = std::move(facts);
                        rejectWith(ctx, std::move(*shared));
                    },
                    [ctx]() { failAt(ctx, "classify", "intent_read_failed"); });
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void auditAndCommit(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["message_id"] = ctx->messageId;
    detail["length"] = static_cast<Json::UInt64>(codePoints(ctx->text));
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx](const drogon::orm::Result &) {
            // Intent (and, when eligible, preference and notice) in this same transaction.
            intent::record(
                ctx->scope, intentOperation(ctx), intentSource(ctx, ctx->messageId), ctx->choice,
                [ctx](Json::Value facts) {
                    ctx->notification = std::move(facts);
                    readFacts(ctx, ctx->messageId, MessageService::Outcome::Created,
                              [ctx](MessageService::Status status) {
                                  status.hasNotification = true;
                                  status.notification = ctx->notification;
                                  ctx->result = std::move(status);
                                  ctx->commitRequested = true;
                                  ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
                              });
                },
                [ctx]() { failAt(ctx, "notification_intent", "write_failed"); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId, ctx->coupleId, std::string(ctx->replying ? "message.replied" : "message.created"),
        ctx->messageId, compactJson(detail));
}

// ---- create ----
void createInsert(const CtxPtr &ctx) {
    ctx->messageId = newUuid();
    ctx->scope->tx().execSqlAsync(
        kMessageInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // A concurrent same-key request committed first.
                classify(ctx, [ctx]() { failAt(ctx, "insert", "conflict_unexplained"); });
                return;
            }
            auditAndCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
        ctx->messageId, ctx->coupleId, ctx->userId, ctx->partnerId, ctx->text, ctx->key, ctx->fingerprint);
}

void createMembership(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kMembershipSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(MessageService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u || rows[0]["couple_id"].isNull()) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["partner_id"].isNull()) {
                rejectWith(ctx, makeStatus(MessageService::Outcome::PartnerRequired, "需要对方加入空间后才能留言"));
                return;
            }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->partnerId = rows[0]["partner_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            classify(ctx, [ctx]() { createInsert(ctx); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId);
}

// ---- reply ----
void replyInsert(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kReplyInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // Either a concurrent same-key reply committed, or another reply already exists.
                classify(ctx, [ctx]() {
                    rejectWith(ctx, makeStatus(MessageService::Outcome::AlreadyReplied, "这条留言已经回复过了"));
                });
                return;
            }
            auditAndCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "insert", "db_error"); },
        ctx->messageId, ctx->coupleId, ctx->userId, ctx->text, ctx->key, ctx->fingerprint);
}

void replyTarget(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kReplyTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(MessageService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["message_id"].isNull()) {
                rejectWith(ctx, makeStatus(MessageService::Outcome::NotFound, "留言不存在"));
                return;
            }
            std::string recipientId;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->messageId = rows[0]["message_id"].as<std::string>();  // canonical form
                recipientId = rows[0]["recipient_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint =
                intent::fingerprint(MessageService::replyFingerprint(ctx->messageId, ctx->text), ctx->choice);
            // Replay first: a committed reply replays for its author.
            classify(ctx, [ctx, recipientId]() {
                if (recipientId != ctx->userId) {
                    rejectWith(ctx, makeStatus(MessageService::Outcome::Forbidden, "只有收到留言的一方可以回复"));
                    return;
                }
                replyInsert(ctx);
            });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->messageId);
}

void run(const CtxPtr &ctx, void (*first)(const CtxPtr &)) {
    std::weak_ptr<Ctx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx, first](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            first(ctx);
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(MessageService::Outcome::ResultUnknown, "",
                                                label(strong->replying, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(MessageService::Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(MessageService::Outcome::Error, "",
                                                label(strong->replying, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "MessageService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

MessageService &MessageService::instance() {
    static MessageService service;
    return service;
}

bool MessageService::parseMessageRequest(const Json::Value &body, std::string &text, std::string &message) {
    return parseText(body, false, text, message);
}

bool MessageService::parseReplyRequest(const Json::Value &body, std::string &text, std::string &message) {
    return parseText(body, true, text, message);
}

std::string MessageService::createFingerprint(const std::string &text) {
    return crypto::sha256Hex("message-create:v1:body=" + std::to_string(text.size()) + ":" + text + ";");
}

std::string MessageService::replyFingerprint(const std::string &messageId, const std::string &text) {
    return crypto::sha256Hex("message-reply:v1:message_id=" + std::to_string(messageId.size()) + ":" + messageId +
                             ";body=" + std::to_string(text.size()) + ":" + text + ";");
}

std::string MessageService::encodeCursor(const Cursor &cursor) {
    return cursor.createdAt + "_" + cursor.id;
}

std::optional<MessageService::Cursor> MessageService::decodeCursor(const std::string &raw) {
    if (raw.size() != 64u || raw[27] != '_') return std::nullopt;
    Cursor cursor{raw.substr(0, 27), raw.substr(28)};
    if (!TaskService::isCursorTimestamp(cursor.createdAt) || !TaskService::isCanonicalUuid(cursor.id)) {
        return std::nullopt;
    }
    return cursor;
}

Json::Value MessageService::messageJson(const MessageData &m) {
    Json::Value out(Json::objectValue);
    out["id"] = m.id;
    out["author_id"] = m.authorId;
    out["recipient_id"] = m.recipientId;
    out["body"] = m.body;
    out["created_at"] = m.createdAt;
    if (m.hasReply) {
        Json::Value reply(Json::objectValue);
        reply["author_id"] = m.replyAuthorId;
        reply["body"] = m.replyBody;
        reply["created_at"] = m.replyCreatedAt;
        out["reply"] = reply;
    } else {
        out["reply"] = Json::Value(Json::nullValue);
    }
    return out;
}

Json::Value MessageService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["mutation"] = status.mutation;
    body["message"] = messageJson(status.data);
    if (status.hasNotification) body["notification"] = status.notification;
    return body;
}

ErrorCode MessageService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::PartnerRequired: return ErrorCode::PartnerRequired;
        case Outcome::NotFound: return ErrorCode::MessageNotFound;
        case Outcome::Forbidden: return ErrorCode::MessageReplyForbidden;
        case Outcome::AlreadyReplied: return ErrorCode::MessageAlreadyReplied;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::Created:
        case Outcome::Replayed:
        case Outcome::Ok:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int MessageService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Created) return 201;
    if (outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void MessageService::create(const std::string &userId, const std::string &key, const std::string &text,
                            Callback cb, intent::Choice choice) {
    if (userId.empty() || text.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(false, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(false, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->userId = userId;
    ctx->key = key;
    ctx->text = text;
    ctx->fingerprint = intent::fingerprint(createFingerprint(text), choice);
    ctx->choice = choice;
    ctx->done = std::move(cb);
    run(ctx, &createMembership);
}

void MessageService::reply(const std::string &userId, const std::string &messageId, const std::string &key,
                           const std::string &text, Callback cb, intent::Choice choice) {
    if (userId.empty() || messageId.empty() || text.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(true, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(true, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<Ctx>();
    ctx->replying = true;
    ctx->userId = userId;
    ctx->messageId = messageId;
    ctx->key = key;
    ctx->text = text;
    ctx->choice = choice;
    ctx->done = std::move(cb);
    run(ctx, &replyTarget);
}

void MessageService::list(const std::string &userId, bool partnerOnly, int limit, const std::optional<Cursor> &cursor,
                          ListCallback cb) {
    ListResult early;
    if (userId.empty() || limit < 1 || limit > TaskService::kMaxPageSize) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = TaskService::failureDetail("message_list", "pool", "not_ready");
        cb(std::move(early));
        return;
    }
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
            for (const auto &row : rows) {
                if (row["id"].isNull()) continue;  // member without (more) messages
                result.messages.push_back(messageFromRow(row));
            }
        } catch (const std::exception &) {
            result = ListResult{};
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("message_list", "query", "column_format");
            (*done)(std::move(result));
            return;
        }
        // limit + 1 rows were requested: an extra row means another page exists.
        if (result.messages.size() > static_cast<size_t>(limit)) {
            result.messages.resize(static_cast<size_t>(limit));
            const auto &last = result.messages.back();
            result.nextCursor = encodeCursor(Cursor{last.createdAt, last.id});
        }
        result.outcome = Outcome::Ok;
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult result;
        result.outcome = Outcome::Error;
        result.detail = TaskService::failureDetail("message_list", "query", "db_error");
        (*done)(std::move(result));
    };
    std::string partner = partnerOnly ? "true" : "false";
    std::string fetch = std::to_string(limit + 1);
    if (cursor) {
        std::string createdAt = cursor->createdAt;
        std::string id = cursor->id;
        db::Pool::instance().client()->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId,
                                                  partner, createdAt, id, fetch);
    } else {
        db::Pool::instance().client()->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId,
                                                  partner, fetch);
    }
}

void MessageService::get(const std::string &userId, const std::string &messageId, Callback cb) {
    if (userId.empty() || messageId.empty()) {
        cb(makeStatus(Outcome::Validation, "请求参数不合法"));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", TaskService::failureDetail("message_get", "pool", "not_ready")));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { (*done)(makeStatus(Outcome::SpaceNotFound, "尚未加入空间")); return; }
            if (rows.size() != 1u) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("message_get", "query", "row_count")));
                return;
            }
            if (rows[0]["id"].isNull()) { (*done)(makeStatus(Outcome::NotFound, "留言不存在")); return; }
            Status status = makeStatus(Outcome::Ok);
            try { status.data = messageFromRow(rows[0]); }
            catch (const std::exception &) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("message_get", "query", "column_format")));
                return;
            }
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("message_get", "query", "db_error")));
        },
        userId, messageId);
}

}  // namespace usapp
