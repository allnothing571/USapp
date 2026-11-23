\set ON_ERROR_STOP on
BEGIN;
DO $setup$
DECLARE
  a uuid := '70000000-0000-4000-8000-000000000001';
  b uuid := '70000000-0000-4000-8000-000000000002';
  c uuid := '70000000-0000-4000-8000-000000000003';
  d uuid := '70000000-0000-4000-8000-000000000004';
  r uuid := '70000000-0000-4000-8000-000000000011';
  s uuid := '70000000-0000-4000-8000-000000000012';
BEGIN
  INSERT INTO users(id,email_canonical,email_display) VALUES
    (a,'room-a@example.test','room-a@example.test'),(b,'room-b@example.test','room-b@example.test'),
    (c,'room-c@example.test','room-c@example.test'),(d,'room-d@example.test','room-d@example.test');
  INSERT INTO couples(id,name,created_by_user_id) VALUES (r,'Room Test',a),(s,'Other Room',c);
  INSERT INTO couple_members(couple_id,user_id,slot) VALUES (r,a,1),(r,b,2),(s,c,1);
  PERFORM set_config('session_replication_role','replica',true);
  INSERT INTO wallet_accounts(user_id,asset_type,couple_id) VALUES (a,'sunshine',r);
  INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
    VALUES ('70000000-0000-4000-8000-000000000021',r,a,'TASK_REWARD','task','room-furniture-test');
  INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
    VALUES ('70000000-0000-4000-8000-000000000021',r,a,'sunshine',1,25,25);
  PERFORM set_config('session_replication_role','origin',true);
END
$setup$;
SET LOCAL ROLE usapp_api;
DO $test$
DECLARE
  a uuid := '70000000-0000-4000-8000-000000000001';
  b uuid := '70000000-0000-4000-8000-000000000002';
  c uuid := '70000000-0000-4000-8000-000000000003';
  d uuid := '70000000-0000-4000-8000-000000000004';
  layout_a jsonb := '[{"item":"lamp","x":1,"y":0}]';
  layout_b jsonb := '[{"item":"plant","x":2,"y":1}]';
  invalid jsonb;
  paid jsonb := '[{"item":"bookshelf","x":0,"y":0}]';
  key text := 'room-furniture-test-a';
BEGIN
  IF has_table_privilege('usapp_api','public.shared_rooms','SELECT,INSERT,UPDATE,DELETE') OR
     has_table_privilege('usapp_api','public.shared_room_avatar_choices','SELECT,INSERT,UPDATE,DELETE') OR
     has_function_privilege('usapp_push','public.usapp_shared_room_save(uuid,integer,jsonb)','EXECUTE') THEN
    RAISE EXCEPTION 'room privilege boundary';
  END IF;
  IF usapp_shared_room_read(d) IS NOT NULL OR usapp_shared_room_save(d,0,layout_a) <> 'no_space' OR
     usapp_shared_room_share(d,true) <> 'no_space' THEN RAISE EXCEPTION 'unbound user'; END IF;
  IF usapp_shared_room_read(a) IS DISTINCT FROM '{"version":0,"layout":[],"share_avatar":false,"partner_id":null}'::jsonb
     THEN RAISE EXCEPTION 'room not lazily empty'; END IF;
  IF usapp_shared_room_save(a,0,layout_a) <> 'saved' THEN RAISE EXCEPTION 'first room save'; END IF;
  IF usapp_shared_room_read(b)->'layout' IS DISTINCT FROM layout_a OR
     usapp_shared_room_read(c)->'layout' IS DISTINCT FROM '[]'::jsonb THEN RAISE EXCEPTION 'room isolation'; END IF;
  IF usapp_shared_room_save(b,0,layout_b) <> 'conflict' THEN RAISE EXCEPTION 'first save replay'; END IF;
  IF usapp_shared_room_save(b,1,layout_b) <> 'saved' THEN RAISE EXCEPTION 'partner save'; END IF;
  IF usapp_shared_room_save(a,1,layout_a) <> 'conflict' OR
     usapp_shared_room_read(a)->'layout' IS DISTINCT FROM layout_b THEN RAISE EXCEPTION 'version conflict'; END IF;
  FOREACH invalid IN ARRAY ARRAY[
     '[0]'::jsonb,'[{"item":"lamp","x":"1","y":0}]','[{"item":"lamp","x":1}]',
     '[{"item":"lamp","x":10,"y":0}]','[{"item":"hat","x":1,"y":0}]',
     '[{"item":"lamp","x":1,"y":0},{"item":"lamp","x":2,"y":0}]',
     '[{"item":"lamp","x":1,"y":0},{"item":"plant","x":1,"y":0}]'
  ] LOOP
    IF usapp_shared_room_save(a,2,invalid) <> 'invalid' THEN RAISE EXCEPTION 'accepted invalid layout'; END IF;
  END LOOP;
  IF usapp_shared_room_share(b,true) <> 'saved' THEN RAISE EXCEPTION 'enable avatar consent'; END IF;
  IF usapp_shared_room_read(a)->>'partner_id' IS DISTINCT FROM b::text OR
     usapp_shared_room_read(c)->>'partner_id' IS NOT NULL OR usapp_shared_room_read(b)->>'partner_id' IS NOT NULL
     THEN RAISE EXCEPTION 'avatar consent'; END IF;
  IF usapp_shared_room_share(b,false) <> 'saved' THEN RAISE EXCEPTION 'revoke avatar consent'; END IF;
  IF usapp_shared_room_read(a)->>'partner_id' IS NOT NULL THEN RAISE EXCEPTION 'revoked avatar exposed'; END IF;
  IF usapp_shared_room_read(a)->>'version' IS DISTINCT FROM '2' THEN RAISE EXCEPTION 'consent changed layout version'; END IF;
  IF has_table_privilege('usapp_api','public.room_furniture_purchases','SELECT,INSERT,UPDATE,DELETE') OR
    has_function_privilege('usapp_push','public.usapp_room_furniture_buy(uuid,text,text)','EXECUTE')
    THEN RAISE EXCEPTION 'furniture privilege boundary'; END IF;
  IF usapp_shared_room_save(b,2,paid) <> 'invalid' THEN RAISE EXCEPTION 'unowned furniture'; END IF;
  IF usapp_room_furniture_buy(a,'armchair',key) <> 'insufficient' THEN RAISE EXCEPTION 'overspend'; END IF;
  IF usapp_room_furniture_buy(a,'bookshelf',key) <> 'saved' THEN RAISE EXCEPTION 'purchase'; END IF;
  IF usapp_room_furniture_buy(a,'bookshelf',key) <> 'replay' THEN RAISE EXCEPTION 'replay'; END IF;
  IF usapp_room_furniture_buy(a,'vase',key) <> 'key_reused' THEN RAISE EXCEPTION 'key reuse'; END IF;
  IF usapp_room_furniture_buy(b,'bookshelf','room-furniture-test-b') <> 'owned' THEN RAISE EXCEPTION 'partner repurchase'; END IF;
  IF NOT (usapp_room_furniture_read(b) @> '[{"item":"bookshelf","owned":true}]') OR
    NOT (usapp_room_furniture_read(c) @> '[{"item":"bookshelf","owned":false}]') THEN RAISE EXCEPTION 'shared ownership scope'; END IF;
  IF usapp_shared_room_save(b,2,paid) <> 'saved' OR usapp_shared_room_save(c,0,paid) <> 'invalid'
    THEN RAISE EXCEPTION 'paid placement entitlement'; END IF;
  IF (SELECT count(*) FROM wallet_ledger_entries WHERE user_id=a AND asset_type='sunshine' AND amount=-20 AND balance_after=5) <> 1
    THEN RAISE EXCEPTION 'sunshine debit'; END IF;
END
$test$;
RESET ROLE;
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
\echo SHARED_ROOM_SQL_PASSED
