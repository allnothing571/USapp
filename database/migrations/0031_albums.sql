-- =====================================================================
-- 0031_albums: Phase 3 P3-2a shared albums (docs/phase3-p32-albums-design.md revision 2). 0000-0030 stay frozen.
--
-- albums                 immutable identity
-- album_texts            the name, referenced by revisions. Held apart so that the deletion decision (D6) can act on text alone
-- album_revisions        create; the highest version is current. The 'edit' rule ships here, its service path is P3-2b
-- album_photo_batches    the identity of one add: its key and fingerprint, once
-- album_items            each photo's one place in its album. Highest seq sorts first
-- album_item_removals    an uploader's removal. Hides the item; the binding and the bytes stay until D6
-- album_cover_events     a member's chosen cover; the latest wins while that photo is active
--
-- A photo's one home is its media_bindings row, now with target kind 'album'. Nothing is ever rebound, and a removal releases
-- nothing: a bound object is never eligible for cleanup. usapp_media_shared_now is the partner's read rule: memory-bound photos
-- for good, as before; album photos only while their item is active.
--
-- Replaces one 0030 function body (the binding guard) and one 0029 CHECK (the target kinds). The plan guard, the release clock,
-- the key disposition and every memory guard are unchanged.
-- Lock order: member FOR SHARE -> 33021 album -> sorted 33018 media. Memory writes take 33020 and never 33021; albums never take
-- 33020; completion and cleanup hold only 33018. No guard takes 33021 after 33018.
-- SQLSTATE: UH001 append-only; UH002 revision; UH003 batch and item; UH004 removal; UH005 cover; UH006 deferred links.
-- =====================================================================

BEGIN;

DO $album_preflight$
BEGIN
    IF to_regclass('public.memories') IS NULL THEN
        RAISE EXCEPTION '0031_albums requires 0030_memories';
    END IF;
END
$album_preflight$;

CREATE TABLE albums (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    creator_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT albums_pkey PRIMARY KEY (id),
    CONSTRAINT albums_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT albums_creator_fk FOREIGN KEY (couple_id, creator_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT
);
CREATE INDEX albums_couple_idx ON albums (couple_id, created_at DESC, id DESC);

CREATE TABLE album_texts (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    name text NOT NULL,
    CONSTRAINT album_texts_pkey PRIMARY KEY (id),
    CONSTRAINT album_texts_id_couple_unique UNIQUE (id, couple_id),
    CONSTRAINT album_texts_name_shape CHECK (
        char_length(name) BETWEEN 1 AND 40
        AND name = btrim(name)
        AND name !~ '[\x01-\x1f\x7f-\x9f  ]'
    )
);

CREATE TABLE album_revisions (
    album_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    version integer NOT NULL,
    actor_id uuid NOT NULL,
    action text NOT NULL,
    text_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_revisions_pkey PRIMARY KEY (album_id, version),
    CONSTRAINT album_revisions_key_unique UNIQUE (actor_id, idempotency_key),
    CONSTRAINT album_revisions_identity_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_revisions_member_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_revisions_text_fk FOREIGN KEY (text_id, couple_id)
        REFERENCES album_texts (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_revisions_version_valid CHECK (version >= 1),
    CONSTRAINT album_revisions_action_valid CHECK (action IN ('create', 'edit')),
    CONSTRAINT album_revisions_create_first CHECK ((action = 'create') = (version = 1)),
    CONSTRAINT album_revisions_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_revisions_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE album_photo_batches (
    id uuid NOT NULL DEFAULT gen_random_uuid(),
    album_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    added_by uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_photo_batches_pkey PRIMARY KEY (id),
    CONSTRAINT album_photo_batches_id_album_unique UNIQUE (id, album_id),
    CONSTRAINT album_photo_batches_key_unique UNIQUE (added_by, idempotency_key),
    CONSTRAINT album_photo_batches_album_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_photo_batches_member_fk FOREIGN KEY (couple_id, added_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_photo_batches_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_photo_batches_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE album_items (
    album_id uuid NOT NULL,
    seq integer NOT NULL,
    couple_id uuid NOT NULL,
    media_id uuid NOT NULL,
    batch_id uuid NOT NULL,
    added_by uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_items_pkey PRIMARY KEY (album_id, seq),
    CONSTRAINT album_items_album_media_unique UNIQUE (album_id, media_id),
    -- One home, even after removal.
    CONSTRAINT album_items_media_unique UNIQUE (media_id),
    CONSTRAINT album_items_album_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_items_batch_fk FOREIGN KEY (batch_id, album_id)
        REFERENCES album_photo_batches (id, album_id) ON DELETE RESTRICT,
    -- Database-level same couple.
    CONSTRAINT album_items_media_fk FOREIGN KEY (media_id, couple_id)
        REFERENCES media_objects (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_items_member_fk FOREIGN KEY (couple_id, added_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_items_seq_valid CHECK (seq >= 1)
);
CREATE INDEX album_items_batch_idx ON album_items (batch_id);

CREATE TABLE album_item_removals (
    album_id uuid NOT NULL,
    media_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    removed_by uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    removed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_item_removals_pkey PRIMARY KEY (album_id, media_id),
    CONSTRAINT album_item_removals_key_unique UNIQUE (removed_by, idempotency_key),
    CONSTRAINT album_item_removals_item_fk FOREIGN KEY (album_id, media_id)
        REFERENCES album_items (album_id, media_id) ON DELETE RESTRICT,
    CONSTRAINT album_item_removals_album_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_item_removals_member_fk FOREIGN KEY (couple_id, removed_by)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_item_removals_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_item_removals_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

CREATE TABLE album_cover_events (
    album_id uuid NOT NULL,
    event_no integer NOT NULL,
    couple_id uuid NOT NULL,
    media_id uuid NOT NULL,
    actor_id uuid NOT NULL,
    idempotency_key text NOT NULL,
    request_fingerprint text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT album_cover_events_pkey PRIMARY KEY (album_id, event_no),
    CONSTRAINT album_cover_events_key_unique UNIQUE (actor_id, idempotency_key),
    CONSTRAINT album_cover_events_item_fk FOREIGN KEY (album_id, media_id)
        REFERENCES album_items (album_id, media_id) ON DELETE RESTRICT,
    CONSTRAINT album_cover_events_album_fk FOREIGN KEY (album_id, couple_id)
        REFERENCES albums (id, couple_id) ON DELETE RESTRICT,
    CONSTRAINT album_cover_events_member_fk FOREIGN KEY (couple_id, actor_id)
        REFERENCES couple_members (couple_id, user_id) ON DELETE RESTRICT,
    CONSTRAINT album_cover_events_event_valid CHECK (event_no >= 1),
    CONSTRAINT album_cover_events_key_shape CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    CONSTRAINT album_cover_events_fingerprint_shape CHECK (request_fingerprint ~ '^[0-9a-f]{64}$')
);

-- ---------------------------------------------------------------- append-only
CREATE FUNCTION usapp_album_reject_mutation() RETURNS trigger LANGUAGE plpgsql AS $album_reject$
BEGIN
    RAISE EXCEPTION USING ERRCODE = 'UH001', MESSAGE = 'USAPP_ALBUM_APPEND_ONLY',
        DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
END
$album_reject$;
CREATE TRIGGER albums_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON albums
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_texts_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_texts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_revisions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_photo_batches_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_photo_batches
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_items_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_items
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_item_removals_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_item_removals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();
CREATE TRIGGER album_cover_events_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON album_cover_events
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_album_reject_mutation();

-- ---------------------------------------------------------------- the partner's read rule
CREATE FUNCTION usapp_media_shared_now(p_media uuid) RETURNS boolean
LANGUAGE sql STABLE AS $shared_now$
    SELECT EXISTS (SELECT 1 FROM media_bindings b
                    WHERE b.media_id = p_media AND b.target_kind = 'memory')
        OR EXISTS (SELECT 1 FROM album_items i
                     JOIN media_bindings b ON b.media_id = i.media_id
                                          AND b.target_kind = 'album'
                                          AND b.target_id = i.album_id
                    WHERE i.media_id = p_media
                      AND NOT EXISTS (SELECT 1 FROM album_item_removals r
                                       WHERE r.album_id = i.album_id AND r.media_id = i.media_id))
$shared_now$;

-- ---------------------------------------------------------------- media bindings
ALTER TABLE media_bindings DROP CONSTRAINT media_bindings_kind_valid;
ALTER TABLE media_bindings ADD CONSTRAINT media_bindings_kind_valid CHECK (target_kind IN ('memory', 'album'));

-- The 0030 body, in the same order, with the checks after TIME branching on kind. A memory binding takes no album lock and meets
-- exactly the 0030 rules. An album binding locks the album first; its target is an album of this couple; only the uploader may
-- bind, whatever a proposal says; and a reserved photo is refused outright.
CREATE OR REPLACE FUNCTION usapp_media_binding_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $binding_guard$
DECLARE
    m media_objects%ROWTYPE;
    reserved uuid;
BEGIN
    IF NEW.media_id IS NULL OR NEW.created_at IS NULL OR NEW.target_kind IS NULL OR NEW.target_id IS NULL THEN
        RETURN NEW;
    END IF;
    IF NEW.target_kind = 'album' THEN
        PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.target_id::text));
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    IF NOT EXISTS (SELECT 1 FROM media_upload_outcomes t WHERE t.media_id = NEW.media_id AND t.outcome = 'ready') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_NOT_READY';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_CLEANUP';
    END IF;
    IF NEW.created_at > clock_timestamp() THEN
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TIME';
    END IF;
    SELECT * INTO m FROM media_objects WHERE id = NEW.media_id;
    reserved := usapp_media_pending_reference(NEW.media_id);
    IF NEW.target_kind = 'memory' THEN
        IF NOT EXISTS (SELECT 1 FROM memories x WHERE x.id = NEW.target_id AND x.couple_id = NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
        END IF;
        IF m.uploader_id IS DISTINCT FROM NEW.bound_by
           AND NOT EXISTS (SELECT 1 FROM memory_proposal_photos x
                             JOIN memory_edit_proposals p ON p.id = x.proposal_id
                             JOIN memory_edit_decisions d ON d.proposal_id = p.id
                            WHERE x.media_id = NEW.media_id AND p.memory_id = NEW.target_id
                              AND d.outcome = 'approved' AND d.decided_by = NEW.bound_by) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_ACTOR';
        END IF;
        IF reserved IS NOT NULL AND NOT EXISTS (SELECT 1 FROM memory_edit_proposals p WHERE p.id = reserved AND p.memory_id = NEW.target_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_RESERVED';
        END IF;
    ELSIF NEW.target_kind = 'album' THEN
        IF NOT EXISTS (SELECT 1 FROM albums x WHERE x.id = NEW.target_id AND x.couple_id = NEW.couple_id) THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
        END IF;
        IF m.uploader_id IS DISTINCT FROM NEW.bound_by THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_ACTOR';
        END IF;
        IF reserved IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_RESERVED';
        END IF;
    ELSE
        RAISE EXCEPTION USING ERRCODE = 'UF007', MESSAGE = 'USAPP_MEDIA_BINDING_TARGET';
    END IF;
    RETURN NEW;
END
$binding_guard$;

-- ---------------------------------------------------------------- album guards. 33021 always before 33018.
CREATE FUNCTION usapp_album_revision_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $revision_guard$
DECLARE
    x albums%ROWTYPE;
    top integer;
BEGIN
    IF NEW.album_id IS NULL OR NEW.couple_id IS NULL OR NEW.version IS NULL OR NEW.actor_id IS NULL OR NEW.action IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    SELECT * INTO x FROM albums WHERE id = NEW.album_id;
    IF NOT FOUND OR x.couple_id IS DISTINCT FROM NEW.couple_id OR NEW.actor_id IS DISTINCT FROM x.creator_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_ACTOR';
    END IF;
    SELECT max(version) INTO top FROM album_revisions WHERE album_id = NEW.album_id;
    IF NEW.version::bigint <> COALESCE(top, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH002', MESSAGE = 'USAPP_ALBUM_VERSION';
    END IF;
    RETURN NEW;
END
$revision_guard$;
CREATE TRIGGER album_revisions_guard BEFORE INSERT ON album_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_album_revision_guard();

CREATE FUNCTION usapp_album_batch_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $batch_guard$
BEGIN
    IF NEW.album_id IS NULL OR NEW.couple_id IS NULL OR NEW.added_by IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    IF NOT EXISTS (SELECT 1 FROM albums x JOIN couple_members c ON c.couple_id = x.couple_id
                    WHERE x.id = NEW.album_id AND x.couple_id = NEW.couple_id AND c.user_id = NEW.added_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH003', MESSAGE = 'USAPP_ALBUM_BATCH_TARGET';
    END IF;
    RETURN NEW;
END
$batch_guard$;
CREATE TRIGGER album_photo_batches_guard BEFORE INSERT ON album_photo_batches
    FOR EACH ROW EXECUTE FUNCTION usapp_album_batch_guard();

-- An item arrives in its batch's own transaction, after its binding: READY, unplanned, unreserved and the couple's own are all
-- established by the binding guard. The batch's xmin rule fails closed inside a savepoint, as in 0030.
CREATE FUNCTION usapp_album_item_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $item_guard$
DECLARE
    b album_photo_batches%ROWTYPE;
    top integer;
BEGIN
    IF NEW.album_id IS NULL OR NEW.seq IS NULL OR NEW.couple_id IS NULL OR NEW.media_id IS NULL OR NEW.batch_id IS NULL OR NEW.added_by IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO b FROM album_photo_batches WHERE id = NEW.batch_id AND xmin = pg_current_xact_id()::xid;
    IF NOT FOUND OR b.album_id IS DISTINCT FROM NEW.album_id OR b.couple_id IS DISTINCT FROM NEW.couple_id
       OR b.added_by IS DISTINCT FROM NEW.added_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UH003', MESSAGE = 'USAPP_ALBUM_ITEM_BATCH';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_bindings x WHERE x.media_id = NEW.media_id AND x.couple_id = NEW.couple_id
                      AND x.target_kind = 'album' AND x.target_id = NEW.album_id AND x.bound_by = NEW.added_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH003', MESSAGE = 'USAPP_ALBUM_ITEM_UNBOUND';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_objects m WHERE m.id = NEW.media_id AND m.uploader_id = NEW.added_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH003', MESSAGE = 'USAPP_ALBUM_ITEM_ACTOR';
    END IF;
    SELECT max(seq) INTO top FROM album_items WHERE album_id = NEW.album_id;
    IF NEW.seq::bigint <> COALESCE(top, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH003', MESSAGE = 'USAPP_ALBUM_ITEM_SEQ';
    END IF;
    RETURN NEW;
END
$item_guard$;
CREATE TRIGGER album_items_guard BEFORE INSERT ON album_items
    FOR EACH ROW EXECUTE FUNCTION usapp_album_item_guard();

-- The uploader only. Removal changes visibility, not bytes, so it takes no media lock.
CREATE FUNCTION usapp_album_removal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $removal_guard$
BEGIN
    IF NEW.album_id IS NULL OR NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.removed_by IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    IF NOT EXISTS (SELECT 1 FROM album_items i WHERE i.album_id = NEW.album_id AND i.media_id = NEW.media_id AND i.couple_id = NEW.couple_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH004', MESSAGE = 'USAPP_ALBUM_REMOVAL_ITEM';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_objects m WHERE m.id = NEW.media_id AND m.uploader_id = NEW.removed_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH004', MESSAGE = 'USAPP_ALBUM_REMOVAL_ACTOR';
    END IF;
    RETURN NEW;
END
$removal_guard$;
CREATE TRIGGER album_item_removals_guard BEFORE INSERT ON album_item_removals
    FOR EACH ROW EXECUTE FUNCTION usapp_album_removal_guard();

-- Either member; only an active item; contiguous events. A removal takes the same lock, so a cover never names a removed item.
CREATE FUNCTION usapp_album_cover_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $cover_guard$
DECLARE
    top integer;
BEGIN
    IF NEW.album_id IS NULL OR NEW.event_no IS NULL OR NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.actor_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33021::integer, hashtext(NEW.album_id::text));
    IF NOT EXISTS (SELECT 1 FROM album_items i WHERE i.album_id = NEW.album_id AND i.media_id = NEW.media_id AND i.couple_id = NEW.couple_id
                      AND NOT EXISTS (SELECT 1 FROM album_item_removals r WHERE r.album_id = i.album_id AND r.media_id = i.media_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH005', MESSAGE = 'USAPP_ALBUM_COVER_ITEM';
    END IF;
    SELECT max(event_no) INTO top FROM album_cover_events WHERE album_id = NEW.album_id;
    IF NEW.event_no::bigint <> COALESCE(top, 0)::bigint + 1 THEN
        RAISE EXCEPTION USING ERRCODE = 'UH005', MESSAGE = 'USAPP_ALBUM_COVER_EVENT';
    END IF;
    RETURN NEW;
END
$cover_guard$;
CREATE TRIGGER album_cover_events_guard BEFORE INSERT ON album_cover_events
    FOR EACH ROW EXECUTE FUNCTION usapp_album_cover_guard();

-- ---------------------------------------------------------------- deferred links, checked at commit
-- 1 to 20 photos in one add.
CREATE FUNCTION usapp_album_batch_valid(n bigint) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $batch_valid$
    SELECT n BETWEEN 1 AND 20
$batch_valid$;

CREATE FUNCTION usapp_album_link_check() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $album_link$
BEGIN
    IF TG_TABLE_NAME = 'albums' THEN
        IF NOT EXISTS (SELECT 1 FROM album_revisions r WHERE r.album_id = NEW.id AND r.version = 1 AND r.action = 'create') THEN
            RAISE EXCEPTION USING ERRCODE = 'UH006', MESSAGE = 'USAPP_ALBUM_CREATE_REVISION';
        END IF;
    ELSIF NOT (SELECT usapp_album_batch_valid(count(*)) FROM album_items i WHERE i.batch_id = NEW.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH006', MESSAGE = 'USAPP_ALBUM_BATCH_ITEMS';
    END IF;
    RETURN NULL;
END
$album_link$;
CREATE CONSTRAINT TRIGGER albums_create_revision AFTER INSERT ON albums
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_album_link_check();
CREATE CONSTRAINT TRIGGER album_photo_batches_items AFTER INSERT ON album_photo_batches
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_album_link_check();

-- Never queued for a memory binding: WHEN is evaluated when the row is inserted.
CREATE FUNCTION usapp_album_binding_item_check() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $check$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM album_items i
                    WHERE i.album_id = NEW.target_id AND i.media_id = NEW.media_id
                      AND i.couple_id = NEW.couple_id AND i.added_by = NEW.bound_by) THEN
        RAISE EXCEPTION USING ERRCODE = 'UH006', MESSAGE = 'USAPP_ALBUM_BINDING_ITEM';
    END IF;
    RETURN NULL;
END
$check$;

CREATE CONSTRAINT TRIGGER media_bindings_album_item AFTER INSERT ON media_bindings
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW
    WHEN (NEW.target_kind = 'album')
    EXECUTE FUNCTION usapp_album_binding_item_check();

REVOKE ALL ON albums, album_texts, album_revisions, album_photo_batches, album_items, album_item_removals,
    album_cover_events FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON albums, album_texts, album_revisions, album_photo_batches, album_items, album_item_removals,
    album_cover_events TO usapp_api;

COMMIT;
