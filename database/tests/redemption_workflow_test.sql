-- M9 rollback-only guard, exact debit/refund and audit-atomicity checks.
\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
    a uuid:=gen_random_uuid(); b uuid:=gen_random_uuid(); c uuid:=gen_random_uuid();
    p uuid:=gen_random_uuid(); d uuid:=gen_random_uuid(); d2 uuid:=gen_random_uuid();
    op uuid:=gen_random_uuid(); debit uuid:=gen_random_uuid(); refund uuid;
    proposal uuid:=gen_random_uuid(); event uuid; tag text:=replace(gen_random_uuid()::text,'-','');
    fp text:=repeat('a',64); is_app boolean:=current_user='usapp_api';
BEGIN
    IF current_database()<>'usapp_test' THEN RAISE EXCEPTION 'usapp_test only'; END IF;
    IF NOT is_app AND NOT (SELECT rolsuper FROM pg_roles WHERE rolname=current_user) THEN
        RAISE EXCEPTION 'application or maintenance role only';
    END IF;
    INSERT INTO users(id,email_canonical,email_display) VALUES
        (a,'m9a_'||tag||'@m3-fixture.test','m9a_'||tag||'@m3-fixture.test'),
        (b,'m9b_'||tag||'@m3-fixture.test','m9b_'||tag||'@m3-fixture.test');
    INSERT INTO couples(id,name,created_by_user_id) VALUES(c,'M9 rollback fixture',a);
    INSERT INTO couple_members(user_id,couple_id,slot) VALUES(a,c,1),(b,c,2);
    INSERT INTO reward_products(id,couple_id,creator_id,owner_id) VALUES(p,c,a,b);
    INSERT INTO reward_product_revisions(product_id,couple_id,version,actor_id,action,title,price_flowers,
        redemption_mode,status,idempotency_key,request_fingerprint)
        VALUES(p,c,1,a,'create','M9 SQL',3,'request','active','product_'||tag,fp);
    INSERT INTO wallet_accounts(user_id,couple_id,asset_type) VALUES(b,c,'flower');
    INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
        VALUES(op,c,b,'TASK_REWARD','wallet_test',op::text);
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
        VALUES(op,c,b,'flower',1,10,10);
    INSERT INTO reward_redemptions(id,couple_id,product_id,product_version,creator_id,redeemer_id,
        title,price_flowers,redemption_mode,operation_id,idempotency_key,request_fingerprint)
        VALUES(d,c,p,1,a,b,'M9 SQL',3,'request',NULL,'request_'||tag,fp),
              (d2,c,p,1,a,b,'M9 SQL',3,'request',NULL,'request2_'||tag,fp);
    IF (SELECT status FROM reward_redemption_state WHERE id=d)<>'pending_confirmation' THEN
        RAISE EXCEPTION 'request state';
    END IF;
    -- A: request actor, state, fulfillment and operation backstops.
    BEGIN
        INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,idempotency_key,request_fingerprint)
            VALUES(d,c,b,'reject','wrong_'||tag,fp);
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='unexpected actor success';
    EXCEPTION WHEN SQLSTATE 'UN001' THEN NULL;
    END;
    BEGIN
        INSERT INTO reward_redemption_fulfillments(redemption_id,couple_id,fulfilled_by,idempotency_key,request_fingerprint)
            VALUES(d,c,a,'fulfill_'||tag,fp);
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='unpaid fulfilled';
    EXCEPTION WHEN SQLSTATE 'UM004' THEN NULL;
    END;
    -- B: incomplete debit cannot commit, even when the event is otherwise valid.
    BEGIN
        INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
            VALUES(debit,c,a,'REWARD_REDEMPTION','reward_redemption',d::text);
        INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,operation_id,idempotency_key,request_fingerprint)
            VALUES(d,c,a,'confirm',debit,'confirm_'||tag,fp);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='missing debit accepted';
    EXCEPTION WHEN SQLSTATE 'UM003' THEN NULL;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    -- C: exact debit, original snapshot; operation actor is creator, account is redeemer.
    INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
        VALUES(debit,c,a,'REWARD_REDEMPTION','reward_redemption',d::text);
    INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,operation_id,idempotency_key,request_fingerprint)
        VALUES(d,c,a,'confirm',debit,'confirm_'||tag,fp);
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
        VALUES(debit,c,b,'flower',2,-3,7);
    SET CONSTRAINTS ALL IMMEDIATE;
    SET CONSTRAINTS ALL DEFERRED;
    INSERT INTO reward_redemption_events(id,redemption_id,couple_id,actor_id,action,idempotency_key,request_fingerprint)
        VALUES(proposal,d,c,a,'cancel_propose','propose_'||tag,fp);
    -- D: same-party approval and stale proposal are rejected; partial refund rolls back.
    BEGIN
        refund:=gen_random_uuid();
        INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
            VALUES(refund,c,a,'REDEMPTION_REFUND','reward_redemption',d::text);
        INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,proposal_id,operation_id,idempotency_key,request_fingerprint)
            VALUES(d,c,a,'cancel_approve',proposal,refund,'self_'||tag,fp);
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='self approval accepted';
    EXCEPTION WHEN SQLSTATE 'UN001' THEN NULL;
    END;
    BEGIN
        refund:=gen_random_uuid();
        INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
            VALUES(refund,c,b,'REDEMPTION_REFUND','reward_redemption',d::text);
        INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,proposal_id,operation_id,idempotency_key,request_fingerprint)
            VALUES(d,c,b,'cancel_approve',proposal,refund,'partial_'||tag,fp);
        INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
            VALUES(refund,c,b,'flower',3,2,9);
        SET CONSTRAINTS ALL IMMEDIATE;
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='partial refund accepted';
    EXCEPTION WHEN SQLSTATE 'UM003' THEN NULL;
    END;
    SET CONSTRAINTS ALL DEFERRED;
    IF (SELECT cancellation_id FROM reward_redemption_state WHERE id=d) IS DISTINCT FROM proposal THEN
        RAISE EXCEPTION 'failed refund changed proposal';
    END IF;
    -- E: full refund once; subsequent fulfillment fails.
    refund:=gen_random_uuid();
    INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
        VALUES(refund,c,b,'REDEMPTION_REFUND','reward_redemption',d::text);
    INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,proposal_id,operation_id,idempotency_key,request_fingerprint)
        VALUES(d,c,b,'cancel_approve',proposal,refund,'refund_'||tag,fp);
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
        VALUES(refund,c,b,'flower',3,3,10);
    SET CONSTRAINTS ALL IMMEDIATE;
    SET CONSTRAINTS ALL DEFERRED;
    BEGIN
        INSERT INTO reward_redemption_fulfillments(redemption_id,couple_id,fulfilled_by,idempotency_key,request_fingerprint)
            VALUES(d,c,a,'closed_'||tag,fp);
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='cancelled fulfilled';
    EXCEPTION WHEN SQLSTATE 'UM004' THEN NULL;
    END;
    IF (SELECT cancel_reason FROM reward_redemption_state WHERE id=d)<>'mutual' THEN
        RAISE EXCEPTION 'refund state';
    END IF;
    -- F: a failed audit rolls the confirmation/debit/event back together.
    BEGIN
        op:=gen_random_uuid();
        INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
            VALUES(op,c,a,'REWARD_REDEMPTION','reward_redemption',d2::text);
        INSERT INTO reward_redemption_events(redemption_id,couple_id,actor_id,action,operation_id,idempotency_key,request_fingerprint)
            VALUES(d2,c,a,'confirm',op,'atomic_'||tag,fp);
        INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
            VALUES(op,c,b,'flower',4,-3,7);
        INSERT INTO audit_logs(actor_type,actor_id,couple_id,action) VALUES('user',a,c,'');
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='bad audit succeeded';
    EXCEPTION WHEN check_violation THEN NULL;
    END;
    IF (SELECT status FROM reward_redemption_state WHERE id=d2)<>'pending_confirmation'
       OR (SELECT count(*) FROM wallet_ledger_entries WHERE couple_id=c)<>3 THEN
        RAISE EXCEPTION 'atomic rollback failed';
    END IF;
    -- G: append-only and view privilege remain enforced in both roles.
    BEGIN
        UPDATE reward_redemption_events SET action=action WHERE false;
        RAISE EXCEPTION USING ERRCODE='UWT00',MESSAGE='event update accepted';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE IS DISTINCT FROM (CASE WHEN is_app THEN '42501' ELSE 'UM001' END) THEN RAISE; END IF;
    END;
    IF is_app AND has_table_privilege(current_user,'reward_redemption_state','INSERT') THEN
        RAISE EXCEPTION 'state view writable';
    END IF;
    RAISE NOTICE 'ALL REDEMPTION WORKFLOW ASSERTIONS PASSED: role=% groups=7',current_user;
END $test$;
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
