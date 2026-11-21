"""Measure where photo-flip latency actually goes, from the app's own log.

For each page flip (_ShowCurrent) we take the time to the next _DecodeCanvas
completion and the time to the next published frame (Capture sequence accepted /
publication timing lines). Read-only.
"""
import re
import sys
from datetime import datetime
from pathlib import Path

logs = sorted(Path(r"F:/Projects/cpp/Magpie/bin/x64/Release/logs").glob("magpie*.log"))
events = []
ts = "%Y-%m-%d %H:%M:%S.%f"
for path in logs:
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        m = re.match(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d\d\d)\|", line)
        if not m:
            continue
        when = datetime.strptime(m.group(1), ts)
        if "|GalleryService.cpp:" in line and "|_ShowCurrent|" in line:
            events.append((when, "flip"))
        elif "_DecodeCanvas|" in line and "|info|" in line:
            events.append((when, "decoded"))
        elif "|_ResizePhotoEffects|" in line and "|info|" in line:
            events.append((when, "chain"))
        elif "Capture sequence accepted" in line:
            events.append((when, "frame"))

events.sort(key=lambda e: e[0])
if not events:
    print("no events")
    sys.exit(0)

flips, decode_ms, chain_ms, frame_ms = 0, [], [], []
for i, (when, kind) in enumerate(events):
    if kind != "flip":
        continue
    flips += 1
    got = {}
    for when2, kind2 in events[i + 1:i + 40]:
        if kind2 in ("decoded", "chain", "frame") and kind2 not in got:
            got[kind2] = (when2 - when).total_seconds() * 1000.0
            if len(got) == 3:
                break
    for key, target in (("decoded", decode_ms), ("chain", chain_ms), ("frame", frame_ms)):
        if key in got:
            target.append(got[key])


def stats(name, values):
    if not values:
        print(f"{name}: no samples")
        return
    values = sorted(values)
    n = len(values)
    print(f"{name}: n={n} p50={values[n // 2]:.1f}ms p90={values[int(n * 0.9)]:.1f}ms "
          f"max={values[-1]:.1f}ms")


print(f"flips in log: {flips}")
stats("flip -> photo decoded ", decode_ms)
stats("flip -> effect chain built", chain_ms)
stats("flip -> frame published  ", frame_ms)
