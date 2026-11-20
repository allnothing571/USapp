// =====================================================================
// services/api/src/services/MediaStore.h
//
// Phase 3 P3-1a: what the media workflow requires of object storage, stated as guarantees rather than vendor calls
// (docs/phase3-plan.md Section 4.2). The test fake is the default for tests; OssObjectStore is opt-in, has only met local stubs, and
// each guarantee must be proved against the real vendor before it may be enabled for real data:
//   G1  A signed upload writes only its exact staging key, within its type and size, and nothing after expiry, except a
//       request already in flight.
//   G2  No client credential can write a final key.
//   G3  copy is atomic: the destination appears whole, holding one version of the source, or not at all. It may succeed
//       and report failure, or take effect after reporting failure. The caller sends it once per key and never retries it.
//   G4  After a reported copy, head and imageInfo read that object.
//   G5  An etag changes if the bytes change. Used only as a consistency check.
//   G6  remove is idempotent.
//   G7  A signed read names one key and one variant, and cannot write.
//   G8  list eventually shows every object under a prefix. Late objects are only found through it.
// All calls are asynchronous and must not block: they run from event-loop and ORM threads.
// =====================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace usapp::media {

// Unavailable says nothing about whether the request took effect. Invalid: the bytes are not a decodable image.
enum class StoreStatus { Ok, NotFound, Unavailable, Invalid };

struct ObjectFacts {
    int64_t size = 0;
    std::string etag;
    std::string crc64;
};

struct ImageFacts {
    std::string format;  // a media type, from decoding the bytes
    int width = 0;
    int height = 0;
};

struct UploadForm {
    std::string url;
    std::map<std::string, std::string> fields;
};

struct KeyPage {
    std::vector<std::string> keys;
    std::string nextMarker;  // empty: the listing is complete
};

class ObjectStore {
  public:
    virtual ~ObjectStore() = default;

    // Local signing. No network, and no secret in anything returned except the signature.
    virtual UploadForm signUpload(const std::string &stagingKey, const std::string &contentType, int64_t size,
                                  int64_t expiresAtEpoch) = 0;
    // variant is thumb, view or original. contentType overrides whatever the object stored.
    virtual std::string signRead(const std::string &finalKey, const std::string &variant, const std::string &contentType,
                                 int64_t expiresAtEpoch) = 0;

    virtual void head(const std::string &key, std::function<void(StoreStatus, ObjectFacts)> done) = 0;
    virtual void imageInfo(const std::string &key, std::function<void(StoreStatus, ImageFacts)> done) = 0;
    virtual void copy(const std::string &source, const std::string &destination, std::function<void(StoreStatus)> done) = 0;
    virtual void remove(const std::string &key, std::function<void(StoreStatus)> done) = 0;
    virtual void list(const std::string &prefix, const std::string &marker, int limit,
                      std::function<void(StoreStatus, KeyPage)> done) = 0;
};

}  // namespace usapp::media
