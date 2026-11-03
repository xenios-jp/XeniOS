#!/usr/bin/env python3
"""Stamp an iOS app bundle's version and build number.

CFBundleShortVersionString is the newest tag reachable from HEAD that matches
--tag-pattern, without its leading "v" (v1.2.3 -> 1.2.3), or 0.0.0 when no tag
matches. CFBundleVersion is the commit count of HEAD, which only grows along a
branch; the update check compares builds of one version by it, and treats a
build number of 1 as an unstamped development build. --version and --build
override either, for builds made outside a git checkout.
"""

import argparse
import pathlib
import plistlib
import re
import subprocess
import sys


def git(source_dir, *args):
    result = subprocess.run(
        ["git", "-C", str(source_dir), *args], capture_output=True, text=True
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("app", type=pathlib.Path)
    parser.add_argument("source_dir", type=pathlib.Path)
    parser.add_argument("--tag-pattern", default="v[0-9]*")
    parser.add_argument("--version", default="")
    parser.add_argument("--build", default="")
    args = parser.parse_args()

    version = args.version
    if not version:
        tag = git(args.source_dir, "describe", "--tags", "--abbrev=0",
                  "--match", args.tag_pattern)
        version = tag[1:] if tag[:1] in ("v", "V") else tag
    version = version or "0.0.0"
    # iOS requires one to three period-separated integers.
    if not re.fullmatch(r"\d+(\.\d+){0,2}", version):
        sys.exit(f"stamp_ios_version: {version!r} is not a valid iOS version")
    build = args.build or git(args.source_dir, "rev-list", "--count", "HEAD")
    if not build.isdigit():
        sys.exit("stamp_ios_version: no build number (not a git checkout? "
                 "pass XENIOS_BUILD_NUMBER)")

    info_path = args.app / "Info.plist"
    with info_path.open("rb") as source:
        info = plistlib.load(source)
    stamped = {
        "CFBundleShortVersionString": version,
        "CFBundleVersion": build,
    }
    if all(info.get(key) == value for key, value in stamped.items()):
        return
    info.update(stamped)
    with info_path.open("wb") as destination:
        plistlib.dump(info, destination)


if __name__ == "__main__":
    main()
