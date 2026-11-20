// =====================================================================
// services/api/src/services/ProductService.h
//
// M7: reward-shop products (docs/phase1-m7-rewards.md). A member creates products
// for the partner (the owner, who will redeem them in later milestones). Identity
// rows are immutable; every create/edit/archive appends a revision with the full
// term set; the current product is the highest version.
//
// Mutations (one TxScope, READ COMMITTED):
//   1. caller membership FOR SHARE OF couple_members (joined to the partner for
//      create, or to the product for edit/archive);
//   2. edit/archive: per-product advisory lock (33007, hashtext(product));
//   3. key classification: same actor+key and fingerprint replays the committed
//      revision (also after later mutations); otherwise IDEMPOTENCY_KEY_REUSED;
//   4. creator-only, not archived, expected_version equals the current version;
//   5. revision INSERT ... ON CONFLICT DO NOTHING (a skipped insert is re-classified);
//   6. audit in the same transaction; read back; commit. Only a confirmed commit
//      reports success; an unconfirmed commit is ResultUnknown.
// Responses separate the immutable revision ("mutation") from the current product.
// M7 never writes wallet data and implements no redemption.
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Errors.h"

namespace usapp {

class ProductService {
  public:
    static ProductService &instance();

    // Approved 2026-10-26: integer price 1-999 flowers.
    static constexpr int kMinPriceFlowers = 1;
    static constexpr int kMaxPriceFlowers = 999;
    // Lists are cursor-paginated (TaskService::parseLimit: default 50, 1-100) in the
    // immutable (created_at DESC, id DESC) order. Visibility is evaluated per request,
    // so edits/archives between page requests can change which products appear;
    // pages are not a snapshot.
    struct Cursor {
        std::string createdAt;  // 'YYYY-MM-DDTHH:MM:SS.ffffffZ'
        std::string id;         // canonical lowercase uuid
    };
    static std::string encodeCursor(const Cursor &cursor);
    // Strict: anything but "<timestamp>_<uuid>" is rejected before any query.
    static std::optional<Cursor> decodeCursor(const std::string &raw);

    enum class Outcome {
        Created,          // 201
        Updated,          // 200: edit or archive committed
        Replayed,         // 200: same key and fingerprint
        Ok,               // 200: read
        Validation,       // 400
        SpaceNotFound,    // 404 SPACE_NOT_FOUND
        PartnerRequired,  // 409 PARTNER_REQUIRED
        NotFound,         // 404 PRODUCT_NOT_FOUND (absent, other space, or archived for the owner)
        Forbidden,        // 403 PRODUCT_FORBIDDEN: only the creator manages a product
        Archived,         // 409 PRODUCT_ARCHIVED
        VersionConflict,  // 409 PRODUCT_VERSION_CONFLICT: stale expected_version
        KeyReused,        // 409 IDEMPOTENCY_KEY_REUSED
        ResultUnknown,    // 503 RESULT_UNKNOWN
        Error,            // 500
    };

    enum class Action { Create, Edit, Archive };

    struct Terms {
        std::string title;        // normalized like task titles
        std::string description;  // normalized like task descriptions
        int priceFlowers = 0;
        std::string redemptionMode;  // "direct" | "request" (stored for later milestones)
    };

    struct ProductData {
        std::string id;
        std::string creatorId;
        std::string ownerId;
        std::string title;
        std::string description;
        int priceFlowers = 0;
        std::string redemptionMode;
        std::string status;  // active | archived
        int version = 0;
        std::string createdAt;
        std::string updatedAt;
    };

    struct Status {
        Outcome outcome = Outcome::Error;
        std::string message;  // user-facing (Chinese), never echoes input
        std::string detail;   // log only: fixed labels
        Json::Value mutation;  // immutable facts of the revision written (or replayed)
        ProductData product;   // current state at read time
        bool success() const {
            return outcome == Outcome::Created || outcome == Outcome::Updated || outcome == Outcome::Replayed ||
                   outcome == Outcome::Ok;
        }
    };

    struct ListResult {
        Outcome outcome = Outcome::Error;
        std::string message;
        std::string detail;
        std::vector<ProductData> products;
        std::optional<std::string> nextCursor;
    };

    using Callback = std::function<void(Status)>;
    using ListCallback = std::function<void(ListResult)>;

    // ---------------------------------------------------------------- pure
    static bool parseCreateRequest(const Json::Value &body, Terms &out, std::string &message);
    // Full replacement terms plus expected_version.
    static bool parseEditRequest(const Json::Value &body, Terms &out, int &expectedVersion, std::string &message);
    static bool parseArchiveRequest(const Json::Value &body, int &expectedVersion, std::string &message);
    static const char *actionName(Action action);
    // SHA-256 of "reward-product-<action>:v1:" + product id, expected version and terms.
    static std::string fingerprint(Action action, const std::string &productId, int expectedVersion,
                                   const Terms &terms);
    static Json::Value productJson(const ProductData &product);
    static Json::Value responseBody(const Status &status);  // {"mutation", "product"}
    static ErrorCode errorCodeFor(Outcome outcome);
    static int httpStatusFor(Outcome outcome);

    // ---------------------------------------------------------------- actions
    void create(const std::string &userId, const std::string &key, const Terms &terms, Callback cb);
    void edit(const std::string &userId, const std::string &productId, const std::string &key, int expectedVersion,
              const Terms &terms, Callback cb);
    void archive(const std::string &userId, const std::string &productId, const std::string &key,
                 int expectedVersion, Callback cb);
    void list(const std::string &userId, int limit, const std::optional<Cursor> &cursor, ListCallback cb);
    void get(const std::string &userId, const std::string &productId, Callback cb);

  private:
    ProductService() = default;
};

}  // namespace usapp
