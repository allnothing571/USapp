#!/usr/bin/env python3
"""
生成 PWA 图标。

为什么用脚本而不是提交一堆二进制手绘素材：
图标本质上由一组品牌常量决定（底色、两个交叠圆、安全区比例）。把常量写进脚本后，
换色只改一处，且 192/512/maskable/apple-touch 四种尺寸不会各自漂移。

用法（由项目负责人手动执行）：
    python apps/web/scripts/generate-icons.py

依赖：Pillow（harness 自带的 Python 运行时已包含）。
本脚本只写文件，不访问网络。
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw

# ---------------------------------------------------------------- 品牌常量
BACKGROUND = (251, 247, 240, 255)  # #FBF7F0 暖纸底
INK = (47, 72, 88, 255)  # #2F4858 深板岩
ACCENT = (194, 85, 74, 255)  # #C2554A 陶土红

OUT_DIR = pathlib.Path(__file__).resolve().parents[1] / "public"
ICONS_DIR = OUT_DIR / "icons"


def render_icon(size: int, *, maskable: bool) -> Image.Image:
    """渲染一枚图标。

    maskable=True 时把图形压到中心 60% 的安全区内，因为 Android 会按自己的
    形状裁切；安全区外必须是可被裁掉的纯背景，否则图形会被切掉一角。
    """
    # 先从 4 倍分辨率渲染再缩小，边缘才不会锯齿。Lanczos 缩小是 Pillow 里质量最好的。
    scale = 4
    canvas_size = size * scale

    image = Image.new("RGBA", (canvas_size, canvas_size), BACKGROUND)
    draw = ImageDraw.Draw(image)

    # maskable 版本图形更小（占 60%），普通版本占 78%。
    content_ratio = 0.60 if maskable else 0.78
    content = canvas_size * content_ratio
    radius = content / 2.6
    center = canvas_size / 2

    # 两个圆的圆心在水平方向分离，分离量约为半径的 1.05 倍，
    # 保证既有明显交叠、又不会看起来像一个大 blob。
    separation = radius * 1.05 / 2

    left_box = (
        center - separation - radius,
        center - radius,
        center - separation + radius,
        center + radius,
    )
    right_box = (
        center + separation - radius,
        center - radius,
        center + separation + radius,
        center + radius,
    )

    # 先画右侧深色圆，再画左侧陶土圆完整覆盖交叠区。
    # 这样左圆是一个完整可辨认的圆形，交叠处的暗色只是右侧圆被压住的部分，
    # 视觉上"两个人、两个颜色、中间是共同的部分"。
    # 不用 alpha 混合：混合会引入依赖渲染器的中间色，maskable 缩小时容易脏。
    draw.ellipse(right_box, fill=INK)
    draw.ellipse(left_box, fill=ACCENT)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    ICONS_DIR.mkdir(parents=True, exist_ok=True)

    targets = [
        (ICONS_DIR / "icon-192.png", 192, False),
        (ICONS_DIR / "icon-512.png", 512, False),
        (ICONS_DIR / "icon-maskable-512.png", 512, True),
        (OUT_DIR / "apple-touch-icon.png", 180, True),
    ]

    for path, size, maskable in targets:
        icon = render_icon(size, maskable=maskable)
        # apple-touch-icon 不支持透明；本图本就是不透明底，这里显式转 RGB 以免校验工具报警。
        if path.name == "apple-touch-icon.png":
            icon = icon.convert("RGB")
        icon.save(path, format="PNG", optimize=True)
        print(f"已写入 {path.relative_to(OUT_DIR.parent.parent.parent)} ({size}x{size})")


if __name__ == "__main__":
    main()
