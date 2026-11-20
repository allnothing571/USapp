-- Synthetic pre-deletion backup fixture for tools/backup-verify.ps1.
DO $fixture$
DECLARE
    actor uuid := gen_random_uuid();
    space uuid := gen_random_uuid();
    memory uuid;
    media uuid;
    title_id uuid;
    base timestamptz := clock_timestamp();
BEGIN
    INSERT INTO users(id,email_canonical,email_display)
    VALUES (actor,'joint_restore@example.com','joint_restore@example.com');
    INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'Joint restore fixture',actor);
    INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,actor,1);
    FOR i IN 1..2 LOOP
        memory := ('00000000-0000-0000-0000-00000000000' || i)::uuid;
        media := ('00000000-0000-0000-0000-00000000001' || i)::uuid;
        INSERT INTO media_objects(id,couple_id,uploader_id,staging_key,declared_type,declared_size,
                                  expires_at,idempotency_key,request_fingerprint,created_at)
        VALUES (media,space,actor,'staging/'||space||'/'||media||'/upload','image/png',4,
                base-interval '9 days','joint-media-fixture-'||i,repeat('a',64),base-interval '9 days 10 minutes');
        INSERT INTO media_promotion_attempts(media_id,attempt_no,couple_id,final_key,created_at)
        VALUES (media,1,space,'media/'||space||'/'||media||'/'||gen_random_uuid()||'/original',
                base-interval '9 days');
        INSERT INTO media_upload_outcomes(media_id,couple_id,outcome,attempt_no,real_format,
                                          width,height,stored_size,etag,decided_at)
        VALUES (media,space,'ready',1,'image/png',4,4,4,'fixture',base-interval '8 days');
        INSERT INTO memory_texts(couple_id,title) VALUES (space,'Joint restore fixture') RETURNING id INTO title_id;
        INSERT INTO memories(id,couple_id,creator_id,created_at) VALUES (memory,space,actor,base-interval '3 days');
        INSERT INTO memory_revisions(memory_id,couple_id,version,actor_id,action,memory_date,text_id,
                                     idempotency_key,request_fingerprint,created_at)
        VALUES (memory,space,1,actor,'create',DATE '1900-01-01',title_id,'joint-revision-'||i,
                repeat('b',64),base-interval '3 days');
        INSERT INTO media_bindings(media_id,couple_id,target_kind,target_id,bound_by,created_at)
        VALUES (media,space,'memory',memory,actor,base-interval '2 days');
        INSERT INTO memory_revision_photos(memory_id,version,position,media_id,couple_id)
        VALUES (memory,1,1,media,space);
    END LOOP;
END
$fixture$;
