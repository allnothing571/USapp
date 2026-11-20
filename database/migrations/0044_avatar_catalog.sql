-- =====================================================================
-- 0044_avatar_catalog.sql
-- Phase 4 B1: the global avatar catalog and starter appearance (docs/phase4-avatar-shop-design.md).
--
-- Additive. No wallet change, ownership, purchase or equip table. No data is written.
--   avatar_assets           approved registry of bundled, versioned asset files; read-only to usapp_api,
--                           written only by the maintenance role after art review.
--   avatar_items            one row per item; category, starter flag and appearance family never change.
--   avatar_item_revisions   name, asset, price and listing, append-only and versioned per item.
--   avatar_starter_configs  append-only global versions naming one default per required slot.
-- Every row is append-only; items are never deleted.
--
-- Locks: 33027 one item's revisions; 33028 the starter configuration; 33029 one admin's idempotency key.
-- SQLSTATE: UA001 append-only; UA002 registry; UA003 item and revision; UA004 starter configuration.
-- The database cannot tell which listener a write came from: only the application's admin boundary
-- does (design Section 2.6).
-- =====================================================================

BEGIN;

DO $avatar_preflight$
BEGIN
    IF to_regclass('public.admin_users') IS NULL OR to_regclass('public.audit_logs') IS NULL THEN
        RAISE EXCEPTION '0044_avatar_catalog requires 0005_admin_audit';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = 'public.notifications'::regclass
                    AND attname = 'album_batch_id' AND NOT attisdropped) THEN
        RAISE EXCEPTION '0044_avatar_catalog requires 0043_phase3_notifications';
    END IF;
END
$avatar_preflight$;

-- ---------------------------------------------------------------- shared shapes
CREATE FUNCTION usapp_avatar_category_valid(c text) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $$
    SELECT c IN ('hair', 'clothes', 'expression', 'hat', 'handheld', 'accessory')
$$;

-- Exactly the category's layer set, each with a lowercase SHA-256 of its file bytes.
CREATE FUNCTION usapp_avatar_layers_valid(c text, layers jsonb) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $$
    SELECT jsonb_typeof(layers) = 'object'
       AND (SELECT array_agg(k ORDER BY k) FROM jsonb_object_keys(layers) k)
           = CASE WHEN c = 'hair' THEN ARRAY['back', 'front', 'front_under_hat'] ELSE ARRAY['main'] END
       AND NOT EXISTS (SELECT 1 FROM jsonb_each(layers) e
                        WHERE jsonb_typeof(e.value) <> 'string' OR (e.value #>> '{}') !~ '^[0-9a-f]{64}$')
$$;

CREATE FUNCTION usapp_avatar_reject_mutation() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UA001', MESSAGE = 'USAPP_AVATAR_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$$;

-- An actor that is an active local administrator.
CREATE FUNCTION usapp_avatar_admin_active(a uuid) RETURNS boolean
LANGUAGE sql STABLE AS $$
    SELECT EXISTS (SELECT 1 FROM admin_users u WHERE u.id = a AND u.status = 'active')
$$;

-- ---------------------------------------------------------------- the registry
-- A bundled file is named <asset_key>.<layer>.<format>; its bytes are never overwritten.
-- A correction is a new revision of the same family.
CREATE TABLE avatar_assets (
    asset_key          text        NOT NULL,
    category           text        NOT NULL,
    family             text        NOT NULL,
    revision           integer     NOT NULL,
    format             text        NOT NULL,
    layers             jsonb       NOT NULL,
    covers_hair_top    boolean,
    accessory_position text,
    created_at         timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_assets_pkey PRIMARY KEY (asset_key),
    CONSTRAINT avatar_assets_family_revision_unique UNIQUE (family, revision),
    CONSTRAINT avatar_assets_category_valid CHECK (usapp_avatar_category_valid(category)),
    CONSTRAINT avatar_assets_family_shape CHECK (
        family ~ '^[a-z]+\.[a-z0-9_]{1,40}$' AND split_part(family, '.', 1) = category),
    CONSTRAINT avatar_assets_revision_range CHECK (revision BETWEEN 1 AND 999),
    CONSTRAINT avatar_assets_key_shape CHECK (asset_key = family || '.v' || revision::text),
    CONSTRAINT avatar_assets_format_valid CHECK (format IN ('svg', 'webp')),
    CONSTRAINT avatar_assets_layers_valid CHECK (usapp_avatar_layers_valid(category, layers)),
    CONSTRAINT avatar_assets_hat_hint CHECK ((covers_hair_top IS NOT NULL) = (category = 'hat')),
    CONSTRAINT avatar_assets_accessory_hint CHECK (
        (accessory_position IS NOT NULL) = (category = 'accessory')
        AND (accessory_position IS NULL OR accessory_position IN ('behind_body', 'neck', 'face', 'above_hat')))
);

-- Revisions are contiguous; a family keeps its category, format and placement hints.
CREATE FUNCTION usapp_avatar_asset_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
DECLARE prev avatar_assets%ROWTYPE;
BEGIN
    IF NEW.family IS NULL OR NEW.revision IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33027::integer, hashtext('asset:' || NEW.family));
    SELECT * INTO prev FROM avatar_assets WHERE family = NEW.family ORDER BY revision DESC LIMIT 1;
    IF NOT FOUND THEN
        IF NEW.revision <> 1 THEN
            RAISE EXCEPTION USING ERRCODE = 'UA002', MESSAGE = 'USAPP_AVATAR_ASSET_REVISION';
        END IF;
        RETURN NEW;
    END IF;
    IF NEW.revision <> prev.revision + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA002', MESSAGE = 'USAPP_AVATAR_ASSET_REVISION';
    END IF;
    IF ROW(NEW.category, NEW.format, NEW.covers_hair_top, NEW.accessory_position)
       IS DISTINCT FROM ROW(prev.category, prev.format, prev.covers_hair_top, prev.accessory_position) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA002', MESSAGE = 'USAPP_AVATAR_ASSET_FAMILY';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_assets_guard BEFORE INSERT ON avatar_assets
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_asset_guard();

-- ---------------------------------------------------------------- items and revisions
CREATE TABLE avatar_items (
    id         uuid        NOT NULL DEFAULT gen_random_uuid(),
    category   text        NOT NULL,
    starter    boolean     NOT NULL,
    family     text        NOT NULL,
    created_by uuid        NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_items_pkey PRIMARY KEY (id),
    -- One item per appearance family: a different appearance is a different item.
    CONSTRAINT avatar_items_family_unique UNIQUE (family),
    CONSTRAINT avatar_items_category_valid CHECK (usapp_avatar_category_valid(category)),
    CONSTRAINT avatar_items_family_category CHECK (split_part(family, '.', 1) = category),
    CONSTRAINT avatar_items_creator_fk FOREIGN KEY (created_by) REFERENCES admin_users (id) ON DELETE RESTRICT
);
CREATE INDEX avatar_items_category_idx ON avatar_items (category, created_at, id);

CREATE TABLE avatar_item_revisions (
    item_id             uuid        NOT NULL,
    version             integer     NOT NULL,
    name                text        NOT NULL,
    asset_key           text        NOT NULL,
    price_sunshine      integer,
    listed              boolean     NOT NULL,
    admin_user_id       uuid        NOT NULL,
    idempotency_key     text        NOT NULL,
    request_fingerprint text        NOT NULL,
    created_at          timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_item_revisions_pkey PRIMARY KEY (item_id, version),
    CONSTRAINT avatar_item_revisions_key_unique UNIQUE (admin_user_id, idempotency_key),
    CONSTRAINT avatar_item_revisions_item_fk FOREIGN KEY (item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_item_revisions_asset_fk FOREIGN KEY (asset_key) REFERENCES avatar_assets (asset_key) ON DELETE RESTRICT,
    CONSTRAINT avatar_item_revisions_admin_fk FOREIGN KEY (admin_user_id) REFERENCES admin_users (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_item_revisions_version_positive CHECK (version >= 1),
    CONSTRAINT avatar_item_revisions_name_shape CHECK (
        char_length(name) BETWEEN 1 AND 20 AND name = btrim(name) AND name !~ '[\x01-\x1f\x7f-\x9f]'),
    CONSTRAINT avatar_item_revisions_price_range CHECK (price_sunshine IS NULL OR price_sunshine BETWEEN 1 AND 9999),
    CONSTRAINT avatar_item_revisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT avatar_item_revisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_avatar_item_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    IF NEW.created_by IS NULL THEN RETURN NEW; END IF;
    IF NOT usapp_avatar_admin_active(NEW.created_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_ADMIN';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_items_guard BEFORE INSERT ON avatar_items
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_item_guard();

-- Contiguous versions under the item lock; the asset stays in the item's family and never moves back;
-- a starter has no price and a paid item always has one.
CREATE FUNCTION usapp_avatar_revision_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
DECLARE i avatar_items%ROWTYPE; a avatar_assets%ROWTYPE; prev_version integer; prev_asset integer;
BEGIN
    IF NEW.item_id IS NULL OR NEW.version IS NULL OR NEW.asset_key IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33027::integer, hashtext('item:' || NEW.item_id::text));
    SELECT * INTO i FROM avatar_items WHERE id = NEW.item_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_ITEM';
    END IF;
    IF NOT usapp_avatar_admin_active(NEW.admin_user_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_ADMIN';
    END IF;
    SELECT r.version, s.revision INTO prev_version, prev_asset
      FROM avatar_item_revisions r JOIN avatar_assets s ON s.asset_key = r.asset_key
     WHERE r.item_id = NEW.item_id ORDER BY r.version DESC LIMIT 1;
    IF NEW.version <> COALESCE(prev_version, 0) + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_VERSION';
    END IF;
    SELECT * INTO a FROM avatar_assets WHERE asset_key = NEW.asset_key;
    IF NOT FOUND OR a.category IS DISTINCT FROM i.category OR a.family IS DISTINCT FROM i.family
       OR (prev_asset IS NOT NULL AND a.revision < prev_asset) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_ASSET';
    END IF;
    IF (NEW.price_sunshine IS NULL) IS DISTINCT FROM i.starter THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_PRICE';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_item_revisions_guard BEFORE INSERT ON avatar_item_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_revision_guard();

-- Every item commits with its first revision.
CREATE FUNCTION usapp_avatar_item_has_revision() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM avatar_item_revisions r WHERE r.item_id = NEW.id AND r.version = 1) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA003', MESSAGE = 'USAPP_AVATAR_ITEM_WITHOUT_REVISION';
    END IF;
    RETURN NULL;
END
$$;

CREATE CONSTRAINT TRIGGER avatar_items_require_revision AFTER INSERT ON avatar_items
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_item_has_revision();

-- ---------------------------------------------------------------- starter configuration
CREATE TABLE avatar_starter_configs (
    version             integer     NOT NULL,
    hair_item_id        uuid        NOT NULL,
    clothes_item_id     uuid        NOT NULL,
    expression_item_id  uuid        NOT NULL,
    admin_user_id       uuid        NOT NULL,
    idempotency_key     text        NOT NULL,
    request_fingerprint text        NOT NULL,
    created_at          timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_starter_configs_pkey PRIMARY KEY (version),
    CONSTRAINT avatar_starter_configs_key_unique UNIQUE (admin_user_id, idempotency_key),
    CONSTRAINT avatar_starter_configs_hair_fk FOREIGN KEY (hair_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_starter_configs_clothes_fk FOREIGN KEY (clothes_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_starter_configs_expression_fk FOREIGN KEY (expression_item_id) REFERENCES avatar_items (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_starter_configs_admin_fk FOREIGN KEY (admin_user_id) REFERENCES admin_users (id) ON DELETE RESTRICT,
    CONSTRAINT avatar_starter_configs_version_positive CHECK (version >= 1),
    CONSTRAINT avatar_starter_configs_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT avatar_starter_configs_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_avatar_starter_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
DECLARE prev integer;
BEGIN
    IF NEW.version IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33028::integer, 0);
    SELECT max(version) INTO prev FROM avatar_starter_configs;
    IF NEW.version <> COALESCE(prev, 0) + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA004', MESSAGE = 'USAPP_AVATAR_STARTER_VERSION';
    END IF;
    IF NOT usapp_avatar_admin_active(NEW.admin_user_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA004', MESSAGE = 'USAPP_AVATAR_ADMIN';
    END IF;
    IF (SELECT count(*) FROM avatar_items i
         WHERE i.starter AND ((i.id = NEW.hair_item_id AND i.category = 'hair')
                           OR (i.id = NEW.clothes_item_id AND i.category = 'clothes')
                           OR (i.id = NEW.expression_item_id AND i.category = 'expression'))) <> 3 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA004', MESSAGE = 'USAPP_AVATAR_STARTER_ITEM';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_starter_configs_guard BEFORE INSERT ON avatar_starter_configs
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_starter_guard();

-- ---------------------------------------------------------------- append-only
CREATE TRIGGER avatar_assets_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_assets
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();
CREATE TRIGGER avatar_items_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_items
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();
CREATE TRIGGER avatar_item_revisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_item_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();
CREATE TRIGGER avatar_starter_configs_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_starter_configs
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();

-- ---------------------------------------------------------------- admin writes
-- Each write first takes the admin-key lock and classifies an existing use of the key across every catalog
-- write, so a retry gets its original answer before any mutable state is read. Then it checks, writes and
-- audits in the caller's transaction. Answers: {"result":"created"|"replay", ...} or {"error": ...}.
CREATE FUNCTION usapp_avatar_admin_key(p_admin uuid, p_key text)
RETURNS TABLE(kind text, fingerprint text, item_id uuid, version integer)
LANGUAGE sql STABLE AS $$
    SELECT CASE WHEN r.version = 1 THEN 'create' ELSE 'revise' END, r.request_fingerprint, r.item_id, r.version
      FROM avatar_item_revisions r WHERE r.admin_user_id = p_admin AND r.idempotency_key = p_key
    UNION ALL
    SELECT 'starter', s.request_fingerprint, NULL::uuid, s.version
      FROM avatar_starter_configs s WHERE s.admin_user_id = p_admin AND s.idempotency_key = p_key
$$;

CREATE FUNCTION usapp_avatar_item_create(p_admin uuid, p_key text, p_fingerprint text, p_category text, p_starter boolean,
                                         p_name text, p_asset_key text, p_price integer, p_listed boolean)
RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE k record; a avatar_assets%ROWTYPE; new_id uuid;
BEGIN
    PERFORM pg_advisory_xact_lock(33029::integer, hashtext(p_admin::text || ':' || p_key));
    SELECT * INTO k FROM usapp_avatar_admin_key(p_admin, p_key);
    IF FOUND THEN
        IF k.kind = 'create' AND k.fingerprint = p_fingerprint THEN
            RETURN jsonb_build_object('result', 'replay', 'item_id', k.item_id, 'version', k.version);
        END IF;
        RETURN jsonb_build_object('error', 'key_reused');
    END IF;
    IF (p_price IS NULL) IS DISTINCT FROM p_starter THEN
        RETURN jsonb_build_object('error', 'invalid');
    END IF;
    SELECT * INTO a FROM avatar_assets WHERE asset_key = p_asset_key;
    IF NOT FOUND OR a.category IS DISTINCT FROM p_category THEN
        RETURN jsonb_build_object('error', 'asset_invalid');
    END IF;
    IF EXISTS (SELECT 1 FROM avatar_items WHERE family = a.family) THEN
        RETURN jsonb_build_object('error', 'asset_invalid');
    END IF;
    INSERT INTO avatar_items (category, starter, family, created_by)
        VALUES (p_category, p_starter, a.family, p_admin) RETURNING id INTO new_id;
    INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                                       idempotency_key, request_fingerprint)
        VALUES (new_id, 1, p_name, p_asset_key, p_price, p_listed, p_admin, p_key, p_fingerprint);
    INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id, detail)
        VALUES ('admin', p_admin, 'avatar.item_created', 'avatar_item', new_id::text,
                jsonb_build_object('version', 1, 'category', p_category, 'starter', p_starter, 'asset_key', p_asset_key,
                                   'price_sunshine', p_price, 'listed', p_listed));
    RETURN jsonb_build_object('result', 'created', 'item_id', new_id, 'version', 1);
END
$$;

CREATE FUNCTION usapp_avatar_item_revise(p_admin uuid, p_key text, p_fingerprint text, p_item uuid, p_expected integer,
                                         p_name text, p_asset_key text, p_price integer, p_listed boolean)
RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE k record; i avatar_items%ROWTYPE; cur avatar_item_revisions%ROWTYPE; cur_asset avatar_assets%ROWTYPE;
        a avatar_assets%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33029::integer, hashtext(p_admin::text || ':' || p_key));
    SELECT * INTO k FROM usapp_avatar_admin_key(p_admin, p_key);
    IF FOUND THEN
        IF k.kind = 'revise' AND k.fingerprint = p_fingerprint AND k.item_id = p_item THEN
            RETURN jsonb_build_object('result', 'replay', 'item_id', k.item_id, 'version', k.version);
        END IF;
        RETURN jsonb_build_object('error', 'key_reused');
    END IF;
    PERFORM pg_advisory_xact_lock(33027::integer, hashtext('item:' || p_item::text));
    SELECT * INTO i FROM avatar_items WHERE id = p_item;
    IF NOT FOUND THEN
        RETURN jsonb_build_object('error', 'not_found');
    END IF;
    SELECT * INTO cur FROM avatar_item_revisions WHERE item_id = p_item ORDER BY version DESC LIMIT 1;
    IF cur.version <> p_expected THEN
        RETURN jsonb_build_object('error', 'conflict', 'current_version', cur.version);
    END IF;
    IF (p_price IS NULL) IS DISTINCT FROM i.starter THEN
        RETURN jsonb_build_object('error', 'invalid');
    END IF;
    SELECT * INTO cur_asset FROM avatar_assets WHERE asset_key = cur.asset_key;
    SELECT * INTO a FROM avatar_assets WHERE asset_key = p_asset_key;
    IF NOT FOUND OR a.family IS DISTINCT FROM i.family OR a.revision < cur_asset.revision THEN
        RETURN jsonb_build_object('error', 'asset_invalid');
    END IF;
    IF ROW(p_name, p_asset_key, p_price, p_listed) IS NOT DISTINCT FROM ROW(cur.name, cur.asset_key, cur.price_sunshine, cur.listed) THEN
        RETURN jsonb_build_object('error', 'invalid');
    END IF;
    INSERT INTO avatar_item_revisions (item_id, version, name, asset_key, price_sunshine, listed, admin_user_id,
                                       idempotency_key, request_fingerprint)
        VALUES (p_item, cur.version + 1, p_name, p_asset_key, p_price, p_listed, p_admin, p_key, p_fingerprint);
    INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id, detail)
        VALUES ('admin', p_admin, 'avatar.item_revised', 'avatar_item', p_item::text,
                jsonb_build_object('version', cur.version + 1, 'asset_key', p_asset_key, 'price_sunshine', p_price,
                                   'listed', p_listed, 'renamed', p_name IS DISTINCT FROM cur.name,
                                   'listing_changed', p_listed IS DISTINCT FROM cur.listed));
    RETURN jsonb_build_object('result', 'created', 'item_id', p_item, 'version', cur.version + 1);
END
$$;

CREATE FUNCTION usapp_avatar_starter_set(p_admin uuid, p_key text, p_fingerprint text, p_expected integer,
                                         p_hair uuid, p_clothes uuid, p_expression uuid)
RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE k record; cur integer;
BEGIN
    PERFORM pg_advisory_xact_lock(33029::integer, hashtext(p_admin::text || ':' || p_key));
    SELECT * INTO k FROM usapp_avatar_admin_key(p_admin, p_key);
    IF FOUND THEN
        IF k.kind = 'starter' AND k.fingerprint = p_fingerprint THEN
            RETURN jsonb_build_object('result', 'replay', 'version', k.version);
        END IF;
        RETURN jsonb_build_object('error', 'key_reused');
    END IF;
    PERFORM pg_advisory_xact_lock(33028::integer, 0);
    SELECT COALESCE(max(version), 0) INTO cur FROM avatar_starter_configs;
    IF cur <> p_expected THEN
        RETURN jsonb_build_object('error', 'conflict', 'current_version', cur);
    END IF;
    IF (SELECT count(*) FROM avatar_items i
         WHERE i.starter AND ((i.id = p_hair AND i.category = 'hair') OR (i.id = p_clothes AND i.category = 'clothes')
                           OR (i.id = p_expression AND i.category = 'expression'))) <> 3 THEN
        RETURN jsonb_build_object('error', 'starter_invalid');
    END IF;
    INSERT INTO avatar_starter_configs (version, hair_item_id, clothes_item_id, expression_item_id, admin_user_id,
                                        idempotency_key, request_fingerprint)
        VALUES (cur + 1, p_hair, p_clothes, p_expression, p_admin, p_key, p_fingerprint);
    INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id, detail)
        VALUES ('admin', p_admin, 'avatar.starter_configured', 'avatar_starter', (cur + 1)::text,
                jsonb_build_object('version', cur + 1, 'hair', p_hair, 'clothes', p_clothes, 'expression', p_expression));
    RETURN jsonb_build_object('result', 'created', 'version', cur + 1);
END
$$;

-- ---------------------------------------------------------------- privileges
-- The registry is read-only to the application; the catalog tables are SELECT and INSERT only.
REVOKE ALL ON avatar_assets, avatar_items, avatar_item_revisions, avatar_starter_configs FROM PUBLIC;
REVOKE ALL ON avatar_assets, avatar_items, avatar_item_revisions, avatar_starter_configs FROM usapp_api, usapp_push;
GRANT SELECT ON avatar_assets TO usapp_api;
GRANT SELECT, INSERT ON avatar_items, avatar_item_revisions, avatar_starter_configs TO usapp_api;

COMMIT;
