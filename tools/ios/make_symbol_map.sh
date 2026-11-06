#!/bin/bash
# Writes the symbol asset the diagnostics Worker symbolicates crash frames
# with, keyed by the executable's LC_UUID, and attached to the build's GitHub
# release.
#
#   make_symbol_map.sh XeniOS.app OUT_DIR [REPO_ROOT]
#
# Output: OUT_DIR/symbols-<UUID>.bin: for any address in the executable the
# frames innermost first, inlined ones included, with repo-relative file:line
# (tools/ios/make_symbol_asset.py has the format). Names are
# demangled as atos prints them. The line tables come from a dSYM that
# dsymutil writes into a temporary directory (the object files must still be
# where the linker left them; with ThinLTO that needs -object_path_lto, see
# the publish workflow); without line tables the asset names functions only.
# REPO_ROOT is the checkout the build ran in (default: this script's repo),
# stripped from file paths.
set -euo pipefail

app="$1"
out_dir="$2"
here="$(cd "$(dirname "$0")" && pwd)"
root="${3:-$(cd "$here/../.." && pwd)}"
exe="$app/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Info.plist")"
uuid="$(dwarfdump --uuid "$exe" | awk '/arm64/ {print $2; exit}')"
[ -n "$uuid" ] || { echo "no arm64 UUID in $exe" >&2; exit 1; }
textvmaddr="$(otool -l "$exe" | awk '/segname __TEXT$/ {getline; print $2; exit}')"

mkdir -p "$out_dir"
work="$(mktemp -d "${TMPDIR:-/tmp}/xsym.XXXXXX")"
trap 'rm -rf "$work"' EXIT

dwarf="$exe"
lines=()
if xcrun dsymutil "$exe" -o "$work/XeniOS.dSYM" >"$work/dsymutil.log" 2>&1 &&
   [ -f "$work/XeniOS.dSYM/Contents/Resources/DWARF/$(basename "$exe")" ]; then
  dwarf="$work/XeniOS.dSYM/Contents/Resources/DWARF/$(basename "$exe")"
  rows="$(xcrun llvm-dwarfdump --debug-line "$dwarf" 2>/dev/null | grep -c '^0x' || true)"
  if [ "${rows:-0}" -gt 0 ]; then
    lines=(--lines)
  fi
else
  echo "dsymutil failed; the asset names functions only:" >&2
  tail -n 5 "$work/dsymutil.log" >&2 || true
fi
[ ${#lines[@]} -gt 0 ] || echo "no line tables in the dSYM; the asset names functions only" >&2

python3 -I "$here/make_symbol_asset.py" --exe "$exe" --dwarf "$dwarf" ${lines[@]+"${lines[@]}"} \
  --uuid "$uuid" --textvmaddr "$textvmaddr" --root "$root" --out "$out_dir/symbols-$uuid.bin"
echo "$out_dir/symbols-$uuid.bin"
