-- M7: reward-shop products (docs/phase1-m7-rewards.md).
-- reward_products holds the immutable identity (space, creator, owner = the partner
-- who will redeem); reward_product_revisions holds one append-only row per create,
-- edit or archive with the full term set after that mutation. The current state is
-- the highest version; a future redemption snapshots one version. Archive is
-- one-way. No wallet data, no redemption. 0000-0013 are unchanged; no data seeded.
-- Lock: per-product advisory lock (33007, hashtext(product uuid)).
-- UR001 append-only; UR002 revision guard; UR003 deferred: product has its create revision.
BEGIN;

DO $product_preflight$
BEGIN
    IF to_regclass('public.task_closures') IS NULL THEN
        RAISE EXCEPTION '0014_reward_products requires 0013_task_changes';
    END IF;
END
$product_preflight$;

CREATE TABLE reward_products (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    creator_id uuid NOT NULL,
    owner_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reward_products_pkey PRIMARY KEY (id),
    CONSTRAINT reward_products_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT reward_products_creator_fk FOREIGN KEY (couple_id, creator_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_products_owner_fk FOREIGN KEY (couple_id, owner_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_products_creator_not_owner CHECK (creator_id <> owner_id)
);

CREATE INDEX reward_products_couple_idx ON reward_products (couple_id, created_at DESC, id);

CREATE TABLE reward_product_revisions (
    product_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    version integer NOT NULL,
    actor_id uuid NOT NULL,
    action text NOT NULL,
    title text NOT NULL,
    description text NOT NULL DEFAULT '',
    price_flowers integer NOT NULL,
    redemption_mode text NOT NULL,
    status text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reward_product_revisions_pkey PRIMARY KEY (product_id, version),
    CONSTRAINT reward_product_revisions_key_unique UNIQUE (actor_id, idempotency_key),
    CONSTRAINT reward_product_revisions_product_fk FOREIGN KEY (product_id, couple_id)
        REFERENCES reward_products (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reward_product_revisions_member_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_product_revisions_version_valid CHECK (version >= 1),
    CONSTRAINT reward_product_revisions_action_valid CHECK (action IN ('create', 'edit', 'archive')),
    CONSTRAINT reward_product_revisions_create_first CHECK ((action = 'create') = (version = 1)),
    CONSTRAINT reward_product_revisions_status_valid CHECK (status IN ('active', 'archived')),
    CONSTRAINT reward_product_revisions_archive_status CHECK ((action = 'archive') = (status = 'archived')),
    CONSTRAINT reward_product_revisions_mode_valid CHECK (redemption_mode IN ('direct', 'request')),
    -- Approved 2026-10-26: integer price 1-999 flowers; no free products, no sunshine pricing.
    CONSTRAINT reward_product_revisions_price_range CHECK (price_flowers BETWEEN 1 AND 999),
    -- Same shapes as the 0007 task title/description CHECKs.
    CONSTRAINT reward_product_revisions_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 40
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT reward_product_revisions_description_shape CHECK (
        char_length(description) <= 500
        AND description !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT reward_product_revisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT reward_product_revisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_product_reject_mutation()
RETURNS trigger LANGUAGE plpgsql
AS $product_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UR001',
        MESSAGE = 'USAPP_PRODUCT_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$product_reject$;

CREATE TRIGGER reward_products_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_products
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_product_reject_mutation();
CREATE TRIGGER reward_product_revisions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_product_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_product_reject_mutation();

-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE FUNCTION usapp_product_revision_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $revision_guard$
DECLARE
    p reward_products%ROWTYPE;
    prev reward_product_revisions%ROWTYPE;
    has_prev boolean;
BEGIN
    IF NEW.product_id IS NULL OR NEW.couple_id IS NULL OR NEW.version IS NULL OR NEW.actor_id IS NULL
       OR NEW.action IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33007::integer, hashtext(NEW.product_id::text));
    SELECT * INTO p FROM reward_products WHERE id = NEW.product_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.actor_id IS DISTINCT FROM p.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UR002', MESSAGE = 'USAPP_PRODUCT_ACTOR';
    END IF;
    SELECT * INTO prev FROM reward_product_revisions
     WHERE product_id = NEW.product_id ORDER BY version DESC LIMIT 1;
    has_prev := FOUND;
    IF NEW.version::bigint <> COALESCE(prev.version, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UR002', MESSAGE = 'USAPP_PRODUCT_VERSION';
    END IF;
    IF has_prev AND prev.status = 'archived' THEN
        RAISE EXCEPTION USING ERRCODE = 'UR002', MESSAGE = 'USAPP_PRODUCT_ARCHIVED';
    END IF;
    IF NEW.action = 'archive'
       AND ROW(NEW.title, NEW.description, NEW.price_flowers, NEW.redemption_mode)
           IS DISTINCT FROM ROW(prev.title, prev.description, prev.price_flowers, prev.redemption_mode) THEN
        RAISE EXCEPTION USING ERRCODE = 'UR002', MESSAGE = 'USAPP_PRODUCT_ARCHIVE_TERMS';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < p.created_at
       OR (has_prev AND NEW.created_at < prev.created_at) THEN
        RAISE EXCEPTION USING ERRCODE = 'UR002', MESSAGE = 'USAPP_PRODUCT_TIME';
    END IF;
    RETURN NEW;
END
$revision_guard$;

CREATE TRIGGER reward_product_revisions_guard BEFORE INSERT ON reward_product_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_product_revision_guard();

-- A product never exists without its version-1 create revision.
CREATE FUNCTION usapp_product_create_check()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $product_create_check$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM reward_product_revisions r
                    WHERE r.product_id = NEW.id AND r.version = 1 AND r.action = 'create') THEN
        RAISE EXCEPTION USING ERRCODE = 'UR003', MESSAGE = 'USAPP_PRODUCT_CREATE_REVISION';
    END IF;
    RETURN NULL;
END
$product_create_check$;

CREATE CONSTRAINT TRIGGER reward_products_create_revision AFTER INSERT ON reward_products
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_product_create_check();

REVOKE ALL ON reward_products, reward_product_revisions FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON reward_products, reward_product_revisions TO usapp_api;

COMMIT;
