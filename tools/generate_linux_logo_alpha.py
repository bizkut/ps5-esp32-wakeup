#!/usr/bin/env python3
"""Generate the final-size anti-aliased Linux LCD logo asset."""

from __future__ import annotations

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ps5-linux.jpg"
HEADER = ROOT / "main" / "logo_assets.h"
OUT_W = 135
OUT_H = 156


def format_array(values: bytes) -> str:
    lines = []
    for i in range(0, len(values), 16):
        chunk = ", ".join(f"0x{value:02x}" for value in values[i : i + 16])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def main() -> int:
    img = Image.open(SOURCE).convert("L")
    bbox = img.point(lambda p: 255 if p < 245 else 0).getbbox()
    if not bbox:
        raise RuntimeError("could not find dark content in Linux logo source")

    pad = 18
    left = max(0, bbox[0] - pad)
    top = max(0, bbox[1] - pad)
    right = min(img.width, bbox[2] + pad)
    bottom = min(img.height, bbox[3] + pad)
    cropped = img.crop((left, top, right, bottom))
    cropped.thumbnail((OUT_W, OUT_H), Image.Resampling.LANCZOS)

    canvas = Image.new("L", (OUT_W, OUT_H), 255)
    x = (OUT_W - cropped.width) // 2
    y = (OUT_H - cropped.height) // 2
    canvas.paste(cropped, (x, y))
    alpha_values = []
    for value in canvas.tobytes():
        alpha = 255 - value
        alpha_values.append(0 if alpha < 18 else alpha)
    alpha = bytes(alpha_values)

    block = (
        f"#define LINUX_LOGO_W {OUT_W}\n"
        f"#define LINUX_LOGO_H {OUT_H}\n"
        f"#define LINUX_LOGO_ALPHA 1\n"
        f"static const uint8_t linux_logo_alpha[{len(alpha)}] = {{\n"
        f"{format_array(alpha)}\n"
        f"}};\n"
    )

    text = HEADER.read_text(encoding="utf-8")
    marker = "#define LINUX_LOGO_W "
    start = text.index(marker)
    text = text[:start] + block
    HEADER.write_text(text, encoding="utf-8")
    print(f"wrote {HEADER}")
    print(f"source={img.width}x{img.height} crop={right-left}x{bottom-top}")
    print(f"asset={OUT_W}x{OUT_H} bytes={len(alpha)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
