\set ON_ERROR_STOP on
BEGIN;
INSERT INTO users(id,email_canonical,email_display) VALUES
 ('7e000000-0000-4000-8000-000000000001','positions-a@example.test','positions-a@example.test'),
 ('7e000000-0000-4000-8000-000000000002','positions-b@example.test','positions-b@example.test'),
 ('7e000000-0000-4000-8000-000000000003','positions-c@example.test','positions-c@example.test');
INSERT INTO couples(id,name,created_by_user_id) VALUES
 ('7e000000-0000-4000-8000-000000000010','Position test','7e000000-0000-4000-8000-000000000001');
INSERT INTO couple_members(couple_id,user_id,slot) VALUES
 ('7e000000-0000-4000-8000-000000000010','7e000000-0000-4000-8000-000000000001',1),
 ('7e000000-0000-4000-8000-000000000010','7e000000-0000-4000-8000-000000000002',2);
INSERT INTO space_invites(id,couple_id,code_hash,code_prefix,created_by_user_id,expires_at,
                         room_issuer_side,used_count,consumed_by_user_id,consumed_at) VALUES
 ('7e000000-0000-4000-8000-000000000020','7e000000-0000-4000-8000-000000000010','positions-test','posi',
  '7e000000-0000-4000-8000-000000000001',now()+interval '1 hour','right',1,
  '7e000000-0000-4000-8000-000000000002',now());
SET LOCAL ROLE usapp_api;
DO $test$
DECLARE
  a uuid := '7e000000-0000-4000-8000-000000000001';
  b uuid := '7e000000-0000-4000-8000-000000000002';
  outsider uuid := '7e000000-0000-4000-8000-000000000003';
  invite uuid := '7e000000-0000-4000-8000-000000000020';
  first uuid := '7e000000-0000-4000-8000-000000000021';
  second uuid := '7e000000-0000-4000-8000-000000000022';
BEGIN
  IF has_table_privilege('usapp_api','public.room_standings','SELECT,INSERT,UPDATE,DELETE') OR
     has_table_privilege('usapp_api','public.avatar_character_choices','INSERT,UPDATE,DELETE') OR
     has_function_privilege('usapp_push','public.usapp_room_positions_write(uuid,uuid,text)','EXECUTE') THEN
    RAISE EXCEPTION 'position privilege boundary';
  END IF;
  PERFORM public.usapp_room_initialize_positions(outsider,invite);
  IF public.usapp_room_positions_read(a)->>'side' <> 'left' THEN RAISE EXCEPTION 'legacy side'; END IF;
  PERFORM public.usapp_room_initialize_positions(b,invite);
  IF public.usapp_room_positions_read(a)->>'side' <> 'right' OR public.usapp_room_positions_read(b)->>'side' <> 'left' THEN
    RAISE EXCEPTION 'invitation initialization';
  END IF;
  IF public.usapp_room_positions_write(a,first,'request') <> 'saved' THEN RAISE EXCEPTION 'request'; END IF;
  IF public.usapp_room_positions_read(b)->'pending'->>'incoming' <> 'true' OR
     public.usapp_room_positions_write(a,first,'accept') <> 'position_conflict' OR
     public.usapp_room_positions_write(b,second,'request') <> 'position_conflict' OR
     public.usapp_room_positions_write(outsider,first,'accept') <> 'no_space' THEN RAISE EXCEPTION 'swap authorization'; END IF;
  IF public.usapp_room_positions_write(b,first,'accept') <> 'saved' THEN RAISE EXCEPTION 'accept'; END IF;
  IF public.usapp_room_positions_write(b,first,'accept') <> 'replay' OR
     public.usapp_room_positions_write(a,first,'request') <> 'replay' THEN RAISE EXCEPTION 'swap replay'; END IF;
  PERFORM public.usapp_room_initialize_positions(b,invite);
  IF public.usapp_room_positions_read(a)->>'side' <> 'left' OR public.usapp_room_positions_read(b)->>'side' <> 'right' THEN
    RAISE EXCEPTION 'exactly one swap, no reset by replay';
  END IF;
  IF public.usapp_room_positions_write(b,second,'request') <> 'saved' THEN RAISE EXCEPTION 'new request'; END IF;
  IF public.usapp_room_positions_write(b,second,'cancel') <> 'saved' OR
     public.usapp_room_positions_write(a,second,'accept') <> 'position_conflict' THEN RAISE EXCEPTION 'cancellation'; END IF;
END
$test$;
ROLLBACK;
