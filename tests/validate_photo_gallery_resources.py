"""Check the photo-gallery resources the UI and the code actually reference.

Two defects this catches, both of which happened during development:
a toast shown with a missing key (empty title), and a duplicated <data name>
entry in a .resw. Runs offline, changes nothing.
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
resw = repo / "src/GMagpie"
BASE = "language-en-US"
# Локали, которые ведёт эта задача; остальные наследуют en-US
MAINTAINED = [BASE, "language-ru", "language-zh-Hans", "language-zh-Hant"]
# Домашняя страница задаёт префикс x:Uid-ключей, сам показ — «Gallery_»
# (тосты поверх окна показа берутся через _ResourceString).
PREFIXES = ("Home_Gallery", "Gallery_")


def is_gallery(name):
    return name.startswith(PREFIXES)


def keys(locale):
    path = resw / f"Resources.{locale}.resw"
    assert path.is_file(), f"missing resource file {path}"
    found = []
    for data in ET.parse(path).getroot().findall("data"):
        name = data.get("name")
        value = data.find("value")
        found.append((name, value is not None and bool((value.text or "").strip())))
    return found


failures = []
per_locale = {}
for locale in MAINTAINED:
    found = keys(locale)
    names = [name for name, _ in found]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        failures.append(f"{locale}: duplicate keys {duplicates}")
    empty = sorted(name for name, filled in found if not filled and is_gallery(name))
    if empty:
        failures.append(f"{locale}: empty values {empty}")
    per_locale[locale] = set(names)

# Ссылки из XAML (x:Uid) и из кода (GetString) должны существовать в базовой локали
referenced = set()
for path in sorted(resw.glob("*.xaml")):
    for uid in re.findall(r'x:Uid="([A-Za-z0-9_.]+)"', path.read_text(encoding="utf-8-sig")):
        if is_gallery(uid):
            referenced.add(uid)
for path in sorted(list(resw.glob("*.cpp")) + list(resw.glob("*.h"))):
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    for name in re.findall(r'(?:GetString|_ResourceString)\(L?"([A-Za-z0-9_.]+)"\)', text):
        if is_gallery(name):
            referenced.add(name)

missing = sorted(name for name in referenced if name not in per_locale[BASE])
# x:Uid раскрывается в <uid>.Content / .Text / .Header / .Description / .ToolTip
unresolved = [name for name in missing if not any(
    f"{name}.{suffix}" in per_locale[BASE]
    for suffix in ("Content", "Text", "Header", "Description", "ToolTip", "HelpText"))]
if unresolved:
    failures.append(f"referenced but not defined in {BASE}: {unresolved}")

for locale in MAINTAINED[1:]:
    absent = sorted(name for name in per_locale[BASE]
                    if is_gallery(name) and name not in per_locale[locale])
    if absent:
        failures.append(f"{locale}: missing keys present in {BASE}: {absent}")

print(f"locales checked: {len(MAINTAINED)}, gallery keys in base: "
      f"{len([n for n in per_locale[BASE] if is_gallery(n)])}, "
      f"names referenced from UI/code: {len(referenced)}")
for failure in failures:
    print("FAIL " + failure)
if failures:
    sys.exit(1)
print("Photo gallery resources: no duplicates, no empty values, every referenced "
      "name resolves in the base locale, maintained locales are complete.")
