#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Update xenios.jp release-builds.json and builds-history.json from CI artifacts."
    )
    parser.add_argument("--website-repo", required=True, help="Path to the xenios.jp checkout")
    parser.add_argument("--metadata-file", required=True, help="build_apple_release.sh --print-metadata output")
    parser.add_argument("--channel", required=True, choices=("release", "preview"))
    parser.add_argument("--release-url", required=True, help="GitHub release URL")
    parser.add_argument("--published-at", help="ISO timestamp for the published release")
    parser.add_argument("--source-label", help="Optional source label override")
    parser.add_argument("--notes", help="Optional notes override")
    parser.add_argument("--ios-ipa")
    parser.add_argument("--ios-download-url")
    parser.add_argument("--macos-arm64-dmg")
    parser.add_argument("--macos-arm64-download-url")
    parser.add_argument("--macos-x86_64-dmg")
    parser.add_argument("--macos-x86_64-download-url")
    parser.add_argument("--macos-universal-dmg")
    parser.add_argument("--macos-universal-download-url")
    return parser.parse_args()


def now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def load_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, ensure_ascii=True)
        handle.write("\n")


def parse_metadata_file(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            metadata[key.strip()] = value.strip()
    return metadata


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def size_label(path: Path) -> str:
    size = path.stat().st_size
    units = ["B", "KB", "MB", "GB"]
    value = float(size)
    unit_index = 0
    while value >= 1024 and unit_index < len(units) - 1:
        value /= 1024
        unit_index += 1
    precision = 0 if value >= 10 or unit_index == 0 else 1
    return f"{value:.{precision}f} {units[unit_index]}"


def sanitize_tag_fragment(value: str) -> str:
    return "".join(char.lower() if char.isalnum() else "-" for char in value).strip("-")


def build_entry(
    *,
    platform: str,
    build_id: str,
    channel: str,
    version: str,
    build_number: str,
    stage: str,
    label: str,
    published_at: str,
    commit_short: str,
    release_url: str,
    artifacts: list[dict[str, Any]],
    notes: str | None,
    source_label: str,
) -> dict[str, Any]:
    return {
        "buildId": build_id,
        "channel": channel,
        "official": True,
        "appVersion": version,
        "buildNumber": build_number,
        "stage": stage,
        "commitShort": commit_short,
        "publishedAt": published_at,
        "label": label,
        "notes": notes,
        "sourceLabel": source_label,
        "sourceUrl": release_url,
        "artifacts": artifacts,
        "platform": platform,
    }


def artifact_record(
    *,
    path_value: str | None,
    download_url: str | None,
    platform: str,
    arch: str,
    kind: str,
    label: str,
    name: str,
) -> dict[str, Any] | None:
    if not path_value or not download_url:
        return None

    path = Path(path_value).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Artifact not found: {path}")

    return {
        "id": f"{platform}-{arch}-{kind}-{sanitize_tag_fragment(path.name)}",
        "name": name,
        "label": label,
        "platform": platform,
        "arch": arch,
        "kind": kind,
        "downloadUrl": download_url,
        "sha256": sha256_file(path),
        "sizeBytes": path.stat().st_size,
        "sizeLabel": size_label(path),
    }


def ensure_release_manifest(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        value = {}
    platforms = value.get("platforms")
    if not isinstance(platforms, dict):
        platforms = {}
    for platform in ("ios", "macos"):
        platform_record = platforms.get(platform)
        if not isinstance(platform_record, dict):
            platform_record = {}
        platform_record.setdefault("release", None)
        platform_record.setdefault("preview", None)
        platforms[platform] = platform_record
    value["platforms"] = platforms
    return value


def ensure_history_manifest(value: Any) -> dict[str, Any]:
    if isinstance(value, list):
        return {"generatedAt": None, "builds": value}
    if not isinstance(value, dict):
        return {"generatedAt": None, "builds": []}
    builds = value.get("builds")
    if not isinstance(builds, list):
        builds = []
    value["builds"] = builds
    return value


def upsert_history_entry(history: list[dict[str, Any]], entry: dict[str, Any], channel: str) -> None:
    entry_id = f"{entry['platform']}-{channel}-{entry['buildId']}"
    history_entry = {
        **entry,
        "id": entry_id,
        "submittedBy": "GitHub Actions",
    }
    for index, existing in enumerate(history):
        if isinstance(existing, dict) and existing.get("id") == entry_id:
            history[index] = history_entry
            return
    history.append(history_entry)


def main() -> int:
    args = parse_args()
    metadata = parse_metadata_file(Path(args.metadata_file).expanduser().resolve())

    required_metadata = [
        "version",
        "build_number",
        "commit_short",
        "display_label",
        "ios_build_id",
        "macos_build_id",
        "stage",
    ]
    missing = [key for key in required_metadata if not metadata.get(key)]
    if missing:
        raise SystemExit(f"metadata file is missing required keys: {', '.join(missing)}")

    published_at = args.published_at or metadata.get("issued_at") or now_iso()
    source_label = args.source_label or ("GitHub Prerelease" if args.channel == "preview" else "GitHub Release")
    notes = args.notes or (
        "Official CI preview build published from xenios-jp/XeniOS."
        if args.channel == "preview"
        else "Official CI release build published from xenios-jp/XeniOS."
    )

    ios_artifacts = [
        artifact_record(
            path_value=args.ios_ipa,
            download_url=args.ios_download_url,
            platform="ios",
            arch="arm64",
            kind="ipa",
            label="iPhone / iPad (IPA)",
            name="XeniOS for iPhone / iPad",
        )
    ]
    ios_artifacts = [artifact for artifact in ios_artifacts if artifact]

    macos_artifacts = [
        artifact_record(
            path_value=args.macos_universal_dmg,
            download_url=args.macos_universal_download_url,
            platform="macos",
            arch="universal",
            kind="dmg",
            label="Universal",
            name="XeniOS for Mac",
        ),
        artifact_record(
            path_value=args.macos_arm64_dmg,
            download_url=args.macos_arm64_download_url,
            platform="macos",
            arch="arm64",
            kind="dmg",
            label="Apple Silicon",
            name="XeniOS for Apple Silicon",
        ),
        artifact_record(
            path_value=args.macos_x86_64_dmg,
            download_url=args.macos_x86_64_download_url,
            platform="macos",
            arch="x86_64",
            kind="dmg",
            label="Intel",
            name="XeniOS for Intel",
        ),
    ]
    macos_artifacts = [artifact for artifact in macos_artifacts if artifact]

    website_repo = Path(args.website_repo).expanduser().resolve()
    release_manifest_path = website_repo / "data" / "release-builds.json"
    history_manifest_path = website_repo / "data" / "builds-history.json"

    release_manifest = ensure_release_manifest(load_json(release_manifest_path, {}))
    history_manifest = ensure_history_manifest(load_json(history_manifest_path, {}))

    ios_entry = build_entry(
        platform="ios",
        build_id=metadata["ios_build_id"],
        channel=args.channel,
        version=metadata["version"],
        build_number=metadata["build_number"],
        stage=metadata["stage"],
        label=metadata["display_label"],
        published_at=published_at,
        commit_short=metadata["commit_short"],
        release_url=args.release_url,
        artifacts=ios_artifacts,
        notes=notes,
        source_label=source_label,
    )
    macos_entry = build_entry(
        platform="macos",
        build_id=metadata["macos_build_id"],
        channel=args.channel,
        version=metadata["version"],
        build_number=metadata["build_number"],
        stage=metadata["stage"],
        label=metadata["display_label"],
        published_at=published_at,
        commit_short=metadata["commit_short"],
        release_url=args.release_url,
        artifacts=macos_artifacts,
        notes=notes,
        source_label=source_label,
    )

    release_manifest["generatedAt"] = now_iso()
    if ios_artifacts:
        release_manifest["platforms"]["ios"][args.channel] = ios_entry
    if macos_artifacts:
        release_manifest["platforms"]["macos"][args.channel] = macos_entry

    history_manifest["generatedAt"] = release_manifest["generatedAt"]
    history = [entry for entry in history_manifest.get("builds", []) if isinstance(entry, dict)]
    if ios_artifacts:
        upsert_history_entry(history, ios_entry, args.channel)
    if macos_artifacts:
        upsert_history_entry(history, macos_entry, args.channel)
    history.sort(key=lambda entry: entry.get("publishedAt", ""), reverse=True)
    history_manifest["builds"] = history

    dump_json(release_manifest_path, release_manifest)
    dump_json(history_manifest_path, history_manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
