// =====================================================================
// services/api/tests/cache_and_contract_tests.cc
//
// 要求 7：静态缓存边界。
//
// 本文件**全部是静态文本断言**：读 contracts/openapi.yaml、
// apps/web/src/pwa/cache-policy.ts、apps/web/src/pwa/sw.ts、
// apps/web/src/pwa/register-sw.ts 的文本，断言其中写明的边界。
//
// !!! 重要声明 !!!
// 这些断言证明的是"契约与源码里写下了这条边界"，**不是**运行时行为。
// 真正的运行时验证在 apps/web/tests/unit/sw-cache-policy.spec.ts（vitest），
// 那套测试直接 import 同一份 cache-policy.ts 实现；本文件不重复它，
// 只保证"边界没有被删掉/改掉"，并且不需要 Node/前端工具链即可运行。
//
// 此外，本文件**只读** apps/web 下的文件，绝不修改它们。
// =====================================================================

#include <string>
#include <vector>

#include "TestSupport.h"
#include "RepoText.h"

namespace {

size_t countOccurrences(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) {
        return 0;
    }
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
    for (const auto &item : v) {
        if (item == s) {
            return true;
        }
    }
    return false;
}

std::string loadOrFail(const std::string &relPath) {
    const std::string text = usapptest::readRepoFile(relPath);
    if (text.empty()) {
        usapptest::fail(__FILE__, __LINE__,
                        std::string("读不到文件: ") + relPath + " (" + usapptest::repoRootHint() +
                            ")");
    }
    return text;
}

}  // namespace

int main() {
    TEST_CASE("要求7 contracts/openapi.yaml：NoStore 引用自洽 + 违规集合恰好是 7 个 admin 响应") {
        usapptest::noteStaticAssertion(
            "读 contracts/openapi.yaml 的文本。这是文本断言，不是运行时响应头检查。");

        const std::string text = loadOrFail("contracts/openapi.yaml");
        if (text.empty()) {
            return usapptest::finish();
        }
        const std::string flat = usapptest::collapseWs(text);

        // NoStore 响应头组件存在，并且内容就是 no-store。
        CHECK_CONTAINS(flat, "#/components/headers/NoStore");
        CHECK_CONTAINS(flat, "no-store");
        CHECK_CONTAINS(flat, "所有 API 响应强制 no-store");

        const size_t totalRefs = countOccurrences(text, "#/components/headers/NoStore");
        const std::string componentsSection =
            usapptest::sliceFromTo(text, "components:", "\x01-no-such-marker-");
        CHECK(!componentsSection.empty());
        const size_t componentRefs = countOccurrences(componentsSection, "#/components/headers/NoStore");

        // components/responses 下必须恰好 6 个可复用错误响应，且每个都带 NoStore。
        CHECK_EQ(componentRefs, static_cast<size_t>(6));
        {
            const std::vector<std::string> componentLines = usapptest::splitLines(componentsSection);
            for (const char *name : {"BadRequest", "Unauthenticated", "Forbidden", "NotFound",
                                     "Conflict", "TooManyRequests"}) {
                const std::string marker = std::string("    ") + name + ":";
                size_t start = componentLines.size();
                for (size_t i = 0; i < componentLines.size(); ++i) {
                    if (usapptest::trimCopy(componentLines[i]) == usapptest::trimCopy(marker)) {
                        start = i;
                        break;
                    }
                }
                if (start == componentLines.size()) {
                    usapptest::fail(__FILE__, __LINE__,
                                    std::string("components/responses 里找不到: ") + name);
                    ++usapptest::totalChecks();
                    continue;
                }
                // 该组件的块：从它这一行到下一个缩进 <= 4 的非空行为止。
                std::string block;
                for (size_t j = start; j < componentLines.size(); ++j) {
                    const std::string &line = componentLines[j];
                    if (j != start && !usapptest::trimCopy(line).empty() &&
                        usapptest::indentOf(line) <= 4) {
                        break;
                    }
                    block += line;
                    block.push_back('\n');
                }
                CHECK_CONTAINS(block, "NoStore");
            }
        }

        // 解析所有路径响应：内联带 NoStore 的 / 只有 $ref 的 / 两者都没有的。
        const std::vector<usapptest::OpenApiResponse> responses = usapptest::parseOpenApiResponses(text);
        CHECK(responses.size() > 30);  // 契约确实被解析到了

        size_t inlineNoStore = 0;
        std::vector<usapptest::OpenApiResponse> violations;
        for (const auto &r : responses) {
            if (r.hasNoStore) {
                ++inlineNoStore;
            } else if (!r.isRef) {
                violations.push_back(r);
            }
        }

        // 引用计数必须能拆成"路径内联" + "components 里 6 个"。
        CHECK_EQ(totalRefs, inlineNoStore + componentRefs);
        CHECK_EQ(inlineNoStore, static_cast<size_t>(13));

        // 已知的例外：7 个 admin 成功响应（200/204）既没有内联 NoStore，
        // 也没有 $ref 到带 NoStore 的可复用响应。
        std::vector<std::string> expected;
        expected.push_back("/api/admin/v1/health get 200");
        expected.push_back("/api/admin/v1/session get 200");
        expected.push_back("/api/admin/v1/sessions post 200");
        expected.push_back("/api/admin/v1/sessions delete 204");
        expected.push_back("/api/admin/v1/users get 200");
        expected.push_back("/api/admin/v1/couples get 200");
        expected.push_back("/api/admin/v1/audit-logs get 200");

        std::vector<std::string> actual;
        for (const auto &r : violations) {
            actual.push_back(r.path + " " + r.method + " " + r.status);
        }

        usapptest::note(std::string("openapi.yaml 中未引用 NoStore 的响应共 ") +
                        std::to_string(actual.size()) + " 个（期望 7 个 admin 成功响应）");
        for (const auto &a : actual) {
            usapptest::note(std::string("  - ") + a);
        }

        CHECK_EQ(actual.size(), static_cast<size_t>(7));
        for (const auto &a : actual) {
            if (!contains(expected, a)) {
                usapptest::fail(__FILE__, __LINE__, std::string("出现未预期的无 NoStore 响应: ") + a);
                ++usapptest::totalChecks();
            }
            // 全部必须落在 /api/admin/v1/ 下。
            CHECK(a.rfind("/api/admin/v1/", 0) == 0);
        }
        for (const auto &e : expected) {
            if (!contains(actual, e)) {
                usapptest::fail(__FILE__, __LINE__,
                                std::string("预期的例外不见了（契约可能已被修改）: ") + e);
                ++usapptest::totalChecks();
            }
        }

        // /api/v1/** 的每一个非内联响应都必须是 $ref 到可复用响应（那些都带 NoStore）。
        size_t v1Responses = 0;
        size_t v1WithoutNoStoreAndNotRef = 0;
        for (const auto &r : responses) {
            if (r.path.rfind("/api/v1/", 0) == 0) {
                ++v1Responses;
                if (!r.hasNoStore && !r.isRef) {
                    ++v1WithoutNoStoreAndNotRef;
                }
            }
        }
        CHECK(v1Responses > 10);
        CHECK_EQ(v1WithoutNoStoreAndNotRef, static_cast<size_t>(0));

        usapptest::note(
            "注意：契约自称『所有 API 响应强制 no-store』，但 /api/admin/v1/** 的 7 个成功响应"
            "并没有引用 NoStore —— 这是契约与实现之间的既有缺口（admin 路由同属 /api/**）。");
    }

    TEST_CASE("要求7 cache-policy.ts：四类敏感前缀 + 静态外壳白名单 + 非 GET 永不缓存") {
        usapptest::noteStaticAssertion(
            "读 apps/web/src/pwa/cache-policy.ts 的文本。运行时行为由同目录的 vitest 用例覆盖。");

        const std::string text = loadOrFail("apps/web/src/pwa/cache-policy.ts");
        if (text.empty()) {
            return usapptest::finish();
        }
        const std::string flat = usapptest::collapseWs(text);

        // 黑名单：四个前缀必须齐全（'/api/' 对应 /api/**）。
        CHECK_CONTAINS(flat, "export const SENSITIVE_PREFIXES: readonly string[] = [");
        CHECK_CONTAINS(flat, "'/api/'");
        CHECK_CONTAINS(flat, "'/media/'");
        CHECK_CONTAINS(flat, "'/private/'");
        CHECK_CONTAINS(flat, "'/uploads/'");
        CHECK_CONTAINS(flat, "/api/**");

        // 白名单：静态外壳缓存名与 destination 集合。
        CHECK_CONTAINS(flat, "export const SHELL_CACHE_NAME = 'usapp-shell-v1'");
        CHECK_CONTAINS(flat, "export const SHELL_DESTINATIONS: readonly string[] = [");
        for (const char *d : {"'document'", "'script'", "'style'", "'font'", "'image'"}) {
            CHECK_CONTAINS(flat, d);
        }

        // 决策函数：非 GET / 跨源 / 敏感路径 一律不可缓存。
        CHECK_CONTAINS(flat, "export function isCacheableRequest(");
        CHECK_CONTAINS(flat, "if ((method || 'GET').toUpperCase() !== 'GET') return false");
        CHECK_CONTAINS(flat, "if (context.sameOrigin === false) return false");
        CHECK_CONTAINS(flat, "if (isSensitivePath(input)) return false");

        // 敏感路径匹配：前缀本身与其子路径都算，且不会误伤 /apifoo（用 '/' 收口）。
        CHECK_CONTAINS(flat, "export function isSensitivePath(");
        CHECK_CONTAINS(flat, "const bare = prefix.endsWith('/') ? prefix.slice(0, -1) : prefix");
        CHECK_CONTAINS(flat, "return path === bare || path.startsWith(prefix)");

        // 该文件是唯一事实来源，自身不做路由（不含 NavigationRoute；策略类在 sw.ts 里 new）。
        CHECK_NOT_CONTAINS(flat, "NavigationRoute");
        CHECK_NOT_CONTAINS(flat, "NetworkOnly");

        // 文件头说明了 /api/** 与契约 no-store 的关系。
        CHECK_CONTAINS(flat, "Cache-Control: no-store");
    }

    TEST_CASE("要求7 sw.ts：黑名单先于白名单注册，且刻意不使用 NavigationRoute") {
        usapptest::noteStaticAssertion(
            "读 apps/web/src/pwa/sw.ts 的文本。这是文本断言：证明路由注册顺序与策略绑定，"
            "不证明 Service Worker 在浏览器里的真实行为。");

        const std::string text =loadOrFail("apps/web/src/pwa/sw.ts");
        if (text.empty()) {
            return usapptest::finish();
        }
        const std::string flat = usapptest::collapseWs(text);

        // 策略对象：敏感路径用 NetworkOnly，静态外壳用 StaleWhileRevalidate。
        CHECK_CONTAINS(flat, "import { NetworkOnly, StaleWhileRevalidate } from 'workbox-strategies'");
        CHECK_CONTAINS(flat, "const networkOnly = new NetworkOnly()");
        CHECK_CONTAINS(flat,
                       "const shellStrategy = new StaleWhileRevalidate({ cacheName: SHELL_CACHE_NAME })");

        // 黑名单路由：isSensitivePath -> networkOnly（覆盖 /api、/media、/private、/uploads）。
        CHECK_CONTAINS(flat, "registerRoute(({ url }) => isSensitivePath(url.pathname), networkOnly)");

        // 白名单路由：isCacheableRequest -> shellStrategy。
        CHECK_CONTAINS(flat, "isCacheableRequest(url.toString(), request.method");

        // 顺序：黑名单必须先注册（第一个 registerRoute 就是敏感路径）。
        CHECK_ORDER(flat, "isSensitivePath(url.pathname)", "isCacheableRequest(url.toString()");
        const std::string firstRoute = usapptest::sliceFromTo(text, "registerRoute(", "isCacheableRequest");
        CHECK(!firstRoute.empty());
        CHECK_CONTAINS(firstRoute, "isSensitivePath");
        CHECK_CONTAINS(firstRoute, "networkOnly");
        CHECK_NOT_CONTAINS(firstRoute, "shellStrategy");

        const std::string shellRoute =
            usapptest::sliceFromTo(text, "isCacheableRequest(url.toString()", "precacheAndRoute");
        CHECK(!shellRoute.empty());
        CHECK_CONTAINS(shellRoute, "shellStrategy");
        CHECK_NOT_CONTAINS(shellRoute, "networkOnly");

        // 刻意没有 NavigationRoute（否则会抢占 /api/** 的 NetworkOnly）。
        CHECK_NOT_CONTAINS(flat, "NavigationRoute(");
        CHECK_NOT_CONTAINS(flat, "new NavigationRoute");
        CHECK_CONTAINS(flat, "刻意不用 NavigationRoute");
        CHECK_CONTAINS(flat, "必须牢牢留在 NetworkOnly 上");

        // 导航离线兜底：setCatchHandler + 预缓存 index.html，找不到就明确失败。
        CHECK_CONTAINS(flat, "setCatchHandler");
        CHECK_CONTAINS(flat, "request.destination === 'document'");
        CHECK_CONTAINS(flat, "matchPrecache('/index.html')");
        CHECK_CONTAINS(flat, "Response.error()");

        // 预缓存与旧缓存清理。
        CHECK_CONTAINS(flat, "cleanupOutdatedCaches()");
        CHECK_CONTAINS(flat, "precacheAndRoute(precacheManifest)");

        // registerType: 'prompt'：不自动接管（没有 clientsClaim 调用），
        // 只有页面显式发 SKIP_WAITING 才 self.skipWaiting()。
        CHECK_CONTAINS(flat, "不调用 clientsClaim()");
        CHECK_CONTAINS(flat, "SKIP_WAITING");
        CHECK_ORDER(flat, "addEventListener('message'", "void self.skipWaiting()");
    }

    TEST_CASE("要求7 register-sw.ts：prompt 语义（不自动刷新、开发模式不注册）") {
        usapptest::noteStaticAssertion("读 apps/web/src/pwa/register-sw.ts 的文本。");

        const std::string text = loadOrFail("apps/web/src/pwa/register-sw.ts");
        if (text.empty()) {
            return usapptest::finish();
        }
        const std::string flat = usapptest::collapseWs(text);

        CHECK_CONTAINS(flat, "import { registerSW } from 'virtual:pwa-register'");
        CHECK_CONTAINS(flat, "immediate: true");
        CHECK_CONTAINS(flat, "onNeedRefresh");
        CHECK_CONTAINS(flat, "onOfflineReady");
        // 开发模式不注册（vite.config.ts 未开启 devOptions，/sw.js 不存在）。
        CHECK_CONTAINS(flat, "!import.meta.env.DEV");
        // 只有用户显式确认才 updateSW(true)（内部 postMessage SKIP_WAITING + reload）。
        CHECK_CONTAINS(flat, "await updateSW(true)");
        // 该文件不涉及路由与缓存策略。
        CHECK_NOT_CONTAINS(flat, "NavigationRoute");
        CHECK_NOT_CONTAINS(flat, "NetworkOnly");
        CHECK_NOT_CONTAINS(flat, "clientsClaim");
    }

    return usapptest::finish();
}
