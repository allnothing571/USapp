-- Rollback-only deterministic rule/maintenance boundary tests. No source timestamps or guards are changed.
BEGIN;
SET LOCAL plpgsql.check_asserts = on;
DO $$
DECLARE j jsonb; a timestamptz; b timestamptz; n integer := 0;
BEGIN
  j := usapp_achievement_facts('{}', NULL);
  ASSERT (j->>'total_completed')::int = 0 AND jsonb_array_length(j->'badges') = 5, 'disabled'; n:=n+1;
  ASSERT NOT EXISTS(SELECT FROM jsonb_array_elements(j->'badges') x WHERE x->>'earned_at' IS NOT NULL), 'disabled earned'; n:=n+1;
  j := usapp_achievement_facts(ARRAY['2026-01-01T00:00:00Z'::timestamptz, '2026-01-01T00:00:00.000001Z'], '2026-01-01T00:00:00.000001Z');
  ASSERT (j->>'total_completed')::int = 1 AND j->'badges'->0->>'earned_at' = '2026-01-01T00:00:00.000001Z', 'inclusive exact cutover, no retroactive count'; n:=n+1;
  j := usapp_achievement_facts(ARRAY(SELECT '2026-01-02T01:00:00Z'::timestamptz + g*interval '1 second' FROM generate_series(1,50) g), '2026-01-01T00:00:00Z');
  ASSERT (j->>'total_completed')::int = 50 AND (j->>'longest_streak')::int = 1, '50 completions same Beijing day'; n:=n+1;
  ASSERT j->'badges'->1->>'earned_at' = '2026-01-02T01:00:10.000000Z' AND j->'badges'->2->>'earned_at' = '2026-01-02T01:00:50.000000Z', '10 and 50 earliest crossing'; n:=n+1;
  ASSERT j->'badges'->3->>'earned_at' IS NULL AND (j->'badges'->0->>'progress')::int = 1, 'one day is not streak3, progress capped'; n:=n+1;
  j := usapp_achievement_facts(ARRAY(SELECT '2026-01-01T16:00:00Z'::timestamptz + g*interval '1 day' FROM generate_series(0,6) g), '2026-01-01T00:00:00Z');
  ASSERT (j->>'longest_streak')::int = 7 AND j->'badges'->3->>'earned_at' = '2026-01-03T16:00:00.000000Z' AND j->'badges'->4->>'earned_at' = '2026-01-07T16:00:00.000000Z', 'Beijing day thresholds'; n:=n+1;
  j := usapp_achievement_facts(ARRAY['2026-01-01T15:59:59.999999Z'::timestamptz, '2026-01-01T16:00:00Z', '2026-01-02T16:00:00Z'], '2026-01-01T00:00:00Z');
  ASSERT (j->>'longest_streak')::int = 3, 'Beijing midnight, not UTC midnight'; n:=n+1;
  j := usapp_achievement_facts(ARRAY['2026-01-01T10:00:00Z'::timestamptz, '2026-01-03T10:00:00Z', '2026-01-03T11:00:00Z', '2026-01-04T10:00:00Z'], '2026-01-01T00:00:00Z');
  ASSERT (j->>'total_completed')::int = 4 AND (j->>'longest_streak')::int = 2 AND j->'badges'->3->>'earned_at' IS NULL, 'gaps and same-day dedup'; n:=n+1;
  j := usapp_achievement_facts(ARRAY(SELECT '2026-01-01T16:00:00Z'::timestamptz + g*interval '1 day' FROM generate_series(0,6) g) || ARRAY['2026-03-01T00:00:00Z'::timestamptz], '2026-01-01T00:00:00Z');
  ASSERT (j->>'longest_streak')::int = 7 AND j->'badges'->4->>'earned_at' = '2026-01-07T16:00:00.000000Z', 'later gap never revokes earned badge'; n:=n+1;
  ASSERT usapp_achievement_facts(ARRAY['2026-01-02T00:00:00Z'::timestamptz,'2026-01-01T00:00:00Z'], '2026-01-01T00:00:00Z') = usapp_achievement_facts(ARRAY['2026-01-01T00:00:00Z'::timestamptz,'2026-01-02T00:00:00Z'], '2026-01-01T00:00:00Z'), 'input order invariant'; n:=n+1;
  ASSERT usapp_achievement_facts(ARRAY[NULL::timestamptz], '2026-01-01T00:00:00Z')->>'total_completed' = '0', 'null source not completion'; n:=n+1;
  ASSERT NOT has_function_privilege('usapp_push', 'public.usapp_achievement_activate()', 'EXECUTE'), 'push cannot activate'; n:=n+1;
  ASSERT NOT has_function_privilege('usapp_api', 'public.usapp_achievement_activate()', 'EXECUTE'), 'API cannot activate'; n:=n+1;
  IF current_user = 'usapp_api' THEN
    BEGIN PERFORM usapp_achievement_activate(); RAISE EXCEPTION 'application activated'; EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
    BEGIN INSERT INTO achievement_activation DEFAULT VALUES; RAISE EXCEPTION 'application inserted'; EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
    BEGIN UPDATE achievement_activation SET enabled_at = now(); RAISE EXCEPTION 'application updated'; EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
    BEGIN DELETE FROM achievement_activation; RAISE EXCEPTION 'application deleted'; EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
    BEGIN TRUNCATE achievement_activation; RAISE EXCEPTION 'application truncated'; EXCEPTION WHEN insufficient_privilege THEN n:=n+1; END;
  ELSE
    a := usapp_achievement_activate(); b := usapp_achievement_activate();
    ASSERT a = b AND a IS NOT NULL, 'activation repeat preserves cutover'; n:=n+1;
    BEGIN INSERT INTO achievement_activation(enabled_at) VALUES ('2000-01-01T00:00:00Z'); RAISE EXCEPTION 'backdated'; EXCEPTION WHEN SQLSTATE 'AZ001' THEN n:=n+1; END;
    BEGIN UPDATE achievement_activation SET enabled_at = now(); RAISE EXCEPTION 'updated'; EXCEPTION WHEN SQLSTATE 'AZ001' THEN n:=n+1; END;
    BEGIN DELETE FROM achievement_activation; RAISE EXCEPTION 'deleted'; EXCEPTION WHEN SQLSTATE 'AZ001' THEN n:=n+1; END;
    BEGIN TRUNCATE achievement_activation; RAISE EXCEPTION 'truncated'; EXCEPTION WHEN SQLSTATE 'AZ001' THEN n:=n+1; END;
  END IF;
  RAISE NOTICE 'ACHIEVEMENT ASSERTIONS PASSED: %', n;
END $$;
ROLLBACK;
