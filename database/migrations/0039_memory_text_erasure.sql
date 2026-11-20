-- =====================================================================
-- 0039_memory_text_erasure: D6 whole-memory deletion, step M1d (docs/phase3-d6-memory-deletion-design.md). 0000-0038 stay frozen.
--
-- memory_purges  one irreversible erasure of one memory, naming the exact cycle that expired. Never changes
--
-- This is the repository's first SECURITY DEFINER function. usapp_memory_erase is the only path that can change memory text or a text-derived
-- fingerprint. It overwrites them in place: no row, id, foreign key, date, photo link or audit fact changes, and shared_plan_events, the plan's own
-- text, is not touched. Erased data remains in dead tuples, WAL, replicas and backups until those expire: a production gate, not something this can do.
--
-- Privilege boundary. The function belongs to usapp_memory_eraser, a NOLOGIN role that is no superuser and has no members, with UPDATE on exactly
-- three columns of text and two fingerprints. usapp_api has no UPDATE and may only execute the function. PUBLIC may not. The search_path is fixed and
-- every object is schema-qualified. For anyone else, superusers included, an UPDATE still fails as before. Even the eraser can only write the fixed
-- values, for a memory with a purge record, and that record's own guard requires the exact expired and unreversed cycle.
--
-- A text used by a memory that has not been erased is refused: this erases nothing rather than another memory's words. No code path shares text.
--
-- Replaces one function body: usapp_memory_reversal_guard (0036), with one more refusal. Replaces the append-only triggers on memory_texts,
-- memory_revisions and memory_edit_proposals (0030): DELETE and TRUNCATE are refused as ever, and UPDATE is the narrow path above.
--
-- Lock order: 33020 only. It writes no photo row, so it needs no 33018, and the media job stays on 33018 alone. No new lock class.
-- SQLSTATE: UE006 erasure, as the design reserved. The replaced guard keeps UE003, and the statement refusal keeps UG001.
-- =====================================================================

BEGIN;

DO $memory_text_erasure_preflight$
BEGIN
    IF to_regclass('public.memory_deletion_photo_cycles') IS NULL THEN
        RAISE EXCEPTION '0039_memory_text_erasure requires 0038_memory_photo_cascade';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'usapp_memory_eraser') THEN
        CREATE ROLE usapp_memory_eraser NOLOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS NOINHERIT;
    END IF;
END
$memory_text_erasure_preflight$;

CREATE TABLE memory_purges (
    deletion_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    purged_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT memory_purges_pkey PRIMARY KEY (deletion_id),
    CONSTRAINT memory_purges_memory_unique UNIQUE (memory_id),
    CONSTRAINT memory_purges_deletion_fk FOREIGN KEY (deletion_id, memory_id)
        REFERENCES memory_deletions (id, memory_id) ON DELETE RESTRICT,
    CONSTRAINT memory_purges_memory_fk FOREIGN KEY (memory_id, couple_id)
        REFERENCES memories (id, couple_id) ON DELETE RESTRICT
);

CREATE TRIGGER memory_purges_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON memory_purges
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_deletion_reject_mutation();

-- The record is the proof the narrow path asks for, so it has its own guard: the exact cycle, unreversed, on the database clock.
CREATE FUNCTION usapp_memory_purge_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $purge_guard$
DECLARE
    d public.memory_deletions%ROWTYPE;
BEGIN
    IF NEW.deletion_id IS NULL OR NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.purged_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO d FROM public.memory_deletions WHERE id = NEW.deletion_id;
    IF NOT FOUND OR d.memory_id IS DISTINCT FROM NEW.memory_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_TARGET';
    END IF;
    IF EXISTS (SELECT 1 FROM public.memory_deletion_reversals r WHERE r.deletion_id = NEW.deletion_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_REVERSED';
    END IF;
    IF public.usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_WINDOW';
    END IF;
    RETURN NEW;
END
$purge_guard$;
CREATE TRIGGER memory_purges_guard BEFORE INSERT ON memory_purges
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_purge_guard();

-- ---------------------------------------------------------------- the narrow path
-- As 0030 for everyone but the eraser, even when a statement touches no row.
CREATE FUNCTION usapp_memory_erase_statement_guard() RETURNS trigger LANGUAGE plpgsql AS $erase_statement_guard$
BEGIN
    IF current_user <> 'usapp_memory_eraser' THEN
        RAISE EXCEPTION USING ERRCODE = 'UG001', MESSAGE = 'USAPP_MEMORY_APPEND_ONLY',
            DETAIL = TG_OP || ' on ' || TG_TABLE_NAME || ' is not allowed';
    END IF;
    RETURN NULL;
END
$erase_statement_guard$;

-- Only to the fixed values, for a text of a memory with a purge record. The check constraints still apply.
CREATE FUNCTION usapp_memory_text_erase_guard() RETURNS trigger LANGUAGE plpgsql AS $text_erase_guard$
BEGIN
    IF NEW.id IS DISTINCT FROM OLD.id OR NEW.couple_id IS DISTINCT FROM OLD.couple_id
       OR NEW.title IS DISTINCT FROM '[erased]' OR NEW.description IS DISTINCT FROM '' THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_VALUE';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM public.memory_revisions r JOIN public.memory_purges p ON p.memory_id = r.memory_id WHERE r.text_id = OLD.id)
       AND NOT EXISTS (SELECT 1 FROM public.memory_edit_proposals q JOIN public.memory_purges p ON p.memory_id = q.memory_id WHERE q.text_id = OLD.id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_UNRECORDED';
    END IF;
    RETURN NEW;
END
$text_erase_guard$;

-- Only this one column, only to the fixed value, for a memory with a purge record. Both tables.
CREATE FUNCTION usapp_memory_fingerprint_erase_guard() RETURNS trigger LANGUAGE plpgsql AS $fingerprint_erase_guard$
BEGIN
    IF NEW.request_fingerprint IS DISTINCT FROM repeat('0', 64)
       OR (to_jsonb(NEW) - 'request_fingerprint') IS DISTINCT FROM (to_jsonb(OLD) - 'request_fingerprint') THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_VALUE';
    END IF;
    IF NOT EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.memory_id = OLD.memory_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_UNRECORDED';
    END IF;
    RETURN NEW;
END
$fingerprint_erase_guard$;

DROP TRIGGER memory_texts_append_only ON memory_texts;
CREATE TRIGGER memory_texts_append_only BEFORE DELETE OR TRUNCATE ON memory_texts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_texts_update_statement BEFORE UPDATE ON memory_texts
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_erase_statement_guard();
CREATE TRIGGER memory_texts_update_row BEFORE UPDATE ON memory_texts
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_text_erase_guard();

DROP TRIGGER memory_revisions_append_only ON memory_revisions;
CREATE TRIGGER memory_revisions_append_only BEFORE DELETE OR TRUNCATE ON memory_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_revisions_update_statement BEFORE UPDATE ON memory_revisions
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_erase_statement_guard();
CREATE TRIGGER memory_revisions_update_row BEFORE UPDATE ON memory_revisions
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_fingerprint_erase_guard();

DROP TRIGGER memory_edit_proposals_append_only ON memory_edit_proposals;
CREATE TRIGGER memory_edit_proposals_append_only BEFORE DELETE OR TRUNCATE ON memory_edit_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_reject_mutation();
CREATE TRIGGER memory_edit_proposals_update_statement BEFORE UPDATE ON memory_edit_proposals
    FOR EACH STATEMENT EXECUTE FUNCTION usapp_memory_erase_statement_guard();
CREATE TRIGGER memory_edit_proposals_update_row BEFORE UPDATE ON memory_edit_proposals
    FOR EACH ROW EXECUTE FUNCTION usapp_memory_fingerprint_erase_guard();

-- ---------------------------------------------------------------- the function
-- Every object is qualified, and pg_temp is searched last, so nothing a caller creates can stand in for one. A trigger it fires inherits this path.
CREATE FUNCTION public.usapp_memory_erase(p_deletion uuid) RETURNS text
LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $memory_erase$
DECLARE
    d public.memory_deletions%ROWTYPE;
BEGIN
    SELECT * INTO d FROM public.memory_deletions WHERE id = p_deletion;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_TARGET';
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(d.memory_id::text));
    IF EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.deletion_id = p_deletion) THEN
        RETURN 'already';
    END IF;
    IF EXISTS (SELECT 1 FROM public.memory_deletion_reversals r WHERE r.deletion_id = p_deletion) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_REVERSED';
    END IF;
    IF public.usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_WINDOW';
    END IF;
    -- A text this memory uses, used too by a memory that has not been erased.
    IF EXISTS (
        WITH mine AS (
            SELECT r.text_id FROM public.memory_revisions r WHERE r.memory_id = d.memory_id
            UNION SELECT q.text_id FROM public.memory_edit_proposals q WHERE q.memory_id = d.memory_id),
        users AS (
            SELECT r.memory_id FROM public.memory_revisions r JOIN mine USING (text_id)
            UNION SELECT q.memory_id FROM public.memory_edit_proposals q JOIN mine USING (text_id))
        SELECT 1 FROM users u
         WHERE u.memory_id <> d.memory_id
           AND NOT EXISTS (SELECT 1 FROM public.memory_purges p WHERE p.memory_id = u.memory_id)) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE006', MESSAGE = 'USAPP_MEMORY_ERASE_SHARED_TEXT';
    END IF;
    INSERT INTO public.memory_purges (deletion_id, memory_id, couple_id) VALUES (d.id, d.memory_id, d.couple_id);
    UPDATE public.memory_texts x SET title = '[erased]', description = ''
     WHERE x.id IN (SELECT r.text_id FROM public.memory_revisions r WHERE r.memory_id = d.memory_id
                    UNION SELECT q.text_id FROM public.memory_edit_proposals q WHERE q.memory_id = d.memory_id)
       AND (x.title <> '[erased]' OR x.description <> '');
    UPDATE public.memory_revisions r SET request_fingerprint = repeat('0', 64)
     WHERE r.memory_id = d.memory_id AND r.request_fingerprint IS NOT NULL;
    UPDATE public.memory_edit_proposals q SET request_fingerprint = repeat('0', 64)
     WHERE q.memory_id = d.memory_id;
    RETURN 'erased';
END
$memory_erase$;

-- ---------------------------------------------------------------- the replaced guard
-- The 0036 body plus one refusal, before the clock: an erased memory never comes back, whatever the clock says.
CREATE OR REPLACE FUNCTION usapp_memory_reversal_guard() RETURNS trigger LANGUAGE plpgsql VOLATILE AS $reversal_guard$
DECLARE
    d memory_deletions%ROWTYPE;
BEGIN
    IF NEW.deletion_id IS NULL OR NEW.memory_id IS NULL OR NEW.couple_id IS NULL OR NEW.reversed_by IS NULL OR NEW.reversed_at IS NULL THEN
        RETURN NEW;
    END IF;
    PERFORM pg_advisory_xact_lock(33020::integer, hashtext(NEW.memory_id::text));
    SELECT * INTO d FROM memory_deletions WHERE id = NEW.deletion_id;
    IF NOT FOUND OR d.memory_id IS DISTINCT FROM NEW.memory_id OR d.couple_id IS DISTINCT FROM NEW.couple_id THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_TARGET';
    END IF;
    IF d.deleted_by IS DISTINCT FROM NEW.reversed_by THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_ACTOR';
    END IF;
    IF EXISTS (SELECT 1 FROM memory_purges p WHERE p.deletion_id = NEW.deletion_id) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_PURGED';
    END IF;
    IF NOT usapp_media_undo_open(d.purge_after, clock_timestamp()) THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_WINDOW';
    END IF;
    IF NEW.reversed_at > clock_timestamp() OR NEW.reversed_at < d.deleted_at THEN
        RAISE EXCEPTION USING ERRCODE = 'UE003', MESSAGE = 'USAPP_MEMORY_REVERSAL_TIME';
    END IF;
    RETURN NEW;
END
$reversal_guard$;

-- ---------------------------------------------------------------- privileges. Owner last, so nothing above ran as the eraser.
REVOKE ALL ON memory_purges FROM PUBLIC, usapp_api, usapp_push;
GRANT SELECT ON memory_purges TO usapp_api;

GRANT USAGE ON SCHEMA public TO usapp_memory_eraser;
GRANT SELECT ON memory_deletions, memory_deletion_reversals, memory_purges, memory_texts, memory_revisions, memory_edit_proposals TO usapp_memory_eraser;
GRANT INSERT ON memory_purges TO usapp_memory_eraser;
GRANT UPDATE (title, description) ON memory_texts TO usapp_memory_eraser;
GRANT UPDATE (request_fingerprint) ON memory_revisions, memory_edit_proposals TO usapp_memory_eraser;

REVOKE ALL ON FUNCTION public.usapp_memory_erase(uuid) FROM PUBLIC;
ALTER FUNCTION public.usapp_memory_erase(uuid) OWNER TO usapp_memory_eraser;
REVOKE ALL ON FUNCTION public.usapp_memory_erase(uuid) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.usapp_memory_erase(uuid) TO usapp_api;

COMMIT;
