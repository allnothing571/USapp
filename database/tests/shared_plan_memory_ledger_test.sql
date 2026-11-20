\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
    a uuid := gen_random_uuid();
    b uuid := gen_random_uuid();
    space uuid := gen_random_uuid();
    outsider_space uuid := gen_random_uuid();
    key text := gen_random_uuid()::text;
    p uuid;
    m uuid;
    result jsonb;
BEGIN
    IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
    INSERT INTO users(id,email_canonical,email_display) VALUES
        (a,'p34a-'||key||'@example.test','p34a-'||key||'@example.test'),
        (b,'p34b-'||key||'@example.test','p34b-'||key||'@example.test');
    INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p34',a),(outsider_space,'other',b);
    INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(outsider_space,b,1);
    result := usapp_shared_plan_write(a,NULL,'create',key,
        '{"body":{"title":"A plan","date":null,"time":null,"place":null,"note":null}}');
    p := (result #>> '{plan,id}')::uuid;
    IF p IS NULL THEN RAISE EXCEPTION 'fixture create'; END IF;
    INSERT INTO memories(couple_id,creator_id) VALUES (outsider_space,b) RETURNING id INTO m;
    result := usapp_plan_memory_attach(a,p,key,m);
    IF result->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'unfinished plan linked'; END IF;
    result := usapp_shared_plan_write(a,p,'advance',key,'{"expected_status":"want"}');
    result := usapp_shared_plan_write(a,p,'advance',key||'-2','{"expected_status":"plan"}');
    IF result #>> '{plan,status}' IS DISTINCT FROM 'finished' THEN RAISE EXCEPTION 'fixture advance'; END IF;
    result := usapp_plan_memory_attach(a,p,key,m);
    IF result->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'cross-space memory linked'; END IF;
    result := usapp_plan_memory_attach(b,p,key,m);
    IF result->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'cross-space plan linked'; END IF;
    BEGIN
        INSERT INTO shared_plan_memory_links (plan_id,couple_id,memory_id,actor_id,mode,request_key)
        VALUES (p,space,m,a,'existing',key);
        RAISE EXCEPTION 'direct cross-space insert accepted';
    EXCEPTION WHEN foreign_key_violation THEN NULL;
    END;
    BEGIN
        INSERT INTO shared_plan_memory_links (plan_id,couple_id,memory_id,actor_id,mode,request_key)
        VALUES (p,space,m,a,'create',key);
        RAISE EXCEPTION 'invalid direct create accepted';
    EXCEPTION WHEN SQLSTATE 'UK006' THEN NULL;
    END;
    IF EXISTS (SELECT 1 FROM shared_plan_memory_links WHERE plan_id = p) THEN RAISE EXCEPTION 'refusal left link'; END IF;
    RAISE NOTICE 'ALL PLAN MEMORY LEDGER ASSERTIONS PASSED';
END
$test$;
ROLLBACK;
