-- Preserve the pre-0049 SQLSTATE for a missing ordinary member actor.
-- The column is nullable only to represent a genuine admin actor; existing user postings still reject NULL as 23502.
BEGIN;
CREATE FUNCTION usapp_wallet_member_actor_required() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF NEW.operation_type IS DISTINCT FROM 'ADMIN_ADJUSTMENT' AND NEW.actor_user_id IS NULL THEN
  RAISE EXCEPTION USING ERRCODE='23502',MESSAGE='member wallet operation requires actor_user_id',
   SCHEMA='public',TABLE='wallet_operations',COLUMN='actor_user_id';
 END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER wallet_operations_member_actor_required BEFORE INSERT ON wallet_operations
 FOR EACH ROW EXECUTE FUNCTION usapp_wallet_member_actor_required();
REVOKE ALL ON FUNCTION usapp_wallet_member_actor_required() FROM PUBLIC,usapp_push;
COMMIT;
