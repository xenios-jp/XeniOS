#!/bin/bash
# Writes the function symbol map the diagnostics Worker uses to symbolicate
# crash frames, keyed by the executable's LC_UUID.
#
#   make_symbol_map.sh XeniOS.app OUT_DIR
#
# Output: OUT_DIR/<UUID>.map.gz, one "hexaddr name" line per text symbol,
# sorted by address, after a "#uuid=... textvmaddr=..." header. Names are
# demangled, without Mach-O's leading underscore, as atos prints them. About
# 370 KB per build. Release builds are not stripped, so this needs no dSYM.
set -euo pipefail

app="$1"
out_dir="$2"
exe="$app/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Info.plist")"
uuid="$(dwarfdump --uuid "$exe" | awk '/arm64/ {print $2; exit}')"
[ -n "$uuid" ] || { echo "no arm64 UUID in $exe" >&2; exit 1; }
textvmaddr="$(otool -l "$exe" | awk '/segname __TEXT$/ {getline; print $2; exit}')"

mkdir -p "$out_dir"
{
  printf '#uuid=%s textvmaddr=%s\n' "$uuid" "$textvmaddr"
  # The name is the rest of the line: Objective-C method names contain spaces.
  nm -n -U "$exe" | awk '$2 ~ /^[tT]$/ {
    name = $0; sub(/^[^ ]+ [^ ]+ /, "", name); sub(/^_/, "", name); print $1, name
  }' | c++filt
} | gzip -9 > "$out_dir/$uuid.map.gz"
echo "$out_dir/$uuid.map.gz"
