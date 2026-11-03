#!/usr/bin/env python3
"""Builds assets/apple/Acknowledgements.json, the license notices shown in the app.

The list is the third-party code that is linked into the XeniOS app (the link line of
the xenia-app target plus the header-only libraries the iOS sources include). Add a
library here when you link a new one. Each entry reads its license text from the
library's directory under third_party/, so a missing or moved file fails the run
instead of shipping an empty notice.

Usage: tools/ios/gen_acknowledgements.py [--check]
  --check   exit 1 if the checked-in JSON is out of date, without writing.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TP = ROOT / "third_party"
OUT = ROOT / "assets" / "apple" / "Acknowledgements.json"


def spdx_body(text):
    """Mesa's licenses/ files are SPDX templates; keep only the license text."""
    marker = "License-Text:"
    return text.split(marker, 1)[1].strip() if marker in text else text


def stb_license(text):
    """stb headers end with a dual MIT / public domain notice."""
    match = re.search(r"-{20,}\s*\n(This software is available under 2 licenses.*?)\*/",
                      text, re.S)
    if not match:
        raise SystemExit("stb: license block not found in stb_image.h")
    return match.group(1).strip()


# (display name, what it is used for, project URL, [license sources], optional note)
# A source is a path under third_party/, or (path, transform).
LIBRARIES = [
    ("FFmpeg (libavcodec, libavformat, libavutil)", "XMA and audio decoding",
     "https://ffmpeg.org",
     ["FFmpeg/COPYING.LGPLv2.1", "FFmpeg/LICENSE.md"],
     "Linked statically under the LGPL v2.1 or later (the build configuration says so). "
     "The corresponding source is available from the XeniOS source repository, "
     "https://github.com/xenios-jp/XeniOS."),
    ("libmspack", "Compressed content decoding", "https://www.cabextract.org.uk/libmspack/",
     ["mspack/COPYING.LIB"],
     "Linked statically under the LGPL v2.1. The corresponding source is available from "
     "the XeniOS source repository, https://github.com/xenios-jp/XeniOS."),
    ("SDL", "Audio output and game controllers", "https://libsdl.org",
     ["SDL3/LICENSE.txt"], None),
    ("Mesa (NIR, SPIR-V to DXIL)", "Shader translation", "https://www.mesa3d.org",
     ["mesa/docs/license.rst", ("mesa/licenses/MIT", spdx_body)], None),
    ("SPIRV-Cross", "Shader translation", "https://github.com/KhronosGroup/SPIRV-Cross",
     ["SPIRV-Cross/LICENSE"], None),
    ("glslang (SPIR-V)", "Shader translation", "https://github.com/KhronosGroup/glslang",
     ["glslang/LICENSE.txt"], None),
    ("spirv2air", "Shader translation", None, ["spirv2air/LICENSE"], None),
    ("LLVM (headers)", "Shader translation", "https://llvm.org",
     ["llvm/LICENSE.txt"], None),
    ("metal-cpp", "Metal C++ bindings", "https://developer.apple.com/metal/cpp/",
     ["metal-cpp/LICENSE.txt"], None),
    ("Metal Shader Converter headers", "Shader translation",
     "https://developer.apple.com/metal/shader-converter/",
     ["metal-shader-converter/LICENSE.txt"],
     "This is the license of the headers. The converter library itself is Apple software "
     "under Apple's own terms."),
    ("Capstone", "Disassembly", "https://www.capstone-engine.org",
     ["capstone/LICENSES/LICENSE.TXT", "capstone/LICENSES/LICENSE_BSD_3_CLAUSE.txt",
      "capstone/LICENSES/LICENSE_LLVM.TXT"], None),
    ("Dear ImGui", "Debug overlay", "https://github.com/ocornut/imgui",
     ["imgui/LICENSE.txt"], None),
    ("{fmt}", "Text formatting", "https://github.com/fmtlib/fmt", ["fmt/LICENSE"], None),
    ("toml++", "Configuration files", "https://marzer.github.io/tomlplusplus/",
     ["tomlplusplus/LICENSE"], None),
    ("cxxopts", "Command line options", "https://github.com/jarro2783/cxxopts",
     ["cxxopts/LICENSE"], None),
    ("UTF8-CPP", "UTF-8 conversion", "https://github.com/nemtrif/utfcpp", ["utfcpp/LICENSE"],
     None),
    ("half", "Half-precision floats", "https://half.sourceforge.net", ["half/LICENSE.txt"],
     None),
    ("RapidJSON", "JSON parsing", "https://rapidjson.org", ["rapidjson/license.txt"], None),
    ("pugixml", "XML parsing", "https://pugixml.org", ["pugixml/LICENSE.md"], None),
    ("stb", "Image reading and writing", "https://github.com/nothings/stb",
     [("stb/stb_image.h", stb_license)], None),
    ("Mbed TLS", "Hashing and encryption", "https://www.trustedfirmware.org/projects/mbed-tls/",
     ["mbedtls/LICENSE"], None),
    ("zlib-ng", "Compression", "https://github.com/zlib-ng/zlib-ng", ["zlib-ng/LICENSE.md"],
     None),
    ("Zstandard", "Compression", "https://facebook.github.io/zstd/", ["zstd/LICENSE"], None),
    ("Snappy", "Compression", "https://github.com/google/snappy", ["snappy/COPYING"], None),
    ("xxHash", "Hashing", "https://xxhash.com", ["xxhash/LICENSE"], None),
    ("ZArchive", "Archive format", "https://github.com/Exzap/ZArchive", ["zarchive/LICENSE"],
     None),
    ("Xbyak for AArch64", "CPU code generation", "https://github.com/fujitsu/xbyak_aarch64",
     ["xbyak_aarch64/LICENSE"], None),
    ("Boost.Context", "Fibers", "https://www.boost.org/doc/libs/release/libs/context/",
     [("mesa/licenses/BSL-1.0", spdx_body)],
     "The standard Boost Software License 1.0 text is shown; third_party/boost_context has "
     "no license file of its own in this checkout."),
    ("Disruptor-plus", "Lock-free queues", "https://github.com/lewissbaker/disruptorplus",
     ["disruptorplus/LICENSE.txt"], None),
    ("Asio", "Networking", "https://think-async.com/Asio/", ["asio/LICENSE_1_0.txt"], None),
    ("DXBC checksum (AMD)", "Shader container checksum", None, [],
     "Derived from DXBCChecksum.cpp by the AMD Developer Tools Team (copyright 2008-2016 "
     "Advanced Micro Devices, Inc.), which includes an MD5 implementation. The file carries "
     "no license text of its own."),
]


def read(spec):
    transform = None
    if isinstance(spec, tuple):
        spec, transform = spec
    path = TP / spec
    if not path.is_file():
        raise SystemExit(f"missing license file: {path}")
    text = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    return (transform(text) if transform else text).strip()


def build():
    entries = [{
        "name": "Xenia",
        "usage": "The emulator XeniOS is built on",
        "url": "https://github.com/xenia-project/xenia",
        "text": (ROOT / "LICENSE").read_text(encoding="utf-8").strip(),
    }]
    for name, usage, url, sources, note in LIBRARIES:
        text = "\n\n".join(read(s) for s in sources)
        entry = {"name": name, "usage": usage}
        if url:
            entry["url"] = url
        if note:
            entry["note"] = note
        entry["text"] = text
        entries.append(entry)
    return json.dumps({"entries": entries}, indent=1, ensure_ascii=False) + "\n"


def main():
    data = build()
    if "--check" in sys.argv:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != data:
            print(f"{OUT} is out of date; run tools/ios/gen_acknowledgements.py")
            return 1
        return 0
    OUT.write_text(data, encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(data) // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
