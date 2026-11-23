BEGIN;

ALTER TABLE public.space_invites ADD COLUMN room_issuer_side text
    CHECK (room_issuer_side IN ('left', 'right'));

CREATE TABLE public.avatar_character_choices (
    user_id uuid PRIMARY KEY REFERENCES public.users(id),
    gender text NOT NULL CHECK (gender IN ('male', 'female'))
);

-- Standing order belongs to the room. Membership slots retain their original meaning.
CREATE TABLE public.room_standings (
    couple_id uuid PRIMARY KEY REFERENCES public.couples(id),
    reversed boolean NOT NULL
);
CREATE TABLE public.room_position_swaps (
    couple_id uuid NOT NULL,
    id uuid NOT NULL,
    requested_by uuid NOT NULL,
    status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'accept', 'decline', 'cancel')),
    PRIMARY KEY (couple_id, id),
    FOREIGN KEY (couple_id, requested_by) REFERENCES public.couple_members(couple_id, user_id)
);
CREATE UNIQUE INDEX room_position_one_pending ON public.room_position_swaps(couple_id) WHERE status = 'pending';

CREATE FUNCTION public.usapp_room_initialize_positions(p_user uuid, p_invite uuid)
RETURNS void LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    INSERT INTO public.room_standings(couple_id, reversed)
    SELECT i.couple_id, (COALESCE(i.room_issuer_side, 'left') = 'right') = (issuer.slot = 1)
    FROM public.space_invites i
    JOIN public.couple_members issuer ON (issuer.couple_id, issuer.user_id) = (i.couple_id, i.created_by_user_id)
    WHERE i.id = p_invite AND i.consumed_by_user_id = p_user AND i.consumed_at IS NOT NULL
    ON CONFLICT (couple_id) DO NOTHING
$$;

CREATE FUNCTION public.usapp_room_positions_read(p_user uuid)
RETURNS jsonb LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    SELECT jsonb_build_object(
        'side', CASE WHEN (m.slot = 1) <> COALESCE(s.reversed, false) THEN 'left' ELSE 'right' END,
        'paired', EXISTS (SELECT FROM public.couple_members other
                          WHERE other.couple_id = m.couple_id AND other.user_id <> m.user_id),
        'pending', CASE WHEN sw.id IS NOT NULL THEN jsonb_build_object(
            'id', sw.id, 'incoming', sw.requested_by <> m.user_id) END
    )
    FROM public.couple_members m
    LEFT JOIN public.room_standings s USING (couple_id)
    LEFT JOIN public.room_position_swaps sw ON sw.couple_id = m.couple_id AND sw.status = 'pending'
    WHERE m.user_id = p_user
$$;

CREATE FUNCTION public.usapp_room_positions_write(p_user uuid, p_id uuid, p_action text)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE room_id uuid; existing public.room_position_swaps%ROWTYPE;
BEGIN
    IF p_id IS NULL OR p_action IS NULL OR p_action NOT IN ('request', 'accept', 'decline', 'cancel') THEN RETURN 'invalid'; END IF;
    SELECT m.couple_id INTO room_id FROM public.couple_members m JOIN public.couples c ON c.id = m.couple_id
    WHERE m.user_id = p_user FOR UPDATE OF c;
    IF NOT FOUND THEN RETURN 'no_space'; END IF;
    IF NOT EXISTS (SELECT FROM public.couple_members WHERE couple_id = room_id AND user_id <> p_user) THEN
        RETURN 'position_conflict';
    END IF;
    SELECT * INTO existing FROM public.room_position_swaps WHERE couple_id = room_id AND id = p_id;
    IF p_action = 'request' THEN
        IF FOUND THEN
            IF existing.requested_by = p_user THEN RETURN 'replay'; END IF;
            RETURN 'position_conflict';
        END IF;
        IF EXISTS (SELECT FROM public.room_position_swaps WHERE couple_id = room_id AND status = 'pending') THEN
            RETURN 'position_conflict';
        END IF;
        INSERT INTO public.room_position_swaps(couple_id, id, requested_by) VALUES (room_id, p_id, p_user);
    ELSE
        IF NOT FOUND OR (p_action = 'cancel') <> (existing.requested_by = p_user) THEN RETURN 'position_conflict'; END IF;
        IF existing.status = p_action THEN RETURN 'replay'; END IF;
        IF existing.status <> 'pending' THEN RETURN 'position_conflict'; END IF;
        IF p_action = 'accept' THEN
            INSERT INTO public.room_standings AS s(couple_id, reversed) VALUES (room_id, true)
            ON CONFLICT (couple_id) DO UPDATE SET reversed = NOT s.reversed;
        END IF;
        UPDATE public.room_position_swaps SET status = p_action WHERE couple_id = room_id AND id = p_id;
    END IF;
    RETURN 'saved';
END
$$;

-- The existing loadout lock, fingerprint, version check, eligibility and audit remain authoritative.
-- Its legacy unqualified identifiers resolve in public before pg_temp.
REVOKE CREATE ON SCHEMA public FROM PUBLIC, usapp_api, usapp_push;
CREATE FUNCTION public.usapp_avatar_loadout_save_with_gender(
    p_user uuid, p_key text, p_fingerprint text, p_version integer, p_slots jsonb, p_gender text)
RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, public, pg_temp AS $$
DECLARE receipt jsonb;
BEGIN
    IF p_gender IS NOT NULL AND p_gender NOT IN ('male', 'female') THEN RETURN jsonb_build_object('error', 'invalid'); END IF;
    receipt := public.usapp_avatar_loadout_save(p_user, p_key, p_fingerprint, p_version, p_slots);
    IF receipt->>'result' = 'created' AND p_gender IS NOT NULL THEN
        INSERT INTO public.avatar_character_choices(user_id, gender) VALUES (p_user, p_gender)
        ON CONFLICT (user_id) DO UPDATE SET gender = EXCLUDED.gender;
    END IF;
    RETURN receipt;
END
$$;

REVOKE ALL ON public.avatar_character_choices, public.room_standings, public.room_position_swaps FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON public.avatar_character_choices TO usapp_api;
REVOKE ALL ON FUNCTION public.usapp_room_initialize_positions(uuid,uuid), public.usapp_room_positions_read(uuid),
    public.usapp_room_positions_write(uuid,uuid,text), public.usapp_avatar_loadout_save_with_gender(uuid,text,text,integer,jsonb,text)
    FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_room_initialize_positions(uuid,uuid), public.usapp_room_positions_read(uuid),
    public.usapp_room_positions_write(uuid,uuid,text), public.usapp_avatar_loadout_save_with_gender(uuid,text,text,integer,jsonb,text)
    TO usapp_api;
COMMIT;
