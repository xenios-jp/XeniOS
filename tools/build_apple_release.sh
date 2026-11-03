#!/usr/bin/env bash
set -euo pipefail

# Ported from the public XeniOS repository's tools/build_apple_release.sh
# (tag preview-v2.1-build-9857). Version, build number, channel, stage,
# attestation and iOS bundle identifier are resolved and stamped exactly as
# there. This tree builds iOS with CMake instead of an Xcode project, so the
# iOS step builds a configured CMake tree; the macOS packaging is not ported.

usage() {
  cat <<'USAGE'
Usage: tools/build_apple_release.sh [options]

Builds and packages the iOS arm64 ad-hoc-signed IPA.

Options:
  --out DIR            Output directory (default: scratch/artifacts)
  --config NAME        checked|debug|release|valgrind (default: release)
  --ios-build-dir DIR  Configured iOS CMake build tree (default: build-ios)
  --version VERSION    Marketing version override (defaults to latest vX.Y.Z tag, then 2.1)
  --build-number NUM   Build number override (defaults to git commit count)
  --channel NAME       release|preview (default: release)
  --stage NAME         alpha|beta|rc|stable (default: stable)
  --issued-at ISO8601  Build attestation timestamp override
  --attestation-key VALUE
                       HMAC key used to attest official CI builds
  --attestation-key-file FILE
                       File containing the HMAC key used for attestation
  --stamp-bundle PATH  Stamp version/attestation metadata into an existing .app bundle and exit
  --platform NAME      Platform for --stamp-bundle (ios|macos)
  --ios-bundle-id ID   Bundle identifier stamped into packaged iOS IPAs
                       (default: com.xenios.jp for attested stable releases,
                       random com.xenios.<id> otherwise)
  --random-ios-bundle-id
                       Force a random iOS bundle identifier for this package
  --ios-min VERSION    iOS minimum version (default: 18.0)
  --attestation-key-id ID
                       Optional key id embedded in the attestation payload
  --print-metadata     Print resolved version/build/channel/attestation data and exit
  -h, --help

Notes:
- iOS packaging creates an ad-hoc-signed .ipa suitable for re-signing.
- The iOS build tree must already be configured (cmake -S . -B DIR
  -G "Ninja Multi-Config" -DCMAKE_SYSTEM_NAME=iOS ...); this script builds
  its xenia-app target.
- Marketing version defaults to the latest reachable vX.Y.Z git tag.
- If no matching tag is available, the marketing version falls back to 2.1.
- Public release stage defaults to stable.
- Official build numbers default to git rev-list --count HEAD.
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

need_bin() {
  command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}

need_file_exec() {
  [ -x "$1" ] || die "missing required executable: $1"
}

cap_config() {
  # checked -> Checked, debug -> Debug, release -> Release, valgrind -> Valgrind
  local s
  s="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
  if [ -z "$s" ]; then
    return 0
  fi
  local first rest
  first="$(printf '%s' "$s" | cut -c1 | tr '[:lower:]' '[:upper:]')"
  rest="$(printf '%s' "$s" | cut -c2-)"
  printf '%s%s' "$first" "$rest"
}

repo_root() {
  cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd
}

trim_string() {
  printf '%s' "${1:-}" | tr '\r\n' ' ' | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//'
}

default_marketing_version() {
  printf '%s' "2.1"
}

validate_marketing_version() {
  [[ "${1:-}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]]
}

validate_build_number() {
  [[ "${1:-}" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]]
}

validate_release_stage() {
  case "${1:-}" in
    alpha|beta|rc|stable)
      return 0
      ;;
  esac
  return 1
}

validate_bundle_identifier() {
  [[ "${1:-}" =~ ^[A-Za-z0-9][A-Za-z0-9-]*(\.[A-Za-z0-9][A-Za-z0-9-]*)+$ ]]
}

generate_random_ios_bundle_identifier() {
  printf 'com.xenios.%s' "$(openssl rand -hex 8)"
}

read_latest_marketing_version_from_git() {
  local root="$1"
  command -v git >/dev/null 2>&1 || return 1
  git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 1

  local tag=""
  local patterns=(
    "v[0-9]*.[0-9]*"
    "[0-9]*.[0-9]*"
  )
  local pattern=""
  for pattern in "${patterns[@]}"; do
    tag="$(git -C "$root" describe --tags --abbrev=0 --match "$pattern" 2>/dev/null || true)"
    if [ -n "$tag" ]; then
      break
    fi
  done
  [ -n "$tag" ] || return 1

  tag="$(trim_string "$tag")"
  tag="${tag#v}"
  validate_marketing_version "$tag" || return 1
  printf '%s' "$tag"
}

resolve_release_version() {
  local root="$1"
  local version="${2:-}"
  if [ -n "$version" ]; then
    validate_marketing_version "$version" || die "invalid marketing version: $version"
    printf '%s' "$version"
    return 0
  fi

  if version="$(read_latest_marketing_version_from_git "$root")"; then
    printf '%s' "$version"
    return 0
  fi

  default_marketing_version
}

resolve_release_stage() {
  local stage="${1:-}"
  if [ -n "$stage" ]; then
    stage="$(trim_string "$stage" | tr '[:upper:]' '[:lower:]')"
    validate_release_stage "$stage" || die "invalid release stage: $stage"
    printf '%s' "$stage"
    return 0
  fi

  printf '%s' "stable"
}

resolve_release_build_number() {
  local build_number="${1:-}"
  if [ -n "$build_number" ]; then
    validate_build_number "$build_number" || die "invalid build number: $build_number"
    printf '%s' "$build_number"
    return 0
  fi

  build_number="$(git rev-list --count HEAD)"
  local run_attempt="${GITHUB_RUN_ATTEMPT:-1}"
  if [ -n "$run_attempt" ] && [ "$run_attempt" != "1" ]; then
    build_number="${build_number}.${run_attempt}"
  fi
  validate_build_number "$build_number" || die "invalid derived build number: $build_number"
  printf '%s' "$build_number"
}

find_first_app() {
  local dir="$1"
  local app=""
  app="$(find "$dir" -maxdepth 1 -type d -name "*.app" | LC_ALL=C sort | head -n 1 || true)"
  [ -n "$app" ] || return 1
  printf '%s' "$app"
}

sanitize_attestation_value() {
  printf '%s' "${1:-}" | tr '\r\n' ' ' | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//'
}

sanitize_build_fragment() {
  printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9.-]+/-/g; s/^-+//; s/-+$//'
}

make_build_id() {
  local platform="$1"
  local channel="$2"
  local version="$3"
  local build_number="$4"
  local stage="${5:-}"
  local p c v b s
  p="$(sanitize_build_fragment "$platform")"
  c="$(sanitize_build_fragment "$channel")"
  v="$(sanitize_build_fragment "$version")"
  b="$(sanitize_build_fragment "$build_number")"
  if [ -z "$p" ] || [ -z "$c" ] || [ -z "$v" ] || [ -z "$b" ]; then
    return 1
  fi
  if [ -n "$stage" ] && [ "$stage" != "stable" ]; then
    s="$(sanitize_build_fragment "$stage")"
    [ -n "$s" ] || return 1
    printf '%s-%s-%s-%s-%s' "$p" "$c" "$v" "$b" "$s"
    return 0
  fi
  printf '%s-%s-%s-%s' "$p" "$c" "$v" "$b"
}

bundle_plist_path() {
  local app_bundle="$1"
  if [ -f "$app_bundle/Contents/Info.plist" ]; then
    printf '%s' "$app_bundle/Contents/Info.plist"
    return 0
  fi
  if [ -f "$app_bundle/Info.plist" ]; then
    printf '%s' "$app_bundle/Info.plist"
    return 0
  fi
  return 1
}

plist_delete_key() {
  local plist="$1"
  local key="$2"
  /usr/libexec/PlistBuddy -c "Delete :$key" "$plist" >/dev/null 2>&1 || true
}

plist_has_key() {
  local plist="$1"
  local key="$2"
  /usr/libexec/PlistBuddy -c "Print :$key" "$plist" >/dev/null 2>&1
}

plist_is_readable() {
  local plist="$1"
  [ -f "$plist" ] || return 1
  /usr/libexec/PlistBuddy -c "Print" "$plist" >/dev/null 2>&1
}

print_actool_errors() {
  local plist="$1"
  if plist_is_readable "$plist"; then
    /usr/libexec/PlistBuddy -c "Print :com.apple.actool.errors" "$plist" >&2 || true
  elif [ -s "$plist" ]; then
    cat "$plist" >&2
  fi
}

plist_set_string() {
  local plist="$1"
  local key="$2"
  local value="$3"
  plist_delete_key "$plist" "$key"
  /usr/libexec/PlistBuddy -c "Add :$key string $value" "$plist" >/dev/null
}

plist_set_bool() {
  local plist="$1"
  local key="$2"
  local value="$3"
  plist_delete_key "$plist" "$key"
  /usr/libexec/PlistBuddy -c "Add :$key bool $value" "$plist" >/dev/null
}

plist_merge() {
  local plist="$1"
  local other_plist="$2"
  [ -f "$other_plist" ] || return 0
  /usr/libexec/PlistBuddy -c "Merge $other_plist" "$plist" >/dev/null
}

bundle_resources_path() {
  local app_bundle="$1"
  if [ -d "$app_bundle/Contents/Resources" ]; then
    printf '%s' "$app_bundle/Contents/Resources"
    return 0
  fi
  printf '%s' "$app_bundle"
}

clean_compiled_icon_assets() {
  local resources_dir="$1"
  [ -d "$resources_dir" ] || return 0
  rm -f "$resources_dir/Assets.car"
  rm -f "$resources_dir/AppIcon.icns"
  rm -f "$resources_dir"/AppIcon*.png
}

compile_bundle_icon_assets() {
  local app_bundle="$1"
  local platform="$2"
  local min_version="$3"
  local icon_source="$root/assets/apple/AppIcon.icon"
  local png_icon_source="$root/assets/apple/XeniOSAssets.xcassets"

  [ -d "$icon_source" ] || die "missing Icon Composer source: $icon_source"

  local resources_dir plist partial_plist
  resources_dir="$(bundle_resources_path "$app_bundle")"
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  mkdir -p "$resources_dir"
  clean_compiled_icon_assets "$resources_dir"

  if [ "$platform" = "iphoneos" ]; then
    plist_delete_key "$plist" "CFBundleIcons"
    plist_delete_key "$plist" "CFBundleIcons~ipad"
  else
    plist_delete_key "$plist" "CFBundleIconFile"
    plist_delete_key "$plist" "CFBundleIconName"
  fi

  run_actool_icon_compile() {
    local source="$1"
    local app_icon_name="$2"
    partial_plist="$(mktemp "${TMPDIR:-/tmp}/xenios_icon_plist.XXXXXX")"
    rm -f "$partial_plist"

    local -a actool_cmd=(
      xcrun actool
      --compile "$resources_dir"
      --app-icon "$app_icon_name"
      --platform "$platform"
      --minimum-deployment-target "$min_version"
      --output-partial-info-plist "$partial_plist"
    )

    if [ "$platform" = "iphoneos" ]; then
      actool_cmd+=(--target-device iphone --target-device ipad)
    fi

    actool_cmd+=("$source")
    "${actool_cmd[@]}" >/dev/null
  }

  local icon_compile_ok=1
  if ! run_actool_icon_compile "$icon_source" "AppIcon"; then
    icon_compile_ok=0
  elif ! plist_is_readable "$partial_plist"; then
    icon_compile_ok=0
  elif plist_has_key "$partial_plist" "com.apple.actool.errors"; then
    icon_compile_ok=0
  elif [ "$platform" = "iphoneos" ] && [ ! -f "$resources_dir/Assets.car" ]; then
    icon_compile_ok=0
  fi

  if [ "$icon_compile_ok" -eq 0 ]; then
    if [ "$platform" != "iphoneos" ]; then
      print_actool_errors "$partial_plist"
      rm -f "$partial_plist"
      die "actool failed compiling app icon for $platform"
    fi

    echo "Icon Composer app icon did not produce iOS Assets.car; falling back to PNG app icon catalog." >&2
    rm -f "$partial_plist"
    clean_compiled_icon_assets "$resources_dir"
    [ -d "$png_icon_source" ] || die "missing PNG app icon catalog: $png_icon_source"

    if ! run_actool_icon_compile "$png_icon_source" "AppIcon"; then
      print_actool_errors "$partial_plist"
      rm -f "$partial_plist"
      die "actool failed compiling fallback iOS app icon catalog"
    fi
    if ! plist_is_readable "$partial_plist" ||
        plist_has_key "$partial_plist" "com.apple.actool.errors" ||
        [ ! -f "$resources_dir/Assets.car" ]; then
      print_actool_errors "$partial_plist"
      rm -f "$partial_plist"
      die "fallback iOS app icon catalog did not produce Assets.car"
    fi
  fi

  plist_merge "$plist" "$partial_plist"
  rm -f "$partial_plist"
}

stamp_bundle_version_metadata() {
  local app_bundle="$1"
  local version="$2"
  local build_number="$3"
  [ -n "$version" ] || return 0
  [ -n "$build_number" ] || return 0
  local plist
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  plist_set_string "$plist" "CFBundleShortVersionString" "$version"
  plist_set_string "$plist" "CFBundleVersion" "$build_number"
}

clear_bundle_attestation_metadata() {
  local app_bundle="$1"
  local plist
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  plist_delete_key "$plist" "XeniOSBuildChannel"
  plist_delete_key "$plist" "XeniOSBuildOfficial"
  plist_delete_key "$plist" "XeniOSBuildAttestationPayload"
  plist_delete_key "$plist" "XeniOSBuildAttestationSignature"
}

stamp_bundle_stage_metadata() {
  local app_bundle="$1"
  local stage="$2"
  local plist
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  plist_delete_key "$plist" "XeniOSBuildStage"
  [ -n "$stage" ] || return 0
  plist_set_string "$plist" "XeniOSBuildStage" "$stage"
}

build_attestation_payload() {
  local platform="$1"
  local channel="$2"
  local build_id="$3"
  local version="$4"
  local build_number="$5"
  local stage="$6"
  local commit_short="$7"
  local issued_at="$8"
  local key_id="$9"
  printf '%s' \
    "xenios-build-attestation-v1;platform=$(sanitize_attestation_value "$platform");channel=$(sanitize_attestation_value "$channel");buildId=$(sanitize_attestation_value "$build_id");appVersion=$(sanitize_attestation_value "$version");buildNumber=$(sanitize_attestation_value "$build_number");stage=$(sanitize_attestation_value "$stage");commitShort=$(sanitize_attestation_value "$commit_short");issuedAt=$(sanitize_attestation_value "$issued_at");keyId=$(sanitize_attestation_value "$key_id")"
}

sign_attestation_payload() {
  local payload="$1"
  local key="$2"
  printf '%s' "$payload" | \
    openssl dgst -sha256 -mac HMAC -macopt "key:$key" -binary | \
    openssl base64 -A | tr '+/' '-_' | tr -d '='
}

stamp_bundle_attestation() {
  local app_bundle="$1"
  local platform="$2"
  local channel="$3"
  local version="$4"
  local build_number="$5"
  local stage="$6"
  local commit_short="$7"
  local issued_at="$8"
  local key_id="$9"
  local attestation_key="${10}"

  clear_bundle_attestation_metadata "$app_bundle"

  if [ -z "$attestation_key" ]; then
    return 0
  fi

  local build_id
  build_id="$(make_build_id "$platform" "$channel" "$version" "$build_number" "$stage")" || \
    die "official build attestation requires version and build number"
  local payload
  payload="$(build_attestation_payload "$platform" "$channel" "$build_id" "$version" \
    "$build_number" "$stage" "$commit_short" "$issued_at" "$key_id")"
  local signature
  signature="$(sign_attestation_payload "$payload" "$attestation_key")"

  local plist
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  plist_set_string "$plist" "XeniOSBuildChannel" "$channel"
  plist_set_bool "$plist" "XeniOSBuildOfficial" true
  plist_set_string "$plist" "XeniOSBuildAttestationPayload" "$payload"
  plist_set_string "$plist" "XeniOSBuildAttestationSignature" "$signature"
}

package_ios_ipa() {
  local app_bundle="$1"
  local ipa_out="$2"
  local bundle_identifier="$3"

  local ipa_dir ipa_base
  ipa_dir="$(cd "$(dirname "$ipa_out")" && pwd)"
  ipa_base="$(basename "$ipa_out")"
  ipa_out="$ipa_dir/$ipa_base"

  /bin/bash "$root/tools/package_ios_ipa.sh" \
    --bundle-id "$bundle_identifier" \
    "$app_bundle" \
    "$root/xenia_ios.entitlements" \
    "$ipa_out" \
    "$ipa_dir/XeniOS.requested-entitlements.plist" \
    "$ipa_dir/XeniOS.signed-entitlements.plist"
}

print_metadata_summary() {
  local channel="$1"
  local stage="$2"
  local version="$3"
  local build_number="$4"
  local commit_short="$5"
  local issued_at="$6"
  local key_id="$7"
  local attestation_key="$8"

  local ios_build_id macos_build_id
  ios_build_id="$(make_build_id "ios" "$channel" "$version" "$build_number" "$stage" || true)"
  macos_build_id="$(make_build_id "macos" "$channel" "$version" "$build_number" "$stage" || true)"
  local version_build_label="$version"
  if [ -n "$build_number" ]; then
    version_build_label="${version}-${build_number}"
  fi

  echo "channel=$channel"
  echo "stage=$stage"
  echo "version=$version"
  echo "build_number=$build_number"
  echo "commit_short=$commit_short"
  echo "issued_at=$issued_at"
  if [ "$stage" = "stable" ]; then
    if [ "$channel" = "preview" ]; then
      echo "display_label=Preview $version_build_label"
    else
      echo "display_label=$version_build_label"
    fi
  else
    local stage_title
    case "$stage" in
      rc)
        stage_title="RC"
        ;;
      *)
        stage_title="$(printf '%s' "$stage" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')"
        ;;
    esac
    if [ "$channel" = "preview" ]; then
      echo "display_label=$stage_title Preview $version_build_label"
    else
      echo "display_label=$stage_title $version_build_label"
    fi
  fi
  echo "ios_build_id=$ios_build_id"
  echo "macos_build_id=$macos_build_id"
  if [ -n "$attestation_key" ]; then
    local ios_payload ios_signature
    ios_payload="$(build_attestation_payload "ios" "$channel" "$ios_build_id" "$version" \
      "$build_number" "$stage" "$commit_short" "$issued_at" "$key_id")"
    ios_signature="$(sign_attestation_payload "$ios_payload" "$attestation_key")"
    echo "attestation=enabled"
    echo "attestation_key_id=$key_id"
    echo "ios_attestation_payload=$ios_payload"
    echo "ios_attestation_signature=$ios_signature"
  else
    echo "attestation=disabled"
  fi
}


print_metadata_only=0
out_dir="scratch/artifacts"
config="release"
ios_build_dir="${XENIOS_IOS_BUILD_DIR:-build-ios}"
release_version="${XENIOS_BUILD_VERSION:-}"
release_build_number="${XENIOS_BUILD_NUMBER:-}"
build_channel="${XENIOS_BUILD_CHANNEL:-release}"
release_stage="${XENIOS_BUILD_STAGE:-}"
issued_at_override="${XENIOS_BUILD_ISSUED_AT:-}"
stamp_bundle=""
stamp_platform=""
ios_min="18.0"
attestation_key="${XENIOS_BUILD_ATTESTATION_KEY:-}"
attestation_key_id="${XENIOS_BUILD_ATTESTATION_KEY_ID:-ci-hmac-v1}"
ios_bundle_id="${XENIOS_IOS_BUNDLE_ID:-}"
force_random_ios_bundle_id=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out)
      out_dir="${2:-}"; shift 2;;
    --config)
      config="${2:-}"; shift 2;;
    --ios-build-dir)
      ios_build_dir="${2:-}"; shift 2;;
    --version)
      release_version="${2:-}"; shift 2;;
    --build-number)
      release_build_number="${2:-}"; shift 2;;
    --channel)
      build_channel="${2:-}"; shift 2;;
    --stage)
      release_stage="${2:-}"; shift 2;;
    --issued-at)
      issued_at_override="${2:-}"; shift 2;;
    --attestation-key)
      attestation_key="${2:-}"; shift 2;;
    --attestation-key-file)
      [ -n "${2:-}" ] || die "missing value for --attestation-key-file"
      attestation_key="$(tr -d '\r\n' < "${2}")"; shift 2;;
    --stamp-bundle)
      stamp_bundle="${2:-}"; shift 2;;
    --platform)
      stamp_platform="${2:-}"; shift 2;;
    --ios-bundle-id)
      ios_bundle_id="${2:-}"; shift 2;;
    --random-ios-bundle-id)
      force_random_ios_bundle_id=1; shift;;
    --attestation-key-id)
      attestation_key_id="${2:-}"; shift 2;;
    --print-metadata)
      print_metadata_only=1; shift;;
    --ios-min)
      ios_min="${2:-}"; shift 2;;
    -h|--help)
      usage; exit 0;;
    *)
      die "unknown argument: $1";;
  esac
done

root="$(repo_root)"
cd "$root"

need_bin openssl

case "$build_channel" in
  release|preview)
    ;;
  *)
    die "build channel must be release or preview"
    ;;
esac

release_version="$(resolve_release_version "$root" "$release_version")"
release_stage="$(resolve_release_stage "$release_stage")"
release_build_number="$(resolve_release_build_number "$release_build_number")"

if [ -n "$ios_bundle_id" ] && [ "$force_random_ios_bundle_id" -eq 1 ]; then
  die "--ios-bundle-id and --random-ios-bundle-id cannot be used together"
fi
if [ "$force_random_ios_bundle_id" -eq 1 ]; then
  ios_bundle_id="$(generate_random_ios_bundle_identifier)"
elif [ -z "$ios_bundle_id" ]; then
  if [ "$build_channel" = "release" ] && [ "$release_stage" = "stable" ] &&
      [ -n "$attestation_key" ]; then
    ios_bundle_id="com.xenios.jp"
  else
    ios_bundle_id="$(generate_random_ios_bundle_identifier)"
  fi
fi
validate_bundle_identifier "$ios_bundle_id" || die "invalid iOS bundle identifier: $ios_bundle_id"

if [ -n "$attestation_key" ]; then
  [ -n "$release_version" ] || die "official attestation requires a marketing version (tag the repo or pass --version)"
  [ -n "$release_build_number" ] || die "official attestation requires a build number"
fi

buildcfg="$(cap_config "$config")"
commit_short="$(git rev-parse --short=9 HEAD)"
issued_at="${issued_at_override:-$(date -u +"%Y-%m-%dT%H:%M:%SZ")}"

if [ "$print_metadata_only" -eq 1 ]; then
  print_metadata_summary "$build_channel" "$release_stage" "$release_version" \
    "$release_build_number" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"
  exit 0
fi

if [ -n "$stamp_bundle" ]; then
  case "$stamp_platform" in
    ios|macos)
      ;;
    *)
      die "--platform must be ios or macos when using --stamp-bundle"
      ;;
  esac

  need_file_exec /usr/libexec/PlistBuddy
  stamp_bundle_version_metadata "$stamp_bundle" "$release_version" "$release_build_number"
  stamp_bundle_stage_metadata "$stamp_bundle" "$release_stage"
  stamp_bundle_attestation "$stamp_bundle" "$stamp_platform" "$build_channel" "$release_version" \
    "$release_build_number" "$release_stage" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"
  exit 0
fi

if [ "$(uname -s)" != "Darwin" ]; then
  die "this script must be run on macOS"
fi

need_bin cmake
need_bin xcrun
need_bin codesign
need_bin ditto
need_file_exec /usr/libexec/PlistBuddy

[ -f "$ios_build_dir/CMakeCache.txt" ] || \
  die "no configured iOS CMake build tree in $ios_build_dir (configure one with -DCMAKE_SYSTEM_NAME=iOS, or pass --ios-build-dir)"

mkdir -p "$out_dir"

echo "Config: $buildcfg"
echo "Output: $out_dir"
if [ -n "$release_version" ] && [ -n "$release_build_number" ]; then
  echo "Bundle version: $release_version ($release_build_number)"
fi
echo "Build channel: $build_channel"
echo "Release stage: $release_stage"
echo "iOS min: $ios_min"
echo "iOS bundle ID: $ios_bundle_id"
if [ -n "$attestation_key" ]; then
  echo "Attestation: enabled (${attestation_key_id})"
else
  echo "Attestation: disabled"
fi

echo ""
echo "== iOS arm64 (CMake-built, ad-hoc-signed ipa) =="

cmake --build "$ios_build_dir" --config "$buildcfg" --target xenia-app

ios_dir="$ios_build_dir/bin/iOS/$buildcfg"
app_bundle="$(find_first_app "$ios_dir")" || die "iOS app not found in $ios_dir"

stamp_bundle_version_metadata "$app_bundle" "$release_version" "$release_build_number"
stamp_bundle_stage_metadata "$app_bundle" "$release_stage"
compile_bundle_icon_assets "$app_bundle" "iphoneos" "$ios_min"
stamp_bundle_attestation "$app_bundle" "ios" "$build_channel" "$release_version" \
  "$release_build_number" "$release_stage" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"
# Ad-hoc sign to embed entitlements (increased-memory-limit).
# Re-signing tools will preserve these when applying a real identity.
codesign --force --sign - --entitlements "$root/xenia_ios.entitlements" "$app_bundle"

package_ios_ipa "$app_bundle" "$out_dir/xenios_ios_iphone_ipad.ipa" "$ios_bundle_id"

echo ""
echo "Done. Artifacts:"
find "$out_dir" -maxdepth 1 -type f -name "*.ipa" -print
