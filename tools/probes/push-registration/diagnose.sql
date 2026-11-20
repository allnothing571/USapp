\set ON_ERROR_STOP on
-- Synthetic fixtures only. The instrumented guard is installed by the ownership-checked disposable runner.
BEGIN;
DO $diagnostic$
DECLARE
    u uuid := gen_random_uuid();
    c uuid := gen_random_uuid();
    s uuid := gen_random_uuid();
    g uuid;
    endpoint text;
    tag text := substr(md5(random()::text),1,12);
    reason text;
    code text;
    counts jsonb := '{}'::jsonb;
    passed integer := 0;
    failed integer := 0;
    controls integer := 0;
    i integer;
BEGIN
    IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database required'; END IF;
    INSERT INTO users(id,email_canonical,email_display) VALUES(u,'push-diag-'||tag||'@example.com','push-diag-'||tag||'@example.com');
    INSERT INTO couples(id,name,created_by_user_id) VALUES(c,'push-diag',u);
    INSERT INTO couple_members(user_id,couple_id,slot) VALUES(u,c,1);
    INSERT INTO sessions(id,token_hash,csrf_token_hash,user_id,is_authenticated,created_at,expires_at)
      VALUES(s,md5(random()::text)||tag,md5(random()::text),u,true,clock_timestamp()-interval '1 day',clock_timestamp()+interval '1 day');
    FOR i IN 1..2000 LOOP
        BEGIN
            g := gen_random_uuid();
            endpoint := 'https://fcm.googleapis.com/fcm/send/push-diag-'||tag||'-'||i;
            INSERT INTO push_subscriptions(id,user_id,couple_id,session_id,endpoint,endpoint_hash,p256dh,auth,vapid_key_id)
              VALUES(g,u,c,s,endpoint,encode(sha256(convert_to(endpoint,'UTF8')),'hex'),'B'||repeat('A',86),repeat('A',22),repeat('a',64));
            INSERT INTO push_registration_commands(user_id,idempotency_key,couple_id,session_id,request_fingerprint,result,generation_id)
              VALUES(u,'push-diag-'||tag||'-'||i,c,s,repeat('b',64),'created',g);
            RAISE EXCEPTION USING ERRCODE='PZ001',MESSAGE='rollback successful sample';
        EXCEPTION WHEN SQLSTATE 'PZ001' THEN passed := passed+1;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS reason=PG_EXCEPTION_DETAIL,code=RETURNED_SQLSTATE;
            IF reason NOT IN ('missing_generation','user_mismatch','couple_mismatch','session_mismatch','session_invalid',
                              'recorded_before_activation','recorded_after_clock') OR reason IS NULL THEN
                reason := 'other_'||code;
            END IF;
            counts := jsonb_set(counts,ARRAY[reason],to_jsonb(COALESCE((counts->>reason)::integer,0)+1));
            failed := failed+1;
        END;
    END LOOP;
    -- Positive controls for the two time labels. These explicit invalid caller times MUST still be refused.
    g := gen_random_uuid();
    endpoint := 'https://fcm.googleapis.com/fcm/send/push-diag-control-'||tag;
    INSERT INTO push_subscriptions(id,user_id,couple_id,session_id,endpoint,endpoint_hash,p256dh,auth,vapid_key_id)
      VALUES(g,u,c,s,endpoint,encode(sha256(convert_to(endpoint,'UTF8')),'hex'),'B'||repeat('A',86),repeat('A',22),repeat('a',64));
    FOR i IN 1..2 LOOP
      BEGIN
        INSERT INTO push_registration_commands(user_id,idempotency_key,couple_id,session_id,request_fingerprint,result,generation_id,recorded_at)
          VALUES(u,'push-diag-control-'||tag||'-'||i,c,s,repeat('b',64),'created',g,
                 CASE WHEN i=1 THEN (SELECT activated_at-interval '1 microsecond' FROM push_subscriptions WHERE id=g)
                      ELSE clock_timestamp()+interval '1 day' END);
        RAISE EXCEPTION 'invalid control unexpectedly accepted';
      EXCEPTION WHEN SQLSTATE 'UY004' THEN
        GET STACKED DIAGNOSTICS reason=PG_EXCEPTION_DETAIL;
        IF reason IS DISTINCT FROM (CASE WHEN i=1 THEN 'recorded_before_activation' ELSE 'recorded_after_clock' END) THEN
          RAISE EXCEPTION 'diagnostic control mismatch';
        END IF;
        controls := controls+1;
      END;
    END LOOP;
    RAISE NOTICE 'PUSH_GUARD_SAMPLE total=2000 accepted=% refused=% labels=% controls=%',passed,failed,counts,controls;
END
$diagnostic$;
ROLLBACK;
