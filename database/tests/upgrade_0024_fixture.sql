-- Synthetic legacy records for the disposable upgrade rehearsal.
BEGIN;
DO $fixture$
DECLARE
    a uuid := gen_random_uuid();
    b uuid := gen_random_uuid();
    space uuid := gen_random_uuid();
    msg uuid := gen_random_uuid();
    op uuid := gen_random_uuid();
BEGIN
    INSERT INTO users(id,email_canonical,email_display)
    VALUES (a,'upgrade0024-a@example.com','upgrade0024-a@example.com'),
           (b,'upgrade0024-b@example.com','upgrade0024-b@example.com');
    INSERT INTO couples(id,name,created_by_user_id) VALUES (space,'Upgrade rehearsal',a);
    INSERT INTO couple_members(couple_id,user_id,slot) VALUES (space,a,1),(space,b,2);
    INSERT INTO sessions(token_hash,csrf_token_hash,user_id,is_authenticated,expires_at)
    VALUES (repeat('1',64),repeat('2',64),a,true,clock_timestamp()+interval '1 day');
    INSERT INTO messages(id,couple_id,author_id,recipient_id,body,idempotency_key,request_fingerprint)
    VALUES (msg,space,a,b,'Created on migration 0024','upgrade0024-message',repeat('a',64));
    INSERT INTO message_replies(message_id,couple_id,author_id,body,idempotency_key,request_fingerprint)
    VALUES (msg,space,b,'Legacy reply','upgrade0024-reply',repeat('b',64));
    INSERT INTO wallet_accounts(user_id,asset_type,couple_id) VALUES (b,'flower',space);
    INSERT INTO wallet_operations(id,couple_id,actor_user_id,operation_type,source_type,source_id)
    VALUES (op,space,b,'TASK_REWARD','wallet_test','upgrade0024-ledger');
    INSERT INTO wallet_ledger_entries(operation_id,couple_id,user_id,asset_type,seq,amount,balance_after)
    VALUES (op,space,b,'flower',1,7,7);
END
$fixture$;
COMMIT;
