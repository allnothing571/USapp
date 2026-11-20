// =====================================================================
// services/api/src/services/MessageService.h
//
// Phase 2 P2-1: partner message board (docs/phase2-messages.md). Either member writes
// a plain-text message to the partner; the recipient may add one short reply. Both
// tables are append-only (no edit or delete in P2-1).
//
// Create (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members, with the partner;
//   2. key classification on (author, key): same fingerprint replays the committed
//      message; otherwise IDEMPOTENCY_KEY_REUSED;
//   3. INSERT ... ON CONFLICT DO NOTHING (a skipped insert is re-classified);
//   4. audit "message.created" in the same transaction; read back; commit.
// Reply (one TxScope):
//   1. membership FOR SHARE joined to a message of the same space;
//   2. key classification on the reply author's key (replay / KEY_REUSED);
//   3. only the message's recipient replies (author -> MESSAGE_REPLY_FORBIDDEN);
//   4. INSERT ... ON CONFLICT DO NOTHING: the primary key (message_id) lets exactly one
//      concurrent reply win; a skipped insert re-classifies, otherwise
//      MESSAGE_ALREADY_REPLIED;
//   5. audit "message.replied"; read back; commit.
// Only a confirmed commit reports success; an unconfirmed commit is ResultUnknown.
// Responses separate the immutable write ("mutation") from the current message.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "services/NotificationIntent.h"

namespace usapp {

class MessageService {
  public:
    static MessageService &instance();

    static constexpr size_t kMaxBodyCodePoints = 500;   // a message: task-description rules
    static constexpr size_t kMaxReplyCodePoints = 100;  // a reply: one line ("一句话")

    enum class Outcome {
        Created,          // 201
        Replayed,         // 200: same key and fingerprint
        Ok,               // 200: read
        Validation,       // 400
        SpaceNotFound,    // 404 SPACE_NOT_FOUND
        PartnerRequired,  // 409 PARTNER_REQUIRED
        NotFound,         // 404 MESSAGE_NOT_FOUND (absent or another space)
        Forbidden,        // 403 MESSAGE_REPLY_FORBIDDEN (the author replying)
        AlreadyReplied,   // 409 MESSAGE_ALREADY_REPLIED
        KeyReused,        // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,    // 503 RESULT_UNKNOWN
        Error,            // 500
    };

    struct MessageData {
        std::string id;
        std::string authorId;
        std::string recipientId;
        std::string body;
        std::string createdAt;
        bool hasReply = false;
        std::string replyAuthorId;
        std::string replyBody;
        std::string replyCreatedAt;
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;   // user-facing (Chinese), never echoes input
        std::string detail;    // log only: fixed labels
        Json::Value mutation;  // immutable facts of the write (or replay)
        MessageData data;      // current message at read time
        // P2-3b: original notification facts of a write (Json null: legacy write without an
        // intent). Reads leave hasNotification false.
        bool hasNotification = false;
        Json::Value notification;
        bool success() const {
            return outcome == Outcome::Created || outcome == Outcome::Replayed || outcome == Outcome::Ok;
        }
    };

    // Same strict cursor as the other lists: "<YYYY-MM-DDTHH:MM:SS.ffffffZ>_<uuid>".
    struct Cursor {
        std::string createdAt;
        std::string id;
    };

    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<MessageData> messages;
        std::optional<std::string> nextCursor;
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // ---------------------------------------------------------------- pure
    // Body exactly {"body": string}; message: 1-500 code points, LF allowed.
    static bool parseMessageRequest(const Json::Value &body, std::string &text, std::string &message);
    // Body exactly {"body": string}; reply: 1-100 code points, single line.
    static bool parseReplyRequest(const Json::Value &body, std::string &text, std::string &message);
    static std::string createFingerprint(const std::string &text);
    static std::string replyFingerprint(const std::string &messageId, const std::string &text);
    static std::string encodeCursor(const Cursor &cursor);
    static std::optional<Cursor> decodeCursor(const std::string &raw);
    static Json::Value messageJson(const MessageData &data);
    static Json::Value responseBody(const Status &status);  // {"mutation", "message"[, "notification"]}
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    // P2-3b: choice is the optional "notify_partner" (see services/NotificationIntent.h).
    void create(const std::string &userId, const std::string &key, const std::string &text, Callback cb,
                intent::Choice choice = {});
    void reply(const std::string &userId, const std::string &messageId, const std::string &key,
               const std::string &text, Callback cb, intent::Choice choice = {});
    // partnerOnly: only messages written by the partner (Home's latest message).
    void list(const std::string &userId, bool partnerOnly, int limit, const std::optional<Cursor> &cursor,
              ListCallback cb);
    void get(const std::string &userId, const std::string &messageId, Callback cb);

  private:
    MessageService() = default;
};

}  // namespace usapp
