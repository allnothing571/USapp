// =====================================================================
// services/api/src/http/HttpUtil.cc
// =====================================================================
#include "http/HttpUtil.h"

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstring>
#include <ctime>
#include <vector>

#include "core/Crypto.h"

namespace usapp::http {

namespace {

constexpr const char *kCookieAttributes =
    "; Path=/; Secure; HttpOnly; SameSite=Lax";

std::string buildSetCookie(const std::string &name,
                           const std::string &value,
                           int maxAgeSeconds) {
    // 手工拼装 Set-Cookie，而不是依赖框架的默认序列化：
    // 这里必须保证“没有 Domain 属性”，并且属性顺序稳定可审计。
    std::string out = name + "=" + value;
    out += kCookieAttributes;
    out += "; Max-Age=" + std::to_string(maxAgeSeconds < 0 ? 0 : maxAgeSeconds);
    return out;
}

}  // namespace

std::string readCookie(const drogon::HttpRequestPtr &req, const std::string &name) {
    if (!req) {
        return "";
    }
    const auto &cookies = req->getCookies();
    const auto it = cookies.find(name);
    if (it == cookies.end()) {
        return "";
    }
    return it->second;
}

void setSessionCookie(const drogon::HttpResponsePtr &resp,
                      const std::string &name,
                      const std::string &value,
                      int maxAgeSeconds) {
    resp->addHeader("Set-Cookie", buildSetCookie(name, value, maxAgeSeconds));
}

void clearSessionCookie(const drogon::HttpResponsePtr &resp, const std::string &name) {
    resp->addHeader("Set-Cookie", buildSetCookie(name, "", 0));
}

bool isStateChangingMethod(const drogon::HttpMethod &method) {
    switch (method) {
        case drogon::Get:
        case drogon::Head:
        case drogon::Options:
            return false;
        default:
            return true;
    }
}

std::string requestOrigin(const drogon::HttpRequestPtr &req) {
    if (!req) {
        return "";
    }
    return req->getHeader("origin");
}

bool originMatches(const std::string &requestOriginHeader, const std::string &expectedOrigin) {
    // 状态修改请求必须带 Origin。缺失即拒绝（而不是回退到 Referer）：
    // 浏览器对跨源非简单请求一定发送 Origin，缺失通常意味着非浏览器客户端。
    if (requestOriginHeader.empty() || expectedOrigin.empty()) {
        return false;
    }
    return requestOriginHeader == expectedOrigin;
}

bool requiresCsrf(const drogon::HttpMethod &method) {
    return isStateChangingMethod(method);
}

std::string peerIp(const drogon::HttpRequestPtr &req) {
    if (!req) {
        return "";
    }
    // 只信任 TCP 层的对端地址。X-Forwarded-For 可被伪造，
    // 管理面判定绝不能基于它。
    return req->getPeerAddr().toIp();
}

namespace {

bool parseIp(const std::string &ip, unsigned char *out, int &family) {
    if (inet_pton(AF_INET, ip.c_str(), out) == 1) {
        family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, ip.c_str(), out) == 1) {
        family = AF_INET6;
        return true;
    }
    return false;
}

bool prefixMatches(const unsigned char *a,
                   const unsigned char *b,
                   int bits,
                   int totalBits) {
    if (bits < 0 || bits > totalBits) {
        return false;
    }
    const int fullBytes = bits / 8;
    const int remBits = bits % 8;
    if (fullBytes > 0 && std::memcmp(a, b, static_cast<size_t>(fullBytes)) != 0) {
        return false;
    }
    if (remBits == 0) {
        return true;
    }
    const unsigned char mask = static_cast<unsigned char>(0xFF << (8 - remBits));
    return (a[fullBytes] & mask) == (b[fullBytes] & mask);
}

}  // namespace

bool ipInCidr(const std::string &ip, const std::string &cidr) {
    if (ip.empty() || cidr.empty()) {
        return false;
    }
    std::string net = cidr;
    int bits = -1;
    const auto slash = cidr.find('/');
    if (slash != std::string::npos) {
        net = cidr.substr(0, slash);
        const std::string bitsStr = cidr.substr(slash + 1);
        if (bitsStr.empty() || bitsStr.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        bits = std::stoi(bitsStr);
    }

    unsigned char ipBuf[16] = {0};
    unsigned char netBuf[16] = {0};
    int ipFamily = 0;
    int netFamily = 0;
    if (!parseIp(ip, ipBuf, ipFamily) || !parseIp(net, netBuf, netFamily)) {
        return false;
    }
    if (ipFamily != netFamily) {
        return false;
    }
    const int totalBits = (ipFamily == AF_INET) ? 32 : 128;
    if (bits < 0) {
        bits = totalBits;  // 无斜杠 = 精确匹配单个地址
    }
    return prefixMatches(ipBuf, netBuf, bits, totalBits);
}

bool ipInAnyCidr(const std::string &ip, const std::vector<std::string> &cidrs) {
    for (const auto &cidr : cidrs) {
        if (ipInCidr(ip, cidr)) {
            return true;
        }
    }
    return false;
}

std::string toIso8601Utc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string nowIso8601Utc() {
    return toIso8601Utc(std::time(nullptr));
}

std::string requestId(const drogon::HttpRequestPtr &req) {
    if (req) {
        const auto incoming = req->getHeader("x-request-id");
        // 只接受形状可控的值，避免把任意客户端输入写进结构化日志。
        if (!incoming.empty() && incoming.size() <= 64 &&
            incoming.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos) {
            return incoming;
        }
    }
    return crypto::randomHex(8);
}

std::string jsonString(const Json::Value &body, const std::string &key) {
    if (!body.isObject() || !body.isMember(key)) {
        return "";
    }
    const auto &v = body[key];
    if (!v.isString()) {
        return "";
    }
    return v.asString();
}

bool isJsonObject(const drogon::HttpRequestPtr &req, Json::Value &out) {
    if (!req) {
        return false;
    }
    const auto body = req->getJsonObject();
    if (!body) {
        return false;
    }
    if (!body->isObject()) {
        return false;
    }
    out = *body;
    return true;
}

}  // namespace usapp::http
