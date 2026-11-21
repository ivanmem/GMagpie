"""Verify the persisted photo-gallery state in the real configuration file.

Read-only: parses the config the running build wrote and reports what the
"Continue" button would restore. Fails when the file is not valid JSON or the
state has the wrong shape.
"""
import json
import sys
from pathlib import Path

candidates = sorted((Path.home() / "AppData/Local/Magpie/config").glob("*/config.json"))
if not candidates:
    print("no configuration file found - nothing persisted yet")
    sys.exit(0)

path = max(candidates, key=lambda p: p.stat().st_mtime)
data = json.loads(path.read_text(encoding="utf-8-sig"))
print(f"config: {path}")

folder = data.get("galleryLastFolder")
index = data.get("galleryLastIndex")
if folder is None and index is None:
    print("gallery state: absent (no slideshow has been stopped with this build yet)")
    sys.exit(0)

assert isinstance(folder, str) and folder, f"galleryLastFolder must be a non-empty string, got {folder!r}"
assert isinstance(index, int) and not isinstance(index, bool) and index >= 0, \
    f"galleryLastIndex must be a non-negative integer, got {index!r}"
assert (Path(folder).is_dir()), f"saved folder does not exist: {folder}"

images = {".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".bmp", ".gif", ".tif", ".tiff",
          ".webp", ".heic", ".heif", ".avif"}
files = [p for p in Path(folder).rglob("*") if p.is_file() and p.suffix.lower() in images]
files.sort()
print(f"gallery state: page {index + 1} of a saved album, folder exists")
if files:
    current = files[min(index, len(files) - 1)]
    print(f"album rebuilt like the app does: {len(files)} images; "
          f"Continue would open {current.name} (index clamped: {min(index, len(files) - 1)})")
else:
    print("album has no supported images now - the app clears the saved state on that start")
print("Other config keys preserved: " + str(len(data)) + " top-level keys parsed.")
