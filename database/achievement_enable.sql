-- Owner-operated, deliberate one-time activation on an explicitly selected database.
-- Never run as part of migration application or API startup. Does not award currency.
BEGIN;
SELECT current_database() AS database, current_user AS maintenance_user;
SELECT public.usapp_achievement_activate() AS enabled_at;
COMMIT;
