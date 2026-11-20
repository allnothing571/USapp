#include <openssl/rand.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "OssStubClient.h"
#include "services/OssObjectStore.h"

using namespace usapp::media;
using namespace std::chrono_literals;
using usapptest::ossstub::clientFetch;
using usapptest::ossstub::clientPost;

namespace {

constexpr char kBucket[] = "usapp-p31d-test-71";
constexpr char kEndpoint[] = "oss-cn-shanghai.aliyuncs.com";

std::string fixture() {
    constexpr char hex[] = "89504e470d0a1a0a0000000d4948445200000020000000200802000000fc18eda3"
                           "0000002a49444154789c63d04a5e4b53c4306ac1a805a3168c5a306ac1a805a3168"
                           "c5a306ac1a805a3160c150b003e42e83d9edc51900000000049454e44ae426082";
    const auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    std::string bytes;
    for (size_t i = 0; i + 1 < sizeof(hex) - 1; i += 2) {
        bytes.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    return bytes;
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T, typename Start>
T await(Start start) {
    auto promise = std::make_shared<std::promise<T>>();
    auto result = promise->get_future();
    start(promise);
    require(result.wait_for(15s) == std::future_status::ready, "adapter callback timed out");
    return result.get();
}

using Head = std::pair<StoreStatus, ObjectFacts>;
Head head(OssObjectStore &store, const std::string &key) {
    return await<Head>([&](auto p) {
        store.head(key, [p](auto status, auto facts) { p->set_value({status, std::move(facts)}); });
    });
}

bool cleanup(OssObjectStore &store, const std::string &key) {
    try {
        return await<StoreStatus>([&](auto p) {
            store.remove(key, [p](auto status) { p->set_value(status); });
        }) == StoreStatus::Ok;
    } catch (...) {
        return false;
    }
}

// Error bodies can contain signed request material. Print only short OSS identifiers.
std::string errorIdentifier(const std::string &body, const std::string &tag) {
    const auto open = "<" + tag + ">";
    const auto begin = body.find(open);
    if (begin == std::string::npos) return {};
    const auto start = begin + open.size();
    const auto end = body.find("</" + tag + ">", start);
    if (end == std::string::npos || end - start > 64) return {};
    const auto value = body.substr(start, end - start);
    if (value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) return {};
    return value;
}

void reportRead(const char *label, const usapptest::ossstub::ClientResult &read) {
    std::cout << label << " HTTP " << read.status << ", response bytes " << read.body.size() << '\n';
    if (read.status == 200) return;
    for (const auto *tag : {"Code", "EC"}) {
        const auto id = errorIdentifier(read.body, tag);
        if (!id.empty()) std::cout << "OSS " << tag << ": " << id << '\n';
    }
}

}  // namespace

int main() {
    const char *enabled = std::getenv("USAPP_OSS_LIVE");
    if (!enabled || std::string(enabled) != "1") {
        std::cerr << "OSS smoke requires USAPP_OSS_LIVE=1\n";
        return 2;
    }
    std::string problem;
    auto config = ossConfigFromEnv(problem);
    if (!config || config->bucket != kBucket || config->region != "cn-shanghai" || config->endpoint != kEndpoint) {
        std::cerr << "Expected complete configuration for the dedicated Shanghai test bucket\n";
        return 2;
    }

    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) return 2;
    const std::string run = oss::hex(std::string(reinterpret_cast<char *>(random), sizeof(random)));
    const std::string staging = "staging/p31d-smoke/" + run + "/upload";
    const std::string final = "media/p31d-smoke/" + run + "/original";
    const auto bytes = fixture();
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 300;
    config->copyDeadlineMs = 10000;
    OssObjectStore store(std::move(*config));
    bool passed = false;
    std::cout << "Smoke run " << run << ", synthetic PNG " << bytes.size() << " bytes\n";
    try {
        const auto post = [&](const std::string &key) {
            return clientPost(store.signUpload(key, "image/png", bytes.size(), expiry), bytes, "", "").status;
        };
        const long denied = post(final);
        std::cout << "Upload signer final-key probe HTTP " << denied << '\n';
        require(denied == 403, "expected upload signer to be denied on the final key");

        const long uploaded = post(staging);
        std::cout << "Staging upload HTTP " << uploaded << '\n';
        require(uploaded == 204, "staging upload failed");
        const auto source = head(store, staging);
        require(source.first == StoreStatus::Ok && source.second.size == static_cast<int64_t>(bytes.size()),
                "staging HEAD failed or size differs");
        require(await<StoreStatus>([&](auto p) {
            store.copy(staging, final, [p](auto status) { p->set_value(status); });
        }) == StoreStatus::Ok, "copy failed or could not be confirmed");
        const auto before = head(store, final);
        require(before.first == StoreStatus::Ok && before.second.size == source.second.size, "final HEAD failed or size differs");
        using Info = std::pair<StoreStatus, ImageFacts>;
        const auto info = await<Info>([&](auto p) {
            store.imageInfo(final, [p](auto status, auto facts) { p->set_value({status, std::move(facts)}); });
        });
        require(info.first == StoreStatus::Ok && info.second.format == "image/png" &&
                info.second.width == 32 && info.second.height == 32, "final image validation failed");
        const auto after = head(store, final);
        require(after.first == StoreStatus::Ok && !before.second.etag.empty() && before.second.etag == after.second.etag &&
                before.second.size == after.second.size && before.second.crc64 == after.second.crc64, "final object changed during inspection");
        const auto original = clientFetch(store.signRead(final, "original", "image/png", expiry), "", "");
        reportRead("Signed original", original);
        require(original.status == 200 && original.body == bytes, "signed original did not return the copied bytes");
        const auto read = clientFetch(store.signRead(final, "thumb", "image/png", expiry), "", "");
        reportRead("Signed thumbnail", read);
        require(read.status == 200 && read.body.size() >= 12 && read.body.compare(0, 4, "RIFF") == 0 &&
                read.body.compare(8, 4, "WEBP") == 0, "signed thumbnail did not return WebP");
        passed = true;
    } catch (const std::runtime_error &error) {
        std::cerr << "Smoke failed: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Smoke failed: unexpected local error\n";
    }
    const bool stagedClean = cleanup(store, staging);
    const bool finalClean = cleanup(store, final);
    if (!stagedClean || !finalClean) {
        std::cerr << "Cleanup unconfirmed for smoke run " << run << '\n';
        return 1;
    }
    std::cout << "Smoke objects deleted\n";
    if (passed) std::cout << "OSS SMOKE PASSED\n";
    return passed ? 0 : 1;
}
