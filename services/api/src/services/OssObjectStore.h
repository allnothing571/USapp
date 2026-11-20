// =====================================================================
// services/api/src/services/OssObjectStore.h
//
// Phase 3 P3-1d: the Alibaba Cloud OSS adapter behind media::ObjectStore (docs/phase3-p31d-oss-plan.md). OFF unless MEDIA_ENABLED
// is true and its configuration is complete. It has only ever talked to local TLS stubs: every vendor guarantee G1-G8 and T1-T4 is
// UNVERIFIED against real OSS until the networked run is approved and passes.
//
// Transport: libcurl's multi interface on one owned thread, rather than Drogon's HttpClient, for what this needs and that client
// lacks: a CA file and name mapping for hermetic tests, certificate and hostname verification that cannot be turned off from
// configuration, one absolute deadline that also covers a queued or stalled transfer, and real cancellation. libcurl is already linked.
//   - One attempt per call. A fresh connection per request, so libcurl has no reused connection to retry on. Nothing here retries; the
//     caller decides, and for a copy it never does: a retry takes a new final key (G3b).
//   - Unavailable says nothing about whether the request took effect. A malformed success is Unavailable, never Ok.
//   - Nothing logs a URL, key, signature, form field, credential or response body.
//
// Identities are separated (G2): the upload signer signs grants, the read signer signs reads, and the server makes the calls.
// =====================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "services/MediaStore.h"

namespace usapp::media {

struct OssCredential {
    std::string keyId;
    std::string secret;
};

struct OssConfig {
    std::string endpoint;  // host only, for example oss-cn-hangzhou.aliyuncs.com
    std::string region;    // for example cn-hangzhou
    std::string bucket;
    OssCredential uploadSigner;
    OssCredential readSigner;
    OssCredential server;
    int readUrlSeconds = 600;  // must equal MediaService::Policy::readUrlSeconds, which floors the expiry it passes in

    // Absolute deadlines, in milliseconds, from the call to its answer: queueing, DNS, connect, TLS, request and response.
    long headDeadlineMs = 10000;
    long infoDeadlineMs = 10000;
    long removeDeadlineMs = 10000;
    long listDeadlineMs = 20000;
    long copyDeadlineMs = 60000;

    // Tests only. fromEnv never sets these. Production uses the system trust store and real name resolution.
    std::string testCaFile;
    std::string testConnectTo;              // host:port to reach, whatever the URL names
    std::function<int64_t()> testClock;     // epoch seconds
};

// Only MEDIA_ENABLED=true reaches a store. Off gives nullopt and no problem. On but incomplete or inconsistent gives nullopt, and a
// problem naming variables and never their values, which refuses startup.
std::optional<OssConfig> ossConfigFromEnv(std::string &problem);
// The checks behind it, for any source of configuration.
std::string ossConfigProblem(const OssConfig &config);

namespace oss {

// ---- Pure: signing. Fixed vectors, including the vendor's worked example, cover these.
std::string uriEncode(const std::string &value, bool keepSlash);
std::string canonicalQuery(std::vector<std::pair<std::string, std::string>> params);
std::string canonicalRequest(const std::string &method, const std::string &canonicalUri, const std::string &query,
                             const std::map<std::string, std::string> &headers, const std::string &additionalHeaders);
std::string stringToSign(const std::string &timestamp, const std::string &region, const std::string &canonical);
std::string signingKey(const std::string &secret, const std::string &date, const std::string &region);
std::string signatureOver(const std::string &key, const std::string &text);  // lowercase hex
std::string hex(const std::string &bytes);
std::string isoTimestamp(int64_t epochSeconds);  // 20260301T000000Z

// ---- Pure: reading responses. False means malformed, which the store reports as Unavailable.
bool parseCopyResult(const std::string &body);
bool parseImageInfo(const std::string &body, ImageFacts &facts, int &frames);
bool parseListPage(const std::string &body, KeyPage &page);

// The transform for a variant; empty for original. False for anything else.
bool variantProcess(const std::string &variant, std::string &process);

}  // namespace oss

class OssObjectStore : public ObjectStore {
  public:
    explicit OssObjectStore(OssConfig config);
    ~OssObjectStore() override;
    OssObjectStore(const OssObjectStore &) = delete;
    OssObjectStore &operator=(const OssObjectStore &) = delete;

    UploadForm signUpload(const std::string &stagingKey, const std::string &contentType, int64_t size, int64_t expiresAtEpoch) override;
    std::string signRead(const std::string &finalKey, const std::string &variant, const std::string &contentType, int64_t expiresAtEpoch) override;
    void head(const std::string &key, std::function<void(StoreStatus, ObjectFacts)> done) override;
    void imageInfo(const std::string &key, std::function<void(StoreStatus, ImageFacts)> done) override;
    void copy(const std::string &source, const std::string &destination, std::function<void(StoreStatus)> done) override;
    void remove(const std::string &key, std::function<void(StoreStatus)> done) override;
    void list(const std::string &prefix, const std::string &marker, int limit, std::function<void(StoreStatus, KeyPage)> done) override;

  private:
    class Engine;
    struct Response;
    using Params = std::vector<std::pair<std::string, std::string>>;

    int64_t now() const;
    std::string host() const;
    void call(const char *op, const std::string &method, const std::string &key, bool bucketLevel, const Params &params,
              std::map<std::string, std::string> extraHeaders, long deadlineMs, size_t maxBody, std::function<void(Response)> done);

    OssConfig config_;
    std::unique_ptr<Engine> engine_;
};

}  // namespace usapp::media
