\set ON_ERROR_STOP on
BEGIN;
DO $test$
DECLARE
    a uuid := gen_random_uuid();
    b uuid := gen_random_uuid();
    c uuid := gen_random_uuid();
    space uuid := gen_random_uuid();
    other_space uuid := gen_random_uuid();
    tag text := gen_random_uuid()::text;
    p uuid;
    later_plan uuid;
    request_id uuid;
    receipt_id uuid;
    result jsonb;
    original jsonb := '{"body":{"title":"Original","date":null,"time":null,"place":null,"note":null}}';
    change jsonb := '{"expected_version":1,"body":{"title":"Suggested","date":"2028-02-29","time":"09:15","place":"Park","note":null}}';
BEGIN
    IF current_database() <> 'usapp_test' THEN RAISE EXCEPTION 'test database only'; END IF;
    INSERT INTO users(id,email_canonical,email_display) VALUES
        (a,'p33a-'||tag||'@example.test','p33a-'||tag||'@example.test'),
        (b,'p33b-'||tag||'@example.test','p33b-'||tag||'@example.test'),
        (c,'p33c-'||tag||'@example.test','p33c-'||tag||'@example.test');
    INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'p33',a),(other_space,'other',c);
    INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2),(other_space,c,1);

    result := usapp_shared_plan_write(a,NULL,'create',tag,original);
    p := (result #>> '{plan,id}')::uuid;
    receipt_id := (result #>> '{mutation,id}')::uuid;
    IF p IS NULL OR result #>> '{plan,status}' IS DISTINCT FROM 'want' THEN RAISE EXCEPTION 'create'; END IF;
    result := usapp_shared_plan_write(b,NULL,'create',tag,original);
    IF result #>> '{plan,creator_id}' IS DISTINCT FROM b::text THEN RAISE EXCEPTION 'partner create'; END IF;
    later_plan := (result #>> '{plan,id}')::uuid;
    result := usapp_shared_plan_write(a,NULL,'create',tag||'-invalid',
        jsonb_set(original,'{body,date}','"2027-02-29"'));
    IF result->>'error' IS DISTINCT FROM 'validation' OR
       (SELECT count(*) FROM shared_plans WHERE couple_id = space) <> 2 THEN
        RAISE EXCEPTION 'invalid creation left a plan';
    END IF;
    result := usapp_shared_plan_write(c,p,'advance',tag,'{"expected_status":"want"}');
    IF result->>'error' IS DISTINCT FROM 'not_found' THEN RAISE EXCEPTION 'cross-space write'; END IF;
    result := usapp_shared_plan_write(b,p,'edit',tag,change);
    IF result->>'error' IS DISTINCT FROM 'forbidden' THEN RAISE EXCEPTION 'partner edit'; END IF;
    result := usapp_shared_plan_write(b,p,'propose',tag,change);
    request_id := (result #>> '{plan,pending_proposal,id}')::uuid;
    IF request_id IS NULL THEN RAISE EXCEPTION 'propose'; END IF;
    result := usapp_shared_plan_write(b,p,'propose',tag||'-second',change);
    IF result->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'second pending'; END IF;
    result := usapp_shared_plan_write(a,p,'edit',tag,change);
    IF result->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'pending edit'; END IF;
    result := usapp_shared_plan_write(a,p,'advance',tag,'{"expected_status":"plan"}');
    IF result->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'skip'; END IF;
    result := usapp_shared_plan_write(a,p,'advance',tag,'{"expected_status":"want"}');
    IF result #>> '{plan,status}' IS DISTINCT FROM 'plan' OR
       result #>> '{plan,body_version}' IS DISTINCT FROM '1' THEN RAISE EXCEPTION 'advance without body edit'; END IF;
    result := usapp_shared_plan_write(a,p,'advance',tag||'-late','{"expected_status":"want"}');
    IF result->>'error' IS DISTINCT FROM 'conflict' THEN RAISE EXCEPTION 'late advance'; END IF;

    result := usapp_shared_plan_write(a,p,'decide',tag,jsonb_build_object('proposal_id',request_id,'outcome','approved'));
    IF result #>> '{plan,body,title}' IS DISTINCT FROM 'Suggested' OR
       result #>> '{plan,status_version}' IS DISTINCT FROM '2' OR
       result #>> '{plan,body_version}' IS DISTINCT FROM '2' THEN RAISE EXCEPTION 'approval independence'; END IF;
    result := usapp_shared_plan_write(b,p,'advance',tag,'{"expected_status":"plan"}');
    IF result #>> '{plan,status}' IS DISTINCT FROM 'finished' THEN RAISE EXCEPTION 'partner advance'; END IF;

    result := usapp_shared_plan_write(a,NULL,'create',tag,original);
    IF result->>'replayed' IS DISTINCT FROM 'true' OR result #>> '{mutation,id}' IS DISTINCT FROM receipt_id::text OR
       result #>> '{plan,status}' IS DISTINCT FROM 'finished' THEN RAISE EXCEPTION 'create replay'; END IF;
    result := usapp_shared_plan_write(a,NULL,'create',tag,change);
    IF result->>'error' IS DISTINCT FROM 'key_reused' THEN RAISE EXCEPTION 'changed key payload'; END IF;
    result := usapp_shared_plan_write(a,p,'edit',tag,jsonb_set(change,'{expected_version}','2'));
    IF result #>> '{plan,body_version}' IS DISTINCT FROM '3' THEN RAISE EXCEPTION 'finished edit'; END IF;

    receipt_id := gen_random_uuid();
    result := usapp_shared_plan_write(b,p,'propose',receipt_id::text,jsonb_set(change,'{expected_version}','3'));
    request_id := (result #>> '{plan,pending_proposal,id}')::uuid;
    IF request_id IS NULL THEN RAISE EXCEPTION 'new proposal'; END IF;
    result := usapp_shared_plan_write(b,p,'decide',receipt_id::text,jsonb_build_object('proposal_id',request_id,'outcome','approved'));
    IF result->>'error' IS DISTINCT FROM 'forbidden' THEN RAISE EXCEPTION 'self approval'; END IF;
    result := usapp_shared_plan_write(b,p,'decide',receipt_id::text,jsonb_build_object('proposal_id',request_id,'outcome','withdrawn'));
    IF result #>> '{plan,latest_decision,outcome}' IS DISTINCT FROM 'withdrawn' OR
       result #>> '{plan,body_version}' IS DISTINCT FROM '3' THEN RAISE EXCEPTION 'withdraw'; END IF;
    result := usapp_shared_plan_write(b,p,'propose',tag||'-reject',jsonb_set(change,'{expected_version}','3'));
    request_id := (result #>> '{plan,pending_proposal,id}')::uuid;
    result := usapp_shared_plan_write(a,p,'decide',tag||'-reject',jsonb_build_object('proposal_id',request_id,'outcome','rejected'));
    IF result #>> '{plan,latest_decision,outcome}' IS DISTINCT FROM 'rejected' OR
       result #>> '{plan,body_version}' IS DISTINCT FROM '3' THEN RAISE EXCEPTION 'reject'; END IF;
    IF (SELECT id FROM shared_plans WHERE couple_id = space ORDER BY created_at DESC, id DESC LIMIT 1)
       IS DISTINCT FROM later_plan OR
       (SELECT id FROM shared_plans WHERE couple_id = space
        AND (created_at, id) < (SELECT created_at, id FROM shared_plans WHERE id = later_plan)
        ORDER BY created_at DESC, id DESC LIMIT 1) IS DISTINCT FROM p THEN
        RAISE EXCEPTION 'revision changed keyset order';
    END IF;
    RAISE NOTICE 'ALL SHARED PLAN LEDGER ASSERTIONS PASSED';
END
$test$;
ROLLBACK;
