"""Copy the per-theme Games captures from a harness run into design/native.

Reads the t<i>-games.png after-shots (actual C renderer output driven by
the real menu code) and writes one <theme>-games.png per theme.
"""
from pathlib import Path
import sys
from PIL import Image

shots = Path(sys.argv[1] if len(sys.argv) > 1 else "build-tests/shots-after")
out_dir = Path(sys.argv[2] if len(sys.argv) > 2 else "design/native")
out_dir.mkdir(parents=True, exist_ok=True)

names = ("midnight", "mono", "blue-wave", "daylight", "cinema", "orbis")

for i, name in enumerate(names):
    src = shots / f"t{i}-games.png"
    if not src.exists():
        sys.exit(f"missing capture: {src}")
    Image.open(src).save(out_dir / f"{name}-games.png")
    print(f"{name}-games.png")
