-- M8: direct redemption of reward products (docs/phase1-m8-redemption.md).
-- A redemption is an append-only snapshot of one product revision (title,
-- description, price, mode, creator, redeemer) paid by exactly one
-- REWARD_REDEMPTION wallet operation with one flower debit entry. Fulfillment is a
-- separate append-only row written by the creator; it is final and never charges.
-- Request mode, cancellation and refunds are later milestones (M9). 0000-0014 are
-- unchanged; no data is seeded.
-- Locks: product (33007, hashtext(product uuid)) shared with M7 edit/archive;
-- redemption (33008, hashtext(redemption uuid)) for fulfillment.
-- UM001 append-only; UM002 redemption guard; UM003 deferred debit link;
-- UM004 fulfillment guard.
BEGIN;

DO $redemption_preflight$
BEGIN
    IF to_regclass('public.reward_product_revisions') IS NULL THEN
        RAISE EXCEPTION '0015_reward_redemptions requires 0014_reward_products';
    END IF;
END
$redemption_preflight$;

CREATE TABLE reward_redemptions (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    product_id uuid NOT NULL,
    product_version integer NOT NULL,
    creator_id uuid NOT NULL,
    redeemer_id uuid NOT NULL,
    title text NOT NULL,
    description text NOT NULL DEFAULT '',
    price_flowers integer NOT NULL,
    redemption_mode text NOT NULL,
    operation_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reward_redemptions_pkey PRIMARY KEY (id),
    CONSTRAINT reward_redemptions_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT reward_redemptions_operation_unique UNIQUE (operation_id),
    CONSTRAINT reward_redemptions_key_unique UNIQUE (redeemer_id, idempotency_key),
    CONSTRAINT reward_redemptions_revision_fk FOREIGN KEY (product_id, product_version)
        REFERENCES reward_product_revisions (product_id, version) ON DELETE RESTRICT,
    CONSTRAINT reward_redemptions_product_fk FOREIGN KEY (product_id, couple_id)
        REFERENCES reward_products (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemptions_creator_fk FOREIGN KEY (couple_id, creator_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemptions_redeemer_fk FOREIGN KEY (couple_id, redeemer_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemptions_operation_fk FOREIGN KEY (operation_id, couple_id)
        REFERENCES wallet_operations (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemptions_creator_not_redeemer CHECK (creator_id <> redeemer_id),
    CONSTRAINT reward_redemptions_price_range CHECK (price_flowers BETWEEN 1 AND 999),
    -- M8 redeems direct-mode products only; request mode arrives with M9.
    CONSTRAINT reward_redemptions_mode_direct CHECK (redemption_mode = 'direct'),
    CONSTRAINT reward_redemptions_title_shape CHECK (
        char_length(title) BETWEEN 1 AND 40
        AND title = btrim(title)
        AND title !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT reward_redemptions_description_shape CHECK (
        char_length(description) <= 500
        AND description !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT reward_redemptions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT reward_redemptions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE INDEX reward_redemptions_couple_idx ON reward_redemptions (couple_id, created_at DESC, id);

CREATE TABLE reward_redemption_fulfillments (
    redemption_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    fulfilled_by uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    fulfilled_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT reward_redemption_fulfillments_pkey PRIMARY KEY (redemption_id),
    CONSTRAINT reward_redemption_fulfillments_key_unique UNIQUE (fulfilled_by, idempotency_key),
    CONSTRAINT reward_redemption_fulfillments_redemption_fk FOREIGN KEY (redemption_id, couple_id)
        REFERENCES reward_redemptions (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemption_fulfillments_member_fk FOREIGN KEY (couple_id, fulfilled_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT reward_redemption_fulfillments_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT reward_redemption_fulfillments_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_redemption_reject_mutation()
RETURNS trigger LANGUAGE plpgsql
AS $redemption_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UM001',
        MESSAGE = 'USAPP_REDEMPTION_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$redemption_reject$;

CREATE TRIGGER reward_redemptions_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemptions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_redemption_reject_mutation();
CREATE TRIGGER reward_redemption_fulfillments_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemption_fulfillments
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_redemption_reject_mutation();

-- VOLATILE: statements after the advisory-lock wait see the previous holder's commit.
CREATE FUNCTION usapp_redemption_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $redemption_guard$
DECLARE
    p reward_products%ROWTYPE;
    r reward_product_revisions%ROWTYPE;
    o wallet_operations%ROWTYPE;
BEGIN
    IF NEW.id IS NULL OR NEW.couple_id IS NULL OR NEW.product_id IS NULL OR NEW.product_version IS NULL
       OR NEW.creator_id IS NULL OR NEW.redeemer_id IS NULL OR NEW.operation_id IS NULL
       OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    -- Same lock as M7 edit/archive: the snapshot cannot race a new revision.
    PERFORM pg_advisory_xact_lock(33007::integer, hashtext(NEW.product_id::text));
    SELECT * INTO p FROM reward_products WHERE id = NEW.product_id;
    IF NOT FOUND OR p.couple_id IS DISTINCT FROM NEW.couple_id
       OR NEW.creator_id IS DISTINCT FROM p.creator_id OR NEW.redeemer_id IS DISTINCT FROM p.owner_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UM002', MESSAGE = 'USAPP_REDEMPTION_ACTOR';
    END IF;
    SELECT * INTO r FROM reward_product_revisions WHERE product_id = NEW.product_id ORDER BY version DESC LIMIT 1;
    IF NOT FOUND OR r.version IS DISTINCT FROM NEW.product_version OR r.status IS DISTINCT FROM 'active'
       OR r.redemption_mode IS DISTINCT FROM 'direct'
       OR ROW(NEW.title, NEW.description, NEW.price_flowers, NEW.redemption_mode)
          IS DISTINCT FROM ROW(r.title, r.description, r.price_flowers, r.redemption_mode) THEN
        RAISE EXCEPTION USING ERRCODE = 'UM002', MESSAGE = 'USAPP_REDEMPTION_SNAPSHOT';
    END IF;
    SELECT * INTO o FROM wallet_operations WHERE id = NEW.operation_id;
    IF NOT FOUND OR o.operation_type IS DISTINCT FROM 'REWARD_REDEMPTION'
       OR o.source_type IS DISTINCT FROM 'reward_redemption' OR o.source_id IS DISTINCT FROM NEW.id::text
       OR o.actor_user_id IS DISTINCT FROM NEW.redeemer_id OR o.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UM002', MESSAGE = 'USAPP_REDEMPTION_OPERATION';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < r.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UM002', MESSAGE = 'USAPP_REDEMPTION_TIME';
    END IF;
    RETURN NEW;
END
$redemption_guard$;

CREATE TRIGGER reward_redemptions_guard BEFORE INSERT ON reward_redemptions
    FOR EACH ROW EXECUTE FUNCTION usapp_redemption_guard();

-- A redemption operation belongs to exactly one redemption and has exactly one
-- entry: the redeemer's flower debit of the snapshot price.
CREATE FUNCTION usapp_redemption_validate(p_operation_id uuid)
RETURNS void LANGUAGE plpgsql
AS $redemption_validate$
DECLARE
    o wallet_operations%ROWTYPE;
    d reward_redemptions%ROWTYPE;
    n_redemptions bigint;
    n_total bigint;
    n_match bigint;
BEGIN
    SELECT * INTO o FROM wallet_operations WHERE id = p_operation_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UM003', MESSAGE = 'USAPP_REDEMPTION_LINK_OPERATION';
    END IF;
    IF o.operation_type IS DISTINCT FROM 'REWARD_REDEMPTION' THEN
        RETURN;
    END IF;
    SELECT count(*) INTO n_redemptions FROM reward_redemptions WHERE operation_id = p_operation_id;
    IF n_redemptions <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UM003', MESSAGE = 'USAPP_REDEMPTION_LINK_REDEMPTION';
    END IF;
    SELECT * INTO d FROM reward_redemptions WHERE operation_id = p_operation_id;
    SELECT count(*),
           count(*) FILTER (WHERE e.user_id = d.redeemer_id AND e.asset_type = 'flower'
                            AND e.amount = -d.price_flowers::bigint)
      INTO n_total, n_match
      FROM wallet_ledger_entries e
     WHERE e.operation_id = p_operation_id;
    IF n_total <> 1 OR n_match <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UM003', MESSAGE = 'USAPP_REDEMPTION_LINK_ENTRIES';
    END IF;
END
$redemption_validate$;

CREATE FUNCTION usapp_redemption_link_check()
RETURNS trigger LANGUAGE plpgsql
AS $redemption_link$
BEGIN
    IF TG_TABLE_NAME = 'reward_redemptions' THEN
        PERFORM usapp_redemption_validate(NEW.operation_id);
    ELSIF TG_TABLE_NAME = 'wallet_operations' THEN
        PERFORM usapp_redemption_validate(NEW.id);
    ELSE
        PERFORM usapp_redemption_validate(NEW.operation_id);
    END IF;
    RETURN NULL;
END
$redemption_link$;

CREATE CONSTRAINT TRIGGER reward_redemptions_debit_valid AFTER INSERT ON reward_redemptions
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_redemption_link_check();
CREATE CONSTRAINT TRIGGER wallet_operations_redemption_valid AFTER INSERT ON wallet_operations
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW WHEN (NEW.operation_type = 'REWARD_REDEMPTION')
    EXECUTE FUNCTION usapp_redemption_link_check();
CREATE CONSTRAINT TRIGGER wallet_ledger_entries_redemption_valid AFTER INSERT ON wallet_ledger_entries
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION usapp_redemption_link_check();

CREATE FUNCTION usapp_redemption_fulfillment_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $fulfillment_guard$
DECLARE
    d reward_redemptions%ROWTYPE;
BEGIN
    IF NEW.redemption_id IS NULL OR NEW.couple_id IS NULL OR NEW.fulfilled_by IS NULL
       OR NEW.fulfilled_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33008::integer, hashtext(NEW.redemption_id::text));
    SELECT * INTO d FROM reward_redemptions WHERE id = NEW.redemption_id;
    IF NOT FOUND OR d.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.fulfilled_by IS DISTINCT FROM d.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UM004', MESSAGE = 'USAPP_FULFILLMENT_ACTOR';
    END IF;
    IF NEW.fulfilled_at > clock_timestamp() OR NEW.fulfilled_at < d.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UM004', MESSAGE = 'USAPP_FULFILLMENT_TIME';
    END IF;
    RETURN NEW;
END
$fulfillment_guard$;

CREATE TRIGGER reward_redemption_fulfillments_guard BEFORE INSERT ON reward_redemption_fulfillments
    FOR EACH ROW EXECUTE FUNCTION usapp_redemption_fulfillment_guard();

REVOKE ALL ON reward_redemptions, reward_redemption_fulfillments FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON reward_redemptions, reward_redemption_fulfillments TO usapp_api;

COMMIT;
