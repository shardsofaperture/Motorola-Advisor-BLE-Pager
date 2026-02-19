#!/usr/bin/env python3
"""
Edit the two source PNGs in app/icon/ and re-run the script to regenerate res icons.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_DIR = REPO_ROOT / "android" / "native-app" / "app"
ICON_DIR = APP_DIR / "icon"

MAIN_ICON_SOURCE = ICON_DIR / "mainicon.png"
NOTIFICATION_ICON_SOURCE = ICON_DIR / "notificationbar.png"

LAUNCHER_OUTPUTS = {
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}

NOTIFICATION_OUTPUTS = {
    "mdpi": 24,
    "hdpi": 36,
    "xhdpi": 48,
    "xxhdpi": 72,
    "xxxhdpi": 96,
}


def require_file(path: Path) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"Missing source icon: {path}")


def load_rgba(path: Path) -> Image.Image:
    with Image.open(path) as img:
        return img.convert("RGBA")


def write_scaled_png(src_rgba: Image.Image, size_px: int, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    resized = src_rgba.resize((size_px, size_px), Image.Resampling.LANCZOS)
    resized.save(out_path, format="PNG")
    validate_output(out_path, size_px)


def validate_output(path: Path, expected_size_px: int) -> None:
    with Image.open(path) as img:
        if img.size != (expected_size_px, expected_size_px):
            raise ValueError(
                f"Bad size for {path}: got {img.size}, expected {(expected_size_px, expected_size_px)}"
            )
        if img.mode != "RGBA" or "A" not in img.getbands():
            raise ValueError(f"Bad mode for {path}: expected RGBA with alpha, got {img.mode}")


def generate_launcher_icons(main_rgba: Image.Image) -> None:
    for density, size_px in LAUNCHER_OUTPUTS.items():
        out_dir = APP_DIR / "src" / "main" / "res" / f"mipmap-{density}"
        write_scaled_png(main_rgba, size_px, out_dir / "ic_launcher.png")
        write_scaled_png(main_rgba, size_px, out_dir / "ic_launcher_round.png")


def generate_notification_icons(notification_rgba: Image.Image) -> None:
    for density, size_px in NOTIFICATION_OUTPUTS.items():
        out_dir = APP_DIR / "src" / "main" / "res" / f"drawable-{density}"
        write_scaled_png(notification_rgba, size_px, out_dir / "ic_stat_pager.png")


def main() -> None:
    require_file(MAIN_ICON_SOURCE)
    require_file(NOTIFICATION_ICON_SOURCE)

    main_rgba = load_rgba(MAIN_ICON_SOURCE)
    notification_rgba = load_rgba(NOTIFICATION_ICON_SOURCE)

    generate_launcher_icons(main_rgba)
    generate_notification_icons(notification_rgba)

    print("Generated launcher and notification icons from app/icon sources.")


if __name__ == "__main__":
    main()
