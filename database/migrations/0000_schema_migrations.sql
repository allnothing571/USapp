-- =====================================================================
-- 0000_schema_migrations.sql
-- 迁移记录表。所有迁移都在显式事务中执行；失败必须整体回滚。
-- 迁移顺序 = 文件名升序。禁止修改已应用的迁移文件内容。
-- =====================================================================

BEGIN;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     text        PRIMARY KEY,
    checksum    text        NOT NULL,
    applied_by  text        NOT NULL DEFAULT current_user,
    applied_at  timestamptz NOT NULL DEFAULT now()
);

COMMENT ON TABLE schema_migrations IS
    '已应用的迁移版本；checksum 用于检测已应用文件被事后修改。';

COMMIT;
