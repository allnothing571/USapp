-- =====================================================================
-- 0001_common.sql
-- 通用触发器函数。阶段 0 只提供 updated_at 维护。
-- =====================================================================

BEGIN;

CREATE OR REPLACE FUNCTION usapp_set_updated_at()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END;
$$;

COMMENT ON FUNCTION usapp_set_updated_at() IS
    '通用 updated_at 维护触发器；由各表 BEFORE UPDATE 触发。';

COMMIT;
