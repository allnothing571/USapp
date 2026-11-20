-- =====================================================================
-- 0038_memory_photo_cascade: D6 whole-memory deletion, step M1c (docs/phase3-d6-memory-deletion-design.md). 0000-0037 stay frozen.
--
-- memory_deletion_photo_cycles  one media deletion cycle made by one memory deletion cycle. Never changes
--
-- Deleting a memory also deletes every photo its creator uploaded that is bound to it and not already deleted, including photos only its
-- history still holds. Each is an ordinary media cycle with the memory cycle's exact deleted_at and purge_after, so the existing media job
-- plans and sweeps it after the deadline, by the existing rules. The partner's photos are never touched.
--
-- An undo of the memory cycle undoes exactly its linked cycles, in one transaction, or nothing: a linked cycle that can no longer be undone
-- fails the whole undo. A linked cycle cannot be undone by itself. A photo's independent cycle is never linked, and an undo leaves it alone.
--
-- Cycles from before this migration have no links and get none: they are not cascaded and nothing is backdated. Their memory stays deleted, their
-- creator's photos stay readable by the creator and appear in the creator's retained list, and their undo is the 0036 undo. See Section 12.
--
-- Replaces one function body: usapp_media_reversal_guard (0035), with one more refusal.
--
-- Lock order: member FOR SHARE -> 33020 memory -> sorted 33018 media, taken by the caller before the memory cycle's row, so that the cycle's
-- time cannot precede an undo of one of these photos. The media job holds only 33018. No new lock class.
-- SQLSTATE: UE007 cascade. The replaced guard keeps UL003.
-- =====================================================================

BEGIN;

DO $memory_photo_cascade_preflight$
BEGIN
    IF to_regclass('public.memory_delete_decisions') IS NULL THEN
        RAISE EXCEPTION '0038_memory_photo_cascade requires 0037_memory_delete_requests';
    END IF;
END
$memory_photo_cascade_preflight$;

CREATE TABLE memory_deletion_photo_cycles (
    media_deletion_id uuid NOT NULL,
    media_id uuid NOT NULL,
    memory_deletion_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    CONSTRAINT memory_deletion_photo_cycles_pkey PRIMARY KEY (media_deletion_id),
    CONSTRAINT memory_deletion_photo_cycles_pair_unique UNIQUE (memory_deletion_id, media_id),
    CONSTRAINT memory_deletion_photo_cycles_media_fk FOREIGN KEY (media_deletion_id, media_id)
        REFERENCES media_deletions (id, media_id) ON DELETE RESTRICT,
    CONSTRAINT memory_deletion_photo_cycles_memory_fk FOREIGN KEY (memory_deletion_id, memory_id)
        REFERENCES memory_deletions (id, memory_id) ON DELETE RESTRICT
);
CREATE INDEX memory_deletion_photo_cycles_media_idx ON memory_deletion_photo_cycles (media_id);

CREATE TRIGGER memory_deletion_photo_cycles_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_deletion_photo_cycles
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();

-- ---------------------------------------------------------------- the link guard
-- Both cycles written by this transaction, with one time, one deadline and one actor; a photo of that actor's, bound to that memory.
CREATE FUNCTION usapp_memory_photo_cycle_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $photo_cycle_guard$
DECLARE
    x memory_deletions%ROWTYPE;
    d media_deletions%ROWTYPE;
BEGIN
    IF NEW.media_deletion_id IS NULL OR NEW.media_id IS NULL OR NEW.memory_deletion_id IS NULL OR NEW.memory_id IS NULL OR NEW.couple_id IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO x FROM memory_deletions WHERE id = NEW.memory_deletion_id AND xmin = pg_current_xact_id()::xid;
    IF NOT FOUND OR x.memory_id IS DISTINCT FROM NEW.memory_id OR x.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_MEMORY_CYCLE';
    END IF;
    SELECT * INTO d FROM media_deletions WHERE id = NEW.media_deletion_id AND xmin = pg_current_xact_id()::xid;
    IF NOT FOUND OR d.media_id IS DISTINCT FROM NEW.media_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_MEDIA_CYCLE';
    END IF;
    IF d.deleted_at IS DISTINCT FROM x.deleted_at OR d.purge_after IS DISTINCT FROM x.purge_after OR d.deleted_by IS DISTINCT FROM x.deleted_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_MISMATCH';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM media_bindings b WHERE b.media_id = NEW.media_id AND b.target_kind = 'memory' AND b.target_id = NEW.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_BINDING';
    END IF;
    RETURN NEW;
END
$photo_cycle_guard$;
CREATE TRIGGER memory_deletion_photo_cycles_guard BEFORE INSERT ON memory_deletion_photo_cycles
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_photo_cycle_guard();

-- ---------------------------------------------------------------- the two transaction steps
-- Called with the locks held, after the memory cycle's row. Keys and fingerprints derive from the two ids, so a repeat cannot create a second cycle.
CREATE FUNCTION usapp_memory_cascade_photos(p_deletion uuid) RETURNS integer LANGUAGE plpgsql VOLATILE AS $cascade_photos$
DECLARE
    x memory_deletions%ROWTYPE;
    photo uuid;
    cycle uuid;
    made integer := 0;
BEGIN
    SELECT * INTO x FROM memory_deletions WHERE id = p_deletion;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_MEMORY_CYCLE';
    END IF;
    FOR photo IN
        SELECT b.media_id FROM media_bindings b JOIN media_objects o ON o.id = b.media_id
         WHERE b.target_kind = 'memory' AND b.target_id = x.memory_id AND o.uploader_id = x.deleted_by
           AND NOT usapp_media_deleted_now(b.media_id)
           AND NOT EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = b.media_id AND p.scope = 'everything')
         ORDER BY hashtext(b.media_id::text)
    LOOP
        INSERT INTO media_deletions (media_id, couple_id, deleted_by, cycle_no, idempotency_key, request_fingerprint, deleted_at, purge_after)
        VALUES (photo, x.couple_id, x.deleted_by,
                1 + COALESCE((SELECT max(d.cycle_no) FROM media_deletions d WHERE d.media_id = photo), 0),
                'mc-' || x.id::text || '-' || photo::text,
                encode(sha256(convert_to('memory-cascade:v1:' || x.id::text || ':' || photo::text, 'UTF8')), 'hex'),
                x.deleted_at, x.purge_after)
        RETURNING id INTO cycle;
        INSERT INTO memory_deletion_photo_cycles (media_deletion_id, media_id, memory_deletion_id, memory_id, couple_id)
        VALUES (cycle, photo, x.id, x.memory_id, x.couple_id);
        made := made + 1;
    END LOOP;
    RETURN made;
END
$cascade_photos$;

-- Called with the locks held, after the memory reversal's row. Any linked cycle that cannot be undone raises, and the whole transaction goes with it.
CREATE FUNCTION usapp_memory_cascade_undo(p_deletion uuid) RETURNS integer LANGUAGE plpgsql VOLATILE AS $cascade_undo$
DECLARE
    x memory_deletions%ROWTYPE;
    link memory_deletion_photo_cycles%ROWTYPE;
    undone integer := 0;
BEGIN
    SELECT * INTO x FROM memory_deletions WHERE id = p_deletion;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_MEMORY_CYCLE';
    END IF;
    FOR link IN
        SELECT l.* FROM memory_deletion_photo_cycles l WHERE l.memory_deletion_id = p_deletion ORDER BY hashtext(l.media_id::text)
    LOOP
        INSERT INTO media_deletion_reversals (deletion_id, media_id, couple_id, reversed_by, idempotency_key, request_fingerprint)
        VALUES (link.media_deletion_id, link.media_id, link.couple_id, x.deleted_by,
                'mu-' || x.id::text || '-' || link.media_id::text,
                encode(sha256(convert_to('memory-cascade-undo:v1:' || x.id::text || ':' || link.media_id::text, 'UTF8')), 'hex'));
        undone := undone + 1;
    END LOOP;
    RETURN undone;
END
$cascade_undo$;

-- ---------------------------------------------------------------- at commit
-- A new cycle leaves none of its creator's photos behind. A cycle from before this migration never meets this: it fires only on a new row.
CREATE FUNCTION usapp_memory_cascade_complete_check() RETURNS trigger LANGUAGE plpgsql AS $cascade_complete$
BEGIN
    IF EXISTS (SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id = NEW.id) THEN
        RETURN NULL;
    END IF;
    IF EXISTS (SELECT 1 FROM media_bindings b JOIN media_objects o ON o.id = b.media_id
                WHERE b.target_kind = 'memory' AND b.target_id = NEW.memory_id AND o.uploader_id = NEW.deleted_by
                  AND NOT usapp_media_deleted_now(b.media_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_INCOMPLETE';
    END IF;
    RETURN NULL;
END
$cascade_complete$;
CREATE CONSTRAINT TRIGGER memory_deletions_cascade_complete AFTER INSERT ON memory_deletions
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_memory_cascade_complete_check();

-- An undo leaves none of its linked cycles behind.
CREATE FUNCTION usapp_memory_cascade_undo_check() RETURNS trigger LANGUAGE plpgsql AS $cascade_undo_check$
BEGIN
    IF EXISTS (SELECT 1 FROM memory_deletion_photo_cycles l
                WHERE l.memory_deletion_id = NEW.deletion_id
                  AND NOT EXISTS (SELECT 1 FROM media_deletion_reversals r WHERE r.deletion_id = l.media_deletion_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE007', MESSAGE = 'USAPP_MEMORY_CASCADE_UNDO_INCOMPLETE';
    END IF;
    RETURN NULL;
END
$cascade_undo_check$;
CREATE CONSTRAINT TRIGGER memory_deletion_reversals_cascade_complete AFTER INSERT ON memory_deletion_reversals
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION usapp_memory_cascade_undo_check();

-- ---------------------------------------------------------------- the replaced guard
-- The 0035 body plus one refusal: a linked cycle is undone only with its memory cycle, whose reversal comes first in that transaction.
CREATE OR REPLACE FUNCTION usapp_media_reversal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $reversal_guard$
DECLARE
    d media_deletions%ROWTYPE;
BEGIN
    IF NEW.deletion_id IS NULL OR NEW.media_id IS NULL OR NEW.couple_id IS NULL OR NEW.reversed_by IS NULL OR NEW.reversed_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33018::integer, hashtext(NEW.media_id::text));
    SELECT * INTO d FROM media_deletions WHERE id = NEW.deletion_id;
    IF NOT FOUND OR d.media_id IS DISTINCT FROM NEW.media_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_TARGET';
    END IF;
    IF d.deleted_by IS DISTINCT FROM NEW.reversed_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_ACTOR';
    END IF;
    IF EXISTS (SELECT 1 FROM memory_deletion_photo_cycles l WHERE l.media_deletion_id = NEW.deletion_id
                  AND NOT EXISTS (SELECT 1 FROM memory_deletion_reversals r WHERE r.deletion_id = l.memory_deletion_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_LINKED';
    END IF;
    IF EXISTS (SELECT 1 FROM media_cleanup_plans p WHERE p.media_id = NEW.media_id AND p.scope = 'everything') THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_PURGE_PLANNED';
    END IF;
    IF NOT usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_WINDOW';
    END IF;
    IF NEW.reversed_at > clock_timestamp() OR NEW.reversed_at < d.deleted_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UL003', MESSAGE = 'USAPP_MEDIA_REVERSAL_TIME';
    END IF;
    RETURN NEW;
END
$reversal_guard$;

REVOKE ALL ON memory_deletion_photo_cycles FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT, INSERT ON memory_deletion_photo_cycles TO usapp_api;

COMMIT;
