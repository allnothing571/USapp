-- M9: request-mode confirmation and mutual cancellation. No data is rewritten.
BEGIN;

ALTER TABLE reward_redemptions ALTER COLUMN operation_id DROP NOT NULL;
ALTER TABLE reward_redemptions DROP CONSTRAINT reward_redemptions_mode_direct;
ALTER TABLE reward_redemptions ADD CONSTRAINT reward_redemptions_mode_valid
    CHECK ((redemption_mode = 'direct' AND operation_id IS NOT NULL)
        OR (redemption_mode = 'request' AND operation_id IS NULL));

CREATE TABLE reward_redemption_events (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    redemption_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    actor_id uuid NOT NULL,
    action text NOT NULL CHECK (action IN ('confirm','reject','withdraw','cancel_propose',
        'cancel_approve','cancel_reject','cancel_withdraw')),
    proposal_id uuid REFERENCES reward_redemption_events(id) ON DELETE RESTRICT,
    operation_id uuid UNIQUE,
    idempotency_key text NOT NULL CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    request_fingerprint text NOT NULL CHECK (request_fingerprint ~ '^[0-9a-f]{64}$'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE(actor_id, idempotency_key),
    FOREIGN KEY(redemption_id,couple_id) REFERENCES reward_redemptions(id,couple_id) ON DELETE RESTRICT,
    FOREIGN KEY(couple_id,actor_id) REFERENCES couple_members(couple_id,user_id) ON DELETE RESTRICT,
    FOREIGN KEY(operation_id,couple_id) REFERENCES wallet_operations(id,couple_id) ON DELETE RESTRICT,
    CHECK ((action IN ('confirm','cancel_approve')) = (operation_id IS NOT NULL)),
    CHECK ((action IN ('cancel_approve','cancel_reject','cancel_withdraw')) = (proposal_id IS NOT NULL))
);
CREATE INDEX reward_redemption_events_redemption_idx ON reward_redemption_events(redemption_id);
CREATE UNIQUE INDEX reward_redemption_events_confirm_unique ON reward_redemption_events(redemption_id)
    WHERE action='confirm';
CREATE UNIQUE INDEX reward_redemption_events_cancelled_unique ON reward_redemption_events(redemption_id)
    WHERE action IN ('reject','withdraw','cancel_approve');
CREATE UNIQUE INDEX reward_redemption_events_proposal_decision_unique ON reward_redemption_events(proposal_id)
    WHERE proposal_id IS NOT NULL;
CREATE TRIGGER reward_redemption_events_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON reward_redemption_events
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_redemption_reject_mutation();

CREATE VIEW reward_redemption_state WITH (security_invoker=true) AS
SELECT d.id,d.couple_id,d.product_id,d.product_version,d.creator_id,d.redeemer_id,
       d.title,d.description,d.price_flowers,d.redemption_mode,
       COALESCE(d.operation_id,c.operation_id) AS operation_id,
       d.idempotency_key,d.request_fingerprint,d.created_at,
       CASE WHEN f.redemption_id IS NOT NULL THEN 'fulfilled'
            WHEN x.id IS NOT NULL THEN 'cancelled'
            WHEN d.operation_id IS NOT NULL OR c.id IS NOT NULL THEN 'pending_fulfillment'
            ELSE 'pending_confirmation' END AS status,
       f.fulfilled_at,f.fulfilled_by,
       CASE x.action WHEN 'reject' THEN 'rejected' WHEN 'withdraw' THEN 'withdrawn'
            WHEN 'cancel_approve' THEN 'mutual' END AS cancel_reason,
       x.created_at AS cancelled_at,
       CASE WHEN f.redemption_id IS NULL AND x.id IS NULL THEN p.id END AS cancellation_id,
       CASE WHEN f.redemption_id IS NULL AND x.id IS NULL THEN p.actor_id END AS cancellation_by
FROM reward_redemptions d
LEFT JOIN reward_redemption_fulfillments f ON f.redemption_id=d.id
LEFT JOIN reward_redemption_events c ON c.redemption_id=d.id AND c.action='confirm'
LEFT JOIN reward_redemption_events x ON x.redemption_id=d.id AND x.action IN ('reject','withdraw','cancel_approve')
LEFT JOIN LATERAL (
    SELECT e.id,e.actor_id FROM reward_redemption_events e
    WHERE e.redemption_id=d.id AND e.action='cancel_propose'
      AND NOT EXISTS (SELECT 1 FROM reward_redemption_events z WHERE z.proposal_id=e.id)
    ORDER BY e.created_at DESC,e.id DESC LIMIT 1
) p ON true;

CREATE OR REPLACE FUNCTION usapp_redemption_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE p reward_products%ROWTYPE; r reward_product_revisions%ROWTYPE; o wallet_operations%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33007::integer,hashtext(NEW.product_id::text));
    SELECT * INTO p FROM reward_products WHERE id=NEW.product_id;
    IF NOT FOUND OR ROW(p.couple_id,p.creator_id,p.owner_id) IS DISTINCT FROM
        ROW(NEW.couple_id,NEW.creator_id,NEW.redeemer_id) THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_ACTOR';
    END IF;
    SELECT * INTO r FROM reward_product_revisions WHERE product_id=NEW.product_id ORDER BY version DESC LIMIT 1;
    IF NOT FOUND OR r.status <> 'active' OR r.version IS DISTINCT FROM NEW.product_version
       OR ROW(NEW.title,NEW.description,NEW.price_flowers,NEW.redemption_mode) IS DISTINCT FROM
          ROW(r.title,r.description,r.price_flowers,r.redemption_mode) THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_SNAPSHOT';
    END IF;
    IF NEW.redemption_mode='direct' THEN
        SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id;
        IF NOT FOUND OR ROW(o.operation_type,o.source_type,o.source_id,o.actor_user_id,o.couple_id)
            IS DISTINCT FROM ROW('REWARD_REDEMPTION'::text,'reward_redemption'::text,NEW.id::text,NEW.redeemer_id,NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_OPERATION';
        END IF;
    ELSIF NEW.redemption_mode <> 'request' OR NEW.operation_id IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_OPERATION';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < r.created_at THEN
        RAISE EXCEPTION USING ERRCODE='UM002',MESSAGE='USAPP_REDEMPTION_TIME';
    END IF;
    RETURN NEW;
END $guard$;

CREATE FUNCTION usapp_redemption_event_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE s reward_redemption_state%ROWTYPE; p reward_redemption_events%ROWTYPE; o wallet_operations%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33008::integer,hashtext(NEW.redemption_id::text));
    SELECT * INTO s FROM reward_redemption_state WHERE id=NEW.redemption_id;
    IF NOT FOUND OR s.couple_id IS DISTINCT FROM NEW.couple_id
       OR NEW.actor_id NOT IN (s.creator_id,s.redeemer_id) THEN
        RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_ACTOR';
    END IF;
    IF NEW.created_at < s.created_at OR NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_TIME';
    END IF;
    IF NEW.action IN ('confirm','reject','withdraw') THEN
        IF s.status <> 'pending_confirmation' OR
           (NEW.action IN ('confirm','reject') AND NEW.actor_id <> s.creator_id) OR
           (NEW.action='withdraw' AND NEW.actor_id <> s.redeemer_id) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_STATE';
        END IF;
    ELSIF NEW.action='cancel_propose' THEN
        IF s.status <> 'pending_fulfillment' OR s.cancellation_id IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_CANCEL_PENDING';
        END IF;
    ELSE
        SELECT * INTO p FROM reward_redemption_events WHERE id=NEW.proposal_id;
        IF NOT FOUND OR s.status <> 'pending_fulfillment' OR s.cancellation_id IS DISTINCT FROM NEW.proposal_id
           OR p.redemption_id IS DISTINCT FROM NEW.redemption_id OR p.action <> 'cancel_propose'
           OR NEW.created_at < p.created_at
           OR (NEW.action='cancel_withdraw' AND NEW.actor_id <> p.actor_id)
           OR (NEW.action IN ('cancel_approve','cancel_reject') AND NEW.actor_id=p.actor_id) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_CANCEL_DECISION';
        END IF;
    END IF;
    IF NEW.operation_id IS NOT NULL THEN
        SELECT * INTO o FROM wallet_operations WHERE id=NEW.operation_id;
        IF NOT FOUND OR o.couple_id IS DISTINCT FROM NEW.couple_id OR o.source_type <> 'reward_redemption'
           OR o.source_id IS DISTINCT FROM NEW.redemption_id::text OR o.actor_user_id IS DISTINCT FROM NEW.actor_id
           OR o.operation_type IS DISTINCT FROM (CASE WHEN NEW.action='confirm' THEN 'REWARD_REDEMPTION' ELSE 'REDEMPTION_REFUND' END) THEN
            RAISE EXCEPTION USING ERRCODE='UN001',MESSAGE='USAPP_REDEMPTION_EVENT_OPERATION';
        END IF;
    END IF;
    RETURN NEW;
END $guard$;
CREATE TRIGGER reward_redemption_events_guard BEFORE INSERT ON reward_redemption_events
    FOR EACH ROW EXECUTE FUNCTION usapp_redemption_event_guard();

CREATE OR REPLACE FUNCTION usapp_redemption_fulfillment_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE AS $guard$
DECLARE s reward_redemption_state%ROWTYPE;
BEGIN
    PERFORM pg_advisory_xact_lock(33008::integer,hashtext(NEW.redemption_id::text));
    SELECT * INTO s FROM reward_redemption_state WHERE id=NEW.redemption_id;
    IF NOT FOUND OR s.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.fulfilled_by IS DISTINCT FROM s.creator_id THEN
        RAISE EXCEPTION USING ERRCODE='UM004',MESSAGE='USAPP_FULFILLMENT_ACTOR';
    END IF;
    IF s.status <> 'pending_fulfillment' OR NEW.fulfilled_at < s.created_at OR NEW.fulfilled_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE='UM004',MESSAGE='USAPP_FULFILLMENT_STATE';
    END IF;
    RETURN NEW;
END $guard$;

CREATE OR REPLACE FUNCTION usapp_redemption_validate(p_operation_id uuid)
RETURNS void LANGUAGE plpgsql AS $validate$
DECLARE o wallet_operations%ROWTYPE; d reward_redemptions%ROWTYPE; n bigint; matching bigint; wanted bigint;
BEGIN
    IF p_operation_id IS NULL THEN RETURN; END IF;
    SELECT * INTO o FROM wallet_operations WHERE id=p_operation_id;
    IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE='UM003',MESSAGE='USAPP_REDEMPTION_LINK_OPERATION'; END IF;
    IF o.operation_type='REWARD_REDEMPTION' THEN
        SELECT count(*) INTO n FROM (
            SELECT id FROM reward_redemptions WHERE operation_id=o.id
            UNION ALL SELECT redemption_id FROM reward_redemption_events WHERE operation_id=o.id AND action='confirm'
        ) linked;
        SELECT r.* INTO d FROM reward_redemptions r WHERE r.operation_id=o.id OR EXISTS (
            SELECT 1 FROM reward_redemption_events e WHERE e.redemption_id=r.id AND e.operation_id=o.id AND e.action='confirm');
        wanted := -d.price_flowers::bigint;
    ELSIF o.operation_type='REDEMPTION_REFUND' AND o.source_type='reward_redemption' THEN
        SELECT count(*) INTO n FROM reward_redemption_events WHERE operation_id=o.id AND action='cancel_approve';
        SELECT r.* INTO d FROM reward_redemptions r JOIN reward_redemption_events e ON e.redemption_id=r.id
            WHERE e.operation_id=o.id AND e.action='cancel_approve';
        wanted := d.price_flowers::bigint;
    ELSE RETURN;
    END IF;
    IF n <> 1 OR d.id IS NULL THEN
        RAISE EXCEPTION USING ERRCODE='UM003',MESSAGE='USAPP_REDEMPTION_LINK_REDEMPTION';
    END IF;
    SELECT count(*),count(*) FILTER (WHERE user_id=d.redeemer_id AND asset_type='flower' AND amount=wanted)
      INTO n,matching FROM wallet_ledger_entries WHERE operation_id=o.id;
    IF n <> 1 OR matching <> 1 THEN
        RAISE EXCEPTION USING ERRCODE='UM003',MESSAGE='USAPP_REDEMPTION_LINK_ENTRIES';
    END IF;
END $validate$;

CREATE CONSTRAINT TRIGGER reward_redemption_events_ledger_valid AFTER INSERT ON reward_redemption_events
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_redemption_link_check();
CREATE CONSTRAINT TRIGGER wallet_operations_refund_valid AFTER INSERT ON wallet_operations
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW
    WHEN (NEW.operation_type='REDEMPTION_REFUND' AND NEW.source_type='reward_redemption')
    EXECUTE FUNCTION usapp_redemption_link_check();

REVOKE ALL ON reward_redemption_events,reward_redemption_state FROM PUBLIC,usapp_api,usapp_push;
GRANT SELECT,INSERT ON reward_redemption_events TO usapp_api;
GRANT SELECT ON reward_redemption_state TO usapp_api;
COMMIT;
