-- Inventory for the two explicit synthetic media records, scoped to this drill.
SELECT b.media_id, b.target_id, p.final_key, o.stored_size,
       EXISTS (
           SELECT 1 FROM deletion_recovery_quarantine q
           WHERE (q.target_kind='memory' AND q.target_id=b.target_id)
              OR (q.target_kind='media' AND q.target_id=b.media_id)
       ) AS held
FROM media_bindings b
JOIN media_upload_outcomes o ON o.media_id=b.media_id AND o.outcome='ready'
JOIN media_promotion_attempts p ON p.media_id=o.media_id AND p.attempt_no=o.attempt_no
WHERE b.target_kind='memory' AND b.target_id IN (
    '00000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000002'
)
ORDER BY b.target_id, b.media_id;
