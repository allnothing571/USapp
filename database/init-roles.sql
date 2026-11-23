-- =====================================================================
-- database/init-roles.sql
--
-- 数据库角色赋权与最小权限收紧脚本（由超级用户 postgres 在目标库执行）。
--
-- 设计原则：
--   1. 动态针对当前连接数据库（current_database()，兼容 usapp 与 usapp_test）授权 CONNECT。
--   2. 严格校验角色存在性；角色不存在时明确中断并报错，严禁回退到弱默认密码。
--   3. usapp_api: 仅限 public 模式现有及未来业务表与序列的 DML 读写。
--   4. usapp_push: 阶段 0 严格隔离，禁止访问任何核心业务表与序列。
-- =====================================================================

-- 1. 严格校验角色存在性（不存在则报错中断，禁止弱密码自动创建）
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'usapp_api') THEN
        RAISE EXCEPTION '角色 usapp_api 不存在！请先通过容器初始化流程 (01-init-roles.sh) 或由超级用户配置安全密码创建角色，严禁使用默认弱密码。';
    END IF;

    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'usapp_push') THEN
        RAISE EXCEPTION '角色 usapp_push 不存在！请先通过容器初始化流程 (01-init-roles.sh) 或由超级用户配置安全密码创建角色，严禁使用默认弱密码。';
    END IF;
END
$$;

-- 1b. D6 M1d: a role belongs to the whole cluster, and this name may be someone else's. Refuse before granting anything; this script never alters that role.
-- Attributes first, which need no migration; then, once 0040 exists, its full validator. Run again at the end of this script.
DO $memory_eraser_precheck$
DECLARE
    found text;
BEGIN
    SELECT concat_ws(',', CASE WHEN r.rolsuper THEN 'superuser' END, CASE WHEN r.rolcanlogin THEN 'login' END, CASE WHEN r.rolcreaterole THEN 'createrole' END,
                     CASE WHEN r.rolcreatedb THEN 'createdb' END, CASE WHEN r.rolreplication THEN 'replication' END, CASE WHEN r.rolbypassrls THEN 'bypassrls' END,
                     CASE WHEN r.rolinherit THEN 'inherit' END, CASE WHEN r.rolconfig IS NOT NULL THEN 'role_settings' END,
                     CASE WHEN EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.roleid = r.oid) THEN 'has_members' END,
                     CASE WHEN EXISTS (SELECT 1 FROM pg_auth_members m WHERE m.member = r.oid) THEN 'has_memberships' END,
                     CASE WHEN EXISTS (SELECT 1 FROM pg_default_acl d WHERE d.defaclrole = r.oid) THEN 'default_privileges' END,
                     CASE WHEN EXISTS (SELECT 1 FROM pg_class c WHERE c.relowner = r.oid) THEN 'owns_relation' END)
      INTO found FROM pg_roles r WHERE r.rolname = 'usapp_memory_eraser';
    IF found IS NOT NULL AND found <> '' THEN
        RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE', DETAIL = found;
    END IF;
    IF to_regprocedure('public.usapp_memory_eraser_violations()') IS NOT NULL THEN
        EXECUTE 'SELECT string_agg(v, '', '' ORDER BY v) FROM public.usapp_memory_eraser_violations() v' INTO found;
        IF found IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE', DETAIL = found;
        END IF;
    END IF;
END
$memory_eraser_precheck$;

-- 2. 动态针对当前连接的数据库授权 CONNECT（兼容 usapp 与 usapp_test）
DO $$
DECLARE
    cur_db text := current_database();
BEGIN
    EXECUTE format('GRANT CONNECT ON DATABASE %I TO usapp_api, usapp_push;', cur_db);
END
$$;

-- 3. Schema 使用权限
GRANT USAGE ON SCHEMA public TO usapp_api;
GRANT USAGE ON SCHEMA public TO usapp_push;

-- 4. 设置默认权限：后续由超级用户执行迁移创建的新表与序列，自动对 usapp_api 授权 DML
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO usapp_api;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT ON SEQUENCES TO usapp_api;

-- 5. 现有表与序列授权（针对当前已执行完迁移的表）
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO usapp_api;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO usapp_api;

-- 6. 推送账户权限收紧（阶段 0 绝不授予用户、空间、管理员及审计表权限）
REVOKE ALL ON ALL TABLES IN SCHEMA public FROM usapp_push;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM usapp_push;

-- 7. 钱包表权限收窄（阶段 1 M2-A；必须位于第 5 节 ALL TABLES 授权之后；可重复执行）
--    第 5 节会把 UPDATE / DELETE 授予 usapp_api 的所有表，这里对钱包表恢复
--    0006_wallet_ledger 设定的有效权限矩阵：
--      usapp_api : 三张表 SELECT, INSERT；仅 wallet_accounts.lock_token 有列级 UPDATE（用于行锁）
--      usapp_push / PUBLIC : 无任何权限
--    表级 REVOKE 会同时收回列级权限。钱包表尚不存在（0006 未应用）时逐表跳过。
DO $wallet_grants$
DECLARE
    wallet_table text;
BEGIN
    FOREACH wallet_table IN ARRAY ARRAY['wallet_accounts', 'wallet_operations', 'wallet_ledger_entries']
    LOOP
        IF to_regclass('public.' || wallet_table) IS NOT NULL THEN
            EXECUTE format('REVOKE ALL ON public.%I FROM PUBLIC', wallet_table);
            EXECUTE format('REVOKE ALL ON public.%I FROM usapp_api', wallet_table);
            EXECUTE format('REVOKE ALL ON public.%I FROM usapp_push', wallet_table);
            EXECUTE format('GRANT SELECT, INSERT ON public.%I TO usapp_api', wallet_table);
        END IF;
    END LOOP;
    IF to_regclass('public.wallet_accounts') IS NOT NULL THEN
        EXECUTE 'GRANT UPDATE (lock_token) ON public.wallet_accounts TO usapp_api';
    END IF;
END
$wallet_grants$;

-- 8. 任务表权限收窄（阶段 1 M3-A；必须位于第 5 节 ALL TABLES 授权之后；可重复执行）
--    恢复 0007_tasks_one_off 设定的有效权限矩阵：
--      usapp_api : tasks、task_completions 仅 SELECT, INSERT（无 UPDATE / DELETE / TRUNCATE，
--                  也没有任何列级 UPDATE：任务表从不加行锁）
--      usapp_push / PUBLIC : 无任何权限
--    表级 REVOKE 会同时收回列级权限。任务表尚不存在（0007 未应用）时逐表跳过。
--    第 7 节（钱包表）保持不变。
DO $task_grants$
DECLARE
    task_table text;
BEGIN
    FOREACH task_table IN ARRAY ARRAY['tasks', 'task_completions']
    LOOP
        IF to_regclass('public.' || task_table) IS NOT NULL THEN
            EXECUTE format('REVOKE ALL ON public.%I FROM PUBLIC', task_table);
            EXECUTE format('REVOKE ALL ON public.%I FROM usapp_api', task_table);
            EXECUTE format('REVOKE ALL ON public.%I FROM usapp_push', task_table);
            EXECUTE format('GRANT SELECT, INSERT ON public.%I TO usapp_api', task_table);
        END IF;
    END LOOP;
END
$task_grants$;

-- 9. M4 progress events remain append-only after the broad section 5 grant.
DO $progress_grants$
BEGIN
    IF to_regclass('public.task_progress_events') IS NOT NULL THEN
        REVOKE ALL ON public.task_progress_events FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.task_progress_events TO usapp_api;
    END IF;
END
$progress_grants$;

-- 10. M5 read model is SELECT-only, including after a broad grant rerun.
DO $daily_read_grants$
BEGIN
    IF to_regclass('public.task_read_model') IS NOT NULL THEN
        REVOKE ALL ON public.task_read_model FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT ON public.task_read_model TO usapp_api;
    END IF;
END
$daily_read_grants$;

-- 11. M6 change proposals, decisions and closures are append-only (SELECT, INSERT).
DO $change_grants$
BEGIN
    IF to_regclass('public.task_closures') IS NOT NULL THEN
        REVOKE ALL ON public.task_change_proposals, public.task_change_decisions, public.task_closures
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.task_change_proposals, public.task_change_decisions, public.task_closures
            TO usapp_api;
    END IF;
END
$change_grants$;

-- 12. M7 reward products and their revisions are append-only (SELECT, INSERT).
DO $product_grants$
BEGIN
    IF to_regclass('public.reward_product_revisions') IS NOT NULL THEN
        REVOKE ALL ON public.reward_products, public.reward_product_revisions FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.reward_products, public.reward_product_revisions TO usapp_api;
    END IF;
END
$product_grants$;

-- 13. M8 redemptions and fulfillments are append-only (SELECT, INSERT).
DO $redemption_grants$
BEGIN
    IF to_regclass('public.reward_redemption_fulfillments') IS NOT NULL THEN
        REVOKE ALL ON public.reward_redemptions, public.reward_redemption_fulfillments
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.reward_redemptions, public.reward_redemption_fulfillments TO usapp_api;
    END IF;
END
$redemption_grants$;

-- 14. M9 events are append-only; the security-invoker state view is SELECT-only.
DO $workflow_grants$
BEGIN
    IF to_regclass('public.reward_redemption_events') IS NOT NULL THEN
        REVOKE ALL ON public.reward_redemption_events,public.reward_redemption_state FROM PUBLIC,usapp_api,usapp_push;
        GRANT SELECT,INSERT ON public.reward_redemption_events TO usapp_api;
        GRANT SELECT ON public.reward_redemption_state TO usapp_api;
    END IF;
END
$workflow_grants$;

-- 15. P2-1 messages and replies are append-only (SELECT, INSERT).
DO $message_grants$
BEGIN
    IF to_regclass('public.message_replies') IS NOT NULL THEN
        REVOKE ALL ON public.messages, public.message_replies FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.messages, public.message_replies TO usapp_api;
    END IF;
END
$message_grants$;

-- 16. P2-2 anniversaries, revisions, edit proposals, decisions and Home pins are
-- append-only (SELECT, INSERT).
DO $anniversary_grants$
BEGIN
    IF to_regclass('public.anniversary_home_pins') IS NOT NULL THEN
        REVOKE ALL ON public.anniversaries, public.anniversary_revisions, public.anniversary_edit_proposals,
            public.anniversary_edit_decisions, public.anniversary_home_pins FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.anniversaries, public.anniversary_revisions, public.anniversary_edit_proposals,
            public.anniversary_edit_decisions, public.anniversary_home_pins TO usapp_api;
    END IF;
END
$anniversary_grants$;

-- 17. P2-3a mandatory in-app notifications and immutable read receipts.
DO $notification_grants$
BEGIN
    IF to_regclass('public.notification_reads') IS NOT NULL THEN
        REVOKE ALL ON public.notifications, public.notification_reads FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.notifications, public.notification_reads TO usapp_api;
    END IF;
END
$notification_grants$;

-- 18. P2-3b immutable notification intents (decision evidence and remembered choices).
DO $notification_intent_grants$
BEGIN
    IF to_regclass('public.notification_intents') IS NOT NULL THEN
        REVOKE ALL ON public.notification_intents FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.notification_intents TO usapp_api;
    END IF;
END
$notification_intent_grants$;

-- 19. P2-4 immutable completion reactions.
DO $reaction_grants$
BEGIN
 IF to_regclass('public.task_reactions') IS NOT NULL THEN
  REVOKE ALL ON public.task_reactions FROM PUBLIC,usapp_api,usapp_push;
  GRANT SELECT,INSERT ON public.task_reactions TO usapp_api;
 END IF;
END
$reaction_grants$;

-- 20. P2-5a immutable push subscription generations, revocations and registration commands.
DO $push_subscription_grants$
BEGIN
    IF to_regclass('public.push_subscriptions') IS NOT NULL THEN
        REVOKE ALL ON public.push_subscriptions, public.push_subscription_revocations,
            public.push_registration_commands FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.push_subscriptions, public.push_subscription_revocations,
            public.push_registration_commands TO usapp_api;
    END IF;
END
$push_subscription_grants$;

-- 21. P2-5b append-only push dispatch facts and control log. usapp_push stays database-free.
DO $push_dispatch_grants$
BEGIN
    IF to_regclass('public.push_deliveries') IS NOT NULL THEN
        REVOKE ALL ON public.push_dispatch_control, public.push_deliveries, public.push_attempts,
            public.push_attempt_observations, public.push_attempt_expiries, public.push_delivery_outcomes
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.push_dispatch_control, public.push_deliveries, public.push_attempts,
            public.push_attempt_observations, public.push_attempt_expiries, public.push_delivery_outcomes
            TO usapp_api;
    END IF;
END
$push_dispatch_grants$;

-- 22. P2-6 append-only reminder rules and occurrences.
DO $reminder_grants$
BEGIN
    IF to_regclass('public.reminder_occurrences') IS NOT NULL THEN
        REVOKE ALL ON public.reminder_rules, public.reminder_occurrences FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.reminder_rules, public.reminder_occurrences TO usapp_api;
    END IF;
END
$reminder_grants$;

-- 23. P3-1a append-only media facts. Image bytes are in object storage; usapp_push has no access.
DO $media_grants$
BEGIN
    IF to_regclass('public.media_objects') IS NOT NULL THEN
        REVOKE ALL ON public.media_objects, public.media_promotion_attempts, public.media_upload_outcomes,
            public.media_cleanup_plans, public.media_cleanup_passes, public.media_bindings, public.media_late_objects
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.media_objects, public.media_promotion_attempts, public.media_upload_outcomes,
            public.media_cleanup_plans, public.media_cleanup_passes, public.media_bindings, public.media_late_objects
            TO usapp_api;
    END IF;
END
$media_grants$;

-- 24. P3-1b append-only photo memories. usapp_push has no access.
DO $memory_grants$
BEGIN
    IF to_regclass('public.memory_edit_decisions') IS NOT NULL THEN
        REVOKE ALL ON public.memories, public.memory_texts, public.memory_revisions, public.memory_revision_photos,
            public.memory_edit_proposals, public.memory_proposal_photos, public.memory_edit_decisions
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.memories, public.memory_texts, public.memory_revisions, public.memory_revision_photos,
            public.memory_edit_proposals, public.memory_proposal_photos, public.memory_edit_decisions
            TO usapp_api;
    END IF;
END
$memory_grants$;

-- 25. P3-2a append-only shared albums. usapp_push has no access.
DO $album_grants$
BEGIN
    IF to_regclass('public.album_cover_events') IS NOT NULL THEN
        REVOKE ALL ON public.albums, public.album_texts, public.album_revisions, public.album_photo_batches,
            public.album_items, public.album_item_removals, public.album_cover_events
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.albums, public.album_texts, public.album_revisions, public.album_photo_batches,
            public.album_items, public.album_item_removals, public.album_cover_events
            TO usapp_api;
    END IF;
END
$album_grants$;

-- 26. P3-2b append-only album rename requests. usapp_push has no access.
DO $album_rename_grants$
BEGIN
    IF to_regclass('public.album_edit_decisions') IS NOT NULL THEN
        REVOKE ALL ON public.album_edit_proposals, public.album_edit_decisions FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.album_edit_proposals, public.album_edit_decisions TO usapp_api;
    END IF;
END
$album_rename_grants$;

-- 27. P3-3a append-only shared plans.
DO $shared_plan_grants$
BEGIN
    IF to_regclass('public.shared_plan_events') IS NOT NULL THEN
        REVOKE ALL ON public.shared_plans, public.shared_plan_events FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.shared_plans, public.shared_plan_events TO usapp_api;
    END IF;
END
$shared_plan_grants$;

-- 28. P3-3b immutable plan-memory links.
DO $shared_plan_memory_grants$
BEGIN
    IF to_regclass('public.shared_plan_memory_links') IS NOT NULL THEN
        REVOKE ALL ON public.shared_plan_memory_links FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.shared_plan_memory_links TO usapp_api;
    END IF;
END
$shared_plan_memory_grants$;

-- 29. D6 append-only media deletion cycles and their reversals.
DO $media_deletion_grants$
BEGIN
    IF to_regclass('public.media_deletion_reversals') IS NOT NULL THEN
        REVOKE ALL ON public.media_deletions, public.media_deletion_reversals FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.media_deletions, public.media_deletion_reversals TO usapp_api;
    END IF;
END
$media_deletion_grants$;

-- 30. D6 append-only memory deletion cycles and their reversals.
DO $memory_deletion_grants$
BEGIN
    IF to_regclass('public.memory_deletion_reversals') IS NOT NULL THEN
        REVOKE ALL ON public.memory_deletions, public.memory_deletion_reversals FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.memory_deletions, public.memory_deletion_reversals TO usapp_api;
    END IF;
END
$memory_deletion_grants$;

-- 31. D6 append-only memory delete requests and their decisions.
DO $memory_delete_request_grants$
BEGIN
    IF to_regclass('public.memory_delete_decisions') IS NOT NULL THEN
        REVOKE ALL ON public.memory_delete_requests, public.memory_delete_decisions FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.memory_delete_requests, public.memory_delete_decisions TO usapp_api;
    END IF;
END
$memory_delete_request_grants$;

-- 32. D6 append-only links from a memory deletion cycle to its media deletion cycles.
DO $memory_photo_cycle_grants$
BEGIN
    IF to_regclass('public.memory_deletion_photo_cycles') IS NOT NULL THEN
        REVOKE ALL ON public.memory_deletion_photo_cycles FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.memory_deletion_photo_cycles TO usapp_api;
    END IF;
END
$memory_photo_cycle_grants$;

-- 33. D6 text erasure. The record is read-only to the application; only the eraser role, which owns the one function that writes it, may change text,
-- and only these columns. Line 44 above granted everything: this narrows it again on every run.
DO $memory_erase_grants$
BEGIN
    IF to_regclass('public.memory_purges') IS NOT NULL THEN
        REVOKE ALL ON public.memory_purges FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT ON public.memory_purges TO usapp_api;
        GRANT USAGE ON SCHEMA public TO usapp_memory_eraser;
        GRANT SELECT ON public.memory_deletions, public.memory_deletion_reversals, public.memory_purges, public.memory_texts, public.memory_revisions,
              public.memory_edit_proposals TO usapp_memory_eraser;
        GRANT INSERT ON public.memory_purges TO usapp_memory_eraser;
        GRANT UPDATE (title, description) ON public.memory_texts TO usapp_memory_eraser;
        GRANT UPDATE (request_fingerprint) ON public.memory_revisions, public.memory_edit_proposals TO usapp_memory_eraser;
        REVOKE ALL ON FUNCTION public.usapp_memory_erase(uuid) FROM PUBLIC;
        GRANT EXECUTE ON FUNCTION public.usapp_memory_erase(uuid) TO usapp_api;
    END IF;
END
$memory_erase_grants$;

-- 34. The same validator after everything above, so a run can never end with an unsafe state it did not report.
DO $memory_eraser_postcheck$
DECLARE
    found text;
BEGIN
    IF to_regprocedure('public.usapp_memory_eraser_violations()') IS NOT NULL THEN
        REVOKE ALL ON FUNCTION public.usapp_memory_eraser_violations() FROM PUBLIC;
        GRANT EXECUTE ON FUNCTION public.usapp_memory_eraser_violations() TO usapp_api, usapp_memory_eraser;
        REVOKE ALL ON FUNCTION public.usapp_memory_purge_due() FROM PUBLIC;
        GRANT EXECUTE ON FUNCTION public.usapp_memory_purge_due() TO usapp_api;
        EXECUTE 'SELECT string_agg(v, '', '' ORDER BY v) FROM public.usapp_memory_eraser_violations() v' INTO found;
        IF found IS NOT NULL THEN
            RAISE EXCEPTION USING ERRCODE = 'UE008', MESSAGE = 'USAPP_MEMORY_ERASER_UNSAFE', DETAIL = found;
        END IF;
    END IF;
END
$memory_eraser_postcheck$;

-- 35. Recovery maintenance is never an application capability. Re-narrow after ALL TABLES above.
DO $recovery_grants$
BEGIN
 IF to_regclass('public.deletion_recovery_attempts') IS NOT NULL THEN
  REVOKE ALL ON public.deletion_recovery_attempts, public.deletion_recovery_manifests,
   public.deletion_recovery_quarantine, public.deletion_recovery_verifications FROM PUBLIC, usapp_api, usapp_push;
  GRANT SELECT ON public.deletion_recovery_attempts, public.deletion_recovery_manifests,
   public.deletion_recovery_quarantine, public.deletion_recovery_verifications TO usapp_api;
  REVOKE ALL ON FUNCTION public.usapp_recovery_lock(), public.usapp_recovery_close(uuid,uuid,uuid), public.usapp_recovery_fence(uuid,uuid,bigint),
   public.usapp_recovery_stage(uuid,uuid,bigint,jsonb), public.usapp_recovery_verify(uuid,uuid,bigint,jsonb),
   public.usapp_recovery_record(uuid,uuid,bigint,bigint,text) FROM PUBLIC, usapp_api, usapp_push, usapp_memory_eraser;
  REVOKE ALL ON FUNCTION public.usapp_recovery_closed() FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_recovery_closed() TO usapp_api, usapp_memory_eraser;
 END IF;
END
$recovery_grants$;

-- 36. Phase 4 B1 avatar catalog. The asset registry is read-only to the application (the maintenance role adds reviewed
-- assets); items, revisions and starter configurations are append-only. usapp_push has no access.
DO $avatar_grants$
BEGIN
    IF to_regclass('public.avatar_starter_configs') IS NOT NULL THEN
        REVOKE ALL ON public.avatar_assets, public.avatar_items, public.avatar_item_revisions, public.avatar_starter_configs
            FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT ON public.avatar_assets TO usapp_api;
        GRANT SELECT, INSERT ON public.avatar_items, public.avatar_item_revisions, public.avatar_starter_configs TO usapp_api;
    END IF;
END
$avatar_grants$;

-- 37. Phase 4 B2 avatar purchases: append-only snapshots that are also the ownership record. usapp_push has no access.
DO $avatar_purchase_grants$
BEGIN
    IF to_regclass('public.avatar_purchases') IS NOT NULL THEN
        REVOKE ALL ON public.avatar_purchases FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.avatar_purchases TO usapp_api;
    END IF;
END
$avatar_purchase_grants$;

-- 38. Phase 4 B3 avatar loadouts: append-only saved outfits. usapp_push has no access.
DO $avatar_loadout_grants$
BEGIN
    IF to_regclass('public.avatar_loadout_events') IS NOT NULL THEN
        REVOKE ALL ON public.avatar_loadout_events FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT, INSERT ON public.avatar_loadout_events TO usapp_api;
    END IF;
END
$avatar_loadout_grants$;

-- 39. Achievements: only maintenance can activate; the API only reads/computes.
DO $achievement_grants$
BEGIN
    IF to_regclass('public.achievement_activation') IS NOT NULL THEN
        REVOKE ALL ON public.achievement_activation FROM PUBLIC, usapp_api, usapp_push;
        GRANT SELECT ON public.achievement_activation TO usapp_api;
        REVOKE ALL ON FUNCTION public.usapp_achievement_activate() FROM PUBLIC, usapp_api, usapp_push;
        REVOKE ALL ON FUNCTION public.usapp_achievement_facts(timestamptz[], timestamptz) FROM PUBLIC, usapp_push;
        GRANT EXECUTE ON FUNCTION public.usapp_achievement_facts(timestamptz[], timestamptz) TO usapp_api;
    END IF;
END
$achievement_grants$;

-- 40. Admin wallet corrections: append-only, app command only, true admin actor in wallet_operations.
DO $admin_adjust_grants$
BEGIN
 IF to_regclass('public.admin_wallet_adjustments') IS NOT NULL THEN
  REVOKE ALL ON public.admin_wallet_adjustments FROM PUBLIC,usapp_api,usapp_push;
  GRANT SELECT,INSERT ON public.admin_wallet_adjustments TO usapp_api;
  REVOKE ALL ON FUNCTION public.usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text),public.usapp_admin_adjustment_fingerprint(uuid,text,integer,bigint,text),public.usapp_admin_adjustment_valid(uuid),public.usapp_admin_adjustment_link(),public.usapp_admin_adjustment_audit_immutable() FROM PUBLIC,usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_admin_wallet_adjust(uuid,uuid,text,integer,bigint,text,text),public.usapp_admin_adjustment_fingerprint(uuid,text,integer,bigint,text),public.usapp_admin_adjustment_valid(uuid) TO usapp_api;
 END IF;
END $admin_adjust_grants$;

-- 41. Admin redemption exceptions: immutable cancellation/refund fact; no legacy member-event mutation.
DO $admin_cancel_grants$
BEGIN
 IF to_regclass('public.admin_redemption_cancellations') IS NOT NULL THEN
  REVOKE ALL ON public.admin_redemption_cancellations FROM PUBLIC,usapp_api,usapp_push;
  GRANT SELECT,INSERT ON public.admin_redemption_cancellations TO usapp_api;
  REVOKE ALL ON FUNCTION public.usapp_admin_redemption_cancel(uuid,uuid,uuid,text,text,text),public.usapp_admin_cancel_fingerprint(uuid,uuid,text,text),public.usapp_admin_cancel_snapshot(),public.usapp_admin_cancel_valid(uuid),public.usapp_admin_cancel_link() FROM PUBLIC,usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_admin_redemption_cancel(uuid,uuid,uuid,text,text,text),public.usapp_admin_cancel_fingerprint(uuid,uuid,text,text),public.usapp_admin_cancel_valid(uuid) TO usapp_api;
 END IF;
END $admin_cancel_grants$;

-- 42. Personal security source facts and mandatory emitters; no public/push source access.
DO $security_notice_grants$
BEGIN
 IF to_regclass('public.account_login_events') IS NOT NULL THEN
  REVOKE ALL ON public.account_login_events FROM PUBLIC,usapp_api,usapp_push;
  GRANT SELECT,INSERT ON public.account_login_events TO usapp_api;
  REVOKE ALL ON FUNCTION public.usapp_login_event_guard(),public.usapp_security_notice_source(text,uuid),public.usapp_security_notice_emit(),public.usapp_session_login_capture(),public.usapp_security_notice_required() FROM PUBLIC,usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_security_notice_source(text,uuid) TO usapp_api;
 END IF;
END $security_notice_grants$;

-- 43. The shared room is accessible only through its member-scoped functions.
DO $shared_room_grants$
BEGIN
 IF to_regclass('public.shared_rooms') IS NOT NULL THEN
  REVOKE ALL ON public.shared_rooms, public.shared_room_avatar_choices FROM PUBLIC, usapp_api, usapp_push;
  REVOKE ALL ON FUNCTION public.usapp_shared_room_read(uuid), public.usapp_shared_room_save(uuid,integer,jsonb),
    public.usapp_shared_room_share(uuid,boolean) FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_shared_room_read(uuid), public.usapp_shared_room_save(uuid,integer,jsonb),
    public.usapp_shared_room_share(uuid,boolean) TO usapp_api;
 END IF;
END $shared_room_grants$;

-- 44. Watering records are read and written only through member-scoped functions.
DO $shared_garden_grants$
BEGIN
 IF to_regclass('public.shared_garden_waterings') IS NOT NULL THEN
  REVOKE ALL ON public.shared_garden_waterings FROM PUBLIC, usapp_api, usapp_push;
  REVOKE ALL ON FUNCTION public.usapp_shared_garden_read(uuid), public.usapp_shared_garden_water(uuid,text) FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_read(uuid), public.usapp_shared_garden_water(uuid,text) TO usapp_api;
 END IF;
END $shared_garden_grants$;

DO $shared_garden_recent_grants$
BEGIN
 IF to_regprocedure('public.usapp_shared_garden_recent(uuid)') IS NOT NULL THEN
  REVOKE ALL ON FUNCTION public.usapp_shared_garden_recent(uuid) FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_recent(uuid) TO usapp_api;
 END IF;
END $shared_garden_recent_grants$;

DO $shared_garden_month_grants$
BEGIN
 IF to_regprocedure('public.usapp_shared_garden_month(uuid,text)') IS NOT NULL THEN
  REVOKE ALL ON FUNCTION public.usapp_shared_garden_month(uuid,text) FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_shared_garden_month(uuid,text) TO usapp_api;
 END IF;
END $shared_garden_month_grants$;

DO $room_furniture_grants$
BEGIN
 IF to_regclass('public.room_furniture_purchases') IS NOT NULL THEN
  REVOKE ALL ON public.room_furniture_catalog, public.room_furniture_purchases FROM PUBLIC, usapp_api, usapp_push;
  REVOKE ALL ON FUNCTION public.usapp_room_furniture_immutable(), public.usapp_room_furniture_debit_guard(),
    public.usapp_room_furniture_ledger_guard(), public.usapp_room_furniture_read(uuid),
    public.usapp_room_furniture_buy(uuid,text,text) FROM PUBLIC, usapp_api, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_room_furniture_read(uuid),
    public.usapp_room_furniture_buy(uuid,text,text) TO usapp_api;
 END IF;
END $room_furniture_grants$;

DO $room_character_grants$
BEGIN
 IF to_regclass('public.room_standings') IS NOT NULL THEN
  REVOKE ALL ON public.avatar_character_choices, public.room_standings, public.room_position_swaps FROM PUBLIC, usapp_api, usapp_push;
  GRANT SELECT ON public.avatar_character_choices TO usapp_api;
  REVOKE ALL ON FUNCTION public.usapp_room_initialize_positions(uuid,uuid), public.usapp_room_positions_read(uuid),
    public.usapp_room_positions_write(uuid,uuid,text), public.usapp_avatar_loadout_save_with_gender(uuid,text,text,integer,jsonb,text)
    FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_room_initialize_positions(uuid,uuid), public.usapp_room_positions_read(uuid),
    public.usapp_room_positions_write(uuid,uuid,text), public.usapp_avatar_loadout_save_with_gender(uuid,text,text,integer,jsonb,text)
    TO usapp_api;
 END IF;
END $room_character_grants$;

DO $avatar_bald_grants$
BEGIN
 IF to_regprocedure('public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean)') IS NOT NULL THEN
  REVOKE ALL ON FUNCTION public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean) FROM PUBLIC, usapp_push;
  GRANT EXECUTE ON FUNCTION public.usapp_avatar_loadout_save_with_style(uuid,text,text,integer,jsonb,text,boolean) TO usapp_api;
 END IF;
END $avatar_bald_grants$;
