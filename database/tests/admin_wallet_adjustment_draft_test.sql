-- Included inside the draft's BEGIN; the harness supplies ROLLBACK. No shared DB.
DO $test$
DECLARE ua uuid:=gen_random_uuid(); ub uuid:=gen_random_uuid(); space uuid:=gen_random_uuid(); adm uuid:=gen_random_uuid();
 tag text:=substr(md5(random()::text),1,10); k text:='adjust-key-'||md5(random()::text); r jsonb; first_id uuid;
 n bigint; before_n bigint; err text; mode text; fid uuid; fop uuid; extra uuid; ad bigint;
BEGIN
 IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'test database required'; END IF;
 IF has_function_privilege('usapp_push','usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text)','EXECUTE')
   OR has_function_privilege('public','usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text)','EXECUTE')
   OR NOT has_function_privilege('usapp_api','usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text)','EXECUTE')
 THEN RAISE EXCEPTION 'command execution grant'; END IF;
 INSERT INTO users(id,email_canonical,email_display) VALUES(ua,'adj_a_'||tag||'@example.com','adj_a_'||tag||'@example.com'),(ub,'adj_b_'||tag||'@example.com','adj_b_'||tag||'@example.com');
 INSERT INTO couples(id,name,created_by_user_id) VALUES(space,'Adjustment fixture',ua);
 INSERT INTO couple_members(user_id,couple_id,slot) VALUES(ua,space,1),(ub,space,2);
 INSERT INTO admin_users(id,username,password_hash) VALUES(adm,'adj_'||tag,'fixture-only-hash');
 r:=usapp_admin_wallet_adjust(adm,ua,'flower',9999,0,'Verified fixture correction',k);
 IF r ? 'error' OR r#>>'{adjustment,balance_after}' IS DISTINCT FROM '9999' OR r#>>'{adjustment,operation_count}' IS DISTINCT FROM '11'
  THEN RAISE EXCEPTION 'flower credit failed: %',r; END IF;
 first_id:=(r#>>'{adjustment,id}')::uuid;
 PERFORM usapp_admin_adjustment_valid(first_id);
 SELECT count(*) INTO before_n FROM wallet_operations;
 IF usapp_admin_wallet_adjust(adm,ua,'flower',9999,0,'Verified fixture correction',k) IS DISTINCT FROM r
  OR (SELECT count(*) FROM wallet_operations)<>before_n THEN RAISE EXCEPTION 'replay changed'; END IF;
 IF usapp_admin_wallet_adjust(adm,ua,'flower',9998,0,'Verified fixture correction',k)->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'key reuse'; END IF;
 r:=usapp_admin_wallet_adjust(adm,ua,'flower',-9999,11,'Verified fixture correction',k||'b');
 IF r#>>'{adjustment,balance_after}' IS DISTINCT FROM '0' OR r#>>'{adjustment,last_seq}' IS DISTINCT FROM '22' THEN RAISE EXCEPTION 'flower debit'; END IF;
 r:=usapp_admin_wallet_adjust(adm,ua,'flower',999,22,'Verified fixture correction',k||'c');
 IF r#>>'{adjustment,operation_count}' IS DISTINCT FROM '1' OR r#>>'{adjustment,last_seq}' IS DISTINCT FROM '23' THEN RAISE EXCEPTION 'flower exact chunk'; END IF;
 SELECT count(*) INTO before_n FROM wallet_operations;
 IF usapp_admin_wallet_adjust(adm,ua,'flower',-1000,23,'Verified fixture correction',k||'d')->>'error' IS DISTINCT FROM 'balance_bound'
 OR usapp_admin_wallet_adjust(adm,ua,'flower',1,22,'Verified fixture correction',k||'e')->>'error' IS DISTINCT FROM 'conflict'
 OR usapp_admin_wallet_adjust(adm,ua,'flower',0,23,'Verified fixture correction',k||'f')->>'error' IS DISTINCT FROM 'validation'
 OR usapp_admin_wallet_adjust(adm,ua,'flower',10000,23,'Verified fixture correction',k||'g')->>'error' IS DISTINCT FROM 'validation'
 OR usapp_admin_wallet_adjust(NULL,ua,'flower',1,23,'Verified fixture correction',k||'h')->>'error' IS DISTINCT FROM 'admin_forbidden'
 OR usapp_admin_wallet_adjust(adm,gen_random_uuid(),'flower',1,0,'Verified fixture correction',k||'i')->>'error' IS DISTINCT FROM 'target_unavailable'
 THEN RAISE EXCEPTION 'refusal classification'; END IF;
 IF (SELECT count(*) FROM wallet_operations)<>before_n THEN RAISE EXCEPTION 'refusal wrote'; END IF;
 r:=usapp_admin_wallet_adjust(adm,ua,'sunshine',9999,0,'Verified fixture correction',k||'j');
 IF r#>>'{adjustment,balance_after}' IS DISTINCT FROM '9999' OR r#>>'{adjustment,operation_count}' IS DISTINCT FROM '1' THEN RAISE EXCEPTION 'sunshine credit'; END IF;
 r:=usapp_admin_wallet_adjust(adm,ua,'sunshine',-9999,1,'Verified fixture correction',k||'k');
 IF r#>>'{adjustment,balance_after}' IS DISTINCT FROM '0' OR r#>>'{adjustment,last_seq}' IS DISTINCT FROM '2' THEN RAISE EXCEPTION 'sunshine debit'; END IF;
 -- Any subtransaction failure rolls back the entire multi-operation correction and audit.
 SELECT count(*) INTO before_n FROM admin_wallet_adjustments;
 BEGIN
  PERFORM usapp_admin_wallet_adjust(adm,ub,'flower',9999,0,'Verified fixture correction',k||'l');
  RAISE EXCEPTION USING ERRCODE='UV199',MESSAGE='test rollback';
 EXCEPTION WHEN SQLSTATE 'UV199' THEN NULL; END;
 IF (SELECT count(*) FROM admin_wallet_adjustments)<>before_n OR EXISTS(SELECT 1 FROM wallet_accounts WHERE user_id=ub)
  THEN RAISE EXCEPTION 'partial correction escaped rollback'; END IF;
 err:=NULL;
 BEGIN UPDATE admin_wallet_adjustments SET reason=reason WHERE id=first_id;
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err NOT IN('42501','UW001') OR err IS NULL THEN RAISE EXCEPTION 'mutable parent'; END IF;
 err:=NULL;
 BEGIN UPDATE audit_logs SET detail='{}' WHERE id=(SELECT audit_id FROM admin_wallet_adjustments WHERE id=first_id);
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM 'UV102' THEN RAISE EXCEPTION 'mutable audit: %',err; END IF;
 err:=NULL;
 BEGIN
  INSERT INTO wallet_operations(couple_id,actor_user_id,actor_admin_id,operation_type,source_type,source_id)
   VALUES(space,ua,adm,'ADMIN_ADJUSTMENT','test','test');
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM '23514' THEN RAISE EXCEPTION 'mixed actor: %',err; END IF;
 err:=NULL;
 BEGIN
  INSERT INTO wallet_operations(couple_id,operation_type,source_type,source_id) VALUES(space,'TASK_REWARD','test','test');
 EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE; END;
 IF err IS DISTINCT FROM '23514' THEN RAISE EXCEPTION 'missing member actor: %',err; END IF;
 -- Construct complete-looking independent forged groups. The normal chain accepts their local math;
 -- the new cross-link guard must still reject disagreement with the reviewed correction.
 FOREACH mode IN ARRAY ARRAY['amount','asset','user','extra_entry','missing_entry','source','audit','fingerprint','extra_operation'] LOOP
  err:=NULL; fid:=gen_random_uuid(); fop:=gen_random_uuid(); extra:=gen_random_uuid();
  BEGIN
   INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
    VALUES('admin',adm,space,'admin.wallet.adjusted','admin_wallet_adjustment',fid::text,
     jsonb_build_object('user_id',ua,'asset','flower','delta','2','expected_seq','23','balance_before','999','balance_after','1001',
       'reason',CASE WHEN mode='audit' THEN 'Wrong audit reason' ELSE 'Forged correction fixture' END)) RETURNING id INTO ad;
   INSERT INTO admin_wallet_adjustments(id,admin_id,couple_id,user_id,asset,delta,expected_seq,balance_before,balance_after,operation_ids,reason,idempotency_key,request_fingerprint,audit_id)
    VALUES(fid,adm,space,ua,'flower',2,23,999,1001,ARRAY[fop],'Forged correction fixture',k||'-'||mode,
     CASE WHEN mode='fingerprint' THEN repeat('0',64) ELSE usapp_admin_adjustment_fingerprint(ua,'flower',2,23,'Forged correction fixture') END,ad);
   INSERT INTO wallet_operations(id,couple_id,actor_admin_id,operation_type,source_type,source_id)
    VALUES(fop,space,adm,'ADMIN_ADJUSTMENT','admin_wallet_adjustment',fid::text||CASE WHEN mode='source' THEN ':2' ELSE ':1' END);
   IF mode='user' OR mode='extra_entry' THEN
    INSERT INTO wallet_accounts(user_id,asset_type,couple_id) VALUES(ub,'flower',space);
   END IF;
   IF mode<>'missing_entry' THEN
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
     VALUES(fop,space,CASE WHEN mode='user' THEN ub ELSE ua END,CASE WHEN mode='asset' THEN 'sunshine' ELSE 'flower' END,
      CASE WHEN mode='user' THEN 1 WHEN mode='asset' THEN 3 ELSE 24 END,
      CASE WHEN mode='amount' THEN 3 ELSE 2 END,
      CASE WHEN mode IN('user','asset') THEN 2 WHEN mode='amount' THEN 1002 ELSE 1001 END);
   END IF;
   IF mode='extra_entry' THEN
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) VALUES(fop,space,ub,'flower',1,2,2);
   END IF;
   IF mode='extra_operation' THEN
    INSERT INTO wallet_operations(id,couple_id,actor_admin_id,operation_type,source_type,source_id) VALUES(extra,space,adm,'ADMIN_ADJUSTMENT','admin_wallet_adjustment',fid::text||':2');
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after) VALUES(extra,space,ua,'flower',25,1,1002);
   END IF;
   SET CONSTRAINTS admin_wallet_adjustments_valid,wallet_operations_admin_adjustment_valid,wallet_entries_admin_adjustment_valid IMMEDIATE;
  EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS err=RETURNED_SQLSTATE;
  END;
  IF err IS DISTINCT FROM 'UV101' THEN RAISE EXCEPTION 'forgery accepted/wrong refusal mode=% state=%',mode,err; END IF;
 END LOOP;
 IF (SELECT count(*) FROM admin_wallet_adjustments)<>before_n THEN RAISE EXCEPTION 'forgery escaped rollback'; END IF;
 FOR first_id IN SELECT id FROM admin_wallet_adjustments LOOP PERFORM usapp_admin_adjustment_valid(first_id); END LOOP;
 RAISE NOTICE 'ADMIN_ADJUSTMENT_DRAFT_PASSED role=% parents=%',current_user,(SELECT count(*) FROM admin_wallet_adjustments);
END $test$;
