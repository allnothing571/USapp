-- Exactly one joined legacy fixture must be visible at each maintenance checkpoint.
-- Project only columns common to the baseline and target schemas.
SET TIME ZONE 'UTC';
SELECT md5(jsonb_build_array(
    a.id, b.id, c.id, c.name, c.created_by_user_id, ma.slot, mb.slot,
    s.id, s.is_authenticated, s.expires_at, s.revoked_at, s.token_hash, s.csrf_token_hash,
    m.id, m.author_id, m.recipient_id, m.body, m.idempotency_key, m.request_fingerprint,
    r.author_id, r.body, r.idempotency_key, r.request_fingerprint,
    o.id, o.actor_user_id, o.operation_type, o.source_type, o.source_id,
    e.id, e.user_id, e.asset_type, e.seq, e.amount, e.balance_after
)::text)
FROM users a
JOIN users b ON b.email_canonical='upgrade0024-b@example.com'
JOIN couples c ON c.created_by_user_id=a.id
JOIN couple_members ma ON ma.couple_id=c.id AND ma.user_id=a.id
JOIN couple_members mb ON mb.couple_id=c.id AND mb.user_id=b.id
JOIN sessions s ON s.user_id=a.id AND s.token_hash=repeat('1',64)
JOIN messages m ON m.couple_id=c.id AND m.idempotency_key='upgrade0024-message'
JOIN message_replies r ON r.message_id=m.id
JOIN wallet_operations o ON o.couple_id=c.id AND o.source_id='upgrade0024-ledger'
JOIN wallet_ledger_entries e ON e.operation_id=o.id
WHERE a.email_canonical='upgrade0024-a@example.com';
