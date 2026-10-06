-- =====================================================================
-- database/init-roles.sql
--
-- 数据库角色赋权与最小权限收紧脚本（由超级用户 postgres 在目标库执行）。
--
-- 设计原则：
--   1. 动态针对当前连接数据库（current_database()，兼容 usapp 与 usapp_test）授权 CONNECT。
--   2. 严格校验角色存在性；角色不存在时明确中断并报错，严禁回退到弱默认密码。
--   3. usapp_api: 仅限 public 模式现有及未来业务表与序列的 DML 读写。
--   4. usapp_push: 阶段 0 严格隔离，禁止访问任何核心业务表与序列。
-- =====================================================================

-- 1. 严格校验角色存在性（不存在则报错中断，禁止弱密码自动创建）
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'usapp_api') THEN
        RAISE EXCEPTION '角色 usapp_api 不存在！请先通过容器初始化流程 (01-init-roles.sh) 或由超级用户配置安全密码创建角色，严禁使用默认弱密码。';
    END IF;

    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'usapp_push') THEN
        RAISE EXCEPTION '角色 usapp_push 不存在！请先通过容器初始化流程 (01-init-roles.sh) 或由超级用户配置安全密码创建角色，严禁使用默认弱密码。';
    END IF;
END
$$;

-- 2. 动态针对当前连接的数据库授权 CONNECT（兼容 usapp 与 usapp_test）
DO $$
DECLARE
    cur_db text := current_database();
BEGIN
    EXECUTE format('GRANT CONNECT ON DATABASE %I TO usapp_api, usapp_push;', cur_db);
END
$$;

-- 3. Schema 使用权限
GRANT USAGE ON SCHEMA public TO usapp_api;
GRANT USAGE ON SCHEMA public TO usapp_push;

-- 4. 设置默认权限：后续由超级用户执行迁移创建的新表与序列，自动对 usapp_api 授权 DML
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO usapp_api;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT ON SEQUENCES TO usapp_api;

-- 5. 现有表与序列授权（针对当前已执行完迁移的表）
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO usapp_api;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO usapp_api;

-- 6. 推送账户权限收紧（阶段 0 绝不授予用户、空间、管理员及审计表权限）
REVOKE ALL ON ALL TABLES IN SCHEMA public FROM usapp_push;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM usapp_push;
