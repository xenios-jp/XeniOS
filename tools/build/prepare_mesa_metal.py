#!/usr/bin/env python3
"""Prepare the optional MSC compiler overlay without modifying the Mesa checkout."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

STAMP = ".xenia-metal-overlay.json"
VERSION_HEADER = "xenia-metal/xenia_mesa_metal_version.h"
IGNORED = {".git", "__pycache__", ".DS_Store", STAMP}


def replace(text, before, after, count=1):
    if text.count(before) != count:
        raise RuntimeError("Unsupported Mesa source layout near: " + before[:80])
    return text.replace(before, after)


def patched_sources(source):
    api = "src/microsoft/spirv_to_dxil/spirv_to_dxil.h"
    implementation = "src/microsoft/spirv_to_dxil/spirv_to_dxil.c"
    emitter = "src/microsoft/compiler/nir_to_dxil.c"
    lowering = "src/compiler/spirv/vtn_glsl450.c"
    header = replace(
        (source / api).read_text(),
        "   bool disable_math_refactoring;\n};",
        "   bool disable_math_refactoring;\n\n"
        "   // Retain explicit 32-bit multiply-add operations for MSC.\n"
        "   // This does not enable implicit contraction.\n"
        "   bool preserve_msc_fmad32;\n};",
    )
    code = replace(
        (source / implementation).read_text(),
        "   nir_options.lower_base_vertex =",
        "   if (conf->preserve_msc_fmad32)\n"
        "      nir_options.float_mul_add32 =\n"
        "         nir_float_muladd_support_has_ffma |\n"
        "         nir_float_muladd_support_prefers_split;\n"
        "   nir_options.lower_base_vertex =",
        2,
    )
    backend = replace(
        (source / emitter).read_text(),
        "   DXIL_INTR_FMA = 47,",
        "   DXIL_INTR_FMAD = 46,\n   DXIL_INTR_FMA = 47,",
    )
    backend = replace(
        backend,
        "      return emit_tertiary_intin(ctx, alu, DXIL_INTR_FMA, src[0], src[1], src[2]);",
        "      return emit_tertiary_intin(ctx, alu,\n"
        "         alu->def.bit_size == 32 ? DXIL_INTR_FMAD : DXIL_INTR_FMA,\n"
        "         src[0], src[1], src[2]);",
    )
    # Preserve the source-language Fma itself, while generated weak FMAs from
    # matrices and interpolation continue to follow the split preference.
    frontend = replace(
        (source / lowering).read_text(),
        "      b->nb.fp_math_ctrl |= extra_fp_math_ctrl;",
        "      if (entrypoint == GLSLstd450Fma && src[0]->bit_size == 32 &&\n"
        "          (b->shader->options->float_mul_add32 &\n"
        "           nir_float_muladd_support_has_ffma))\n"
        "         op = nir_op_ffma;\n"
        "      b->nb.fp_math_ctrl |= extra_fp_math_ctrl;",
    )
    return {api: header, implementation: code, emitter: backend, lowering: frontend}


def source_state(source):
    digest = hashlib.sha256()
    for base, dirs, files in os.walk(source, followlinks=False):
        dirs[:] = sorted(name for name in dirs if name not in IGNORED)
        for name in dirs:
            path = Path(base) / name
            if path.is_symlink():
                info = path.lstat()
                digest.update(repr((str(path.relative_to(source)),
                                    info.st_mtime_ns, os.readlink(path))).encode())
        for name in sorted(files):
            if name in IGNORED or name.endswith(".pyc"):
                continue
            path = Path(base) / name
            info = path.lstat()
            entry = (str(path.relative_to(source)), info.st_size, info.st_mtime_ns)
            digest.update(repr(entry).encode())
            if path.is_symlink():
                digest.update(os.readlink(path).encode())
    return digest.hexdigest()


_clonefile = None
if sys.platform == "darwin":
    try:
        _clonefile = ctypes.CDLL(None, use_errno=True).clonefile
        _clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32)
        _clonefile.restype = ctypes.c_int
    except AttributeError:
        pass


def copy_source_file(source, dest):
    # APFS clones share data blocks; other filesystems use an ordinary copy.
    if _clonefile is not None:
        if _clonefile(os.fsencode(source), os.fsencode(dest), 0) == 0:
            return dest
    return shutil.copy2(source, dest)


def prepare(source, destination):
    source = source.resolve()
    destination = destination.absolute()
    if destination.is_symlink():
        raise RuntimeError("The generated Mesa overlay must not be a symlink")
    destination = destination.resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise RuntimeError("Mesa checkout and generated overlay must not overlap")
    if not (source / "meson.build").is_file():
        raise RuntimeError("Mesa source tree is missing meson.build")
    revision = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True, text=True,
    ).stdout.strip()
    if len(revision) not in (40, 64) or any(c not in "0123456789abcdef" for c in revision):
        revision = "0" * 40
    script_digest = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    state = {"source": str(source), "source_state": source_state(source),
             "upstream_revision": revision, "script_digest": script_digest}
    stamp = destination / STAMP
    if destination.exists():
        if not stamp.is_file():
            raise RuntimeError("Refusing to replace an unrecognized Mesa overlay directory")
        previous = json.loads(stamp.read_text())
        if all(previous.get(k) == v for k, v in state.items()):
            if (destination / VERSION_HEADER).is_file():
                return previous
    changes = patched_sources(source)
    version = hashlib.sha256()
    version.update(revision.encode())
    version.update(script_digest.encode())
    for name, contents in sorted(changes.items()):
        version.update(name.encode())
        version.update(contents.encode())
    state["overlay_revision"] = version.hexdigest()[:16]
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="mesa-metal-source-", dir=destination.parent))
    try:
        shutil.copytree(source, temporary, dirs_exist_ok=True, symlinks=True,
                        ignore=shutil.ignore_patterns(*IGNORED, "*.pyc"),
                        copy_function=copy_source_file)
        for name, contents in changes.items():
            path = temporary / name
            if path.is_symlink():
                raise RuntimeError("Patched Mesa source must be a regular file: " + name)
            mode = path.stat().st_mode
            path.chmod(mode | stat.S_IWUSR)
            path.write_text(contents)
            path.chmod(mode)
        version_path = temporary / VERSION_HEADER
        if version_path.is_symlink() or version_path.parent.is_symlink():
            raise RuntimeError("Generated Mesa config directory must not be a symlink")
        version_path.parent.mkdir(exist_ok=True)
        version_path.write_text(
            "// Generated by prepare_mesa_metal.py.\n"
            "#define XE_MESA_METAL_OVERLAY_REVISION "
            "0x" + state["overlay_revision"] + "ULL\n")
        (temporary / STAMP).write_text(json.dumps(state, indent=2) + "\n")
        if destination.exists():
            shutil.rmtree(destination)
        temporary.rename(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--run", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    state = prepare(args.source, args.destination)
    if args.run:
        env = os.environ.copy()
        env["MESA_GIT_SHA1_OVERRIDE"] = state["upstream_revision"]
        return subprocess.run(args.run, env=env).returncode
    print(json.dumps(state))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("Mesa Metal overlay: " + str(error), file=sys.stderr)
        sys.exit(1)
