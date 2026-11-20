-- First achievement batch: five computed badges, no currency or award writes.
-- Activation is an explicit, one-time maintenance step, not installation time.
-- task_completions is append-only; task progress/replies/rewards never count here.
BEGIN;
CREATE TABLE achievement_activation (
    singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton),
    enabled_at timestamptz NOT NULL
);
CREATE FUNCTION usapp_achievement_activation_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP <> 'INSERT' THEN
        RAISE EXCEPTION USING ERRCODE = 'AZ001', MESSAGE = 'USAPP_ACHIEVEMENT_ACTIVATION_IMMUTABLE';
    END IF;
    IF NEW.enabled_at IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'AZ001', MESSAGE = 'USAPP_ACHIEVEMENT_ACTIVATION_SERVER_TIME';
    END IF;
    NEW.enabled_at := clock_timestamp();
    RETURN NEW;
END $$;
CREATE TRIGGER achievement_activation_insert BEFORE INSERT ON achievement_activation
    FOR EACH ROW EXECUTE FUNCTION usapp_achievement_activation_guard();
CREATE TRIGGER achievement_activation_immutable BEFORE UPDATE OR DELETE OR TRUNCATE ON achievement_activation
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_achievement_activation_guard();

-- Invoker rights, maintenance only. Concurrent/repeated activation keeps the first committed timestamp.
CREATE FUNCTION usapp_achievement_activate() RETURNS timestamptz LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp AS $$
DECLARE result timestamptz;
BEGIN
    INSERT INTO public.achievement_activation DEFAULT VALUES ON CONFLICT DO NOTHING;
    SELECT enabled_at INTO STRICT result FROM public.achievement_activation WHERE singleton;
    RETURN result;
END $$;

-- Pure rule kernel. Supplied times are completion instants, not progress events.
-- Timestamp equality is included at cutover. Historical best streak never resets on a later inactive day.
CREATE FUNCTION usapp_achievement_facts(events timestamptz[], cutover timestamptz)
RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE SET search_path = pg_catalog, pg_temp AS $$
WITH eligible AS (
    SELECT t FROM unnest(events) AS e(t) WHERE cutover IS NOT NULL AND t >= cutover
), ordered AS (
    SELECT t, row_number() OVER (ORDER BY t) AS n FROM eligible
), days AS (
    SELECT (t AT TIME ZONE 'Asia/Shanghai')::date AS day, min(t) AS first_at FROM eligible GROUP BY 1
), islands AS (
    SELECT day, first_at, day - (row_number() OVER (ORDER BY day))::integer AS grp FROM days
), runs AS (
    SELECT first_at, row_number() OVER (PARTITION BY grp ORDER BY day) AS n FROM islands
), counts AS (
    SELECT (SELECT count(*) FROM eligible) AS total, COALESCE((SELECT max(n) FROM runs), 0) AS longest
), rules(code, metric, target, position) AS (
    VALUES ('first_task', 'total', 1, 1), ('task_10', 'total', 10, 2), ('task_50', 'total', 50, 3),
           ('streak_3', 'streak', 3, 4), ('streak_7', 'streak', 7, 5)
), badges AS (
    SELECT position, jsonb_build_object('code', code, 'metric', metric, 'target', target,
      'progress', LEAST(target, CASE WHEN metric = 'total' THEN total ELSE longest END),
      'earned_at', to_char(CASE WHEN metric = 'total' THEN (SELECT min(t) FROM ordered WHERE n >= target)
                              ELSE (SELECT min(first_at) FROM runs WHERE n >= target) END
                          AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')) AS badge
    FROM rules CROSS JOIN counts
)
SELECT jsonb_build_object('total_completed', total, 'longest_streak', longest,
                         'badges', (SELECT jsonb_agg(badge ORDER BY position) FROM badges)) FROM counts;
$$;
REVOKE ALL ON achievement_activation FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON achievement_activation TO usapp_api;
REVOKE ALL ON FUNCTION usapp_achievement_activate() FROM PUBLIC, usapp_api, usapp_push;
REVOKE ALL ON FUNCTION usapp_achievement_facts(timestamptz[], timestamptz) FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION usapp_achievement_facts(timestamptz[], timestamptz) TO usapp_api;
COMMIT;
