-- =====================================================================
-- 0045_avatar_purchases.sql
-- Phase 4 B2: sunshine purchases of paid avatar items (docs/phase4-avatar-shop-design.md Section 6).
--
-- Additive. The owner authorized the AVATAR_PURCHASE wallet posting path (2026-11-09).
--   avatar_purchases  one immutable snapshot per (buyer, item); it is also the ownership record. No other grant path.
--   AVATAR_PURCHASE   one operation, one purchase, exactly one sunshine debit of the snapshot price by the buyer.
-- Starter items are never purchased. Flowers are never touched. No refund, gift, transfer or equip.
--
-- Lock order: purchase key 33030 -> item 33027 (shared with catalog revisions) -> the buyer's sunshine account row.
-- SQLSTATE: UA005 purchase guard; UA006 deferred purchase/operation/entry link. UA001 append-only (0044).
-- =====================================================================

BEGIN;

DO $avatar_purchase_preflight$
BEGIN
    IF to_regclass('public.avatar_starter_configs') IS NULL THEN
        RAISE EXCEPTION '0045_avatar_purchases requires 0044_avatar_catalog';
    END IF;
END
$avatar_purchase_preflight$;

-- ---------------------------------------------------------------- the new posting type, and only it
ALTER TABLE wallet_operations DROP CONSTRAINT wallet_operations_type_valid;
ALTER TABLE wallet_operations
    ADD CONSTRAINT wallet_operations_type_valid
        CHECK (operation_type IN ('TASK_REWARD', 'REWARD_REDEMPTION', 'REDEMPTION_REFUND', 'AVATAR_PURCHASE'));

-- ---------------------------------------------------------------- purchases (= ownership)
CREATE TABLE avatar_purchases (
    id                  uuid        NOT NULL,
    couple_id           uuid        NOT NULL,
    user_id             uuid        NOT NULL,
    item_id             uuid        NOT NULL,
    item_version        integer     NOT NULL,
    name                text        NOT NULL,
    category            text        NOT NULL,
    asset_key           text        NOT NULL,
    price_sunshine      integer     NOT NULL,
    operation_id        uuid        NOT NULL,
    idempotency_key     text        NOT NULL,
    request_fingerprint text        NOT NULL,
    created_at          timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT avatar_purchases_pkey PRIMARY KEY (id),
    CONSTRAINT avatar_purchases_owner_unique UNIQUE (user_id, item_id),
    CONSTRAINT avatar_purchases_key_unique UNIQUE (user_id, idempotency_key),
    CONSTRAINT avatar_purchases_operation_unique UNIQUE (operation_id),
    CONSTRAINT avatar_purchases_buyer_fk FOREIGN KEY (couple_id, user_id) REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT avatar_purchases_revision_fk FOREIGN KEY (item_id, item_version) REFERENCES avatar_item_revisions (item_id, version) ON DELETE RESTRICT,
    CONSTRAINT avatar_purchases_asset_fk FOREIGN KEY (asset_key) REFERENCES avatar_assets (asset_key) ON DELETE RESTRICT,
    CONSTRAINT avatar_purchases_operation_fk FOREIGN KEY (operation_id, couple_id) REFERENCES wallet_operations (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT avatar_purchases_category_valid CHECK (usapp_avatar_category_valid(category)),
    CONSTRAINT avatar_purchases_price_range CHECK (price_sunshine BETWEEN 1 AND 9999),
    CONSTRAINT avatar_purchases_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT avatar_purchases_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX avatar_purchases_owner_list_idx ON avatar_purchases (user_id, created_at DESC, id DESC);
CREATE INDEX avatar_purchases_item_idx ON avatar_purchases (item_id);

-- The snapshot is the item's current listed revision of a paid item, and its operation is exactly this purchase's.
CREATE FUNCTION usapp_avatar_purchase_guard() RETURNS trigger
LANGUAGE plpgsql VOLATILE AS $$
DECLARE i avatar_items%ROWTYPE; r avatar_item_revisions%ROWTYPE; o wallet_operations%ROWTYPE;
BEGIN
    IF NEW.item_id IS NULL OR NEW.operation_id IS NULL OR NEW.id IS NULL THEN RETURN NEW; END IF;
    PERFORM pg_advisory_xact_lock(33027::integer, hashtext('item:' || NEW.item_id::text));
    SELECT * INTO i FROM avatar_items WHERE id = NEW.item_id;
    IF NOT FOUND OR i.starter THEN
        RAISE EXCEPTION USING ERRCODE = 'UA005', MESSAGE = 'USAPP_AVATAR_PURCHASE_ITEM';
    END IF;
    SELECT * INTO r FROM avatar_item_revisions WHERE item_id = NEW.item_id ORDER BY version DESC LIMIT 1;
    IF r.version IS DISTINCT FROM NEW.item_version OR NOT r.listed
       OR ROW(NEW.name, NEW.category, NEW.asset_key, NEW.price_sunshine)
          IS DISTINCT FROM ROW(r.name, i.category, r.asset_key, r.price_sunshine) THEN
        RAISE EXCEPTION USING ERRCODE = 'UA005', MESSAGE = 'USAPP_AVATAR_PURCHASE_SNAPSHOT';
    END IF;
    SELECT * INTO o FROM wallet_operations WHERE id = NEW.operation_id;
    IF NOT FOUND OR o.operation_type IS DISTINCT FROM 'AVATAR_PURCHASE' OR o.source_type IS DISTINCT FROM 'avatar_purchase'
       OR o.source_id IS DISTINCT FROM NEW.id::text OR o.actor_user_id IS DISTINCT FROM NEW.user_id
       OR o.couple_id IS DISTINCT FROM NEW.couple_id OR o.idempotency_key IS DISTINCT FROM NEW.idempotency_key
       OR o.request_fingerprint IS DISTINCT FROM NEW.request_fingerprint THEN
        RAISE EXCEPTION USING ERRCODE = 'UA005', MESSAGE = 'USAPP_AVATAR_PURCHASE_OPERATION';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER avatar_purchases_guard BEFORE INSERT ON avatar_purchases
    FOR EACH ROW EXECUTE FUNCTION usapp_avatar_purchase_guard();
CREATE TRIGGER avatar_purchases_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON avatar_purchases
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_avatar_reject_mutation();

-- Every AVATAR_PURCHASE operation: exactly one purchase, and exactly one entry, the buyer's sunshine debit of the snapshot price.
CREATE FUNCTION usapp_avatar_purchase_validate(p_operation_id uuid) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE o wallet_operations%ROWTYPE; p avatar_purchases%ROWTYPE; n_purchases bigint; n_total bigint; n_match bigint;
BEGIN
    IF p_operation_id IS NULL THEN RETURN; END IF;
    SELECT * INTO o FROM wallet_operations WHERE id = p_operation_id;
    IF NOT FOUND OR o.operation_type IS DISTINCT FROM 'AVATAR_PURCHASE' THEN RETURN; END IF;
    IF o.source_type IS DISTINCT FROM 'avatar_purchase' THEN
        RAISE EXCEPTION USING ERRCODE = 'UA006', MESSAGE = 'USAPP_AVATAR_PURCHASE_LINK_SOURCE';
    END IF;
    SELECT count(*) INTO n_purchases FROM avatar_purchases WHERE operation_id = p_operation_id;
    IF n_purchases <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA006', MESSAGE = 'USAPP_AVATAR_PURCHASE_LINK_PURCHASE';
    END IF;
    SELECT * INTO p FROM avatar_purchases WHERE operation_id = p_operation_id;
    SELECT count(*),
           count(*) FILTER (WHERE e.user_id = p.user_id AND e.asset_type = 'sunshine' AND e.amount = -p.price_sunshine::bigint)
      INTO n_total, n_match
      FROM wallet_ledger_entries e WHERE e.operation_id = p_operation_id;
    IF n_total <> 1 OR n_match <> 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UA006', MESSAGE = 'USAPP_AVATAR_PURCHASE_LINK_ENTRIES';
    END IF;
END
$$;

CREATE FUNCTION usapp_avatar_purchase_link_check() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_TABLE_NAME = 'wallet_operations' THEN
        PERFORM usapp_avatar_purchase_validate(NEW.id);
    ELSE
        PERFORM usapp_avatar_purchase_validate(NEW.operation_id);
    END IF;
    RETURN NULL;
END
$$;

CREATE CONSTRAINT TRIGGER avatar_purchases_debit_valid AFTER INSERT ON avatar_purchases
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_avatar_purchase_link_check();
CREATE CONSTRAINT TRIGGER wallet_operations_avatar_purchase_valid AFTER INSERT ON wallet_operations
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN (NEW.operation_type = 'AVATAR_PURCHASE')
    EXECUTE FUNCTION usapp_avatar_purchase_link_check();
CREATE CONSTRAINT TRIGGER wallet_ledger_entries_avatar_purchase_valid AFTER INSERT ON wallet_ledger_entries
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_avatar_purchase_link_check();

-- ---------------------------------------------------------------- the purchase
-- Answers {"result":"created"|"replay","purchase_id":...} or {"error": ...}. Refusals write nothing; the caller rolls back.
CREATE FUNCTION usapp_avatar_purchase(p_user uuid, p_key text, p_fingerprint text, p_item uuid, p_expected integer)
RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE k avatar_purchases%ROWTYPE; space uuid; i avatar_items%ROWTYPE; cur avatar_item_revisions%ROWTYPE;
        owned uuid; prev_seq bigint; prev_balance bigint; op uuid := gen_random_uuid(); pid uuid := gen_random_uuid();
BEGIN
    -- 1-2. Key lock, then classification before any catalog, ownership or balance read.
    PERFORM pg_advisory_xact_lock(33030::integer, hashtext(p_user::text || ':' || p_key));
    SELECT * INTO k FROM avatar_purchases WHERE user_id = p_user AND idempotency_key = p_key;
    IF FOUND THEN
        IF k.request_fingerprint = p_fingerprint THEN
            RETURN jsonb_build_object('result', 'replay', 'purchase_id', k.id);
        END IF;
        RETURN jsonb_build_object('error', 'key_reused');
    END IF;
    -- 3. Membership, as the wallet requires.
    SELECT m.couple_id INTO space FROM couple_members m WHERE m.user_id = p_user FOR SHARE;
    IF NOT FOUND THEN
        RETURN jsonb_build_object('error', 'no_space');
    END IF;
    -- 4-5. The item lock, shared with catalog revisions; then today's catalog state.
    PERFORM pg_advisory_xact_lock(33027::integer, hashtext('item:' || p_item::text));
    SELECT * INTO i FROM avatar_items WHERE id = p_item;
    IF NOT FOUND THEN
        RETURN jsonb_build_object('error', 'not_found');
    END IF;
    IF i.starter THEN
        RETURN jsonb_build_object('error', 'starter');
    END IF;
    SELECT * INTO cur FROM avatar_item_revisions WHERE item_id = p_item ORDER BY version DESC LIMIT 1;
    IF NOT cur.listed THEN
        RETURN jsonb_build_object('error', 'unlisted');
    END IF;
    IF cur.version <> p_expected THEN
        RETURN jsonb_build_object('error', 'conflict', 'current_version', cur.version, 'current_price', cur.price_sunshine);
    END IF;
    -- 6. One purchase per user per item, including one committed by another key while this request waited.
    SELECT id INTO owned FROM avatar_purchases WHERE user_id = p_user AND item_id = p_item;
    IF FOUND THEN
        RETURN jsonb_build_object('error', 'already_owned', 'purchase_id', owned);
    END IF;
    -- 7-8. The wallet's own serialization: the buyer's sunshine account row, then the latest entry.
    INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (p_user, 'sunshine', space)
        ON CONFLICT (user_id, asset_type) DO NOTHING;
    PERFORM 1 FROM wallet_accounts WHERE user_id = p_user AND asset_type = 'sunshine' FOR UPDATE;
    SELECT e.seq, e.balance_after INTO prev_seq, prev_balance
      FROM wallet_ledger_entries e WHERE e.user_id = p_user AND e.asset_type = 'sunshine' ORDER BY e.seq DESC LIMIT 1;
    prev_seq := COALESCE(prev_seq, 0);
    prev_balance := COALESCE(prev_balance, 0);
    IF prev_balance < cur.price_sunshine THEN
        RETURN jsonb_build_object('error', 'insufficient', 'price', cur.price_sunshine);
    END IF;
    -- 9. Operation, snapshot, the exact debit and the audit row.
    INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint)
        VALUES (op, space, p_user, 'AVATAR_PURCHASE', 'avatar_purchase', pid::text, p_key, p_fingerprint);
    INSERT INTO avatar_purchases (id, couple_id, user_id, item_id, item_version, name, category, asset_key, price_sunshine,
                                  operation_id, idempotency_key, request_fingerprint)
        VALUES (pid, space, p_user, p_item, cur.version, cur.name, i.category, cur.asset_key, cur.price_sunshine,
                op, p_key, p_fingerprint);
    INSERT INTO wallet_ledger_entries (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op, space, p_user, 'sunshine', prev_seq + 1, -cur.price_sunshine::bigint, prev_balance - cur.price_sunshine);
    INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail)
        VALUES ('user', p_user, space, 'avatar.purchased', 'avatar_purchase', pid::text,
                jsonb_build_object('item_id', p_item, 'item_version', cur.version, 'price_sunshine', cur.price_sunshine,
                                   'operation_id', op));
    -- 10. This purchase's deferred links, checked now: a violation is a definite statement failure, never an ambiguous commit.
    -- Only the named checks, then back to their declared DEFERRED mode, so the caller's transaction state is not changed.
    SET CONSTRAINTS avatar_purchases_debit_valid, wallet_operations_avatar_purchase_valid,
        wallet_ledger_entries_avatar_purchase_valid, wallet_operations_require_entries IMMEDIATE;
    SET CONSTRAINTS avatar_purchases_debit_valid, wallet_operations_avatar_purchase_valid,
        wallet_ledger_entries_avatar_purchase_valid, wallet_operations_require_entries DEFERRED;
    RETURN jsonb_build_object('result', 'created', 'purchase_id', pid);
END
$$;

-- ---------------------------------------------------------------- privileges
REVOKE ALL ON avatar_purchases FROM PUBLIC;
REVOKE ALL ON avatar_purchases FROM usapp_api, usapp_push;
GRANT SELECT, INSERT ON avatar_purchases TO usapp_api;

COMMIT;
