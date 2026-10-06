#!/bin/sh
set -e

# =====================================================================
# deploy/docker-entrypoint-initdb.d/01-init-roles.sh
#
# PostgreSQL 17 容器初始化阶段：由官方镜像 entrypoint 自动以超级用户身份执行。
# 作用：
#   1. 创建应用运行账户 usapp_api（NOSUPERUSER），配置密码。
#   2. 创建推送辅助账户 usapp_push（NOSUPERUSER），配置密码。
#   3. 配置最小权限与默认权限，实现应用、推送与超级用户的职责隔离。
# =====================================================================

API_USER="${POSTGRES_API_USER:-usapp_api}"
API_PASS="${POSTGRES_API_PASSWORD:-change-me-locally}"
PUSH_USER="${POSTGRES_PUSH_USER:-usapp_push}"
PUSH_PASS="${POSTGRES_PUSH_PASSWORD:-change-me-locally}"
DB_NAME="${POSTGRES_DB:-usapp}"

echo "[init-roles] 初始化数据库角色与权限控制: db=${DB_NAME}, api_user=${API_USER}, push_user=${PUSH_USER}"

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$DB_NAME" <<-EOSQL
    -- 1. 创建受限运行角色（若不存在）
    DO \$\$
    BEGIN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${API_USER}') THEN
            CREATE ROLE ${API_USER} WITH LOGIN PASSWORD '${API_PASS}' NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
        ELSE
            ALTER ROLE ${API_USER} WITH PASSWORD '${API_PASS}' NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
        END IF;

        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${PUSH_USER}') THEN
            CREATE ROLE ${PUSH_USER} WITH LOGIN PASSWORD '${PUSH_PASS}' NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
        ELSE
            ALTER ROLE ${PUSH_USER} WITH PASSWORD '${PUSH_PASS}' NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION;
        END IF;
    END
    \$\$;

    -- 2. 数据库连接与 Schema 访问
    GRANT CONNECT ON DATABASE "${DB_NAME}" TO ${API_USER};
    GRANT CONNECT ON DATABASE "${DB_NAME}" TO ${PUSH_USER};

    GRANT USAGE ON SCHEMA public TO ${API_USER};
    GRANT USAGE ON SCHEMA public TO ${PUSH_USER};

    -- 3. 设置默认权限：由超级用户执行迁移创建的表与序列，自动授权给 api 账户
    ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO ${API_USER};
    ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT ON SEQUENCES TO ${API_USER};

    -- 4. 授权现有表与序列（若已存在）
    GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO ${API_USER};
    GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO ${API_USER};

    -- 5. 推送账户严格隔离：阶段 0 绝不授予用户、空间、管理员及审计表的任何权限
    REVOKE ALL ON ALL TABLES IN SCHEMA public FROM ${PUSH_USER};
    REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM ${PUSH_USER};
EOSQL

echo "[init-roles] 角色与权限初始化完成。"
