#!/usr/bin/env python3
"""Install actool output and merge its icon metadata into an iOS app bundle."""

import pathlib
import plistlib
import shutil
import sys


def main():
    assets, app = map(pathlib.Path, sys.argv[1:])
    with (assets / "asset-info.plist").open("rb") as source:
        asset_info = plistlib.load(source)
    info_path = app / "Info.plist"
    with info_path.open("rb") as source:
        info = plistlib.load(source)
    for path in assets.iterdir():
        if path.name != "asset-info.plist" and path.is_file():
            shutil.copy2(path, app / path.name)
    info.update(asset_info)
    with info_path.open("wb") as destination:
        plistlib.dump(info, destination)


if __name__ == "__main__":
    main()
