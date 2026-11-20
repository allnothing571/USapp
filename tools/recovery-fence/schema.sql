-- Install once in a separate authority database. Never install into the restored application database.
BEGIN;
DO $check$
BEGIN
    IF current_database() <> 'usapp_recovery_fence' OR
       (SELECT count(*) FROM pg_roles r
          WHERE rolname IN ('usapp_fence_writer', 'usapp_fence_authority')
            AND NOT (rolsuper OR rolcreaterole OR rolcreatedb OR rolreplication OR rolbypassrls OR rolcanlogin)
            AND NOT EXISTS (SELECT 1 FROM pg_auth_members WHERE member = r.oid)) <> 2 THEN
        RAISE EXCEPTION 'external fence database and non-login capability roles required';
    END IF;
END
$check$;

CREATE SCHEMA recovery_fence;
REVOKE ALL ON SCHEMA recovery_fence FROM PUBLIC;
GRANT USAGE ON SCHEMA recovery_fence TO usapp_fence_writer, usapp_fence_authority;

CREATE TABLE recovery_fence.journals (
    journal_id uuid PRIMARY KEY,
    state text NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'draining', 'paused')),
    pause_id uuid,
    CHECK ((state = 'open') = (pause_id IS NULL))
);
CREATE TABLE recovery_fence.writer_roster (
    journal_id uuid NOT NULL REFERENCES recovery_fence.journals,
    writer_role name NOT NULL,
    PRIMARY KEY (journal_id, writer_role)
);
CREATE TABLE recovery_fence.admissions (
    journal_id uuid NOT NULL,
    admission_id uuid NOT NULL,
    writer_role name NOT NULL,
    drained_at timestamptz,
    PRIMARY KEY (journal_id, admission_id),
    FOREIGN KEY (journal_id, writer_role) REFERENCES recovery_fence.writer_roster
);
CREATE UNIQUE INDEX one_active_admission_per_writer
    ON recovery_fence.admissions(journal_id, writer_role) WHERE drained_at IS NULL;

-- This row is the transaction lock for both registration and pause transitions.
-- The fresh read after taking it requires READ COMMITTED. Different isolations refuse.
CREATE FUNCTION recovery_fence.lock_scope(p_journal uuid)
RETURNS recovery_fence.journals LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $body$
DECLARE scope recovery_fence.journals;
BEGIN
    IF current_setting('transaction_isolation') <> 'read committed' THEN
        RAISE EXCEPTION 'recovery fence requires read committed' USING ERRCODE = '25001';
    END IF;
    IF current_setting('synchronous_commit') NOT IN ('on', 'remote_apply') OR current_setting('fsync') <> 'on' THEN
        RAISE EXCEPTION 'recovery fence requires durable commits' USING ERRCODE = '55000';
    END IF;
    SELECT * INTO scope FROM recovery_fence.journals WHERE journal_id = p_journal FOR UPDATE;
    RETURN scope;
END
$body$;

CREATE FUNCTION recovery_fence.admit(p_journal uuid, p_admission uuid)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $body$
DECLARE scope recovery_fence.journals;
        prior recovery_fence.admissions;
BEGIN
    IF p_admission IS NULL OR NOT EXISTS (
        SELECT 1 FROM recovery_fence.writer_roster
        WHERE journal_id = p_journal AND writer_role = session_user
    ) THEN RETURN 'refused'; END IF;
    scope := recovery_fence.lock_scope(p_journal);
    IF scope.state IS DISTINCT FROM 'open' THEN RETURN 'refused'; END IF;
    SELECT * INTO prior FROM recovery_fence.admissions
    WHERE journal_id = p_journal AND admission_id = p_admission;
    IF FOUND THEN
        IF prior.writer_role = session_user AND prior.drained_at IS NULL THEN RETURN 'admitted'; END IF;
        RETURN 'refused';
    END IF;
    IF EXISTS (SELECT 1 FROM recovery_fence.admissions
               WHERE journal_id = p_journal AND writer_role = session_user AND drained_at IS NULL)
    THEN RETURN 'refused'; END IF;
    INSERT INTO recovery_fence.admissions (journal_id, admission_id, writer_role)
    VALUES (p_journal, p_admission, session_user);
    RETURN 'admitted';
END
$body$;

CREATE FUNCTION recovery_fence.confirm_drained(p_journal uuid, p_admission uuid)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $body$
BEGIN
    PERFORM recovery_fence.lock_scope(p_journal);
    UPDATE recovery_fence.admissions SET drained_at = clock_timestamp()
    WHERE journal_id = p_journal AND admission_id = p_admission
      AND writer_role = session_user AND drained_at IS NULL;
    IF FOUND OR EXISTS (SELECT 1 FROM recovery_fence.admissions
                        WHERE journal_id = p_journal AND admission_id = p_admission
                          AND writer_role = session_user AND drained_at IS NOT NULL)
    THEN RETURN 'drained'; END IF;
    RETURN 'refused';
END
$body$;

CREATE FUNCTION recovery_fence.request_pause(p_journal uuid, p_pause uuid)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $body$
DECLARE scope recovery_fence.journals;
BEGIN
    scope := recovery_fence.lock_scope(p_journal);
    IF p_pause IS NULL OR scope.journal_id IS NULL OR NOT EXISTS (
        SELECT 1 FROM recovery_fence.writer_roster WHERE journal_id = p_journal
    ) THEN RETURN 'refused'; END IF;
    IF scope.state = 'open' THEN
        UPDATE recovery_fence.journals SET state = 'draining', pause_id = p_pause WHERE journal_id = p_journal;
        RETURN 'draining';
    END IF;
    IF scope.pause_id = p_pause THEN RETURN scope.state; END IF;
    RETURN 'refused';
END
$body$;

CREATE FUNCTION recovery_fence.confirm_pause(p_journal uuid, p_pause uuid)
RETURNS text LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $body$
DECLARE scope recovery_fence.journals;
BEGIN
    scope := recovery_fence.lock_scope(p_journal);
    IF p_pause IS NULL OR scope.pause_id IS DISTINCT FROM p_pause THEN RETURN 'refused'; END IF;
    IF EXISTS (SELECT 1 FROM recovery_fence.admissions WHERE journal_id = p_journal AND drained_at IS NULL)
    THEN RETURN 'draining'; END IF;
    IF scope.state = 'draining' THEN
        UPDATE recovery_fence.journals SET state = 'paused' WHERE journal_id = p_journal;
    END IF;
    RETURN 'paused';
END
$body$;

REVOKE ALL ON ALL TABLES IN SCHEMA recovery_fence FROM PUBLIC, usapp_fence_writer, usapp_fence_authority;
REVOKE ALL ON ALL FUNCTIONS IN SCHEMA recovery_fence FROM PUBLIC, usapp_fence_writer, usapp_fence_authority;
GRANT EXECUTE ON FUNCTION recovery_fence.admit(uuid, uuid), recovery_fence.confirm_drained(uuid, uuid)
    TO usapp_fence_writer;
GRANT EXECUTE ON FUNCTION recovery_fence.request_pause(uuid, uuid), recovery_fence.confirm_pause(uuid, uuid)
    TO usapp_fence_authority;
COMMIT;
