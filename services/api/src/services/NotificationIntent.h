// =====================================================================
// services/api/src/services/NotificationIntent.h
//
// Phase 2 P2-3b (docs/phase2-notifications.md Section 12): optional "notify_partner" on five
// ordinary operations. Every NEW operation records one immutable notification_intents row
// (migration 0020) inside the caller's TxScope, after its audit insert and before commit:
//   omitted          -> not requested; no remembered-choice update; no notice;
//   explicit choice  -> with a partner: preference sequence (lock 33012) and, for true, exactly
//                       one linked in-app notice; without a partner (anniversaries only): no
//                       preference update and no notice ("no_partner").
// Recipient, space and actor come from the immutable business row (SQL resolver), never from
// the client. Classified replays only READ the original intent (legacy operations: null).
// The preference lock is the last lock taken; nothing here touches another domain or a wallet.
//
// D20 (0043, docs/phase3-plan.md Section 30): three Phase 3 operations use the same intent and notice path: memory creation
// (not plan conversion), shared-plan creation and one album photo batch. They are default off on every form and never remember a
// choice: no preference sequence, and they are not in the remembered-choices read.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <memory>
#include <string>

#include "db/Db.h"

namespace usapp::intent {

enum class Operation { TaskPublish, AnniversaryCreate, AnniversaryEdit, MessageCreate, MessageReply, MemoryCreate, SharedPlanCreate, AlbumPhotosAdd };

// The five P2-3b operations remember an explicit choice; the D20 operations never do.
bool remembersChoice(Operation operation);

const char *operationName(Operation operation);

struct Choice {
    bool provided = false;  // false: the field was omitted (old clients); never remembered
    bool value = false;
    bool requested() const { return provided && value; }
};

// Reads and REMOVES the optional boolean "notify_partner" from a JSON object body, so the
// existing strict request parsers see exactly their legacy body. A non-boolean value fails.
bool takeChoice(Json::Value &body, Choice &out, std::string &message);

// Omitted/false keep the legacy fingerprint bytes; true uses the documented domain-separated
// extension SHA-256("notify-partner:v1:true:" + legacy), in the same fingerprint column.
std::string fingerprint(const std::string &legacy, const Choice &choice);

// Typed source: exactly one family for the operation.
struct Source {
    std::string taskId;
    std::string anniversaryId;
    int anniversaryVersion = 0;
    std::string messageId;       // message creation
    std::string replyMessageId;  // the reply row (message_replies.message_id)
    std::string memoryId;        // D20: a memory created directly (never by plan conversion)
    std::string sharedPlanId;    // D20: a created shared plan
    std::string albumBatchId;    // D20: one album_photo_batches row
};

// {requested, created, reason, notification_id}; reason: delivered_to_inbox | not_requested |
// no_partner. "created" means a durable in-app row, not Push delivery or that it was seen.
Json::Value facts(bool requested, bool hasRecipient, const std::string &notificationId);

using FactsCallback = std::function<void(Json::Value)>;

// New operation: append the intent (and, when eligible, the preference sequence and notice).
void record(const std::shared_ptr<db::TxScope> &scope, Operation operation, const Source &source,
            const Choice &choice, FactsCallback next, std::function<void()> failed);

// Replay: the original intent's facts, or Json null for a legacy operation without one.
void readOriginal(const std::shared_ptr<db::TxScope> &scope, Operation operation, const Source &source,
                  FactsCallback next, std::function<void()> failed);

// GET /api/v1/notification-choices for the caller: the five remembered defaults.
struct ChoicesResult {
    int status = 500;
    Json::Value body;
};
void readChoices(const std::string &userId, std::function<void(ChoicesResult)> cb);

// Diagnostics for an unexplained intermittent USAPP_NOTIFICATION_INTENT_SOURCE (design Section 13). Off unless a test binary sets a sink, and
// production never does: with no sink, no statement is added. With one, each intent write first reads the guard's predicates read-only. A failed
// transaction can read nothing more, so that read is the evidence. The line holds fixed labels, booleans and microsecond counts: no id, key or text.
struct SourcePredicates {
    bool found = false;
    bool operation = false;
    bool couple = false;
    bool actor = false;
    bool recipient = false;
    long long sourceAgeUs = 0;       // the clock at the probe, minus the source row's time. Negative: the row is in the future
    long long clockAfterProbeUs = 0; // the clock after the failure, minus the clock at the probe. Negative: the clock went backwards
};
using SourceFailureSink = std::function<void(const std::string &)>;
void setSourceFailureSink(SourceFailureSink sink);
// $1 family, $2 source id, $3 version or empty, $4 operation, $5 couple, $6 actor, $7 recipient or empty.
const char *sourcePredicateProbeSql();
// Pure. The first failed predicate, in the guard's order; a clock comparison when the identity predicates all held.
std::string describeSourceFailure(const char *operation, const SourcePredicates &predicates);

}  // namespace usapp::intent
