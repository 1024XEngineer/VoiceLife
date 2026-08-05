#!/usr/bin/env python3
"""Validate adapter profiles, build firmware, and create traceable packages.

The profile/build workflow is adapted from 78/xiaozhi-esp32 scripts/build.py
(MIT), then reduced to VoiceLife's explicit adapter boundaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / "config" / "profiles"
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
CAPABILITY_PATTERN = re.compile(r"^[a-z][a-z0-9_.-]*$")
SDKCONFIG_PATTERN = re.compile(r"^CONFIG_[A-Z0-9_]+=.+$")
ADAPTER_KINDS = ("audio", "speech", "storage", "im")
SUPPORTED_TARGETS = {"esp32s3"}


class ProfileError(ValueError):
    pass


def load_profile(path: Path) -> dict:
    try:
        profile = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProfileError(f"无法读取 Profile {path}: {error}") from error
    validate_profile(profile, path)
    return profile


def validate_profile(profile: dict, path: Path) -> None:
    allowed_root = {"schemaVersion", "id", "target", "adapters", "sdkconfig"}
    unknown_root = set(profile) - allowed_root
    if unknown_root:
        raise ProfileError(f"{path}: 未知字段 {sorted(unknown_root)}")
    if profile.get("schemaVersion") != 1:
        raise ProfileError(f"{path}: 仅支持 schemaVersion=1")
    profile_id = profile.get("id")
    if not isinstance(profile_id, str) or not ID_PATTERN.fullmatch(profile_id):
        raise ProfileError(f"{path}: id 只能使用小写字母、数字、点和连字符")
    if profile.get("target") not in SUPPORTED_TARGETS:
        raise ProfileError(f"{path}: 当前主干只验证 esp32s3")

    adapters = profile.get("adapters")
    if not isinstance(adapters, dict) or set(adapters) != set(ADAPTER_KINDS):
        raise ProfileError(f"{path}: adapters 必须且只能包含 {', '.join(ADAPTER_KINDS)}")
    for kind, adapter in adapters.items():
        if not isinstance(adapter, dict):
            raise ProfileError(f"{path}: adapters.{kind} 必须是对象")
        unknown = set(adapter) - {"driver", "capabilities", "configRef"}
        if unknown:
            raise ProfileError(f"{path}: adapters.{kind} 存在未知字段 {sorted(unknown)}")
        driver = adapter.get("driver")
        if not isinstance(driver, str) or not ID_PATTERN.fullmatch(driver):
            raise ProfileError(f"{path}: adapters.{kind}.driver 格式错误")
        capabilities = adapter.get("capabilities")
        if not isinstance(capabilities, list):
            raise ProfileError(f"{path}: adapters.{kind}.capabilities 必须是数组")
        if any(not isinstance(item, str) or not CAPABILITY_PATTERN.fullmatch(item) for item in capabilities):
            raise ProfileError(f"{path}: adapters.{kind}.capabilities 格式错误")
        if len(capabilities) != len(set(capabilities)):
            raise ProfileError(f"{path}: adapters.{kind}.capabilities 不能重复")
        config_ref = adapter.get("configRef")
        if config_ref is not None and (
            not isinstance(config_ref, str) or not re.fullmatch(r"(nvs|env|secret)://.+", config_ref)
        ):
            raise ProfileError(f"{path}: adapters.{kind}.configRef 只能引用 nvs/env/secret")

    sdkconfig = profile.get("sdkconfig")
    if not isinstance(sdkconfig, list):
        raise ProfileError(f"{path}: sdkconfig 必须是数组")
    if any(not isinstance(item, str) or not SDKCONFIG_PATTERN.fullmatch(item) for item in sdkconfig):
        raise ProfileError(f"{path}: sdkconfig 只能包含 CONFIG_NAME=value")
    if len(sdkconfig) != len(set(sdkconfig)):
        raise ProfileError(f"{path}: sdkconfig 不能重复")


def profile_path(profile_id: str) -> Path:
    path = PROFILES / f"{profile_id}.json"
    if not path.is_file():
        raise ProfileError(f"找不到 Profile: {profile_id}")
    return path


def list_profiles(as_json: bool) -> None:
    profiles = [load_profile(path) for path in sorted(PROFILES.glob("*.json"))]
    if as_json:
        print(json.dumps(profiles, ensure_ascii=False))
        return
    for profile in profiles:
        adapters = ", ".join(f"{kind}={value['driver']}" for kind, value in profile["adapters"].items())
        print(f"{profile['id']:<20} {profile['target']:<10} {adapters}")


def validate_all() -> None:
    paths = sorted(PROFILES.glob("*.json"))
    if not paths:
        raise ProfileError("没有可验证的 Profile")
    for path in paths:
        profile = load_profile(path)
        print(f"PASS {profile['id']}")


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    try:
        subprocess.run(command, cwd=ROOT, check=True)
    except FileNotFoundError as error:
        raise ProfileError(f"找不到命令 {command[0]}，请先加载对应工具链环境") from error


def build(profile_id: str) -> Path:
    profile = load_profile(profile_path(profile_id))
    build_dir = ROOT / "build" / profile_id
    build_dir.mkdir(parents=True, exist_ok=True)
    defaults = build_dir / "sdkconfig.profile.defaults"
    defaults.write_text("\n".join(profile["sdkconfig"]) + "\n", encoding="utf-8")
    run(
        [
            "idf.py",
            "-B",
            str(build_dir),
            f"-DSDKCONFIG={build_dir / 'sdkconfig'}",
            f"-DSDKCONFIG_DEFAULTS={ROOT / 'sdkconfig.defaults'};{defaults}",
            f"-DIDF_TARGET={profile['target']}",
            "build",
        ]
    )
    return build_dir


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def project_version() -> str:
    content = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER "([^"]+)"\)', content)
    if not match:
        raise ProfileError("CMakeLists.txt 缺少 PROJECT_VER")
    return match.group(1)


def package(profile_id: str, rebuild: bool) -> Path:
    profile = load_profile(profile_path(profile_id))
    build_dir = build(profile_id) if rebuild else ROOT / "build" / profile_id
    if not (build_dir / "voicelife.bin").is_file():
        raise ProfileError(f"{profile_id} 尚未构建，请先运行 build 或使用 package --build")
    run(["idf.py", "-B", str(build_dir), "merge-bin"])
    merged = build_dir / "merged-binary.bin"
    if not merged.is_file():
        raise ProfileError("idf.py merge-bin 未生成 merged-binary.bin")

    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    artifact = dist / f"voicelife-v{project_version()}-{profile_id}.zip"
    manifest = {
        "project": "voicelife",
        "version": project_version(),
        "profile": profile,
        "firmware": {"file": "merged-binary.bin", "sha256": sha256(merged), "bytes": merged.stat().st_size},
    }
    with zipfile.ZipFile(artifact, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(merged, "merged-binary.bin")
        archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(artifact)
    return artifact


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    list_parser = subparsers.add_parser("list", help="列出可用适配器 Profile")
    list_parser.add_argument("--json", action="store_true")
    subparsers.add_parser("validate", help="验证全部 Profile")
    build_parser = subparsers.add_parser("build", help="按 Profile 构建固件")
    build_parser.add_argument("profile")
    package_parser = subparsers.add_parser("package", help="合并并打包固件")
    package_parser.add_argument("profile")
    package_parser.add_argument("--build", action="store_true", help="打包前重新构建")
    args = parser.parse_args()

    try:
        if args.command == "list":
            list_profiles(args.json)
        elif args.command == "validate":
            validate_all()
        elif args.command == "build":
            build(args.profile)
        elif args.command == "package":
            package(args.profile, args.build)
    except (ProfileError, subprocess.CalledProcessError) as error:
        print(f"ERROR {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
