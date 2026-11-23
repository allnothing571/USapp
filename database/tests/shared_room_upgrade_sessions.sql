-- Synthetic credentials confined to the isolated release-pair probe.
BEGIN;
INSERT INTO sessions(token_hash,csrf_token_hash,user_id,is_authenticated,expires_at)
SELECT encode(sha256(convert_to(repeat(fixture.token_byte,32),'UTF8')),'hex'),
       encode(sha256(convert_to(repeat('ca',32),'UTF8')),'hex'),
       users.id,true,clock_timestamp()+interval '1 day'
FROM (VALUES ('upgrade0024-a@example.com','a1'), ('upgrade0024-b@example.com','98'))
     AS fixture(email,token_byte)
JOIN users ON users.email_canonical=fixture.email;
COMMIT;
