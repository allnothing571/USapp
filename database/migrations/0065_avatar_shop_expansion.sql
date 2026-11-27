-- Add 11 bundled WebP avatar assets (hair, clothes, expression, hat, handheld, face accessory).
BEGIN;
LOCK TABLE public.avatar_assets IN ACCESS EXCLUSIVE MODE;

INSERT INTO public.avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top, accessory_position)
VALUES
    ('hair.linen_part.v1', 'hair', 'hair.linen_part', 1, 'webp', '{"back":"6f550a7deeb3bc5655caeacdc4434a04d6a2fde9f8cbbebc7cad2263b591e1e9","front":"eb547bcc21d726dcde990a8a5850c7ed1b987dac602e4631c3e986157c1b48b5","front_under_hat":"20d80aaaba26948a65594bab84d03fa563824e0fb4482ec0caf27858983d6afd"}'::jsonb, NULL, NULL),
    ('clothes.denim_overall.v1', 'clothes', 'clothes.denim_overall', 1, 'webp', '{"main":"c502f098cc52b8a8176591153b8676a2607b354f91b581bedf6546eb4c9b8dfd"}'::jsonb, NULL, NULL),
    ('hair.tea_bun.v1', 'hair', 'hair.tea_bun', 1, 'webp', '{"back":"24fa72d1e2a10e3b14e503bbf57b9a49e793b1edd4aa249f8a4160d439914d41","front":"164dda764863e8c21bb7d28b818a2f89686605af6a74e88dc3a07802906ea345","front_under_hat":"be3004b485e86636255ef678de58227c87b51af2d39fed38bd16b8a104aa37fa"}'::jsonb, NULL, NULL),
    ('clothes.oat_cardigan.v1', 'clothes', 'clothes.oat_cardigan', 1, 'webp', '{"main":"4eaf8b0282117f4440f0fb0c64fb4b1c28bf75ca27552a4282ad4eb972f38fc0"}'::jsonb, NULL, NULL),
    ('clothes.cloud_pajama.v1', 'clothes', 'clothes.cloud_pajama', 1, 'webp', '{"main":"ae4ad8e01acebe792f546e8a07db93e00bfd3597d6f1895b033f6c64b73aabbd"}'::jsonb, NULL, NULL),
    ('expression.happy_beam.v1', 'expression', 'expression.happy_beam', 1, 'webp', '{"main":"b6d6de66ed4476d913c42ed9b31a87f41f838add1ea54ce7e2a73a4c79e22d95"}'::jsonb, NULL, NULL),
    ('expression.shy_blush.v1', 'expression', 'expression.shy_blush', 1, 'webp', '{"main":"223e6ee63c4e02c28e10abee03b1bf81ebae152b91da68e49b96143192eb3cce"}'::jsonb, NULL, NULL),
    ('hat.straw_bucket.v1', 'hat', 'hat.straw_bucket', 1, 'webp', '{"main":"a4abddb292dfefe64188f5fc53afe50718564ff1a22d4f94469c2fc8ef2d5511"}'::jsonb, TRUE, NULL),
    ('hat.bear_beanie.v1', 'hat', 'hat.bear_beanie', 1, 'webp', '{"main":"9c949e3ea16b1a42b0567b19746759c352c8af09785ee777fc375647705f4a52"}'::jsonb, TRUE, NULL),
    ('handheld.hot_latte.v1', 'handheld', 'handheld.hot_latte', 1, 'webp', '{"main":"ddcbf9b298d86272c32357b0a28259c243b67bf6fd848790dc1ba2b638ba3da8"}'::jsonb, NULL, NULL),
    ('accessory.round_glasses.v1', 'accessory', 'accessory.round_glasses', 1, 'webp', '{"main":"5cdeea0fde8c90b9378ca6de8a08aa2e5c2dae3c7cde1f52ccddfe505420f7e6"}'::jsonb, NULL, 'face')
ON CONFLICT (asset_key) DO NOTHING;

COMMIT;
