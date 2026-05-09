#!/usr/bin/env python3
"""
Flash a prepped character pack via USB (pio run -t uploadfs).
Faster than the BLE drop target when you're iterating on a character.

Usage:
  python3 tools/flash_character.py characters/bufo
"""
import json, sys, shutil, subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
DATA    = PROJECT / "data" / "characters"
CAP     = 1_800_000


def _find_platformio() -> list[str]:
    for cmd in (["pio"], ["platformio"]):
        if shutil.which(cmd[0]):
            return cmd
    raise SystemExit(
        "PlatformIO CLI not found. Install it first, then rerun either:\n"
        "  pipx install platformio\n"
        "or:\n"
        "  python3 -m pip install --user platformio\n"
        "After install, run:\n"
        "  pio run -t uploadfs"
    )


def flash(src: Path) -> None:
    if not (src / "manifest.json").exists():
        sys.exit(f"no manifest.json in {src} — run tools/prep_character.py first")
    name = json.loads((src / "manifest.json").read_text())["name"]

    total = sum(f.stat().st_size for f in src.iterdir() if f.is_file())
    if total > CAP:
        sys.exit(f"{total:,} bytes — over the {CAP:,} LittleFS cap")

    # uploadfs flashes everything under data/; the firmware only reads one
    # character at a time, so a stale sibling just wastes partition space.
    if DATA.exists():
        shutil.rmtree(DATA)
    dst = DATA / name
    shutil.copytree(src, dst)
    print(f"staged {name}: {total:,} bytes -> {dst}")

    subprocess.run([*_find_platformio(), "run", "-t", "uploadfs"], cwd=PROJECT, check=True)
    print(f"\nflashed. on the stick: hold A -> settings -> species -> GIF")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    flash(Path(sys.argv[1]).resolve())
