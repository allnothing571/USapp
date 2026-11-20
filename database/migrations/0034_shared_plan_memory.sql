BEGIN;

CREATE TABLE shared_plan_memory_links (
    plan_id uuid PRIMARY KEY,
    couple_id uuid NOT NULL,
    memory_id uuid NOT NULL,
    actor_id uuid NOT NULL,
    mode text NOT NULL CHECK (mode IN ('existing','create')),
    request_key text NOT NULL CHECK (request_key ~ '^[A-Za-z0-9_-]{16,128}$'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (actor_id, mode, request_key),
    FOREIGN KEY (plan_id, couple_id) REFERENCES shared_plans (id, couple_id),
    FOREIGN KEY (memory_id, couple_id) REFERENCES memories (id, couple_id),
    FOREIGN KEY (couple_id, actor_id) REFERENCES couple_members (couple_id, user_id)
);

-- Call before reading or changing link state, and before any media locks.
CREATE FUNCTION usapp_plan_memory_preflight(p_actor uuid, p_plan uuid, p_mode text, p_key text)
RETURNS text LANGUAGE plpgsql SET search_path = pg_catalog, public, pg_temp AS $$
DECLARE
    space uuid;
    link shared_plan_memory_links%ROWTYPE;
    current_status text;
BEGIN
    SELECT couple_id INTO space FROM couple_members WHERE user_id = p_actor FOR SHARE;
    IF NOT FOUND OR NOT EXISTS (SELECT 1 FROM shared_plans WHERE id = p_plan AND couple_id = space) THEN
        RETURN 'not_found';
    END IF;
    PERFORM pg_advisory_xact_lock(33024, hashtext(jsonb_build_array(p_actor,p_mode,p_key)::text));
    PERFORM pg_advisory_xact_lock(33022, hashtext(p_plan::text));
    SELECT * INTO link FROM shared_plan_memory_links WHERE actor_id = p_actor AND mode = p_mode AND request_key = p_key;
    IF FOUND THEN
        IF link.plan_id <> p_plan OR link.couple_id <> space THEN RETURN 'key_reused'; END IF;
        RETURN 'replay';
    END IF;
    IF EXISTS (SELECT 1 FROM shared_plan_memory_links WHERE plan_id = p_plan) THEN RETURN 'conflict'; END IF;
    SELECT status INTO current_status FROM shared_plan_events
        WHERE plan_id = p_plan AND status_version IS NOT NULL ORDER BY status_version DESC LIMIT 1;
    IF current_status IS DISTINCT FROM 'finished' THEN RETURN 'conflict'; END IF;
    RETURN 'new';
END
$$;

CREATE FUNCTION usapp_plan_memory_link_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path = pg_catalog, public, pg_temp AS $$
BEGIN
    IF usapp_plan_memory_preflight(NEW.actor_id, NEW.plan_id, NEW.mode, NEW.request_key) <> 'new' THEN
        RAISE EXCEPTION USING ERRCODE = 'UK006', MESSAGE = 'USAPP_PLAN_MEMORY_LINK_REFUSED';
    END IF;
    IF NEW.mode = 'create' AND NOT EXISTS (
        SELECT 1 FROM memories m WHERE m.id = NEW.memory_id AND m.couple_id = NEW.couple_id
            AND m.creator_id = NEW.actor_id AND m.xmin = pg_current_xact_id()::xid
    ) THEN
        RAISE EXCEPTION USING ERRCODE = 'UK006', MESSAGE = 'USAPP_PLAN_MEMORY_CREATION_REQUIRED';
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER shared_plan_memory_link_guard BEFORE INSERT ON shared_plan_memory_links
FOR EACH ROW EXECUTE FUNCTION usapp_plan_memory_link_guard();
CREATE TRIGGER shared_plan_memory_link_immutable BEFORE UPDATE OR DELETE OR TRUNCATE ON shared_plan_memory_links
FOR EACH STATEMENT EXECUTE FUNCTION usapp_shared_plan_immutable();

CREATE FUNCTION usapp_plan_memory_attach(p_actor uuid, p_plan uuid, p_key text, p_memory uuid)
RETURNS jsonb LANGUAGE plpgsql SET search_path = pg_catalog, public, pg_temp AS $$
DECLARE
    check_result text;
    link shared_plan_memory_links%ROWTYPE;
BEGIN
    check_result := usapp_plan_memory_preflight(p_actor, p_plan, 'existing', p_key);
    IF check_result NOT IN ('new','replay') THEN RETURN jsonb_build_object('error', check_result); END IF;
    IF check_result = 'replay' THEN
        SELECT * INTO link FROM shared_plan_memory_links WHERE plan_id = p_plan;
        IF link.memory_id <> p_memory THEN RETURN '{"error":"key_reused"}'; END IF;
    ELSE
        INSERT INTO shared_plan_memory_links(plan_id,couple_id,memory_id,actor_id,mode,request_key)
        SELECT p.id,p.couple_id,m.id,p_actor,'existing',p_key
        FROM shared_plans p JOIN memories m ON m.couple_id = p.couple_id
        WHERE p.id = p_plan AND m.id = p_memory
        RETURNING * INTO link;
        IF NOT FOUND THEN RETURN '{"error":"not_found"}'; END IF;
        INSERT INTO audit_logs(actor_type,actor_id,couple_id,action,target_type,target_id,detail)
        VALUES ('user',p_actor,link.couple_id,'shared_plan.memory_linked','shared_plan',p_plan::text,
                jsonb_build_object('memory_id',p_memory));
    END IF;
    RETURN jsonb_build_object('plan_id',p_plan,'memory_id',link.memory_id,'replayed',check_result = 'replay');
END
$$;

COMMIT;
