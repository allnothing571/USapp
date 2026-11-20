-- 0049: owner-approved bounded, audited admin wallet corrections.
-- Original 49 migrations unchanged; rollback-only dual-role validation passed before numbering.
-- One correction, bounded child operations, true admin actor, legacy entry bounds unchanged.
-- SQLSTATE UV101 link, UV102 audit immutability, UV103 internal definite refusal; lock 33032.
BEGIN;

ALTER TABLE wallet_operations ADD COLUMN actor_admin_id uuid REFERENCES admin_users(id) ON DELETE RESTRICT;
ALTER TABLE wallet_operations ALTER COLUMN actor_user_id DROP NOT NULL;
ALTER TABLE wallet_operations DROP CONSTRAINT wallet_operations_type_valid;
ALTER TABLE wallet_operations ADD CONSTRAINT wallet_operations_type_valid CHECK
 (operation_type IN ('TASK_REWARD','REWARD_REDEMPTION','REDEMPTION_REFUND','AVATAR_PURCHASE','ADMIN_ADJUSTMENT'));
ALTER TABLE wallet_operations ADD CONSTRAINT wallet_operations_actor_kind CHECK
 ((operation_type='ADMIN_ADJUSTMENT' AND actor_admin_id IS NOT NULL AND actor_user_id IS NULL)
  OR (operation_type<>'ADMIN_ADJUSTMENT' AND actor_admin_id IS NULL AND actor_user_id IS NOT NULL));

CREATE TABLE admin_wallet_adjustments (
 id uuid PRIMARY KEY,
 admin_id uuid NOT NULL REFERENCES admin_users(id) ON DELETE RESTRICT,
 couple_id uuid NOT NULL,
 user_id uuid NOT NULL,
 asset text NOT NULL CHECK(asset IN ('flower','sunshine')),
 delta integer NOT NULL CHECK(delta BETWEEN -9999 AND 9999 AND delta<>0),
 expected_seq bigint NOT NULL CHECK(expected_seq>=0),
 balance_before bigint NOT NULL CHECK(balance_before>=0),
 balance_after bigint NOT NULL CHECK(balance_after>=0 AND balance_after=balance_before+delta),
 operation_ids uuid[] NOT NULL CHECK(cardinality(operation_ids) BETWEEN 1 AND 11),
 reason text NOT NULL CHECK(char_length(reason) BETWEEN 5 AND 500 AND reason=btrim(reason)
   AND reason !~ '[\x01-\x1f\x7f-\x9f]'),
 idempotency_key text NOT NULL CHECK(idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
 request_fingerprint text NOT NULL CHECK(request_fingerprint ~ '^[0-9a-f]{64}$'),
 audit_id bigint NOT NULL UNIQUE,
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 UNIQUE(admin_id,idempotency_key),
 FOREIGN KEY(couple_id,user_id) REFERENCES couple_members(couple_id,user_id) ON DELETE RESTRICT,
 CHECK((asset='flower' AND balance_after<=1000000) OR (asset='sunshine' AND balance_after<=1000000000))
);
CREATE INDEX admin_wallet_adjustments_user_idx ON admin_wallet_adjustments(user_id,created_at DESC,id DESC);
CREATE TRIGGER admin_wallet_adjustments_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON admin_wallet_adjustments
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_wallet_reject_mutation();

CREATE FUNCTION usapp_admin_adjustment_fingerprint(p_user uuid,p_asset text,p_delta integer,p_seq bigint,p_reason text)
RETURNS text LANGUAGE sql IMMUTABLE STRICT AS $$
 SELECT encode(sha256(convert_to('admin-wallet-adjust:v1:'||p_user::text||':'||p_asset||':'||p_delta::text||':'||p_seq::text||':'||p_reason,'UTF8')),'hex')
$$;

CREATE FUNCTION usapp_admin_adjustment_valid(p_id uuid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a admin_wallet_adjustments%ROWTYPE; o wallet_operations%ROWTYPE; e wallet_ledger_entries%ROWTYPE;
 n integer; k integer; chunk integer; left_over integer; prior bigint; audit audit_logs%ROWTYPE;
BEGIN
 SELECT * INTO a FROM admin_wallet_adjustments WHERE id=p_id;
 IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_PARENT'; END IF;
 n:=CASE WHEN a.asset='flower' THEN (abs(a.delta)+998)/999 ELSE 1 END;
 IF cardinality(a.operation_ids)<>n OR array_position(a.operation_ids,NULL) IS NOT NULL
    OR (SELECT count(DISTINCT x) FROM unnest(a.operation_ids) x)<>n
    OR a.request_fingerprint IS DISTINCT FROM usapp_admin_adjustment_fingerprint(a.user_id,a.asset,a.delta,a.expected_seq,a.reason)
 THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_TERMS'; END IF;
 SELECT * INTO audit FROM audit_logs WHERE id=a.audit_id;
 IF NOT FOUND OR ROW(audit.actor_type,audit.actor_id,audit.couple_id,audit.action,audit.target_type,audit.target_id)
    IS DISTINCT FROM ROW('admin'::text,a.admin_id,a.couple_id,'admin.wallet.adjusted'::text,'admin_wallet_adjustment'::text,a.id::text)
    OR audit.detail IS DISTINCT FROM jsonb_build_object('user_id',a.user_id,'asset',a.asset,'delta',a.delta::text,
      'expected_seq',a.expected_seq::text,'balance_before',a.balance_before::text,'balance_after',a.balance_after::text,'reason',a.reason)
 THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_AUDIT'; END IF;
 prior:=COALESCE((SELECT balance_after FROM wallet_ledger_entries WHERE user_id=a.user_id AND asset_type=a.asset AND seq=a.expected_seq),0);
 IF (a.expected_seq>0 AND NOT EXISTS(SELECT 1 FROM wallet_ledger_entries WHERE user_id=a.user_id AND asset_type=a.asset AND seq=a.expected_seq))
    OR prior<>a.balance_before THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_BASE'; END IF;
 left_over:=a.delta;
 FOR k IN 1..n LOOP
  chunk:=CASE WHEN a.asset='flower' THEN sign(left_over)*least(abs(left_over),999) ELSE left_over END;
  SELECT * INTO o FROM wallet_operations WHERE id=a.operation_ids[k];
  IF NOT FOUND OR ROW(o.couple_id,o.actor_user_id,o.actor_admin_id,o.operation_type,o.source_type,o.source_id,o.idempotency_key,o.request_fingerprint)
    IS DISTINCT FROM ROW(a.couple_id,NULL::uuid,a.admin_id,'ADMIN_ADJUSTMENT'::text,'admin_wallet_adjustment'::text,a.id::text||':'||k::text,NULL::text,NULL::text)
  THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_OPERATION'; END IF;
  IF (SELECT count(*) FROM wallet_ledger_entries WHERE operation_id=o.id)<>1
  THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_ENTRY_COUNT'; END IF;
  SELECT * INTO e FROM wallet_ledger_entries WHERE operation_id=o.id;
  IF ROW(e.couple_id,e.user_id,e.asset_type,e.seq,e.amount,e.balance_after)
     IS DISTINCT FROM ROW(a.couple_id,a.user_id,a.asset,a.expected_seq+k,chunk::bigint,prior+chunk)
  THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_ENTRY'; END IF;
  left_over:=left_over-chunk; prior:=prior+chunk;
 END LOOP;
 IF left_over<>0 OR prior<>a.balance_after OR
   (SELECT count(*) FROM wallet_operations WHERE operation_type='ADMIN_ADJUSTMENT'
    AND source_type='admin_wallet_adjustment' AND split_part(source_id,':',1)=a.id::text)<>n
 THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_TOTAL'; END IF;
END $$;

CREATE FUNCTION usapp_admin_adjustment_link() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE o wallet_operations%ROWTYPE; parent_id uuid;
BEGIN
 IF TG_TABLE_NAME='admin_wallet_adjustments' THEN parent_id:=NEW.id;
 ELSE
  IF TG_TABLE_NAME='wallet_operations' THEN o:=NEW;
  ELSE SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id; END IF;
  IF o.operation_type IS DISTINCT FROM 'ADMIN_ADJUSTMENT' THEN RETURN NULL; END IF;
  IF o.source_type<>'admin_wallet_adjustment' OR o.source_id !~ '^[a-f0-9]{8}(-[a-f0-9]{4}){3}-[a-f0-9]{12}:[1-9][0-9]?$'
  THEN RAISE EXCEPTION USING ERRCODE='UV101',MESSAGE='ADMIN_ADJUSTMENT_SOURCE'; END IF;
  parent_id:=split_part(o.source_id,':',1)::uuid;
 END IF;
 PERFORM usapp_admin_adjustment_valid(parent_id); RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER admin_wallet_adjustments_valid AFTER INSERT ON admin_wallet_adjustments
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_admin_adjustment_link();
CREATE CONSTRAINT TRIGGER wallet_operations_admin_adjustment_valid AFTER INSERT ON wallet_operations
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN(NEW.operation_type='ADMIN_ADJUSTMENT') EXECUTE FUNCTION usapp_admin_adjustment_link();
CREATE CONSTRAINT TRIGGER wallet_entries_admin_adjustment_valid AFTER INSERT ON wallet_ledger_entries
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_admin_adjustment_link();

-- Existing audit permissions are broader. Protect every correction audit even before/after its parent is read.
CREATE FUNCTION usapp_admin_adjustment_audit_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF TG_OP='TRUNCATE' THEN
  IF EXISTS(SELECT 1 FROM audit_logs WHERE action='admin.wallet.adjusted') THEN
   RAISE EXCEPTION USING ERRCODE='UV102',MESSAGE='ADMIN_ADJUSTMENT_AUDIT_IMMUTABLE'; END IF;
 ELSIF OLD.action='admin.wallet.adjusted' THEN
  RAISE EXCEPTION USING ERRCODE='UV102',MESSAGE='ADMIN_ADJUSTMENT_AUDIT_IMMUTABLE';
 END IF;
 IF TG_OP='DELETE' THEN RETURN OLD; ELSIF TG_OP='UPDATE' THEN RETURN NEW; END IF;
 RETURN NULL;
END $$;
CREATE TRIGGER audit_admin_adjustment_immutable BEFORE UPDATE OR DELETE ON audit_logs
 FOR EACH ROW EXECUTE FUNCTION usapp_admin_adjustment_audit_immutable();
CREATE TRIGGER audit_admin_adjustment_no_truncate BEFORE TRUNCATE ON audit_logs
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_admin_adjustment_audit_immutable();

CREATE FUNCTION usapp_admin_wallet_adjust(p_admin uuid,p_user uuid,p_asset text,p_delta integer,p_seq bigint,p_reason text,p_key text)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE a admin_wallet_adjustments%ROWTYPE; space uuid; fp text; balance bigint; last_seq bigint;
 ids uuid[]:='{}'; n integer; k integer; chunk integer; left_over integer; aid bigint; ident uuid:=gen_random_uuid();
BEGIN
 IF NOT EXISTS(SELECT 1 FROM admin_users WHERE id=p_admin AND status='active' FOR SHARE) THEN
  RETURN jsonb_build_object('error','admin_forbidden'); END IF;
 IF p_user IS NULL OR p_asset IS NULL OR p_asset NOT IN('flower','sunshine') OR p_delta IS NULL OR p_delta=0 OR abs(p_delta::bigint)>9999
   OR p_seq IS NULL OR p_seq<0 OR p_reason IS NULL OR char_length(p_reason) NOT BETWEEN 5 AND 500 OR p_reason<>btrim(p_reason)
   OR p_reason ~ '[\x01-\x1f\x7f-\x9f]' OR p_key IS NULL OR p_key !~ '^[A-Za-z0-9_-]{16,128}$'
 THEN RETURN jsonb_build_object('error','validation'); END IF;
 fp:=usapp_admin_adjustment_fingerprint(p_user,p_asset,p_delta,p_seq,p_reason);
 PERFORM pg_advisory_xact_lock(33032,hashtext(p_admin::text||':'||p_key));
 SELECT * INTO a FROM admin_wallet_adjustments WHERE admin_id=p_admin AND idempotency_key=p_key;
 IF FOUND THEN
  IF a.request_fingerprint<>fp THEN RETURN jsonb_build_object('error','key_reused'); END IF;
 ELSE
  BEGIN
   SELECT m.couple_id INTO space FROM couple_members m JOIN users u ON u.id=m.user_id
    WHERE m.user_id=p_user AND u.status='active' FOR SHARE OF m,u;
   IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV103',MESSAGE='target_unavailable'; END IF;
   INSERT INTO wallet_accounts(user_id,asset_type,couple_id) VALUES(p_user,p_asset,space) ON CONFLICT DO NOTHING;
   PERFORM 1 FROM wallet_accounts WHERE user_id=p_user AND asset_type=p_asset AND couple_id=space FOR UPDATE;
   IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV103',MESSAGE='target_unavailable'; END IF;
   SELECT seq,balance_after INTO last_seq,balance FROM wallet_ledger_entries WHERE user_id=p_user AND asset_type=p_asset ORDER BY seq DESC LIMIT 1;
   last_seq:=COALESCE(last_seq,0); balance:=COALESCE(balance,0);
   IF last_seq<>p_seq THEN RAISE EXCEPTION USING ERRCODE='UV103',MESSAGE='conflict'; END IF;
   IF balance+p_delta<0 OR balance+p_delta>(CASE WHEN p_asset='flower' THEN 1000000 ELSE 1000000000 END)
    THEN RAISE EXCEPTION USING ERRCODE='UV103',MESSAGE='balance_bound'; END IF;
   n:=CASE WHEN p_asset='flower' THEN (abs(p_delta)+998)/999 ELSE 1 END;
   FOR k IN 1..n LOOP ids:=array_append(ids,gen_random_uuid()); END LOOP;
   INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
    VALUES('admin',p_admin,space,'admin.wallet.adjusted','admin_wallet_adjustment',ident::text,
     jsonb_build_object('user_id',p_user,'asset',p_asset,'delta',p_delta::text,'expected_seq',p_seq::text,
      'balance_before',balance::text,'balance_after',(balance+p_delta)::text,'reason',p_reason)) RETURNING id INTO aid;
   INSERT INTO admin_wallet_adjustments(id,admin_id,couple_id,user_id,asset,delta,expected_seq,balance_before,balance_after,operation_ids,reason,idempotency_key,request_fingerprint,audit_id)
    VALUES(ident,p_admin,space,p_user,p_asset,p_delta,p_seq,balance,balance+p_delta,ids,p_reason,p_key,fp,aid);
   left_over:=p_delta;
   FOR k IN 1..n LOOP
    chunk:=CASE WHEN p_asset='flower' THEN sign(left_over)*least(abs(left_over),999) ELSE left_over END;
    INSERT INTO wallet_operations(id,couple_id,actor_admin_id,operation_type,source_type,source_id)
     VALUES(ids[k],space,p_admin,'ADMIN_ADJUSTMENT','admin_wallet_adjustment',ident::text||':'||k::text);
    INSERT INTO wallet_ledger_entries(id,operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
     VALUES(gen_random_uuid(),ids[k],space,p_user,p_asset,last_seq+k,chunk,balance+chunk);
    left_over:=left_over-chunk; balance:=balance+chunk;
   END LOOP;
   SET CONSTRAINTS admin_wallet_adjustments_valid,wallet_operations_admin_adjustment_valid,wallet_entries_admin_adjustment_valid,wallet_operations_require_entries IMMEDIATE;
   SET CONSTRAINTS admin_wallet_adjustments_valid,wallet_operations_admin_adjustment_valid,wallet_entries_admin_adjustment_valid,wallet_operations_require_entries DEFERRED;
   SELECT * INTO a FROM admin_wallet_adjustments WHERE id=ident;
  EXCEPTION WHEN SQLSTATE 'UV103' THEN RETURN jsonb_build_object('error',SQLERRM);
  END;
 END IF;
 RETURN jsonb_build_object('adjustment',jsonb_build_object('id',a.id,'admin_id',a.admin_id,'user_id',a.user_id,'couple_id',a.couple_id,
   'asset',a.asset,'delta',a.delta::text,'expected_seq',a.expected_seq::text,'balance_before',a.balance_before::text,
   'balance_after',a.balance_after::text,'operation_count',cardinality(a.operation_ids),'last_seq',(a.expected_seq+cardinality(a.operation_ids))::text,
   'reason',a.reason,'created_at',to_char(a.created_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.US"Z"')));
END $$;

REVOKE ALL ON admin_wallet_adjustments FROM PUBLIC,usapp_api,usapp_push;
GRANT SELECT,INSERT ON admin_wallet_adjustments TO usapp_api;
REVOKE ALL ON FUNCTION usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text) FROM PUBLIC,usapp_push;
GRANT EXECUTE ON FUNCTION usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text) TO usapp_api;
REVOKE ALL ON FUNCTION usapp_admin_adjustment_fingerprint(uuid,text,integer,bigint,text),usapp_admin_adjustment_valid(uuid),usapp_admin_adjustment_link(),usapp_admin_adjustment_audit_immutable() FROM PUBLIC,usapp_push;
GRANT EXECUTE ON FUNCTION usapp_admin_adjustment_fingerprint(uuid,text,integer,bigint,text),usapp_admin_adjustment_valid(uuid) TO usapp_api;
COMMIT;
