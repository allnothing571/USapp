// =====================================================================
// services/api/src/db/TaskPrivilegePolicy.cc
// =====================================================================
#include "db/TaskPrivilegePolicy.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace usapp::db {

namespace {

// 与钱包矩阵查询结构相同，只把表清单换成任务表。只返回实际值。
const char *kTaskMatrixSql = R"SQL(
WITH
task_tables(table_name) AS (
    VALUES ('tasks'), ('task_completions')
),
resolved AS (
    SELECT w.table_name, to_regclass('public.' || w.table_name)::oid AS rel
      FROM task_tables w
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

// 每张表至少有一个必然存在的列要出现在列级结果里，防止列枚举为空时静默通过。
const char *requiredColumnFor(const std::string &table) {
    return table == "tasks" ? "id" : "task_id";
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

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
    for (const auto &table : TaskPrivilegePolicy::taskTables()) {
        keys.insert(keyOf("table", "current", table, "", "exists"));
        keys.insert(keyOf("table", "current", table, "", "owned_by_current_user"));
        for (const auto &subject : WalletPrivilegePolicy::subjects()) {
            for (const auto &privilege : WalletPrivilegePolicy::tablePrivileges()) {
                keys.insert(keyOf("table_privilege", subject, table, "", privilege));
            }
            for (const auto &privilege : WalletPrivilegePolicy::anyColumnPrivileges()) {
                keys.insert(keyOf("any_column_privilege", subject, table, "", privilege));
            }
            keys.insert(keyOf("column_privilege", subject, table, requiredColumnFor(table), "UPDATE"));
        }
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

const std::vector<std::string> &TaskPrivilegePolicy::taskTables() {
    static const std::vector<std::string> tables = {"tasks", "task_completions"};
    return tables;
}

const char *TaskPrivilegePolicy::matrixSql() {
    return kTaskMatrixSql;
}

std::optional<bool> TaskPrivilegePolicy::expectedFor(const WalletPrivilegeRow &row) {
    if (row.kind == "identity") {
        if (row.subject == "current") {
            if (row.privilege == "is_app_role") return true;
            if (row.privilege == "rolsuper") return false;
            if (row.privilege == "has_role_memberships") return false;
        }
        if (row.subject == "usapp_push" && row.privilege == "role_exists") return true;
        return std::nullopt;
    }

    if (!contains(taskTables(), row.object)) {
        return std::nullopt;
    }

    if (row.kind == "table") {
        if (row.subject != "current") return std::nullopt;
        if (row.privilege == "exists") return true;
        if (row.privilege == "owned_by_current_user") return false;
        return std::nullopt;
    }

    if (!contains(WalletPrivilegePolicy::subjects(), row.subject)) {
        return std::nullopt;
    }
    const bool isApp = row.subject == "current";

    if (row.kind == "table_privilege") {
        if (!contains(WalletPrivilegePolicy::tablePrivileges(), row.privilege)) return std::nullopt;
        return isApp && (row.privilege == "SELECT" || row.privilege == "INSERT");
    }

    if (row.kind == "any_column_privilege") {
        if (!contains(WalletPrivilegePolicy::anyColumnPrivileges(), row.privilege)) return std::nullopt;
        return isApp && (row.privilege == "SELECT" || row.privilege == "INSERT");
    }

    if (row.kind == "column_privilege") {
        if (row.privilege != "UPDATE" || row.column.empty()) return std::nullopt;
        return false;  // 任务表没有任何可更新的列
    }

    return std::nullopt;
}

WalletPrivilegeReport TaskPrivilegePolicy::evaluate(const std::vector<WalletPrivilegeRow> &rows) {
    WalletPrivilegeReport report;
    std::set<std::string> seen;
    for (const auto &row : rows) {
        if (row.kind == "identity" && row.privilege == "is_app_role") {
            report.currentRole = WalletPrivilegePolicy::safeName(row.object);
        }
        const std::optional<bool> expected = expectedFor(row);
        if (!expected.has_value()) {
            report.unexpected.push_back(WalletPrivilegePolicy::safeName(keyOf(row)));
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
            report.missing.push_back(WalletPrivilegePolicy::safeName(key));
        }
    }
    return report;
}

std::string TaskPrivilegePolicy::summary(const WalletPrivilegeReport &report) {
    const auto bad = report.mismatches();
    std::ostringstream os;
    os << "task privilege policy: role=" << (report.currentRole.empty() ? "?" : report.currentRole)
       << " ok=" << (report.ok() ? "true" : "false") << " checks=" << report.checks.size()
       << " mismatches=" << bad.size() << " missing=" << report.missing.size()
       << " unexpected=" << report.unexpected.size();
    for (const auto &check : bad) {
        os << "\n  mismatch: " << describe(check);
    }
    for (const auto &key : report.missing) {
        os << "\n  missing: " << key;
    }
    for (const auto &key : report.unexpected) {
        os << "\n  unexpected: " << key;
    }
    return os.str();
}

void TaskPrivilegePolicy::check(const ClientPtr &client,
                                WalletPrivilegePolicy::ReportCallback onReport,
                                WalletPrivilegePolicy::ErrorCallback onError) {
    if (!client) {
        onError("任务权限矩阵检查失败：数据库客户端未初始化");
        return;
    }
    client->execSqlAsync(
        kTaskMatrixSql,
        [onReport](const drogon::orm::Result &result) {
            onReport(evaluate(WalletPrivilegePolicy::rowsFromResult(result)));
        },
        [onError](const drogon::orm::DrogonDbException &e) {
            onError(describePgError("任务权限矩阵查询失败", e.base()));
        });
}

}  // namespace usapp::db
