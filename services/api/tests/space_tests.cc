// =====================================================================
// services/api/tests/space_tests.cc
//
// 要求 3「邀请加入及成员数量约束」。
//
// 覆盖：
//   * SpaceService::normalizeInviteCode —— 去连字符/空白、转大写、
//     长度必须 12、非法字符返回空串、幂等。
//   * SpaceService::generateInviteCode —— 长度/字符集/分组形状、
//     多次生成不重复、归一化后仍等于自身。
//   * 成员数量约束（=2）的**静态文本断言**：读
//     database/migrations/0003_couple_binding.sql，断言
//     `slot IN (1, 2)`、`UNIQUE (user_id)`、`UNIQUE (couple_id, slot)`
//     三条约束存在，且 couples 表里没有 max_members 列。
//
// normalizeInviteCode / generateInviteCode 是运行时验证（不需要数据库）；
// 迁移约束部分是静态文本断言，测试内已显式标注。
// =====================================================================

#include <set>
#include <string>
#include <vector>

#include "RepoText.h"
#include "TestSupport.h"
#include "services/SpaceService.h"

using usapp::SpaceService;

namespace {

// SpaceService.cc 的 kInviteAlphabet 去掉 0/O、1/I/L、U 之后的取值域。
const std::string kAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ";

bool allInAlphabet(const std::string &s) {
    for (char c : s) {
        if (kAlphabet.find(c) == std::string::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // =================================================================
    // normalizeInviteCode
    // =================================================================
    TEST_CASE("要求3 normalizeInviteCode：去分隔符、转大写、长度与字符集校验") {
        // 规范形状（带连字符）-> 12 字符大写。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD-EFGH-JKMN"), std::string("ABCDEFGHJKMN"));
        // 小写 -> 大写。
        CHECK_EQ(SpaceService::normalizeInviteCode("abcd-efgh-jkmn"), std::string("ABCDEFGHJKMN"));
        // 紧凑形式（用户手抄时常常没有连字符）。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMN"), std::string("ABCDEFGHJKMN"));
        // 空白（空格与制表符）被跳过。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD EFGH JKMN"), std::string("ABCDEFGHJKMN"));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD\tEFGH\tJKMN"), std::string("ABCDEFGHJKMN"));
        CHECK_EQ(SpaceService::normalizeInviteCode("  ABCD-EFGH-JKMN  "), std::string("ABCDEFGHJKMN"));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD - EFGH - JKMN"), std::string("ABCDEFGHJKMN"));

        // 长度不足 / 超长 -> 空串。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKM"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMNX"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode(""), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("------------"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("   "), std::string(""));

        // 非法字符（不在 kInviteAlphabet 内）-> 空串。
        // 0/O、1/I/L、U 是字母表刻意排除的易混字符。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKUZ"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKM0"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMO"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKM1"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMI"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKML"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD-EFGH-JKM!"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD-EFGH-JKM."), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD_EFGH_JKMN"), std::string(""));

        // 边界：实现只跳过 '-'、' '、'\t' 三种字符（SpaceService.cc:460），
        // 换行/回车**不被**跳过，会参与长度与字符集校验 -> 空串。
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMN\n"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCDEFGHJKMN\r"), std::string(""));
        CHECK_EQ(SpaceService::normalizeInviteCode("ABCD-EFGH-JKMN\n"), std::string(""));

        // 幂等：合法码归一化两次结果相同。
        const std::string once = SpaceService::normalizeInviteCode("abcd-efgh-jkmn");
        CHECK_EQ(once, std::string("ABCDEFGHJKMN"));
        CHECK_EQ(SpaceService::normalizeInviteCode(once), once);
        CHECK_EQ(SpaceService::normalizeInviteCode(SpaceService::normalizeInviteCode(once)), once);

        // 归一化结果必然落在字母表内且长度恰为 12（随机抽样一批真实生成码）。
        for (int i = 0; i < 50; ++i) {
            const std::string normalized = SpaceService::normalizeInviteCode(SpaceService::generateInviteCode());
            CHECK_EQ(static_cast<int>(normalized.size()), 12);
            CHECK(allInAlphabet(normalized));
        }
    }

    // =================================================================
    // generateInviteCode
    // =================================================================
    TEST_CASE("要求3 generateInviteCode：形状、字符集、不重复、归一化自等") {
        const std::string code = SpaceService::generateInviteCode();

        // 形如 XXXX-XXXX-XXXX（12 个有效字符 + 2 个连字符 = 14）。
        CHECK_EQ(static_cast<int>(code.size()), 14);
        CHECK_EQ(code[4], '-');
        CHECK_EQ(code[9], '-');

        // 分隔格式确定：带连字符的位置恰为 4 与 9。
        int hyphenCount = 0;
        for (char c : code) {
            if (c == '-') {
                ++hyphenCount;
            }
        }
        CHECK_EQ(hyphenCount, 2);

        // 去掉连字符后长度 12 且全部落在字母表内。
        std::string body;
        for (char c : code) {
            if (c != '-') {
                body.push_back(c);
            }
        }
        CHECK_EQ(static_cast<int>(body.size()), 12);
        CHECK(allInAlphabet(body));

        // 归一化后等于去连字符的 body（归一化是生成的逆操作）。
        const std::string normalized = SpaceService::normalizeInviteCode(code);
        CHECK_EQ(normalized, body);
        // 归一化幂等 / 自等。
        CHECK_EQ(SpaceService::normalizeInviteCode(normalized), normalized);
        CHECK_EQ(SpaceService::normalizeInviteCode(SpaceService::normalizeInviteCode(normalized)),
                 normalized);

        // 字母表本身取自实现的字面量：不含易混字符 0/O/1/I/L/U。
        const std::string impl = usapptest::readRepoFile("services/api/src/services/SpaceService.cc");
        CHECK(!impl.empty());
        if (!impl.empty()) {
            CHECK_CONTAINS(impl, "constexpr const char *kInviteAlphabet");
            CHECK_CONTAINS(impl, "23456789ABCDEFGHJKMNPQRSTVWXYZ");
            CHECK_CONTAINS(impl, "constexpr size_t kInviteCodeLength = 12;");
            CHECK_CONTAINS(impl, "constexpr size_t kInviteCodeGroup = 4;");
        }

        // 多次生成不重复：碰撞概率极低，200 次必须全不相同。
        std::set<std::string> seen;
        const int kRounds = 200;
        int duplicates = 0;
        for (int i = 0; i < kRounds; ++i) {
            const std::string c = SpaceService::generateInviteCode();
            if (!seen.insert(c).second) {
                ++duplicates;
            }
            CHECK_EQ(static_cast<int>(c.size()), 14);
            CHECK_EQ(SpaceService::normalizeInviteCode(c).size(), static_cast<size_t>(12));
        }
        CHECK_EQ(static_cast<int>(seen.size()), kRounds);
        CHECK_EQ(duplicates, 0);
    }

    // =================================================================
    // 成员数量约束（静态文本断言）
    // =================================================================
    TEST_CASE("要求3 成员数量=2 的静态文本断言：0003 迁移的三条约束") {
        usapptest::noteStaticAssertion(
            "（读 database/migrations/0003_couple_binding.sql 文本；这是"
            "“成员数量最多 2”在无数据库环境下的可执行代理证据，不是运行时验证。"
            "真正的运行时证据在 database/tests/constraints_test.sql，见 README）");

        const std::string sql =
            usapptest::readRepoFile("database/migrations/0003_couple_binding.sql");
        CHECK(!sql.empty());
        if (sql.empty()) {
            usapptest::note(usapptest::repoRootHint());
        } else {
            const std::string flat = usapptest::collapseWs(sql);
            const std::string compact = usapptest::stripAllWs(sql);

            // 1) slot 取值域 (1, 2) —— 容量上界由值域定义。
            CHECK_CONTAINS(compact, "CHECK(slotIN(1,2))");
            CHECK_CONTAINS(flat, "CONSTRAINT couple_members_slot_valid CHECK (slot IN (1, 2))");

            // 2) 一个用户只能属于一个空间。
            CHECK_CONTAINS(compact, "UNIQUE(user_id)");
            CHECK_CONTAINS(flat, "CONSTRAINT couple_members_user_unique UNIQUE (user_id)");

            // 3) 同一空间同一槽位只能有一人 —— 与 slot 值域合起来就是「最多 2 人」。
            CHECK_CONTAINS(compact, "UNIQUE(couple_id,slot)");
            CHECK_CONTAINS(flat, "CONSTRAINT couple_members_couple_slot_unique UNIQUE (couple_id, slot)");

            // 约束确实挂在 couple_members 表上。
            CHECK_CONTAINS(compact, "CREATETABLEcouple_members(");
            CHECK_CONTAINS(compact, "slotsmallintNOTNULL");
            // couple_members 的三个约束名都在，且没有多出别的 capacity 约束。
            CHECK_CONTAINS(flat, "couple_members_slot_valid");
            CHECK_CONTAINS(flat, "couple_members_couple_slot_unique");

            // couples 表内部不得出现 max_members 列（容量由 slot 值域定义）。
            // 注意：文件头部注释里**故意提到** "max_members" 一词
            // （migrations/0003:11 的解释性注释），所以不能对整文件断言不存在，
            // 必须切片到 CREATE TABLE couples (...) 之内。
            const std::string couplesTable =
                usapptest::sliceFromTo(sql, "CREATE TABLE couples (", "CREATE TABLE couple_members (");
            CHECK(!couplesTable.empty());
            if (!couplesTable.empty()) {
                CHECK_NOT_CONTAINS(couplesTable, "max_members");
                CHECK_NOT_CONTAINS(couplesTable, "max_capacity");
                CHECK_NOT_CONTAINS(couplesTable, "capacity");
            }

            // 迁移文件的注释把意图写明（此处允许出现 max_members 字样）。
            CHECK_CONTAINS(flat, "容量由 slot 取值域 (1,2) 定义");
            CHECK_CONTAINS(flat, "刻意不引入 max_members 列");
            CHECK_CONTAINS(flat, "slot 只能为 1 或 2");
        }
    }

    return usapptest::finish();
}
