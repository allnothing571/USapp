#pragma once

// P3-1a: an asynchronous fake enforcing the guarantees in services/MediaStore.h, so tests can break each one on purpose.
// It proves nothing about any vendor: that is P3-1d. A test image's bytes are "type:WIDTHxHEIGHT:payload"; anything else
// does not decode. Callbacks run on its own loop, as a real client's would, never on the caller's stack.
#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Crypto.h"
#include "services/MediaStore.h"

namespace usapptest::media {

namespace m = usapp::media;

class FakeObjectStore : public m::ObjectStore {
  public:
    FakeObjectStore() { loop_.run(); }

    // ---------------------------------------------------------------- what a client can do with a grant
    // G1 and G2: the form's exact key, before its expiry, never a final key.
    bool clientUpload(const m::UploadForm &form, const std::string &key, const std::string &bytes, long long nowEpoch) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto named = form.fields.find("key");
        const auto expiry = form.fields.find("x-test-expires");
        if (named == form.fields.end() || named->second != key || key.rfind("staging/", 0) != 0 || expiry == form.fields.end() ||
            nowEpoch >= std::stoll(expiry->second) || form.fields.at("x-test-signature") != signatureFor(key, expiry->second)) {
            ++refusedClientWrites;
            return false;
        }
        objects_[key] = bytes;
        return true;
    }
    // A delayed request, as a late upload would be, is past the checks already.
    void lateClientWrite(const std::string &key, const std::string &bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        objects_[key] = bytes;
    }

    // ---------------------------------------------------------------- what the test can force
    std::function<void(const std::string &op, const std::string &key)> hook;  // runs before each operation takes effect
    std::atomic<int> copyFailButApply{0};                                       // effect happens, failure reported
    std::atomic<int> copyFailThenLandLater{0};                                  // failure reported; effect waits for landLate()
    std::atomic<int> storeDown{0};                                              // Unavailable, no effect
    std::atomic<bool> holdCopies{false};                                        // effect and report wait for releaseHeld()
    std::atomic<bool> holdImageInfo{false};                                     // report waits for releaseHeld()
    std::atomic<bool> dropCopyCallback{false};                                  // process stopped: report never arrives
    std::atomic<bool> removeSaysNotFound{false};                                // a store that answers NotFound for an absent key

    // D6: this exact key fails to delete, with no effect, until the test clears it.
    void failRemovalOf(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex_);
        failingRemovals_.insert(key);
    }
    void clearRemovalFailures() {
        std::lock_guard<std::mutex> guard(mutex_);
        failingRemovals_.clear();
        lostRemovalReplies_.clear();
    }
    // J2c2: this exact key IS deleted, once, but the reply is lost: Unavailable, as a timeout would look.
    void loseRemovalReplyOf(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex_);
        lostRemovalReplies_.insert(key);
    }
    // Every delete request that reached the store, in order.
    std::vector<std::string> removalRequests() {
        std::lock_guard<std::mutex> guard(mutex_);
        return removalRequests_;
    }

    void releaseHeld() {
        std::vector<std::function<void()>> held;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            held.swap(held_);
        }
        for (auto &action : held) loop_.getLoop()->queueInLoop(action);
    }
    size_t heldCount() {
        std::lock_guard<std::mutex> guard(mutex_);
        return held_.size();
    }
    void landLate() {
        std::vector<std::pair<std::string, std::string>> pending;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            pending.swap(late_);
            for (const auto &[key, bytes] : pending) writeServer(key, bytes);
        }
    }
    // Breaks G2 on purpose: the object changes behind everything's back.
    void tamper(const std::string &key, const std::string &bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        objects_[key] = bytes;
    }

    // ---------------------------------------------------------------- what the test can read
    std::atomic<int> calls{0};
    std::atomic<int> copies{0};
    std::atomic<int> refusedClientWrites{0};
    std::atomic<int> secondWritesToFinalKey{0};  // always a test failure
    std::atomic<int> signedStagingReads{0};      // always a test failure
    std::atomic<int> readSignings{0};            // every signRead call

    bool exists(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex_);
        return objects_.count(key) > 0;
    }
    std::string bytesAt(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = objects_.find(key);
        return found == objects_.end() ? std::string() : found->second;
    }
    std::vector<std::string> keysUnder(const std::string &prefix) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string> keys;
        for (const auto &[key, bytes] : objects_) if (key.rfind(prefix, 0) == 0) keys.push_back(key);
        return keys;
    }
    // G7: a signed read yields exactly what its key holds; an altered URL is refused.
    std::string followRead(const std::string &url) {
        const auto signAt = url.rfind("&sig=");
        if (url.rfind("fake://", 0) != 0 || signAt == std::string::npos) return "";
        const std::string body = url.substr(0, signAt);
        if (url.substr(signAt + 5) != usapp::crypto::sha256Hex("read:" + body)) return "";
        const auto keyEnd = body.find('?');
        return bytesAt(body.substr(7, keyEnd - 7));
    }

    // ---------------------------------------------------------------- the interface
    m::UploadForm signUpload(const std::string &stagingKey, const std::string &contentType, int64_t size, int64_t expiresAtEpoch) override {
        m::UploadForm form;
        form.url = "https://fake-store.invalid/";
        form.fields["key"] = stagingKey;
        form.fields["Content-Type"] = contentType;
        form.fields["x-test-size"] = std::to_string(size);
        form.fields["x-test-expires"] = std::to_string(expiresAtEpoch);
        form.fields["x-test-signature"] = signatureFor(stagingKey, std::to_string(expiresAtEpoch));
        return form;
    }
    std::string signRead(const std::string &finalKey, const std::string &variant, const std::string &contentType, int64_t expiresAtEpoch) override {
        ++readSignings;
        if (finalKey.rfind("media/", 0) != 0) ++signedStagingReads;
        const std::string body = "fake://" + finalKey + "?variant=" + variant + "&type=" + contentType + "&expires=" + std::to_string(expiresAtEpoch);
        return body + "&sig=" + usapp::crypto::sha256Hex("read:" + body);
    }
    void head(const std::string &key, std::function<void(m::StoreStatus, m::ObjectFacts)> done) override {
        ++calls;
        later([this, key, done]() {
            if (hook) hook("head", key);
            if (takeDown()) { done(m::StoreStatus::Unavailable, {}); return; }
            std::string bytes;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                const auto found = objects_.find(key);
                if (found == objects_.end()) { done(m::StoreStatus::NotFound, {}); return; }
                bytes = found->second;
            }
            m::ObjectFacts facts;
            facts.size = static_cast<int64_t>(reportedSize(bytes));
            facts.etag = usapp::crypto::sha256Hex(bytes);
            facts.crc64 = facts.etag.substr(0, 16);
            done(m::StoreStatus::Ok, facts);
        });
    }
    void imageInfo(const std::string &key, std::function<void(m::StoreStatus, m::ImageFacts)> done) override {
        ++calls;
        auto run = [this, key, done]() {
            if (hook) hook("image_info", key);
            if (takeDown()) { done(m::StoreStatus::Unavailable, {}); return; }
            const std::string bytes = bytesAt(key);
            const auto first = bytes.find(':');
            const auto x = bytes.find('x', first == std::string::npos ? 0 : first);
            const auto second = bytes.find(':', first == std::string::npos ? 0 : first + 1);
            if (bytes.rfind("image/", 0) != 0 || first == std::string::npos || x == std::string::npos || second == std::string::npos) {
                done(m::StoreStatus::Invalid, {});
                return;
            }
            m::ImageFacts facts;
            facts.format = bytes.substr(0, first);
            facts.width = std::stoi(bytes.substr(first + 1, x - first - 1));
            facts.height = std::stoi(bytes.substr(x + 1, second - x - 1));
            done(m::StoreStatus::Ok, facts);
        };
        if (holdImageInfo) {
            std::lock_guard<std::mutex> guard(mutex_);
            held_.push_back(run);
            return;
        }
        later(run);
    }
    // G3. The source is read when the request reaches the store. Its effect may be separated from its report.
    void copy(const std::string &source, const std::string &destination, std::function<void(m::StoreStatus)> done) override {
        ++calls;
        ++copies;
        later([this, source, destination, done]() {
            if (hook) hook("copy", destination);
            if (takeDown()) { done(m::StoreStatus::Unavailable); return; }
            std::string bytes;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                const auto found = objects_.find(source);
                if (found == objects_.end()) { done(m::StoreStatus::NotFound); return; }
                bytes = found->second;
            }
            int expected = copyFailThenLandLater.load();
            if (expected > 0 && copyFailThenLandLater.compare_exchange_strong(expected, expected - 1)) {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    late_.push_back({destination, bytes});
                }
                done(m::StoreStatus::Unavailable);
                return;
            }
            if (holdCopies) {
                std::lock_guard<std::mutex> guard(mutex_);
                held_.push_back([this, destination, bytes, done]() { apply(destination, bytes); done(m::StoreStatus::Ok); });
                return;
            }
            if (dropCopyCallback) {
                std::lock_guard<std::mutex> guard(mutex_);
                late_.push_back({destination, bytes});
                return;
            }
            apply(destination, bytes);
            expected = copyFailButApply.load();
            if (expected > 0 && copyFailButApply.compare_exchange_strong(expected, expected - 1)) {
                done(m::StoreStatus::Unavailable);
                return;
            }
            done(m::StoreStatus::Ok);
        });
    }
    void remove(const std::string &key, std::function<void(m::StoreStatus)> done) override {
        ++calls;
        later([this, key, done]() {
            if (hook) hook("remove", key);
            bool absent = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                removalRequests_.push_back(key);
            }
            if (takeDown()) { done(m::StoreStatus::Unavailable); return; }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (failingRemovals_.count(key) > 0u) {
                    loop_.getLoop()->queueInLoop([done]() { done(m::StoreStatus::Unavailable); });
                    return;
                }
                absent = objects_.erase(key) == 0u;
                if (lostRemovalReplies_.erase(key) > 0u) {
                    loop_.getLoop()->queueInLoop([done]() { done(m::StoreStatus::Unavailable); });
                    return;
                }
            }
            done(absent && removeSaysNotFound ? m::StoreStatus::NotFound : m::StoreStatus::Ok);
        });
    }
    void list(const std::string &prefix, const std::string &marker, int limit, std::function<void(m::StoreStatus, m::KeyPage)> done) override {
        later([this, prefix, marker, limit, done]() {
            m::KeyPage page;
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto at = objects_.upper_bound(marker); at != objects_.end(); ++at) {
                if (at->first.rfind(prefix, 0) != 0) continue;
                if (static_cast<int>(page.keys.size()) >= limit) { page.nextMarker = page.keys.back(); break; }
                page.keys.push_back(at->first);
            }
            loop_.getLoop()->queueInLoop([done, page]() { done(m::StoreStatus::Ok, page); });
        });
    }

  private:
    static std::string signatureFor(const std::string &key, const std::string &expires) {
        return usapp::crypto::sha256Hex("upload:" + key + ":" + expires);
    }
    // A fixture states its size by a final ";size=N" so tests can use short bytes for any declared size.
    static size_t reportedSize(const std::string &bytes) {
        const auto at = bytes.rfind(";size=");
        return at == std::string::npos ? bytes.size() : static_cast<size_t>(std::stoull(bytes.substr(at + 6)));
    }
    void later(std::function<void()> action) { loop_.getLoop()->queueInLoop(std::move(action)); }
    bool takeDown() {
        int expected = storeDown.load();
        return expected > 0 && storeDown.compare_exchange_strong(expected, expected - 1);
    }
    void apply(const std::string &key, const std::string &bytes) {
        std::lock_guard<std::mutex> guard(mutex_);
        writeServer(key, bytes);
    }
    void writeServer(const std::string &key, const std::string &bytes) {
        if (!writtenFinalKeys_.insert_or_assign(key, true).second) ++secondWritesToFinalKey;
        objects_[key] = bytes;
    }

    trantor::EventLoopThread loop_;
    std::mutex mutex_;
    std::map<std::string, std::string> objects_;
    std::map<std::string, bool> writtenFinalKeys_;
    std::vector<std::function<void()>> held_;
    std::vector<std::pair<std::string, std::string>> late_;
    std::set<std::string> failingRemovals_;
    std::set<std::string> lostRemovalReplies_;
    std::vector<std::string> removalRequests_;
};

}  // namespace usapptest::media
