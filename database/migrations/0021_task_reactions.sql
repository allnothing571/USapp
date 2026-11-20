-- P2-4: one immutable creator reaction per actual task completion. No wallet writes.
BEGIN;
DO $preflight$
BEGIN
 IF to_regclass('public.notification_intents') IS NULL THEN
   RAISE EXCEPTION '0021_task_reactions requires 0020_notification_choices';
 END IF;
END
$preflight$;

CREATE TABLE task_reactions (
 id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
 completion_task_id uuid NOT NULL UNIQUE REFERENCES task_completions(task_id) ON DELETE RESTRICT,
 couple_id uuid NOT NULL,
 author_id uuid NOT NULL,
 kind text NOT NULL CHECK (kind IN ('emoji','text')),
 emoji_code text,
 body text,
 idempotency_key text NOT NULL CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{16,128}$'),
 request_fingerprint text NOT NULL CHECK (request_fingerprint ~ '^[0-9a-f]{64}$'),
 created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
 UNIQUE(author_id, idempotency_key),
 FOREIGN KEY(completion_task_id, couple_id) REFERENCES tasks(id, couple_id) ON DELETE RESTRICT,
 FOREIGN KEY(couple_id, author_id) REFERENCES couple_members(couple_id, user_id) ON DELETE RESTRICT,
 CONSTRAINT task_reactions_content CHECK (
   (kind='emoji' AND emoji_code IS NOT NULL AND emoji_code IN ('heart','clap','flower') AND body IS NULL)
   OR (kind='text' AND emoji_code IS NULL AND body IS NOT NULL AND char_length(body) BETWEEN 1 AND 40
       AND body=btrim(body) AND body !~ '[\x01-\x1f\x7f-\x9f  ]'))
);
CREATE INDEX task_reactions_couple_idx ON task_reactions(couple_id, created_at DESC, id DESC);

CREATE FUNCTION usapp_task_reaction_append_only() RETURNS trigger LANGUAGE plpgsql AS $guard$
BEGIN
 RAISE EXCEPTION USING ERRCODE='UQ001', MESSAGE='USAPP_TASK_REACTION_APPEND_ONLY';
END
$guard$;
CREATE TRIGGER task_reactions_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON task_reactions
 FOR EACH STATEMENT EXECUTE FUNCTION usapp_task_reaction_append_only();

CREATE FUNCTION usapp_task_reaction_guard() RETURNS trigger LANGUAGE plpgsql AS $guard$
DECLARE facts record;
BEGIN
 SELECT t.couple_id,t.creator_id,c.completed_by,c.completed_at INTO facts FROM tasks t
 JOIN task_completions c ON c.task_id=t.id WHERE t.id=NEW.completion_task_id;
 IF NOT FOUND OR facts.couple_id IS DISTINCT FROM NEW.couple_id
    OR facts.creator_id IS DISTINCT FROM NEW.author_id OR facts.completed_by IS NOT DISTINCT FROM NEW.author_id
    OR NEW.created_at < facts.completed_at OR NEW.created_at > clock_timestamp() THEN
   RAISE EXCEPTION USING ERRCODE='UQ002', MESSAGE='USAPP_TASK_REACTION_SOURCE';
 END IF;
 RETURN NEW;
END
$guard$;
CREATE TRIGGER task_reactions_guard BEFORE INSERT ON task_reactions
 FOR EACH ROW EXECUTE FUNCTION usapp_task_reaction_guard();

REVOKE ALL ON task_reactions FROM PUBLIC,usapp_api,usapp_push;
GRANT SELECT,INSERT ON task_reactions TO usapp_api;
COMMIT;
