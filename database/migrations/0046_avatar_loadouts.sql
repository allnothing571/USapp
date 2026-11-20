-- =====================================================================
-- 0046_avatar_loadouts.sql
-- Phase 4 B3: saved avatar loadouts and equip rules (docs/phase4-avatar-shop-design.md Section 7).
--
-- Additive. No wallet, purchase or catalog table changes; no item grant.
--   avatar_loadout_events  one append-only row per saved whole loadout, versioned per user from 1; the latest is current.
--                          Resolved item ids are stored, so a replay never substitutes a later default.
-- Lock order: loadout user 33031 -> starter configuration 33028 (shared, only when a required slot falls back).
-- SQLSTATE: UA007 loadout guard. UA001 append-only (0044).
-- =====================================================================

BEGIN;

DO $avatar_loadout_preflight$
BEGIN
    IF to_regclass('public.avatar_purchases') IS NULL THEN
        RAISE EXCEPTION '0046_avatar_loadouts requires 0045_avatar_purchases';
    END IF;
END
$avatar_loadout_preflight$;

CREATE TABLE avatar_loadout_events (
    id                   uuid        NOT NULL DEFAULT gen_random_uuid(),
    user_id              uuid        NOT NULL,
    couple_id            uuid        NOT NULL,
    version              integer     NOT NULL,
    hair_item_id         uuid        NOT NULL,
    clothes_item_id      uuid        NOT NULL,
    expression_item_id   uuid        NOT NULL,
    hat_item_id          uuid,
    handheld_item_id     uuid,
    accessory_item_id    uuid,
    requested            jsonb       NOT NULL,
    starter_version      integer,
    idempotency_key      text        NOT NULL,
    request_fingerprint  text        NOT NULL,
    created_at           timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_loadout_events_pkey PRIMARY KEY (id),
    CONSTRAINT avatar_loadout_events_version_unique UNIQUE (user_id, version),
    CONSTRAINT avatar_loadout_events_key_unique UNIQUE (user_id, idempotency_key),
    CONSTRAINT avatar_loadout_events_member_fk FOREIGN KEY (couple_id, user_id) REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_hair_fk FOREIGN KEY (hair_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_clothes_fk FOREIGN KEY (clothes_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_expression_fk FOREIGN KEY (expression_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_hat_fk FOREIGN KEY (hat_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_handheld_fk FOREIGN KEY (handheld_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_accessory_fk FOREIGN KEY (accessory_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_starter_fk FOREIGN KEY (starter_version) REFERENCES avatar_starter_configs (version) ON DELETE RESTRICT,
    CONSTRAINT avatar_loadout_events_version_positive CHECK (version >= 1),
    CONSTRAINT avatar_loadout_events_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT avatar_loadout_events_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX avatar_loadout_events_latest_idx ON avatar_loadout_events (user_id, version DESC);

-- An item may fill a slot when it has the slot's category and is a starter or the user's own purchase.
CREATE FUNCTION usapp_avatar_equippable(p_user uuid, p_item uuid, p_category text) RETURNS boolean
LANGUAGE sql STABLE AS $$
    SELECT EXISTS (SELECT 1 FROM avatar_items i WHERE i.id = p_item AND i.category = p_category
                      AND (i.starter OR EXISTS (SELECT 1 FROM avatar_purchases p WHERE p.user_id = p_user AND p.item_id = i.id)))
$$;

-- Exactly the six slot keys, each a uuid string or null.
CREATE FUNCTION usapp_avatar_requested_valid(r jsonb) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $$
    SELECT jsonb_typeof(r) = 'object'
       AND (SELECT array_agg(k ORDER BY k) FROM jsonb_object_keys(r) k)
           = ARRAY['accessory', 'clothes', 'expression', 'hair', 'handheld', 'hat']
       AND NOT EXISTS (SELECT 1 FROM jsonb_each(r) e
                        WHERE jsonb_typeof(e.value) <> 'null'
                          AND (jsonb_typeof(e.value) <> 'string'
                               OR (e.value #>> '{}') !~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'))
$$;

ALTER TABLE avatar_loadout_events
    ADD CONSTRAINT avatar_loadout_events_requested_shape CHECK (usapp_avatar_requested_valid(requested));

CREATE FUNCTION usapp_avatar_loadout_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
DECLARE prev integer; s avatar_starter_configs%ROWTYPE; needs_default boolean;
BEGIN
    IF NEW.user_id IS NULL OR NEW.version IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33031::integer, hashtext(NEW.user_id::text));
    SELECT max(version) INTO prev FROM avatar_loadout_events WHERE user_id = NEW.user_id;
    IF NEW.version <> COALESCE(prev, 0) + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA007', MESSAGE = 'USAPP_AVATAR_LOADOUT_VERSION';
    END IF;
    IF NOT usapp_avatar_equippable(NEW.user_id, NEW.hair_item_id, 'hair')
       OR NOT usapp_avatar_equippable(NEW.user_id, NEW.clothes_item_id, 'clothes')
       OR NOT usapp_avatar_equippable(NEW.user_id, NEW.expression_item_id, 'expression')
       OR (NEW.hat_item_id IS NOT NULL AND NOT usapp_avatar_equippable(NEW.user_id, NEW.hat_item_id, 'hat'))
       OR (NEW.handheld_item_id IS NOT NULL AND NOT usapp_avatar_equippable(NEW.user_id, NEW.handheld_item_id, 'handheld'))
       OR (NEW.accessory_item_id IS NOT NULL AND NOT usapp_avatar_equippable(NEW.user_id, NEW.accessory_item_id, 'accessory')) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA007', MESSAGE = 'USAPP_AVATAR_LOADOUT_ITEM';
    END IF;
    -- Requested and resolved agree: a given id is stored as given; an empty optional slot stays empty; a null required slot is
    -- the recorded configuration's default and nothing else.
    IF NEW.hat_item_id IS DISTINCT FROM (NEW.requested ->> 'hat')::uuid
       OR NEW.handheld_item_id IS DISTINCT FROM (NEW.requested ->> 'handheld')::uuid
       OR NEW.accessory_item_id IS DISTINCT FROM (NEW.requested ->> 'accessory')::uuid
       OR ((NEW.requested ->> 'hair') IS NOT NULL AND NEW.hair_item_id IS DISTINCT FROM (NEW.requested ->> 'hair')::uuid)
       OR ((NEW.requested ->> 'clothes') IS NOT NULL AND NEW.clothes_item_id IS DISTINCT FROM (NEW.requested ->> 'clothes')::uuid)
       OR ((NEW.requested ->> 'expression') IS NOT NULL AND NEW.expression_item_id IS DISTINCT FROM (NEW.requested ->> 'expression')::uuid) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA007', MESSAGE = 'USAPP_AVATAR_LOADOUT_REQUEST';
    END IF;
    needs_default := (NEW.requested ->> 'hair') IS NULL OR (NEW.requested ->> 'clothes') IS NULL OR (NEW.requested ->> 'expression') IS NULL;
    IF needs_default IS DISTINCT FROM (NEW.starter_version IS NOT NULL) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA007', MESSAGE = 'USAPP_AVATAR_LOADOUT_DEFAULT';
    END IF;
    IF needs_default THEN
        SELECT * INTO s FROM avatar_starter_configs WHERE version = NEW.starter_version;
        IF NOT FOUND
           OR ((NEW.requested ->> 'hair') IS NULL AND NEW.hair_item_id IS DISTINCT FROM s.hair_item_id)
           OR ((NEW.requested ->> 'clothes') IS NULL AND NEW.clothes_item_id IS DISTINCT FROM s.clothes_item_id)
           OR ((NEW.requested ->> 'expression') IS NULL AND NEW.expression_item_id IS DISTINCT FROM s.expression_item_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UA007', MESSAGE = 'USAPP_AVATAR_LOADOUT_DEFAULT';
        END IF;
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_loadout_events_guard BEFORE INSERT ON avatar_loadout_events
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_loadout_guard();
CREATE TRIGGER avatar_loadout_events_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_loadout_events
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();

-- ---------------------------------------------------------------- the save
-- Answers {"result":"created"|"replay","event_id":...} or {"error": ...}. Refusals write nothing; the caller rolls back.
CREATE FUNCTION usapp_avatar_loadout_save(p_user uuid, p_key text, p_fingerprint text, p_expected integer, p_requested jsonb)
RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE k avatar_loadout_events%ROWTYPE; space uuid; cur integer; s avatar_starter_configs%ROWTYPE; starter integer;
        resolved jsonb; slot text; item uuid; owned boolean; new_id uuid := gen_random_uuid();
BEGIN
    IF NOT usapp_avatar_requested_valid(p_requested) THEN
        RETURN jsonb_build_object('error', 'invalid');
    END IF;
    -- 1-2. The user's loadout lock, then classification before any membership, version, starter or ownership read.
    PERFORM pg_advisory_xact_lock(33031::integer, hashtext(p_user::text));
    SELECT * INTO k FROM avatar_loadout_events WHERE user_id = p_user AND idempotency_key = p_key;
    IF FOUND THEN
        IF k.request_fingerprint = p_fingerprint THEN
            RETURN jsonb_build_object('result', 'replay', 'event_id', k.id);
        END IF;
        RETURN jsonb_build_object('error', 'key_reused');
    END IF;
    -- 3. Membership, as purchases.
    SELECT m.couple_id INTO space FROM couple_members m WHERE m.user_id = p_user FOR SHARE;
    IF NOT FOUND THEN
        RETURN jsonb_build_object('error', 'no_space');
    END IF;
    -- 4. Version.
    SELECT COALESCE(max(version), 0) INTO cur FROM avatar_loadout_events WHERE user_id = p_user;
    IF cur <> p_expected THEN
        RETURN jsonb_build_object('error', 'conflict', 'current_version', cur);
    END IF;
    -- 5. Fallback from one configuration, only when a required slot is null.
    resolved := p_requested;
    IF (p_requested ->> 'hair') IS NULL OR (p_requested ->> 'clothes') IS NULL OR (p_requested ->> 'expression') IS NULL THEN
        PERFORM pg_advisory_xact_lock_shared(33028::integer, 0);
        SELECT * INTO s FROM avatar_starter_configs ORDER BY version DESC LIMIT 1;
        IF NOT FOUND THEN
            RETURN jsonb_build_object('error', 'defaults_unavailable');
        END IF;
        starter := s.version;
        IF (p_requested ->> 'hair') IS NULL THEN resolved := jsonb_set(resolved, '{hair}', to_jsonb(s.hair_item_id::text)); END IF;
        IF (p_requested ->> 'clothes') IS NULL THEN resolved := jsonb_set(resolved, '{clothes}', to_jsonb(s.clothes_item_id::text)); END IF;
        IF (p_requested ->> 'expression') IS NULL THEN resolved := jsonb_set(resolved, '{expression}', to_jsonb(s.expression_item_id::text)); END IF;
    END IF;
    -- 6. Eligibility of every filled slot.
    FOREACH slot IN ARRAY ARRAY['hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory'] LOOP
        item := (resolved ->> slot)::uuid;
        CONTINUE WHEN item IS NULL;
        IF NOT EXISTS (SELECT 1 FROM avatar_items WHERE id = item) THEN
            RETURN jsonb_build_object('error', 'not_found', 'slot', slot);
        END IF;
        IF NOT EXISTS (SELECT 1 FROM avatar_items WHERE id = item AND category = slot) THEN
            RETURN jsonb_build_object('error', 'slot_invalid', 'slot', slot);
        END IF;
        IF NOT usapp_avatar_equippable(p_user, item, slot) THEN
            RETURN jsonb_build_object('error', 'not_owned', 'slot', slot);
        END IF;
    END LOOP;
    -- 7. The event and its audit row.
    INSERT INTO avatar_loadout_events (id, user_id, couple_id, version, hair_item_id, clothes_item_id, expression_item_id,
                                       hat_item_id, handheld_item_id, accessory_item_id, requested, starter_version,
                                       idempotency_key, request_fingerprint)
        VALUES (new_id, p_user, space, cur + 1, (resolved ->> 'hair')::uuid, (resolved ->> 'clothes')::uuid,
                (resolved ->> 'expression')::uuid, (resolved ->> 'hat')::uuid, (resolved ->> 'handheld')::uuid,
                (resolved ->> 'accessory')::uuid, p_requested, starter, p_key, p_fingerprint);
    INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail)
        VALUES ('user', p_user, space, 'avatar.loadout_saved', 'avatar_loadout', new_id::text,
                jsonb_build_object('version', cur + 1, 'starter_version', starter));
    RETURN jsonb_build_object('result', 'created', 'event_id', new_id);
END
$$;

-- ---------------------------------------------------------------- privileges
REVOKE ALL ON avatar_loadout_events FROM PUBLIC;
REVOKE ALL ON avatar_loadout_events FROM usapp_api, usapp_push;
GRANT SELECT, INSERT ON avatar_loadout_events TO usapp_api;

COMMIT;
