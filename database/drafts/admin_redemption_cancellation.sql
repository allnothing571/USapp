-- DRAFT: validate with rollback on an owned disposable 0050 database before numbering.
BEGIN;
ALTER TABLE wallet_operations DROP CONSTRAINT wallet_operations_type_valid;
ALTER TABLE wallet_operations ADD CONSTRAINT wallet_operations_type_valid CHECK
 (operation_type IN('TASK_REWARD','REWARD_REDEMPTION','REDEMPTION_REFUND','AVATAR_PURCHASE','ADMIN_ADJUSTMENT','ADMIN_REDEMPTION_REFUND'));
ALTER TABLE wallet_operations DROP CONSTRAINT wallet_operations_actor_kind;
ALTER TABLE wallet_operations ADD CONSTRAINT wallet_operations_actor_kind CHECK
 ((operation_type IN('ADMIN_ADJUSTMENT','ADMIN_REDEMPTION_REFUND') AND actor_admin_id IS NOT NULL AND actor_user_id IS NULL)
 OR (operation_type NOT IN('ADMIN_ADJUSTMENT','ADMIN_REDEMPTION_REFUND') AND actor_admin_id IS NULL AND actor_user_id IS NOT NULL));
CREATE OR REPLACE FUNCTION usapp_wallet_member_actor_required() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF NEW.operation_type IS NULL OR (NEW.operation_type NOT IN('ADMIN_ADJUSTMENT','ADMIN_REDEMPTION_REFUND') AND NEW.actor_user_id IS NULL) THEN
  IF NEW.actor_user_id IS NULL THEN
   RAISE EXCEPTION USING ERRCODE='23502',MESSAGE='member wallet operation requires actor_user_id',SCHEMA='public',TABLE='wallet_operations',COLUMN='actor_user_id';
  END IF;
 END IF;
 RETURN NEW;
END $$;

CREATE TABLE admin_redemption_cancellations (
 id uuid PRIMARY KEY,
 admin_id uuid NOT NULL REFERENCES admin_users(id) ON DELETE RESTRICT,
 couple_id uuid NOT NULL,
 redemption_id uuid NOT NULL UNIQUE,
 redeemer_id uuid NOT NULL,
 expected_status text NOT NULL CHECK(expected_status IN('pending_confirmation','pending_fulfillment')),
 price_flowers integer NOT NULL CHECK(price_flowers BETWEEN 1 AND 999),
 refunded_flowers integer NOT NULL CHECK(refunded_flowers=CASE WHEN expected_status='pending_fulfillment' THEN price_flowers ELSE 0 END),
 refund_operation_id uuid UNIQUE,
 refund_seq bigint,
 balance_before bigint,
 balance_after bigint,
 reason text NOT NULL CHECK(char_length(reason) BETWEEN 5 AND 500 AND reason=btrim(reason) AND reason !~ '[\x01-\x1f\x7f-\x9f]'),
 idempotency_key text NOT NULL CHECK(idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
 request_fingerprint text NOT NULL CHECK(request_fingerprint ~ '^[a-f0-9]{64}$'),
 audit_id bigint NOT NULL UNIQUE,
 created_at timestamptz NOT NULL,
 UNIQUE(admin_id,idempotency_key),
 FOREIGN KEY(redemption_id,couple_id) REFERENCES reward_redemptions(id,couple_id) ON DELETE RESTRICT,
 FOREIGN KEY(couple_id,redeemer_id) REFERENCES couple_members(couple_id,user_id) ON DELETE RESTRICT,
 FOREIGN KEY(refund_operation_id,couple_id) REFERENCES wallet_operations(id,couple_id) ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED,
 CHECK((refunded_flowers=0 AND refund_operation_id IS NULL AND refund_seq IS NULL AND balance_before IS NULL AND balance_after IS NULL)
    OR (refunded_flowers>0 AND refund_operation_id IS NOT NULL AND refund_seq>0 AND balance_before>=0
       AND balance_after=balance_before+refunded_flowers AND balance_after<=1000000
       AND refund_seq IS NOT NULL AND balance_before IS NOT NULL AND balance_after IS NOT NULL))
);
CREATE INDEX admin_redemption_cancellations_space_idx ON admin_redemption_cancellations(couple_id,created_at DESC,id DESC);
CREATE TRIGGER admin_redemption_cancellations_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON admin_redemption_cancellations
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_redemption_reject_mutation();

-- Same old columns/types, plus an independently authenticated terminal admin fact. Legacy member events stay untouched.
CREATE OR REPLACE VIEW reward_redemption_state WITH(security_invoker=true) AS
SELECT d.id,d.couple_id,d.product_id,d.product_version,d.creator_id,d.redeemer_id,
 d.title,d.description,d.price_flowers,d.redemption_mode,COALESCE(d.operation_id,c.operation_id) AS operation_id,
 d.idempotency_key,d.request_fingerprint,d.created_at,
 CASE WHEN f.redemption_id IS NOT NULL THEN 'fulfilled' WHEN ac.id IS NOT NULL OR x.id IS NOT NULL THEN 'cancelled'
  WHEN d.operation_id IS NOT NULL OR c.id IS NOT NULL THEN 'pending_fulfillment' ELSE 'pending_confirmation' END AS status,
 f.fulfilled_at,f.fulfilled_by,
 CASE WHEN ac.id IS NOT NULL THEN 'admin' ELSE CASE x.action WHEN 'reject' THEN 'rejected' WHEN 'withdraw' THEN 'withdrawn' WHEN 'cancel_approve' THEN 'mutual' END END AS cancel_reason,
 COALESCE(ac.created_at,x.created_at) AS cancelled_at,
 CASE WHEN f.redemption_id IS NULL AND x.id IS NULL AND ac.id IS NULL THEN p.id END AS cancellation_id,
 CASE WHEN f.redemption_id IS NULL AND x.id IS NULL AND ac.id IS NULL THEN p.actor_id END AS cancellation_by
FROM reward_redemptions d
LEFT JOIN reward_redemption_fulfillments f ON f.redemption_id=d.id
LEFT JOIN reward_redemption_events c ON c.redemption_id=d.id AND c.action='confirm'
LEFT JOIN reward_redemption_events x ON x.redemption_id=d.id AND x.action IN('reject','withdraw','cancel_approve')
LEFT JOIN admin_redemption_cancellations ac ON ac.redemption_id=d.id AND ac.couple_id=d.couple_id
LEFT JOIN LATERAL(SELECT e.id,e.actor_id FROM reward_redemption_events e WHERE e.redemption_id=d.id AND e.action='cancel_propose'
 AND NOT EXISTS(SELECT 1 FROM reward_redemption_events z WHERE z.proposal_id=e.id) ORDER BY e.created_at DESC,e.id DESC LIMIT 1) p ON true;

CREATE FUNCTION usapp_admin_cancel_fingerprint(p_space uuid,p_redemption uuid,p_status text,p_reason text) RETURNS text
LANGUAGE sql IMMUTABLE STRICT AS $$
 SELECT encode(sha256(convert_to('admin-redemption-cancel:v1:'||p_space::text||':'||p_redemption::text||':'||p_status||':'||p_reason,'UTF8')),'hex')
$$;
CREATE FUNCTION usapp_admin_cancel_snapshot() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE r reward_redemption_state%ROWTYPE; causal timestamptz;
BEGIN
 PERFORM pg_advisory_xact_lock(33008,hashtext(NEW.redemption_id::text));
 SELECT * INTO r FROM reward_redemption_state WHERE id=NEW.redemption_id AND couple_id=NEW.couple_id;
 IF NOT FOUND OR r.status NOT IN('pending_confirmation','pending_fulfillment') OR r.status IS DISTINCT FROM NEW.expected_status
    OR ROW(r.redeemer_id,r.price_flowers) IS DISTINCT FROM ROW(NEW.redeemer_id,NEW.price_flowers)
    OR NOT EXISTS(SELECT 1 FROM admin_users WHERE id=NEW.admin_id AND status='active')
    OR NEW.request_fingerprint IS DISTINCT FROM usapp_admin_cancel_fingerprint(NEW.couple_id,NEW.redemption_id,NEW.expected_status,NEW.reason)
 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_SNAPSHOT'; END IF;
 causal:=GREATEST(r.created_at,(SELECT max(created_at) FROM reward_redemption_events WHERE redemption_id=r.id));
 IF NEW.created_at IS NULL THEN NEW.created_at:=GREATEST(clock_timestamp(),causal);
 ELSIF NEW.created_at<causal OR NEW.created_at>clock_timestamp() THEN
  RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_TIME';
 END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER admin_redemption_cancellations_snapshot BEFORE INSERT ON admin_redemption_cancellations
 FOR EACH ROW EXECUTE FUNCTION usapp_admin_cancel_snapshot();

CREATE FUNCTION usapp_admin_cancel_valid(p_id uuid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a admin_redemption_cancellations%ROWTYPE; d reward_redemptions%ROWTYPE; o wallet_operations%ROWTYPE;
 e wallet_ledger_entries%ROWTYPE; paid uuid; aud audit_logs%ROWTYPE; before_balance bigint;
BEGIN
 SELECT * INTO a FROM admin_redemption_cancellations WHERE id=p_id;
 IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_PARENT'; END IF;
 SELECT * INTO d FROM reward_redemptions WHERE id=a.redemption_id AND couple_id=a.couple_id;
 paid:=COALESCE(d.operation_id,(SELECT operation_id FROM reward_redemption_events WHERE redemption_id=d.id AND action='confirm'));
 IF ROW(d.redeemer_id,d.price_flowers) IS DISTINCT FROM ROW(a.redeemer_id,a.price_flowers)
  OR (paid IS NOT NULL) IS DISTINCT FROM (a.refunded_flowers>0)
  OR EXISTS(SELECT 1 FROM reward_redemption_fulfillments WHERE redemption_id=d.id)
  OR EXISTS(SELECT 1 FROM reward_redemption_events WHERE redemption_id=d.id AND action IN('reject','withdraw','cancel_approve'))
 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_ORIGINAL_STATE'; END IF;
 SELECT * INTO aud FROM audit_logs WHERE id=a.audit_id;
 IF NOT FOUND OR ROW(aud.actor_type,aud.actor_id,aud.couple_id,aud.action,aud.target_type,aud.target_id)
   IS DISTINCT FROM ROW('admin'::text,a.admin_id,a.couple_id,'admin.redemption.cancelled'::text,'admin_redemption_cancellation'::text,a.id::text)
   OR aud.detail IS DISTINCT FROM jsonb_build_object('redemption_id',a.redemption_id,'previous_status',a.expected_status,'refunded_flowers',a.refunded_flowers::text,'reason',a.reason)
 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_AUDIT'; END IF;
 IF (SELECT count(*) FROM wallet_operations WHERE operation_type='ADMIN_REDEMPTION_REFUND' AND source_type='admin_redemption_cancel' AND source_id=a.id::text)
   <>(CASE WHEN a.refunded_flowers>0 THEN 1 ELSE 0 END) THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_OPERATION_COUNT'; END IF;
 IF a.refunded_flowers=0 THEN RETURN; END IF;
 SELECT * INTO o FROM wallet_operations WHERE id=a.refund_operation_id;
 IF NOT FOUND OR ROW(o.couple_id,o.actor_user_id,o.actor_admin_id,o.operation_type,o.source_type,o.source_id,o.idempotency_key,o.request_fingerprint)
   IS DISTINCT FROM ROW(a.couple_id,NULL::uuid,a.admin_id,'ADMIN_REDEMPTION_REFUND'::text,'admin_redemption_cancel'::text,a.id::text,NULL::text,NULL::text)
 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_OPERATION'; END IF;
 IF (SELECT count(*) FROM wallet_ledger_entries WHERE operation_id=o.id)<>1 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_ENTRY_COUNT'; END IF;
 SELECT * INTO e FROM wallet_ledger_entries WHERE operation_id=o.id;
 IF ROW(e.couple_id,e.user_id,e.asset_type,e.seq,e.amount,e.balance_after)
   IS DISTINCT FROM ROW(a.couple_id,a.redeemer_id,'flower'::text,a.refund_seq,a.refunded_flowers::bigint,a.balance_after)
 THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_ENTRY'; END IF;
 before_balance:=COALESCE((SELECT balance_after FROM wallet_ledger_entries WHERE user_id=a.redeemer_id AND asset_type='flower' AND seq=a.refund_seq-1),0);
 IF before_balance IS DISTINCT FROM a.balance_before THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_BALANCE'; END IF;
END $$;
CREATE FUNCTION usapp_admin_cancel_link() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE o wallet_operations%ROWTYPE; parent_id uuid;
BEGIN
 IF TG_TABLE_NAME='admin_redemption_cancellations' THEN parent_id:=NEW.id;
 ELSE
  IF TG_TABLE_NAME='wallet_operations' THEN o:=NEW; ELSE SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id; END IF;
  IF o.operation_type IS DISTINCT FROM 'ADMIN_REDEMPTION_REFUND' THEN RETURN NULL; END IF;
  IF o.source_type<>'admin_redemption_cancel' OR o.source_id !~ '^[a-f0-9]{8}(-[a-f0-9]{4}){3}-[a-f0-9]{12}$'
  THEN RAISE EXCEPTION USING ERRCODE='UV201',MESSAGE='ADMIN_CANCEL_SOURCE'; END IF;
  parent_id:=o.source_id::uuid;
 END IF;
 PERFORM usapp_admin_cancel_valid(parent_id);RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER admin_redemption_cancellations_valid AFTER INSERT ON admin_redemption_cancellations
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_admin_cancel_link();
CREATE CONSTRAINT TRIGGER wallet_operations_admin_cancel_valid AFTER INSERT ON wallet_operations
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN(NEW.operation_type='ADMIN_REDEMPTION_REFUND') EXECUTE FUNCTION usapp_admin_cancel_link();
CREATE CONSTRAINT TRIGGER wallet_entries_admin_cancel_valid AFTER INSERT ON wallet_ledger_entries
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_admin_cancel_link();

CREATE OR REPLACE FUNCTION usapp_admin_adjustment_audit_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF TG_OP='TRUNCATE' THEN
  IF EXISTS(SELECT 1 FROM audit_logs WHERE action IN('admin.wallet.adjusted','admin.redemption.cancelled')) THEN
   RAISE EXCEPTION USING ERRCODE='UV102',MESSAGE='ADMIN_FINANCIAL_AUDIT_IMMUTABLE'; END IF;
 ELSIF OLD.action IN('admin.wallet.adjusted','admin.redemption.cancelled') THEN
  RAISE EXCEPTION USING ERRCODE='UV102',MESSAGE='ADMIN_FINANCIAL_AUDIT_IMMUTABLE';
 END IF;
 IF TG_OP='DELETE' THEN RETURN OLD; ELSIF TG_OP='UPDATE' THEN RETURN NEW; END IF;RETURN NULL;
END $$;

CREATE FUNCTION usapp_admin_redemption_cancel(p_admin uuid,p_space uuid,p_redemption uuid,p_status text,p_reason text,p_key text)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE a admin_redemption_cancellations%ROWTYPE; r reward_redemption_state%ROWTYPE; fp text;
 ident uuid:=gen_random_uuid(); op uuid; aid bigint; amount integer:=0; before_balance bigint; after_balance bigint; last_seq bigint; target_seq bigint;
BEGIN
 IF NOT EXISTS(SELECT 1 FROM admin_users WHERE id=p_admin AND status='active' FOR SHARE) THEN RETURN jsonb_build_object('error','admin_forbidden'); END IF;
 IF p_space IS NULL OR p_redemption IS NULL OR p_status IS NULL OR p_status NOT IN('pending_confirmation','pending_fulfillment')
   OR p_reason IS NULL OR char_length(p_reason) NOT BETWEEN 5 AND 500 OR p_reason<>btrim(p_reason) OR p_reason ~ '[\x01-\x1f\x7f-\x9f]'
   OR p_key IS NULL OR p_key !~ '^[A-Za-z0-9_-]{16,128}$' THEN RETURN jsonb_build_object('error','validation'); END IF;
 fp:=usapp_admin_cancel_fingerprint(p_space,p_redemption,p_status,p_reason);
 PERFORM pg_advisory_xact_lock(33033,hashtext(p_admin::text||':'||p_key));
 SELECT * INTO a FROM admin_redemption_cancellations WHERE admin_id=p_admin AND idempotency_key=p_key;
 IF FOUND THEN
  IF a.request_fingerprint<>fp THEN RETURN jsonb_build_object('error','key_reused'); END IF;
 ELSE
  BEGIN
   PERFORM pg_advisory_xact_lock(33008,hashtext(p_redemption::text));
   SELECT * INTO r FROM reward_redemption_state WHERE id=p_redemption AND couple_id=p_space;
   IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV203',MESSAGE='not_found'; END IF;
   IF r.status IS DISTINCT FROM p_status OR r.status NOT IN('pending_confirmation','pending_fulfillment') THEN RAISE EXCEPTION USING ERRCODE='UV203',MESSAGE='conflict'; END IF;
   IF r.status='pending_fulfillment' THEN
    PERFORM 1 FROM wallet_accounts WHERE user_id=r.redeemer_id AND couple_id=r.couple_id AND asset_type='flower' FOR UPDATE;
    IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV203',MESSAGE='account_unavailable'; END IF;
    SELECT seq,balance_after INTO last_seq,before_balance FROM wallet_ledger_entries WHERE user_id=r.redeemer_id AND asset_type='flower' ORDER BY seq DESC LIMIT 1;
    IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UV203',MESSAGE='account_unavailable'; END IF;
    amount:=r.price_flowers;after_balance:=before_balance+amount;target_seq:=last_seq+1;op:=gen_random_uuid();
    IF after_balance>1000000 THEN RAISE EXCEPTION USING ERRCODE='UV203',MESSAGE='balance_bound'; END IF;
   END IF;
   INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
    VALUES('admin',p_admin,p_space,'admin.redemption.cancelled','admin_redemption_cancellation',ident::text,
     jsonb_build_object('redemption_id',p_redemption,'previous_status',p_status,'refunded_flowers',amount::text,'reason',p_reason)) RETURNING id INTO aid;
   INSERT INTO admin_redemption_cancellations(id,admin_id,couple_id,redemption_id,redeemer_id,expected_status,price_flowers,refunded_flowers,refund_operation_id,refund_seq,balance_before,balance_after,reason,idempotency_key,request_fingerprint,audit_id)
    VALUES(ident,p_admin,p_space,p_redemption,r.redeemer_id,p_status,r.price_flowers,amount,op,target_seq,before_balance,after_balance,p_reason,p_key,fp,aid);
   IF amount>0 THEN
    INSERT INTO wallet_operations(id,couple_id,actor_admin_id,operation_type,source_type,source_id)
     VALUES(op,p_space,p_admin,'ADMIN_REDEMPTION_REFUND','admin_redemption_cancel',ident::text);
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
     VALUES(op,p_space,r.redeemer_id,'flower',target_seq,amount,after_balance);
   END IF;
   SET CONSTRAINTS admin_redemption_cancellations_valid,wallet_operations_admin_cancel_valid,wallet_entries_admin_cancel_valid,wallet_operations_require_entries IMMEDIATE;
   SET CONSTRAINTS admin_redemption_cancellations_valid,wallet_operations_admin_cancel_valid,wallet_entries_admin_cancel_valid,wallet_operations_require_entries DEFERRED;
   SELECT * INTO a FROM admin_redemption_cancellations WHERE id=ident;
  EXCEPTION WHEN SQLSTATE 'UV203' THEN RETURN jsonb_build_object('error',SQLERRM);
  END;
 END IF;
 RETURN jsonb_build_object('cancellation',jsonb_build_object('id',a.id,'admin_id',a.admin_id,'couple_id',a.couple_id,'redemption_id',a.redemption_id,
  'redeemer_id',a.redeemer_id,'previous_status',a.expected_status,'refunded_flowers',a.refunded_flowers::text,'refund_operation_id',a.refund_operation_id,
  'refund_seq',a.refund_seq::text,'balance_before',a.balance_before::text,'balance_after',a.balance_after::text,'reason',a.reason,
  'created_at',to_char(a.created_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.US"Z"')));
END $$;

REVOKE ALL ON admin_redemption_cancellations FROM PUBLIC,usapp_api,usapp_push;
GRANT SELECT,INSERT ON admin_redemption_cancellations TO usapp_api;
REVOKE ALL ON FUNCTION usapp_admin_redemption_cancel(uuid,uuid,uuid,text,text,text),usapp_admin_cancel_fingerprint(uuid,uuid,text,text),usapp_admin_cancel_snapshot(),usapp_admin_cancel_valid(uuid),usapp_admin_cancel_link() FROM PUBLIC,usapp_push;
GRANT EXECUTE ON FUNCTION usapp_admin_redemption_cancel(uuid,uuid,uuid,text,text,text),usapp_admin_cancel_fingerprint(uuid,uuid,text,text),usapp_admin_cancel_valid(uuid) TO usapp_api;
COMMIT;
