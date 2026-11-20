-- Rollback-only causal controls for the registration command's terminal timestamp.
-- Run as maintenance on literal usapp_test, -v expected=baseline (0022) or fixed (0048), -v mode=application|maintenance.
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('usapp.push_clock_expected', :'expected', true);
SELECT set_config('usapp.push_clock_role', :'mode', true);
SELECT set_config('usapp.push_clock_base', pg_catalog.clock_timestamp()::text, true);
SELECT set_config('usapp.push_clock_offset','0 seconds',true);
SELECT set_config('usapp.push_clock_step','off',true);
DO $guard$
BEGIN
  IF current_database() <> 'usapp_test' OR NOT (SELECT rolsuper FROM pg_roles WHERE rolname=current_user) THEN
    RAISE EXCEPTION 'maintenance on usapp_test required';
  END IF;
  IF current_setting('usapp.push_clock_expected') NOT IN ('baseline','fixed') OR current_setting('usapp.push_clock_role') NOT IN ('application','maintenance') THEN
    RAISE EXCEPTION 'invalid control mode';
  END IF;
  IF (SELECT atthasdef FROM pg_attribute WHERE attrelid='public.push_registration_commands'::regclass AND attname='recorded_at')
     IS DISTINCT FROM (current_setting('usapp.push_clock_expected')='baseline') THEN RAISE EXCEPTION 'unexpected timestamp default'; END IF;
END $guard$;
CREATE SCHEMA usapp_push_clock;
GRANT USAGE ON SCHEMA usapp_push_clock TO usapp_api;
CREATE FUNCTION usapp_push_clock.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE AS
$$ SELECT current_setting('usapp.push_clock_base')::timestamptz + current_setting('usapp.push_clock_offset')::interval $$;
CREATE FUNCTION usapp_push_clock.step() RETURNS trigger LANGUAGE plpgsql VOLATILE AS
$$ BEGIN
  IF current_setting('usapp.push_clock_step')='on' THEN
    PERFORM set_config('usapp.push_clock_offset','-1 day',true);
    PERFORM set_config('usapp.push_clock_step','off',true);
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER a_push_clock_step BEFORE INSERT ON push_registration_commands FOR EACH ROW EXECUTE FUNCTION usapp_push_clock.step();
-- Bind the old column default to the same controllable clock; the fixed schema deliberately has no default.
DO $$ BEGIN
  IF current_setting('usapp.push_clock_expected')='baseline' THEN
    ALTER TABLE public.push_registration_commands ALTER COLUMN recorded_at SET DEFAULT usapp_push_clock.clock_timestamp();
  END IF;
END $$;
SET LOCAL search_path=usapp_push_clock,pg_catalog,public;
DO $$ BEGIN IF current_setting('usapp.push_clock_role')='application' THEN SET LOCAL ROLE usapp_api; END IF; END $$;
DO $test$
DECLARE
  u uuid:=gen_random_uuid(); other_u uuid:=gen_random_uuid(); c uuid:=gen_random_uuid(); s uuid:=gen_random_uuid();
  g uuid; first_g uuid; endpoint text; tag text:=substr(md5(random()::text),1,12);
  base timestamptz:=current_setting('usapp.push_clock_base')::timestamptz;
  actual timestamptz; code text; message text; accepted boolean;
  fixed boolean:=current_setting('usapp.push_clock_expected')='fixed';
  cases integer:=0; i integer;
BEGIN
  INSERT INTO users(id,email_canonical,email_display) VALUES
    (u,'pc-time-a-'||tag||'@example.com','pc-time-a-'||tag||'@example.com'),
    (other_u,'pc-time-b-'||tag||'@example.com','pc-time-b-'||tag||'@example.com');
  INSERT INTO couples(id,name,created_by_user_id) VALUES(c,'pc-time',u);
  INSERT INTO couple_members(user_id,couple_id,slot) VALUES(u,c,1),(other_u,c,2);
  INSERT INTO sessions(id,token_hash,csrf_token_hash,user_id,is_authenticated,created_at,expires_at)
    VALUES(s,md5(random()::text)||tag,md5(random()::text),u,true,base-interval '3 days',base+interval '30 days');
  FOR i IN 1..2 LOOP
    PERFORM set_config('usapp.push_clock_offset','0 seconds',true);
    g:=gen_random_uuid(); IF i=1 THEN first_g:=g; END IF;
    endpoint:='https://fcm.googleapis.com/fcm/send/pc-time-'||tag||'-'||i;
    INSERT INTO push_subscriptions(id,user_id,couple_id,session_id,endpoint,endpoint_hash,p256dh,auth,vapid_key_id,activated_at)
      VALUES(g,u,c,s,endpoint,encode(sha256(convert_to(endpoint,'UTF8')),'hex'),'B'||repeat('A',86),repeat('A',22),repeat('a',64),
             CASE WHEN i=1 THEN base ELSE base-interval '2 days' END);
    IF i=1 THEN PERFORM set_config('usapp.push_clock_offset','-1 day',true);
    ELSE PERFORM set_config('usapp.push_clock_step','on',true); END IF;
    accepted:=false;code:=NULL;message:=NULL;
    BEGIN
      INSERT INTO push_registration_commands(user_id,idempotency_key,couple_id,session_id,request_fingerprint,result,generation_id)
        VALUES(u,'pc-clock-auto-'||tag||'-'||i,c,s,repeat('b',64),'created',g) RETURNING recorded_at INTO actual;
      accepted:=true;
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS code=RETURNED_SQLSTATE,message=MESSAGE_TEXT; END;
    IF fixed THEN
      IF NOT accepted OR actual IS DISTINCT FROM (CASE WHEN i=1 THEN base ELSE base-interval '1 day' END) THEN
        RAISE EXCEPTION 'automatic command timestamp failed causal control % state=%',i,code;
      END IF;
    ELSIF accepted OR code IS DISTINCT FROM 'UY004' OR message IS DISTINCT FROM 'USAPP_PUSH_COMMAND_TARGET' THEN
      RAISE EXCEPTION 'baseline did not refuse causal control % state=%',i,code;
    END IF;
    cases:=cases+1;
  END LOOP;
  PERFORM set_config('usapp.push_clock_offset','0 seconds',true);
  PERFORM set_config('usapp.push_clock_step','off',true);
  -- A supplied before-source or future timestamp remains invalid, and caller identity still wins over timestamp handling.
  FOR i IN 1..3 LOOP
    code:=NULL;message:=NULL;
    BEGIN
      INSERT INTO push_registration_commands(user_id,idempotency_key,couple_id,session_id,request_fingerprint,result,generation_id,recorded_at)
        VALUES(CASE WHEN i=3 THEN other_u ELSE u END,'pc-clock-invalid-'||tag||'-'||i,c,s,repeat('b',64),'already_active',first_g,
          CASE WHEN i=1 THEN base-interval '1 microsecond' WHEN i=2 THEN base+interval '1 day' ELSE base END);
    EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS code=RETURNED_SQLSTATE,message=MESSAGE_TEXT; END;
    IF code IS DISTINCT FROM 'UY004' OR message IS DISTINCT FROM 'USAPP_PUSH_COMMAND_TARGET' THEN
      RAISE EXCEPTION 'explicit/identity refusal changed at control % state=%',i,code;
    END IF;
    cases:=cases+1;
  END LOOP;
  IF fixed THEN
    INSERT INTO push_registration_commands(user_id,idempotency_key,couple_id,session_id,request_fingerprint,result,generation_id,recorded_at)
      VALUES(u,'pc-clock-null-'||tag,c,s,repeat('b',64),'already_active',first_g,NULL) RETURNING recorded_at INTO actual;
    IF actual IS DISTINCT FROM base THEN RAISE EXCEPTION 'explicit NULL was not assigned'; END IF;
    cases:=cases+1;
  END IF;
  RAISE NOTICE 'PUSH_COMMAND_CLOCK expected=% role=% cases=% passed',current_setting('usapp.push_clock_expected'),current_setting('usapp.push_clock_role'),cases;
END $test$;
ROLLBACK;
