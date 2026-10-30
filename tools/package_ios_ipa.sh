#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/package_ios_ipa.sh [options] APP_BUNDLE REQUESTED_ENTITLEMENTS IPA_OUT [REQUESTED_OUT] [SIGNED_OUT]

Packages an iOS .app bundle as an IPA by copying it into Payload, ad-hoc
signing the copied app with REQUESTED_ENTITLEMENTS, and emitting entitlement
helper files:
- REQUESTED_OUT: the entitlements plist the build asks codesign/Xcode to use.
- SIGNED_OUT: the entitlements extracted from the copied Payload app signature.

The IPA is still only installable on stock iOS after a real signing tool
re-signs it with a provisioning profile that permits the requested entitlements.
The source .app bundle is not modified.

Options:
  --bundle-id ID      Override CFBundleIdentifier in the copied Payload app
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

validate_bundle_identifier() {
  [[ "${1:-}" =~ ^[A-Za-z0-9][A-Za-z0-9-]*(\.[A-Za-z0-9][A-Za-z0-9-]*)+$ ]]
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

bundle_identifier=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --bundle-id)
      bundle_identifier="${2:-}"
      [ -n "$bundle_identifier" ] || die "missing value for --bundle-id"
      validate_bundle_identifier "$bundle_identifier" || \
        die "invalid bundle identifier: $bundle_identifier"
      shift 2
      ;;
    --)
      shift
      break
      ;;
    -*)
      die "unknown argument: $1"
      ;;
    *)
      break
      ;;
  esac
done

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
  usage >&2
  exit 1
fi

app_bundle="$1"
requested_entitlements="$2"
ipa_out="$3"
requested_out="${4:-}"
signed_out="${5:-}"

[ -d "$app_bundle" ] || die "app bundle not found: $app_bundle"
[ -f "$requested_entitlements" ] || die "requested entitlements not found: $requested_entitlements"
command -v ditto >/dev/null 2>&1 || die "missing required tool: ditto"
command -v codesign >/dev/null 2>&1 || die "missing required tool: codesign"
command -v /usr/libexec/PlistBuddy >/dev/null 2>&1 || die "missing required tool: PlistBuddy"

ipa_dir="$(cd "$(dirname "$ipa_out")" && pwd)"
ipa_base="$(basename "$ipa_out")"
ipa_out="$ipa_dir/$ipa_base"
mkdir -p "$ipa_dir"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/xenios_ipa.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

if [ -n "$requested_out" ]; then
  requested_dir="$(dirname "$requested_out")"
  mkdir -p "$requested_dir"
  cp -f "$requested_entitlements" "$requested_out"
fi

payload_app="$tmp/Payload/$(basename "$app_bundle")"
mkdir -p "$tmp/Payload"
ditto "$app_bundle" "$payload_app"
if [ -n "$bundle_identifier" ]; then
  /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $bundle_identifier" \
    "$payload_app/Info.plist"
fi
codesign --force --deep --sign - --entitlements "$requested_entitlements" "$payload_app"

if [ -n "$signed_out" ]; then
  signed_dir="$(dirname "$signed_out")"
  mkdir -p "$signed_dir"
  if ! codesign -d --entitlements :- "$payload_app" >"$signed_out.tmp" 2>/dev/null; then
    rm -f "$signed_out.tmp"
    die "failed to extract signed entitlements from $payload_app"
  fi
  mv -f "$signed_out.tmp" "$signed_out"
fi

rm -f "$ipa_out"
(cd "$tmp" && ditto -c -k --sequesterRsrc --keepParent "Payload" "$ipa_out")

echo "IPA: $ipa_out"
if [ -n "$bundle_identifier" ]; then
  echo "Bundle identifier: $bundle_identifier"
fi
if [ -n "$requested_out" ]; then
  echo "Requested entitlements: $requested_out"
fi
if [ -n "$signed_out" ]; then
  echo "Signed entitlements: $signed_out"
fi
