-- =====================================================================
-- 0017_messages: Phase 2 P2-1 partner message board (docs/phase2-messages.md).
--
-- Two append-only tables: messages (author -> partner) and message_replies (at most
-- one reply per message, written by the message's recipient). usapp_api gets SELECT,
-- INSERT only; UPDATE/DELETE/TRUNCATE are rejected by UB001 for every role.
-- No notification tables: P2-3 consumes the stable ids and audit rows later.
-- =====================================================================

BEGIN;

DO $message_preflight$
BEGIN
    IF to_regclass('public.reward_redemption_events') IS NULL THEN
        RAISE EXCEPTION '0017_messages requires 0016_redemption_workflow';
    END IF;
END
$message_preflight$;

CREATE TABLE messages (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    author_id uuid NOT NULL,
    recipient_id uuid NOT NULL,
    body text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT messages_pkey PRIMARY KEY (id),
    CONSTRAINT messages_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT messages_key_unique UNIQUE (author_id, idempotency_key),
    CONSTRAINT messages_author_fk FOREIGN KEY (couple_id, author_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT messages_recipient_fk FOREIGN KEY (couple_id, recipient_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- A space has two members, so a different member is the partner.
    CONSTRAINT messages_author_not_recipient CHECK (author_id <> recipient_id),
    -- Plain text, 1-500 code points, LF allowed; same character rules as task descriptions.
    CONSTRAINT messages_body_shape CHECK (
        char_length(body) BETWEEN 1 AND 500
        AND body = btrim(body, E' \n')
        AND body !~ '[\x01-\x09\x0b-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT messages_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT messages_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE INDEX messages_couple_idx ON messages (couple_id, created_at DESC, id DESC);
CREATE INDEX messages_recipient_idx ON messages (recipient_id, created_at DESC, id DESC);

CREATE TABLE message_replies (
    message_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    author_id uuid NOT NULL,
    body text NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- One reply per message ("单条回应").
    CONSTRAINT message_replies_pkey PRIMARY KEY (message_id),
    CONSTRAINT message_replies_key_unique UNIQUE (author_id, idempotency_key),
    CONSTRAINT message_replies_message_fk FOREIGN KEY (message_id, couple_id)
        REFERENCES messages (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT message_replies_author_fk FOREIGN KEY (couple_id, author_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    -- One sentence: single line, 1-100 code points.
    CONSTRAINT message_replies_body_shape CHECK (
        char_length(body) BETWEEN 1 AND 100
        AND body = btrim(body)
        AND body !~ '[\x01-\x1f\x7f-\x9f  ]'
    ),
    CONSTRAINT message_replies_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT message_replies_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE FUNCTION usapp_message_reject_mutation()
RETURNS trigger LANGUAGE plpgsql
AS $message_reject$
BEGIN
    RAISE EXCEPTION USING
        ERRCODE = 'UB001',
        MESSAGE = 'USAPP_MESSAGE_APPEND_ONLY',
        DETAIL  = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$message_reject$;

CREATE TRIGGER messages_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON messages
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_message_reject_mutation();
CREATE TRIGGER message_replies_append_only
    BEFORE UPDATE OR DELETE OR TRUNCATE ON message_replies
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_message_reject_mutation();

CREATE FUNCTION usapp_message_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $message_guard$
BEGIN
    IF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_TIME';
    END IF;
    RETURN NEW;
END
$message_guard$;

CREATE TRIGGER messages_guard BEFORE INSERT ON messages
    FOR EACH ROW EXECUTE FUNCTION usapp_message_guard();

-- Only the message's recipient replies, never before the message and never in the future.
CREATE FUNCTION usapp_message_reply_guard()
RETURNS trigger LANGUAGE plpgsql VOLATILE
AS $message_reply_guard$
DECLARE
    m messages%ROWTYPE;
BEGIN
    IF NEW.message_id IS NULL OR NEW.couple_id IS NULL OR NEW.author_id IS NULL OR NEW.created_at IS NULL THEN
        RETURN NEW;
    END IF;
    SELECT * INTO m FROM messages WHERE id = NEW.message_id;
    IF NOT FOUND OR m.couple_id IS DISTINCT FROM NEW.couple_id OR m.recipient_id IS DISTINCT FROM NEW.author_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_REPLY_ACTOR';
    END IF;
    IF NEW.created_at > clock_timestamp() OR NEW.created_at < m.created_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UB002', MESSAGE = 'USAPP_MESSAGE_REPLY_TIME';
    END IF;
    RETURN NEW;
END
$message_reply_guard$;

CREATE TRIGGER message_replies_guard BEFORE INSERT ON message_replies
    FOR EACH ROW EXECUTE FUNCTION usapp_message_reply_guard();

REVOKE ALL ON messages, message_replies FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON messages, message_replies TO usapp_api;

COMMIT;
