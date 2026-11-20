// =====================================================================
// services/api/tests/OssStubTest.cc
//
// P3-1d, offline: OssObjectStore against local TLS stubs. No database, no network beyond loopback, no credentials: every key here is a
// fixed test string. These tests prove the adapter's side of each guarantee. They prove NOTHING about the vendor, whose guarantees stay
// unverified until the networked run is approved (docs/phase3-p31d-oss-plan.md).
// =====================================================================
#include <drogon/drogon.h>
#include <json/json.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "OssStubClient.h"
#include "OssStubServer.h"
#include "TestSupport.h"
#include "services/OssObjectStore.h"

using namespace usapptest::ossstub;
using usapp::media::ImageFacts;
using usapp::media::KeyPage;
using usapp::media::ObjectFacts;
using usapp::media::OssConfig;
using usapp::media::OssObjectStore;
using usapp::media::StoreStatus;
using usapp::media::UploadForm;
namespace oss = usapp::media::oss;

namespace {

const std::string kBucket = "usapp-stub";
const std::string kEndpoint = "oss-stub.example.test";
const std::string kHost = kBucket + "." + kEndpoint;
const std::string kRegion = "cn-stub";
const std::string kStaging = "staging/c/m/upload";
const std::string kFinal = "media/c/m/a1/original";

template <typename T>
struct Waiter {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<T> value;
    void deliver(T v) {
        std::lock_guard<std::mutex> guard(mutex);
        value = std::move(v);
        ready.notify_all();
    }
    std::optional<T> wait(int ms = 5000) {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return value.has_value(); });
        return value;
    }
};

struct Pki {
    Certificate ca = makeCertificate("usapp stub ca", nullptr, "");
    Certificate good = makeCertificate(kHost, &ca, kHost);
    Certificate wrongName = makeCertificate("other", &ca, "other.example.test");
    Certificate expired = makeCertificate(kHost, &ca, kHost, -1);
    Certificate otherCa = makeCertificate("someone else", nullptr, "");
    Certificate untrusted = makeCertificate(kHost, &otherCa, kHost);
    std::string caFile = writeCaFile(ca, "trusted");
};
Pki &pki() {
    static Pki instance;
    return instance;
}

OssConfig baseConfig() {
    OssConfig config;
    config.endpoint = kEndpoint;
    config.region = kRegion;
    config.bucket = kBucket;
    config.uploadSigner = {"UPLOADKEYID", "UPLOADSECRET"};
    config.readSigner = {"READKEYID", "READSECRET"};
    config.server = {"SERVERKEYID", "SERVERSECRET"};
    config.testCaFile = pki().caFile;
    config.headDeadlineMs = config.infoDeadlineMs = config.removeDeadlineMs = config.listDeadlineMs = config.copyDeadlineMs = 3000;
    return config;
}

StubOss::Identities identities() {
    return {{{"UPLOADKEYID", "UPLOADSECRET"}, {"READKEYID", "READSECRET"}, {"SERVERKEYID", "SERVERSECRET"}}, kRegion, kBucket, kHost};
}

struct Rig {
    std::shared_ptr<StubOss> model = std::make_shared<StubOss>(identities());
    std::unique_ptr<StubServer> server;
    std::unique_ptr<OssObjectStore> store;

    explicit Rig(long deadlineMs = 3000, const Certificate *leaf = nullptr) {
        auto held = model;
        server = std::make_unique<StubServer>(leaf ? *leaf : pki().good, [held](const Recorded &request) { return held->handle(request); });
        OssConfig config = baseConfig();
        config.headDeadlineMs = config.infoDeadlineMs = config.removeDeadlineMs = config.listDeadlineMs = config.copyDeadlineMs = deadlineMs;
        config.testConnectTo = server->connectTo();
        store = std::make_unique<OssObjectStore>(std::move(config));
    }
    StoreStatus copy(const std::string &source = kStaging, const std::string &destination = kFinal) {
        auto waiter = std::make_shared<Waiter<StoreStatus>>();
        store->copy(source, destination, [waiter](StoreStatus s) { waiter->deliver(s); });
        return waiter->wait().value_or(StoreStatus::Invalid);
    }
    std::pair<StoreStatus, ObjectFacts> head(const std::string &key) {
        auto waiter = std::make_shared<Waiter<std::pair<StoreStatus, ObjectFacts>>>();
        store->head(key, [waiter](StoreStatus s, ObjectFacts f) { waiter->deliver({s, std::move(f)}); });
        return waiter->wait().value_or(std::make_pair(StoreStatus::Invalid, ObjectFacts{}));
    }
    std::pair<StoreStatus, ImageFacts> info(const std::string &key) {
        auto waiter = std::make_shared<Waiter<std::pair<StoreStatus, ImageFacts>>>();
        store->imageInfo(key, [waiter](StoreStatus s, ImageFacts f) { waiter->deliver({s, std::move(f)}); });
        return waiter->wait().value_or(std::make_pair(StoreStatus::Invalid, ImageFacts{}));
    }
    StoreStatus remove(const std::string &key) {
        auto waiter = std::make_shared<Waiter<StoreStatus>>();
        store->remove(key, [waiter](StoreStatus s) { waiter->deliver(s); });
        return waiter->wait().value_or(StoreStatus::Invalid);
    }
    std::pair<StoreStatus, KeyPage> list(const std::string &prefix, const std::string &marker = "") {
        auto waiter = std::make_shared<Waiter<std::pair<StoreStatus, KeyPage>>>();
        store->list(prefix, marker, 100, [waiter](StoreStatus s, KeyPage p) { waiter->deliver({s, std::move(p)}); });
        return waiter->wait().value_or(std::make_pair(StoreStatus::Invalid, KeyPage{}));
    }
    void seed(const std::string &key, const std::string &bytes, std::map<std::string, std::string> metadata = {}) {
        std::lock_guard<std::mutex> guard(model->mutex_);
        model->objects[key] = StoredObject{bytes, std::move(metadata), 0};
    }
};

// A stub whose every request gets one fixed action.
struct FixedRig {
    std::unique_ptr<StubServer> server;
    std::unique_ptr<OssObjectStore> store;
    explicit FixedRig(Action action, long deadlineMs = 3000) {
        server = std::make_unique<StubServer>(pki().good, [action](const Recorded &) { return action; });
        OssConfig config = baseConfig();
        config.headDeadlineMs = config.infoDeadlineMs = config.removeDeadlineMs = config.listDeadlineMs = config.copyDeadlineMs = deadlineMs;
        config.testConnectTo = server->connectTo();
        store = std::make_unique<OssObjectStore>(std::move(config));
    }
    StoreStatus copy() {
        auto waiter = std::make_shared<Waiter<StoreStatus>>();
        store->copy(kStaging, kFinal, [waiter](StoreStatus s) { waiter->deliver(s); });
        return waiter->wait().value_or(StoreStatus::Invalid);
    }
};

Action respond(int status, const std::string &body = "", std::map<std::string, std::string> headers = {}) {
    Action action;
    action.status = status;
    action.body = body;
    action.headers = std::move(headers);
    return action;
}

long long millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

std::string queryValue(const std::string &url, const std::string &name) {
    const auto mark = url.find('?');
    const auto query = splitQuery(url.substr(mark + 1));
    const auto found = query.find(name);
    return found == query.end() ? std::string() : percentDecode(found->second);
}

std::string replaceOnce(std::string text, const std::string &from, const std::string &to) {
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

}  // namespace

int main() {
    trantor::Logger::setLogLevel(trantor::Logger::kFatal);

    TEST_CASE("OSS signing: reproduces the vendor's worked example, as far as the vendor's values allow") {
        // Vendor example (signature V4 page). Its canonical request and final signature are published. Its key is not derivable from the
        // placeholder secret it prints, so the derivation is covered below by an independent implementation instead.
        const std::string canonical = oss::canonicalRequest(
            "PUT", oss::uriEncode("/examplebucket/exampleobject", true), "",
            {{"content-disposition", "attachment"}, {"content-length", "3"}, {"content-md5", "ICy5YqxZB1uWSwcVLSNLcA=="}, {"content-type", "text/plain"},
             {"x-oss-content-sha256", "UNSIGNED-PAYLOAD"}, {"x-oss-date", "20250411T064124Z"}},
            "content-disposition;content-length");
        const std::string text = oss::stringToSign("20250411T064124Z", "cn-hangzhou", canonical);
        CHECK_EQ(text, std::string("OSS4-HMAC-SHA256\n20250411T064124Z\n20250411/cn-hangzhou/oss/aliyun_v4_request\nc46d96390bdbc2d739ac9363293ae9d710b14e48081fcb22cd8ad54b63136eca"));
        std::string vendorKey;
        for (size_t i = 0; i < 64; i += 2) vendorKey.push_back(static_cast<char>(std::stoi(std::string("3543b7686e65eda71e5e5ca19d548d78423c37e8ddba4dc9d83f90228b457c76").substr(i, 2), nullptr, 16)));
        CHECK_EQ(oss::signatureOver(vendorKey, text), std::string("053edbf550ebd239b32a9cdfd93b0b2b3f2d223083aa61f75e9ac16856d61f23"));
    }

    TEST_CASE("OSS signing: key derivation and URL form match an independent implementation") {
        // Computed separately, in Python's hmac and hashlib, from the documented steps. No vendor vector exists for these.
        const std::string key = oss::signingKey("yourAccessKeySecret", "20250411", "cn-hangzhou");
        CHECK_EQ(oss::hex(key), std::string("8a01ff4efcc65ca2cbc75375045c61ab5f3fa8b9a2d84f0add27ef16a25feb3c"));
        const std::string query = "response-content-disposition=inline&response-content-type=image%2Fwebp&x-oss-additional-headers=host&x-oss-credential=READKEYID%2F20260301%2Fcn-test%2Foss%2Faliyun_v4_request&x-oss-date=20260301T000000Z&x-oss-expires=600&x-oss-process=image%2Fauto-orient%2C1%2Fresize%2Cw_480%2Fformat%2Cwebp&x-oss-signature-version=OSS4-HMAC-SHA256";
        const std::string canonical = oss::canonicalRequest("GET", "/test-bucket/media/a/b/c/original", query, {{"host", "test-bucket.oss-cn-test.example.test"}}, "host");
        CHECK_EQ(oss::signatureOver(oss::signingKey("READSECRET", "20260301", "cn-test"), oss::stringToSign("20260301T000000Z", "cn-test", canonical)),
                 std::string("dee98295a68c1ac63ecae7ff6a6ed6f5d146f7b52ca97197cb97c7dfed5ce4e7"));
        CHECK_EQ(oss::signatureOver(oss::signingKey("UPLOADSECRET", "20260301", "cn-test"), "eyJ0ZXN0IjoxfQ=="), std::string("c4be3c91ef2bd46acd18fba048aeda7a046e1a9d7310da847ed6fc5303c0d354"));
        // Encoding and ordering.
        CHECK_EQ(oss::uriEncode("/a b/c~d_e-f.g", true), std::string("/a%20b/c~d_e-f.g"));
        CHECK_EQ(oss::uriEncode("a/b,c", false), std::string("a%2Fb%2Cc"));
        CHECK_EQ(oss::canonicalQuery({{"z", "1"}, {"a", "x y"}, {"acl", ""}}), std::string("a=x%20y&acl&z=1"));
        CHECK_EQ(oss::isoTimestamp(1772323200), std::string("20260301T000000Z"));
    }

    TEST_CASE("OSS responses: a malformed success is never a success") {
        CHECK(oss::parseCopyResult("<?xml version=\"1.0\"?><CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>"));
        for (const char *bad : {"", "ok", "<CopyObjectResult></CopyObjectResult>", "<CopyObjectResult><ETag></ETag></CopyObjectResult>", "<CopyObjectResult><ETag>\"a\"</ETag>",
                                "<Error><Code>InternalError</Code></Error>", "<Error><Code>x</Code></Error><CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>", "<html>gateway</html>"}) {
            CHECK_FALSE(oss::parseCopyResult(bad));
        }
        ImageFacts facts;
        int frames = 0;
        CHECK(oss::parseImageInfo("{\"Format\":{\"value\":\"jpg\"},\"ImageWidth\":{\"value\":\"4032\"},\"ImageHeight\":{\"value\":\"3024\"},\"FrameCount\":{\"value\":\"1\"}}", facts, frames));
        CHECK_EQ(facts.format, std::string("image/jpeg"));
        CHECK_EQ(facts.width, 4032);
        CHECK_EQ(frames, 1);
        CHECK(oss::parseImageInfo("{\"Format\":{\"value\":\"WEBP\"},\"ImageWidth\":{\"value\":\"1\"},\"ImageHeight\":{\"value\":\"1\"},\"FrameCount\":{\"value\":\"1\"}}", facts, frames));
        CHECK_EQ(facts.format, std::string("image/webp"));
        for (const char *bad : {"", "[]", "{}", "{\"Format\":{\"value\":\"jpg\"}}", "{\"Format\":{\"value\":\"\"},\"ImageWidth\":{\"value\":\"1\"},\"ImageHeight\":{\"value\":\"1\"},\"FrameCount\":{\"value\":\"1\"}}",
                                "{\"Format\":{\"value\":\"jpg\"},\"ImageWidth\":{\"value\":\"-1\"},\"ImageHeight\":{\"value\":\"1\"},\"FrameCount\":{\"value\":\"1\"}}",
                                "{\"Format\":{\"value\":\"jpg\"},\"ImageWidth\":{\"value\":\"99999999999\"},\"ImageHeight\":{\"value\":\"1\"},\"FrameCount\":{\"value\":\"1\"}}",
                                "{\"Format\":{\"value\":\"jpg\"},\"ImageWidth\":1,\"ImageHeight\":1,\"FrameCount\":1}"}) {
            CHECK_FALSE(oss::parseImageInfo(bad, facts, frames));
        }
        KeyPage page;
        CHECK(oss::parseListPage("<ListBucketResult><IsTruncated>true</IsTruncated><NextContinuationToken>t&amp;1</NextContinuationToken><Contents><Key>a&amp;b</Key></Contents><Contents><Key>c</Key></Contents></ListBucketResult>", page));
        CHECK_EQ(page.keys.size(), 2u);
        CHECK_EQ(page.keys[0], std::string("a&b"));
        CHECK_EQ(page.nextMarker, std::string("t&1"));
        for (const char *bad : {"", "<Error/>", "<ListBucketResult></ListBucketResult>", "<ListBucketResult><IsTruncated>true</IsTruncated></ListBucketResult>",
                                "<ListBucketResult><IsTruncated>maybe</IsTruncated></ListBucketResult>",
                                "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Size>1</Size></Contents></ListBucketResult>",
                                "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Key>a&bad;</Key></Contents></ListBucketResult>"}) {
            CHECK_FALSE(oss::parseListPage(bad, page));
        }
    }

    TEST_CASE("OSS configuration: off by default, explicit to enable, and never prints a value") {
        const auto clearAll = []() {
            for (const char *name : {"MEDIA_ENABLED", "OSS_ENDPOINT", "OSS_REGION", "OSS_BUCKET", "OSS_UPLOAD_KEY_ID", "OSS_UPLOAD_KEY_SECRET", "OSS_READ_KEY_ID", "OSS_READ_KEY_SECRET", "OSS_SERVER_KEY_ID", "OSS_SERVER_KEY_SECRET"}) ::unsetenv(name);
        };
        std::string problem;
        clearAll();
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK_EQ(problem, std::string());
        // Credentials alone do not enable anything.
        ::setenv("OSS_SERVER_KEY_ID", "sentinel-id", 1);
        ::setenv("OSS_SERVER_KEY_SECRET", "sentinel-secret", 1);
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK_EQ(problem, std::string());
        ::setenv("MEDIA_ENABLED", "false", 1);
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK_EQ(problem, std::string());
        // Enabled but incomplete: a problem, naming variables only.
        ::setenv("MEDIA_ENABLED", "true", 1);
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK(!problem.empty());
        CHECK(problem.find("sentinel") == std::string::npos);
        ::setenv("OSS_ENDPOINT", kEndpoint.c_str(), 1);
        ::setenv("OSS_REGION", kRegion.c_str(), 1);
        ::setenv("OSS_BUCKET", kBucket.c_str(), 1);
        ::setenv("OSS_UPLOAD_KEY_ID", "sentinel-id", 1);
        ::setenv("OSS_UPLOAD_KEY_SECRET", "sentinel-secret-u", 1);
        ::setenv("OSS_READ_KEY_ID", "sentinel-id-r", 1);
        ::setenv("OSS_READ_KEY_SECRET", "sentinel-secret-r", 1);
        // Two jobs on one identity undo the separation.
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK(problem.find("three different identities") != std::string::npos);
        CHECK(problem.find("sentinel") == std::string::npos);
        ::setenv("OSS_SERVER_KEY_ID", "sentinel-id-s", 1);
        const auto ready = usapp::media::ossConfigFromEnv(problem);
        CHECK(ready.has_value());
        CHECK_EQ(problem, std::string());
        if (ready) {
            CHECK(ready->testCaFile.empty());
            CHECK(ready->testConnectTo.empty());
            CHECK(!ready->testClock);
        }
        // A scheme, path or odd bucket is refused rather than interpreted.
        ::setenv("OSS_ENDPOINT", "https://oss.example.test/path", 1);
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK(problem.find("OSS_ENDPOINT") != std::string::npos);
        ::setenv("OSS_ENDPOINT", kEndpoint.c_str(), 1);
        ::setenv("OSS_BUCKET", "bad.bucket", 1);
        CHECK_FALSE(usapp::media::ossConfigFromEnv(problem).has_value());
        CHECK(problem.find("OSS_BUCKET") != std::string::npos);
        clearAll();
    }

    TEST_CASE("OSS grant: one exact key, type and size; signed by the upload signer; no secret in the form") {
        Rig rig;
        const UploadForm form = rig.store->signUpload(kStaging, "image/png", 5, 1893456000);
        CHECK_EQ(form.url, "https://" + kHost + "/");
        CHECK_EQ(form.fields.at("key"), kStaging);
        CHECK_EQ(form.fields.at("Content-Type"), std::string("image/png"));
        CHECK_EQ(form.fields.at("success_action_status"), std::string("204"));
        CHECK(form.fields.at("x-oss-credential").rfind("UPLOADKEYID/", 0) == 0u);
        for (const auto &field : form.fields) {
            for (const char *secret : {"UPLOADSECRET", "READSECRET", "SERVERSECRET", "SERVERKEYID", "READKEYID"}) CHECK(field.second.find(secret) == std::string::npos);
        }
        // What the policy states.
        unsigned char decoded[4096];
        const auto &policyText = form.fields.at("policy");
        const int length = EVP_DecodeBlock(decoded, reinterpret_cast<const unsigned char *>(policyText.data()), static_cast<int>(policyText.size()));
        std::string json(reinterpret_cast<char *>(decoded), static_cast<size_t>(length));
        while (!json.empty() && json.back() == '\0') json.pop_back();
        Json::Value policy;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        CHECK(reader->parse(json.data(), json.data() + json.size(), &policy, &errors));
        CHECK_EQ(policy["expiration"].asString(), std::string("2030-01-01T00:00:00.000Z"));
        std::map<std::string, std::string> equal;
        Json::Int64 low = -1, high = -1;
        std::string bucket;
        for (const auto &condition : policy["conditions"]) {
            if (condition.isObject()) bucket = condition["bucket"].asString();
            else if (condition[0].asString() == "eq") equal[condition[1].asString()] = condition[2].asString();
            else if (condition[0].asString() == "content-length-range") { low = condition[1].asInt64(); high = condition[2].asInt64(); }
            else CHECK(false);  // only exact conditions: no starts-with
        }
        CHECK_EQ(bucket, kBucket);
        CHECK_EQ(equal.at("$key"), kStaging);
        CHECK_EQ(equal.at("$Content-Type"), std::string("image/png"));
        CHECK_EQ(equal.at("$success_action_status"), std::string("204"));
        CHECK_EQ(low, 5);
        CHECK_EQ(high, 5);
        CHECK_EQ(equal.count("$success_action_redirect"), 0u);

        // Against the model: the exact file is accepted; anything else is refused.
        const std::string &ca = pki().caFile;
        const std::string connect = rig.server->connectTo();
        CHECK_EQ(clientPost(form, "12345", ca, connect).status, 204L);
        CHECK_EQ(rig.model->objects.at(kStaging).bytes, std::string("12345"));
        CHECK_EQ(clientPost(form, "1234", ca, connect).status, 400L);
        CHECK_EQ(clientPost(form, "123456", ca, connect).status, 400L);
        CHECK_EQ(clientPost(form, "12345", ca, connect, {}, {{"key", kFinal}}).status, 403L);
        CHECK_EQ(clientPost(form, "12345", ca, connect, {}, {{"key", "staging/c/other/upload"}}).status, 403L);
        CHECK_EQ(clientPost(form, "12345", ca, connect, {}, {{"Content-Type", "text/html"}}).status, 403L);
        CHECK_EQ(rig.model->objects.count(kFinal), 0u);
        CHECK_EQ(rig.model->objects.count("staging/c/other/upload"), 0u);
    }

    TEST_CASE("OSS calls: every request is signed by the server identity, and answers map as the workflow expects") {
        Rig rig;
        rig.seed(kStaging, syntheticPng(false));
        rig.seed("staging/c/x/upload", "x");
        const auto present = rig.head(kStaging);
        CHECK(present.first == StoreStatus::Ok);
        CHECK_EQ(present.second.size, static_cast<int64_t>(syntheticPng(false).size()));
        CHECK(!present.second.etag.empty());
        CHECK_EQ(present.second.crc64, std::string("12345"));
        CHECK(rig.head("staging/none").first == StoreStatus::NotFound);
        CHECK(rig.copy() == StoreStatus::Ok);
        CHECK(rig.copy("staging/none", "media/c/m/a9/original") == StoreStatus::NotFound);
        CHECK_EQ(rig.model->objects.at(kFinal).bytes, syntheticPng(false));
        const auto info = rig.info(kFinal);
        CHECK(info.first == StoreStatus::Ok);
        CHECK_EQ(info.second.format, std::string("image/png"));
        CHECK_EQ(info.second.width, 4032);
        const auto listed = rig.list("staging/");
        CHECK(listed.first == StoreStatus::Ok);
        CHECK_EQ(listed.second.keys.size(), 2u);
        CHECK_EQ(rig.list("media/").second.keys.size(), 1u);
        CHECK(rig.remove("staging/c/x/upload") == StoreStatus::Ok);
        CHECK(rig.remove("staging/c/x/upload") == StoreStatus::Ok);  // absent: still success
        CHECK_EQ(rig.model->refusedSignatures, 0);
        for (const auto &request : rig.server->requests()) {
            CHECK(request.headers.at("authorization").find("Credential=SERVERKEYID/") != std::string::npos);
            CHECK_EQ(request.headers.at("host"), kHost);
            CHECK_EQ(request.headers.at("x-oss-content-sha256"), std::string("UNSIGNED-PAYLOAD"));
            CHECK_EQ(request.headers.count("cookie"), 0u);
            CHECK_EQ(request.headers.count("x-csrf-token"), 0u);
        }
        // The model refuses a wrong signature, so the zero above means something.
        Recorded forged;
        forged.method = "HEAD";
        forged.path = "/" + kStaging;
        forged.headers = {{"authorization", "OSS4-HMAC-SHA256 Credential=SERVERKEYID/20260301/cn-stub/oss/aliyun_v4_request, Signature=00"}, {"x-oss-date", "20260301T000000Z"}, {"x-oss-content-sha256", "UNSIGNED-PAYLOAD"}};
        CHECK_EQ(rig.model->handle(forged).status, 403);
    }

    TEST_CASE("OSS image info: animation and refusals never pass as a photo, and a refusal is not a rejection") {
        const std::string animated = "{\"Format\":{\"value\":\"webp\"},\"ImageWidth\":{\"value\":\"1\"},\"ImageHeight\":{\"value\":\"1\"},\"FrameCount\":{\"value\":\"4\"}}";
        {
            FixedRig rig(respond(200, animated));
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            rig.store->imageInfo(kFinal, [waiter](StoreStatus s, ImageFacts) { waiter->deliver(s); });
            CHECK(waiter->wait() == StoreStatus::Invalid);
        }
        // The vendor's answer for a non-image is undocumented: a 400 stays Unavailable, so an outage can never become a rejection.
        for (const int status : {400, 403, 500, 503}) {
            FixedRig rig(respond(status, "<Error><Code>x</Code></Error>"));
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            rig.store->imageInfo(kFinal, [waiter](StoreStatus s, ImageFacts) { waiter->deliver(s); });
            CHECK(waiter->wait() == StoreStatus::Unavailable);
        }
        {
            FixedRig rig(respond(200, "not json"));
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            rig.store->imageInfo(kFinal, [waiter](StoreStatus s, ImageFacts) { waiter->deliver(s); });
            CHECK(waiter->wait() == StoreStatus::Unavailable);
        }
        // A HEAD missing what the workflow compares is not usable.
        for (const auto &headers : std::vector<std::map<std::string, std::string>>{{{"Content-Length", "5"}}, {{"ETag", "\"a\""}, {"Content-Length", "x"}}}) {
            FixedRig rig(respond(200, "", headers));
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            rig.store->head(kFinal, [waiter](StoreStatus s, ObjectFacts) { waiter->deliver(s); });
            CHECK(waiter->wait() == StoreStatus::Unavailable);
        }
        {
            // A delete answered with anything but 204, and a listing that does not parse.
            FixedRig rig(respond(200, "<html>"));
            auto removed = std::make_shared<Waiter<StoreStatus>>();
            rig.store->remove(kFinal, [removed](StoreStatus s) { removed->deliver(s); });
            CHECK(removed->wait() == StoreStatus::Unavailable);
            auto listed = std::make_shared<Waiter<StoreStatus>>();
            rig.store->list("media/", "", 10, [listed](StoreStatus s, KeyPage) { listed->deliver(s); });
            CHECK(listed->wait() == StoreStatus::Unavailable);
        }
    }

    TEST_CASE("OSS TLS: an untrusted, expired or wrongly named certificate gets no request at all") {
        const auto refused = [](const Certificate &leaf, bool useTestCa) {
            StubServer server(leaf, [](const Recorded &) { return respond(200, "<CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>"); });
            OssConfig config = baseConfig();
            config.testConnectTo = server.connectTo();
            if (!useTestCa) config.testCaFile.clear();
            OssObjectStore store(std::move(config));
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            store.copy(kStaging, kFinal, [waiter](StoreStatus s) { waiter->deliver(s); });
            const auto status = waiter->wait();
            CHECK(status == StoreStatus::Unavailable);
            CHECK_EQ(server.requestCount(), 0u);
        };
        refused(pki().untrusted, true);    // another CA
        refused(pki().wrongName, true);    // right CA, wrong host
        refused(pki().expired, true);      // right CA and host, expired
        refused(pki().good, false);        // no test CA: the system store does not know this one, which shows verification is on by default

        // The control: the same stub and store succeed with a valid certificate.
        StubServer server(pki().good, [](const Recorded &) { return respond(200, "<CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>"); });
        OssConfig config = baseConfig();
        config.testConnectTo = server.connectTo();
        OssObjectStore store(std::move(config));
        auto waiter = std::make_shared<Waiter<StoreStatus>>();
        store.copy(kStaging, kFinal, [waiter](StoreStatus s) { waiter->deliver(s); });
        CHECK(waiter->wait() == StoreStatus::Ok);
        CHECK_EQ(server.requestCount(), 1u);
    }

    TEST_CASE("OSS deadlines: absolute, for a held answer, a trickling body and a stalled handshake, and nothing is sent after one") {
        const long deadline = 300;
        const auto bounded = [deadline](long long elapsed) { CHECK(elapsed >= deadline - 20); CHECK(elapsed < deadline + 1500); };
        {
            Action hold = respond(200, "<CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>");
            hold.kind = Action::Kind::HoldThenRespond;
            hold.holdMs = 1500;
            FixedRig rig(hold, deadline);
            const auto start = std::chrono::steady_clock::now();
            CHECK(rig.copy() == StoreStatus::Unavailable);
            bounded(millisSince(start));
            CHECK_EQ(rig.server->requestCount(), 1u);
        }
        {
            // Steady progress: only an absolute deadline ends this.
            Action trickle;
            trickle.kind = Action::Kind::Trickle;
            FixedRig rig(trickle, deadline);
            const auto start = std::chrono::steady_clock::now();
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            rig.store->imageInfo(kFinal, [waiter](StoreStatus s, ImageFacts) { waiter->deliver(s); });
            CHECK(waiter->wait() == StoreStatus::Unavailable);
            bounded(millisSince(start));
        }
        {
            // Stuck before TLS. When the store comes back, the request must not arrive: this is the failure found in Drogon's client.
            FixedRig rig(respond(200, "<CopyObjectResult><ETag>\"a\"</ETag></CopyObjectResult>"), deadline);
            rig.server->stallHandshakeMs = 800;
            const auto start = std::chrono::steady_clock::now();
            CHECK(rig.copy() == StoreStatus::Unavailable);
            bounded(millisSince(start));
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            CHECK_EQ(rig.server->requestCount(), 0u);
            CHECK_EQ(rig.server->bytesAfterStall.load(), 0);
            rig.server->stallHandshakeMs = 0;
        }
        {
            // Every operation, not just copy, and one slow call does not hold up another.
            Action hold = respond(200);
            hold.kind = Action::Kind::HoldThenRespond;
            hold.holdMs = 1500;
            FixedRig rig(hold, deadline);
            const auto start = std::chrono::steady_clock::now();
            auto a = std::make_shared<Waiter<StoreStatus>>();
            auto b = std::make_shared<Waiter<StoreStatus>>();
            auto c = std::make_shared<Waiter<StoreStatus>>();
            auto d = std::make_shared<Waiter<StoreStatus>>();
            rig.store->head(kFinal, [a](StoreStatus s, ObjectFacts) { a->deliver(s); });
            rig.store->imageInfo(kFinal, [b](StoreStatus s, ImageFacts) { b->deliver(s); });
            rig.store->remove(kFinal, [c](StoreStatus s) { c->deliver(s); });
            rig.store->list("media/", "", 10, [d](StoreStatus s, KeyPage) { d->deliver(s); });
            CHECK(a->wait() == StoreStatus::Unavailable);
            CHECK(b->wait() == StoreStatus::Unavailable);
            CHECK(c->wait() == StoreStatus::Unavailable);
            CHECK(d->wait() == StoreStatus::Unavailable);
            bounded(millisSince(start));
        }
        {
            // Nothing is waited on past destruction: every pending call is still answered.
            Action hold = respond(200);
            hold.kind = Action::Kind::HoldThenRespond;
            hold.holdMs = 1000;
            auto waiter = std::make_shared<Waiter<StoreStatus>>();
            {
                FixedRig rig(hold, 5000);
                rig.store->copy(kStaging, kFinal, [waiter](StoreStatus s) { waiter->deliver(s); });
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            CHECK(waiter->wait(100) == StoreStatus::Unavailable);
        }
    }

    TEST_CASE("OSS copy: exactly one request per attempt, whatever happens to it") {
        const auto once = [](Action action, bool closeBefore) {
            FixedRig rig(action, 1500);
            rig.server->closeBeforeRequest = closeBefore;
            CHECK(rig.copy() == StoreStatus::Unavailable);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            CHECK_EQ(rig.server->connections.load(), 1);
            CHECK(rig.server->requestCount() <= 1u);
            if (!closeBefore) CHECK_EQ(rig.server->requestCount(), 1u);
        };
        Action closed;
        closed.kind = Action::Kind::CloseWithoutResponse;
        once(closed, false);                // read it, then gone: the response is lost
        once(closed, true);                 // gone before it was read
        once(respond(500, "<Error/>"), false);
        once(respond(503, "<Error/>"), false);
        once(respond(302, "", {{"Location", "https://elsewhere.example.test/"}}), false);  // not followed
        once(respond(200, ""), false);
        once(respond(200, "<html>gateway</html>"), false);
        once(respond(200, "<Error><Code>InternalError</Code></Error>"), false);
        once(respond(200, std::string(70000, 'x')), false);  // over the response cap
    }

    TEST_CASE("OSS copy: success with a lost response takes effect in the store, and the adapter reports it as unknown") {
        Rig rig;
        rig.seed(kStaging, "bytes");
        rig.model->loseNextCopyResponse = true;
        CHECK(rig.copy() == StoreStatus::Unavailable);
        CHECK_EQ(rig.model->objects.at(kFinal).bytes, std::string("bytes"));
        CHECK_EQ(rig.model->copiesPerDestination.at(kFinal), 1);
        // Wait out anything a client might resend by itself.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ(rig.model->copiesPerDestination.at(kFinal), 1);
        CHECK_EQ(rig.server->requestCount(), 1u);
        // A malformed 200 is unknown in the same way.
        rig.model->malformNextCopy = "<html>200 from something</html>";
        CHECK(rig.copy(kStaging, "media/c/m/a2/original") == StoreStatus::Unavailable);
        CHECK_EQ(rig.model->copiesPerDestination.at("media/c/m/a2/original"), 1);
    }

    TEST_CASE("OSS metadata: nothing set during a staging upload controls the finalized object's read headers") {
        Rig rig;
        const std::string &ca = pki().caFile;
        const std::string connect = rig.server->connectTo();
        const std::string bytes = syntheticJpeg(false);
        const UploadForm form = rig.store->signUpload(kStaging, "image/jpeg", static_cast<int64_t>(bytes.size()), 1893456000);
        const std::map<std::string, std::string> hostile = {
            {"Content-Disposition", "inline; filename=evil.html"}, {"Cache-Control", "public, max-age=99999999"}, {"Content-Language", "evil"},
            {"x-oss-meta-evil", "1"}, {"x-oss-object-acl", "public-read"}, {"Expires", "Fri, 01 Jan 2100 00:00:00 GMT"},
        };
        // Worst case: the model stores what the policy did not mention.
        CHECK_EQ(clientPost(form, bytes, ca, connect, hostile).status, 204L);
        CHECK_EQ(rig.model->objects.at(kStaging).metadata.at("Content-Disposition"), std::string("inline; filename=evil.html"));

        // The control: the model's default, COPY, does carry them, so this test can tell the difference.
        {
            std::lock_guard<std::mutex> guard(rig.model->mutex_);
            rig.model->verifySignatures = false;
        }
        Recorded plain;
        plain.method = "PUT";
        plain.path = "/media/c/m/control/original";
        plain.headers = {{"x-oss-copy-source", "/" + kBucket + "/" + kStaging}};
        rig.model->handle(plain);
        CHECK_EQ(rig.model->objects.at("media/c/m/control/original").metadata.count("Cache-Control"), 1u);
        rig.model->verifySignatures = true;

        // The adapter: REPLACE, with one neutral type and nothing else.
        CHECK(rig.copy() == StoreStatus::Ok);
        const auto sent = rig.server->requests().back();
        CHECK_EQ(sent.headers.at("x-oss-metadata-directive"), std::string("REPLACE"));
        const auto &stored = rig.model->objects.at(kFinal).metadata;
        CHECK_EQ(stored.size(), 1u);
        CHECK_EQ(stored.at("Content-Type"), std::string("application/octet-stream"));

        // Reads: the URL decides, and a variant is labelled for what it is.
        const auto fetch = [&](const std::string &variant) { return clientFetch(rig.store->signRead(kFinal, variant, "image/jpeg", 1893456000), ca, connect); };
        const auto view = fetch("view");
        CHECK_EQ(view.status, 200L);
        CHECK_EQ(view.headers.at("content-type"), std::string("image/webp"));
        CHECK_EQ(view.headers.at("content-disposition"), std::string("inline"));
        CHECK_EQ(view.headers.at("cache-control"), std::string("private, max-age=600"));
        CHECK_EQ(view.headers.count("content-language"), 0u);
        CHECK_EQ(view.headers.count("x-oss-meta-evil"), 0u);
        const auto original = fetch("original");
        CHECK_EQ(original.status, 200L);
        CHECK_EQ(original.headers.at("content-type"), std::string("application/octet-stream"));
        CHECK_EQ(original.headers.at("content-disposition"), std::string("attachment"));
        CHECK_EQ(original.headers.count("x-oss-meta-evil"), 0u);
        // An unexpected format is never echoed into a type a browser would render.
        const auto odd = clientFetch(rig.store->signRead(kFinal, "original", "text/html", 1893456000), ca, connect);
        CHECK_EQ(odd.headers.at("content-type"), std::string("application/octet-stream"));
    }

    TEST_CASE("OSS signed read: bound to one key, one transform, one method, signed by the read signer") {
        Rig rig;
        const std::string &ca = pki().caFile;
        const std::string connect = rig.server->connectTo();
        rig.seed(kFinal, syntheticPng(false));
        rig.seed("media/c/other/a/original", "another couple");
        rig.seed(kStaging, "staging");
        const int64_t expires = 1893456000;
        const std::string url = rig.store->signRead(kFinal, "thumb", "image/png", expires);
        CHECK(url.rfind("https://" + kHost + "/" + kFinal + "?", 0) == 0u);
        CHECK_EQ(queryValue(url, "x-oss-process"), std::string("image/auto-orient,1/resize,w_480/format,webp"));
        CHECK_EQ(queryValue(rig.store->signRead(kFinal, "view", "image/png", expires), "x-oss-process"), std::string("image/auto-orient,1/resize,w_1600/format,webp"));
        CHECK_EQ(queryValue(rig.store->signRead(kFinal, "original", "image/png", expires), "x-oss-process"), std::string());
        CHECK_EQ(rig.store->signRead(kFinal, "raw", "image/png", expires), std::string());
        CHECK(queryValue(url, "x-oss-credential").rfind("READKEYID/", 0) == 0u);
        CHECK_EQ(queryValue(url, "x-oss-expires"), std::string("600"));
        CHECK_EQ(queryValue(url, "x-oss-additional-headers"), std::string("host"));
        CHECK_EQ(queryValue(url, "x-oss-date"), oss::isoTimestamp(expires - 600));
        for (const char *secret : {"READSECRET", "SERVERKEYID", "UPLOADKEYID"}) CHECK(url.find(secret) == std::string::npos);
        // The same image keeps one URL within a bucket.
        CHECK_EQ(rig.store->signRead(kFinal, "thumb", "image/png", expires), url);
        CHECK(rig.store->signRead(kFinal, "thumb", "image/png", expires + 300) != url);

        CHECK_EQ(clientFetch(url, ca, connect).status, 200L);
        const auto refused = [&](const std::string &altered, const std::string &method = "GET") {
            CHECK(altered != url || method != "GET");
            CHECK_EQ(clientFetch(altered, ca, connect, method).status, 403L);
        };
        refused(replaceOnce(url, kFinal, "media/c/other/a/original"));
        refused(replaceOnce(url, kFinal, kStaging));
        refused(replaceOnce(url, "w_480", "w_9999"));
        refused(replaceOnce(url, "format%2Cwebp", "format%2Cjpg"));
        refused(replaceOnce(url, "x-oss-expires=600", "x-oss-expires=604800"));
        refused(url + "&response-content-type=text%2Fhtml");
        refused(url + "&x-oss-process=image/info");
        refused(url, "PUT");
        refused(url, "DELETE");
    }

    TEST_CASE("OSS synthetic files: the metadata scanner finds EXIF, GPS, XMP and text, and finds nothing in clean files") {
        for (const std::string &file : {syntheticJpeg(true), syntheticPng(true), syntheticWebp(true)}) {
            const auto found = scanMetadata(file);
            CHECK(found.structureOk);
            CHECK(found.exif);
            CHECK(found.gps);
            CHECK(found.xmp || found.text);
        }
        for (const std::string &file : {syntheticJpeg(false), syntheticPng(false), syntheticWebp(false)}) {
            const auto found = scanMetadata(file);
            CHECK(found.structureOk);
            CHECK_FALSE(found.exif);
            CHECK_FALSE(found.gps);
            CHECK_FALSE(found.xmp);
            CHECK_FALSE(found.text);
        }
        CHECK_FALSE(scanMetadata("not an image").structureOk);
        CHECK_FALSE(scanMetadata(syntheticJpeg(true).substr(0, 12)).structureOk);
        // Each format decodes in the model's image/info, and survives a real upload and copy with its bytes intact.
        Rig rig;
        for (const auto &sample : std::vector<std::pair<std::string, std::string>>{{"image/jpeg", syntheticJpeg(true)}, {"image/png", syntheticPng(true)}, {"image/webp", syntheticWebp(true)}}) {
            const auto form = rig.store->signUpload(kStaging, sample.first, static_cast<int64_t>(sample.second.size()), 1893456000);
            CHECK_EQ(clientPost(form, sample.second, pki().caFile, rig.server->connectTo()).status, 204L);
            const std::string finalKey = "media/c/" + sample.first.substr(6) + "/a/original";
            CHECK(rig.copy(kStaging, finalKey) == StoreStatus::Ok);
            CHECK_EQ(rig.info(finalKey).second.format, sample.first);
            CHECK(scanMetadata(rig.model->objects.at(finalKey).bytes).gps);
        }
    }

    return usapptest::finish();
}
