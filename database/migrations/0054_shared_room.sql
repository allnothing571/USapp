-- V2 first room: one layout per couple, with independent per-member avatar consent.
BEGIN;

CREATE TABLE public.shared_rooms (
    couple_id uuid PRIMARY KEY REFERENCES public.couples (id),
    version integer NOT NULL CHECK (version BETWEEN 1 AND 1000000000),
    layout jsonb NOT NULL
);
CREATE TABLE public.shared_room_avatar_choices (
    couple_id uuid NOT NULL,
    user_id uuid NOT NULL,
    shared boolean NOT NULL,
    PRIMARY KEY (couple_id, user_id),
    FOREIGN KEY (couple_id, user_id) REFERENCES public.couple_members (couple_id, user_id)
);

CREATE FUNCTION public.usapp_shared_room_read(p_user uuid)
RETURNS jsonb LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    SELECT jsonb_build_object(
        'version', COALESCE(room.version, 0),
        'layout', COALESCE(room.layout, '[]'::jsonb),
        'share_avatar', COALESCE(mine.shared, false),
        'partner_id', CASE WHEN theirs.shared THEN partner.user_id ELSE NULL END
    )
    FROM public.couple_members member
    LEFT JOIN public.shared_rooms room ON room.couple_id = member.couple_id
    LEFT JOIN public.shared_room_avatar_choices mine ON mine.couple_id = member.couple_id AND mine.user_id = member.user_id
    LEFT JOIN public.couple_members partner ON partner.couple_id = member.couple_id AND partner.user_id <> member.user_id
    LEFT JOIN public.shared_room_avatar_choices theirs ON theirs.couple_id = partner.couple_id AND theirs.user_id = partner.user_id
    WHERE member.user_id = p_user
$$;

CREATE FUNCTION public.usapp_shared_room_save(p_user uuid, p_version integer, p_layout jsonb)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE room_couple uuid;
BEGIN
    SELECT couple_id INTO room_couple FROM public.couple_members WHERE user_id = p_user;
    IF NOT FOUND THEN RETURN 'no_space'; END IF;
    IF p_version IS NULL OR p_version NOT BETWEEN 0 AND 999999999 OR
       jsonb_typeof(p_layout) IS DISTINCT FROM 'array' THEN RETURN 'invalid'; END IF;
    IF jsonb_array_length(p_layout) > 4 OR EXISTS (
        SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
        WHERE CASE WHEN jsonb_typeof(item) <> 'object' THEN true ELSE
           NOT (item ?& ARRAY['item', 'x', 'y'])
           OR item - ARRAY['item', 'x', 'y'] <> '{}'::jsonb
           OR item->>'item' NOT IN ('table', 'lamp', 'plant', 'rug')
           OR jsonb_typeof(item->'item') <> 'string'
           OR jsonb_typeof(item->'x') <> 'number' OR item->>'x' NOT IN ('0','1','2','3')
           OR jsonb_typeof(item->'y') <> 'number' OR item->>'y' NOT IN ('0','1','2') END
    ) THEN RETURN 'invalid'; END IF;
    IF EXISTS (SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
               GROUP BY item->>'item' HAVING count(*) > 1) OR
       EXISTS (SELECT 1 FROM jsonb_array_elements(p_layout) AS entries(item)
               GROUP BY item->>'x', item->>'y' HAVING count(*) > 1) THEN RETURN 'invalid'; END IF;

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

CREATE FUNCTION public.usapp_shared_room_share(p_user uuid, p_shared boolean)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
    IF p_shared IS NULL THEN RETURN 'invalid'; END IF;
    INSERT INTO public.shared_room_avatar_choices(couple_id, user_id, shared)
    SELECT couple_id, user_id, p_shared FROM public.couple_members WHERE user_id = p_user
    ON CONFLICT (couple_id, user_id) DO UPDATE SET shared = EXCLUDED.shared;
    IF FOUND THEN RETURN 'saved'; END IF;
    RETURN 'no_space';
END
$$;

REVOKE ALL ON TABLE public.shared_rooms, public.shared_room_avatar_choices FROM PUBLIC, usapp_api, usapp_push;
REVOKE ALL ON FUNCTION public.usapp_shared_room_read(uuid), public.usapp_shared_room_save(uuid, integer, jsonb),
    public.usapp_shared_room_share(uuid, boolean) FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_shared_room_read(uuid), public.usapp_shared_room_save(uuid, integer, jsonb),
    public.usapp_shared_room_share(uuid, boolean) TO usapp_api;
COMMIT;
