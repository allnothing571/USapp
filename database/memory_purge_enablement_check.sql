-- D6 M1d: run this before setting MEMORY_PURGE_ENABLED, and read all of it (docs/phase3-d6-memory-deletion-design.md Section 15).
-- Read-only: it writes nothing and enables nothing. It lists every cycle whose text the first ticks would erase, irreversibly, with ids, times and counts
-- only. It uses the job's own definition of what is due, so it cannot disagree with the job.
--
--   psql -X -v ON_ERROR_STOP=1 -U postgres -d <database> -f database/memory_purge_enablement_check.sql
--
-- It exits non-zero while the role or function is unsafe. A passing report is not approval: Section 9's gates still apply.
\set ON_ERROR_STOP on
BEGIN READ ONLY;

\echo '=== memory_purge enablement check: nothing here has been erased ==='

\echo '--- Safety of usapp_memory_eraser and usapp_memory_erase. Any row here refuses enablement.'
SELECT v AS violation FROM public.usapp_memory_eraser_violations() v ORDER BY v;

\echo '--- Every cycle the first ticks would erase, oldest first. linked_photos = 0: no cascade, as from before 0038; its photos stay with its creator.'
SELECT deletion_id, memory_id, deleted_at, purge_after, clock_timestamp() - purge_after AS expired_for, linked_photos
  FROM public.usapp_memory_purge_due() WHERE NOT blocked_shared_text ORDER BY purge_after, deletion_id;

\echo '--- Cycles the job would leave alone: their text is shared with a memory that has not been erased. Resolve these by hand.'
SELECT deletion_id, memory_id, deleted_at, purge_after
  FROM public.usapp_memory_purge_due() WHERE blocked_shared_text ORDER BY purge_after, deletion_id;

\echo '--- Totals. first_ticks is how many ticks that takes, at 50 per tick.'
SELECT count(*) FILTER (WHERE NOT blocked_shared_text) AS would_erase,
       count(*) FILTER (WHERE NOT blocked_shared_text AND linked_photos = 0) AS would_erase_with_no_linked_photos,
       count(*) FILTER (WHERE blocked_shared_text) AS blocked_shared_text,
       ceil(count(*) FILTER (WHERE NOT blocked_shared_text) / 50.0)::integer AS first_ticks,
       (SELECT count(*) FROM public.memory_purges) AS already_erased
  FROM public.usapp_memory_purge_due();

DO $enablement$
DECLARE
    found text;
BEGIN
    SELECT string_agg(v, ', ' ORDER BY v) INTO found FROM public.usapp_memory_eraser_violations() v;
    IF found IS NOT NULL THEN
        RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE', DETAIL = found;
    END IF;
END
$enablement$;

ROLLBACK;
\echo '=== MEMORY PURGE ENABLEMENT CHECK COMPLETE: read the lists above. This changed nothing, and MEMORY_PURGE_ENABLED is yours to set. ==='
