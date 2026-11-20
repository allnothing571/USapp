// =====================================================================
// services/api/src/services/OssObjectStore.cc
// =====================================================================
#include "services/OssObjectStore.h"

#include <curl/curl.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "core/Env.h"

namespace usapp::media {

namespace {

constexpr const char *kAlgorithm = "OSS4-HMAC-SHA256";
constexpr const char *kUnsigned = "UNSIGNED-PAYLOAD";
constexpr size_t kMaxSmallBody = 64 * 1024;
constexpr size_t kMaxListBody = 4 * 1024 * 1024;

std::string hmacSha256(const std::string &key, const std::string &message) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(message.data()), message.size(), out, &length);
    return std::string(reinterpret_cast<const char *>(out), length);
}

std::string sha256HexOf(const std::string &text) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), out);
    return oss::hex(std::string(reinterpret_cast<const char *>(out), sizeof(out)));
}

std::string base64(const std::string &bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string isoMillis(int64_t epochSeconds) {
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return buffer;
}

std::string credentialScope(const std::string &date, const std::string &region) { return date + "/" + region + "/oss/aliyun_v4_request"; }

std::string elementText(const std::string &body, const std::string &name, size_t &from) {
    const std::string open = "<" + name + ">", close = "</" + name + ">";
    const auto start = body.find(open, from);
    if (start == std::string::npos) { from = std::string::npos; return {}; }
    const auto end = body.find(close, start + open.size());
    if (end == std::string::npos) { from = std::string::npos; return {}; }
    from = end + close.size();
    return body.substr(start + open.size(), end - start - open.size());
}

// The five predefined entities; anything else is not what this listing should contain.
bool decodeXmlText(const std::string &text, std::string &out) {
    out.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '<') return false;
        if (c != '&') { out.push_back(c); continue; }
        static const std::pair<const char *, char> kEntities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool known = false;
        for (const auto &entity : kEntities) {
            const std::string name = entity.first;
            if (text.compare(i, name.size(), name) == 0) {
                out.push_back(entity.second);
                i += name.size() - 1;
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool isTokenChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'; }

bool validHostLabelText(const std::string &value, size_t maxLength) {
    return !value.empty() && value.size() <= maxLength && std::all_of(value.begin(), value.end(), isTokenChar) && value.front() != '.' && value.front() != '-' && value.back() != '.' && value.back() != '-';
}

}  // namespace

// =====================================================================
// Pure functions
// =====================================================================
namespace oss {

std::string hex(const std::string &bytes) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0x0f]);
    }
    return out;
}

std::string uriEncode(const std::string &value, bool keepSlash) {
    static const char *kDigits = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kDigits[c >> 4]);
            out.push_back(kDigits[c & 0x0f]);
        }
    }
    return out;
}

std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params) {
    std::vector<std::pair<std::string, std::string>> encoded;
    encoded.reserve(params.size());
    for (const auto &param : params) encoded.emplace_back(uriEncode(param.first, false), uriEncode(param.second, false));
    std::stable_sort(encoded.begin(), encoded.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string out;
    for (const auto &param : encoded) {
        if (!out.empty()) out.push_back('&');
        out += param.first;
        if (!param.second.empty()) out += "=" + param.second;
    }
    return out;
}

std::string canonicalRequest(const std::string &method, const std::string &canonicalUri, const std::string &query,
                             const std::map<std::string, std::string> &headers, const std::string &additionalHeaders) {
    std::string out = method + "\n" + canonicalUri + "\n" + query + "\n";
    for (const auto &header : headers) out += header.first + ":" + header.second + "\n";
    return out + "\n" + additionalHeaders + "\n" + kUnsigned;
}

std::string stringToSign(const std::string &timestamp, const std::string &region, const std::string &canonical) {
    return std::string(kAlgorithm) + "\n" + timestamp + "\n" + credentialScope(timestamp.substr(0, 8), region) + "\n" + sha256HexOf(canonical);
}

std::string signingKey(const std::string &secret, const std::string &date, const std::string &region) {
    const std::string dateKey = hmacSha256("aliyun_v4" + secret, date);
    const std::string regionKey = hmacSha256(dateKey, region);
    const std::string serviceKey = hmacSha256(regionKey, "oss");
    return hmacSha256(serviceKey, "aliyun_v4_request");
}

std::string signatureOver(const std::string &key, const std::string &text) { return hex(hmacSha256(key, text)); }

std::string isoTimestamp(int64_t epochSeconds) {
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

bool parseCopyResult(const std::string &body) {
    // The whole body is one CopyObjectResult with an ETag. A 200 carrying anything else proves nothing.
    if (body.find("<Error>") != std::string::npos) return false;
    size_t from = 0;
    const std::string result = elementText(body, "CopyObjectResult", from);
    if (from == std::string::npos) return false;
    size_t inner = 0;
    const std::string etag = elementText(result, "ETag", inner);
    return inner != std::string::npos && !etag.empty();
}

bool parseImageInfo(const std::string &body, ImageFacts &facts, int &frames) {
    Json::Value value;
    Json::CharReaderBuilder builder;
    builder["strictRoot"] = true;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 8;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &value, &errors) || !value.isObject()) return false;
    const auto text = [&value](const char *name, std::string &out) {
        const Json::Value &field = value[name];
        if (!field.isObject() || !field["value"].isString()) return false;
        out = field["value"].asString();
        return !out.empty();
    };
    const auto number = [&text](const char *name, int &out) {
        std::string raw;
        if (!text(name, raw) || raw.size() > 9u || !std::all_of(raw.begin(), raw.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
        out = std::stoi(raw);
        return true;
    };
    std::string format;
    int width = 0, height = 0;
    if (!text("Format", format) || !number("ImageWidth", width) || !number("ImageHeight", height) || !number("FrameCount", frames)) return false;
    for (auto &c : format) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    facts.format = format == "jpg" || format == "jpeg" ? "image/jpeg" : "image/" + format;
    facts.width = width;
    facts.height = height;
    return true;
}

bool parseListPage(const std::string &body, KeyPage &page) {
    if (body.find("<ListBucketResult") == std::string::npos) return false;
    page.keys.clear();
    page.nextMarker.clear();
    // Each object's key sits in <Contents>; a stray <Key> elsewhere would not name an object.
    size_t from = 0;
    while (true) {
        const std::string contents = elementText(body, "Contents", from);
        if (from == std::string::npos) break;
        size_t inner = 0;
        const std::string raw = elementText(contents, "Key", inner);
        std::string key;
        if (inner == std::string::npos || !decodeXmlText(raw, key) || key.empty()) return false;
        page.keys.push_back(std::move(key));
    }
    size_t truncatedAt = 0;
    const std::string truncated = elementText(body, "IsTruncated", truncatedAt);
    if (truncatedAt == std::string::npos || (truncated != "true" && truncated != "false")) return false;
    if (truncated == "true") {
        size_t tokenAt = 0;
        const std::string raw = elementText(body, "NextContinuationToken", tokenAt);
        if (tokenAt == std::string::npos || !decodeXmlText(raw, page.nextMarker) || page.nextMarker.empty()) return false;
    }
    return true;
}

bool variantProcess(const std::string &variant, std::string &process) {
    if (variant == "thumb") process = "image/auto-orient,1/resize,w_480/format,webp";
    else if (variant == "view") process = "image/auto-orient,1/resize,w_1600/format,webp";
    else if (variant == "original") process.clear();
    else return false;
    return true;
}

}  // namespace oss

// =====================================================================
// Configuration
// =====================================================================
std::string ossConfigProblem(const OssConfig &config) {
    if (!validHostLabelText(config.endpoint, 253)) return "OSS_ENDPOINT must be a host name, with no scheme or path";
    if (!validHostLabelText(config.region, 64)) return "OSS_REGION is not a region id";
    if (!validHostLabelText(config.bucket, 63) || config.bucket.find('.') != std::string::npos) return "OSS_BUCKET is not a bucket name";
    const std::pair<const OssCredential *, const char *> identities[] = {
        {&config.uploadSigner, "OSS_UPLOAD_KEY_ID and OSS_UPLOAD_KEY_SECRET"},
        {&config.readSigner, "OSS_READ_KEY_ID and OSS_READ_KEY_SECRET"},
        {&config.server, "OSS_SERVER_KEY_ID and OSS_SERVER_KEY_SECRET"},
    };
    for (const auto &identity : identities) {
        if (identity.first->keyId.empty() || identity.first->secret.empty()) return std::string(identity.second) + " are required";
    }
    // G2: one key doing two of these jobs would undo the separation.
    if (config.uploadSigner.keyId == config.readSigner.keyId || config.uploadSigner.keyId == config.server.keyId || config.readSigner.keyId == config.server.keyId) {
        return "OSS_UPLOAD_KEY_ID, OSS_READ_KEY_ID and OSS_SERVER_KEY_ID must be three different identities";
    }
    if (config.readUrlSeconds < 1 || config.readUrlSeconds > 3600) return "the read URL lifetime is out of range";
    for (const long deadline : {config.headDeadlineMs, config.infoDeadlineMs, config.removeDeadlineMs, config.listDeadlineMs, config.copyDeadlineMs}) {
        if (deadline < 1) return "every store call needs a deadline";
    }
    return {};
}

std::optional<OssConfig> ossConfigFromEnv(std::string &problem) {
    problem.clear();
    if (!env::getBool("MEDIA_ENABLED", false)) return std::nullopt;
    OssConfig config;
    config.endpoint = env::get("OSS_ENDPOINT");
    config.region = env::get("OSS_REGION");
    config.bucket = env::get("OSS_BUCKET");
    config.uploadSigner = {env::get("OSS_UPLOAD_KEY_ID"), env::get("OSS_UPLOAD_KEY_SECRET")};
    config.readSigner = {env::get("OSS_READ_KEY_ID"), env::get("OSS_READ_KEY_SECRET")};
    config.server = {env::get("OSS_SERVER_KEY_ID"), env::get("OSS_SERVER_KEY_SECRET")};
    problem = ossConfigProblem(config);
    if (!problem.empty()) return std::nullopt;
    return config;
}

// =====================================================================
// The transport
// =====================================================================
struct OssObjectStore::Response {
    bool transferred = false;  // a complete HTTP response arrived
    long status = 0;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body;
};

class OssObjectStore::Engine {
  public:
    struct Request {
        std::string method;
        std::string url;
        std::vector<std::string> headers;
        long deadlineMs = 0;
        size_t maxBody = 0;
        std::function<void(Response)> done;
    };

    Engine(std::string caFile, std::string connectTo) : caFile_(std::move(caFile)), connectTo_(std::move(connectTo)) {
        static std::once_flag once;
        std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
        multi_ = curl_multi_init();
        thread_ = std::thread([this]() { loop(); });
    }

    ~Engine() {
        stopping_ = true;
        curl_multi_wakeup(multi_);
        thread_.join();
        curl_multi_cleanup(multi_);
    }

    void submit(Request request) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            pending_.push_back(std::move(request));
        }
        curl_multi_wakeup(multi_);
    }

  private:
    struct Transfer {
        Request request;
        Response response;
        CURL *easy = nullptr;
        curl_slist *headers = nullptr;
    };

    static size_t onBody(char *data, size_t size, size_t count, void *user) {
        auto *transfer = static_cast<Transfer *>(user);
        const size_t length = size * count;
        // Past the cap the transfer fails, which reports Unavailable.
        if (transfer->response.body.size() + length > transfer->request.maxBody) return 0;
        transfer->response.body.append(data, length);
        return length;
    }

    static size_t onHeader(char *data, size_t size, size_t count, void *user) {
        auto *transfer = static_cast<Transfer *>(user);
        const size_t length = size * count;
        const std::string line(data, length);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (auto &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string value = line.substr(colon + 1);
            const auto first = value.find_first_not_of(" \t");
            const auto last = value.find_last_not_of(" \t\r\n");
            transfer->response.headers[name] = first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
        }
        return length;
    }

    static size_t onUpload(char *, size_t, size_t, void *) { return 0; }

    void start(Request request) {
        auto transfer = std::make_unique<Transfer>();
        transfer->request = std::move(request);
        CURL *easy = curl_easy_init();
        if (!easy) {
            transfer->request.done(Response{});
            return;
        }
        transfer->easy = easy;
        for (const auto &header : transfer->request.headers) transfer->headers = curl_slist_append(transfer->headers, header.c_str());
        curl_easy_setopt(easy, CURLOPT_URL, transfer->request.url.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        // One absolute deadline for the whole call, from here, including any wait for a connection slot.
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, transfer->request.deadlineMs);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, transfer->request.deadlineMs);
        // One attempt on one new connection: libcurl only re-sends on a reused connection, and none is ever reused.
        curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L);
        curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        // TLS only, always verified. There is deliberately no configuration that turns this off.
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        // No environment proxy may sit between this process and its credentials' signatures.
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
        if (!caFile_.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, caFile_.c_str());
        curl_slist *connectTo = nullptr;
        if (!connectTo_.empty()) {
            const std::string mapping = "::" + connectTo_;
            connectTo = curl_slist_append(connectTo, mapping.c_str());
            curl_easy_setopt(easy, CURLOPT_CONNECT_TO, connectTo);
        }
        connectTargets_[easy] = connectTo;
        const std::string &method = transfer->request.method;
        if (method == "HEAD") {
            curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        } else if (method == "PUT") {
            // An empty body, as a copy has.
            curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(easy, CURLOPT_READFUNCTION, onUpload);
            curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(0));
        } else if (method != "GET") {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        curl_multi_add_handle(multi_, easy);
        transfer.release();
    }

    void finish(CURL *easy, CURLcode code) {
        char *raw = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &raw);
        std::unique_ptr<Transfer> transfer(reinterpret_cast<Transfer *>(raw));
        if (code == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &transfer->response.status);
            transfer->response.transferred = true;
        }
        curl_multi_remove_handle(multi_, easy);
        curl_slist_free_all(transfer->headers);
        curl_slist_free_all(connectTargets_[easy]);
        connectTargets_.erase(easy);
        curl_easy_cleanup(easy);
        transfer->request.done(std::move(transfer->response));
    }

    void loop() {
        while (!stopping_) {
            std::deque<Request> starting;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                starting.swap(pending_);
            }
            for (auto &request : starting) start(std::move(request));
            int running = 0;
            curl_multi_perform(multi_, &running);
            int left = 0;
            while (CURLMsg *message = curl_multi_info_read(multi_, &left)) {
                if (message->msg == CURLMSG_DONE) finish(message->easy_handle, message->data.result);
            }
            int ready = 0;
            curl_multi_poll(multi_, nullptr, 0, 1000, &ready);
        }
        // Stopping: nothing further is sent, and every caller still gets an answer. What was in flight may still take effect.
        std::deque<Request> starting;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            starting.swap(pending_);
        }
        for (auto &request : starting) request.done(Response{});
        std::vector<CURL *> active;
        active.reserve(connectTargets_.size());
        for (const auto &entry : connectTargets_) active.push_back(entry.first);
        for (CURL *easy : active) finish(easy, CURLE_ABORTED_BY_CALLBACK);
    }

    std::string caFile_;
    std::string connectTo_;
    CURLM *multi_ = nullptr;
    std::mutex mutex_;
    std::deque<Request> pending_;
    std::map<CURL *, curl_slist *> connectTargets_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// =====================================================================
// The store
// =====================================================================
OssObjectStore::OssObjectStore(OssConfig config)
    : config_(std::move(config)), engine_(std::make_unique<Engine>(config_.testCaFile, config_.testConnectTo)) {}

OssObjectStore::~OssObjectStore() = default;

int64_t OssObjectStore::now() const {
    if (config_.testClock) return config_.testClock();
    return static_cast<int64_t>(std::time(nullptr));
}

std::string OssObjectStore::host() const { return config_.bucket + "." + config_.endpoint; }

UploadForm OssObjectStore::signUpload(const std::string &stagingKey, const std::string &contentType, int64_t size, int64_t expiresAtEpoch) {
    const std::string timestamp = oss::isoTimestamp(now());
    const std::string date = timestamp.substr(0, 8);
    const std::string credential = config_.uploadSigner.keyId + "/" + credentialScope(date, config_.region);
    // Every field the form carries is stated exactly. What the vendor does with fields a holder adds is unverified (N4), which is why
    // the copy to the final key replaces all metadata.
    Json::Value conditions(Json::arrayValue);
    const auto equal = [&conditions](const std::string &field, const std::string &value) {
        Json::Value condition(Json::arrayValue);
        condition.append("eq");
        condition.append("$" + field);
        condition.append(value);
        conditions.append(condition);
    };
    Json::Value bucket(Json::objectValue);
    bucket["bucket"] = config_.bucket;
    conditions.append(bucket);
    equal("key", stagingKey);
    equal("Content-Type", contentType);
    equal("success_action_status", "204");
    equal("x-oss-signature-version", kAlgorithm);
    equal("x-oss-credential", credential);
    equal("x-oss-date", timestamp);
    Json::Value range(Json::arrayValue);
    range.append("content-length-range");
    range.append(static_cast<Json::Int64>(size));
    range.append(static_cast<Json::Int64>(size));
    conditions.append(range);
    Json::Value policy(Json::objectValue);
    policy["expiration"] = isoMillis(expiresAtEpoch);
    policy["conditions"] = conditions;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string encodedPolicy = base64(Json::writeString(writer, policy));

    UploadForm form;
    form.url = "https://" + host() + "/";
    form.fields["key"] = stagingKey;
    form.fields["Content-Type"] = contentType;
    form.fields["success_action_status"] = "204";
    form.fields["policy"] = encodedPolicy;
    form.fields["x-oss-signature-version"] = kAlgorithm;
    form.fields["x-oss-credential"] = credential;
    form.fields["x-oss-date"] = timestamp;
    form.fields["x-oss-signature"] = oss::signatureOver(oss::signingKey(config_.uploadSigner.secret, date, config_.region), encodedPolicy);
    return form;
}

std::string OssObjectStore::signRead(const std::string &finalKey, const std::string &variant, const std::string &, int64_t expiresAtEpoch) {
    std::string process;
    if (!oss::variantProcess(variant, process)) return {};
    // The caller floors the expiry to its bucket, so the same image keeps one URL; the signing time comes from that same value.
    const int64_t signedAt = expiresAtEpoch - config_.readUrlSeconds;
    const std::string timestamp = oss::isoTimestamp(signedAt);
    const std::string date = timestamp.substr(0, 8);
    Params params = {
        {"x-oss-signature-version", kAlgorithm},
        {"x-oss-credential", config_.readSigner.keyId + "/" + credentialScope(date, config_.region)},
        {"x-oss-date", timestamp},
        {"x-oss-expires", std::to_string(config_.readUrlSeconds)},
        {"x-oss-additional-headers", "host"},
        {"response-cache-control", "private, max-age=" + std::to_string(config_.readUrlSeconds)},
    };
    // OSS rejects response-content-type (0017-00000902). Copies store a neutral type;
    // the image processor supplies the variant type and originals download as attachments.
    if (process.empty()) {
        params.emplace_back("response-content-disposition", "attachment");
    } else {
        params.emplace_back("x-oss-process", process);
        params.emplace_back("response-content-disposition", "inline");
    }
    const std::string query = oss::canonicalQuery(params);
    const std::string canonical = oss::canonicalRequest("GET", oss::uriEncode("/" + config_.bucket + "/" + finalKey, true), query, {{"host", host()}}, "host");
    const std::string signature = oss::signatureOver(oss::signingKey(config_.readSigner.secret, date, config_.region), oss::stringToSign(timestamp, config_.region, canonical));
    return "https://" + host() + "/" + oss::uriEncode(finalKey, true) + "?" + query + "&x-oss-signature=" + signature;
}

void OssObjectStore::call(const char *op, const std::string &method, const std::string &key, bool bucketLevel, const Params &params,
                          std::map<std::string, std::string> extraHeaders, long deadlineMs, size_t maxBody, std::function<void(Response)> done) {
    const std::string timestamp = oss::isoTimestamp(now());
    const std::string date = timestamp.substr(0, 8);
    std::map<std::string, std::string> headers = std::move(extraHeaders);
    headers["x-oss-content-sha256"] = kUnsigned;
    headers["x-oss-date"] = timestamp;
    const std::string query = oss::canonicalQuery(params);
    const std::string resource = bucketLevel ? "/" + config_.bucket + "/" : "/" + config_.bucket + "/" + key;
    const std::string canonical = oss::canonicalRequest(method, oss::uriEncode(resource, true), query, headers, "");
    const std::string signature = oss::signatureOver(oss::signingKey(config_.server.secret, date, config_.region), oss::stringToSign(timestamp, config_.region, canonical));

    Engine::Request request;
    request.method = method;
    request.url = "https://" + host() + "/" + (bucketLevel ? std::string() : oss::uriEncode(key, true)) + (query.empty() ? std::string() : "?" + query);
    for (const auto &header : headers) request.headers.push_back(header.first + ": " + header.second);
    request.headers.push_back("Authorization: " + std::string(kAlgorithm) + " Credential=" + config_.server.keyId + "/" + credentialScope(date, config_.region) + ", Signature=" + signature);
    // libcurl would otherwise add headers of its own that this request does not need.
    request.headers.push_back("Expect:");
    request.headers.push_back("Accept:");
    request.deadlineMs = deadlineMs;
    request.maxBody = maxBody;
    const std::string operation = op;
    request.done = [operation, done = std::move(done)](Response response) {
        // Fixed labels and a status only: no URL, key, header or body.
        if (!response.transferred) LOG_WARN << "oss call failed op=" << operation << " transfer=incomplete";
        else if (response.status >= 400 && response.status != 404) LOG_WARN << "oss call refused op=" << operation << " status=" << response.status;
        done(std::move(response));
    };
    engine_->submit(std::move(request));
}

void OssObjectStore::head(const std::string &key, std::function<void(StoreStatus, ObjectFacts)> done) {
    call("head", "HEAD", key, false, {}, {}, config_.headDeadlineMs, kMaxSmallBody, [done = std::move(done)](Response response) {
        if (!response.transferred) { done(StoreStatus::Unavailable, {}); return; }
        if (response.status == 404) { done(StoreStatus::NotFound, {}); return; }
        if (response.status != 200) { done(StoreStatus::Unavailable, {}); return; }
        ObjectFacts facts;
        const auto length = response.headers.find("content-length");
        const auto etag = response.headers.find("etag");
        if (length == response.headers.end() || etag == response.headers.end() || etag->second.empty() || length->second.empty() || length->second.size() > 18u ||
            !std::all_of(length->second.begin(), length->second.end(), [](unsigned char c) { return std::isdigit(c); })) {
            done(StoreStatus::Unavailable, {});
            return;
        }
        facts.size = std::stoll(length->second);
        facts.etag = etag->second;
        const auto crc = response.headers.find("x-oss-hash-crc64ecma");
        if (crc != response.headers.end()) facts.crc64 = crc->second;
        done(StoreStatus::Ok, std::move(facts));
    });
}

void OssObjectStore::imageInfo(const std::string &key, std::function<void(StoreStatus, ImageFacts)> done) {
    call("image_info", "GET", key, false, {{"x-oss-process", "image/info"}}, {}, config_.infoDeadlineMs, kMaxSmallBody, [done = std::move(done)](Response response) {
        if (!response.transferred) { done(StoreStatus::Unavailable, {}); return; }
        if (response.status == 404) { done(StoreStatus::NotFound, {}); return; }
        // A refusal is not evidence about the photo: the vendor's answer for a non-image is undocumented (B6), and an outage must never
        // become a rejection. Until measured, a non-image ends through the attempt cap rather than through Invalid.
        if (response.status != 200) { done(StoreStatus::Unavailable, {}); return; }
        ImageFacts facts;
        int frames = 0;
        if (!oss::parseImageInfo(response.body, facts, frames)) { done(StoreStatus::Unavailable, {}); return; }
        // An animation is a decodable image that this product does not accept.
        if (frames != 1) { done(StoreStatus::Invalid, {}); return; }
        done(StoreStatus::Ok, std::move(facts));
    });
}

void OssObjectStore::copy(const std::string &source, const std::string &destination, std::function<void(StoreStatus)> done) {
    // REPLACE: nothing a grant holder set on the staging object reaches the final object. Sent once; see the header.
    // Originals keep the neutral stored type. No ACL header: that would need a wider permission
    // than plan Section 2.3, and the bucket is private with public access blocked. That the source's ACL does not carry is unverified.
    std::map<std::string, std::string> headers = {
        {"content-type", "application/octet-stream"},
        {"x-oss-copy-source", "/" + config_.bucket + "/" + oss::uriEncode(source, true)},
        {"x-oss-metadata-directive", "REPLACE"},
    };
    call("copy", "PUT", destination, false, {}, std::move(headers), config_.copyDeadlineMs, kMaxSmallBody, [done = std::move(done)](Response response) {
        if (!response.transferred) { done(StoreStatus::Unavailable); return; }
        if (response.status == 404) { done(StoreStatus::NotFound); return; }
        // A 200 proves nothing by itself: only the documented result body does.
        done(response.status == 200 && oss::parseCopyResult(response.body) ? StoreStatus::Ok : StoreStatus::Unavailable);
    });
}

void OssObjectStore::remove(const std::string &key, std::function<void(StoreStatus)> done) {
    call("remove", "DELETE", key, false, {}, {}, config_.removeDeadlineMs, kMaxSmallBody, [done = std::move(done)](Response response) {
        done(response.transferred && response.status == 204 ? StoreStatus::Ok : StoreStatus::Unavailable);
    });
}

void OssObjectStore::list(const std::string &prefix, const std::string &marker, int limit, std::function<void(StoreStatus, KeyPage)> done) {
    Params params = {{"list-type", "2"}, {"prefix", prefix}, {"max-keys", std::to_string(std::clamp(limit, 1, 1000))}};
    if (!marker.empty()) params.emplace_back("continuation-token", marker);
    call("list", "GET", {}, true, params, {}, config_.listDeadlineMs, kMaxListBody, [done = std::move(done)](Response response) {
        KeyPage page;
        if (!response.transferred || response.status != 200 || !oss::parseListPage(response.body, page)) {
            done(StoreStatus::Unavailable, {});
            return;
        }
        done(StoreStatus::Ok, std::move(page));
    });
}

}  // namespace usapp::media
