// =====================================================================
// services/api/tests/TaskServiceUnitTests.h
//
// 阶段 1 M3-B：TaskService 纯函数与启动判定的单元测试（由 UnitTest.cc 的 main 调用）。
// 不访问数据库与网络。
// =====================================================================
#pragma once

#include <json/json.h>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Errors.h"
#include "db/TaskPrivilegePolicy.h"
#include "db/TaskStartupCheck.h"
#include "services/TaskService.h"

namespace usapptest::taskunit {

using usapp::TaskService;

inline Json::Value parseJson(const std::string &text) {
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors)) {
        throw std::runtime_error("测试 JSON 无法解析");
    }
    return value;
}

// 测试自己的 base64url（无填充）编码，用来构造各种畸形游标。
inline std::string b64url(const std::string &in) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const unsigned char c : in) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    return out;
}

inline std::string repeat(const std::string &unit, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += unit;
    return out;
}

inline bool parses(const std::string &json, TaskService::CreateInput &out) {
    std::string message;
    const bool ok = TaskService::parseCreateRequest(parseJson(json), out, message);
    if (!ok && message.empty()) {
        usapptest::note("parseCreateRequest 拒绝时没有提示文本");
    }
    return ok;
}

inline bool parses(const std::string &json) {
    TaskService::CreateInput ignored;
    return parses(json, ignored);
}

inline void runTaskServiceUnitTests() {
    TEST_CASE("M5: daily counting is explicit, date-bound and preserves one-off fingerprints") {
        TaskService::CreateInput input;
        CHECK(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2,"schedule_type":"daily"})", input));
        CHECK_EQ(input.scheduleType, std::string("daily"));
        const auto daily = TaskService::createFingerprint(input);
        input.scheduleType = "one_off";
        CHECK_NE(TaskService::createFingerprint(input), daily);
        CHECK_FALSE(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"schedule_type":"daily"})", input));
        CHECK_FALSE(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2,"schedule_type":"monthly"})", input));
        CHECK_FALSE(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2,"schedule_type":"daily","deadline":{"local_date":"2026-10-20"}})", input));
        // M5-B: weekly has the same publication rules and a distinct fingerprint;
        // the daily and one-off canonical strings above are unchanged.
        CHECK(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2,"schedule_type":"weekly"})", input));
        const auto weekly = TaskService::createFingerprint(input);
        CHECK_NE(weekly, daily);
        CHECK_FALSE(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"schedule_type":"weekly"})", input));
        CHECK_FALSE(parses(R"({"title":"D","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2,"schedule_type":"weekly","deadline":{"local_date":"2026-10-25"}})", input));
        // Mondays across a year boundary, a leap year and a 2029 New Year's Eve; other weekdays fail.
        for (const char *monday : {"2026-10-19", "2024-12-30", "2028-02-28", "2029-12-31", "1970-01-05"})
            CHECK(TaskService::isWeekStart(monday));
        for (const char *other : {"2026-10-18", "2026-10-25", "2025-01-01", "2028-02-29", "1970-01-01", "2026-02-30"})
            CHECK_FALSE(TaskService::isWeekStart(other));
        CHECK(TaskService::validPeriodDate("2028-02-29"));
        CHECK_FALSE(TaskService::validPeriodDate("2026-02-29"));
        CHECK_FALSE(TaskService::validPeriodDate("2026-10-16T00:00"));
        TaskService::TaskData task;
        task.scheduleType = "daily";
        task.completionMode = "count";
        task.periodDate = "2026-10-16";
        CHECK_EQ(TaskService::taskJson(task)["period_date"].asString(), task.periodDate);
        task.scheduleType = "weekly";
        task.periodDate = "2026-10-19";
        CHECK_EQ(TaskService::taskJson(task)["schedule_type"].asString(), std::string("weekly"));
        CHECK_EQ(TaskService::taskJson(task)["period_date"].asString(), task.periodDate);
        task.scheduleType = "custom";
        task.periodDate.clear();
        task.startDate = "2026-10-26";
        const auto customJson = TaskService::taskJson(task);
        CHECK_EQ(customJson["start_date"].asString(), task.startDate);
        CHECK_FALSE(customJson.isMember("period_date"));
    }

    TEST_CASE("M5-C: custom range publication is one bounded counting interval") {
        TaskService::CreateInput input;
        const std::string base = R"({"title":"R","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":3,"schedule_type":"custom")";
        CHECK(parses(base + R"(,"start_date":"2026-12-31","end_date":"2027-01-02"})", input));
        CHECK_EQ(input.startDate, std::string("2026-12-31"));
        CHECK_EQ(input.deadlineLocalDate, std::string("2027-01-02"));
        CHECK(input.deadlineLocalTime.empty());
        const auto range = TaskService::createFingerprint(input);
        CHECK(parses(base + R"(,"start_date":"2026-12-30","end_date":"2027-01-02"})", input));
        CHECK_NE(TaskService::createFingerprint(input), range);  // the start date is part of the identity
        CHECK(parses(base + R"(,"start_date":"2026-10-26","end_date":"2026-10-26"})", input));  // same day
        const std::vector<std::string> rejected = {
            base + R"(,"start_date":"2026-10-27","end_date":"2026-10-26"})",            // end before start
            base + R"(,"start_date":"2026-10-26"})",                                    // missing end
            base + R"(,"start_date":"2026-02-30","end_date":"2026-03-01"})",            // invalid date
            base + R"(,"start_date":"2026-10-26","end_date":"2026-10-27","deadline":{"local_date":"2026-10-27"}})",
            R"({"title":"R","reward_flowers":1,"reward_sunshine":0,"schedule_type":"custom","start_date":"2026-10-26","end_date":"2026-10-27"})",
            R"({"title":"R","reward_flowers":1,"reward_sunshine":0,"start_date":"2026-10-26","end_date":"2026-10-27"})",
        };
        for (const auto &body : rejected) CHECK_FALSE(parses(body, input));
    }

    TEST_CASE("M4: counting publication rules, legacy fingerprints and response defaults") {
        TaskService::CreateInput input;
        const std::vector<std::pair<std::string, bool>> cases = {
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":1})", true},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2147483647})", true},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":0})", false},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":1.0})", false},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count","target_count":2147483648})", false},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"count"})", false},
            {R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"single","target_count":2})", false},
        };
        for (const auto &c : cases) CHECK_EQ(parses(c.first, input), c.second);
        CHECK(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0})", input));
        const auto legacy = TaskService::createFingerprint(input);
        CHECK(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"completion_mode":"single","target_count":1})", input));
        CHECK_EQ(TaskService::createFingerprint(input), legacy);
        input.completionMode = "count";
        CHECK_NE(TaskService::createFingerprint(input), legacy);
        TaskService::TaskData data;
        CHECK_FALSE(TaskService::taskJson(data).isMember("completion_mode"));
        data.completionMode = "count";
        data.targetCount = 3;
        CHECK_EQ(TaskService::taskJson(data)["progress_count"].asInt(), 0);
        CHECK_EQ(TaskService::taskJson(data)["target_count"].asInt(), 3);
    }
    using TE = TaskService::TextError;

    TEST_CASE("TaskService: 标题严格 UTF-8、去首尾 Zs 空白、1-40 个码点、拒绝控制字符与换行") {
        std::string out;
        CHECK(TaskService::normalizeTitle("  Read one chapter  ", out) == TE::None);
        CHECK_EQ(out, std::string("Read one chapter"));
        // U+3000 表意空格与 U+00A0 被裁掉
        CHECK(TaskService::normalizeTitle("\xE3\x80\x80\xC2\xA0Hi\xE3\x80\x80", out) == TE::None);
        CHECK_EQ(out, std::string("Hi"));
        const std::string cjk = "\xE8\xAF\xBB";  // 读
        CHECK(TaskService::normalizeTitle(repeat(cjk, 40), out) == TE::None);
        CHECK(TaskService::normalizeTitle(repeat(cjk, 41), out) == TE::TooLong);
        const std::string emoji = "\xF0\x9F\x98\x80";  // 😀 计 1 个码点
        CHECK(TaskService::normalizeTitle(repeat(emoji, 40), out) == TE::None);
        CHECK(TaskService::normalizeTitle(repeat("x", 41), out) == TE::TooLong);
        CHECK(TaskService::normalizeTitle("", out) == TE::Empty);
        CHECK(TaskService::normalizeTitle("   \xE3\x80\x80 ", out) == TE::Empty);
        CHECK(TaskService::normalizeTitle("a\nb", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle("title\n", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle("a\tb", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle("a\rb", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle(std::string("a\0b", 3), out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle("a\x7f", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeTitle("a\xC2\x85", out) == TE::ControlCharacter);    // U+0085
        CHECK(TaskService::normalizeTitle("a\xE2\x80\xA8", out) == TE::ControlCharacter);  // U+2028
        CHECK(TaskService::normalizeTitle("a\xE2\x80\xA9", out) == TE::ControlCharacter);  // U+2029
        CHECK(TaskService::normalizeTitle("bad\xFF", out) == TE::InvalidUtf8);
        CHECK(TaskService::normalizeTitle("\xC0\x80", out) == TE::InvalidUtf8);            // 过长编码
        CHECK(TaskService::normalizeTitle("\xED\xA0\x80", out) == TE::InvalidUtf8);        // 代理项
        CHECK(TaskService::normalizeTitle("\xF4\x90\x80\x80", out) == TE::InvalidUtf8);    // > U+10FFFF
        CHECK(TaskService::normalizeTitle("\xE8\xAF", out) == TE::InvalidUtf8);            // 截断
        CHECK(TaskService::normalizeTitle(std::string(2000, 'x'), out) == TE::TooLong);
        CHECK(out.empty());
    }

    TEST_CASE("TaskService: 描述 CRLF 归一为 LF，单独 CR 与其他控制字符拒绝，0-500 个码点") {
        std::string out;
        CHECK(TaskService::normalizeDescription("", out) == TE::None);
        CHECK(out.empty());
        CHECK(TaskService::normalizeDescription("line1\r\nline2", out) == TE::None);
        CHECK_EQ(out, std::string("line1\nline2"));
        CHECK(TaskService::normalizeDescription("\r\n\n  text \xE3\x80\x80\n", out) == TE::None);
        CHECK_EQ(out, std::string("text"));
        CHECK(TaskService::normalizeDescription("a\n\nb", out) == TE::None);
        CHECK_EQ(out, std::string("a\n\nb"));
        CHECK(TaskService::normalizeDescription("   \n  ", out) == TE::None);
        CHECK(out.empty());
        CHECK(TaskService::normalizeDescription("a\rb", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeDescription("a\r", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeDescription("a\tb", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeDescription("a\xE2\x80\xA8" "b", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeDescription("a\xC2\x9F", out) == TE::ControlCharacter);
        CHECK(TaskService::normalizeDescription("bad\xFF", out) == TE::InvalidUtf8);
        CHECK(TaskService::normalizeDescription(repeat("x", 500), out) == TE::None);
        CHECK(TaskService::normalizeDescription(repeat("\xF0\x9F\x98\x80", 500), out) == TE::None);
        CHECK(TaskService::normalizeDescription(repeat("x", 501), out) == TE::TooLong);
        // 首尾被裁掉的空白不计入上限
        CHECK(TaskService::normalizeDescription("\n" + repeat("x", 500) + "  \n", out) == TE::None);
        CHECK_EQ(out.size(), 500u);
        // 每个 CRLF 归一后计为 1 个码点
        CHECK(TaskService::normalizeDescription("a" + repeat("\r\n", 498) + "b", out) == TE::None);
        CHECK_EQ(out.size(), 500u);
    }

    TEST_CASE("TaskService: 请求体字段白名单、整数奖励、范围、至少一项为正、截止日期与时间形状") {
        TaskService::CreateInput in;
        CHECK(parses(R"({"title":" T ","reward_flowers":1,"reward_sunshine":0})", in));
        CHECK_EQ(in.title, std::string("T"));
        CHECK(in.description.empty());
        CHECK(in.deadlineLocalDate.empty());
        CHECK(in.deadlineLocalTime.empty());

        CHECK(parses(R"({"title":"T","description":"a\r\nb","reward_flowers":999,"reward_sunshine":100000,
                         "deadline":{"local_date":"2026-10-20","local_time":"23:00"}})", in));
        CHECK_EQ(in.description, std::string("a\nb"));
        CHECK_EQ(in.rewardFlowers, 999);
        CHECK_EQ(in.rewardSunshine, 100000);
        CHECK_EQ(in.deadlineLocalDate, std::string("2026-10-20"));
        CHECK_EQ(in.deadlineLocalTime, std::string("23:00"));

        CHECK(parses(R"({"title":"T","reward_flowers":0,"reward_sunshine":5,"deadline":{"local_date":"2028-02-29"}})", in));
        CHECK_EQ(in.deadlineLocalDate, std::string("2028-02-29"));
        CHECK(in.deadlineLocalTime.empty());
        CHECK(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"deadline":null})", in));
        CHECK(in.deadlineLocalDate.empty());
        CHECK(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,
                         "deadline":{"local_date":"2026-10-20","local_time":null}})", in));
        CHECK(in.deadlineLocalTime.empty());

        // 服务端推导身份：请求体里的身份字段一律拒绝
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"assignee_id":"x"})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"couple_id":"x"})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"creator_id":"x"})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"status":"completed"})"));
        // 必填与类型
        CHECK_FALSE(parses(R"({"reward_flowers":1,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":5,"reward_flowers":1,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_sunshine":5})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1})"));
        CHECK_FALSE(parses(R"({"title":"T","description":null,"reward_flowers":1,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","description":7,"reward_flowers":1,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"   ","reward_flowers":1,"reward_sunshine":0})"));
        // 奖励只接受 JSON 整数
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1.0,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1e2,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":"5","reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":true,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":null,"reward_sunshine":5})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":-1,"reward_sunshine":5})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":1000,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":0,"reward_sunshine":100001})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":0,"reward_sunshine":0})"));
        CHECK_FALSE(parses(R"({"title":"T","reward_flowers":99999999999,"reward_sunshine":0})"));
        // 截止时间
        const std::string base = R"({"title":"T","reward_flowers":1,"reward_sunshine":0,"deadline":)";
        CHECK_FALSE(parses(base + R"("2026-10-20"})"));
        CHECK_FALSE(parses(base + R"({}})"));
        CHECK_FALSE(parses(base + R"({"local_time":"10:00"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","at":"2026-10-20T00:00:00Z"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-02-30"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2027-02-29"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-2-3"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20T00:00"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"9999-12-31"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":20261020}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","local_time":"24:00"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","local_time":"23:60"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","local_time":"7:00"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","local_time":"07:00:00"}})"));
        CHECK_FALSE(parses(base + R"({"local_date":"2026-10-20","local_time":700}})"));
        // 非对象请求体
        std::string message;
        CHECK_FALSE(TaskService::parseCreateRequest(parseJson("[1]"), in, message));
    }

    TEST_CASE("TaskService: 发布指纹是带长度的规范串的 SHA-256，任何字段变化都改变指纹") {
        TaskService::CreateInput a;
        a.title = "T";
        a.description = "d";
        a.rewardFlowers = 5;
        a.rewardSunshine = 40;
        a.deadlineLocalDate = "2026-10-20";
        a.deadlineLocalTime = "23:00";
        CHECK_EQ(TaskService::canonicalCreateString(a),
                 std::string("create-task:v1:title=1:T;description=1:d;reward_flowers=1:5;reward_sunshine=2:40;"
                             "deadline_local_date=10:2026-10-20;deadline_local_time=5:23:00;"));
        const std::string fp = TaskService::createFingerprint(a);
        CHECK_EQ(fp.size(), 64u);
        CHECK(fp.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK_EQ(TaskService::createFingerprint(a), fp);

        auto changed = [&](const std::function<void(TaskService::CreateInput &)> &edit) {
            TaskService::CreateInput b = a;
            edit(b);
            return TaskService::createFingerprint(b) != fp;
        };
        CHECK(changed([](TaskService::CreateInput &b) { b.title = "U"; }));
        CHECK(changed([](TaskService::CreateInput &b) { b.description = ""; }));
        CHECK(changed([](TaskService::CreateInput &b) { b.rewardFlowers = 6; }));
        CHECK(changed([](TaskService::CreateInput &b) { b.rewardSunshine = 41; }));
        CHECK(changed([](TaskService::CreateInput &b) { b.deadlineLocalDate = "2026-10-21"; }));
        CHECK(changed([](TaskService::CreateInput &b) { b.deadlineLocalTime = ""; }));
        // 带长度：把内容在字段之间挪动不会产生相同的规范串
        TaskService::CreateInput x = a, y = a;
        x.title = "a;description=0:";
        x.description = "";
        y.title = "a";
        y.description = ";description=0:";
        CHECK_NE(TaskService::canonicalCreateString(x), TaskService::canonicalCreateString(y));
    }

    TEST_CASE("TaskService: Idempotency-Key 形状与 limit 严格解析") {
        CHECK(TaskService::validIdempotencyKey("0f8fad5b-d9cb-469f-a165-70867728950e"));
        CHECK(TaskService::validIdempotencyKey(std::string(16, 'a')));
        CHECK(TaskService::validIdempotencyKey(std::string(128, 'Z')));
        CHECK_FALSE(TaskService::validIdempotencyKey(std::string(15, 'a')));
        CHECK_FALSE(TaskService::validIdempotencyKey(std::string(129, 'a')));
        CHECK_FALSE(TaskService::validIdempotencyKey(""));
        CHECK_FALSE(TaskService::validIdempotencyKey("0f8fad5b d9cb 469f a165 70867728950e"));
        CHECK_FALSE(TaskService::validIdempotencyKey("0f8fad5b.d9cb.469f.a165.70867728950e"));
        CHECK_FALSE(TaskService::validIdempotencyKey("0f8fad5b-d9cb-469f-a165-7086772895\xC3\xA9"));

        CHECK(TaskService::parseLimit("") == std::optional<int>(50));
        CHECK(TaskService::parseLimit("1") == std::optional<int>(1));
        CHECK(TaskService::parseLimit("100") == std::optional<int>(100));
        CHECK_FALSE(TaskService::parseLimit("0").has_value());
        CHECK_FALSE(TaskService::parseLimit("101").has_value());
        CHECK_FALSE(TaskService::parseLimit("-1").has_value());
        CHECK_FALSE(TaskService::parseLimit("1.5").has_value());
        CHECK_FALSE(TaskService::parseLimit(" 5").has_value());
        CHECK_FALSE(TaskService::parseLimit("abc").has_value());
        CHECK_FALSE(TaskService::parseLimit("0050").has_value());
        CHECK_FALSE(TaskService::parseView("").has_value());
        CHECK_FALSE(TaskService::parseView("OPEN").has_value());
        CHECK(TaskService::parseView("open") == std::optional<TaskService::View>(TaskService::View::Open));
        CHECK(TaskService::parseView("history") == std::optional<TaskService::View>(TaskService::View::History));
    }

    TEST_CASE("TaskService: 游标往返与严格拒绝（格式、版本、视图、字段、时间戳、uuid）") {
        using V = TaskService::View;
        const std::string id = "0f8fad5b-d9cb-469f-a165-70867728950e";
        const std::string ts = "2026-10-20T15:00:00.000000Z";
        const std::string ts2 = "2026-10-09T01:02:03.123456Z";

        TaskService::Cursor dated;
        dated.view = V::Open;
        dated.deadlineAt = ts;
        dated.createdAt = ts2;
        dated.id = id;
        const std::string datedRaw = TaskService::encodeCursor(dated);
        CHECK(datedRaw.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
        const auto datedBack = TaskService::decodeCursor(datedRaw, V::Open);
        CHECK(datedBack.has_value());
        if (datedBack) {
            CHECK(datedBack->deadlineAt == std::optional<std::string>(ts));
            CHECK_EQ(datedBack->createdAt, ts2);
            CHECK_EQ(datedBack->id, id);
        }
        CHECK(TaskService::listQueryFor(V::Open, datedBack) == TaskService::ListQuery::OpenAfterDated);

        TaskService::Cursor undated = dated;
        undated.deadlineAt.reset();
        const auto undatedBack = TaskService::decodeCursor(TaskService::encodeCursor(undated), V::Open);
        CHECK(undatedBack.has_value());
        if (undatedBack) CHECK_FALSE(undatedBack->deadlineAt.has_value());
        CHECK(TaskService::listQueryFor(V::Open, undatedBack) == TaskService::ListQuery::OpenAfterUndated);
        CHECK(TaskService::listQueryFor(V::Open, std::nullopt) == TaskService::ListQuery::OpenFirst);

        TaskService::Cursor history;
        history.view = V::History;
        history.historyAt = ts;
        history.id = id;
        const std::string historyRaw = TaskService::encodeCursor(history);
        const auto historyBack = TaskService::decodeCursor(historyRaw, V::History);
        CHECK(historyBack.has_value());
        if (historyBack) CHECK_EQ(historyBack->historyAt, ts);
        CHECK(TaskService::listQueryFor(V::History, historyBack) == TaskService::ListQuery::HistoryAfter);
        CHECK(TaskService::listQueryFor(V::History, std::nullopt) == TaskService::ListQuery::HistoryFirst);

        // 文档格式：解码后的 JSON 恰好是这些字段
        CHECK_EQ(datedRaw, b64url("{\"v\":1,\"view\":\"open\",\"dl\":\"" + ts + "\",\"ca\":\"" + ts2 +
                                  "\",\"id\":\"" + id + "\"}"));
        CHECK_EQ(historyRaw, b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"" + ts + "\",\"id\":\"" + id + "\"}"));

        // 视图不匹配
        CHECK_FALSE(TaskService::decodeCursor(datedRaw, V::History).has_value());
        CHECK_FALSE(TaskService::decodeCursor(historyRaw, V::Open).has_value());

        const std::string open = "{\"v\":1,\"view\":\"open\",\"dl\":\"" + ts + "\",\"ca\":\"" + ts2 + "\",\"id\":\"" + id;
        struct Bad {
            const char *label;
            std::string raw;
            V view;
        };
        const std::vector<Bad> bad = {
            {"空串", "", V::Open},
            {"非 base64url 字符", "abc$def", V::Open},
            {"带填充", datedRaw + "=", V::Open},
            {"标准 base64 字符", "ab+/", V::Open},
            {"长度余 1", "abcde", V::Open},
            {"不是 JSON", b64url("not json"), V::Open},
            {"JSON 数组", b64url("[1,2]"), V::Open},
            {"JSON 后有多余内容", b64url(open + "\"} x"), V::Open},
            {"未知版本", b64url("{\"v\":2" + open.substr(6) + "\"}"), V::Open},
            {"版本是字符串", b64url("{\"v\":\"1\"" + open.substr(6) + "\"}"), V::Open},
            {"版本是 1.0", b64url("{\"v\":1.0" + open.substr(6) + "\"}"), V::Open},
            {"多余字段", b64url(open + "\",\"x\":1}"), V::Open},
            {"另一视图的字段", b64url(open + "\",\"ha\":\"" + ts + "\"}"), V::Open},
            {"重复字段", b64url(open + "\",\"id\":\"" + id + "\"}"), V::Open},
            {"缺字段", b64url("{\"v\":1,\"view\":\"open\",\"dl\":null,\"id\":\"" + id + "\"}"), V::Open},
            {"ca 为 null", b64url("{\"v\":1,\"view\":\"open\",\"dl\":null,\"ca\":null,\"id\":\"" + id + "\"}"), V::Open},
            {"id 为 null", b64url("{\"v\":1,\"view\":\"open\",\"dl\":null,\"ca\":\"" + ts + "\",\"id\":null}"), V::Open},
            {"ha 为 null", b64url("{\"v\":1,\"view\":\"history\",\"ha\":null,\"id\":\"" + id + "\"}"), V::History},
            {"时间戳少于六位小数", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-10-20T15:00:00.000Z\",\"id\":\"" + id + "\"}"), V::History},
            {"时间戳没有小数", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-10-20T15:00:00Z\",\"id\":\"" + id + "\"}"), V::History},
            {"时间戳带偏移", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-10-20T15:00:00.000000+08:00\",\"id\":\"" + id + "\"}"), V::History},
            {"月份越界", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-13-01T00:00:00.000000Z\",\"id\":\"" + id + "\"}"), V::History},
            {"日期不存在", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-02-30T00:00:00.000000Z\",\"id\":\"" + id + "\"}"), V::History},
            {"小时越界", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"2026-10-20T24:00:00.000000Z\",\"id\":\"" + id + "\"}"), V::History},
            {"大写 uuid", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"" + ts + "\",\"id\":\"0F8FAD5B-D9CB-469F-A165-70867728950E\"}"), V::History},
            {"无连字符 uuid", b64url("{\"v\":1,\"view\":\"history\",\"ha\":\"" + ts + "\",\"id\":\"0f8fad5bd9cb469fa16570867728950e\"}"), V::History},
            {"view 不是字符串", b64url("{\"v\":1,\"view\":1,\"ha\":\"" + ts + "\",\"id\":\"" + id + "\"}"), V::History},
            {"过长", std::string(600, 'A'), V::Open},
        };
        for (const auto &item : bad) {
            const bool rejected = !TaskService::decodeCursor(item.raw, item.view).has_value();
            CHECK(rejected);
            if (!rejected) usapptest::note(std::string("畸形游标被接受: ") + item.label);
        }
        // 非零剩余位：同一字节串的非规范编码
        std::string nonCanonical = TaskService::encodeCursor(history);
        if (!nonCanonical.empty() && nonCanonical.size() % 4 != 0) {
            char &lastChar = nonCanonical.back();
            lastChar = lastChar == 'B' ? 'C' : (lastChar == 'A' ? 'B' : lastChar);
            if (nonCanonical != historyRaw) {
                CHECK_FALSE(TaskService::decodeCursor(nonCanonical, V::History).has_value());
            }
        }
    }

    TEST_CASE("TaskService: 生产 SQL 的成员关系只取自会话参数，绑定参数与分页键") {
        using Q = TaskService::ListQuery;
        for (const Q q : {Q::OpenFirst, Q::OpenAfterDated, Q::OpenAfterUndated, Q::HistoryFirst, Q::HistoryAfter}) {
            const std::string sql = TaskService::listSql(q);
            CHECK_CONTAINS(sql, "WHERE m.user_id = $1::uuid");
            CHECK_CONTAINS(sql, "t.couple_id = m.couple_id");
            CHECK_CONTAINS(sql, "::integer) x ON true");
            CHECK_NOT_CONTAINS(sql, "OFFSET");
        }
        const std::string dated = TaskService::listSql(Q::OpenAfterDated);
        CHECK_CONTAINS(dated, "(t.deadline_at IS NOT NULL AND (t.deadline_at, t.created_at, COALESCE(t.recurrence_id, t.id)) > "
                              "($2::timestamptz, $3::timestamptz, $4::uuid)) OR t.deadline_at IS NULL");
        CHECK_CONTAINS(dated, "LIMIT $5::integer");
        const std::string undated = TaskService::listSql(Q::OpenAfterUndated);
        CHECK_CONTAINS(undated, "t.deadline_at IS NULL AND (t.created_at, COALESCE(t.recurrence_id, t.id)) > ($2::timestamptz, $3::uuid)");
        CHECK_CONTAINS(undated, "LIMIT $4::integer");
        CHECK_CONTAINS(TaskService::listSql(Q::OpenFirst),
                       "ORDER BY t.deadline_at ASC NULLS LAST, t.created_at ASC, COALESCE(t.recurrence_id, t.id) ASC LIMIT $2::integer");
        const std::string historyAfter = TaskService::listSql(Q::HistoryAfter);
        // M6: a closed row's history key is its immutable closed_at (completed_at, else closed_at, else deadline_at).
        CHECK_CONTAINS(historyAfter,
                       "(COALESCE(c.completed_at, kc.closed_at, t.deadline_at), t.id) < ($2::timestamptz, $3::uuid)");
        CHECK_CONTAINS(historyAfter, "interval '30 days'");
        CHECK_CONTAINS(historyAfter,
                       "ORDER BY COALESCE(c.completed_at, kc.closed_at, t.deadline_at) DESC, t.id DESC LIMIT $4::integer");
        CHECK_CONTAINS(historyAfter, "HH24:MI:SS.US");
        CHECK_CONTAINS(TaskService::getSql(), "WHERE m.user_id = $1::uuid");
        CHECK_CONTAINS(TaskService::getSql(), "t.couple_id = m.couple_id AND COALESCE(t.recurrence_id, t.id) = $2::uuid");
    }

    TEST_CASE("TaskService: Task 响应形状与结果到 HTTP 的映射") {
        TaskService::TaskData task;
        task.id = "0f8fad5b-d9cb-469f-a165-70867728950e";
        task.title = "T";
        task.creatorId = "a";
        task.assigneeId = "b";
        task.rewardFlowers = 3;
        task.status = "open";
        task.createdAt = "2026-10-09T01:02:03.123456Z";
        Json::Value json = TaskService::taskJson(task);
        CHECK_EQ(json.size(), 12u);
        CHECK(json["reaction"].isNull());
        CHECK(json["deadline"].isNull());
        CHECK(json["completed_at"].isNull());
        CHECK(json["reward_flowers"].isInt());
        CHECK_FALSE(json.isMember("couple_id"));
        CHECK_FALSE(json.isMember("flower"));
        CHECK_FALSE(json.isMember("wallet"));
        task.deadlineLocalDate = "2026-10-20";
        task.deadlineAt = "2026-10-20T16:00:00.000000Z";
        json = TaskService::taskJson(task);
        CHECK(json["deadline"].isObject());
        CHECK_EQ(json["deadline"].size(), 3u);
        CHECK(json["deadline"]["local_time"].isNull());
        task.deadlineLocalTime = "23:00";
        CHECK_EQ(TaskService::taskJson(task)["deadline"]["local_time"].asString(), std::string("23:00"));

        using O = TaskService::Outcome;
        CHECK_EQ(TaskService::httpStatusFor(O::Created), 201);
        CHECK_EQ(TaskService::httpStatusFor(O::Replayed), 200);
        CHECK_EQ(TaskService::httpStatusFor(O::Ok), 200);
        CHECK_EQ(TaskService::httpStatusFor(O::Validation), 400);
        CHECK_EQ(TaskService::httpStatusFor(O::SpaceNotFound), 404);
        CHECK_EQ(TaskService::httpStatusFor(O::TaskNotFound), 404);
        CHECK_EQ(TaskService::httpStatusFor(O::PartnerRequired), 409);
        CHECK_EQ(TaskService::httpStatusFor(O::KeyReused), 409);
        CHECK_EQ(TaskService::httpStatusFor(O::ResultUnknown), 503);
        CHECK_EQ(TaskService::httpStatusFor(O::Error), 500);
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::SpaceNotFound))),
                 std::string("SPACE_NOT_FOUND"));
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::TaskNotFound))),
                 std::string("TASK_NOT_FOUND"));
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::PartnerRequired))),
                 std::string("PARTNER_REQUIRED"));
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::KeyReused))),
                 std::string("IDEMPOTENCY_KEY_REUSED"));
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::ResultUnknown))),
                 std::string("RESULT_UNKNOWN"));
        CHECK_EQ(std::string(usapp::errorCodeToString(TaskService::errorCodeFor(O::Error))),
                 std::string("INTERNAL_ERROR"));
    }

    TEST_CASE("TaskService: 失败详情是固定标签，非 [a-z_] 字符一律替换") {
        CHECK_EQ(TaskService::failureDetail("task_create", "claim", "db_error"),
                 std::string("task_create stage=claim reason=db_error"));
        const std::string injected = TaskService::failureDetail("task_get", "query\nERROR: \"Title\"", "x y");
        CHECK_NOT_CONTAINS(injected, "\n");
        CHECK_NOT_CONTAINS(injected, "ERROR");
        CHECK_NOT_CONTAINS(injected, "Title");
        CHECK_EQ(TaskService::failureDetail("", "", ""), std::string("? stage=? reason=?"));
    }

    TEST_CASE("Task startup: 缺报告、缺表、角色不符或不匹配都拒绝启动，且不含连接信息") {
        const auto absent = usapp::db::checkTaskPrivilegesForStartup(nullptr);
        CHECK(absent.state == usapp::db::TaskStartupResult::State::QueryError);
        CHECK_FALSE(absent.allowed());
        CHECK_FALSE(absent.report.has_value());
        CHECK_FALSE(usapp::db::taskStartupAllowed(std::nullopt));

        // 空结果：所有必需项缺失。
        CHECK_FALSE(usapp::db::taskStartupAllowed(usapp::db::TaskPrivilegePolicy::evaluate({})));

        // 两张任务表都不存在：exists=false，列级检查缺失。
        std::vector<usapp::db::WalletPrivilegeRow> rows;
        auto add = [&rows](const std::string &kind, const std::string &subject, const std::string &object,
                           const std::string &privilege, std::optional<bool> actual) {
            usapp::db::WalletPrivilegeRow row;
            row.kind = kind;
            row.subject = subject;
            row.object = object;
            row.privilege = privilege;
            row.actual = actual;
            rows.push_back(row);
        };
        add("identity", "current", "usapp_api", "is_app_role", true);
        add("identity", "current", "", "rolsuper", false);
        add("identity", "current", "", "has_role_memberships", false);
        add("identity", "usapp_push", "", "role_exists", true);
        for (const std::string table : {"tasks", "task_completions"}) {
            add("table", "current", table, "exists", false);
            add("table", "current", table, "owned_by_current_user", std::nullopt);
        }
        const auto missingTables = usapp::db::TaskPrivilegePolicy::evaluate(rows);
        CHECK_EQ(missingTables.currentRole, std::string("usapp_api"));
        CHECK_FALSE(usapp::db::taskStartupAllowed(missingTables));
        const std::string summary = usapp::db::TaskPrivilegePolicy::summary(missingTables);
        CHECK_CONTAINS(summary, "task privilege policy");
        CHECK_CONTAINS(summary, "ok=false");
        CHECK_NOT_CONTAINS(summary, "postgresql://");
        CHECK_NOT_CONTAINS(summary, "password");
    }
}

}  // namespace usapptest::taskunit
