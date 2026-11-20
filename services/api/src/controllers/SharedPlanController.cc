#include <drogon/HttpController.h>

#include "db/Db.h"
#include "http/AuthFilter.h"
#include "http/ControllerSupport.h"
#include "http/HttpUtil.h"
#include "services/AlbumService.h"
#include "services/MemoryService.h"
#include "services/NotificationIntent.h"
#include "services/TaskService.h"

namespace usapp::controllers {
namespace {
using Reply = std::function<void(const drogon::HttpResponsePtr &)>;

bool parseObject(const std::string &raw, Json::Value &value) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(raw.data(), raw.data() + raw.size(), &value, nullptr) && value.isObject();
}

constexpr auto kListSql = R"sql(
    SELECT p.id::text AS id,
           to_char(p.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') AS cursor_time,
           usapp_shared_plan_read(p.id)::text AS facts
    FROM couple_members m
    JOIN shared_plans p ON p.couple_id = m.couple_id
    WHERE m.user_id = $1::uuid
      AND (NULLIF($2, '') IS NULL OR (p.created_at, p.id) < (NULLIF($2, '')::timestamptz, NULLIF($3, '')::uuid))
    ORDER BY p.created_at DESC, p.id DESC
    LIMIT $4::integer
)sql";

void write(const drogon::HttpRequestPtr &req, const Reply &reply, const std::string &plan,
           const std::string &operation, const std::string &proposal = {}) {
    std::string actor;
    if (!ctrl::requireUser(req, reply, actor)) return;
    if ((operation != "create" && !ctrl::looksLikeUuid(plan)) ||
        (operation == "decide" && !ctrl::looksLikeUuid(proposal))) {
        reply(ctrl::fail(ErrorCode::NotFound, "计划或请求不存在", req));
        return;
    }
    const auto key = req->getHeader("idempotency-key");
    Json::Value body;
    if (!TaskService::validIdempotencyKey(key)) {
        reply(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key", req));
        return;
    }
    if (!ctrl::parseBody(req, reply, body)) return;
    // D20: only plan creation may notify. The optional notify_partner leaves the body before the stored request is built, so omitted
    // and false are one request, as in P2-3b. Every other plan operation keeps refusing an unknown field.
    intent::Choice choice;
    if (operation == "create") {
        std::string message;
        if (!intent::takeChoice(body, choice, message)) {
            reply(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
    }
    if (operation == "link_memory" && (body.size() != 1 || !body["memory_id"].isString() ||
                                      !ctrl::looksLikeUuid(body["memory_id"].asString()))) {
        reply(ctrl::fail(ErrorCode::ValidationError, "请求必须包含有效的 memory_id", req));
        return;
    }
    if (operation == "decide") {
        if (body.isMember("proposal_id")) {
            reply(ctrl::fail(ErrorCode::ValidationError, "请求正文不能包含 proposal_id", req));
            return;
        }
        // PostgreSQL serializes UUIDs in lowercase. Canonicalize before classifying the key.
        std::string canonical = proposal;
        for (auto &c : canonical) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        body["proposal_id"] = canonical;
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    auto payload = Json::writeString(writer, body);
    const auto unknown = [req, reply](const std::string &) {
        reply(ctrl::fail(ErrorCode::ResultUnknown, "结果未确认，请使用相同的请求重试", req));
    };
    db::TxScope::beginAsync([req, reply, actor, plan, operation, key, payload, choice](const std::shared_ptr<db::TxScope> &tx) {
        tx->tx().execSqlAsync(
            "SELECT (CASE $3::text WHEN 'link_memory' THEN "
            "usapp_plan_memory_attach($1::uuid, NULLIF($2::text,'')::uuid, $4, ($5::jsonb->>'memory_id')::uuid) "
            "ELSE usapp_shared_plan_write($1::uuid, NULLIF($2::text,'')::uuid, $3, $4, $5::jsonb) END)::text AS facts",
            [tx, req, reply, operation, choice](const drogon::orm::Result &rows) {
                Json::Value result;
                if (rows.size() != 1 || !parseObject(rows[0]["facts"].as<std::string>(), result)) {
                    tx->fail("invalid shared plan receipt");
                    return;
                }
                if (result.isMember("error")) {
                    tx->rollback([req, reply, result]() {
                        const auto error = result["error"].asString();
                        ErrorCode code = ErrorCode::ValidationError;
                        if (error == "not_found") code = ErrorCode::NotFound;
                        else if (error == "forbidden") code = ErrorCode::PlanEditForbidden;
                        else if (error == "conflict") code = ErrorCode::PlanConflict;
                        else if (error == "key_reused") code = ErrorCode::IdempotencyKeyReused;
                        reply(ctrl::fail(code, "计划操作未通过，请检查权限、状态或提交内容", req));
                    });
                    return;
                }
                if (operation != "create") {
                    tx->commit([reply, result, operation]() {
                        reply(jsonResponse(result, operation == "create" && !result["replayed"].asBool() ? 201 : 200));
                    });
                    return;
                }
                intent::Source source;
                source.sharedPlanId = result["plan"]["id"].asString();
                if (!result["replayed"].asBool()) {
                    // A new plan: its intent (and, when requested with a partner, the one notice) in this same transaction.
                    intent::record(
                        tx, intent::Operation::SharedPlanCreate, source, choice,
                        [tx, reply, result](Json::Value facts) mutable {
                            result["notification"] = std::move(facts);
                            tx->commit([reply, result]() { reply(jsonResponse(result, 201)); });
                        },
                        [tx]() { tx->fail("shared plan notification intent failed"); });
                    return;
                }
                // A replay only reads the original decision. The stored request omits the choice, so a different requested
                // choice under the same key is key reuse here, as a different fingerprint is in P2-3b; omitted and false agree.
                intent::readOriginal(
                    tx, intent::Operation::SharedPlanCreate, source,
                    [tx, req, reply, result, choice](Json::Value facts) mutable {
                        const bool originalRequested = facts.isObject() && facts["requested"].asBool();
                        if (originalRequested != choice.requested()) {
                            tx->rollback([req, reply]() {
                                reply(ctrl::fail(ErrorCode::IdempotencyKeyReused, "计划操作未通过，请检查权限、状态或提交内容", req));
                            });
                            return;
                        }
                        result["notification"] = std::move(facts);
                        tx->commit([reply, result]() { reply(jsonResponse(result, 200)); });
                    },
                    [tx]() { tx->fail("shared plan notification intent read failed"); });
            },
            [tx](const drogon::orm::DrogonDbException &) { tx->fail("shared plan write failed"); },
            actor, plan, operation, key, payload);
    }, unknown);
}
}  // namespace

class SharedPlanController : public drogon::HttpController<SharedPlanController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SharedPlanController::create, "/api/v1/shared-plans", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::list, "/api/v1/shared-plans", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::detail, "/api/v1/shared-plans/{planId}", drogon::Get, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::edit, "/api/v1/shared-plans/{planId}/revisions", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::propose, "/api/v1/shared-plans/{planId}/proposals", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::decide, "/api/v1/shared-plans/{planId}/proposals/{proposalId}/decision", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::advance, "/api/v1/shared-plans/{planId}/status", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::linkMemory, "/api/v1/shared-plans/{planId}/memory-link", drogon::Post, AuthFilter::classTypeName());
    ADD_METHOD_TO(SharedPlanController::createMemory, "/api/v1/shared-plans/{planId}/memory", drogon::Post, AuthFilter::classTypeName());
    METHOD_LIST_END

    void create(const drogon::HttpRequestPtr &req, Reply &&reply) { write(req, reply, {}, "create"); }
    void edit(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) { write(req, reply, plan, "edit"); }
    void propose(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) { write(req, reply, plan, "propose"); }
    void decide(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan, std::string proposal) {
        write(req, reply, plan, "decide", proposal);
    }
    void advance(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) { write(req, reply, plan, "advance"); }
    void linkMemory(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) { write(req, reply, plan, "link_memory"); }

    void createMemory(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) {
        std::string actor;
        if (!ctrl::requireUser(req, reply, actor)) return;
        if (!ctrl::looksLikeUuid(plan)) {
            reply(ctrl::fail(ErrorCode::NotFound, "计划不存在", req));
            return;
        }
        const auto key = req->getHeader("idempotency-key");
        if (!TaskService::validIdempotencyKey(key)) {
            reply(ctrl::fail(ErrorCode::ValidationError, "缺少或无效的 Idempotency-Key", req));
            return;
        }
        Json::Value body;
        if (!ctrl::parseBody(req, reply, body)) return;
        MemoryService::Terms terms;
        std::string message;
        if (!MemoryService::parseCreateRequest(body, terms, message)) {
            reply(ctrl::fail(ErrorCode::ValidationError, message, req));
            return;
        }
        MemoryService::instance().createForPlan(actor, plan, key, terms, [req, reply](MemoryService::Status status) {
            if (status.success()) {
                reply(jsonResponse(MemoryService::responseBody(status), MemoryService::httpStatusFor(status.outcome)));
                return;
            }
            reply(ctrl::fail(MemoryService::errorCodeFor(status.outcome),
                             status.message.empty() ? std::string("创建关联回忆未完成，请保留请求标识") : status.message, req));
        });
    }

    void detail(const drogon::HttpRequestPtr &req, Reply &&reply, std::string plan) {
        std::string actor;
        if (!ctrl::requireUser(req, reply, actor)) return;
        if (!ctrl::looksLikeUuid(plan)) {
            reply(ctrl::fail(ErrorCode::NotFound, "计划不存在", req));
            return;
        }
        db::Pool::instance().client()->execSqlAsync(
            // D6: a deleted memory's id is withheld and its state is stated. The link itself is immutable, so an undo restores this.
            "SELECT (usapp_shared_plan_read(p.id) || jsonb_build_object("
            "'memory_id', CASE WHEN usapp_memory_deleted_now(l.memory_id) THEN NULL ELSE l.memory_id END, "
            "'memory_unavailable', COALESCE(usapp_memory_deleted_now(l.memory_id), false)))::text AS facts "
            "FROM couple_members m JOIN shared_plans p ON p.couple_id = m.couple_id "
            "LEFT JOIN shared_plan_memory_links l ON l.plan_id = p.id AND l.couple_id = p.couple_id "
            "WHERE m.user_id = $1::uuid AND p.id = $2::uuid",
            [req, reply](const drogon::orm::Result &rows) {
                Json::Value result;
                if (rows.empty() || rows[0]["facts"].isNull()) reply(ctrl::fail(ErrorCode::NotFound, "计划不存在", req));
                else if (parseObject(rows[0]["facts"].as<std::string>(), result)) reply(jsonResponse(result, 200));
                else reply(ctrl::fail(ErrorCode::InternalError, "计划读取失败", req));
            },
            [req, reply](const drogon::orm::DrogonDbException &) {
                reply(ctrl::fail(ErrorCode::InternalError, "计划读取失败", req));
            }, actor, plan);
    }

    void list(const drogon::HttpRequestPtr &req, Reply &&reply) {
        std::string actor;
        if (!ctrl::requireUser(req, reply, actor)) return;
        const auto limit = TaskService::parseLimit(req->getParameter("limit"));
        const auto raw = req->getParameter("cursor");
        const auto cursor = AlbumService::decodeCursor(raw);
        if (!limit || (!raw.empty() && !cursor)) {
            reply(ctrl::fail(ErrorCode::ValidationError, "分页参数无效", req));
            return;
        }
        db::Pool::instance().client()->execSqlAsync(
            kListSql,
            [req, reply, limit = *limit](const drogon::orm::Result &rows) {
                Json::Value result(Json::objectValue);
                result["plans"] = Json::Value(Json::arrayValue);
                result["next_cursor"] = Json::nullValue;
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (i == static_cast<size_t>(limit)) {
                        result["next_cursor"] = AlbumService::encodeCursor({
                            rows[i - 1]["cursor_time"].as<std::string>(), rows[i - 1]["id"].as<std::string>()});
                        break;
                    }
                    Json::Value plan;
                    if (rows[i]["facts"].isNull() || !parseObject(rows[i]["facts"].as<std::string>(), plan)) {
                        reply(ctrl::fail(ErrorCode::InternalError, "计划列表读取失败", req));
                        return;
                    }
                    result["plans"].append(std::move(plan));
                }
                reply(jsonResponse(result, 200));
            },
            [req, reply](const drogon::orm::DrogonDbException &) {
                reply(ctrl::fail(ErrorCode::InternalError, "计划列表读取失败", req));
            }, actor, cursor ? std::string(cursor->createdAt) : std::string{},
            cursor ? std::string(cursor->id) : std::string{}, std::to_string(*limit + 1));
    }
};
}  // namespace usapp::controllers
