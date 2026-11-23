BEGIN;

ALTER TABLE public.avatar_character_choices
    ALTER COLUMN gender DROP NOT NULL,
    ADD COLUMN hair_hidden boolean;

-- The equipped item is retained while hidden. The same loadout event and lock
-- cover both the visible style choice and the six equipment slots.
CREATE FUNCTION public.usapp_avatar_loadout_save_with_style(
    p_user uuid, p_key text, p_fingerprint text, p_version integer,
    p_slots jsonb, p_gender text, p_hair_hidden boolean)
RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE receipt jsonb;
BEGIN
    receipt := public.usapp_avatar_loadout_save_with_gender(p_user, p_key, p_fingerprint, p_version, p_slots, p_gender);
    IF receipt->>'result' = 'created' AND p_hair_hidden IS NOT NULL THEN
        INSERT INTO public.avatar_character_choices(user_id, hair_hidden) VALUES (p_user, p_hair_hidden)
        ON CONFLICT (user_id) DO UPDATE SET hair_hidden = EXCLUDED.hair_hidden;
    END IF;
    RETURN receipt;
END
$$;

REVOKE ALL ON FUNCTION public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean)
    FROM PUBLIC, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean) TO usapp_api;
COMMIT;
