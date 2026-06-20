#!/usr/bin/env python3
"""
将纯文本渲染为 PNG，用于在仓库内生成“自测截图”（可复现、可审计）。

用法:
  python3 tools/text_to_png.py --in docs/self_test/logs/a.txt --out docs/self_test/screenshots/a.png --title "xxx"
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFont


@dataclass(frozen=True)
class Style:
    padding: int = 24
    line_gap: int = 6
    bg: tuple[int, int, int] = (14, 18, 24)
    fg: tuple[int, int, int] = (230, 235, 240)
    muted: tuple[int, int, int] = (160, 170, 180)


def load_font(size: int) -> ImageFont.ImageFont:
    # 优先尝试常见等宽字体；找不到就回退默认字体。
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/Library/Fonts/Menlo.ttc",
    ]
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size=size)
            except Exception:
                pass
    return ImageFont.load_default()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--in", dest="in_path", required=True)
    parser.add_argument("--out", dest="out_path", required=True)
    parser.add_argument("--title", default="")
    parser.add_argument("--font-size", type=int, default=16)
    args = parser.parse_args()

    style = Style()
    font = load_font(args.font_size)
    title_font = load_font(args.font_size + 2)

    with open(args.in_path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()

    # 控制图片大小：避免超长文本导致输出不可用。
    lines = raw.splitlines()
    max_lines = 40
    if len(lines) > max_lines:
        lines = lines[: max_lines - 1] + ["...（已截断）"]

    title = args.title.strip()
    if title:
        lines = [title, ""] + lines

    # 计算画布
    dummy = Image.new("RGB", (10, 10))
    draw = ImageDraw.Draw(dummy)

    widths = []
    heights = []
    for idx, line in enumerate(lines):
        font_to_use = title_font if title and idx == 0 else font
        bbox = draw.textbbox((0, 0), line, font=font_to_use)
        widths.append(bbox[2] - bbox[0])
        heights.append(bbox[3] - bbox[1])

    text_w = max(widths) if widths else 0
    text_h = sum(heights) + style.line_gap * max(0, len(lines) - 1)
    img_w = text_w + style.padding * 2
    img_h = text_h + style.padding * 2

    img = Image.new("RGB", (img_w, img_h), style.bg)
    draw = ImageDraw.Draw(img)

    y = style.padding
    for idx, line in enumerate(lines):
        font_to_use = title_font if title and idx == 0 else font
        color = style.muted if title and idx == 0 else style.fg
        draw.text((style.padding, y), line, font=font_to_use, fill=color)
        y += heights[idx] + style.line_gap

    os.makedirs(os.path.dirname(args.out_path), exist_ok=True)
    img.save(args.out_path, format="PNG")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

