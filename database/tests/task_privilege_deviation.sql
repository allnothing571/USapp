-- Read-only M5 privilege check (usapp_test). Returns one row per deviation from
-- the expected task/wallet matrix; an empty result means no deviation.
-- usapp_api: SELECT everywhere, INSERT on base tables only, column UPDATE only on
-- wallet_accounts.lock_token; usapp_push and PUBLIC: nothing.
WITH objs(name, is_view) AS (VALUES ('tasks', false), ('task_completions', false),
                                    ('task_progress_events', false), ('task_read_model', true),
                                    ('task_change_proposals', false), ('task_change_decisions', false),
                                    ('task_closures', false), ('reward_products', false),
                                    ('reward_product_revisions', false), ('reward_redemptions', false),
                                    ('reward_redemption_fulfillments', false),
                                    ('reward_redemption_events', false), ('reward_redemption_state', true),
                                    ('messages', false), ('message_replies', false),
                                    ('anniversaries', false), ('anniversary_revisions', false),
                                    ('anniversary_edit_proposals', false), ('anniversary_edit_decisions', false),
                                    ('anniversary_home_pins', false), ('notifications', false), ('notification_reads', false),
                                    ('notification_intents', false), ('task_reactions', false),
                                    ('push_subscriptions', false), ('push_subscription_revocations', false),
                                    ('push_registration_commands', false),
                                    ('push_dispatch_control', false), ('push_deliveries', false),
                                    ('push_attempts', false), ('push_attempt_observations', false),
                                    ('push_attempt_expiries', false), ('push_delivery_outcomes', false),
                                    ('reminder_rules', false), ('reminder_occurrences', false),
                                    ('media_objects', false), ('media_promotion_attempts', false),
                                    ('media_upload_outcomes', false), ('media_cleanup_plans', false),
                                    ('media_cleanup_passes', false), ('media_bindings', false),
                                    ('media_late_objects', false),
                                    ('memories', false), ('memory_texts', false), ('memory_revisions', false),
                                    ('memory_revision_photos', false), ('memory_edit_proposals', false),
                                    ('memory_proposal_photos', false), ('memory_edit_decisions', false),
                                    ('albums', false), ('album_texts', false), ('album_revisions', false),
                                    ('album_photo_batches', false), ('album_items', false),
                                    ('album_item_removals', false), ('album_cover_events', false),
                                    ('album_edit_proposals', false), ('album_edit_decisions', false), ('shared_plans', false), ('shared_plan_events', false), ('shared_plan_memory_links', false),
                                    ('media_deletions', false), ('media_deletion_reversals', false),
                                    ('memory_deletions', false), ('memory_deletion_reversals', false),
                                    ('memory_delete_requests', false), ('memory_delete_decisions', false),
                                    ('memory_deletion_photo_cycles', false),
                                    -- Read-only to the application, like a view: only usapp_memory_erase writes it.
                                    ('memory_purges', true),
                                    ('deletion_recovery_attempts', true), ('deletion_recovery_manifests', true),
                                    ('deletion_recovery_quarantine', true), ('deletion_recovery_verifications', true),
                                    -- Phase 4 B1: the asset registry is read-only to the application; the catalog is append-only.
                                    ('avatar_assets', true), ('avatar_items', false), ('avatar_item_revisions', false),
                                    ('avatar_starter_configs', false), ('avatar_purchases', false), ('avatar_loadout_events', false), ('achievement_activation', true),
                                    ('admin_wallet_adjustments', false), ('admin_redemption_cancellations', false), ('account_login_events', false), ('wallet_accounts', false), ('wallet_operations', false),
                                    ('wallet_ledger_entries', false)),
roles(r) AS (VALUES ('usapp_api'), ('usapp_push'), ('public')),
tprivs(p) AS (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('DELETE'), ('TRUNCATE'),
                     ('REFERENCES'), ('TRIGGER'), ('MAINTAIN')),
cprivs(p) AS (VALUES ('SELECT'), ('INSERT'), ('UPDATE'), ('REFERENCES')),
tbl AS (
  SELECT o.name, r.r, 'table ' || p.p AS what,
         has_table_privilege(r.r, ('public.' || o.name)::regclass, p.p) AS granted,
         (r.r = 'usapp_api' AND (p.p = 'SELECT' OR (p.p = 'INSERT' AND NOT o.is_view))) AS want
    FROM objs o CROSS JOIN roles r CROSS JOIN tprivs p),
col AS (
  SELECT o.name, r.r, 'column ' || p.p || '(' || a.attname || ')' AS what,
         has_column_privilege(r.r, c.oid, a.attnum, p.p) AS granted,
         (r.r = 'usapp_api' AND (p.p = 'SELECT'
                                 OR (p.p = 'INSERT' AND NOT o.is_view)
                                 OR (p.p = 'UPDATE' AND o.name = 'wallet_accounts'
                                     AND a.attname = 'lock_token'))) AS want
    FROM objs o
    JOIN pg_class c ON c.oid = ('public.' || o.name)::regclass
    JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
    CROSS JOIN roles r CROSS JOIN cprivs p)
SELECT name || '|' || r || '|' || what || '|granted=' || granted || '|want=' || want
  FROM (SELECT * FROM tbl UNION ALL SELECT * FROM col) d
 WHERE granted IS DISTINCT FROM want
 ORDER BY 1;
