// =====================================================================
// services/api/src/services/ProductService.cc
// =====================================================================
#include "services/ProductService.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <exception>
#include <memory>
#include <set>
#include <utility>

#include "core/Crypto.h"
#include "db/Db.h"
#include "services/TaskService.h"

namespace usapp {

namespace {

// ---------------------------------------------------------------- SQL
#define USAPP_TS(expr) "to_char((" expr ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')"

// Current product: immutable identity joined to its highest revision.
#define USAPP_PRODUCT_COLUMNS                                                                          \
    "p.id::text AS id, p.creator_id::text AS creator_id, p.owner_id::text AS owner_id, r.title, "      \
    "r.description, r.price_flowers, r.redemption_mode, r.status, r.version, "                          \
    USAPP_TS("p.created_at") " AS created_at, " USAPP_TS("r.created_at") " AS updated_at"
#define USAPP_LATEST_REVISION                                                                          \
    "JOIN LATERAL (SELECT x.* FROM reward_product_revisions x WHERE x.product_id = p.id "              \
    "ORDER BY x.version DESC LIMIT 1) r ON true"
// D14: the creator sees active and archived products; the owner sees active ones only.
#define USAPP_VISIBLE "(p.creator_id = $1::uuid OR (p.owner_id = $1::uuid AND r.status = 'active'))"

// Pages in the immutable (created_at DESC, id DESC) order; visibility is applied on
// every page. First page: $1 user, $2 limit+1. Next pages: $2 created_at, $3 id, $4 limit+1.
#define USAPP_LIST_SQL(predicate, limit)                                                               \
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "                               \
    "LEFT JOIN LATERAL (SELECT " USAPP_PRODUCT_COLUMNS ", p.created_at AS sort_ca, p.id AS sort_id "  \
    "FROM reward_products p " USAPP_LATEST_REVISION " WHERE p.couple_id = m.couple_id AND " USAPP_VISIBLE \
    predicate " ORDER BY p.created_at DESC, p.id DESC LIMIT " limit "::integer) x ON true "           \
    "WHERE m.user_id = $1::uuid ORDER BY x.sort_ca DESC, x.sort_id DESC"

const char *kListFirstSql = USAPP_LIST_SQL("", "$2");
const char *kListAfterSql = USAPP_LIST_SQL(" AND (p.created_at, p.id) < ($2::timestamptz, $3::uuid)", "$4");

#undef USAPP_LIST_SQL

const char *kGetSql =
    "SELECT m.couple_id::text AS couple_id, x.* FROM couple_members m "
    "LEFT JOIN LATERAL (SELECT " USAPP_PRODUCT_COLUMNS " FROM reward_products p " USAPP_LATEST_REVISION
    " WHERE p.couple_id = m.couple_id AND p.id = $2::uuid AND " USAPP_VISIBLE ") x ON true "
    "WHERE m.user_id = $1::uuid";

// Read back inside a mutation transaction (the creator always sees the product).
const char *kProductByIdSql =
    "SELECT " USAPP_PRODUCT_COLUMNS " FROM reward_products p " USAPP_LATEST_REVISION " WHERE p.id = $1::uuid";

// Immutable facts of one revision.
const char *kMutationSql =
    "SELECT json_build_object('action', r.action, 'version', r.version, 'actor_id', r.actor_id, "
    "'title', r.title, 'description', r.description, 'price_flowers', r.price_flowers, "
    "'redemption_mode', r.redemption_mode, 'status', r.status, "
    "'created_at', " USAPP_TS("r.created_at") ")::text AS mutation_json "
    "FROM reward_product_revisions r WHERE r.product_id = $1::uuid AND r.version = $2::integer";

// Create step 1: caller and partner; the caller's member row FOR SHARE.
const char *kMembershipSql =
    "SELECT m.couple_id::text AS couple_id, "
    "       (SELECT q.user_id::text FROM couple_members q "
    "         WHERE q.couple_id = m.couple_id AND q.user_id <> m.user_id ORDER BY q.slot LIMIT 1) AS partner_id "
    "FROM couple_members m WHERE m.user_id = $1::uuid FOR SHARE OF m";

// Edit/archive step 1: membership FOR SHARE joined to a product of the same space.
const char *kMutationTargetSql =
    "SELECT m.couple_id::text AS couple_id, p.id::text AS product_id, p.creator_id::text AS creator_id "
    "FROM couple_members m LEFT JOIN reward_products p ON p.couple_id = m.couple_id AND p.id = $2::uuid "
    "WHERE m.user_id = $1::uuid FOR SHARE OF m";

const char *kLockSql = "SELECT pg_advisory_xact_lock(33007::integer, hashtext($1::text))";

const char *kByKeySql =
    "SELECT product_id::text AS product_id, version, request_fingerprint FROM reward_product_revisions "
    "WHERE actor_id = $1::uuid AND idempotency_key = $2";

const char *kLatestSql =
    "SELECT version, status FROM reward_product_revisions WHERE product_id = $1::uuid "
    "ORDER BY version DESC LIMIT 1";

const char *kProductInsertSql =
    "INSERT INTO reward_products (id, couple_id, creator_id, owner_id) "
    "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid)";

// Archive copies the current terms (the database guard verifies that).
const char *kRevisionInsertSql =
    "INSERT INTO reward_product_revisions (product_id, couple_id, version, actor_id, action, title, description, "
    "price_flowers, redemption_mode, status, idempotency_key, request_fingerprint) "
    "SELECT $1::uuid, $2::uuid, $3::integer, $4::uuid, $5::text, "
    "CASE WHEN $5::text = 'archive' THEN prev.title ELSE $6::text END, "
    "CASE WHEN $5::text = 'archive' THEN prev.description ELSE $7::text END, "
    "CASE WHEN $5::text = 'archive' THEN prev.price_flowers ELSE NULLIF($8::text, '')::integer END, "
    "CASE WHEN $5::text = 'archive' THEN prev.redemption_mode ELSE $9::text END, "
    "CASE WHEN $5::text = 'archive' THEN 'archived' ELSE 'active' END, $10::text, $11::text "
    "FROM (SELECT 1) one LEFT JOIN reward_product_revisions prev "
    "ON prev.product_id = $1::uuid AND prev.version = $3::integer - 1 "
    "ON CONFLICT DO NOTHING RETURNING version";

const char *kAuditSql =
    "INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, $2::uuid, $3::text, 'reward_product', $4::text, $5::jsonb)";

#undef USAPP_VISIBLE
#undef USAPP_LATEST_REVISION
#undef USAPP_PRODUCT_COLUMNS
#undef USAPP_TS

bool isStrictInteger(const Json::Value &v) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) && v.isInt();
}

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

ProductService::ProductData productFromRow(const drogon::orm::Row &row) {
    ProductService::ProductData p;
    p.id = row["id"].as<std::string>();
    p.creatorId = row["creator_id"].as<std::string>();
    p.ownerId = row["owner_id"].as<std::string>();
    p.title = row["title"].as<std::string>();
    p.description = row["description"].as<std::string>();
    p.priceFlowers = row["price_flowers"].as<int>();
    p.redemptionMode = row["redemption_mode"].as<std::string>();
    p.status = row["status"].as<std::string>();
    p.version = row["version"].as<int>();
    p.createdAt = row["created_at"].as<std::string>();
    p.updatedAt = row["updated_at"].as<std::string>();
    return p;
}

ProductService::Status makeStatus(ProductService::Outcome outcome, std::string message = "",
                                  std::string detail = "") {
    ProductService::Status status;
    status.outcome = outcome;
    status.message = std::move(message);
    status.detail = std::move(detail);
    return status;
}

std::string label(ProductService::Action action, const char *stage, const char *reason) {
    const std::string op = std::string("product_") + ProductService::actionName(action);
    return TaskService::failureDetail(op, stage, reason);
}

// Text and price rules shared by create and edit (task title/description rules).
bool parseTermsFields(const Json::Value &body, ProductService::Terms &out, std::string &message) {
    if (!body.isMember("title") || !body["title"].isString()) {
        message = "名称必须是字符串";
        return false;
    }
    switch (TaskService::normalizeTitle(body["title"].asString(), out.title)) {
        case TaskService::TextError::None:
            break;
        case TaskService::TextError::InvalidUtf8:
            message = "名称包含无法识别的字符";
            return false;
        case TaskService::TextError::ControlCharacter:
            message = "名称不能包含换行或控制字符";
            return false;
        case TaskService::TextError::Empty:
            message = "名称不能为空";
            return false;
        case TaskService::TextError::TooLong:
            message = "名称最多 40 个字符";
            return false;
    }
    out.description.clear();
    if (body.isMember("description")) {
        if (!body["description"].isString()) {
            message = "描述必须是字符串";
            return false;
        }
        switch (TaskService::normalizeDescription(body["description"].asString(), out.description)) {
            case TaskService::TextError::None:
            case TaskService::TextError::Empty:
                break;
            case TaskService::TextError::InvalidUtf8:
                message = "描述包含无法识别的字符";
                return false;
            case TaskService::TextError::ControlCharacter:
                message = "描述只允许普通换行，不能包含其他控制字符";
                return false;
            case TaskService::TextError::TooLong:
                message = "描述最多 500 个字符";
                return false;
        }
    }
    if (!body.isMember("price_flowers") || !isStrictInteger(body["price_flowers"]) ||
        body["price_flowers"].asInt() < ProductService::kMinPriceFlowers ||
        body["price_flowers"].asInt() > ProductService::kMaxPriceFlowers) {
        message = "价格必须是 1 到 999 之间的整数（小红花）";
        return false;
    }
    out.priceFlowers = body["price_flowers"].asInt();
    const std::string mode = body["redemption_mode"].isString() ? body["redemption_mode"].asString() : "";
    if (mode != "direct" && mode != "request") {
        message = "兑换方式必须是 direct 或 request";
        return false;
    }
    out.redemptionMode = mode;
    return true;
}

bool onlyFields(const Json::Value &body, const std::set<std::string> &allowed, std::string &message) {
    if (!body.isObject()) {
        message = "请求体必须是一个 JSON 对象";
        return false;
    }
    for (const auto &name : body.getMemberNames()) {
        if (allowed.find(name) == allowed.end()) {
            message = "请求体包含不支持的字段";
            return false;
        }
    }
    return true;
}

bool parseExpectedVersion(const Json::Value &body, int &out, std::string &message) {
    if (!body.isMember("expected_version") || !isStrictInteger(body["expected_version"]) ||
        body["expected_version"].asInt() < 1) {
        message = "expected_version 必须是正整数";
        return false;
    }
    out = body["expected_version"].asInt();
    return true;
}

// ---------------------------------------------------------------------
// Lifetime: MutationCtx --(shared)--> TxScope --(onError_, weak)--> MutationCtx, as in
// the task services. Every end goes through complete() exactly once; commitRequested
// is set before commit, after which an error is ResultUnknown.
// ---------------------------------------------------------------------
struct MutationCtx {
    std::shared_ptr<db::TxScope> scope;
    ProductService::Action action = ProductService::Action::Create;
    std::string userId;
    std::string productId;  // canonical from the database (or generated for create)
    std::string coupleId;
    std::string ownerId;
    std::string key;
    std::string fingerprint;
    ProductService::Terms terms;
    int expectedVersion = 0;
    int version = 0;  // the revision written
    ProductService::Status result;
    ProductService::Callback done;
    bool completed = false;
    bool commitRequested = false;
    std::string failDetail;
};

using CtxPtr = std::shared_ptr<MutationCtx>;

void complete(const CtxPtr &ctx, ProductService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "ProductService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) ctx->done(std::move(status));
}

void failAt(const CtxPtr &ctx, const char *stage, const char *reason) {
    ctx->failDetail = label(ctx->action, stage, reason);
    ctx->scope->fail(ctx->failDetail);
}

void rejectWith(const CtxPtr &ctx, ProductService::Status status) {
    auto shared = std::make_shared<ProductService::Status>(std::move(status));
    ctx->scope->rollback([ctx, shared]() { complete(ctx, std::move(*shared)); });
}

// Immutable revision facts plus the current product, inside the transaction.
void readFacts(const CtxPtr &ctx, const std::string &productId, int version, ProductService::Outcome outcome,
               std::function<void(ProductService::Status)> next) {
    ctx->scope->tx().execSqlAsync(
        kMutationSql,
        [ctx, productId, outcome, next = std::move(next)](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "read_mutation", "row_count"); return; }
            auto status = std::make_shared<ProductService::Status>(makeStatus(outcome));
            try {
                if (!parseObject(rows[0]["mutation_json"].as<std::string>(), status->mutation)) {
                    failAt(ctx, "read_mutation", "json_format");
                    return;
                }
            } catch (const std::exception &) { failAt(ctx, "read_mutation", "column_format"); return; }
            std::string id = productId;
            ctx->scope->tx().execSqlAsync(
                kProductByIdSql,
                [ctx, status, next](const drogon::orm::Result &products) {
                    if (products.size() != 1u) { failAt(ctx, "read_product", "row_count"); return; }
                    try { status->product = productFromRow(products[0]); }
                    catch (const std::exception &) { failAt(ctx, "read_product", "column_format"); return; }
                    next(std::move(*status));
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_product", "db_error"); },
                id);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "read_mutation", "db_error"); },
        productId, std::to_string(version));
}

// Same actor and key: an identical request replays its committed revision (read-only).
void classify(const CtxPtr &ctx, std::function<void()> onMissing) {
    ctx->scope->tx().execSqlAsync(
        kByKeySql,
        [ctx, onMissing = std::move(onMissing)](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { onMissing(); return; }
            std::string productId;
            std::string stored;
            int version = 0;
            try {
                productId = rows[0]["product_id"].as<std::string>();
                version = rows[0]["version"].as<int>();
                stored = rows[0]["request_fingerprint"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "classify", "column_format"); return; }
            if (stored != ctx->fingerprint) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::KeyReused, "该请求标识已用于另一项操作"));
                return;
            }
            readFacts(ctx, productId, version, ProductService::Outcome::Replayed,
                      [ctx](ProductService::Status status) { rejectWith(ctx, std::move(status)); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "classify", "db_error"); },
        ctx->userId, ctx->key);
}

void commitWithFacts(const CtxPtr &ctx, ProductService::Outcome outcome) {
    readFacts(ctx, ctx->productId, ctx->version, outcome, [ctx](ProductService::Status status) {
        ctx->result = std::move(status);
        ctx->commitRequested = true;
        ctx->scope->commit([ctx]() { complete(ctx, std::move(ctx->result)); });
    });
}

void auditAndCommit(const CtxPtr &ctx) {
    Json::Value detail(Json::objectValue);
    detail["product_id"] = ctx->productId;
    detail["version"] = ctx->version;
    if (ctx->action != ProductService::Action::Archive) {
        detail["price_flowers"] = ctx->terms.priceFlowers;
        detail["redemption_mode"] = ctx->terms.redemptionMode;
    }
    const char *auditAction = ctx->action == ProductService::Action::Create ? "reward_product.created"
                            : ctx->action == ProductService::Action::Edit   ? "reward_product.edited"
                                                                            : "reward_product.archived";
    ctx->scope->tx().execSqlAsync(
        kAuditSql,
        [ctx](const drogon::orm::Result &) {
            commitWithFacts(ctx, ctx->action == ProductService::Action::Create ? ProductService::Outcome::Created
                                                                               : ProductService::Outcome::Updated);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "audit", "db_error"); },
        ctx->userId, ctx->coupleId, std::string(auditAction), ctx->productId, compactJson(detail));
}

void insertRevision(const CtxPtr &ctx) {
    const bool archive = ctx->action == ProductService::Action::Archive;
    std::string title = archive ? std::string() : ctx->terms.title;
    std::string description = archive ? std::string() : ctx->terms.description;
    std::string price = archive ? std::string() : std::to_string(ctx->terms.priceFlowers);
    std::string mode = archive ? std::string() : ctx->terms.redemptionMode;
    ctx->scope->tx().execSqlAsync(
        kRevisionInsertSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                // A concurrent same-key request on another product committed first.
                classify(ctx, [ctx]() { failAt(ctx, "revision", "conflict_unexplained"); });
                return;
            }
            auditAndCommit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "revision", "db_error"); },
        ctx->productId, ctx->coupleId, std::to_string(ctx->version), ctx->userId,
        std::string(ProductService::actionName(ctx->action)), title, description, price, mode, ctx->key,
        ctx->fingerprint);
}

// ---- create ----
void createInsert(const CtxPtr &ctx) {
    ctx->productId = newUuid();
    ctx->version = 1;
    ctx->scope->tx().execSqlAsync(
        kProductInsertSql,
        [ctx](const drogon::orm::Result &) { insertRevision(ctx); },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "product", "db_error"); },
        ctx->productId, ctx->coupleId, ctx->userId, ctx->ownerId);
}

void createMembership(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kMembershipSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u || rows[0]["couple_id"].isNull()) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["partner_id"].isNull()) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::PartnerRequired, "需要对方加入空间后才能上架奖励"));
                return;
            }
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->ownerId = rows[0]["partner_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            classify(ctx, [ctx]() { createInsert(ctx); });
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId);
}

// ---- edit / archive ----
void mutationState(const CtxPtr &ctx, const std::string &creatorId) {
    ctx->scope->tx().execSqlAsync(
        kLatestSql,
        [ctx, creatorId](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) { failAt(ctx, "state", "row_count"); return; }
            int current = 0;
            std::string status;
            try {
                current = rows[0]["version"].as<int>();
                status = rows[0]["status"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "state", "column_format"); return; }
            if (creatorId != ctx->userId) {
                // The owner sees active products only; an archived one stays not found.
                if (status == "active") {
                    rejectWith(ctx, makeStatus(ProductService::Outcome::Forbidden, "只有上架这件奖励的一方可以修改或下架"));
                } else {
                    rejectWith(ctx, makeStatus(ProductService::Outcome::NotFound, "奖励不存在"));
                }
                return;
            }
            if (status == "archived") {
                rejectWith(ctx, makeStatus(ProductService::Outcome::Archived, "奖励已下架，不能再修改"));
                return;
            }
            if (current != ctx->expectedVersion) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::VersionConflict, "奖励已被修改，请刷新后再操作"));
                return;
            }
            ctx->version = current + 1;
            insertRevision(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "state", "db_error"); },
        ctx->productId);
}

void mutationTarget(const CtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kMutationTargetSql,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::SpaceNotFound, "尚未加入空间"));
                return;
            }
            if (rows.size() != 1u) { failAt(ctx, "membership", "row_shape"); return; }
            if (rows[0]["product_id"].isNull()) {
                rejectWith(ctx, makeStatus(ProductService::Outcome::NotFound, "奖励不存在"));
                return;
            }
            std::string creatorId;
            try {
                ctx->coupleId = rows[0]["couple_id"].as<std::string>();
                ctx->productId = rows[0]["product_id"].as<std::string>();  // canonical lock key
                creatorId = rows[0]["creator_id"].as<std::string>();
            } catch (const std::exception &) { failAt(ctx, "membership", "column_format"); return; }
            ctx->fingerprint = ProductService::fingerprint(ctx->action, ctx->productId, ctx->expectedVersion, ctx->terms);
            ctx->scope->tx().execSqlAsync(
                kLockSql,
                [ctx, creatorId](const drogon::orm::Result &) {
                    // Replay first: a committed request replays even after later edits or archive.
                    classify(ctx, [ctx, creatorId]() { mutationState(ctx, creatorId); });
                },
                [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "lock", "db_error"); },
                ctx->productId);
        },
        [ctx](const drogon::orm::DrogonDbException &) { failAt(ctx, "membership", "db_error"); },
        ctx->userId, ctx->productId);
}

void runMutation(const CtxPtr &ctx, void (*first)(const CtxPtr &)) {
    std::weak_ptr<MutationCtx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx, first](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            first(ctx);
        },
        [weakCtx](const std::string &) {
            // The message may contain database text; only fixed labels are reported.
            if (auto strong = weakCtx.lock()) {
                if (strong->commitRequested) {
                    complete(strong, makeStatus(ProductService::Outcome::ResultUnknown, "",
                                                label(strong->action, "commit", "commit_unconfirmed")));
                } else if (!strong->failDetail.empty()) {
                    complete(strong, makeStatus(ProductService::Outcome::Error, "", strong->failDetail));
                } else {
                    complete(strong, makeStatus(ProductService::Outcome::Error, "",
                                                label(strong->action, "transaction", "begin_failed")));
                }
                return;
            }
            LOG_ERROR << "ProductService 事务错误回调触发时上下文已释放 stage=transaction";
        });
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================

ProductService &ProductService::instance() {
    static ProductService service;
    return service;
}

bool ProductService::parseCreateRequest(const Json::Value &body, Terms &out, std::string &message) {
    out = Terms{};
    if (!onlyFields(body, {"title", "description", "price_flowers", "redemption_mode"}, message)) return false;
    return parseTermsFields(body, out, message);
}

bool ProductService::parseEditRequest(const Json::Value &body, Terms &out, int &expectedVersion,
                                      std::string &message) {
    out = Terms{};
    if (!onlyFields(body, {"title", "description", "price_flowers", "redemption_mode", "expected_version"}, message)) {
        return false;
    }
    return parseExpectedVersion(body, expectedVersion, message) && parseTermsFields(body, out, message);
}

bool ProductService::parseArchiveRequest(const Json::Value &body, int &expectedVersion, std::string &message) {
    if (!onlyFields(body, {"expected_version"}, message)) return false;
    return parseExpectedVersion(body, expectedVersion, message);
}

const char *ProductService::actionName(Action action) {
    switch (action) {
        case Action::Create: return "create";
        case Action::Edit: return "edit";
        case Action::Archive: return "archive";
    }
    return "create";
}

std::string ProductService::fingerprint(Action action, const std::string &productId, int expectedVersion,
                                        const Terms &terms) {
    auto field = [](const char *name, const std::string &value) {
        return std::string(name) + "=" + std::to_string(value.size()) + ":" + value + ";";
    };
    std::string canonical = std::string("reward-product-") + actionName(action) + ":v1:";
    if (action != Action::Create) {
        canonical += field("product_id", productId) + field("expected_version", std::to_string(expectedVersion));
    }
    if (action != Action::Archive) {
        canonical += field("title", terms.title) + field("description", terms.description) +
                     field("price_flowers", std::to_string(terms.priceFlowers)) +
                     field("redemption_mode", terms.redemptionMode);
    }
    return crypto::sha256Hex(canonical);
}

Json::Value ProductService::productJson(const ProductData &p) {
    Json::Value out(Json::objectValue);
    out["id"] = p.id;
    out["creator_id"] = p.creatorId;
    out["owner_id"] = p.ownerId;
    out["title"] = p.title;
    out["description"] = p.description;
    out["price_flowers"] = p.priceFlowers;
    out["redemption_mode"] = p.redemptionMode;
    out["status"] = p.status;
    out["version"] = p.version;
    out["created_at"] = p.createdAt;
    out["updated_at"] = p.updatedAt;
    return out;
}

Json::Value ProductService::responseBody(const Status &status) {
    Json::Value body(Json::objectValue);
    body["mutation"] = status.mutation;
    body["product"] = productJson(status.product);
    return body;
}

ErrorCode ProductService::errorCodeFor(Outcome outcome) {
    switch (outcome) {
        case Outcome::Validation: return ErrorCode::ValidationError;
        case Outcome::SpaceNotFound: return ErrorCode::SpaceNotFound;
        case Outcome::PartnerRequired: return ErrorCode::PartnerRequired;
        case Outcome::NotFound: return ErrorCode::ProductNotFound;
        case Outcome::Forbidden: return ErrorCode::ProductForbidden;
        case Outcome::Archived: return ErrorCode::ProductArchived;
        case Outcome::VersionConflict: return ErrorCode::ProductVersionConflict;
        case Outcome::KeyReused: return ErrorCode::IdempotencyKeyReused;
        case Outcome::ResultUnknown: return ErrorCode::ResultUnknown;
        case Outcome::Created:
        case Outcome::Updated:
        case Outcome::Replayed:
        case Outcome::Ok:
        case Outcome::Error:
            break;
    }
    return ErrorCode::InternalError;
}

int ProductService::httpStatusFor(Outcome outcome) {
    if (outcome == Outcome::Created) return 201;
    if (outcome == Outcome::Updated || outcome == Outcome::Replayed || outcome == Outcome::Ok) return 200;
    return defaultStatusFor(errorCodeFor(outcome));
}

// =====================================================================
// Actions
// =====================================================================

void ProductService::create(const std::string &userId, const std::string &key, const Terms &terms, Callback cb) {
    if (userId.empty() || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求标识不合法", label(Action::Create, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(Action::Create, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<MutationCtx>();
    ctx->action = Action::Create;
    ctx->userId = userId;
    ctx->key = key;
    ctx->terms = terms;
    ctx->fingerprint = fingerprint(Action::Create, "", 0, terms);
    ctx->done = std::move(cb);
    runMutation(ctx, &createMembership);
}

void ProductService::edit(const std::string &userId, const std::string &productId, const std::string &key,
                          int expectedVersion, const Terms &terms, Callback cb) {
    if (userId.empty() || productId.empty() || expectedVersion < 1 || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Action::Edit, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(Action::Edit, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<MutationCtx>();
    ctx->action = Action::Edit;
    ctx->userId = userId;
    ctx->productId = productId;
    ctx->key = key;
    ctx->expectedVersion = expectedVersion;
    ctx->terms = terms;
    ctx->done = std::move(cb);
    runMutation(ctx, &mutationTarget);
}

void ProductService::archive(const std::string &userId, const std::string &productId, const std::string &key,
                             int expectedVersion, Callback cb) {
    if (userId.empty() || productId.empty() || expectedVersion < 1 || !TaskService::validIdempotencyKey(key)) {
        cb(makeStatus(Outcome::Validation, "请求不合法", label(Action::Archive, "input", "invalid_request")));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", label(Action::Archive, "pool", "not_ready")));
        return;
    }
    auto ctx = std::make_shared<MutationCtx>();
    ctx->action = Action::Archive;
    ctx->userId = userId;
    ctx->productId = productId;
    ctx->key = key;
    ctx->expectedVersion = expectedVersion;
    ctx->done = std::move(cb);
    runMutation(ctx, &mutationTarget);
}

std::string ProductService::encodeCursor(const Cursor &cursor) {
    return cursor.createdAt + "_" + cursor.id;
}

std::optional<ProductService::Cursor> ProductService::decodeCursor(const std::string &raw) {
    // 27-character timestamp, '_', 36-character uuid.
    if (raw.size() != 64u || raw[27] != '_') return std::nullopt;
    Cursor cursor{raw.substr(0, 27), raw.substr(28)};
    if (!TaskService::isCursorTimestamp(cursor.createdAt) || !TaskService::isCanonicalUuid(cursor.id)) {
        return std::nullopt;
    }
    return cursor;
}

void ProductService::list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb) {
    ListResult early;
    if (userId.empty() || limit < 1 || limit > TaskService::kMaxPageSize) {
        early.outcome = Outcome::Validation;
        early.message = "请求参数不合法";
        cb(std::move(early));
        return;
    }
    if (!db::Pool::instance().ready()) {
        early.outcome = Outcome::Error;
        early.detail = TaskService::failureDetail("product_list", "pool", "not_ready");
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
                if (row["id"].isNull()) continue;  // member without (more) visible products
                result.products.push_back(productFromRow(row));
            }
        } catch (const std::exception &) {
            result = ListResult{};
            result.outcome = Outcome::Error;
            result.detail = TaskService::failureDetail("product_list", "query", "column_format");
            (*done)(std::move(result));
            return;
        }
        // limit + 1 rows were requested: an extra row means another page exists.
        if (result.products.size() > static_cast<size_t>(limit)) {
            result.products.resize(static_cast<size_t>(limit));
            const auto &last = result.products.back();
            result.nextCursor = encodeCursor(Cursor{last.createdAt, last.id});
        }
        result.outcome = Outcome::Ok;
        (*done)(std::move(result));
    };
    auto onError = [done](const drogon::orm::DrogonDbException &) {
        ListResult result;
        result.outcome = Outcome::Error;
        result.detail = TaskService::failureDetail("product_list", "query", "db_error");
        (*done)(std::move(result));
    };
    std::string fetch = std::to_string(limit + 1);
    if (cursor) {
        std::string createdAt = cursor->createdAt;
        std::string id = cursor->id;
        db::Pool::instance().client()->execSqlAsync(kListAfterSql, std::move(onRows), std::move(onError), userId,
                                                  createdAt, id, fetch);
    } else {
        db::Pool::instance().client()->execSqlAsync(kListFirstSql, std::move(onRows), std::move(onError), userId,
                                                  fetch);
    }
}

void ProductService::get(const std::string &userId, const std::string &productId, Callback cb) {
    if (userId.empty() || productId.empty()) {
        cb(makeStatus(Outcome::Validation, "请求参数不合法"));
        return;
    }
    if (!db::Pool::instance().ready()) {
        cb(makeStatus(Outcome::Error, "", TaskService::failureDetail("product_get", "pool", "not_ready")));
        return;
    }
    auto done = std::make_shared<Callback>(std::move(cb));
    db::Pool::instance().client()->execSqlAsync(
        kGetSql,
        [done](const drogon::orm::Result &rows) {
            if (rows.size() == 0u) { (*done)(makeStatus(Outcome::SpaceNotFound, "尚未加入空间")); return; }
            if (rows.size() != 1u) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("product_get", "query", "row_count")));
                return;
            }
            if (rows[0]["id"].isNull()) { (*done)(makeStatus(Outcome::NotFound, "奖励不存在")); return; }
            Status status = makeStatus(Outcome::Ok);
            try { status.product = productFromRow(rows[0]); }
            catch (const std::exception &) {
                (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("product_get", "query", "column_format")));
                return;
            }
            (*done)(std::move(status));
        },
        [done](const drogon::orm::DrogonDbException &) {
            (*done)(makeStatus(Outcome::Error, "", TaskService::failureDetail("product_get", "query", "db_error")));
        },
        userId, productId);
}

}  // namespace usapp
