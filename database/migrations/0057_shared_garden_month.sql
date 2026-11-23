BEGIN;

CREATE FUNCTION public.usapp_shared_garden_month(p_user uuid, p_month text)
RETURNS TABLE (requested_month text, current_month text, watering_date text, watered boolean, partner_watered boolean)
LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
    space_id uuid;
    today date := (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
    current_start date := date_trunc('month', today::timestamp)::date;
    first_day date;
    last_day date;
BEGIN
    SELECT member.couple_id INTO space_id FROM public.couple_members AS member WHERE member.user_id = p_user;
    IF NOT FOUND THEN RETURN; END IF;

    IF p_month IS NULL OR p_month = '' THEN
        first_day := current_start;
    ELSIF p_month ~ '^[0-9]{4}-(0[1-9]|1[0-2])$' AND left(p_month, 4) <> '0000' THEN
        first_day := make_date(left(p_month, 4)::integer, right(p_month, 2)::integer, 1);
    END IF;
    IF first_day IS NULL OR first_day > current_start THEN
        RETURN QUERY SELECT p_month, to_char(current_start, 'YYYY-MM'), NULL::text, false, false;
        RETURN;
    END IF;

    last_day := LEAST(today, (first_day + interval '1 month')::date - 1);
    RETURN QUERY
    SELECT to_char(first_day, 'YYYY-MM'), to_char(current_start, 'YYYY-MM'),
           to_char(first_day + offsets.day_offset, 'YYYY-MM-DD'), water.mine, water.partner
    FROM generate_series(0, last_day - first_day) AS offsets(day_offset)
    CROSS JOIN LATERAL (
        SELECT COALESCE(bool_or(w.user_id = p_user), false) AS mine,
               COALESCE(bool_or(w.user_id <> p_user), false) AS partner
        FROM public.shared_garden_waterings AS w
        WHERE w.couple_id = space_id AND w.watering_date = first_day + offsets.day_offset
    ) AS water
    ORDER BY offsets.day_offset;
END
$$;

REVOKE ALL ON FUNCTION public.usapp_shared_garden_month(uuid,text) FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_month(uuid,text) TO usapp_api;
COMMIT;
