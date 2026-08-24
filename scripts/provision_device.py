#!/usr/bin/env python3
"""一键构建、烧录并配置一块 VoiceLife IM 设备（SparkBot 或 PCB）。

流程：构建固件 → 烧录应用分区（不覆盖 NVS/分区表）→ 在 Gateway 服务器注册
设备 → 通过物理 USB 写入 IM 凭据 → 执行真实设备认证冒烟。Token 不经过
shell 参数或日志，服务器端不落盘，本地临时凭据文件用后即删。

支持跳过任一阶段：--skip-build / --skip-flash / --skip-register /
--skip-provision / --skip-smoke。
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVISION_SCRIPT = ROOT / "scripts" / "provision_im_config.py"
PAIRING_SCRIPT = ROOT / "scripts" / "start_im_pairing.py"

SERVER = None
SERVER_DIR = None
DEFAULT_GATEWAY_ORIGIN = None

BOARD_CONFIGS: dict[str, dict] = {
    "sparkbot": {
        "profile": "esp32s3-esp-sparkbot",
        "default_flash_offset": "0x10000",
        "description": "ESP-SparkBot（ST7789/LVGL 彩屏）",
    },
    "pcb": {
        "profile": "esp32s3-voicelife-pcb-pcm",
        "default_flash_offset": "0x20000",
        "description": "VoiceLife PCB（SSD1306 点阵屏）",
    },
}

# 与真机烧录一致的 esptool 固定参数。
FLASH_SETTINGS = ("--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m")

UUID_V4_PATTERN = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
DEVICE_ID_PATTERN = re.compile(r"^[!-~]{1,128}$")
TOKEN_PATTERN = re.compile(r"^[A-Za-z0-9_-]{43}$")
GIT_COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class RemoteOperationError(RuntimeError):
    """A remote command failed without exposing its output."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--board",
        required=True,
        choices=sorted(BOARD_CONFIGS),
        help="目标板型：sparkbot 或 pcb",
    )
    parser.add_argument("--port", help="本地串口设备；缺省自动探测 USB JTAG 串口")
    parser.add_argument("--server", help="Gateway 服务器 SSH 目标（缺省从 .env 的 PROVISION_SERVER 读取）")
    parser.add_argument("--server-dir", help="服务器仓库目录（缺省从 .env 的 PROVISION_SERVER_DIR 读取）")
    parser.add_argument(
        "--gateway-origin", help="Gateway HTTPS origin（缺省 PROVISION_GATEWAY_ORIGIN 或 WECHAT_ACTION_UI_BASE_URL）"
    )
    parser.add_argument("--user-id", help="内部 userId；缺省继承服务器上最新 active 设备的所有者")
    parser.add_argument("--idf-dir", help="ESP-IDF 安装目录（缺省 ~/esp/esp-idf）")
    parser.add_argument("--force", action="store_true", help="provisioning 时覆盖板子已有 IM 配置（VLI2）")
    parser.add_argument("--skip-build", action="store_true", help="跳过固件构建")
    parser.add_argument("--skip-flash", action="store_true", help="跳过烧录应用分区")
    parser.add_argument("--skip-register", action="store_true", help="跳过服务器设备注册")
    parser.add_argument("--skip-provision", action="store_true", help="跳过 USB IM 凭据写入")
    parser.add_argument("--skip-smoke", action="store_true", help="跳过真机认证冒烟")
    parser.add_argument("--smoke-timeout", type=float, default=240.0, help="认证冒烟超时秒数（默认 240）")
    return parser.parse_args(argv)


def load_dotenv(path: Path) -> dict[str, str]:
    """解析 KEY=VALUE 的 .env 文件；忽略注释与空行，不做值展开。"""
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def resolve_server_config(
    explicit_server: str | None,
    explicit_dir: str | None,
    environ: dict[str, str],
    env_file: Path,
) -> tuple[str, str]:
    """解析服务器目标与目录：命令行参数 > 环境变量 > .env 文件。"""
    dotenv_values = load_dotenv(env_file)
    server = explicit_server or environ.get("PROVISION_SERVER") or dotenv_values.get("PROVISION_SERVER")
    server_dir = (
        explicit_dir
        or environ.get("PROVISION_SERVER_DIR")
        or dotenv_values.get("PROVISION_SERVER_DIR")
        or "/root/XE6-15"
    )
    if not server:
        raise SystemExit("缺少 PROVISION_SERVER；请在 .env 中配置或使用 --server 指定")
    return server, server_dir


def gateway_origin_from_base_url(base_url: str) -> str:
    """从 Action UI 基础 URL 提取 HTTPS origin，丢弃路径、查询与片段。"""
    if not base_url.startswith("https://"):
        raise ValueError("WECHAT_ACTION_UI_BASE_URL 必须是 HTTPS")
    return base_url.split("/", 3)[0] + "//" + base_url.split("/", 3)[2]


def resolve_gateway_origin(
    explicit: str | None,
    environ: dict[str, str],
    env_file: Path,
) -> str:
    """Gateway origin：--gateway-origin > PROVISION_GATEWAY_ORIGIN > WECHAT_ACTION_UI_BASE_URL。"""
    if explicit:
        if not explicit.startswith("https://") or "://" not in explicit:
            raise ValueError("--gateway-origin 必须是 HTTPS origin")
        return explicit
    provision_origin = environ.get("PROVISION_GATEWAY_ORIGIN")
    if provision_origin:
        return provision_origin
    if env_file.is_file():
        values = load_dotenv(env_file)
        provision_origin = values.get("PROVISION_GATEWAY_ORIGIN")
        if provision_origin:
            return provision_origin
        base_url = values.get("WECHAT_ACTION_UI_BASE_URL")
        if base_url:
            return gateway_origin_from_base_url(base_url)
    raise SystemExit("缺少 Gateway origin；请配置 PROVISION_GATEWAY_ORIGIN 或 WECHAT_ACTION_UI_BASE_URL")


def validate_credential(data: dict, expected_user_id: str | None = None, *, require_uuid: bool = True) -> dict:
    """校验服务器返回的设备凭据，绝不回显 Token。"""
    device_id = data.get("deviceId")
    user_id = data.get("userId")
    token = data.get("deviceToken")
    status = data.get("status")
    pattern = UUID_V4_PATTERN if require_uuid else DEVICE_ID_PATTERN
    if not isinstance(device_id, str) or not pattern.fullmatch(device_id):
        raise ValueError("服务器返回的 deviceId 格式无效")
    if not isinstance(user_id, str) or not user_id:
        raise ValueError("服务器返回的 userId 为空")
    if expected_user_id is not None and user_id != expected_user_id:
        raise ValueError("服务器返回的 userId 与期望不一致")
    if not isinstance(token, str) or not TOKEN_PATTERN.fullmatch(token):
        raise ValueError("服务器返回的 deviceToken 不是 43 位 base64url")
    if status != "active":
        raise ValueError("服务器返回的设备状态不是 active")
    return data


def idf_bootstrap(idf_dir: str | None = None) -> str:
    """返回加载 ESP-IDF 环境的 bash 前缀。"""
    if idf_dir:
        return f"source {idf_dir}/export.sh &&"
    home = Path.home()
    if (home / "esp" / "esp-idf" / "export.sh").is_file():
        return f"source {home}/esp/esp-idf/export.sh &&"
    return ""


def build_command(board: str, idf_dir: str | None = None) -> str:
    profile = BOARD_CONFIGS[board]["profile"]
    prefix = idf_bootstrap(idf_dir)
    return f"{prefix} python3 {ROOT / 'scripts' / 'firmware.py'} build {profile}"


def app_flash_offset(build_dir: Path, board: str) -> str:
    """优先读 idf.py 生成的 flasher_args.json，缺失时回退板型默认偏移。"""
    flasher = build_dir / "flasher_args.json"
    if flasher.is_file():
        try:
            files = json.loads(flasher.read_text(encoding="utf-8")).get("flash_files", {})
            for offset, name in files.items():
                if name == "voicelife.bin":
                    return str(offset)
        except (OSError, json.JSONDecodeError, AttributeError):
            pass
    return BOARD_CONFIGS[board]["default_flash_offset"]


def flash_command(board: str, port: str, build_dir: Path, idf_dir: str | None = None) -> str:
    binary = build_dir / "voicelife.bin"
    if not binary.is_file():
        raise SystemExit(f"未找到应用镜像：{binary}；请先构建固件")
    offset = app_flash_offset(build_dir, board)
    prefix = idf_bootstrap(idf_dir)
    settings = " ".join(FLASH_SETTINGS)
    return (
        f"{prefix} python -m esptool --chip esp32s3 --port {port} -b 460800 "
        f"--before default-reset --after hard-reset write-flash {settings} {offset} {binary}"
    )


def _shell_assignment(name: str, value: str) -> str:
    if not DEVICE_ID_PATTERN.fullmatch(value) or "'" in value:
        raise ValueError(f"{name} contains unsafe shell characters")
    return f"{name}='{value}'"


def _shell_directory(value: str) -> str:
    if not value.startswith("/") or not DEVICE_ID_PATTERN.fullmatch(value) or "'" in value:
        raise ValueError("server directory contains unsafe shell characters")
    return f"'{value}'"


def server_register_script(
    server_dir: str,
    user_id: str | None,
    *,
    device_id: str | None = None,
    include_commit: bool = False,
) -> str:
    """构造在 Gateway 服务器上创建设备的 bash 脚本；stdout 仅输出凭据 JSON。"""
    lines = [
        "set -euo pipefail",
        f"cd {_shell_directory(server_dir)}",
    ]
    if user_id:
        lines.append(_shell_assignment("user_id", user_id))
    else:
        lines.append(
            "user_id=$(docker compose exec -T postgres sh -lc "
            '\'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -Atc '
            '"SELECT user_id FROM im_devices WHERE status=\\$\\$active\\$\\$ '
            "ORDER BY created_at DESC LIMIT 1\"' </dev/null)"
        )
        lines.append('if [ -z "$user_id" ]; then echo "no active device to inherit userId" >&2; exit 3; fi')
    create = 'docker compose exec -T gateway pnpm --silent device -- create --user-id "$user_id"'
    if device_id is not None:
        lines.append(_shell_assignment("device_id", device_id))
        create += ' --device-id "$device_id"'
    create += " </dev/null"
    if include_commit:
        lines.extend(
            [
                "gateway_commit=$(git rev-parse HEAD)",
                f'if ! printf "%s" "$gateway_commit" | grep -Eq "{GIT_COMMIT_PATTERN.pattern}"; then exit 4; fi',
                f"credential=$({create})",
                "printf '%s\\n' \"$credential\" | python3 -c 'import json,sys; d=json.load(sys.stdin); "
                'd["gatewayCommit"]=sys.argv[1]; print(json.dumps(d,separators=(",",":")))\' "$gateway_commit"',
            ]
        )
    else:
        lines.append(create)
    return "\n".join(lines) + "\n"


def server_revoke_script(server_dir: str, device_id: str) -> str:
    """构造幂等吊销测试设备的远端脚本。"""
    return (
        "\n".join(
            [
                "set -euo pipefail",
                f"cd {_shell_directory(server_dir)}",
                _shell_assignment("device_id", device_id),
                (
                    "docker compose exec -T gateway pnpm --silent device -- revoke "
                    '--device-id "$device_id" </dev/null >/dev/null'
                ),
            ]
        )
        + "\n"
    )


def run_remote(server: str, script: str, timeout: int = 180) -> str:
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", server, "bash", "-s"],
        input=script,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        raise RemoteOperationError("remote_operation_failed")
    return result.stdout


def run_with_getpass(script: Path, argv: list[str], secret: str) -> None:
    """以隐藏输入运行 provisioning/pairing 脚本，secret 不进入 argv。"""
    real_getpass = getpass.getpass
    getpass.getpass = lambda _prompt="": secret  # noqa: B023
    try:
        sys.argv = ["provision"] + argv
        runpy.run_path(str(script), run_name="__main__")
    finally:
        getpass.getpass = real_getpass


def main() -> None:
    args = parse_args()
    board = args.board
    config = BOARD_CONFIGS[board]
    build_dir = ROOT / "build" / config["profile"]
    env_file = ROOT / ".env"
    server, server_dir = resolve_server_config(args.server, args.server_dir, os.environ, env_file)
    origin = resolve_gateway_origin(args.gateway_origin, os.environ, env_file)

    port = args.port
    if port is None:
        candidates = sorted(Path("/dev").glob("cu.usbmodem*"))
        if not candidates:
            raise SystemExit("未找到 USB 串口；请用 --port 显式指定")
        port = str(candidates[0])
        print(f"检测到串口：{port}")

    print(f"[1/5] 构建固件 profile={config['profile']}")
    if not args.skip_build:
        subprocess.run(["bash", "-lc", build_command(board, args.idf_dir)], check=True)
    else:
        print("跳过构建")

    print("[2/5] 烧录应用分区（不覆盖 NVS/分区表）")
    if not args.skip_flash:
        subprocess.run(["bash", "-lc", flash_command(board, port, build_dir, args.idf_dir)], check=True)
    else:
        print("跳过烧录")

    print(f"[3/5] 在服务器注册设备（{server}）")
    credential_data: dict | None = None
    if not args.skip_register:
        script = server_register_script(server_dir, args.user_id)
        stdout = run_remote(server, script)
        line = next((ln for ln in stdout.splitlines() if ln.startswith("{")), None)
        if line is None:
            raise SystemExit("服务器未返回凭据 JSON")
        credential_data = validate_credential(json.loads(line), args.user_id)
    else:
        print("跳过注册（无法 provisioning/smoke，请确认设备已注册）")

    token_file: str | None = None
    try:
        if credential_data is not None:
            with tempfile.NamedTemporaryFile(
                prefix=f"im-{board}-credentials.", suffix=".json", mode="w", delete=False
            ) as handle:
                handle.write(json.dumps(credential_data))
                token_file = handle.name
            print(f"[4/5] USB provisioning（{config['description']}）")
            if not args.skip_provision:
                run_with_getpass(
                    PROVISION_SCRIPT,
                    [
                        "--port",
                        port,
                        "--gateway-origin",
                        origin,
                        "--device-id",
                        credential_data["deviceId"],
                        "--user-id",
                        credential_data["userId"],
                    ]
                    + (["--force"] if args.force else []),
                    credential_data["deviceToken"],
                )
            else:
                print("跳过 provisioning")

            print("[5/5] 真机认证冒烟（需设备已联网）")
            if not args.skip_smoke:
                run_with_getpass(
                    PAIRING_SCRIPT,
                    [
                        "--port",
                        port,
                        "--auth-smoke",
                        "--expected-device-id",
                        credential_data["deviceId"],
                        "--expected-user-id",
                        credential_data["userId"],
                        "--timeout",
                        str(args.smoke_timeout),
                    ],
                    "",
                )
            else:
                print("跳过认证冒烟")
        else:
            print("未注册新设备；跳过 provisioning 与冒烟")
    finally:
        if token_file is not None:
            Path(token_file).unlink(missing_ok=True)

    print(f"完成：{config['description']}（{board}）")


if __name__ == "__main__":
    main()
