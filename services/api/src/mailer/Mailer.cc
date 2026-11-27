// =====================================================================
// services/api/src/mailer/Mailer.cc
// =====================================================================
#include "mailer/Mailer.h"

#include <curl/curl.h>
#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>

#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

#include "config/AppConfig.h"

namespace usapp::mailer {

namespace {

std::mutex g_transportMutex;
Mailer::Transport g_transport;

// curl 的读回调：把整条 MIME 报文分块喂给 libcurl。
struct UploadPayload {
    const std::string *data = nullptr;
    std::size_t offset = 0;
};

std::size_t readCallback(char *buffer, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *payload = static_cast<UploadPayload *>(userdata);
    if (payload == nullptr || payload->data == nullptr || size == 0 || nmemb == 0) {
        return 0;
    }
    const std::size_t capacity = size * nmemb;
    const std::string &source = *payload->data;
    const std::size_t remaining = source.size() - payload->offset;
    if (remaining == 0) {
        return 0;
    }
    const std::size_t chunk = remaining < capacity ? remaining : capacity;
    source.copy(buffer, chunk, payload->offset);
    payload->offset += chunk;
    return chunk;
}

// 从 "Name <addr>" 里取出裸地址；没有尖括号就原样返回。
std::string bareAddress(const std::string &value) {
    const std::size_t lt = value.find('<');
    const std::size_t gt = value.find('>');
    if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1) {
        return value.substr(lt + 1, gt - lt - 1);
    }
    return value;
}

}  // namespace

Mailer &Mailer::instance() {
    static Mailer mailer;
    return mailer;
}

bool Mailer::containsCrLf(const std::string &value) {
    return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
}

bool Mailer::deliveringRealMail() const {
    std::lock_guard<std::mutex> lock(g_transportMutex);
    if (g_transport) {
        return true;
    }
    return AppConfig::instance().mailMode == "smtp";
}

void Mailer::setTransportForTest(Transport transport) {
    std::lock_guard<std::mutex> lock(g_transportMutex);
    g_transport = std::move(transport);
}

void Mailer::postToLoop(SendResult result, const std::function<void(SendResult)> &done) {
    if (!done) {
        return;
    }
    // 回调统一回到事件循环线程，避免调用方在别的线程上碰 Drogon 对象。
    trantor::EventLoop *loop = nullptr;
    try {
        loop = drogon::app().getLoop();
    } catch (...) {
        loop = nullptr;
    }
    if (loop == nullptr) {
        done(result);
        return;
    }
    loop->queueInLoop([result, done]() { done(result); });
}

std::string Mailer::buildMessage(const std::string &toEmail, const std::string &code,
                                 int ttlSeconds) {
    const std::string &from = AppConfig::instance().mailFrom;
    const int minutes = (ttlSeconds + 59) / 60;
    std::string message;
    message.reserve(512);
    message += "From: " + from + "\r\n";
    message += "To: " + toEmail + "\r\n";
    // 主题保持 ASCII：避免引入 RFC 2047 编码，减少发送路径上的可变量。
    message += "Subject: JYJ login code\r\n";
    message += "MIME-Version: 1.0\r\n";
    message += "Content-Type: text/plain; charset=UTF-8\r\n";
    message += "Content-Transfer-Encoding: 8bit\r\n";
    message += "\r\n";
    message += "「今与久」(JYJ) 登录验证码：" + code + "\r\n";
    message += "\r\n";
    message += "验证码将在 " + std::to_string(minutes) + " 分钟后失效。\r\n";
    message += "如非本人操作请忽略本邮件，切勿向他人透露验证码。\r\n";
    return message;
}

// SendResult 定义在 usapp::mailer 命名空间作用域（见 Mailer.h），不是
// Mailer 的嵌套类型，因此返回类型必须写裸的 SendResult。
// 写 Mailer::SendResult 会报 "Mailer::SendResult does not name a type"。
// 不移动、不重新定义该结构体，保持既有声明与调用方（AuthService）不变。
SendResult Mailer::writeToLog(const std::string &toEmail, const std::string &code,
                              int ttlSeconds) {
    // 只有 MAIL_MODE=log 才会走到这里；生产环境加载配置时就已拒绝该组合。
    LOG_INFO << "[MAIL_MODE=log] 登录验证码 to=" << toEmail << " code=" << code
             << " ttl_seconds=" << ttlSeconds;
    SendResult result;
    result.ok = true;
    return result;
}

SendResult Mailer::sendViaSmtp(const std::string &toEmail, const std::string &rawMessage) {
    SendResult result;
    const AppConfig &config = AppConfig::instance();

    // 收件人来自参数（已由 canonicalizeEmail 规范化），不从报文里解析。
    if (toEmail.empty() || containsCrLf(toEmail)) {
        result.error = "SMTP 收件人非法";
        return result;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = "curl_easy_init 失败";
        return result;
    }
    struct curl_slist *recipients = nullptr;
    UploadPayload payload{&rawMessage, 0};
    char errorBuffer[CURL_ERROR_SIZE] = {0};

    try {
        const std::string scheme = config.smtpImplicitTls() ? "smtps" : "smtp";
        const std::string url =
            scheme + "://" + config.smtpHost + ":" + std::to_string(config.smtpPort);

        recipients = curl_slist_append(recipients, toEmail.c_str());
        if (recipients == nullptr) {
            curl_easy_cleanup(curl);
            result.error = "构造收件人列表失败";
            return result;
        }

        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, bareAddress(config.mailFrom).c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         static_cast<curl_off_t>(rawMessage.size()));
        // 我们在工作线程里调用 libcurl，必须关掉信号处理。
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(config.mailTimeoutSeconds));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config.mailTimeoutSeconds) * 3L);
        // 永不关闭 TLS 校验：不提供"跳过证书校验"的开关。
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        if (config.smtpImplicitTls()) {
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        } else if (config.smtpStartTls) {
            // 明文端口 + 升级：CURLUSESSL_ALL 表示"必须成功升级，否则失败"。
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        } else {
            // 允许明文投递仅用于本地中继；生产环境由 .env 决定，不在代码里放行。
            curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_NONE));
        }

        if (!config.smtpUser.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, config.smtpUser.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, config.smtpPassword.c_str());
            // 默认 AUTH 机制集合；交由服务端协商。
            curl_easy_setopt(curl, CURLOPT_LOGIN_OPTIONS, "AUTH=*");
        }

        const CURLcode code = curl_easy_perform(curl);
        if (code != CURLE_OK) {
            result.ok = false;
            // 只回短描述，不回完整报文，避免把验证码带进日志。
            result.error = std::string("SMTP 发送失败: ") +
                           (errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code));
            long responseCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
            if (responseCode > 0) {
                result.error += " (response=" + std::to_string(responseCode) + ")";
            }
        } else {
            result.ok = true;
        }
    } catch (const std::exception &e) {
        result.ok = false;
        result.error = std::string("SMTP 发送异常: ") + e.what();
    }

    if (recipients != nullptr) {
        curl_slist_free_all(recipients);
    }
    curl_easy_cleanup(curl);
    return result;
}

SendResult Mailer::deliver(const std::string &toEmail, const std::string &code,
                           int ttlSeconds, const std::string &rawMessage) {
    Transport transport;
    {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        transport = g_transport;
    }
    if (transport) {
        SendResult injected = transport(rawMessage);
        // 注入的传输层也必须显式返回结果，避免"看起来成功"。
        return injected;
    }
    if (AppConfig::instance().mailMode == "smtp") {
        return sendViaSmtp(toEmail, rawMessage);
    }
    return writeToLog(toEmail, code, ttlSeconds);
}

void Mailer::sendVerificationCode(const std::string &toEmail, const std::string &code,
                                  int ttlSeconds, std::function<void(SendResult)> done) {
    auto doneCopy = std::move(done);
    if (toEmail.empty() || code.empty()) {
        postToLoop(SendResult{false, "收件人或验证码为空"}, doneCopy);
        return;
    }
    // 防头注入：地址与验证码都不可能含 CR/LF（canonicalizeEmail 与
    // randomNumericCode 已保证），这里再挡一层，避免以后换了生成器就漏。
    if (containsCrLf(toEmail) || containsCrLf(code) ||
        containsCrLf(AppConfig::instance().mailFrom)) {
        postToLoop(SendResult{false, "邮件字段包含非法换行"}, doneCopy);
        return;
    }

    const std::string message = buildMessage(toEmail, code, ttlSeconds);
    // 网络投递放到独立线程，绝不在事件循环上阻塞，也绝不在持锁事务里。
    std::thread([toEmail, code, ttlSeconds, message, doneCopy]() {
        SendResult result;
        try {
            result = Mailer::instance().deliver(toEmail, code, ttlSeconds, message);
        } catch (const std::exception &e) {
            result.ok = false;
            result.error = std::string("发送邮件抛出异常: ") + e.what();
        } catch (...) {
            result.ok = false;
            result.error = "发送邮件抛出未知异常";
        }
        Mailer::postToLoop(result, doneCopy);
    }).detach();
}

}  // namespace usapp::mailer
