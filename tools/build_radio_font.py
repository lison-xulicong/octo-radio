#!/usr/bin/env python3
"""Regenerate the main UI font with nationwide cities and common Chinese text."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def gb2312_characters() -> set[str]:
    """Every character representable by the GB2312 double-byte encoding."""
    characters: set[str] = set()
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                decoded = bytes((lead, trail)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            characters.update(decoded)
    return characters


def non_ascii_from_source() -> set[str]:
    """UI text plus all city/province names, not generated binary contents."""
    paths = sorted(MAIN.glob("*.cc")) + sorted(MAIN.glob("*.h"))
    characters: set[str] = set()
    for path in paths:
        characters.update(
            character
            for character in path.read_text(encoding="utf-8")
            if ord(character) > 0x7F
        )
    return characters


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--font",
        required=True,
        help="Noto Sans SC variable TTF used by the existing thin UI font",
    )
    args = parser.parse_args()

    font = Path(args.font)
    if not font.is_file():
        parser.error(f"font not found: {font}")

    characters = gb2312_characters() | non_ascii_from_source()
    common = [
        "npx",
        "--yes",
        "lv_font_conv",
        "--font",
        str(font),
        "--format",
        "lvgl",
        "--bpp",
        "4",
        "--no-compress",
        "--no-prefilter",
        "--lv-include",
        "lvgl.h",
    ]

    main_output = MAIN / "radio_font.c"
    main_symbols = "".join(sorted(characters))
    main_command = common + [
        "--symbols",
        main_symbols,
        "--range",
        "0x20-0x7f",
        "--size",
        "16",
        "--lv-font-name",
        "radio_font",
        "-o",
        str(main_output),
    ]
    print(f"Generating {main_output} with {len(characters)} non-ASCII glyphs")
    subprocess.run(main_command, check=True, cwd=ROOT)
    main_text = main_output.read_text(encoding="utf-8")
    main_text = main_text.replace(str(font), font.name, 1)
    main_text = main_text.replace(str(main_output), str(main_output.relative_to(ROOT)), 1)
    main_output.write_text(main_text, encoding="utf-8")

    # The title font is only used for the "联网收音机" heading. Keeping its
    # former city/station alphabet in Flash served no runtime purpose.
    title_output = MAIN / "radio_font_title.c"
    title_command = common + [
        "--symbols",
        "联网收音机",
        "--size",
        "20",
        "--lv-font-name",
        "radio_font_title",
        "-o",
        str(title_output),
    ]
    print(f"Generating {title_output} with 5 title glyphs")
    subprocess.run(title_command, check=True, cwd=ROOT)
    title_text = title_output.read_text(encoding="utf-8")
    title_text = title_text.replace(str(font), font.name, 1)
    title_text = title_text.replace(str(title_output), str(title_output.relative_to(ROOT)), 1)
    title_output.write_text(title_text, encoding="utf-8")


if __name__ == "__main__":
    main()
