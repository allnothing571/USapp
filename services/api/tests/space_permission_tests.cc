// =====================================================================
// services/api/tests/space_permission_tests.cc
//
// 要求 4「同空间权限」。
//
// 无数据库环境下无法调用 SpaceService::getSpaceForMember（它要走
// db::Pool::instance().client()->execSqlAsync），因此本文件用
// **静态文本断言** 给出可执行证据，测试内已显式标注：
//
//   1) services/api/src/services/SpaceService.cc 的 kSpaceQueryById
//      的 SQL 同时含 `self_m.user_id = $1` 与 `c.id = $2` ——
//      即「只能查到自己是成员的空间」。
//   2) 同一文件里不存在任何 Forbidden / 403 分支 —— 非成员不返回 403。
//   3) 头文件注释声明「非成员一律 SpaceNotFound」。
//   4) contracts/openapi.yaml 的 /api/v1/spaces/{spaceId} 只声明
//      200 / 401 / 404，没有 403。
//
// 这些断言证明的是「源码与契约文本的形态」，不是运行时行为；
// 运行时证据需要真实 PostgreSQL（见 README.md 与 integration_tests.cc）。
// =====================================================================

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "RepoText.h"
#include "TestSupport.h"

namespace {

std::vector<std::string> statusesFor(const std::vector<usapptest::OpenApiResponse> &all,
                                     const std::string &path,
                                     const std::string &method) {
    std::vector<std::string> out;
    for (const auto &r : all) {
        if (r.path == path && r.method == method) {
            out.push_back(r.status);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += v[i];
    }
    return out;
}

}  // namespace

int main() {
    const std::string space = usapptest::readRepoFile("services/api/src/services/SpaceService.cc");
    const std::string spaceHeader =
        usapptest::readRepoFile("services/api/src/services/SpaceService.h");
    const std::string spec = usapptest::readRepoFile("contracts/openapi.yaml");

    TEST_CASE("要求4 静态断言：kSpaceQueryById 同时限定 self 成员身份与目标空间 id") {
        usapptest::noteStaticAssertion(
            "（读 services/api/src/services/SpaceService.cc 文本；无数据库环境下这是"
            "“只能查到自己是成员的空间”的可执行代理证据，不是运行时验证）");

        CHECK(!space.empty());
        if (space.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            // 抓取 kSpaceQueryById 这一个常量的完整定义（到第一个分号为止）。
            const std::string byIdRaw = usapptest::sliceToSemicolon(space, "kSpaceQueryById");
            CHECK(!byIdRaw.empty());

            const std::string compact = usapptest::stripAllWs(byIdRaw);
            const std::string flat = usapptest::collapseWs(byIdRaw);

            // WHERE 子句里两个谓词都必须存在，且是 AND 关系。
            CHECK_CONTAINS(compact, "self_m.user_id=$1::uuid");
            CHECK_CONTAINS(compact, "c.id=$2::uuid");
            CHECK_CONTAINS(compact, "WHEREself_m.user_id=$1::uuidANDc.id=$2::uuid");
            CHECK_CONTAINS(flat, "WHERE self_m.user_id = $1::uuid AND c.id = $2::uuid");

            // 该查询仍然从 couple_members 出发做自连接——$1 必须是「调用者自己」。
            CHECK_CONTAINS(compact, "FROMcouple_membersASself_m");
            CHECK_CONTAINS(compact, "JOINcouplesAScONc.id=self_m.couple_id");

            // 对照：按「当前用户」查询的 kSpaceQuery 没有 $2 谓词，
            // 证明 $2 是 by-id 变体独有的、用于限定目标空间的约束。
            const std::string byUserRaw = usapptest::sliceToSemicolon(space, "const char *kSpaceQuery =");
            CHECK(!byUserRaw.empty());
            const std::string byUserCompact = usapptest::stripAllWs(byUserRaw);
            CHECK_CONTAINS(byUserCompact, "WHEREself_m.user_id=$1::uuid");
            CHECK_NOT_CONTAINS(byUserCompact, "$2");
            CHECK_NE(byUserCompact, compact);
        }
    }

    TEST_CASE("要求4 静态断言：非成员不返回 403，只有 SpaceNotFound") {
        usapptest::noteStaticAssertion(
            "（读 SpaceService.cc / SpaceService.h 文本；证明“不区分不存在与无权限”"
            "的设计在代码里落实为没有 403 分支，不是运行时验证）");

        CHECK(!space.empty());
        if (!space.empty()) {
            // 实现里根本没有 Forbidden / 403 这条路径。
            CHECK_NOT_CONTAINS(space, "Forbidden");
            CHECK_NOT_CONTAINS(space, "403");
            CHECK_NOT_CONTAINS(space, "ForbiddenSpace");
            // 非成员/无权的统一结果是 SpaceNotFound。
            CHECK_CONTAINS(space, "Outcome::SpaceNotFound");
            CHECK_CONTAINS(space, "空间不存在或无权查看");
            // getSpaceForMember 存在，且缺少参数时是 Validation（而不是 403）。
            CHECK_CONTAINS(space, "void SpaceService::getSpaceForMember(const std::string &userId,");
            CHECK_CONTAINS(space, "缺少用户或空间标识");
        }

        CHECK(!spaceHeader.empty());
        if (!spaceHeader.empty()) {
            // Outcome 枚举里没有 Forbidden。
            const std::string enumSlice =
                usapptest::sliceFromTo(spaceHeader, "enum class Outcome {", "};");
            CHECK(!enumSlice.empty());
            if (!enumSlice.empty()) {
                CHECK_NOT_CONTAINS(enumSlice, "Forbidden");
                CHECK_CONTAINS(enumSlice, "SpaceNotFound");
                CHECK_CONTAINS(enumSlice, "AlreadyInSpace");
                CHECK_CONTAINS(enumSlice, "SpaceFull");
            }
            // 接口注释声明非成员一律 SpaceNotFound。
            CHECK_CONTAINS(spaceHeader, "非成员一律 SpaceNotFound");
            CHECK_CONTAINS(spaceHeader, "不区分");
        }
    }

    TEST_CASE("要求4 静态断言：openapi 的 /api/v1/spaces/{spaceId} 只有 200/401/404") {
        usapptest::noteStaticAssertion(
            "（读 contracts/openapi.yaml 文本；证明对外契约不给“不是成员”返回 403，"
            "不是运行时验证）");

        CHECK(!spec.empty());
        if (spec.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            const std::vector<usapptest::OpenApiResponse> all =
                usapptest::parseOpenApiResponses(spec);
            CHECK(all.size() > 30);  // 解析器至少应看到全部业务响应

            const std::string path = "/api/v1/spaces/{spaceId}";
            const std::vector<std::string> statuses = statusesFor(all, path, "get");
            CHECK(!statuses.empty());
            CHECK_EQ(join(statuses), std::string("200,401,404"));

            // 显式断言：没有任何 403（或 410/409 之类会泄露存在性的状态码）。
            for (const auto &r : all) {
                if (r.path == path) {
                    CHECK_NE(r.status, std::string("403"));
                    CHECK_NE(r.status, std::string("410"));
                    CHECK_NE(r.status, std::string("409"));
                }
            }

            // 该路径的 200 响应带 no-store 契约；401/404 走组件响应。
            for (const auto &r : all) {
                if (r.path == path && r.status == "200") {
                    CHECK(r.hasNoStore);
                }
            }

            // 契约文本明确「不区分不存在与无权限」，与实现一致。
            const std::string flat = usapptest::collapseWs(spec);
            CHECK_CONTAINS(flat, "非本空间成员一律得到 404");
            CHECK_CONTAINS(flat, "SPACE_NOT_FOUND");
            CHECK_CONTAINS(flat, "避免通过 id 探测其他情侣空间是否存在");
            CHECK_CONTAINS(flat, "operationId: getSpaceById");
            CHECK_CONTAINS(flat, "仅本空间成员");
        }
    }

    return usapptest::finish();
}
