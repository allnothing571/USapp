#pragma once

// P3-1d: what a browser does against a stub: a form POST and a GET, over verified TLS, with extra fields when a test wants them.
#include <curl/curl.h>

#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "services/MediaStore.h"

namespace usapptest::ossstub {

struct ClientResult {
    long status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

inline size_t clientBody(char *data, size_t size, size_t count, void *user) {
    auto &body = static_cast<ClientResult *>(user)->body;
    const size_t length = size * count;
    if (length > 1024 * 1024 - body.size()) return 0;
    body.append(data, length);
    return length;
}
inline size_t clientHeader(char *data, size_t size, size_t count, void *user) {
    const std::string line(data, size * count);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        for (auto &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        static_cast<ClientResult *>(user)->headers[name] = value;
    }
    return size * count;
}

inline void clientCommon(CURL *curl, const std::string &url, const std::string &caFile, const std::string &connectTo, curl_slist *&mapping, ClientResult &result) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!caFile.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caFile.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, clientBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, clientHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    if (!connectTo.empty()) {
        mapping = curl_slist_append(mapping, ("::" + connectTo).c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_TO, mapping);
    }
}

inline ClientResult clientPost(const usapp::media::UploadForm &form, const std::string &bytes, const std::string &caFile, const std::string &connectTo,
                               const std::map<std::string, std::string> &extras = {}, const std::map<std::string, std::string> &overrides = {}) {
    ClientResult result;
    CURL *curl = curl_easy_init();
    curl_slist *mapping = nullptr;
    clientCommon(curl, form.url, caFile, connectTo, mapping, result);
    curl_mime *mime = curl_mime_init(curl);
    const auto field = [mime](const std::string &name, const std::string &value) {
        curl_mimepart *part = curl_mime_addpart(mime);
        curl_mime_name(part, name.c_str());
        curl_mime_data(part, value.data(), value.size());
    };
    for (const auto &entry : form.fields) {
        const auto replaced = overrides.find(entry.first);
        field(entry.first, replaced == overrides.end() ? entry.second : replaced->second);
    }
    for (const auto &extra : extras) field(extra.first, extra.second);
    curl_mimepart *file = curl_mime_addpart(mime);
    curl_mime_name(file, "file");
    curl_mime_filename(file, "synthetic.png");
    curl_mime_type(file, form.fields.at("Content-Type").c_str());
    curl_mime_data(file, bytes.data(), bytes.size());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_slist *headers = curl_slist_append(nullptr, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_slist_free_all(mapping);
    curl_easy_cleanup(curl);
    return result;
}

inline ClientResult clientFetch(const std::string &url, const std::string &caFile, const std::string &connectTo, const std::string &method = "GET") {
    ClientResult result;
    CURL *curl = curl_easy_init();
    curl_slist *mapping = nullptr;
    clientCommon(curl, url, caFile, connectTo, mapping, result);
    if (method != "GET") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(mapping);
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace usapptest::ossstub
