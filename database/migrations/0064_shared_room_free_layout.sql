BEGIN;

CREATE OR REPLACE FUNCTION public.usapp_shared_room_save(p_user uuid, p_version integer, p_layout jsonb)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE room_couple uuid;
BEGIN
    SELECT couple_id INTO room_couple FROM public.couple_members WHERE user_id = p_user;
    IF NOT FOUND THEN RETURN 'no_space'; END IF;
    IF p_version IS NULL OR p_version NOT BETWEEN 0 AND 999999999 OR
       jsonb_typeof(p_layout) IS DISTINCT FROM 'array' THEN RETURN 'invalid'; END IF;
    IF jsonb_array_length(p_layout) > 7 OR EXISTS (
        SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
        WHERE CASE
            WHEN jsonb_typeof(item) <> 'object' THEN true
            WHEN NOT (item ?& ARRAY['item', 'x', 'y'])
              OR item - ARRAY['item', 'x', 'y', 'flip'] <> '{}'::jsonb
              OR jsonb_typeof(item->'item') <> 'string'
              OR jsonb_typeof(item->'x') <> 'number'
              OR jsonb_typeof(item->'y') <> 'number'
              OR (item ? 'flip' AND jsonb_typeof(item->'flip') <> 'boolean') THEN true
            WHEN item->>'item' NOT IN ('table', 'lamp', 'plant', 'rug') AND NOT EXISTS (
                SELECT 1 FROM public.room_furniture_purchases p
                WHERE p.couple_id = room_couple AND p.item = entries.item->>'item') THEN true
            WHEN (item->'x')::numeric <> trunc((item->'x')::numeric)
              OR (item->'x')::numeric NOT BETWEEN 0 AND 1000
              OR (item->'y')::numeric <> trunc((item->'y')::numeric)
              OR (item->'y')::numeric NOT BETWEEN 0 AND 1000 THEN true
            ELSE false END
    ) THEN RETURN 'invalid'; END IF;
    IF EXISTS (SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
               GROUP BY item->>'item' HAVING count(*) > 1) OR
       EXISTS (SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
               WHERE item->>'item' <> 'rug'
               GROUP BY (item->'x')::numeric, (item->'y')::numeric HAVING count(*) > 1) THEN RETURN 'invalid'; END IF;

    IF p_version = 0 THEN
        INSERT INTO public.shared_rooms(couple_id, version, layout)
        VALUES (room_couple, 1, p_layout)
        ON CONFLICT (couple_id) DO NOTHING;
    ELSE
        UPDATE public.shared_rooms
        SET version = p_version + 1, layout = p_layout
        WHERE couple_id = room_couple AND version = p_version;
    END IF;
    IF FOUND THEN RETURN 'saved'; END IF;
    RETURN 'conflict';
END
$$;

REVOKE ALL ON FUNCTION public.usapp_shared_room_save(uuid, integer, jsonb) FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_shared_room_save(uuid, integer, jsonb) TO usapp_api;

COMMIT;
