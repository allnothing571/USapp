\set ON_ERROR_STOP on
BEGIN;
SET LOCAL TIME ZONE 'Pacific/Honolulu';
DO $setup$
BEGIN
  INSERT INTO users(id,email_canonical,email_display) VALUES
    ('71000000-0000-4000-8000-000000000001','garden-a@example.test','garden-a@example.test'),
    ('71000000-0000-4000-8000-000000000002','garden-b@example.test','garden-b@example.test'),
    ('71000000-0000-4000-8000-000000000003','garden-c@example.test','garden-c@example.test');
  INSERT INTO couples(id,name,created_by_user_id) VALUES
    ('71000000-0000-4000-8000-000000000011','Garden test','71000000-0000-4000-8000-000000000001'),
    ('71000000-0000-4000-8000-000000000012','Other garden','71000000-0000-4000-8000-000000000003');
  INSERT INTO couple_members(couple_id,user_id,slot) VALUES
    ('71000000-0000-4000-8000-000000000011','71000000-0000-4000-8000-000000000001',1),
    ('71000000-0000-4000-8000-000000000011','71000000-0000-4000-8000-000000000002',2),
    ('71000000-0000-4000-8000-000000000012','71000000-0000-4000-8000-000000000003',1);
  INSERT INTO shared_garden_waterings(couple_id,watering_date,user_id)
  VALUES ('71000000-0000-4000-8000-000000000011',
          (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date - 2,
          '71000000-0000-4000-8000-000000000001');
END
$setup$;
SET LOCAL ROLE usapp_api;
DO $proof$
DECLARE
  a constant uuid := '71000000-0000-4000-8000-000000000001';
  b constant uuid := '71000000-0000-4000-8000-000000000002';
  outsider constant uuid := '71000000-0000-4000-8000-000000000003';
  unbound constant uuid := '71000000-0000-4000-8000-000000000004';
  today date := (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
  canonical text := to_char(today,'YYYY-MM-DD');
  view_row record;
  recent_row record;
BEGIN
  IF has_table_privilege('usapp_api','public.shared_garden_waterings','SELECT,INSERT,UPDATE,DELETE') OR
     has_table_privilege('usapp_push','public.shared_garden_waterings','SELECT,INSERT,UPDATE,DELETE') OR
     has_function_privilege('usapp_push','public.usapp_shared_garden_read(uuid)','EXECUTE') OR
     has_function_privilege('usapp_push','public.usapp_shared_garden_water(uuid,text)','EXECUTE') OR
     has_function_privilege('usapp_push','public.usapp_shared_garden_recent(uuid)','EXECUTE')
    THEN RAISE EXCEPTION 'garden privilege boundary'; END IF;

  SELECT * INTO STRICT view_row FROM usapp_shared_garden_read(a);
  IF view_row.watering_date <> canonical OR view_row.total_waterings <> 1
      OR view_row.watered_today OR view_row.partner_watered_today
    THEN RAISE EXCEPTION 'previous watering does not limit today'; END IF;
  IF EXISTS(SELECT FROM usapp_shared_garden_read(unbound)) OR
     usapp_shared_garden_water(unbound,canonical) <> 'no_space'
    THEN RAISE EXCEPTION 'unbound user accessed garden'; END IF;
  IF usapp_shared_garden_water(a,to_char(today-2,'YYYY-MM-DD')) <> 'date_changed' OR
     usapp_shared_garden_water(a,to_char(today-1,'YYYY-MM-DD')) <> 'date_changed' OR
     usapp_shared_garden_water(a,to_char(today+1,'YYYY-MM-DD')) <> 'date_changed' OR
     usapp_shared_garden_water(a,NULL) <> 'date_changed'
    THEN RAISE EXCEPTION 'accepted wrong date'; END IF;
  IF usapp_shared_garden_water(a,canonical) <> 'watered' OR
     usapp_shared_garden_water(a,canonical) <> 'watered'
    THEN RAISE EXCEPTION 'same-day replay failed'; END IF;
  SELECT * INTO STRICT view_row FROM usapp_shared_garden_read(b);
  IF view_row.total_waterings <> 2 OR view_row.watered_today OR NOT view_row.partner_watered_today
    THEN RAISE EXCEPTION 'shared progress or replay count mismatch'; END IF;
  IF usapp_shared_garden_water(b,canonical) <> 'watered'
    THEN RAISE EXCEPTION 'partner could not water on same day'; END IF;
  SELECT * INTO STRICT view_row FROM usapp_shared_garden_read(a);
  IF view_row.total_waterings <> 3 OR NOT view_row.watered_today OR NOT view_row.partner_watered_today
    THEN RAISE EXCEPTION 'same-day progress mismatch'; END IF;
  SELECT * INTO STRICT view_row FROM usapp_shared_garden_read(outsider);
  IF view_row.total_waterings <> 0 OR view_row.watered_today OR view_row.partner_watered_today
    THEN RAISE EXCEPTION 'garden leaked across couples'; END IF;
  IF (SELECT array_agg(watering_date) FROM usapp_shared_garden_recent(a)) IS DISTINCT FROM
       ARRAY(SELECT to_char(today - days_ago, 'YYYY-MM-DD') FROM generate_series(0,6) AS days_ago)
    THEN RAISE EXCEPTION 'recent watering dates mismatch'; END IF;
  SELECT * INTO STRICT recent_row FROM usapp_shared_garden_recent(b) WHERE watering_date = canonical;
  IF NOT recent_row.watered OR NOT recent_row.partner_watered
    THEN RAISE EXCEPTION 'today history does not show both members'; END IF;
  SELECT * INTO STRICT recent_row FROM usapp_shared_garden_recent(b) WHERE watering_date = to_char(today-2,'YYYY-MM-DD');
  IF recent_row.watered OR NOT recent_row.partner_watered
    THEN RAISE EXCEPTION 'historical perspective is not relative to viewer'; END IF;
  IF EXISTS (SELECT FROM usapp_shared_garden_recent(b)
             WHERE watering_date = to_char(today-1,'YYYY-MM-DD') AND (watered OR partner_watered)) OR
     EXISTS (SELECT FROM usapp_shared_garden_recent(outsider) WHERE watered OR partner_watered) OR
     EXISTS (SELECT FROM usapp_shared_garden_recent(unbound))
    THEN RAISE EXCEPTION 'daily empty rows or membership scope mismatch'; END IF;
  IF has_function_privilege('usapp_push','public.usapp_shared_garden_month(uuid,text)','EXECUTE') OR
     EXISTS (SELECT FROM usapp_shared_garden_month(unbound,NULL))
    THEN RAISE EXCEPTION 'month privilege boundary'; END IF;
  IF (SELECT count(*) FROM usapp_shared_garden_month(a,NULL)) <> extract(day FROM today) OR
     (SELECT max(watering_date) FROM usapp_shared_garden_month(a,NULL)) IS DISTINCT FROM canonical
    THEN RAISE EXCEPTION 'current Beijing month includes an incorrect day'; END IF;
  IF (SELECT count(*) FROM usapp_shared_garden_month(a,'2000-02')) <> 29 OR
     EXISTS (SELECT FROM usapp_shared_garden_month(outsider,NULL) WHERE watered OR partner_watered)
    THEN RAISE EXCEPTION 'leap month or month membership mismatch'; END IF;
  IF EXISTS (SELECT FROM usapp_shared_garden_month(a,'2000-13') WHERE watering_date IS NOT NULL) OR
     EXISTS (SELECT FROM usapp_shared_garden_month(a,to_char(today + interval '1 month','YYYY-MM')) WHERE watering_date IS NOT NULL)
    THEN RAISE EXCEPTION 'invalid or future month produced days'; END IF;
END
$proof$;
ROLLBACK;
\echo SHARED_GARDEN_SQL_PASSED
