-- =====================================================================
-- database/bootstrap-online.sql
-- Idempotent day-one online launch seed executed as postgres superuser
-- AFTER `api` has booted and bootstrapped `admin_users`:
--   1. Publishes all 20 avatar shop items (6 starters + 14 paid)
--   2. Configures avatar starter outfit defaults (version 1) if missing
--   3. Activates achievement tracking (no retroactive currency awards)
--   4. Enables push_dispatch_control if currently disabled
-- =====================================================================

BEGIN;

DO $bootstrap$
DECLARE
    v_admin       uuid;
    v_item        record;
    v_res         jsonb;
    v_hair_id     uuid;
    v_clothes_id  uuid;
    v_expr_id     uuid;
    v_push_state  text;
BEGIN
    SELECT id INTO v_admin
      FROM public.admin_users
     WHERE status = 'active'
     ORDER BY created_at ASC, id ASC
     LIMIT 1;

    IF v_admin IS NULL THEN
        RAISE EXCEPTION 'No active admin user found in public.admin_users. Ensure ADMIN_BOOTSTRAP_USERNAME & ADMIN_BOOTSTRAP_PASSWORD_HASH are set and start the api service once before running bootstrap-online.sql.';
    END IF;

    -- 1. Publish all 20 avatar items idempotently via official admin command function
    FOR v_item IN
        SELECT * FROM (VALUES
            ('hair',       'hair.placeholder_bob',         'hair.placeholder_bob.v1',         '栗色短发',       true,  NULL::integer),
            ('hair',       'hair.placeholder_wave',        'hair.placeholder_wave.v1',        '蓬松卷发',       true,  NULL::integer),
            ('clothes',    'clothes.placeholder_cream',    'clothes.placeholder_cream.v1',    '奶油色针织衫',   true,  NULL::integer),
            ('clothes',    'clothes.placeholder_teal',     'clothes.placeholder_teal.v1',     '橘棕衬衫',       true,  NULL::integer),
            ('expression', 'expression.placeholder_smile', 'expression.placeholder_smile.v1', '微笑',           true,  NULL::integer),
            ('expression', 'expression.placeholder_wink',  'expression.placeholder_wink.v1',  '眨眼',           true,  NULL::integer),
            ('hat',        'hat.placeholder_cap',          'hat.placeholder_cap.v1',          '柿橙贝雷帽',     false, 20),
            ('handheld',   'handheld.placeholder_flower',  'handheld.placeholder_flower.v1',  '庭院小花',       false, 20),
            ('accessory',  'accessory.placeholder_bow',    'accessory.placeholder_bow.v1',    '红茶蝴蝶结',     false, 20),
            ('hair',       'hair.linen_part',              'hair.linen_part.v1',              '亚麻微分碎发',   false, 25),
            ('hair',       'hair.tea_bun',                 'hair.tea_bun.v1',                 '黑茶丸子头',     false, 25),
            ('clothes',    'clothes.denim_overall',        'clothes.denim_overall.v1',        '牛仔背带裤套装', false, 35),
            ('clothes',    'clothes.oat_cardigan',         'clothes.oat_cardigan.v1',         '燕麦学院开衫',   false, 30),
            ('clothes',    'clothes.cloud_pajama',         'clothes.cloud_pajama.v1',         '暖黄云朵睡衣',   false, 30),
            ('expression', 'expression.happy_beam',        'expression.happy_beam.v1',        '弯眉元气笑',     false, 15),
            ('expression', 'expression.shy_blush',         'expression.shy_blush.v1',         '害羞微醺脸',     false, 15),
            ('hat',        'hat.straw_bucket',             'hat.straw_bucket.v1',             '草编渔夫帽',     false, 25),
            ('hat',        'hat.bear_beanie',              'hat.bear_beanie.v1',              '小熊毛绒针织帽', false, 25),
            ('handheld',   'handheld.hot_latte',           'handheld.hot_latte.v1',           '热拿铁纸杯',     false, 20),
            ('accessory',  'accessory.round_glasses',      'accessory.round_glasses.v1',      '复古圆框眼镜',   false, 20)
        ) AS t(category, family, asset_key, name, starter, price)
    LOOP
        IF NOT EXISTS (SELECT 1 FROM public.avatar_items WHERE family = v_item.family) THEN
            SELECT public.usapp_avatar_item_create(
                v_admin,
                'bootstrap_item_' || replace(v_item.family, '.', '_'),
                encode(pg_catalog.sha256(convert_to('bootstrap:item:' || v_item.asset_key, 'UTF8')), 'hex'),
                v_item.category,
                v_item.starter,
                v_item.name,
                v_item.asset_key,
                v_item.price,
                true
            ) INTO v_res;

            IF v_res IS NULL OR (v_res ? 'error') THEN
                RAISE EXCEPTION 'Failed to seed avatar item %: %', v_item.family, v_res;
            END IF;
        END IF;
    END LOOP;

    -- 2. Configure initial starter outfit defaults if not yet set
    IF NOT EXISTS (SELECT 1 FROM public.avatar_starter_configs) THEN
        SELECT id INTO v_hair_id    FROM public.avatar_items WHERE family = 'hair.placeholder_bob';
        SELECT id INTO v_clothes_id FROM public.avatar_items WHERE family = 'clothes.placeholder_cream';
        SELECT id INTO v_expr_id    FROM public.avatar_items WHERE family = 'expression.placeholder_smile';

        IF v_hair_id IS NULL OR v_clothes_id IS NULL OR v_expr_id IS NULL THEN
            RAISE EXCEPTION 'Starter items missing; cannot configure avatar_starter_configs';
        END IF;

        SELECT public.usapp_avatar_starter_set(
            v_admin,
            'bootstrap_starter_config_v1',
            encode(pg_catalog.sha256(convert_to('bootstrap:starter:v1', 'UTF8')), 'hex'),
            0,
            v_hair_id,
            v_clothes_id,
            v_expr_id
        ) INTO v_res;

        IF v_res IS NULL OR (v_res ? 'error') THEN
            RAISE EXCEPTION 'Failed to configure avatar_starter_configs: %', v_res;
        END IF;
    END IF;

    -- 3. Activate achievements tracking (safe & idempotent)
    PERFORM public.usapp_achievement_activate();

    -- 4. Enable Web Push dispatch control if currently disabled
    SELECT state INTO v_push_state FROM public.usapp_push_control_state();
    IF v_push_state = 'disabled' THEN
        INSERT INTO public.push_dispatch_control (revision, kind, actor, reason_code)
        VALUES (1, 'enable', 'owner', 'initial_online_launch');
    END IF;
END
$bootstrap$;

COMMIT;
