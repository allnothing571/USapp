BEGIN;

CREATE FUNCTION public.usapp_shared_garden_recent(p_user uuid)
RETURNS TABLE (watering_date text, watered boolean, partner_watered boolean)
LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    SELECT to_char(day.today - offsets.days_ago, 'YYYY-MM-DD'), water.mine, water.partner
    FROM public.couple_members AS member
    CROSS JOIN (SELECT (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS today) AS day
    CROSS JOIN generate_series(0, 6) AS offsets(days_ago)
    CROSS JOIN LATERAL (
        SELECT COALESCE(bool_or(w.user_id = p_user), false) AS mine,
               COALESCE(bool_or(w.user_id <> p_user), false) AS partner
        FROM public.shared_garden_waterings AS w
        WHERE w.couple_id = member.couple_id
          AND w.watering_date = day.today - offsets.days_ago
    ) AS water
    WHERE member.user_id = p_user
    ORDER BY offsets.days_ago
$$;

REVOKE ALL ON FUNCTION public.usapp_shared_garden_recent(uuid) FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_recent(uuid) TO usapp_api;
COMMIT;
