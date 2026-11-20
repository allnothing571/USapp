-- Maintenance-owned rollback fixture. Also runs with expect_blocked=false against 0042 as the negative control.
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('usapp.test.recovery_catalog_blocked', :'expect_blocked', true);
GRANT INSERT ON public.deletion_recovery_attempts TO usapp_api;
SET LOCAL ROLE usapp_api;
CREATE TEMP TABLE pg_class (oid oid, relowner oid);
INSERT INTO pg_temp.pg_class VALUES ('public.deletion_recovery_attempts'::regclass, 'usapp_api'::regrole);
SET LOCAL search_path = pg_temp, pg_catalog, public;
DO $probe$
DECLARE blocked boolean := false;
BEGIN
 BEGIN
  INSERT INTO public.deletion_recovery_attempts(generation,database_id,attempt_id,journal_id)
   VALUES(9000000000000000000,gen_random_uuid(),gen_random_uuid(),gen_random_uuid());
 EXCEPTION WHEN SQLSTATE 'UJ001' THEN blocked := true;
 END;
 IF blocked IS DISTINCT FROM current_setting('usapp.test.recovery_catalog_blocked')::boolean THEN
  RAISE EXCEPTION 'recovery catalog guard result unexpected';
 END IF;
 RAISE NOTICE 'recovery_catalog_shadow_blocked=%', blocked;
END
$probe$;
ROLLBACK;
