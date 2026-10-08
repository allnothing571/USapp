// =====================================================================
// services/api/src/db/WalletPrivilegePolicy.cc
// =====================================================================
#include "db/WalletPrivilegePolicy.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace usapp::db {

namespace {

// 只读矩阵查询：只返回实际值，期望值由 C++ 的 expectedFor 决定。
// 表或角色不存在时对应权限为 NULL（报告中按不符合处理），绝不因缺表报错中断。
// has_*_privilege 计入直接授权、PUBLIC 授权与继承授权；非继承角色经 SET ROLE
// 可获得的权限不计入，因此另有 has_role_memberships 检查。
const char *kMatrixSql = R"SQL(
WITH
wallet_tables(table_name) AS (
    VALUES ('wallet_accounts'), ('wallet_operations'), ('wallet_ledger_entries')
),
resolved AS (
    SELECT w.table_name, to_regclass('public.' || w.table_name)::oid AS rel
      FROM wallet_tables w
),
subjects(subject, role_name) AS (
    VALUES ('current', current_user::text), ('usapp_push', 'usapp_push'), ('public', 'public')
),
subject_state AS (
    SELECT s.subject, s.role_name,
           (s.subject = 'public'
            OR EXISTS (SELECT 1 FROM pg_roles r WHERE r.rolname = s.role_name)) AS role_exists
      FROM subjects s
),
table_privs(privilege) AS (
    VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'),
           ('TRUNCATE'), ('REFERENCES'), ('TRIGGER'), ('MAINTAIN')
),
column_privs(privilege) AS (
    VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')
)
SELECT 'identity'::text AS kind, 'current'::text AS subject, current_user::text AS object,
       ''::text AS column_name, 'is_app_role'::text AS privilege,
       (current_user = 'usapp_api') AS actual
UNION ALL
SELECT 'identity', 'current', '', '', 'rolsuper',
       (SELECT r.rolsuper FROM pg_roles r WHERE r.rolname = current_user)
UNION ALL
SELECT 'identity', 'current', '', '', 'has_role_memberships',
       EXISTS (SELECT 1
                 FROM pg_auth_members m
                 JOIN pg_roles r ON r.oid = m.member
                WHERE r.rolname = current_user)
UNION ALL
SELECT 'identity', 'usapp_push', '', '', 'role_exists', s.role_exists
  FROM subject_state s
 WHERE s.subject = 'usapp_push'
UNION ALL
SELECT 'table', 'current', t.table_name, '', 'exists', t.rel IS NOT NULL
  FROM resolved t
UNION ALL
SELECT 'table', 'current', t.table_name, '', 'owned_by_current_user',
       (SELECT c.relowner = (SELECT r.oid FROM pg_roles r WHERE r.rolname = current_user)
          FROM pg_class c
         WHERE c.oid = t.rel)
  FROM resolved t
UNION ALL
SELECT 'table_privilege', s.subject, t.table_name, '', p.privilege,
       CASE WHEN t.rel IS NULL OR NOT s.role_exists THEN NULL
            ELSE has_table_privilege(s.role_name, t.rel, p.privilege) END
  FROM subject_state s CROSS JOIN resolved t CROSS JOIN table_privs p
UNION ALL
SELECT 'any_column_privilege', s.subject, t.table_name, '', p.privilege,
       CASE WHEN t.rel IS NULL OR NOT s.role_exists THEN NULL
            ELSE has_any_column_privilege(s.role_name, t.rel, p.privilege) END
  FROM subject_state s CROSS JOIN resolved t CROSS JOIN column_privs p
UNION ALL
SELECT 'column_privilege', s.subject, t.table_name, a.attname::text, 'UPDATE',
       CASE WHEN NOT s.role_exists THEN NULL
            ELSE has_column_privilege(s.role_name, t.rel, a.attnum, 'UPDATE') END
  FROM subject_state s
 CROSS JOIN resolved t
  JOIN pg_attribute a ON a.attrelid = t.rel AND a.attnum > 0 AND NOT a.attisdropped
)SQL";

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 完整性检查用的键。identity 行的 object 是角色名，不参与键。
std::string keyOf(const std::string &kind,
                  const std::string &subject,
                  const std::string &object,
                  const std::string &column,
                  const std::string &privilege) {
    std::string key = kind + "/" + subject + "/";
    if (kind != "identity") {
        key += object;
        if (!column.empty()) {
            key += "." + column;
        }
        key += "/";
    }
    key += privilege;
    return key;
}

std::string keyOf(const WalletPrivilegeRow &row) {
    return keyOf(row.kind, row.subject, row.object, row.column, row.privilege);
}

std::set<std::string> requiredKeys() {
    std::set<std::string> keys;
    keys.insert(keyOf("identity", "current", "", "", "is_app_role"));
    keys.insert(keyOf("identity", "current", "", "", "rolsuper"));
    keys.insert(keyOf("identity", "current", "", "", "has_role_memberships"));
    keys.insert(keyOf("identity", "usapp_push", "", "", "role_exists"));
    for (const auto &table : WalletPrivilegePolicy::walletTables()) {
        keys.insert(keyOf("table", "current", table, "", "exists"));
        keys.insert(keyOf("table", "current", table, "", "owned_by_current_user"));
        for (const auto &subject : WalletPrivilegePolicy::subjects()) {
            for (const auto &privilege : WalletPrivilegePolicy::tablePrivileges()) {
                keys.insert(keyOf("table_privilege", subject, table, "", privilege));
            }
            for (const auto &privilege : WalletPrivilegePolicy::anyColumnPrivileges()) {
                keys.insert(keyOf("any_column_privilege", subject, table, "", privilege));
            }
        }
    }
    // 列是动态枚举的；至少 lock_token 那一列必须出现在每个主体的结果里。
    for (const auto &subject : WalletPrivilegePolicy::subjects()) {
        keys.insert(keyOf("column_privilege", subject, WalletPrivilegePolicy::kLockColumnTable,
                          WalletPrivilegePolicy::kLockColumn, "UPDATE"));
    }
    return keys;
}

std::string describe(const WalletPrivilegeCheck &check) {
    const auto &row = check.row;
    std::ostringstream os;
    os << WalletPrivilegePolicy::safeName(row.kind) << " " << WalletPrivilegePolicy::safeName(row.subject);
    if (row.kind != "identity") {
        os << " " << WalletPrivilegePolicy::safeName(row.object);
        if (!row.column.empty()) {
            os << "." << WalletPrivilegePolicy::safeName(row.column);
        }
    }
    os << " " << WalletPrivilegePolicy::safeName(row.privilege) << " expected="
       << (check.expected ? "true" : "false") << " actual=";
    if (row.actual.has_value()) {
        os << (*row.actual ? "true" : "false");
    } else {
        os << "null";
    }
    return os.str();
}

}  // namespace

const std::vector<std::string> &WalletPrivilegePolicy::walletTables() {
    static const std::vector<std::string> tables = {"wallet_accounts", "wallet_operations",
                                                    "wallet_ledger_entries"};
    return tables;
}

const std::vector<std::string> &WalletPrivilegePolicy::subjects() {
    static const std::vector<std::string> values = {"current", "usapp_push", "public"};
    return values;
}

const std::vector<std::string> &WalletPrivilegePolicy::tablePrivileges() {
    static const std::vector<std::string> values = {"SELECT",   "INSERT",     "UPDATE",  "DELETE",
                                                    "TRUNCATE", "REFERENCES", "TRIGGER", "MAINTAIN"};
    return values;
}

const std::vector<std::string> &WalletPrivilegePolicy::anyColumnPrivileges() {
    static const std::vector<std::string> values = {"SELECT", "INSERT", "UPDATE", "REFERENCES"};
    return values;
}

const char *WalletPrivilegePolicy::matrixSql() {
    return kMatrixSql;
}

std::optional<bool> WalletPrivilegePolicy::expectedFor(const WalletPrivilegeRow &row) {
    if (row.kind == "identity") {
        if (row.subject == "current") {
            if (row.privilege == "is_app_role") return true;
            if (row.privilege == "rolsuper") return false;
            if (row.privilege == "has_role_memberships") return false;
        }
        if (row.subject == "usapp_push" && row.privilege == "role_exists") return true;
        return std::nullopt;
    }

    if (!contains(walletTables(), row.object)) {
        return std::nullopt;
    }

    if (row.kind == "table") {
        if (row.subject != "current") return std::nullopt;
        if (row.privilege == "exists") return true;
        if (row.privilege == "owned_by_current_user") return false;
        return std::nullopt;
    }

    if (!contains(subjects(), row.subject)) {
        return std::nullopt;
    }
    const bool isApp = row.subject == "current";

    if (row.kind == "table_privilege") {
        if (!contains(tablePrivileges(), row.privilege)) return std::nullopt;
        return isApp && (row.privilege == "SELECT" || row.privilege == "INSERT");
    }

    if (row.kind == "any_column_privilege") {
        if (!contains(anyColumnPrivileges(), row.privilege)) return std::nullopt;
        if (!isApp) return false;
        if (row.privilege == "SELECT" || row.privilege == "INSERT") return true;
        if (row.privilege == "UPDATE") return row.object == kLockColumnTable;
        return false;  // REFERENCES
    }

    if (row.kind == "column_privilege") {
        if (row.privilege != "UPDATE" || row.column.empty()) return std::nullopt;
        return isApp && row.object == kLockColumnTable && row.column == kLockColumn;
    }

    return std::nullopt;
}

WalletPrivilegeReport WalletPrivilegePolicy::evaluate(const std::vector<WalletPrivilegeRow> &rows) {
    WalletPrivilegeReport report;
    std::set<std::string> seen;
    for (const auto &row : rows) {
        if (row.kind == "identity" && row.privilege == "is_app_role") {
            report.currentRole = safeName(row.object);
        }
        const std::optional<bool> expected = expectedFor(row);
        if (!expected.has_value()) {
            report.unexpected.push_back(safeName(keyOf(row)));
            continue;
        }
        seen.insert(keyOf(row));
        WalletPrivilegeCheck check;
        check.row = row;
        check.expected = *expected;
        report.checks.push_back(std::move(check));
    }
    for (const auto &key : requiredKeys()) {
        if (seen.find(key) == seen.end()) {
            report.missing.push_back(safeName(key));
        }
    }
    return report;
}

std::vector<WalletPrivilegeRow> WalletPrivilegePolicy::rowsFromResult(const drogon::orm::Result &result) {
    std::vector<WalletPrivilegeRow> rows;
    rows.reserve(result.size());
    for (const auto &r : result) {
        WalletPrivilegeRow row;
        row.kind = r["kind"].as<std::string>();
        row.subject = r["subject"].as<std::string>();
        row.object = r["object"].isNull() ? std::string() : r["object"].as<std::string>();
        row.column = r["column_name"].isNull() ? std::string() : r["column_name"].as<std::string>();
        row.privilege = r["privilege"].as<std::string>();
        if (!r["actual"].isNull()) {
            row.actual = r["actual"].as<bool>();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string WalletPrivilegePolicy::safeName(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '.' || c == '-' || c == '/';
        out.push_back(safe ? c : '?');
    }
    return out;
}

void WalletPrivilegePolicy::check(const ClientPtr &client, ReportCallback onReport, ErrorCallback onError) {
    if (!client) {
        onError("钱包权限矩阵检查失败：数据库客户端未初始化");
        return;
    }
    client->execSqlAsync(
        kMatrixSql,
        [onReport](const drogon::orm::Result &result) { onReport(evaluate(rowsFromResult(result))); },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(describePgError("钱包权限矩阵查询失败", e.base()));
        });
}

std::vector<WalletPrivilegeCheck> WalletPrivilegeReport::mismatches() const {
    std::vector<WalletPrivilegeCheck> out;
    for (const auto &check : checks) {
        if (!check.ok()) {
            out.push_back(check);
        }
    }
    return out;
}

bool WalletPrivilegeReport::ok() const {
    return missing.empty() && unexpected.empty() && mismatches().empty();
}

std::string WalletPrivilegeReport::summary() const {
    const auto bad = mismatches();
    std::ostringstream os;
    os << "wallet privilege policy: role=" << (currentRole.empty() ? "?" : currentRole)
       << " ok=" << (ok() ? "true" : "false") << " checks=" << checks.size()
       << " mismatches=" << bad.size() << " missing=" << missing.size()
       << " unexpected=" << unexpected.size();
    for (const auto &check : bad) {
        os << "\n  mismatch: " << describe(check);
    }
    for (const auto &key : missing) {
        os << "\n  missing: " << key;
    }
    for (const auto &key : unexpected) {
        os << "\n  unexpected: " << key;
    }
    return os.str();
}

}  // namespace usapp::db
