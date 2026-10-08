// =====================================================================
// services/api/src/main.cc
//
// USapp 主进程入口（阶段 0 基础 + 阶段 1 增量）。
//
// 启动顺序（每一步失败都 fail fast，绝不带病启动）：
//   1. 日志级别（LOG_LEVEL，默认 info）
//   2. AppConfig::load()        环境变量读取与校验
//   3. 数据库连接池初始化 + 有界等待（等 Postgres 起来，最多 30 秒）
//      随后钱包有效权限检查（最多 10 秒）；失败时在监听/调度前退出。
//   4. 注册两个过滤器（AuthFilter / AdminFilter），供控制器按名引用
//   5. 配置并启动进程内调度（阶段 0 只注册心跳任务，不做任何业务结算）
//   6. 绑定两个监听器：公开（Caddy 反代）与管理（只经宿主回环）
//   7. 默认处理器：未命中的接口统一回 JSON 404（不泄露框架信息）
//   8. run()；返回后停掉调度线程再退出
//
// 本文件不启动任何业务结算、不引入 Redis、不接管资产操作。
// TLS 由 Caddy 终止，本进程只监听明文回环/内网地址。
// =====================================================================

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "config/AppConfig.h"
#include "core/Errors.h"
#include "db/Db.h"
#include "db/WalletStartupCheck.h"
#include "http/AdminFilter.h"
#include "http/AuthFilter.h"
#include "http/HttpUtil.h"
#include "scheduler/Scheduler.h"
#include "scheduler/jobs/SchedulerHeartbeatJob.h"
#include "services/AdminAuthService.h"
#include "services/RegistrationPolicy.h"

namespace {

using usapp::AppConfig;

// 置位后不再重复处理信号：第二次 SIGINT/SIGTERM 不应再动退出流程。
std::atomic<bool> g_stopRequested{false};

// 信号处理器只做一件事：让 Drogon 的事件循环退出。
// 刻意不在处理器里调用 Scheduler::stop() 或任何会 join 线程/加锁的函数，
// 那些属于退出清理，放在 run() 返回之后由主线程执行。
void onTerminateSignal(int /*signo*/) {
    if (g_stopRequested.exchange(true)) {
        return;
    }
    drogon::app().quit();
}

// ---- 日志 ----
// 只允许固定的几个档位，写错就回落到 info 并提示，不静默接受未知值。
void configureLogging() {
    const char *raw = std::getenv("LOG_LEVEL");
    std::string level = raw != nullptr ? std::string(raw) : std::string("info");
    trantor::Logger::LogLevel chosen = trantor::Logger::kInfo;
    if (level == "trace") {
        chosen = trantor::Logger::kTrace;
    } else if (level == "debug") {
        chosen = trantor::Logger::kDebug;
    } else if (level == "info") {
        chosen = trantor::Logger::kInfo;
    } else if (level == "warn" || level == "warning") {
        chosen = trantor::Logger::kWarn;
    } else if (level == "error") {
        chosen = trantor::Logger::kError;
    } else if (level == "fatal") {
        chosen = trantor::Logger::kFatal;
    } else {
        LOG_WARN << "LOG_LEVEL 取值无法识别（" << level << "），回落到 info；"
                 << "可选值：trace/debug/info/warn/error/fatal";
    }
    drogon::app().setLogLevel(chosen);
}

// ---- 数据库 ----
// 为什么不直接 init 完就走：Docker Compose 下 api 常常比 postgres 先就绪，
// 一次性探测失败会让容器反复重启。这里做有界重试（默认 15 次 × 2 秒 = 30 秒），
// 超时仍失败就明确退出——阶段 0 不接受"没有数据库还继续跑"。
// 说明：连接池只创建客户端，真正的连接建立发生在第一次执行 SQL 时，
// 所以这里必须用一次真实查询（SELECT 1）作为就绪探针。
bool waitForDatabase(int maxAttempts) {
    const AppConfig &cfg = AppConfig::instance();
    try {
        usapp::db::Pool::instance().init(cfg.postgresConnectionString(), cfg.postgresPoolSize);
    } catch (const std::exception &e) {
        LOG_ERROR << "数据库连接池初始化失败：" << e.what();
        return false;
    }

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        try {
            usapp::db::Pool::instance().client()->execSqlSync("SELECT 1");
            LOG_INFO << "数据库已就绪（第 " << attempt << " 次探测成功）";
            return true;
        } catch (const std::exception &e) {
            LOG_WARN << "数据库尚不可用（第 " << attempt << "/" << maxAttempts
                     << " 次探测失败）：" << e.what();
            if (attempt < maxAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }
    LOG_ERROR << "等待数据库超时（" << maxAttempts
              << " 次探测均失败）。请先启动 PostgreSQL 并执行 database/migrations 下的迁移，"
              << "再重启本进程。详见 docs/phase0-handoff.md";
    return false;
}

// ---- 调度 ----
// 阶段 0 只注册心跳任务：它证明"进程内调度入口 + pg_try_advisory_xact_lock"
// 这条链路可用，本身不结算任何业务。任务结算属于阶段 1 及以后。
bool startScheduler() {
    const AppConfig &cfg = AppConfig::instance();
    try {
        usapp::scheduler::Scheduler::instance().configure(
            cfg.schedulerEnabled, cfg.schedulerTickIntervalMs, cfg.schedulerInstanceId);
        usapp::scheduler::Scheduler::instance().registerJob(
            std::make_shared<usapp::scheduler::SchedulerHeartbeatJob>());
        usapp::scheduler::Scheduler::instance().start();
    } catch (const std::exception &e) {
        LOG_ERROR << "调度启动失败：" << e.what();
        return false;
    }

    if (!usapp::scheduler::Scheduler::instance().enabled()) {
        LOG_INFO << "调度已配置为关闭（SCHEDULER_ENABLED=false）：不创建调度线程";
    } else {
        LOG_INFO << "调度已启动：间隔 " << cfg.schedulerTickIntervalMs << "ms，实例标识 "
                 << cfg.schedulerInstanceId << "，锁键命名空间 " << cfg.schedulerKeyNamespace
                 << "，已注册任务数 " << usapp::scheduler::Scheduler::instance().jobCount()
                 << "（阶段 0 仅含心跳，不做业务结算）";
    }
    return true;
}

}  // namespace

int main() {
    if (std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ) != 0) {
        std::cerr << "设置 stdout 行缓冲失败" << std::endl;
        return 1;
    }

    configureLogging();

    // ---- 1) 配置 ----
    try {
        AppConfig::instance().load();
    } catch (const std::exception &e) {
        LOG_ERROR << "配置校验失败，进程退出：" << e.what();
        std::cerr << "启动失败：配置不合法，请检查 .env（见 .env.example 与 docs/local-development.md）"
                  << std::endl;
        return 1;
    }
    const AppConfig &cfg = AppConfig::instance();

    usapp::RegistrationPolicy::instance().configure(
        cfg.registrationMode, cfg.allowedEmails);

    LOG_INFO << "USapp API 启动中：version=" << cfg.appVersion << " env=" << cfg.appEnv
             << " (阶段 0：登录/空间绑定/调度基础；不做任务结算与资产操作)";

    // ---- 2) 数据库 ----
    if (!waitForDatabase(15)) {
        return 1;
    }

    // M2-B: require the effective wallet privilege matrix before any listener or job.
    const auto walletStartup =
        usapp::db::checkWalletPrivilegesForStartup(usapp::db::Pool::instance().client());
    if (!walletStartup.allowed()) {
        if (walletStartup.report) {
            LOG_ERROR << walletStartup.report->summary();
        } else {
            LOG_ERROR << "wallet privilege policy: ok=false reason="
                      << (walletStartup.state == usapp::db::WalletStartupResult::State::Timeout
                              ? "timeout" : "query_error");
        }
        return 1;
    }
    LOG_INFO << walletStartup.report->summary();

    // ---- 3) Drogon 运行参数 ----
    // 阶段 0 的请求体都很小（验证码、邀请码、JSON 元数据），这里给一个明确上限：
    // 原图后续走私有 OSS 直传，不经过本服务，所以不需要放大这个值。
    drogon::app().setClientMaxBodySize(64 * 1024);
    // 线程数刻意取小：本地单机部署，连接池与调度都按低并发设计。
    drogon::app().setThreadNum(2);
    drogon::app().setIdleConnectionTimeout(60);

    // ---- 4) 过滤器 ----
    // 显式注册，控制器用 <Filter>::classTypeName() 按名引用。
    // 两个过滤器分别负责公开面与管理面；管理面的四层边界见 AdminFilter.h 头部注释。
    drogon::app().registerFilter(std::make_shared<usapp::AuthFilter>());
    drogon::app().registerFilter(std::make_shared<usapp::AdminFilter>());

    // ---- 5) 未命中接口统一回 JSON 404 ----
    // 不使用框架默认的 HTML 错误页：同类响应形状一致，也不泄露框架与版本信息。
    drogon::app().setDefaultHandler(
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            callback(usapp::errorResponse(usapp::ErrorCode::NotFound, "接口不存在",
                                          0, usapp::http::requestId(req)));
        });

    // ---- 6) 监听 ----
    // 公开监听器：由 Caddy 反代，只发布到容器网络，不对宿主直接发布。
    // 管理监听器：仅经宿主回环 + Caddy 管理站点可达，应用层另有 CIDR 校验。
    // TLS 由 Caddy 终止，本进程两个监听器都是明文。
    try {
        drogon::app().addListener(cfg.publicBind, cfg.publicPort);
        drogon::app().addListener(cfg.adminBind, cfg.adminPort);
    } catch (const std::exception &e) {
        LOG_ERROR << "监听地址绑定失败：" << e.what() << "（public " << cfg.publicBind << ":"
                  << cfg.publicPort << " / admin " << cfg.adminBind << ":" << cfg.adminPort << "）";
        return 1;
    }
    LOG_INFO << "监听已登记：公开 " << cfg.publicBind << ":" << cfg.publicPort << "（明文，"
             << "由 Caddy 终止 TLS，对外 " << cfg.publicOrigin << "）；管理 " << cfg.adminBind
             << ":" << cfg.adminPort << "（对外 " << cfg.adminOrigin << "，仅宿主回环可达）";
    LOG_INFO << "邮件模式：" << cfg.mailMode
             << "（MAIL_MODE=log 时验证码只写日志且明确标注，绝不谎报投递成功）";
    LOG_INFO << "注册策略：" << cfg.registrationMode << "（白名单只约束新账号注册，"
             << "不定义任何业务权限；白名单邮箱数量 " << cfg.allowedEmails.size() << "）";

    // ---- 7) 调度 ----
    if (!startScheduler()) {
        return 1;
    }

    // ---- 8) 事件循环启动后的小动作 ----
    drogon::app().registerBeginningAdvice([]() {
        LOG_INFO << "事件循环已启动，开始接受请求";
        usapp::AdminAuthService::instance().syncBootstrapAdmin([](bool created) {
            if (created) {
                LOG_INFO << "管理员引导账号已写入/更新（口令散列来自环境变量，明文不落库）";
            } else {
                LOG_INFO << "未提供 ADMIN_BOOTSTRAP_USERNAME/PASSWORD_HASH，跳过管理员引导"
                         << "（本地开发默认如此；管理面仍可在库中预置管理员后登录）";
            }
        });
    });

    // ---- 9) 信号与主循环 ----
    if (std::signal(SIGINT, onTerminateSignal) == SIG_ERR) {
        LOG_WARN << "注册 SIGINT 处理器失败（Ctrl+C 可能无法优雅退出）";
    }
    if (std::signal(SIGTERM, onTerminateSignal) == SIG_ERR) {
        LOG_WARN << "注册 SIGTERM 处理器失败（容器停止可能无法优雅退出）";
    }

    LOG_INFO << "USapp API 就绪";
    drogon::app().run();

    // run() 返回后事件循环已停止，此时再停调度线程是安全的：
    // 反过来在信号处理器里停会与事件循环的退出产生死锁风险。
    usapp::scheduler::Scheduler::instance().stop();
    LOG_INFO << "USapp API 已退出";
    return 0;
}
