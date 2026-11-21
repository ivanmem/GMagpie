# -*- coding: utf-8 -*-
"""Audit: CJK characters on lines the working-tree diff ADDS (the honest check:
PowerShell pipes mangle git's UTF-8 output, so this runs git through Python)."""
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
CJK = re.compile(r"[一-鿿　-〿＀-￯]")

files = subprocess.run(["git", "diff", "HEAD", "--name-only"], capture_output=True,
                       text=True, cwd=REPO).stdout.split()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
total = 0
for f in files:
    if not f.endswith((".cpp", ".h", ".idl", ".xaml")):
        continue
    d = subprocess.run(["git", "diff", "--unified=0", "--", f], capture_output=True,
                       text=True, encoding="utf-8", cwd=REPO).stdout
    hits = [l for l in d.splitlines() if l.startswith("+") and not l.startswith("+++") and CJK.search(l)]
    if hits:
        print(f"=== {f} : {len(hits)}")
        for h in hits:
            print(h)
        total += len(hits)
print("total added CJK lines:", total)
