-- Furnishings belong to the space. Only the purchaser's sunshine account is debited.
BEGIN;

ALTER TABLE public.wallet_operations DROP CONSTRAINT wallet_operations_type_valid;
ALTER TABLE public.wallet_operations ADD CONSTRAINT wallet_operations_type_valid CHECK
 (operation_type IN ('TASK_REWARD','REWARD_REDEMPTION','REDEMPTION_REFUND','AVATAR_PURCHASE',
    'ADMIN_ADJUSTMENT','ADMIN_REDEMPTION_REFUND','ROOM_FURNITURE_PURCHASE'));

-- Retain the wallet chain check under the furniture writer's pinned search path.
CREATE OR REPLACE FUNCTION public.usapp_wallet_ledger_chain()
RETURNS trigger
LANGUAGE plpgsql
AS $wallet_chain$
DECLARE
    prev_seq     bigint;
    prev_balance bigint;
BEGIN
    IF NEW.user_id IS NULL OR NEW.asset_type IS NULL OR NEW.seq IS NULL
       OR NEW.amount IS NULL OR NEW.balance_after IS NULL THEN
        RETURN NEW;
    END IF;

    PERFORM 1
       FROM public.wallet_accounts a
      WHERE a.user_id = NEW.user_id AND a.asset_type = NEW.asset_type
        FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW002',
            MESSAGE = 'USAPP_WALLET_ACCOUNT_MISSING';
    END IF;

    SELECT e.seq, e.balance_after
      INTO prev_seq, prev_balance
      FROM public.wallet_ledger_entries e
     WHERE e.user_id = NEW.user_id AND e.asset_type = NEW.asset_type
     ORDER BY e.seq DESC
     LIMIT 1;
    IF NOT FOUND THEN
        prev_seq := 0;
        prev_balance := 0;
    END IF;

    IF NEW.seq <> prev_seq + 1 THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW003',
            MESSAGE = 'USAPP_WALLET_CHAIN_SEQ',
            DETAIL  = 'expected seq ' || (prev_seq + 1) || ', got ' || NEW.seq;
    END IF;
    IF NEW.balance_after <> prev_balance + NEW.amount THEN
        RAISE EXCEPTION USING
            ERRCODE = 'UW004',
            MESSAGE = 'USAPP_WALLET_CHAIN_BALANCE',
            DETAIL  = 'expected balance_after ' || (prev_balance + NEW.amount)
                      || ', got ' || NEW.balance_after;
    END IF;
    RETURN NEW;
END
$wallet_chain$;

CREATE TABLE public.room_furniture_catalog (
    item text PRIMARY KEY,
    price integer NOT NULL CHECK (price BETWEEN 1 AND 9999)
);
INSERT INTO public.room_furniture_catalog VALUES ('bookshelf', 20), ('armchair', 30), ('vase', 10);

CREATE TABLE public.room_furniture_purchases (
    couple_id uuid NOT NULL REFERENCES public.couples(id),
    item text NOT NULL REFERENCES public.room_furniture_catalog(item),
    buyer uuid NOT NULL REFERENCES public.users(id),
    price integer NOT NULL CHECK (price BETWEEN 1 AND 9999),
    request_key text NOT NULL CHECK (request_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    operation_id uuid NOT NULL UNIQUE REFERENCES public.wallet_operations(id),
    PRIMARY KEY (couple_id, item),
    UNIQUE (buyer, request_key)
);

CREATE FUNCTION public.usapp_room_furniture_immutable() RETURNS trigger
LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
BEGIN
    RAISE EXCEPTION 'Room furniture facts are append only';
END
$$;
CREATE TRIGGER room_furniture_purchases_immutable
BEFORE UPDATE OR DELETE OR TRUNCATE ON public.room_furniture_purchases
FOR EACH STATEMENT EXECUTE FUNCTION public.usapp_room_furniture_immutable();

-- Wallet writers have no direct rights on furniture purchases. Independently
-- reject a room operation posted through the ordinary wallet grants.
CREATE FUNCTION public.usapp_room_furniture_debit_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
    IF NEW.operation_type = 'ROOM_FURNITURE_PURCHASE' AND NOT EXISTS (
        SELECT 1 FROM public.room_furniture_purchases p
        WHERE p.operation_id = NEW.id AND p.couple_id = NEW.couple_id
          AND p.buyer = NEW.actor_user_id AND p.request_key = NEW.idempotency_key
          AND NEW.source_type = 'room_furniture_purchase' AND NEW.source_id = NEW.id::text
          AND (SELECT count(*) FROM public.wallet_ledger_entries e WHERE e.operation_id = NEW.id) = 1
          AND EXISTS (SELECT 1 FROM public.wallet_ledger_entries e WHERE e.operation_id = NEW.id
                      AND e.user_id = p.buyer AND e.couple_id = p.couple_id
                      AND e.asset_type = 'sunshine' AND e.amount = -p.price)
    ) THEN RAISE EXCEPTION 'Missing or invalid room furniture debit'; END IF;
    RETURN NULL;
END
$$;
CREATE CONSTRAINT TRIGGER room_furniture_debit_guard AFTER INSERT ON public.wallet_operations
DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION public.usapp_room_furniture_debit_guard();

CREATE FUNCTION public.usapp_room_furniture_ledger_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM public.wallet_operations o WHERE o.id = NEW.operation_id
          AND o.operation_type = 'ROOM_FURNITURE_PURCHASE'
    ) AND NOT EXISTS (
        SELECT 1 FROM public.room_furniture_purchases p
        WHERE p.operation_id = NEW.operation_id AND p.buyer = NEW.user_id
          AND p.couple_id = NEW.couple_id AND NEW.asset_type = 'sunshine' AND NEW.amount = -p.price
          AND (SELECT count(*) FROM public.wallet_ledger_entries e WHERE e.operation_id = NEW.operation_id) = 1
    ) THEN RAISE EXCEPTION 'Invalid additional room furniture debit'; END IF;
    RETURN NULL;
END
$$;
CREATE CONSTRAINT TRIGGER room_furniture_ledger_guard AFTER INSERT ON public.wallet_ledger_entries
DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION public.usapp_room_furniture_ledger_guard();

CREATE FUNCTION public.usapp_room_furniture_read(p_user uuid) RETURNS jsonb
LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
    SELECT jsonb_agg(jsonb_build_object('item', c.item, 'price', c.price, 'owned', p.item IS NOT NULL) ORDER BY c.item)
    FROM public.couple_members m CROSS JOIN public.room_furniture_catalog c
    LEFT JOIN public.room_furniture_purchases p ON p.couple_id = m.couple_id AND p.item = c.item
    WHERE m.user_id = p_user
$$;

CREATE FUNCTION public.usapp_room_furniture_buy(p_user uuid, p_item text, p_key text) RETURNS text
LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE
    saved public.room_furniture_purchases%ROWTYPE;
    space uuid;
    cost integer;
    last_seq bigint;
    balance bigint;
    op uuid := gen_random_uuid();
BEGIN
    IF p_key IS NULL OR p_key !~ '^[A-Za-z0-9_-]{16,128}$' OR p_item IS NULL THEN RETURN 'invalid'; END IF;
    PERFORM pg_advisory_xact_lock(33034, hashtext(p_user::text || ':' || p_key));
    SELECT * INTO saved FROM public.room_furniture_purchases WHERE buyer = p_user AND request_key = p_key;
    IF FOUND THEN
        IF saved.item <> p_item THEN RETURN 'key_reused'; END IF;
        IF NOT EXISTS (SELECT 1 FROM public.couple_members WHERE user_id = p_user AND couple_id = saved.couple_id)
            THEN RETURN 'no_space'; END IF;
        RETURN 'replay';
    END IF;
    SELECT couple_id INTO space FROM public.couple_members WHERE user_id = p_user FOR SHARE;
    IF NOT FOUND THEN RETURN 'no_space'; END IF;
    SELECT price INTO cost FROM public.room_furniture_catalog WHERE item = p_item;
    IF NOT FOUND THEN RETURN 'invalid'; END IF;
    PERFORM pg_advisory_xact_lock(33035, hashtext(space::text || ':' || p_item));
    IF EXISTS (SELECT 1 FROM public.room_furniture_purchases WHERE couple_id = space AND item = p_item)
        THEN RETURN 'owned'; END IF;

    PERFORM 1 FROM public.wallet_accounts WHERE user_id = p_user AND asset_type = 'sunshine'
      AND couple_id = space FOR UPDATE;
    IF NOT FOUND THEN RETURN 'insufficient'; END IF;
    SELECT seq, balance_after INTO last_seq, balance FROM public.wallet_ledger_entries
      WHERE user_id = p_user AND asset_type = 'sunshine' ORDER BY seq DESC LIMIT 1;
    IF COALESCE(balance, 0) < cost THEN RETURN 'insufficient'; END IF;

    INSERT INTO public.wallet_operations
        (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint)
        VALUES (op, space, p_user, 'ROOM_FURNITURE_PURCHASE', 'room_furniture_purchase', op::text,
                p_key, encode(sha256(convert_to(p_item, 'UTF8')), 'hex'));
    INSERT INTO public.room_furniture_purchases VALUES (space, p_item, p_user, cost, p_key, op);
    INSERT INTO public.wallet_ledger_entries
        (operation_id, couple_id, user_id, asset_type, seq, amount, balance_after)
        VALUES (op, space, p_user, 'sunshine', last_seq + 1, -cost, balance - cost);
    INSERT INTO public.audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail)
        VALUES ('user', p_user, space, 'room.furniture_purchased', 'room_furniture_purchase', op::text,
                jsonb_build_object('item', p_item, 'price_sunshine', cost));
    RETURN 'saved';
END
$$;

CREATE OR REPLACE FUNCTION public.usapp_shared_room_save(p_user uuid, p_version integer, p_layout jsonb)
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
           OR (item->>'item' NOT IN ('table', 'lamp', 'plant', 'rug') AND NOT EXISTS (
               SELECT 1 FROM public.room_furniture_purchases p
               WHERE p.couple_id = room_couple AND p.item = entries.item->>'item'))
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

REVOKE ALL ON TABLE public.room_furniture_catalog, public.room_furniture_purchases FROM PUBLIC, usapp_api, usapp_push;
REVOKE ALL ON FUNCTION public.usapp_room_furniture_immutable(), public.usapp_room_furniture_debit_guard(),
    public.usapp_room_furniture_ledger_guard(), public.usapp_room_furniture_read(uuid),
    public.usapp_room_furniture_buy(uuid,text,text) FROM PUBLIC, usapp_api, usapp_push;
GRANT EXECUTE ON FUNCTION public.usapp_room_furniture_read(uuid), public.usapp_room_furniture_buy(uuid,text,text) TO usapp_api;
COMMIT;

