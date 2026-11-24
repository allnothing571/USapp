"""Extract aligned avatar layers locally. No network, front-end transforms, or database writes."""

import argparse
import hashlib
import io
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageOps


SIZE = 512
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("base_faceless", "base_smile", "girl_outfit", "boy_outfit")
OUTFITS = {
    "girl_outfit": ("hair.placeholder_wave.v1", "clothes.placeholder_cream.v1"),
    "boy_outfit": ("hair.placeholder_bob.v1", "clothes.placeholder_teal.v1"),
}
HAIR_LAYERS = ("back", "front", "front_under_hat")
SMILE = "expression.placeholder_smile.v1"
OPTIONAL_SOURCES = {
    "base_wink": "expression.placeholder_wink.v1",
    "item_hat": "hat.placeholder_cap.v1",
    "item_handheld": "handheld.placeholder_flower.v1",
    "item_accessory": "accessory.placeholder_bow.v1",
}
GRID_Y, GRID_X = np.ogrid[:SIZE, :SIZE]


def flood_foreground(rgb):
    near_white = (rgb.min(axis=2) > 240).astype(np.uint8)
    height, width = near_white.shape
    for point in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
        if near_white[point[1], point[0]] == 1:
            cv2.floodFill(near_white, None, point, 2, flags=4)
    return near_white != 2


def kernel(radius):
    diameter = radius * 2 + 1
    return cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (diameter, diameter))


def dilate(mask, radius=1):
    return cv2.dilate(mask.astype(np.uint8), kernel(radius)) > 0


def erode(mask, radius=1):
    return cv2.erode(mask.astype(np.uint8), kernel(radius)) > 0


def largest(mask, minimum=1):
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
    if count < 2:
        return np.zeros_like(mask)
    index = stats[1:, cv2.CC_STAT_AREA].argmax() + 1
    return (labels == index) if stats[index, cv2.CC_STAT_AREA] >= minimum else np.zeros_like(mask)


def keep_components(mask, minimum):
    _, labels, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
    keep = stats[:, cv2.CC_STAT_AREA] >= minimum
    keep[0] = False
    return keep[labels]


def fill_small_holes(mask, max_area):
    _, labels, stats, _ = cv2.connectedComponentsWithStats((~mask).astype(np.uint8), connectivity=8)
    keep = stats[:, cv2.CC_STAT_AREA] <= max_area
    keep[0] = False
    return mask | keep[labels]


def warp(image, matrix, mode=cv2.INTER_LINEAR):
    return cv2.warpAffine(image, matrix, (SIZE, SIZE), flags=mode, borderMode=cv2.BORDER_CONSTANT)


def normalize(rgb, foreground, matrix):
    # Resample premultiplied color so discarded white never enters the edge.
    distance = cv2.distanceTransform(foreground.astype(np.uint8), cv2.DIST_L2, 3)
    alpha = np.clip(distance / 2.0, 0, 1)
    color = rgb.astype(np.float32) / 255
    opacity = alpha[..., None]
    unmatted = np.clip((color - (1 - opacity)) / np.maximum(opacity, 0.01), 0, 1)
    packed = warp(np.dstack((unmatted * opacity, alpha)).astype(np.float32), matrix)
    rgb_out = np.divide(packed[..., :3], np.maximum(packed[..., 3:], 0.001))
    return np.uint8(np.round(np.clip(np.dstack((rgb_out, packed[..., 3])), 0, 1) * 255))


def silhouette_bounds(mask):
    yy, xx = np.nonzero(mask)
    if len(xx) < 500:
        raise ValueError("Foreground silhouette is missing or too small")
    return xx.min(), yy.min(), xx.max(), yy.max()


def load_sources(directory):
    rgbs = {}
    outlines = {}
    names = (*SOURCES, *(name for name in OPTIONAL_SOURCES if (directory / f"{name}.jpg").is_file()))
    for name in names:
        with Image.open(directory / f"{name}.jpg") as image:
            rgb = np.asarray(ImageOps.exif_transpose(image).convert("RGB"))
        rgbs[name] = rgb
        foreground = flood_foreground(rgb)
        outlines[name] = keep_components(foreground, 48) if name in OPTIONAL_SOURCES else largest(foreground)
    if len({image.shape for image in rgbs.values()}) != 1:
        raise ValueError("The source drawings must have a common pixel canvas")
    left, top, right, bottom = silhouette_bounds(outlines["base_faceless"])
    scale = 422 / (bottom - top + 1)
    matrix = np.float32([[scale, 0, 256 - scale * (left + right) / 2], [0, scale, 48 - scale * top]])
    return {name: normalize(rgbs[name], outlines[name], matrix) for name in names}


def skin(image):
    red, green, blue = image[..., :3].astype(np.int16).transpose(2, 0, 1)
    return (image[..., 3] > 24) & (red >= 200) & (green >= 132) & (red - green >= 11) & (
        red - green < 76
    ) & (green - blue >= 8) & (red - blue >= 24)


def neck_from_outline(mask):
    _, top, _, bottom = silhouette_bounds(mask)
    height = bottom - top
    widths = np.count_nonzero(mask, axis=1).astype(np.float32)
    widths = np.convolve(widths, np.ones(5) / 5, mode="same")
    start, stop = (int(top + height * portion) for portion in (0.27, 0.42))
    return start + int(np.argmin(widths[start:stop]))


def face_region(base, neck):
    x0, y0, x1, _ = silhouette_bounds((base[..., 3] > 120) & (GRID_Y < neck - 7))
    return x0, y0, x1, neck - 7


def face_signal(image):
    # Close small holes in the skin silhouette caused by eyes and mouth.
    mask = cv2.morphologyEx(skin(image).astype(np.uint8), cv2.MORPH_CLOSE, kernel(3))
    return cv2.GaussianBlur(mask.astype(np.float32), (0, 0), 2.0)


def align(base, image, bounds):
    x0, y0, x1, y1 = bounds
    # Forehead is frequently covered by bangs. Match cheeks, ears, and jaw.
    region_top = round(y0 + (y1 - y0) * 0.48)
    roi = np.zeros((SIZE, SIZE), np.uint8)
    roi[region_top:y1 + 7, x0 - 10:x1 + 11] = 255
    target, moving = face_signal(base), face_signal(image)
    identity = np.float32([[1, 0, 0], [0, 1, 0]])
    fitted = identity.copy()
    try:
        _, inverse = cv2.findTransformECC(
            target, moving, identity.copy(), cv2.MOTION_AFFINE,
            (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 100, 1e-6), roi, 5,
        )
        # Project the fit onto translation plus bounded uniform scale.
        ratio = float(np.clip(np.trace(inverse[:, :2]) / 2, 0.98, 1.02))
        center = np.array([(x0 + x1) / 2, (region_top + y1) / 2])
        displacement = inverse[:, :2] @ center + inverse[:, 2] - center
        translation = center * (1 - ratio) + np.clip(displacement, -8, 8)
        fitted = cv2.invertAffineTransform(np.float32([[ratio, 0, translation[0]], [0, ratio, translation[1]]]))
    except cv2.error:
        pass

    test = roi > 0

    def error(matrix):
        return float(np.mean((target[test] - warp(moving, matrix)[test]) ** 2))

    center = np.array([(x0 + x1) / 2, (region_top + y1) / 2])
    candidates = [identity, fitted]
    for scale in (0.98, 1.0, 1.02):
        for x in (-4, 0, 4):
            for y in (-4, 0, 4):
                dx, dy = center * (1 - scale) + (x, y)
                candidates.append(np.float32([[scale, 0, dx], [0, scale, dy]]))
    selected = min(candidates, key=error)
    # Interpolate packed RGBA, avoiding colored edges on transparent pixels.
    opacity = image[..., 3:].astype(np.float32) / 255
    packed = warp(np.dstack((image[..., :3] * opacity, image[..., 3])).astype(np.float32), selected)
    packed[..., :3] = packed[..., :3] / np.maximum(packed[..., 3:] / 255, 0.001)
    return np.uint8(np.round(np.clip(packed, 0, 255))), error(selected), selected


def layer(source, mask):
    if np.count_nonzero(mask) < 8:
        raise ValueError("An extracted layer has too little foreground")
    alpha = cv2.GaussianBlur(mask.astype(np.float32), (3, 3), 0.57)
    result = source.copy()
    result[..., 3] = np.uint8(np.round(source[..., 3] * alpha))
    result[result[..., 3] == 0] = 0
    return result


def garment_layer(source, garment, exclusion):
    # Extend only a one-pixel ring using its closest painted garment color.
    # This covers the shared base outline without smearing cuffs into hands.
    ring = dilate(garment) & ~garment & ~exclusion
    _, labels = cv2.distanceTransformWithLabels((~garment).astype(np.uint8), cv2.DIST_L2, 5,
                                                labelType=cv2.DIST_LABEL_PIXEL)
    lookup = np.zeros((int(labels.max()) + 1, 4), dtype=np.uint8)
    lookup[labels[garment]] = source[garment]
    tinted = source.copy()
    tinted[ring] = lookup[labels[ring]]
    return layer(tinted, garment | ring)


def difference(a, b):
    return np.max(np.abs(a[..., :3].astype(np.int16) - b[..., :3]), axis=2)


def expression_layer(base, expression, bounds):
    x0, y0, x1, y1 = bounds
    inset = round((x1 - x0) * 0.17)
    center = (GRID_Y >= y0 + (y1 - y0) * 0.38) & (GRID_Y <= y1 - 12)
    center = center & (GRID_X >= x0 + inset) & (GRID_X <= x1 - inset)
    center = center & erode(base[..., 3] > 180, 8)
    changed = (difference(base, expression) > 30) & (expression[..., :3].min(axis=2) < 155)
    points = keep_components(changed & center, 15)
    return layer(expression, dilate(points) & center)


def item_layer(base, source, bounds, neck, category):
    left, top, right, bottom = silhouette_bounds(base[..., 3] > 127)
    x0, y0, x1, y1 = bounds
    offset = np.abs(GRID_X - (x0 + x1) / 2)
    if category == "hat":
        region = (GRID_Y <= y0 + (y1 - y0) * 0.55) & (GRID_X >= x0 - 45) & (GRID_X <= x1 + 45)
    elif category == "accessory":
        region = (GRID_Y >= neck - 16) & (GRID_Y <= neck + 65) & (offset <= 65)
    else:
        # Allow both hands and objects extending up, down, or out from either cuff.
        height = bottom - top
        region = (GRID_Y >= neck) & (GRID_Y <= neck + 0.53 * height)
        region = region & (offset >= (right - left) * 0.27)
    occupied = source[..., 3] > 32
    added = (base[..., 3] <= 32) | (difference(base, source) > 32)
    base_dark = (base[..., 3] > 32) & (base[..., :3].max(axis=2) < 130)
    source_dark = (source[..., 3] > 32) & (source[..., :3].max(axis=2) < 130)
    outline_drift = source_dark & dilate(base_dark, 3)
    skin_shift = skin(source) & (skin(base) | dilate(base[..., 3] > 32, 3) & ~erode(base[..., 3] > 32, 3))
    core = occupied & region & added & ~skin_shift & ~outline_drift
    core = cv2.morphologyEx(core.astype(np.uint8), cv2.MORPH_OPEN, kernel(1)) > 0
    core = keep_components(core, 60)
    item = fill_small_holes(core | (dilate(core, 4) & occupied & region & added & ~skin_shift), 150)
    item = keep_components(item, 150)
    try:
        return layer(source, dilate(item) & region & occupied)
    except ValueError as exc:
        raise ValueError(f"No {category} found in the aligned source region") from exc


def outfit_layers(source, smile, base, bounds, neck):
    x0, top, x1, _ = bounds
    silhouette = source[..., 3] > 28
    altered = (difference(source, smile) > 28) | (smile[..., 3] < 42)
    hsv = cv2.cvtColor(source[..., :3], cv2.COLOR_RGB2HSV)
    # Warm brown hair is the connected mass surrounding the head. This keeps
    # detached facial features, shirt pixels, and trouser pockets out.
    head_band = (GRID_Y < neck + 58) & (GRID_Y >= top - 49) & (GRID_X > x0 - 62) & (GRID_X < x1 + 62)
    above_neck = GRID_Y < neck + 7
    crown = GRID_Y < top + (neck - top) * 0.40
    hair_core = silhouette & head_band & (altered | crown) & (hsv[..., 2] < 224) & (hsv[..., 1] > 46) & (~skin(source) | crown)
    hair_core = hair_core & (above_neck | (hsv[..., 2] < 180))
    compact = cv2.morphologyEx(hair_core.astype(np.uint8), cv2.MORPH_CLOSE, kernel(3)) > 0
    hair = largest(compact, 80) & silhouette
    hair = fill_small_holes(hair | (dilate(hair, 1) & hair_core), 220) & silhouette
    front = hair & (GRID_Y <= neck + 10)
    front_y = np.nonzero(front)[0]
    if not len(front_y):
        raise ValueError("Could not locate a connected front hair region")
    brim = round(front_y.min() + 0.25 * (front_y.max() - front_y.min()))
    trimmed = layer(source, front & (GRID_Y >= brim))

    _, _, _, bottom = silhouette_bounds(base[..., 3] > 127)
    center = (x0 + x1) / 2
    # Locate exposed hands from their color *and* from their position below
    # the cuffs. A palette test alone mistakes knitted cream fabric for skin.
    chest_y = neck + (bottom - neck) * 0.46
    hands = skin(source) & (GRID_Y > chest_y) & (GRID_Y < chest_y + 51) & (np.abs(GRID_X - center) > 70)
    hands = dilate(keep_components(hands, 28), 3) & silhouette
    warm = source[..., 0].astype(np.int16) - source[..., 1] > 20
    neckline = skin(source) & skin(base) & warm & (GRID_Y < neck + 18) & (np.abs(GRID_X - center) < 29)
    exclusion = dilate(hair) | hands | neckline | (GRID_Y < neck)
    worn = silhouette & ~exclusion
    # Retain connected garment pixels even when a cream sweater or white
    # sneaker closely matches the old silhouette.
    garment = largest(cv2.morphologyEx(worn.astype(np.uint8), cv2.MORPH_CLOSE, kernel(2)) > 0)
    garment = garment & worn
    seed = garment & altered
    if np.count_nonzero(seed) < 1000:
        raise ValueError("Outfit differs too little from the aligned base")
    garment = fill_small_holes(garment, 80) & worn
    return {
        "back": layer(source, hair), "front": layer(source, front),
        "front_under_hat": trimmed, "main": garment_layer(source, garment, exclusion),
    }


def webp_bytes(pixels):
    stream = io.BytesIO()
    Image.fromarray(pixels).save(stream, format="WEBP", lossless=True, method=6)
    return stream.getvalue()


def file_record(filename, data):
    return {"file": filename, "sha256": hashlib.sha256(data).hexdigest()}


def registry_migration(changes):
    def sql_json(value):
        return "'" + json.dumps(value, sort_keys=True, separators=(",", ":")).replace("'", "''") + "'::jsonb"

    entries = []
    for key, (old, new) in sorted(changes.items()):
        previous = sql_json({name: record["sha256"] for name, record in old["layers"].items()})
        expected = sql_json({name: record["sha256"] for name, record in new["layers"].items()})
        covers = "NULL" if old["covers_hair_top"] is None else str(old["covers_hair_top"]).upper()
        position = "NULL" if old["accessory_position"] is None else f"'{old['accessory_position']}'"
        entries.append(
            f"    ('{key}', '{old['category']}', '{old['family']}', {old['revision']}, "
            f"{covers}, {position}, '{old['format']}', {previous}, {expected})"
        )
    values = ",\n".join(entries)
    return f"""-- Generated by scripts/auto_extract_avatars.py. Preserve prior migration checksums.
BEGIN;
LOCK TABLE public.avatar_assets IN ACCESS EXCLUSIVE MODE;

CREATE TEMP TABLE avatar_webp_upgrade (
    asset_key text PRIMARY KEY, category text NOT NULL, family text NOT NULL,
    revision integer NOT NULL, covers_hair_top boolean, accessory_position text,
    old_format text NOT NULL, old_layers jsonb NOT NULL, new_layers jsonb NOT NULL
) ON COMMIT DROP;
INSERT INTO avatar_webp_upgrade VALUES
{values};

-- Accept only the previously registered hashes or the exact generated WebP.
-- A later revision with the old format needs a separate migration.
DO $check$
BEGIN
    IF EXISTS (
        SELECT 1 FROM public.avatar_assets a JOIN avatar_webp_upgrade u USING (asset_key)
        WHERE a.category <> u.category OR a.family <> u.family OR a.revision <> u.revision
           OR a.covers_hair_top IS DISTINCT FROM u.covers_hair_top
           OR a.accessory_position IS DISTINCT FROM u.accessory_position
           OR NOT ((a.format = u.old_format AND a.layers = u.old_layers)
                OR (a.format = 'webp' AND a.layers = u.new_layers))
    ) OR EXISTS (
        SELECT 1 FROM public.avatar_assets a JOIN avatar_webp_upgrade u USING (family)
        WHERE a.revision > u.revision AND a.format <> 'webp'
    ) THEN
        RAISE EXCEPTION 'Avatar registry differs from the supported placeholder upgrade';
    END IF;
END
$check$;

-- This one-time format migration keeps existing catalog, purchase and loadout
-- key references intact. The write guard is restored before commit.
ALTER TABLE public.avatar_assets DISABLE TRIGGER avatar_assets_append_only;
UPDATE public.avatar_assets a SET format = 'webp', layers = u.new_layers
FROM avatar_webp_upgrade u
WHERE a.asset_key = u.asset_key AND (a.format, a.layers) IS DISTINCT FROM ('webp', u.new_layers);
ALTER TABLE public.avatar_assets ENABLE TRIGGER avatar_assets_append_only;

INSERT INTO public.avatar_assets (asset_key, category, family, revision, format, layers, covers_hair_top, accessory_position)
SELECT u.asset_key, u.category, u.family, u.revision, 'webp', u.new_layers, u.covers_hair_top, u.accessory_position
FROM avatar_webp_upgrade u WHERE NOT EXISTS (SELECT 1 FROM public.avatar_assets a WHERE a.asset_key = u.asset_key);
COMMIT;
"""


def next_migration():
    directory = ROOT / "database/migrations"
    latest = max(int(path.name.split("_", 1)[0]) for path in directory.glob("[0-9][0-9][0-9][0-9]_*.sql"))
    return directory / f"{latest + 1:04d}_avatar_layer_refresh.sql"


def write_preview(path, files):
    # Decode the actual WebP bytes so QA also covers the encoder.
    decoded = {name: Image.open(io.BytesIO(data)).convert("RGBA") for name, data in files.items()}
    girl_hair, girl_clothes = OUTFITS["girl_outfit"]
    boy_hair, boy_clothes = OUTFITS["boy_outfit"]
    combinations = [
        ("Bald + smile", None, None, SMILE),
        ("Girl outfit", girl_hair, girl_clothes, SMILE),
        ("Boy outfit", boy_hair, boy_clothes, SMILE),
        ("Boy hair + girl clothes", boy_hair, girl_clothes, SMILE),
    ]
    for key in OPTIONAL_SOURCES.values():
        if f"{key}.main.webp" in decoded:
            combinations.append((key.split(".")[1].removeprefix("placeholder_"), None, None, key))
    panel = Image.new("RGB", (1024, ((len(combinations) + 1) // 2) * 540), "#f3ebdd")
    draw = ImageDraw.Draw(panel)
    for index, (label, hair, clothes, art) in enumerate(combinations):
        order = [f"{hair}.back.webp"] if hair else []
        order.append("base.v1.webp")
        if clothes:
            order.append(f"{clothes}.main.webp")
        if art.startswith("expression."):
            order.append(f"{art}.main.webp")
        else:
            order.extend((f"{SMILE}.main.webp", f"{art}.main.webp"))
        if hair:
            order.append(f"{hair}.front.webp")
        figure = Image.new("RGBA", (SIZE, SIZE))
        for filename in order:
            figure.alpha_composite(decoded[filename])
        x, y = (index % 2) * 512, (index // 2) * 540
        panel.paste(figure, (x, y), figure)
        draw.text((x + 15, y + 513), label, fill="#3a3025")
    path.parent.mkdir(parents=True, exist_ok=True)
    panel.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source_root = ROOT / "assets-raw/source"
    if not source_root.is_dir() and ROOT.parent.name == ".worktrees":
        source_root = ROOT.parent.parent / "assets-raw/source"
    parser.add_argument("--source-dir", type=Path, default=source_root)
    parser.add_argument("--preview", type=Path, help="Default: debug_preview.png next to the source directory")
    parser.add_argument("--preview-only", action="store_true", help="Render QA without updating assets or enrollment SQL")
    parser.add_argument("--neck-y", type=int, help="Override detected neck row on the 512px canvas")
    parser.add_argument("--migration", type=Path, help="Default: next numbered migration if registry data changes")
    args = parser.parse_args()
    preview_path = args.preview or args.source_dir.parent / "debug_preview.png"
    images = load_sources(args.source_dir)
    base = images["base_faceless"]
    neck = neck_from_outline(base[..., 3] > 127) if args.neck_y is None else args.neck_y
    if not 150 <= neck <= 240:
        parser.error(f"neck is outside the supported head/shoulder band: {neck}")
    bounds = face_region(base, neck)
    print(f"neck_y={neck}, face={bounds}")
    for name in images:
        if name == "base_faceless":
            continue
        images[name], error, matrix = align(base, images[name], bounds)
        print(f"{name}: facial mask MSE={error:.4f}; affine={matrix.tolist()}")
    smile = images["base_smile"]
    files = {"base.v1.webp": webp_bytes(base), f"{SMILE}.main.webp": webp_bytes(expression_layer(base, smile, bounds))}
    targets = {SMILE: ("main",)}
    for name, key in OPTIONAL_SOURCES.items():
        if name not in images:
            print(f"Skipped optional source: {name}.jpg")
            continue
        category = key.split(".", 1)[0]
        image = images[name]
        part = expression_layer(base, image, bounds) if category == "expression" else item_layer(base, image, bounds, neck, category)
        targets[key] = ("main",)
        files[f"{key}.main.webp"] = webp_bytes(part)
    for name, (hair, clothes) in OUTFITS.items():
        parts = outfit_layers(images[name], smile, base, bounds, neck)
        files.update({f"{hair}.{key}.webp": webp_bytes(parts[key]) for key in HAIR_LAYERS})
        files[f"{clothes}.main.webp"] = webp_bytes(parts["main"])
        targets[hair] = HAIR_LAYERS
        targets[clothes] = ("main",)
    if args.preview_only:
        write_preview(preview_path, files)
        print(f"Preview: {preview_path}")
        return
    web = ROOT / "apps/web"
    manifest_path = web / "src/assets/avatar-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["placeholder"] = False
    manifest["base"] = file_record("base.v1.webp", files["base.v1.webp"])
    manifest["bases"] = dict.fromkeys(("female", "male"), manifest["base"])
    directory = web / "public/avatars"
    changes = {}
    for key, names in targets.items():
        old = manifest["assets"][key]
        layers = {}
        for name in names:
            filename = f"{key}.{name}.webp"
            layers[name] = file_record(filename, files[filename])
        entry = {**old, "format": "webp", "layers": layers}
        if old != entry:
            changes[key] = old, entry
            manifest["assets"][key] = entry
    migration_path = None
    if changes:
        migration_path = args.migration or next_migration()
        migration = registry_migration(changes)
        if migration_path.exists() and migration_path.read_text(encoding="utf-8") != migration:
            parser.error("existing migration differs; preserve its checksum and specify a new migration path")
    write_preview(preview_path, files)
    for name, data in files.items():
        path = directory / name
        if not path.is_file() or path.read_bytes() != data:
            path.write_bytes(data)
            print(f"{name} {len(data)} bytes")
    if migration_path is not None:
        migration_path.write_bytes(migration.encode("utf-8"))
        print(f"Registry migration: {migration_path}")
    manifest_bytes = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    if manifest_path.read_bytes() != manifest_bytes:
        manifest_path.write_bytes(manifest_bytes)
    print(f"Preview: {preview_path}")


if __name__ == "__main__":
    main()
