#!/usr/bin/env python3
"""Builds assets/apple/Base.lproj/Localizable.strings from the iOS sources.

Every user-facing string in src/xenia/ui/ios is an NSLocalizedString(@"<English>",
@"<comment>") call whose key is the English text, so the table maps each key to itself
until translations are added. genstrings (Xcode) does the parsing; this script runs it
over every source file that calls NSLocalizedString and stores the result as UTF-8 so
it diffs. Run it after adding or changing a user-facing string.

Usage: tools/ios/gen_localizable_strings.py [--check]
  --check   exit 1 if the checked-in table is out of date, without writing.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCES = ROOT / "src" / "xenia" / "ui" / "ios"
OUT = ROOT / "assets" / "apple" / "Base.lproj" / "Localizable.strings"


def generate():
    files = sorted(
        str(path)
        for pattern in ("*.mm", "*.m", "*.h")
        for path in SOURCES.rglob(pattern)
        if "NSLocalizedString(" in path.read_text(encoding="utf-8", errors="replace")
    )
    with tempfile.TemporaryDirectory() as out_dir:
        # -noPositionalParameters: by default genstrings rewrites the value of a key with
        # two or more arguments into %1$@ form and drops widths on the way (%08X becomes
        # %3$X), which would change the English on screen.
        result = subprocess.run(
            ["xcrun", "genstrings", "-q", "-noPositionalParameters", "-o", out_dir, *files],
            capture_output=True,
            text=True,
        )
        # genstrings warns about keys used with more than one comment and still writes
        # them; anything else (an argument that isn't a literal) is an error.
        errors = [line for line in result.stderr.splitlines() if "error:" in line]
        if result.returncode != 0 or errors:
            sys.exit("genstrings failed:\n" + (result.stderr or str(result.returncode)))
        raw = (pathlib.Path(out_dir) / "Localizable.strings").read_bytes()
    text = raw.decode("utf-16")
    # Until translations exist, every value must be its key.
    for key, value in re.findall(r'^"((?:[^"\\]|\\.)*)" = "((?:[^"\\]|\\.)*)";$', text, re.M):
        if key != value:
            sys.exit(f"genstrings changed a value: {key!r} -> {value!r}")
    return text


def main():
    text = generate()
    if "--check" in sys.argv[1:]:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            sys.exit(f"{OUT.relative_to(ROOT)} is out of date; run {pathlib.Path(__file__).name}")
        return
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}: {text.count(chr(10) + chr(34))} strings")


if __name__ == "__main__":
    main()
