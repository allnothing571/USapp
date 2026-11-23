BEGIN;

CREATE TABLE public.shared_garden_waterings (
    couple_id uuid NOT NULL,
    watering_date date NOT NULL,
    user_id uuid NOT NULL,
    PRIMARY KEY (couple_id, watering_date, user_id),
    FOREIGN KEY (couple_id, user_id) REFERENCES public.couple_members (couple_id, user_id)
);

CREATE FUNCTION public.usapp_shared_garden_read(p_user uuid)
RETURNS TABLE (watering_date text, total_waterings bigint, watered_today boolean, partner_watered_today boolean)
LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    SELECT to_char(day.today, 'YYYY-MM-DD'), water.total, water.mine, water.partner
    FROM public.couple_members AS member
    CROSS JOIN (SELECT (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date AS today) AS day
    CROSS JOIN LATERAL (
        SELECT count(*) AS total,
               COALESCE(bool_or(w.user_id = p_user AND w.watering_date = day.today), false) AS mine,
               COALESCE(bool_or(w.user_id <> p_user AND w.watering_date = day.today), false) AS partner
        FROM public.shared_garden_waterings AS w
        WHERE w.couple_id = member.couple_id
    ) AS water
    WHERE member.user_id = p_user
$$;

CREATE FUNCTION public.usapp_shared_garden_water(p_user uuid, p_date text)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
    space_id uuid;
    today date := (statement_timestamp() AT TIME ZONE 'Asia/Shanghai')::date;
BEGIN
    SELECT couple_id INTO space_id FROM public.couple_members WHERE user_id = p_user;
    IF NOT FOUND THEN RETURN 'no_space'; END IF;
    IF p_date IS DISTINCT FROM to_char(today, 'YYYY-MM-DD') THEN RETURN 'date_changed'; END IF;

    INSERT INTO public.shared_garden_waterings (couple_id, watering_date, user_id)
    VALUES (space_id, today, p_user)
    ON CONFLICT (couple_id, watering_date, user_id) DO NOTHING;
    RETURN 'watered';
END
$$;

REVOKE ALL ON public.shared_garden_waterings FROM PUBLIC, usapp_api, usapp_push;
REVOKE ALL ON FUNCTION public.usapp_shared_garden_read(uuid), public.usapp_shared_garden_water(uuid,text)
    FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_read(uuid), public.usapp_shared_garden_water(uuid,text)
    TO usapp_api;
COMMIT;
