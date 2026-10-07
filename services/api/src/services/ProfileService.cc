// =====================================================================
// services/api/src/services/ProfileService.cc
// =====================================================================
#include "services/ProfileService.h"

#include <drogon/drogon.h>

#include <memory>
#include <utility>
#include <vector>

#include "db/Db.h"

namespace usapp {

namespace {

// 一个解码后的码点及其在原始字节串中的位置，用于按码点裁剪后再按字节截取。
struct DecodedCodePoint {
    char32_t value = 0;
    size_t offset = 0;
    size_t length = 0;
};

// 严格 UTF-8 解码：拒绝过长编码、代理项（U+D800..U+DFFF）、超出 U+10FFFF
// 的码点与截断序列。不做任何替换或修复。
bool decodeUtf8Strict(const std::string &raw, std::vector<DecodedCodePoint> &out) {
    out.clear();
    const size_t n = raw.size();
    size_t i = 0;
    while (i < n) {
        const auto lead = static_cast<unsigned char>(raw[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (lead < 0x80) {
            cp = lead;
            len = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            cp = lead & 0x1F;
            len = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            cp = lead & 0x0F;
            len = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            cp = lead & 0x07;
            len = 4;
        } else {
            return false;  // 续字节开头、0xC0/0xC1 过长前导、0xF5 以上
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            const auto cont = static_cast<unsigned char>(raw[i + k]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;
        }
        out.push_back(DecodedCodePoint{cp, i, len});
        i += len;
    }
    return true;
}

// 控制字符与换行：C0（含 \t \n \r）、DEL、C1（含 U+0085）、U+2028、U+2029。
bool isForbiddenCodePoint(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
}

// 可裁剪的首尾空白：Unicode Zs（空格分隔符）类。换行与制表符属于上面的
// 禁止字符，刻意不在这里，避免 "名字\n" 被悄悄裁成合法值。
bool isTrimmableSpace(char32_t cp) {
    return cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

const char *kUpdateDisplayName =
    "UPDATE users SET display_name = $2 WHERE id = $1::uuid RETURNING id::text AS id";

// couple_id 取自调用者当前空间（couple_members.user_id 唯一，最多一行）；
// 没有空间时为 NULL。detail 只记录字段名，不记录昵称值。
const char *kAuditInsert =
    "INSERT INTO audit_logs "
    "(actor_type, actor_id, couple_id, action, target_type, target_id, detail) "
    "VALUES ('user', $1::uuid, "
    "        (SELECT couple_id FROM couple_members WHERE user_id = $1::uuid), "
    "        'user.display_name_updated', 'user', $2, $3::jsonb)";

const char *kAuditDetail = "{\"field\":\"display_name\"}";

ProfileService::Status makeStatus(ProfileService::Outcome outcome, std::string detail = "") {
    ProfileService::Status status;
    status.outcome = outcome;
    status.detail = std::move(detail);
    return status;
}

// ---------------------------------------------------------------------
// 生命周期与所有权（不得形成强引用环）：
//
//   UpdateCtx --(shared)--> TxScope --(onError_，弱引用)--> UpdateCtx
//
// TxScope 会把 begin() 收到的错误回调保存在 onError_ 里，直到 TxScope 析构；
// 而 TxScope 由 ctx->scope 持有。若该错误回调强捕获 ctx，就会形成
// ctx -> scope -> onError_ -> ctx 的环：提交成功或 NotFound 回滚之后，
// ctx 与其中的请求回调（done）永远不会释放。因此存进 TxScope 的错误回调
// 只弱捕获 ctx。
//
// 异步期间由谁保活 ctx（每条错误路径触发时都至少有一个强引用在栈上）：
//   * 事务创建失败：begin() 同步调用错误回调，updateDisplayName 的局部 ctx 仍在。
//   * SQL 失败：Drogon 持有的 SQL 回调强捕获 ctx，并在其中调用 scope->fail()，
//     fail() 同步调用错误回调。
//   * 提交失败：提交回调（由 Drogon 事务对象持有）里捕获了 onDone，onDone 强捕获
//     ctx，所以提交回调执行期间 ctx 一定存活，弱引用必然能 lock 成功。
//   * 提交成功 / NotFound：完成回调本身强捕获 ctx。
// 这些强引用都只存在于 Drogon 正在等待或正在执行的回调里，回调执行完被释放后，
// ctx -> scope 这条单向链随之整体析构。本文件从不在 TxScope 成员函数执行期间
// 重置 ctx->scope。
//
// 恰好一次：所有结束路径都经 complete()；completed 标志保证 done 最多调用一次。
// done 刻意保留到 ctx 析构时才释放，使“请求回调被释放”与“ctx 被释放”是同一件事，
// 回归测试据此用回调持有的哨兵证明 ctx 没有泄漏。
// ---------------------------------------------------------------------
struct UpdateCtx {
    std::shared_ptr<db::TxScope> scope;
    std::string userId;
    std::string displayName;
    ProfileService::StatusCallback done;
    bool completed = false;
};

using UpdateCtxPtr = std::shared_ptr<UpdateCtx>;

void complete(const UpdateCtxPtr &ctx, ProfileService::Status status) {
    if (ctx->completed) {
        LOG_ERROR << "ProfileService 重复结束被忽略 outcome=" << static_cast<int>(status.outcome);
        return;
    }
    ctx->completed = true;
    if (ctx->done) {
        ctx->done(std::move(status));
    }
}

void failWithDb(const UpdateCtxPtr &ctx, const char *context, const drogon::orm::DrogonDbException &e) {
    ctx->scope->fail(db::describePgError(context, e.base()));
}

// ---- 步骤 2：写审计（与更新同事务），然后提交 ----
void updateStepAudit(const UpdateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kAuditInsert,
        [ctx](const drogon::orm::Result &) {
            // 只有提交真正成功才回调 Ok；提交失败由 TxScope 转到 begin 的错误回调。
            // onDone 强捕获 ctx：它也是提交失败时弱引用能 lock 成功的保证（见上方说明）。
            ctx->scope->commit([ctx]() { complete(ctx, makeStatus(ProfileService::Outcome::Ok)); });
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx, "写昵称审计失败", e); },
        ctx->userId,
        ctx->userId,
        std::string(kAuditDetail));
}

// ---- 步骤 1：只更新会话用户自己的行 ----
void updateStepWrite(const UpdateCtxPtr &ctx) {
    ctx->scope->tx().execSqlAsync(
        kUpdateDisplayName,
        [ctx](const drogon::orm::Result &rows) {
            if (rows.size() != 1u) {
                ctx->scope->rollback([ctx]() {
                    complete(ctx, makeStatus(ProfileService::Outcome::NotFound, "会话用户不存在"));
                });
                return;
            }
            updateStepAudit(ctx);
        },
        [ctx](const drogon::orm::DrogonDbException &e) { failWithDb(ctx, "更新昵称失败", e); },
        ctx->userId,
        ctx->displayName);
}

}  // namespace

ProfileService &ProfileService::instance() {
    static ProfileService service;
    return service;
}

ProfileService::DisplayNameError ProfileService::normalizeDisplayName(const std::string &raw,
                                                                       std::string &out) {
    out.clear();
    if (raw.size() > kDisplayNameMaxRawBytes) {
        return DisplayNameError::TooLong;
    }

    std::vector<DecodedCodePoint> cps;
    if (!decodeUtf8Strict(raw, cps)) {
        return DisplayNameError::InvalidUtf8;
    }
    for (const auto &cp : cps) {
        if (isForbiddenCodePoint(cp.value)) {
            return DisplayNameError::ControlCharacter;
        }
    }

    size_t first = 0;
    size_t last = cps.size();  // 半开区间 [first, last)
    while (first < last && isTrimmableSpace(cps[first].value)) {
        ++first;
    }
    while (last > first && isTrimmableSpace(cps[last - 1].value)) {
        --last;
    }
    if (first == last) {
        return DisplayNameError::Empty;
    }
    if (last - first > kDisplayNameMaxCodePoints) {
        return DisplayNameError::TooLong;
    }

    const size_t begin = cps[first].offset;
    const size_t end = cps[last - 1].offset + cps[last - 1].length;
    out = raw.substr(begin, end - begin);
    return DisplayNameError::None;
}

const char *ProfileService::displayNameErrorMessage(DisplayNameError error) {
    switch (error) {
        case DisplayNameError::InvalidUtf8:
            return "昵称包含无法识别的字符";
        case DisplayNameError::ControlCharacter:
            return "昵称不能包含换行或控制字符";
        case DisplayNameError::Empty:
            return "昵称不能为空";
        case DisplayNameError::TooLong:
            return "昵称最多 20 个字符";
        case DisplayNameError::None:
            break;
    }
    return "请求参数不合法";
}

void ProfileService::updateDisplayName(const std::string &userId,
                                       const std::string &rawName,
                                       StatusCallback cb) {
    if (userId.empty()) {
        cb(makeStatus(Outcome::NotFound, "缺少用户标识"));
        return;
    }

    std::string normalized;
    const DisplayNameError error = normalizeDisplayName(rawName, normalized);
    if (error != DisplayNameError::None) {
        Status status = makeStatus(Outcome::Validation, "昵称校验未通过");
        status.validation = error;
        cb(status);
        return;
    }

    auto ctx = std::make_shared<UpdateCtx>();
    ctx->userId = userId;
    ctx->displayName = std::move(normalized);
    ctx->done = std::move(cb);

    // onReady 只在 begin() 内同步调用、不被保存，可以强捕获；
    // 错误回调会被 TxScope 保存为 onError_，只能弱捕获（见文件内生命周期说明）。
    std::weak_ptr<UpdateCtx> weakCtx = ctx;
    db::TxScope::begin(
        [ctx](std::shared_ptr<db::TxScope> scope) {
            ctx->scope = std::move(scope);
            updateStepWrite(ctx);
        },
        [weakCtx](const std::string &message) {
            if (auto strong = weakCtx.lock()) {
                complete(strong, makeStatus(Outcome::Error, message));
                return;
            }
            // 按上方的保活分析不应发生；发生则说明有路径丢失了强引用，只能记录。
            LOG_ERROR << "ProfileService 事务错误回调触发时上下文已释放，无法回调：" << message;
        });
}

}  // namespace usapp
