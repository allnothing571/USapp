BEGIN;

CREATE TABLE shared_plans (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    couple_id uuid NOT NULL,
    creator_id uuid NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (id, couple_id),
    FOREIGN KEY (couple_id, creator_id) REFERENCES couple_members (couple_id, user_id)
);
CREATE INDEX shared_plans_list_idx ON shared_plans (couple_id, created_at DESC, id DESC);

-- One immutable event also serves as the command receipt. A decision and its
-- approved revision are the same event, so neither can commit on its own.
CREATE TABLE shared_plan_events (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    plan_id uuid NOT NULL,
    couple_id uuid NOT NULL,
    actor_id uuid NOT NULL,
    operation text NOT NULL CHECK (operation IN ('create','edit','propose','decide','advance')),
    request_key text NOT NULL CHECK (char_length(request_key) BETWEEN 1 AND 128),
    request jsonb NOT NULL,
    seq integer NOT NULL CHECK (seq > 0),
    body_version integer,
    status_version integer,
    status text CHECK (status IN ('want','plan','finished')),
    body jsonb,
    proposal_id uuid,
    outcome text CHECK (outcome IN ('approved','rejected','withdrawn')),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (id, plan_id),
    UNIQUE (plan_id, seq),
    UNIQUE (actor_id, operation, request_key),
    UNIQUE (plan_id, body_version),
    UNIQUE (plan_id, status_version),
    UNIQUE (proposal_id),
    FOREIGN KEY (plan_id, couple_id) REFERENCES shared_plans (id, couple_id),
    FOREIGN KEY (couple_id, actor_id) REFERENCES couple_members (couple_id, user_id),
    FOREIGN KEY (proposal_id, plan_id) REFERENCES shared_plan_events (id, plan_id)
);

CREATE FUNCTION usapp_shared_plan_read(p_plan uuid) RETURNS jsonb
LANGUAGE sql STABLE SET search_path = pg_catalog, public AS $$
    SELECT jsonb_build_object('id', p.id, 'creator_id', p.creator_id, 'created_at', p.created_at,
        'body', b.body, 'body_version', b.body_version, 'status', s.status, 'status_version', s.status_version,
        'pending_proposal', pending.facts, 'latest_decision', decision.facts)
    FROM shared_plans p
    CROSS JOIN LATERAL (SELECT body, body_version FROM shared_plan_events WHERE plan_id = p.id
        AND body_version IS NOT NULL ORDER BY body_version DESC LIMIT 1) b
    CROSS JOIN LATERAL (SELECT status, status_version FROM shared_plan_events WHERE plan_id = p.id
        AND status_version IS NOT NULL ORDER BY status_version DESC LIMIT 1) s
    LEFT JOIN LATERAL (
        SELECT jsonb_build_object('id', e.id, 'proposer_id', e.actor_id, 'base_version', (e.request->>'expected_version')::int,
            'original', b.body, 'suggested', e.body, 'created_at', e.created_at) AS facts
        FROM shared_plan_events e WHERE e.plan_id = p.id AND e.operation = 'propose'
        AND NOT EXISTS (SELECT 1 FROM shared_plan_events d WHERE d.proposal_id = e.id)
        ORDER BY e.seq DESC LIMIT 1
    ) pending ON true
    LEFT JOIN LATERAL (
        SELECT jsonb_build_object('proposal_id', e.proposal_id, 'actor_id', e.actor_id, 'outcome', e.outcome,
            'created_at', e.created_at) AS facts
        FROM shared_plan_events e WHERE e.plan_id = p.id AND e.operation = 'decide'
        ORDER BY e.seq DESC LIMIT 1
    ) decision ON true
    WHERE p.id = p_plan
$$;

CREATE FUNCTION usapp_shared_plan_event_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path = pg_catalog, public AS $$
DECLARE
    root shared_plans%ROWTYPE;
    proposal shared_plan_events%ROWTYPE;
    current_plan jsonb;
    v_body jsonb;
BEGIN
    PERFORM 1 FROM couple_members WHERE user_id = NEW.actor_id AND couple_id = NEW.couple_id FOR SHARE;
    IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE = 'UK001'; END IF;
    PERFORM pg_advisory_xact_lock(33022, hashtext(NEW.plan_id::text));
    SELECT * INTO root FROM shared_plans WHERE id = NEW.plan_id AND couple_id = NEW.couple_id;
    IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE = 'UK001'; END IF;

    SELECT coalesce(max(seq), 0) + 1 INTO NEW.seq FROM shared_plan_events WHERE plan_id = root.id;
    current_plan := usapp_shared_plan_read(root.id);
    NEW.body_version := NULL;
    NEW.status_version := NULL;
    NEW.body := NULL;
    NEW.status := NULL;
    NEW.proposal_id := NULL;
    NEW.outcome := NULL;

    IF jsonb_typeof(NEW.request) IS DISTINCT FROM 'object' THEN RAISE EXCEPTION USING ERRCODE = 'UK002'; END IF;
    IF NEW.request - (CASE NEW.operation
        WHEN 'create' THEN ARRAY['body']
        WHEN 'edit' THEN ARRAY['body','expected_version']
        WHEN 'propose' THEN ARRAY['body','expected_version']
        WHEN 'advance' THEN ARRAY['expected_status']
        WHEN 'decide' THEN ARRAY['proposal_id','outcome']
        ELSE ARRAY[]::text[]
    END) <> '{}'::jsonb THEN RAISE EXCEPTION USING ERRCODE = 'UK002'; END IF;
    IF NEW.operation IN ('create','edit','propose') THEN
        v_body := NEW.request->'body';
        IF jsonb_typeof(v_body) IS DISTINCT FROM 'object' THEN RAISE EXCEPTION USING ERRCODE = 'UK002'; END IF;
        IF NOT (v_body ?& ARRAY['title','date','time','place','note']) OR
           v_body - ARRAY['title','date','time','place','note'] <> '{}'::jsonb OR
           jsonb_typeof(v_body->'title') <> 'string' OR char_length(v_body->>'title') NOT BETWEEN 1 AND 80 OR
           v_body->>'title' <> btrim(v_body->>'title') OR
           jsonb_typeof(v_body->'place') NOT IN ('string','null') OR char_length(v_body->>'place') > 120 OR
           jsonb_typeof(v_body->'note') NOT IN ('string','null') OR char_length(v_body->>'note') > 2000 OR
           jsonb_typeof(v_body->'date') NOT IN ('string','null') OR
           jsonb_typeof(v_body->'time') NOT IN ('string','null') OR
           (v_body->>'date' IS NOT NULL AND (v_body->>'date' !~ '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' OR (v_body->>'date')::date IS NULL)) OR
           (v_body->>'time' IS NOT NULL AND (v_body->>'date' IS NULL OR v_body->>'time' !~ '^([01][0-9]|2[0-3]):[0-5][0-9]$'))
        THEN RAISE EXCEPTION USING ERRCODE = 'UK002'; END IF;
        NEW.body := v_body;
    END IF;

    IF NEW.operation = 'create' THEN
        IF NEW.actor_id <> root.creator_id OR NEW.seq <> 1 THEN RAISE EXCEPTION USING ERRCODE = 'UK003'; END IF;
        NEW.body_version := 1;
        NEW.status_version := 1;
        NEW.status := 'want';
    ELSE
        IF current_plan IS NULL THEN RAISE EXCEPTION USING ERRCODE = 'UK004'; END IF;
        CASE NEW.operation
        WHEN 'edit', 'propose' THEN
            IF (NEW.operation = 'edit') <> (NEW.actor_id = root.creator_id) THEN
                RAISE EXCEPTION USING ERRCODE = 'UK003';
            END IF;
            IF current_plan->'pending_proposal' <> 'null'::jsonb OR
               NEW.request->'expected_version' IS DISTINCT FROM current_plan->'body_version' THEN
                RAISE EXCEPTION USING ERRCODE = 'UK004';
            END IF;
            IF NEW.operation = 'edit' THEN NEW.body_version := (current_plan->>'body_version')::int + 1; END IF;
        WHEN 'advance' THEN
            IF NEW.request->>'expected_status' IS DISTINCT FROM current_plan->>'status' OR
               current_plan->>'status' = 'finished' THEN RAISE EXCEPTION USING ERRCODE = 'UK004'; END IF;
            NEW.status := CASE current_plan->>'status' WHEN 'want' THEN 'plan' ELSE 'finished' END;
            NEW.status_version := (current_plan->>'status_version')::int + 1;
        WHEN 'decide' THEN
            SELECT * INTO proposal FROM shared_plan_events WHERE id = (NEW.request->>'proposal_id')::uuid
                AND plan_id = root.id AND operation = 'propose';
            IF NOT FOUND THEN RAISE EXCEPTION USING ERRCODE = 'UK001'; END IF;
            NEW.outcome := NEW.request->>'outcome';
            IF NEW.outcome IS NULL OR NEW.outcome NOT IN ('approved','rejected','withdrawn') THEN
                RAISE EXCEPTION USING ERRCODE = 'UK002';
            END IF;
            IF (NEW.outcome = 'withdrawn' AND NEW.actor_id <> proposal.actor_id) OR
               (NEW.outcome <> 'withdrawn' AND NEW.actor_id <> root.creator_id) THEN
                RAISE EXCEPTION USING ERRCODE = 'UK003';
            END IF;
            IF current_plan #>> '{pending_proposal,id}' IS DISTINCT FROM proposal.id::text OR
               proposal.request->'expected_version' IS DISTINCT FROM current_plan->'body_version' THEN
                RAISE EXCEPTION USING ERRCODE = 'UK004';
            END IF;
            NEW.proposal_id := proposal.id;
            IF NEW.outcome = 'approved' THEN
                NEW.body := proposal.body;
                NEW.body_version := (current_plan->>'body_version')::int + 1;
            END IF;
        ELSE RAISE EXCEPTION USING ERRCODE = 'UK002';
        END CASE;
    END IF;
    RETURN NEW;
END
$$;

CREATE TRIGGER shared_plan_event_guard BEFORE INSERT ON shared_plan_events
FOR EACH ROW EXECUTE FUNCTION usapp_shared_plan_event_guard();

CREATE FUNCTION usapp_shared_plan_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION USING ERRCODE = 'UK005'; END
$$;
CREATE TRIGGER shared_plans_immutable BEFORE UPDATE OR DELETE OR TRUNCATE ON shared_plans
FOR EACH STATEMENT EXECUTE FUNCTION usapp_shared_plan_immutable();
CREATE TRIGGER shared_plan_events_immutable BEFORE UPDATE OR DELETE OR TRUNCATE ON shared_plan_events
FOR EACH STATEMENT EXECUTE FUNCTION usapp_shared_plan_immutable();

CREATE FUNCTION usapp_shared_plan_write(p_actor uuid, p_plan uuid, p_operation text, p_key text, p_request jsonb)
RETURNS jsonb LANGUAGE plpgsql SET search_path = pg_catalog, public AS $$
DECLARE
    space uuid;
    receipt shared_plan_events%ROWTYPE;
    replay boolean;
BEGIN
    SELECT couple_id INTO space FROM couple_members WHERE user_id = p_actor FOR SHARE;
    IF NOT FOUND THEN RETURN '{"error":"not_found"}'; END IF;
    IF p_operation <> 'create' AND NOT EXISTS (SELECT 1 FROM shared_plans WHERE id = p_plan AND couple_id = space) THEN
        RETURN '{"error":"not_found"}';
    END IF;
    -- Serialize the same actor/operation/key even when requests name different plans.
    PERFORM pg_advisory_xact_lock(33023, hashtext(jsonb_build_array(p_actor, p_operation, p_key)::text));
    IF p_plan IS NOT NULL THEN PERFORM pg_advisory_xact_lock(33022, hashtext(p_plan::text)); END IF;
    SELECT * INTO receipt FROM shared_plan_events WHERE actor_id = p_actor AND operation = p_operation AND request_key = p_key;
    replay := FOUND;
    IF replay THEN
        IF receipt.couple_id <> space OR (p_operation <> 'create' AND receipt.plan_id <> p_plan) OR
           receipt.request IS DISTINCT FROM p_request THEN RETURN '{"error":"key_reused"}'; END IF;
    ELSE
        IF p_operation = 'create' THEN
            INSERT INTO shared_plans (couple_id, creator_id) VALUES (space, p_actor) RETURNING id INTO p_plan;
        END IF;
        INSERT INTO shared_plan_events (plan_id, couple_id, actor_id, operation, request_key, request)
        VALUES (p_plan, space, p_actor, p_operation, p_key, p_request) RETURNING * INTO receipt;
        INSERT INTO audit_logs (actor_type, actor_id, couple_id, action, target_type, target_id, detail)
        VALUES ('user', p_actor, space, 'shared_plan.' || p_operation, 'shared_plan', p_plan::text,
                jsonb_build_object('event_id', receipt.id));
    END IF;
    RETURN jsonb_build_object('replayed', replay, 'plan', usapp_shared_plan_read(receipt.plan_id),
        'mutation', jsonb_build_object('id', receipt.id, 'operation', receipt.operation, 'body_version', receipt.body_version,
            'status_version', receipt.status_version, 'status', receipt.status, 'proposal_id', receipt.proposal_id,
            'outcome', receipt.outcome));
EXCEPTION
    WHEN SQLSTATE 'UK001' THEN RETURN '{"error":"not_found"}';
    WHEN SQLSTATE 'UK002' OR invalid_datetime_format OR datetime_field_overflow THEN RETURN '{"error":"validation"}';
    WHEN invalid_text_representation THEN RETURN '{"error":"validation"}';
    WHEN SQLSTATE 'UK003' THEN RETURN '{"error":"forbidden"}';
    WHEN SQLSTATE 'UK004' THEN RETURN '{"error":"conflict"}';
END
$$;

COMMIT;
