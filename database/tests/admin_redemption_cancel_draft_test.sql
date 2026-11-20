-- Rollback-only draft proof; appended inside the draft transaction by the owned preflight.
DO $test$
DECLARE a uuid:=gen_random_uuid(); b uuid:=gen_random_uuid(); s uuid:=gen_random_uuid(); adm uuid:=gen_random_uuid();
 p uuid:=gen_random_uuid(); d uuid:=gen_random_uuid(); pending uuid:=gen_random_uuid(); fulfilled uuid:=gen_random_uuid();
 op uuid:=gen_random_uuid(); debit uuid; proposal uuid:=gen_random_uuid();
 tag text:=md5(random()::text); fp text:=repeat('a',64); r jsonb; original jsonb; n bigint; err text;
 mode text; parent uuid; rop uuid; aid bigint;
BEGIN
 IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'disposable database required'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES(a,'ac_a_'||tag||'@example.com','ac_a_'||tag||'@example.com'),(b,'ac_b_'||tag||'@example.com','ac_b_'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES(s,'Admin cancellation fixture',a);
 INSERT INTO couple_members(user_id,couple_id,slot) VALUES(a,s,1),(b,s,2);
 INSERT INTO admin_users(id,username,password_hash) VALUES(adm,'ac_'||tag,'fixture-only-hash');
 INSERT INTO reward_products(id,couple_id,creator_id,owner_id) VALUES(p,s,a,b);
 INSERT INTO reward_product_revisions(product_id,couple_id,version,actor_id,action,title,price_flowers,redemption_mode,status,idempotency_key,request_fingerprint)
  VALUES(p,s,1,a,'create','Admin cancellation',3,'request','active','product_'||tag,fp);
 INSERT INTO wallet_accounts(user_id,couple_id,asset_type) VALUES(b,s,'flower');
 INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) VALUES(op,s,b,'TASK_REWARD','wallet_test',op::text);
 INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) VALUES(op,s,b,'flower',1,10,10);
 INSERT INTO reward_redemptions(id,couple_id,product_id,product_version,creator_id,redeemer_id,title,price_flowers,redemption_mode,idempotency_key,request_fingerprint)
  VALUES(d,s,p,1,a,b,'Admin cancellation',3,'request','redemption_'||tag,fp),
        (pending,s,p,1,a,b,'Admin cancellation',3,'request','pending_'||tag,fp),
        (fulfilled,s,p,1,a,b,'Admin cancellation',3,'request','fulfilled_'||tag,fp);
 debit:=gen_random_uuid();
 INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) VALUES(debit,s,a,'REWARD_REDEMPTION','reward_redemption',d::text);
 INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,operation_id,idempotency_key,request_fingerprint) VALUES(d,s,a,'confirm',debit,'confirm_'||tag,fp);
 INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) VALUES(debit,s,b,'flower',2,-3,7);
 INSERT INTO reward_redemption_events(id,redemption_id,couple_id,actor_id,action,idempotency_key,request_fingerprint) VALUES(proposal,d,s,a,'cancel_propose','propose_'||tag,fp);
 SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED;
 SELECT count(*) INTO n FROM wallet_operations;
 IF usapp_admin_redemption_cancel(adm,s,d,'pending_confirmation','Reviewed cancellation','key_'||tag)->>'error' IS DISTINCT FROM 'conflict'
  OR usapp_admin_redemption_cancel(adm,s,gen_random_uuid(),'pending_confirmation','Reviewed cancellation','key_'||tag)->>'error' IS DISTINCT FROM 'not_found'
  OR usapp_admin_redemption_cancel(NULL,s,d,'pending_fulfillment','Reviewed cancellation','key_'||tag)->>'error' IS DISTINCT FROM 'admin_forbidden'
 THEN RAISE EXCEPTION 'refusal classification'; END IF;
 IF (SELECT count(*) FROM wallet_operations)<>n THEN RAISE EXCEPTION 'refusal posted'; END IF;
 r:=usapp_admin_redemption_cancel(adm,s,d,'pending_fulfillment','Reviewed cancellation','key_'||tag);
 IF r ? 'error' OR r#>>'{cancellation,refunded_flowers}' IS DISTINCT FROM '3'
  OR r#>>'{cancellation,balance_after}' IS DISTINCT FROM '10' THEN RAISE EXCEPTION 'paid cancellation: %',r; END IF;
 original:=r;
 IF (SELECT status FROM reward_redemption_state WHERE id=d)<>'cancelled'
  OR (SELECT cancel_reason FROM reward_redemption_state WHERE id=d)<>'admin'
  OR (SELECT cancellation_id FROM reward_redemption_state WHERE id=d) IS NOT NULL
  OR NOT EXISTS(SELECT 1 FROM reward_redemption_events WHERE id=proposal)
 THEN RAISE EXCEPTION 'terminal overlay or retained history'; END IF;
 SELECT count(*) INTO n FROM wallet_operations;
 IF usapp_admin_redemption_cancel(adm,s,d,'pending_fulfillment','Reviewed cancellation','key_'||tag) IS DISTINCT FROM original
  OR (SELECT count(*) FROM wallet_operations)<>n THEN RAISE EXCEPTION 'replay changed'; END IF;
 IF usapp_admin_redemption_cancel(adm,s,d,'pending_fulfillment','Changed cancellation','key_'||tag)->>'error' IS DISTINCT FROM 'key_reused'
  OR usapp_admin_redemption_cancel(adm,s,d,'pending_fulfillment','Reviewed cancellation','key2_'||tag)->>'error' IS DISTINCT FROM 'conflict'
 THEN RAISE EXCEPTION 'duplicate classification'; END IF;
 r:=usapp_admin_redemption_cancel(adm,s,pending,'pending_confirmation','Reviewed unpaid request','unpaid_'||tag);
 IF r ? 'error' OR r#>>'{cancellation,refunded_flowers}' IS DISTINCT FROM '0'
  OR r#>'{cancellation,refund_operation_id}' IS DISTINCT FROM 'null'::jsonb
  OR (SELECT count(*) FROM wallet_operations)<>n THEN RAISE EXCEPTION 'unpaid request posted: %',r; END IF;
 err:=NULL;
 BEGIN INSERT INTO reward_redemption_fulfillments(redemption_id,couple_id,fulfilled_by,idempotency_key,request_fingerprint) VALUES(d,s,a,'fulfill_'||tag,fp);
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM 'UM004' THEN RAISE EXCEPTION 'cancelled fulfillment accepted: %',err; END IF;
 err:=NULL;
 BEGIN INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,idempotency_key,request_fingerprint) VALUES(pending,s,a,'reject','reject_'||tag,fp);
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM 'UN001' THEN RAISE EXCEPTION 'cancelled member event accepted: %',err; END IF;
 err:=NULL;
 BEGIN UPDATE audit_logs SET detail='{}' WHERE id=(SELECT audit_id FROM admin_redemption_cancellations WHERE redemption_id=d);
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM 'UV102' THEN RAISE EXCEPTION 'mutable audit: %',err; END IF;
 -- A second paid request supplies an independently chargeable fixture for forged refund probes.
 debit:=gen_random_uuid();
 INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id) VALUES(debit,s,a,'REWARD_REDEMPTION','reward_redemption',fulfilled::text);
 INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,operation_id,idempotency_key,request_fingerprint) VALUES(fulfilled,s,a,'confirm',debit,'confirm2_'||tag,fp);
 INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) VALUES(debit,s,b,'flower',4,-3,7);
 SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED;
 FOREACH mode IN ARRAY ARRAY['audit','missing_entry','amount','source','actor','orphan'] LOOP
  err:=NULL;
  BEGIN
   parent:=gen_random_uuid();rop:=gen_random_uuid();
   IF mode<>'orphan' THEN
    INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
     VALUES('admin',adm,s,'admin.redemption.cancelled','admin_redemption_cancellation',parent::text,
       CASE WHEN mode='audit' THEN '{}'::jsonb ELSE jsonb_build_object('redemption_id',fulfilled,'previous_status','pending_fulfillment','refunded_flowers','3','reason','Forged cancellation probe') END) RETURNING id INTO aid;
    INSERT INTO admin_redemption_cancellations(id,admin_id,couple_id,redemption_id,redeemer_id,expected_status,price_flowers,refunded_flowers,refund_operation_id,refund_seq,balance_before,balance_after,reason,idempotency_key,request_fingerprint,audit_id)
     VALUES(parent,adm,s,fulfilled,b,'pending_fulfillment',3,3,rop,5,7,10,'Forged cancellation probe','forge_'||mode||tag,
       usapp_admin_cancel_fingerprint(s,fulfilled,'pending_fulfillment','Forged cancellation probe'),aid);
   END IF;
   INSERT INTO wallet_operations(id,couple_id,actor_admin_id,operation_type,source_type,source_id)
    VALUES(rop,s,CASE WHEN mode='actor' THEN gen_random_uuid() ELSE adm END,'ADMIN_REDEMPTION_REFUND',CASE WHEN mode='source' THEN 'forged' ELSE 'admin_redemption_cancel' END,parent::text);
   IF mode<>'missing_entry' THEN
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
     VALUES(rop,s,b,'flower',5,CASE WHEN mode='amount' THEN 2 ELSE 3 END,CASE WHEN mode='amount' THEN 9 ELSE 10 END);
   END IF;
   SET CONSTRAINTS ALL IMMEDIATE;
  EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
  IF err IS NULL OR err NOT IN('UV201','23503') THEN RAISE EXCEPTION 'forged % accepted/wrong refusal: %',mode,err; END IF;
  SET CONSTRAINTS ALL DEFERRED;
 END LOOP;
 INSERT INTO reward_redemption_fulfillments(redemption_id,couple_id,fulfilled_by,idempotency_key,request_fingerprint) VALUES(fulfilled,s,a,'fulfilled_'||tag,fp);
 IF usapp_admin_redemption_cancel(adm,s,fulfilled,'pending_fulfillment','Already fulfilled probe','closed_'||tag)->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'fulfilled cancellation'; END IF;
 SET CONSTRAINTS ALL IMMEDIATE;
 RAISE NOTICE 'ADMIN_REDEMPTION_CANCEL_DRAFT_PASSED role=%',current_user;
END $test$;
