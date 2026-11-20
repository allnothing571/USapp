#pragma once

// P3-1d: local stand-ins for object storage, all on loopback TLS with certificates generated per run. No vendor is contacted.
//   - makeCertificate: a test CA and server certificates, valid, expired or for the wrong name.
//   - StubServer: records each complete request and does what the test asks: answer, hold, close, trickle.
//   - StubOss: an in-memory model of the calls the adapter makes, written from the vendor's documentation, with its own independent
//     V4 signature check. It models documented behaviour; passing against it proves the adapter's side only, and NOTHING about the vendor.
#include <json/json.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace usapptest::ossstub {

// ---------------------------------------------------------------- certificates
struct Certificate {
    std::shared_ptr<X509> cert;
    std::shared_ptr<EVP_PKEY> key;
};

inline std::shared_ptr<EVP_PKEY> newKey() {
    EVP_PKEY *key = EVP_EC_gen("P-256");
    if (!key) throw std::runtime_error("key generation failed");
    return std::shared_ptr<EVP_PKEY>(key, EVP_PKEY_free);
}

// `issuer` empty: a self-signed CA. Otherwise a leaf for dnsName. daysValid negative: already expired.
inline Certificate makeCertificate(const std::string &commonName, const Certificate *issuer, const std::string &dnsName, int daysValid = 2) {
    Certificate out;
    out.key = newKey();
    X509 *x509 = X509_new();
    if (!x509) throw std::runtime_error("certificate allocation failed");
    out.cert = std::shared_ptr<X509>(x509, X509_free);
    static std::atomic<long> serial{1000};
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(x509), daysValid < 0 ? 86400L * (daysValid - 1) : -3600L);
    X509_gmtime_adj(X509_getm_notAfter(x509), 86400L * daysValid);
    X509_set_pubkey(x509, out.key.get());
    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(x509, issuer ? X509_get_subject_name(issuer->cert.get()) : name);
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer ? issuer->cert.get() : x509, x509, nullptr, nullptr, 0);
    const auto addExtension = [&](int id, const std::string &value) {
        X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, id, value.c_str());
        if (!extension) throw std::runtime_error("certificate extension failed");
        X509_add_ext(x509, extension, -1);
        X509_EXTENSION_free(extension);
    };
    addExtension(NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
    if (!issuer) addExtension(NID_key_usage, "critical,keyCertSign,cRLSign");
    if (issuer) addExtension(NID_subject_alt_name, "DNS:" + dnsName);
    if (!X509_sign(x509, issuer ? issuer->key.get() : out.key.get(), EVP_sha256())) throw std::runtime_error("certificate signing failed");
    return out;
}

inline std::string writeCaFile(const Certificate &ca, const std::string &label) {
    static std::atomic<int> sequence{0};
    const auto path = std::filesystem::temp_directory_path() / ("usapp-oss-stub-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence) + "-" + label + ".pem");
    FILE *file = std::fopen(path.string().c_str(), "w");
    if (!file) throw std::runtime_error("cannot write CA file");
    PEM_write_X509(file, ca.cert.get());
    std::fclose(file);
    return path.string();
}

// ---------------------------------------------------------------- server
struct Recorded {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body;
};

struct Action {
    enum class Kind { Respond, CloseWithoutResponse, HoldThenRespond, Trickle } kind = Kind::Respond;
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    int holdMs = 0;
    // Extra journal transport fixtures; defaults preserve existing OSS stub behavior.
    bool omitContentLength = false;
    int trickleContentLength = 100000;
};

class StubServer {
  public:
    using Handler = std::function<Action(const Recorded &)>;

    StubServer(const Certificate &leaf, Handler handler) : handler_(std::move(handler)) {
        ::signal(SIGPIPE, SIG_IGN);
        context_ = SSL_CTX_new(TLS_server_method());
        if (!context_ || SSL_CTX_use_certificate(context_, leaf.cert.get()) != 1 || SSL_CTX_use_PrivateKey(context_, leaf.key.get()) != 1) {
            throw std::runtime_error("stub TLS setup failed");
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(listener_, 64) != 0) {
            throw std::runtime_error("stub listen failed");
        }
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this]() { acceptLoop(); });
    }

    ~StubServer() {
        stopping_ = true;
        acceptor_.join();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            workers.swap(workers_);
        }
        for (auto &worker : workers) worker.join();
        ::close(listener_);
        SSL_CTX_free(context_);
    }

    int port() const { return port_; }
    std::string connectTo() const { return "127.0.0.1:" + std::to_string(port_); }
    std::vector<Recorded> requests() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_;
    }
    size_t requestCount() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_.size();
    }
    void clear() {
        std::lock_guard<std::mutex> guard(mutex_);
        requests_.clear();
    }

    std::atomic<int> connections{0};
    std::atomic<int> handshakesCompleted{0};
    std::atomic<int> bytesAfterStall{0};
    // Accept the connection, then wait this long before TLS: what a stuck store or queue looks like.
    std::atomic<int> stallHandshakeMs{0};
    // Close once TLS is up, before reading anything.
    std::atomic<bool> closeBeforeRequest{false};

  private:
    void acceptLoop() {
        while (!stopping_) {
            pollfd entry{listener_, POLLIN, 0};
            if (::poll(&entry, 1, 25) <= 0) continue;
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) continue;
            ++connections;
            std::lock_guard<std::mutex> guard(mutex_);
            workers_.emplace_back([this, fd]() { serve(fd); });
        }
    }

    bool waitReadable(int fd, int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!stopping_) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            pollfd entry{fd, POLLIN, 0};
            const int ready = ::poll(&entry, 1, static_cast<int>(std::min<long long>(left, 25)));
            if (ready > 0) return true;
        }
        return false;
    }

    void sleepFor(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!stopping_ && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    void serve(int fd) {
        const bool stalled = stallHandshakeMs.load() > 0;
        if (stalled) sleepFor(stallHandshakeMs.load());
        SSL *ssl = SSL_new(context_);
        SSL_set_fd(ssl, fd);
        struct timeval timeout{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (SSL_accept(ssl) == 1) {
            ++handshakesCompleted;
            if (!closeBeforeRequest.load()) serveRequest(ssl, fd, stalled);
        }
        ERR_clear_error();
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(fd);
    }

    void serveRequest(SSL *ssl, int fd, bool stalled) {
        std::string raw;
        char buffer[8192];
        size_t headerEnd = std::string::npos;
        size_t contentLength = 0;
        while (true) {
            if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4 + contentLength) break;
            if (SSL_pending(ssl) == 0 && !waitReadable(fd, 2000)) return;
            const int got = SSL_read(ssl, buffer, sizeof(buffer));
            if (got <= 0) return;
            if (stalled) bytesAfterStall += got;
            raw.append(buffer, static_cast<size_t>(got));
            if (headerEnd == std::string::npos) {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string lower = raw.substr(0, headerEnd);
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    const auto at = lower.find("\r\ncontent-length:");
                    if (at != std::string::npos) contentLength = static_cast<size_t>(std::stoul(lower.substr(at + 17)));
                }
            }
        }
        Recorded request = parse(raw, headerEnd);
        Action action = handler_(request);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            requests_.push_back(request);
        }
        switch (action.kind) {
            case Action::Kind::CloseWithoutResponse:
                return;
            case Action::Kind::HoldThenRespond:
                sleepFor(action.holdMs);
                [[fallthrough]];
            case Action::Kind::Respond:
                writeResponse(ssl, action, request.method == "HEAD");
                return;
            case Action::Kind::Trickle: {
                // Always making progress, never finishing: an idle timeout would never fire on this.
                std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(action.trickleContentLength) + "\r\nConnection: close\r\n\r\n";
                if (SSL_write(ssl, head.data(), static_cast<int>(head.size())) <= 0) return;
                for (int i = 0; i < 400 && !stopping_; ++i) {
                    if (SSL_write(ssl, "x", 1) <= 0) return;
                    sleepFor(25);
                }
                return;
            }
        }
    }

    static void writeResponse(SSL *ssl, const Action &action, bool headOnly) {
        std::string out = "HTTP/1.1 " + std::to_string(action.status) + " Stub\r\n";
        const bool explicitLength = action.headers.count("Content-Length") != 0;
        for (const auto &header : action.headers) out += header.first + ": " + header.second + "\r\n";
        if (!explicitLength && !action.omitContentLength) out += "Content-Length: " + std::to_string(action.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        if (!headOnly) out += action.body;
        SSL_write(ssl, out.data(), static_cast<int>(out.size()));
    }

    static Recorded parse(const std::string &raw, size_t headerEnd) {
        Recorded request;
        const auto lineEnd = raw.find("\r\n");
        const std::string line = raw.substr(0, lineEnd);
        const auto first = line.find(' ');
        const auto second = line.find(' ', first + 1);
        request.method = line.substr(0, first);
        request.target = line.substr(first + 1, second - first - 1);
        const auto mark = request.target.find('?');
        request.path = request.target.substr(0, mark);
        if (mark != std::string::npos) request.query = request.target.substr(mark + 1);
        size_t at = lineEnd + 2;
        while (at < headerEnd) {
            const auto end = raw.find("\r\n", at);
            const std::string header = raw.substr(at, end - at);
            const auto colon = header.find(':');
            if (colon != std::string::npos) {
                std::string name = header.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string value = header.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                request.headers[name] = value;
            }
            at = end + 2;
        }
        request.body = raw.substr(headerEnd + 4);
        return request;
    }

    Handler handler_;
    SSL_CTX *context_ = nullptr;
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    mutable std::mutex mutex_;
    std::vector<std::thread> workers_;
    std::vector<Recorded> requests_;
};

// ---------------------------------------------------------------- an independent V4 check, sharing no code with the adapter
inline std::string rawHmac(const std::string &key, const std::string &text) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(text.data()), text.size(), out, &length);
    return std::string(reinterpret_cast<char *>(out), length);
}
inline std::string hexOf(const std::string &bytes) {
    std::string out;
    char pair[3];
    for (const unsigned char c : bytes) {
        std::snprintf(pair, sizeof(pair), "%02x", c);
        out += pair;
    }
    return out;
}
inline std::string sha256Of(const std::string &text) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), out);
    return hexOf(std::string(reinterpret_cast<char *>(out), sizeof(out)));
}
inline std::string signatureFor(const std::string &secret, const std::string &region, const std::string &timestamp, const std::string &canonical) {
    const std::string date = timestamp.substr(0, 8);
    const std::string key = rawHmac(rawHmac(rawHmac(rawHmac("aliyun_v4" + secret, date), region), "oss"), "aliyun_v4_request");
    const std::string text = "OSS4-HMAC-SHA256\n" + timestamp + "\n" + date + "/" + region + "/oss/aliyun_v4_request\n" + sha256Of(canonical);
    return hexOf(rawHmac(key, text));
}
inline std::string percentDecode(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}
inline std::map<std::string, std::string> splitQuery(const std::string &query) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at < query.size()) {
        auto end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(at, end - at);
        const auto equal = pair.find('=');
        out[pair.substr(0, equal)] = equal == std::string::npos ? std::string() : pair.substr(equal + 1);
        at = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------- an in-memory model
struct StoredObject {
    std::string bytes;
    std::map<std::string, std::string> metadata;
    int version = 0;
};

class StubOss {
  public:
    struct Identities {
        std::map<std::string, std::string> secrets;  // key id -> secret
        std::string region;
        std::string bucket;
        std::string host;
    };

    explicit StubOss(Identities identities) : identities_(std::move(identities)) {}

    Action handle(const Recorded &request) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (request.method == "POST") return post(request);
        const auto query = splitQuery(request.query);
        if (query.count("x-oss-signature")) return signedRead(request, query);
        if (!authorized(request)) return error(403, "SignatureDoesNotMatch");
        const std::string key = percentDecode(request.path.substr(1));
        if (request.method == "HEAD") {
            const auto found = objects.find(key);
            if (found == objects.end()) return error(404, "NoSuchKey");
            Action action;
            action.headers["ETag"] = "\"" + sha256Of(found->second.bytes).substr(0, 32) + "\"";
            action.headers["x-oss-hash-crc64ecma"] = "12345";
            action.headers["Content-Length"] = std::to_string(found->second.bytes.size());
            return action;
        }
        if (request.method == "DELETE") {
            objects.erase(key);
            Action action;
            action.status = 204;
            return action;
        }
        if (request.method == "PUT") return copy(request, key);
        if (query.count("list-type")) return listing(query);
        if (query.count("x-oss-process")) return imageInfo(key);
        return error(400, "Unsupported");
    }

    std::mutex mutex_;
    std::map<std::string, StoredObject> objects;
    std::map<std::string, int> copiesPerDestination;
    int refusedSignatures = 0;
    // Faults, each consumed once.
    bool loseNextCopyResponse = false;  // the copy applies; the answer never comes
    std::string malformNextCopy;        // the copy applies; this 200 body is the answer
    bool verifySignatures = true;       // off only for the model's own control case

  private:
    static Action error(int status, const std::string &code) {
        Action action;
        action.status = status;
        action.headers["Content-Type"] = "application/xml";
        action.body = "<Error><Code>" + code + "</Code></Error>";
        return action;
    }

    bool authorized(const Recorded &request) {
        if (!verifySignatures) return true;
        const auto header = request.headers.find("authorization");
        const auto date = request.headers.find("x-oss-date");
        const auto sha = request.headers.find("x-oss-content-sha256");
        if (header == request.headers.end() || date == request.headers.end() || sha == request.headers.end() || sha->second != "UNSIGNED-PAYLOAD") return refuse();
        const auto credentialAt = header->second.find("Credential=");
        const auto signatureAt = header->second.find("Signature=");
        if (credentialAt == std::string::npos || signatureAt == std::string::npos) return refuse();
        const std::string credential = header->second.substr(credentialAt + 11, header->second.find(',', credentialAt) - credentialAt - 11);
        const auto secret = identities_.secrets.find(credential.substr(0, credential.find('/')));
        if (secret == identities_.secrets.end()) return refuse();
        std::string canonicalHeaders;
        for (const auto &entry : request.headers) {
            if (entry.first == "content-type" || entry.first.rfind("x-oss-", 0) == 0) canonicalHeaders += entry.first + ":" + entry.second + "\n";
        }
        std::vector<std::string> pairs;
        size_t at = 0;
        while (at < request.query.size()) {
            auto end = request.query.find('&', at);
            if (end == std::string::npos) end = request.query.size();
            pairs.push_back(request.query.substr(at, end - at));
            at = end + 1;
        }
        std::sort(pairs.begin(), pairs.end());
        std::string query;
        for (const auto &pair : pairs) query += (query.empty() ? "" : "&") + pair;
        const std::string uri = "/" + identities_.bucket + request.path;
        const std::string canonical = request.method + "\n" + uri + "\n" + query + "\n" + canonicalHeaders + "\n\nUNSIGNED-PAYLOAD";
        if (header->second.substr(signatureAt + 10) != signatureFor(secret->second, identities_.region, date->second, canonical)) return refuse();
        return true;
    }

    bool refuse() {
        ++refusedSignatures;
        return false;
    }

    Action copy(const Recorded &request, const std::string &destination) {
        const auto source = request.headers.find("x-oss-copy-source");
        if (source == request.headers.end()) return error(400, "InvalidArgument");
        const std::string prefix = "/" + identities_.bucket + "/";
        if (source->second.rfind(prefix, 0) != 0) return error(400, "InvalidArgument");
        const auto found = objects.find(percentDecode(source->second.substr(prefix.size())));
        if (found == objects.end()) return error(404, "NoSuchKey");
        // As documented: COPY, the default, carries the source's metadata; REPLACE takes it from this request.
        StoredObject target;
        target.bytes = found->second.bytes;
        target.version = ++copiesPerDestination[destination];
        const auto directive = request.headers.find("x-oss-metadata-directive");
        if (directive != request.headers.end() && directive->second == "REPLACE") {
            const auto type = request.headers.find("content-type");
            if (type != request.headers.end()) target.metadata["Content-Type"] = type->second;
        } else {
            target.metadata = found->second.metadata;
        }
        objects[destination] = std::move(target);
        if (loseNextCopyResponse) {
            loseNextCopyResponse = false;
            Action action;
            action.kind = Action::Kind::CloseWithoutResponse;
            return action;
        }
        Action action;
        action.headers["Content-Type"] = "application/xml";
        if (!malformNextCopy.empty()) {
            action.body = malformNextCopy;
            malformNextCopy.clear();
            return action;
        }
        action.body = "<?xml version=\"1.0\"?><CopyObjectResult><ETag>\"abc\"</ETag><LastModified>2026-03-01T00:00:00.000Z</LastModified></CopyObjectResult>";
        return action;
    }

    Action imageInfo(const std::string &key) {
        const auto found = objects.find(key);
        if (found == objects.end()) return error(404, "NoSuchKey");
        const std::string &bytes = found->second.bytes;
        std::string format;
        if (bytes.rfind("\xFF\xD8", 0) == 0) format = "jpg";
        else if (bytes.rfind("\x89PNG", 0) == 0) format = "png";
        else if (bytes.rfind("RIFF", 0) == 0 && bytes.find("WEBP") == 8u) format = "webp";
        else return error(400, "BadRequest");  // the real answer for a non-image is undocumented
        Action action;
        action.headers["Content-Type"] = "application/json";
        action.body = "{\"FileSize\":{\"value\":\"" + std::to_string(bytes.size()) + "\"},\"Format\":{\"value\":\"" + format +
                      "\"},\"FrameCount\":{\"value\":\"1\"},\"ImageHeight\":{\"value\":\"3024\"},\"ImageWidth\":{\"value\":\"4032\"}}";
        return action;
    }

    Action listing(const std::map<std::string, std::string> &query) {
        const auto prefix = percentDecode(query.count("prefix") ? query.at("prefix") : std::string());
        std::string body = "<?xml version=\"1.0\"?><ListBucketResult><Name>b</Name><IsTruncated>false</IsTruncated>";
        for (const auto &entry : objects) {
            if (entry.first.rfind(prefix, 0) == 0) body += "<Contents><Key>" + entry.first + "</Key></Contents>";
        }
        Action action;
        action.headers["Content-Type"] = "application/xml";
        action.body = body + "</ListBucketResult>";
        return action;
    }

    // Reads answer with the response-* the URL carries over what the object stored, as the vendor documents for overrides.
    Action signedRead(const Recorded &request, const std::map<std::string, std::string> &query) {
        if (request.method != "GET") return error(403, "SignatureDoesNotMatch");
        std::vector<std::string> pairs;
        for (const auto &entry : query) {
            if (entry.first != "x-oss-signature") pairs.push_back(entry.second.empty() ? entry.first : entry.first + "=" + entry.second);
        }
        std::sort(pairs.begin(), pairs.end());
        std::string canonicalQuery;
        for (const auto &pair : pairs) canonicalQuery += (canonicalQuery.empty() ? "" : "&") + pair;
        const std::string credential = percentDecode(query.count("x-oss-credential") ? query.at("x-oss-credential") : std::string());
        const auto secret = identities_.secrets.find(credential.substr(0, credential.find('/')));
        const auto date = query.find("x-oss-date");
        if (secret == identities_.secrets.end() || date == query.end() || !query.count("x-oss-additional-headers")) return error(403, "SignatureDoesNotMatch");
        const std::string canonical = "GET\n/" + identities_.bucket + request.path + "\n" + canonicalQuery + "\nhost:" + identities_.host + "\n\nhost\nUNSIGNED-PAYLOAD";
        if (query.at("x-oss-signature") != signatureFor(secret->second, identities_.region, date->second, canonical)) {
            ++refusedSignatures;
            return error(403, "SignatureDoesNotMatch");
        }
        if (query.count("response-content-type")) return error(400, "InvalidRequest");
        const auto found = objects.find(percentDecode(request.path.substr(1)));
        if (found == objects.end()) return error(404, "NoSuchKey");
        Action action;
        action.body = found->second.bytes;
        for (const auto &entry : found->second.metadata) action.headers[entry.first] = entry.second;
        if (query.count("x-oss-process")) action.headers["Content-Type"] = "image/webp";
        for (const auto &override : std::map<std::string, std::string>{{"response-content-disposition", "Content-Disposition"}, {"response-cache-control", "Cache-Control"}}) {
            const auto value = query.find(override.first);
            if (value != query.end()) action.headers[override.second] = percentDecode(value->second);
        }
        return action;
    }

    // A form POST. Worst case on purpose: fields the policy did not mention are accepted and stored as metadata, because what the vendor
    // does with them is unverified (test N4 in the plan).
    Action post(const Recorded &request) {
        const auto type = request.headers.find("content-type");
        if (type == request.headers.end() || type->second.find("boundary=") == std::string::npos) return error(400, "InvalidArgument");
        const std::string boundary = "--" + type->second.substr(type->second.find("boundary=") + 9);
        std::map<std::string, std::string> fields;
        size_t at = request.body.find(boundary);
        while (at != std::string::npos) {
            const size_t start = at + boundary.size();
            if (request.body.compare(start, 2, "--") == 0) break;
            const auto next = request.body.find(boundary, start);
            if (next == std::string::npos) break;
            const std::string part = request.body.substr(start + 2, next - start - 4);
            const auto split = part.find("\r\n\r\n");
            const std::string head = part.substr(0, split);
            const auto nameAt = head.find("name=\"");
            if (nameAt != std::string::npos) {
                const std::string name = head.substr(nameAt + 6, head.find('"', nameAt + 6) - nameAt - 6);
                fields[name] = part.substr(split + 4);
            }
            at = next;
        }
        if (!fields.count("policy") || !fields.count("x-oss-signature") || !fields.count("x-oss-credential") || !fields.count("x-oss-date") || !fields.count("key") || !fields.count("file")) {
            return error(400, "InvalidArgument");
        }
        const std::string credential = fields["x-oss-credential"];
        const auto secret = identities_.secrets.find(credential.substr(0, credential.find('/')));
        if (secret == identities_.secrets.end()) return error(403, "InvalidAccessKeyId");
        const std::string date = fields["x-oss-date"].substr(0, 8);
        const std::string key = rawHmac(rawHmac(rawHmac(rawHmac("aliyun_v4" + secret->second, date), identities_.region), "oss"), "aliyun_v4_request");
        if (hexOf(rawHmac(key, fields["policy"])) != fields["x-oss-signature"]) {
            ++refusedSignatures;
            return error(403, "SignatureDoesNotMatch");
        }
        // The policy's own conditions, enforced exactly.
        unsigned char decoded[8192];
        const int decodedLength = EVP_DecodeBlock(decoded, reinterpret_cast<const unsigned char *>(fields["policy"].data()), static_cast<int>(fields["policy"].size()));
        if (decodedLength <= 0) return error(400, "InvalidPolicyDocument");
        std::string text(reinterpret_cast<char *>(decoded), static_cast<size_t>(decodedLength));
        while (!text.empty() && text.back() == '\0') text.pop_back();
        Json::Value policy;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if (!reader->parse(text.data(), text.data() + text.size(), &policy, &errors)) return error(400, "InvalidPolicyDocument");
        for (const auto &condition : policy["conditions"]) {
            if (condition.isObject()) {
                if (condition["bucket"].asString() != identities_.bucket) return error(403, "AccessDenied");
            } else if (condition[0].asString() == "eq") {
                if (fields[condition[1].asString().substr(1)] != condition[2].asString()) return error(403, "AccessDenied");
            } else if (condition[0].asString() == "content-length-range") {
                const auto size = static_cast<long long>(fields["file"].size());
                if (size < condition[1].asInt64() || size > condition[2].asInt64()) return error(400, "EntityTooLarge");
            }
        }
        StoredObject stored;
        stored.bytes = fields["file"];
        static const std::set<std::string> kSignature = {"key", "policy", "file", "success_action_status", "x-oss-signature", "x-oss-signature-version", "x-oss-credential", "x-oss-date"};
        for (const auto &field : fields) {
            if (!kSignature.count(field.first)) stored.metadata[field.first] = field.second;
        }
        objects[fields["key"]] = std::move(stored);
        Action action;
        action.status = 204;
        return action;
    }

    Identities identities_;
};

// ---------------------------------------------------------------- synthetic image files: structure only, no real photo
inline std::string be32(uint32_t value) {
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[static_cast<size_t>(i)] = static_cast<char>((value >> (8 * (3 - i))) & 0xff);
    return out;
}
inline std::string le32(uint32_t value) {
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[static_cast<size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    return out;
}
// An EXIF block holding one GPS IFD pointer, tag 0x8825, little-endian.
inline std::string exifWithGps() {
    std::string tiff = "II*";
    tiff.push_back('\0');
    tiff += le32(8);
    tiff += std::string("\x01\x00", 2);                       // one entry
    tiff += std::string("\x25\x88\x04\x00\x01\x00\x00\x00", 8);  // GPSInfo, LONG, count 1
    tiff += le32(26);
    tiff += le32(0);
    return tiff;
}
inline std::string jpegSegment(unsigned char marker, const std::string &payload) {
    const size_t length = payload.size() + 2;
    std::string out;
    out.push_back('\xFF');
    out.push_back(static_cast<char>(marker));
    out.push_back(static_cast<char>((length >> 8) & 0xff));
    out.push_back(static_cast<char>(length & 0xff));
    return out + payload;
}
inline std::string syntheticJpeg(bool withMetadata) {
    std::string out("\xFF\xD8", 2);
    if (withMetadata) {
        out += jpegSegment(0xE1, std::string("Exif\0\0", 6) + exifWithGps());
        out += jpegSegment(0xE1, std::string("http://ns.adobe.com/xap/1.0/\0", 29) + "<x:xmpmeta/>");
        out += jpegSegment(0xFE, "synthetic comment");
    }
    out += jpegSegment(0xDB, std::string(4, '\x01'));
    out += std::string("\xFF\xDA\x00\x02", 4) + "scan" + std::string("\xFF\xD9", 2);
    return out;
}
inline std::string pngChunk(const std::string &type, const std::string &data) { return be32(static_cast<uint32_t>(data.size())) + type + data + be32(0); }
inline std::string syntheticPng(bool withMetadata) {
    std::string out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", std::string(13, '\x01'));
    if (withMetadata) {
        out += pngChunk("eXIf", exifWithGps());
        out += pngChunk("tEXt", std::string("Comment\0synthetic", 17));
    }
    return out + pngChunk("IDAT", "data") + pngChunk("IEND", "");
}
inline std::string webpChunk(const std::string &type, const std::string &data) {
    std::string out = type + le32(static_cast<uint32_t>(data.size())) + data;
    if (data.size() % 2) out.push_back('\0');
    return out;
}
inline std::string syntheticWebp(bool withMetadata) {
    std::string chunks = "WEBP";
    chunks += webpChunk("VP8X", std::string(10, '\0'));
    chunks += webpChunk("VP8 ", "data");
    if (withMetadata) {
        chunks += webpChunk("EXIF", exifWithGps());
        chunks += webpChunk("XMP ", "<x:xmpmeta/>");
    }
    return "RIFF" + le32(static_cast<uint32_t>(chunks.size())) + chunks;
}

// What metadata an image's bytes carry, by walking their structure. This is the check the networked run will apply to real variants.
struct MetadataFound {
    bool structureOk = false;
    bool exif = false;
    bool gps = false;
    bool xmp = false;
    bool text = false;
};
inline bool exifHasGpsPointer(const std::string &tiff) {
    if (tiff.size() < 10 || tiff.compare(0, 2, "II") != 0) return tiff.find(std::string("\x88\x25", 2)) != std::string::npos;
    return tiff.find(std::string("\x25\x88", 2)) != std::string::npos;
}
inline MetadataFound scanMetadata(const std::string &bytes) {
    MetadataFound found;
    if (bytes.rfind("\xFF\xD8", 0) == 0) {
        size_t at = 2;
        while (at + 4 <= bytes.size() && static_cast<unsigned char>(bytes[at]) == 0xFF) {
            const auto marker = static_cast<unsigned char>(bytes[at + 1]);
            if (marker == 0xDA || marker == 0xD9) { found.structureOk = true; break; }
            const size_t length = (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
            if (length < 2 || at + 2 + length > bytes.size()) return found;
            const std::string payload = bytes.substr(at + 4, length - 2);
            if (marker == 0xE1 && payload.rfind(std::string("Exif\0\0", 6), 0) == 0) { found.exif = true; found.gps = found.gps || exifHasGpsPointer(payload.substr(6)); }
            if (marker == 0xE1 && payload.rfind("http://ns.adobe.com/xap", 0) == 0) found.xmp = true;
            if (marker == 0xFE) found.text = true;
            at += 2 + length;
        }
    } else if (bytes.rfind("\x89PNG", 0) == 0) {
        size_t at = 8;
        while (at + 12 <= bytes.size()) {
            const size_t length = (static_cast<unsigned char>(bytes[at]) << 24) | (static_cast<unsigned char>(bytes[at + 1]) << 16) | (static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]);
            const std::string type = bytes.substr(at + 4, 4);
            if (at + 12 + length > bytes.size()) return found;
            if (type == "eXIf") { found.exif = true; found.gps = found.gps || exifHasGpsPointer(bytes.substr(at + 8, length)); }
            if (type == "tEXt" || type == "iTXt" || type == "zTXt") found.text = true;
            if (type == "IEND") found.structureOk = true;
            at += 12 + length;
        }
    } else if (bytes.rfind("RIFF", 0) == 0 && bytes.compare(8, 4, "WEBP") == 0) {
        size_t at = 12;
        while (at + 8 <= bytes.size()) {
            const std::string type = bytes.substr(at, 4);
            const size_t length = static_cast<unsigned char>(bytes[at + 4]) | (static_cast<unsigned char>(bytes[at + 5]) << 8) | (static_cast<unsigned char>(bytes[at + 6]) << 16) | (static_cast<unsigned char>(bytes[at + 7]) << 24);
            if (at + 8 + length > bytes.size()) return found;
            if (type == "EXIF") { found.exif = true; found.gps = found.gps || exifHasGpsPointer(bytes.substr(at + 8, length)); }
            if (type == "XMP ") found.xmp = true;
            at += 8 + length + (length % 2);
        }
        found.structureOk = at == bytes.size();
    }
    return found;
}

}  // namespace usapptest::ossstub
