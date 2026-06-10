#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/build_apple_release.sh [options]

Builds and packages Apple release artifacts:
- macOS arm64 DMG
- macOS x86_64 DMG
- macOS universal DMG when both macOS architecture bundles are available
- iOS arm64 ad-hoc-signed IPA

Options:
  --out DIR            Output directory (default: scratch/artifacts)
  --config NAME        checked|debug|release|valgrind (default: release)
  --version VERSION    Marketing version override (defaults to latest vX.Y.Z tag, then 2.0.1)
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
  --macos-min VERSION  macOS minimum version (default: 15.0)
  --mac-sign IDENTITY  macOS codesign identity (default: ad-hoc '-')
  --attestation-key-id ID
                       Optional key id embedded in the attestation payload
  --print-metadata     Print resolved version/build/channel/attestation data and exit
  --package-macos-universal ARM64_APP X86_64_APP
                       Package a universal macOS DMG from two prepared .app bundles
  --skip-ios
  --skip-macos-arm64
  --skip-macos-x86_64
  --skip-macos-universal
  -h, --help

Notes:
- iOS packaging creates an ad-hoc-signed .ipa suitable for re-signing.
- This script expects Xcode command line tools (xcodebuild, codesign, hdiutil).
- Marketing version defaults to the latest reachable vX.Y.Z git tag.
- If no matching tag is available, the marketing version falls back to 2.0.1.
- Public release stage defaults to stable.
- Official build numbers default to git rev-list --count HEAD.
EOF
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

has_bundled_patch_files() {
  local patches_dir="$1"
  local first_patch
  [ -d "$patches_dir" ] || return 1
  first_patch="$(find "$patches_dir" -type f -name "*.patch.toml" -print -quit 2>/dev/null || true)"
  [ -n "$first_patch" ]
}

release_data_repos_ready() {
  local data_dir="$root/build/data_repos"
  has_bundled_patch_files "$data_dir/game-patches/patches" || return 1
  [ -f "$data_dir/xenia-manager-database/data/game-compatibility/canary.json" ] || return 1
  [ -f "$data_dir/xenia-manager-database/data/game-compatibility/stable.json" ] || return 1
  [ -f "$data_dir/SDL_GameControllerDB/gamecontrollerdb.txt" ] || return 1
  return 0
}

ensure_release_data_repos() {
  if release_data_repos_ready; then
    echo "Data repos: ready"
    return 0
  fi

  echo "Data repos: missing or incomplete; running ./xb fetchdata"
  ./xb fetchdata
  release_data_repos_ready || die "data repos still incomplete after ./xb fetchdata"
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
  printf '%s' "2.0.1"
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
    "v[0-9]*.[0-9]*.[0-9]*"
    "[0-9]*.[0-9]*.[0-9]*"
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

bundle_executable_path() {
  local app_bundle="$1"
  local plist
  plist="$(bundle_plist_path "$app_bundle")" || return 1
  local executable_name
  executable_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist" 2>/dev/null || true)"
  [ -n "$executable_name" ] || return 1
  if [ -d "$app_bundle/Contents/MacOS" ]; then
    printf '%s/%s' "$app_bundle/Contents/MacOS" "$executable_name"
    return 0
  fi
  printf '%s/%s' "$app_bundle" "$executable_name"
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
  local icon_catalog="$root/assets/apple/AppIcon.xcassets"

  [ -d "$icon_catalog" ] || die "missing icon catalog: $icon_catalog"

  local resources_dir plist partial_plist
  resources_dir="$(bundle_resources_path "$app_bundle")"
  plist="$(bundle_plist_path "$app_bundle")" || die "Info.plist not found in $app_bundle"
  mkdir -p "$resources_dir"
  clean_compiled_icon_assets "$resources_dir"

  partial_plist="$(mktemp "${TMPDIR:-/tmp}/xenios_icon_plist.XXXXXX")"
  rm -f "$partial_plist"

  local -a actool_cmd=(
    xcrun actool
    --compile "$resources_dir"
    --app-icon AppIcon
    --platform "$platform"
    --minimum-deployment-target "$min_version"
    --output-partial-info-plist "$partial_plist"
  )

  if [ "$platform" = "iphoneos" ]; then
    actool_cmd+=(--target-device iphone --target-device ipad)
    plist_delete_key "$plist" "CFBundleIcons"
    plist_delete_key "$plist" "CFBundleIcons~ipad"
  else
    plist_delete_key "$plist" "CFBundleIconFile"
    plist_delete_key "$plist" "CFBundleIconName"
  fi

  actool_cmd+=("$icon_catalog")
  "${actool_cmd[@]}" >/dev/null
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

prepare_macos_runtime_dylibs() {
  local app_bundle="$1"
  local dxilconv_dylib="$2"
  local frameworks_dir="$app_bundle/Contents/Frameworks"

  rm -rf "$frameworks_dir" "$app_bundle/Contents/PlugIns" \
    "$app_bundle/Contents/Resources/qt.conf"
  mkdir -p "$frameworks_dir"

  if [ -f third_party/metal-shader-converter/lib/libmetalirconverter.dylib ]; then
    cp -f third_party/metal-shader-converter/lib/libmetalirconverter.dylib \
      "$frameworks_dir/" || true
  fi
  if [ -f "$dxilconv_dylib" ]; then
    cp -f "$dxilconv_dylib" "$frameworks_dir/" || true
  fi
}

sign_macos_app() {
  local app_bundle="$1"
  local entitlements="$2"
  local identity="${3:--}"

  local main_exe=""
  main_exe="$(bundle_executable_path "$app_bundle" || true)"
  local frameworks_dir="$app_bundle/Contents/Frameworks"
  local plugins_dir="$app_bundle/Contents/PlugIns"

  if [ -f "$main_exe" ]; then
    codesign --remove-signature "$main_exe" >/dev/null 2>&1 || true
  fi
  codesign --remove-signature "$app_bundle" >/dev/null 2>&1 || true

  if [ -d "$frameworks_dir" ]; then
    find "$frameworks_dir" -maxdepth 2 -name "*.framework" -type d -print0 | \
      xargs -0 -I{} codesign --remove-signature "{}" >/dev/null 2>&1 || true
    find "$frameworks_dir" -name "*.dylib" -type f -print0 | \
      xargs -0 -I{} codesign --remove-signature "{}" >/dev/null 2>&1 || true

    find "$frameworks_dir" -maxdepth 2 -name "*.framework" -type d -print0 | \
      xargs -0 -I{} codesign --force --sign "$identity" --timestamp=none "{}"
    find "$frameworks_dir" -name "*.dylib" -type f -print0 | \
      xargs -0 -I{} codesign --force --sign "$identity" --timestamp=none "{}"
  fi

  if [ -d "$plugins_dir" ]; then
    find "$plugins_dir" -name "*.dylib" -type f -print0 | \
      xargs -0 -I{} codesign --remove-signature "{}" >/dev/null 2>&1 || true
    find "$plugins_dir" -name "*.dylib" -type f -print0 | \
      xargs -0 -I{} codesign --force --sign "$identity" --timestamp=none "{}"
  fi

  if [ -f "$main_exe" ]; then
    codesign --force --sign "$identity" --timestamp=none "$main_exe"
  fi

  codesign --force --deep --timestamp=none --sign "$identity" \
    --entitlements "$entitlements" "$app_bundle"

  # Some Homebrew dylibs can be copied with read-only mode and carry provenance
  # xattrs, making recursive xattr clearing fail with EPERM.
  if ! xattr -cr "$app_bundle" >/dev/null 2>&1; then
    chmod -R u+w "$app_bundle" >/dev/null 2>&1 || true
    if ! xattr -cr "$app_bundle" >/dev/null 2>&1; then
      echo "warning: failed to clear one or more extended attributes in $app_bundle"
    fi
  fi
}

package_macos_dmg() {
  local app_bundle="$1"
  local dmg_out="$2"
  local license_file="$3"

  rm -f "$dmg_out"
  local dmg_contents
  dmg_contents="$(mktemp -d "${TMPDIR:-/tmp}/xenia_dmg.XXXXXX")"
  mkdir -p "$dmg_contents"
  cp -R "$app_bundle" "$dmg_contents/"
  cp -f "$license_file" "$dmg_contents/" || true
  ln -s /Applications "$dmg_contents/Applications"
  hdiutil create -volname "XeniOS" -srcfolder "$dmg_contents" -ov -format UDZO "$dmg_out"
  rm -rf "$dmg_contents"
}

is_macho_file() {
  local path="$1"
  [ -f "$path" ] || return 1
  file -b "$path" 2>/dev/null | grep -q "Mach-O"
}

should_skip_universal_compare() {
  local rel="$1"
  case "$rel" in
    Contents/_CodeSignature/*|_CodeSignature/*)
      return 0
      ;;
  esac
  return 1
}

verify_universal_bundle_inputs() {
  local arm64_bundle="$1"
  local x86_64_bundle="$2"
  local path rel other
  local warned_resource_difference=0

  while IFS= read -r -d '' path; do
    rel="${path#"$arm64_bundle/"}"
    should_skip_universal_compare "$rel" && continue
    other="$x86_64_bundle/$rel"
    [ -e "$other" ] || die "universal bundle missing x86_64 counterpart: $rel"
    if is_macho_file "$path"; then
      is_macho_file "$other" || die "universal bundle type mismatch for $rel"
      continue
    fi
    if ! cmp -s "$path" "$other" && [ "$warned_resource_difference" -eq 0 ]; then
      echo "warning: universal bundle resources differ outside Mach-O payloads; keeping arm64 resources"
      warned_resource_difference=1
    fi
  done < <(find "$arm64_bundle" -type f -print0)

  while IFS= read -r -d '' path; do
    rel="${path#"$x86_64_bundle/"}"
    should_skip_universal_compare "$rel" && continue
    [ -e "$arm64_bundle/$rel" ] || die "universal bundle missing arm64 counterpart: $rel"
    if is_macho_file "$path" && ! is_macho_file "$arm64_bundle/$rel"; then
      die "universal bundle type mismatch for $rel"
    fi
  done < <(find "$x86_64_bundle" -type f -print0)
}

thin_macho_slice_if_needed() {
  local path="$1"
  local arch="$2"
  local info
  info="$(lipo -info "$path" 2>/dev/null || true)"
  lipo "$path" -verify_arch "$arch" >/dev/null || \
    die "Mach-O payload does not contain $arch slice: $path"
  if [[ "$info" == Non-fat* ]]; then
    printf '%s' "$path"
    return 0
  fi

  local tmp
  tmp="$(mktemp "${TMPDIR:-/tmp}/xenios_lipo.XXXXXX")"
  rm -f "$tmp"
  lipo "$path" -thin "$arch" -output "$tmp"
  printf '%s' "$tmp"
}

merge_macho_slices() {
  local arm64_path="$1"
  local x86_64_path="$2"
  local info
  info="$(lipo -info "$arm64_path" 2>/dev/null || true)"
  if [[ "$info" == *"arm64"* && "$info" == *"x86_64"* ]]; then
    return 0
  fi

  local arm64_input x86_64_input output
  arm64_input="$(thin_macho_slice_if_needed "$arm64_path" arm64)"
  x86_64_input="$(thin_macho_slice_if_needed "$x86_64_path" x86_64)"
  output="$(mktemp "${TMPDIR:-/tmp}/xenios_lipo_out.XXXXXX")"
  rm -f "$output"
  lipo -create "$arm64_input" "$x86_64_input" -output "$output"
  mv "$output" "$arm64_path"

  if [ "$arm64_input" != "$arm64_path" ]; then
    rm -f "$arm64_input"
  fi
  if [ "$x86_64_input" != "$x86_64_path" ]; then
    rm -f "$x86_64_input"
  fi
}

merge_macos_bundle_macho_payloads() {
  local universal_bundle="$1"
  local x86_64_bundle="$2"
  local path rel other

  while IFS= read -r -d '' path; do
    rel="${path#"$universal_bundle/"}"
    should_skip_universal_compare "$rel" && continue
    if ! is_macho_file "$path"; then
      continue
    fi
    other="$x86_64_bundle/$rel"
    merge_macho_slices "$path" "$other"
  done < <(find "$universal_bundle" -type f -print0)
}

package_macos_universal_dmg() {
  local arm64_app_bundle="$1"
  local x86_64_app_bundle="$2"
  local dmg_out="$3"
  local license_file="$4"
  local identity="${5:--}"

  [ -d "$arm64_app_bundle" ] || die "missing arm64 app bundle: $arm64_app_bundle"
  [ -d "$x86_64_app_bundle" ] || die "missing x86_64 app bundle: $x86_64_app_bundle"

  local tmp universal_bundle main_exe info
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/xenios_universal.XXXXXX")"
  cp -R "$arm64_app_bundle" "$tmp/"
  universal_bundle="$(find_first_app "$tmp")" || die "universal app not found after copy"

  verify_universal_bundle_inputs "$universal_bundle" "$x86_64_app_bundle"
  merge_macos_bundle_macho_payloads "$universal_bundle" "$x86_64_app_bundle"

  main_exe="$(bundle_executable_path "$universal_bundle")" || \
    die "unable to locate universal bundle executable"
  info="$(lipo -info "$main_exe")"
  echo "$info"
  if ! echo "$info" | grep -q "arm64" || ! echo "$info" | grep -q "x86_64"; then
    die "universal binary missing an architecture: $info"
  fi

  sign_macos_app "$universal_bundle" "xenia.entitlements" "$identity"
  package_macos_dmg "$universal_bundle" "$dmg_out" "$license_file"
  rm -rf "$tmp"
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

build_dir_for_target_arch() {
  local target_arch="$1"
  local host_arch
  host_arch="$(uname -m)"

  case "$target_arch" in
    arm64)
      if [ "$host_arch" = "arm64" ]; then
        printf '%s' "build"
      else
        printf '%s' "build-arm64"
      fi
      ;;
    x64|x86_64)
      if [ "$host_arch" = "arm64" ]; then
        printf '%s' "build-x64"
      else
        printf '%s' "build"
      fi
      ;;
    *)
      die "unsupported target arch: $target_arch"
      ;;
  esac
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

build_ios=1
build_macos_arm64=1
build_macos_x86_64=1
build_macos_universal=1
print_metadata_only=0
out_dir="scratch/artifacts"
config="release"
release_version="${XENIOS_BUILD_VERSION:-}"
release_build_number="${XENIOS_BUILD_NUMBER:-}"
build_channel="${XENIOS_BUILD_CHANNEL:-release}"
release_stage="${XENIOS_BUILD_STAGE:-}"
issued_at_override="${XENIOS_BUILD_ISSUED_AT:-}"
stamp_bundle=""
stamp_platform=""
ios_min="18.0"
macos_min="15.0"
mac_sign_identity="-"
attestation_key="${XENIOS_BUILD_ATTESTATION_KEY:-}"
attestation_key_id="${XENIOS_BUILD_ATTESTATION_KEY_ID:-ci-hmac-v1}"
ios_bundle_id="${XENIOS_IOS_BUNDLE_ID:-}"
force_random_ios_bundle_id=0
package_macos_universal_arm64_app=""
package_macos_universal_x86_64_app=""

while [ $# -gt 0 ]; do
  case "$1" in
    --out)
      out_dir="${2:-}"; shift 2;;
    --config)
      config="${2:-}"; shift 2;;
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
    --package-macos-universal)
      [ $# -ge 3 ] || die "missing app bundle arguments for --package-macos-universal"
      package_macos_universal_arm64_app="${2:-}"
      package_macos_universal_x86_64_app="${3:-}"
      shift 3;;
    --ios-min)
      ios_min="${2:-}"; shift 2;;
    --macos-min)
      macos_min="${2:-}"; shift 2;;
    --mac-sign)
      mac_sign_identity="${2:-}"; shift 2;;
    --skip-ios)
      build_ios=0; shift;;
    --skip-macos-arm64)
      build_macos_arm64=0; shift;;
    --skip-macos-x86_64)
      build_macos_x86_64=0; shift;;
    --skip-macos-universal)
      build_macos_universal=0; shift;;
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

mkdir -p "$out_dir"

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

need_bin xcodebuild
need_bin xcrun
need_bin codesign
need_bin hdiutil
need_bin ditto
need_bin xattr
need_bin otool
need_bin file
need_bin lipo
need_file_exec /usr/libexec/PlistBuddy

if [ -n "$package_macos_universal_arm64_app" ] || [ -n "$package_macos_universal_x86_64_app" ]; then
  [ -n "$package_macos_universal_arm64_app" ] || die "missing arm64 app bundle for universal package"
  [ -n "$package_macos_universal_x86_64_app" ] || die "missing x86_64 app bundle for universal package"
  echo "== macOS universal =="
  package_macos_universal_dmg \
    "$package_macos_universal_arm64_app" \
    "$package_macos_universal_x86_64_app" \
    "$out_dir/xenios_macos_universal.dmg" \
    "LICENSE" \
    "$mac_sign_identity"
  echo ""
  echo "Done. Artifacts:"
  find "$out_dir" -maxdepth 1 -type f -name "*.dmg" -print
  exit 0
fi

if [ "$build_ios" -eq 1 ] || [ "$build_macos_arm64" -eq 1 ] || [ "$build_macos_x86_64" -eq 1 ]; then
  ensure_release_data_repos
fi

echo "Config: $buildcfg"
echo "Output: $out_dir"
if [ -n "$release_version" ] && [ -n "$release_build_number" ]; then
  echo "Bundle version: $release_version ($release_build_number)"
fi
echo "Build channel: $build_channel"
echo "Release stage: $release_stage"
echo "macOS min: $macos_min"
echo "iOS min: $ios_min"
if [ "$build_ios" -eq 1 ]; then
  echo "iOS bundle ID: $ios_bundle_id"
fi
echo "macOS signing: $mac_sign_identity"
if [ -n "$attestation_key" ]; then
  echo "Attestation: enabled (${attestation_key_id})"
else
  echo "Attestation: disabled"
fi

macos_arm64_app_bundle=""
macos_x86_64_app_bundle=""

if [ "$build_macos_arm64" -eq 1 ]; then
  echo ""
  echo "== macOS arm64 =="
  ./xb build --config="$config" --target-arch=arm64 --target=xenia-app

  mac_dir="$(build_dir_for_target_arch arm64)/bin/macOS/$buildcfg"
  app_bundle="$(find_first_app "$mac_dir")" || die "macOS arm64 app not found in $mac_dir"

  prepare_macos_runtime_dylibs "$app_bundle" \
    "third_party/DirectXShaderCompiler/build_dxilconv_macos/lib/libdxilconv.dylib"

  stamp_bundle_version_metadata "$app_bundle" "$release_version" "$release_build_number"
  stamp_bundle_stage_metadata "$app_bundle" "$release_stage"
  compile_bundle_icon_assets "$app_bundle" "macosx" "$macos_min"
  stamp_bundle_attestation "$app_bundle" "macos" "$build_channel" "$release_version" \
    "$release_build_number" "$release_stage" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"

  sign_macos_app "$app_bundle" "xenia.entitlements" "$mac_sign_identity"
  macos_arm64_app_bundle="$app_bundle"
  package_macos_dmg "$app_bundle" "$out_dir/xenios_macos_apple_silicon.dmg" "LICENSE"
fi

if [ "$build_macos_x86_64" -eq 1 ]; then
  echo ""
  echo "== macOS x86_64 =="
  ./xb build --config="$config" --target-arch=x64 --target=xenia-app

  mac_dir="$(build_dir_for_target_arch x64)/bin/macOS/$buildcfg"
  app_bundle="$(find_first_app "$mac_dir")" || die "macOS x86_64 app not found in $mac_dir"

  prepare_macos_runtime_dylibs "$app_bundle" \
    "third_party/DirectXShaderCompiler/build_dxilconv_macos_x86_64/lib/libdxilconv.dylib"

  stamp_bundle_version_metadata "$app_bundle" "$release_version" "$release_build_number"
  stamp_bundle_stage_metadata "$app_bundle" "$release_stage"
  compile_bundle_icon_assets "$app_bundle" "macosx" "$macos_min"
  stamp_bundle_attestation "$app_bundle" "macos" "$build_channel" "$release_version" \
    "$release_build_number" "$release_stage" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"

  sign_macos_app "$app_bundle" "xenia.entitlements" "$mac_sign_identity"
  macos_x86_64_app_bundle="$app_bundle"
  package_macos_dmg "$app_bundle" "$out_dir/xenios_macos_intel.dmg" "LICENSE"
fi

if [ "$build_macos_universal" -eq 1 ] && [ "$build_macos_arm64" -eq 1 ] && \
    [ "$build_macos_x86_64" -eq 1 ]; then
  echo ""
  echo "== macOS universal =="
  package_macos_universal_dmg \
    "$macos_arm64_app_bundle" \
    "$macos_x86_64_app_bundle" \
    "$out_dir/xenios_macos_universal.dmg" \
    "LICENSE" \
    "$mac_sign_identity"
fi

if [ "$build_ios" -eq 1 ]; then
  echo ""
  echo "== iOS arm64 (Xcode-built, ad-hoc-signed ipa) =="

  ./xb build --config="$config" --target=xenia-shader-cc
  ./xb devenv --target-os=ios --config="$config" --no-open
  xcodebuild \
    -project build-ios-xcode/xenia.xcodeproj \
    -scheme xenia-app \
    -configuration "$buildcfg" \
    -destination generic/platform=iOS \
    CODE_SIGNING_ALLOWED=NO \
    build

  # CMake's Xcode generator appends $(CONFIGURATION)$(EFFECTIVE_PLATFORM_NAME)
  # to the runtime output directory, so device builds land in
  # "$buildcfg-iphoneos" rather than the Ninja-style bare "$buildcfg".
  ios_dir="build-ios-xcode/bin/iOS/${buildcfg}-iphoneos"
  [ -d "$ios_dir" ] || ios_dir="build-ios-xcode/bin/iOS/$buildcfg"
  app_bundle="$(find_first_app "$ios_dir")" || die "iOS app not found in $ios_dir"

  stamp_bundle_version_metadata "$app_bundle" "$release_version" "$release_build_number"
  stamp_bundle_stage_metadata "$app_bundle" "$release_stage"
  stamp_bundle_attestation "$app_bundle" "ios" "$build_channel" "$release_version" \
    "$release_build_number" "$release_stage" "$commit_short" "$issued_at" "$attestation_key_id" "$attestation_key"
  # Ad-hoc sign to embed entitlements (increased-memory-limit).
  # Re-signing tools will preserve these when applying a real identity.
  codesign --force --sign - --entitlements "$root/xenia_ios.entitlements" "$app_bundle"

  package_ios_ipa "$app_bundle" "$out_dir/xenios_ios_iphone_ipad.ipa" "$ios_bundle_id"
fi

echo ""
echo "Done. Artifacts:"
find "$out_dir" -maxdepth 1 -type f \( -name "*.dmg" -o -name "*.ipa" \) -print
