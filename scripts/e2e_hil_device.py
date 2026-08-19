#!/usr/bin/env python3
"""Device inventory, lease, and application-only flash guards for HIL E2E."""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import fcntl
except ImportError:  # pragma: no cover - HIL runners are POSIX hosts.
    fcntl = None

try:
    from sqlite_board_probe_protocol import Partition, partition_by_label
except ModuleNotFoundError:
    from scripts.sqlite_board_probe_protocol import Partition, partition_by_label

SAFE_NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
DESCRIPTOR_KEYS = frozenset({"schema_version", "name", "port", "profile"})


class HilConfigurationError(ValueError):
    """Local HIL configuration is invalid."""


class HilLeaseUnavailable(RuntimeError):
    """The selected device or serial port is already leased."""


class HilProfileMismatch(RuntimeError):
    """The selected profile does not match the observed board layout or image."""


@dataclass(frozen=True)
class OfficialProfile:
    name: str
    firmware_profile: str
    application_label: str
    application_offset: int
    application_size: int
    partition_signature: tuple[tuple[str, int, int, int, int], ...]


OFFICIAL_PROFILES = {
    "sparkbot": OfficialProfile(
        name="sparkbot",
        firmware_profile="esp32s3-esp-sparkbot",
        application_label="factory",
        application_offset=0x10000,
        application_size=0x2D0000,
        partition_signature=(
            ("nvs", 1, 2, 0x9000, 0x4000),
            ("otadata", 1, 0, 0xD000, 0x2000),
            ("phy_init", 1, 1, 0xF000, 0x1000),
            ("factory", 0, 0, 0x10000, 0x2D0000),
            ("linx_secrets", 1, 2, 0x2E0000, 0x10000),
            ("assets", 1, 0x82, 0x300000, 0x100000),
            ("model", 1, 0x82, 0x400000, 0x300000),
        ),
    ),
    "pcb": OfficialProfile(
        name="pcb",
        firmware_profile="esp32s3-voicelife-pcb-pcm",
        application_label="ota_0",
        application_offset=0x20000,
        application_size=0x3E0000,
        partition_signature=(
            ("nvs", 1, 2, 0x9000, 0x6000),
            ("otadata", 1, 0, 0xF000, 0x2000),
            ("phy_init", 1, 1, 0x11000, 0x1000),
            ("ota_0", 0, 0x10, 0x20000, 0x3E0000),
            ("ota_1", 0, 0x11, 0x400000, 0x3E0000),
            ("voicelife", 1, 0x82, 0x7E0000, 0x200000),
            ("linx_secrets", 1, 2, 0xA00000, 0x10000),
            ("model", 1, 0x82, 0xA10000, 0x300000),
        ),
    ),
}


@dataclass(frozen=True)
class DeviceDescriptor:
    name: str
    port: Path
    profile: str
    firmware_profile: str
    application_label: str


@dataclass(frozen=True)
class ApplicationImage:
    path: Path
    offset: int
    size: int
    sha256: str

    def __post_init__(self) -> None:
        if self.offset <= 0 or self.size <= 0 or HEX_SHA256.fullmatch(self.sha256) is None:
            raise HilConfigurationError("invalid application image metadata")


@dataclass(frozen=True)
class FlashOperation:
    kind: str
    offset: int
    argv: tuple[str, ...]


def official_profile(profile: str) -> OfficialProfile:
    try:
        return OFFICIAL_PROFILES[profile]
    except KeyError as error:
        raise HilConfigurationError("unknown HIL profile") from error


def _load_json_object(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HilConfigurationError("device descriptor is unreadable") from error
    if not isinstance(document, dict) or set(document) != DESCRIPTOR_KEYS:
        raise HilConfigurationError("device descriptor has invalid fields")
    return document


def load_device_descriptor(path: Path, requested_profile: str) -> DeviceDescriptor:
    """Load a local descriptor and bind it to one official profile."""
    document = _load_json_object(path)
    if document["schema_version"] != 1:
        raise HilConfigurationError("unsupported device descriptor schema")
    name = document["name"]
    port = document["port"]
    profile = document["profile"]
    if not isinstance(name, str) or SAFE_NAME.fullmatch(name) is None:
        raise HilConfigurationError("invalid device descriptor name")
    if not isinstance(port, str) or not port.startswith("/dev/") or "\x00" in port:
        raise HilConfigurationError("invalid serial port")
    if not isinstance(profile, str) or profile != requested_profile:
        raise HilConfigurationError("device profile does not match requested profile")
    selected = official_profile(profile)
    return DeviceDescriptor(
        name=name,
        port=Path(port),
        profile=profile,
        firmware_profile=selected.firmware_profile,
        application_label=selected.application_label,
    )


def _lease_key(prefix: str, value: str) -> str:
    return f"{prefix}-{hashlib.sha256(value.encode('utf-8')).hexdigest()[:24]}"


class DeviceLease:
    """Hold non-blocking POSIX locks for both inventory identity and serial path."""

    def __init__(self, descriptor: DeviceDescriptor, root: Path) -> None:
        self._descriptor = descriptor
        self._root = root
        self._handles: list[object] = []

    def acquire(self) -> None:
        if self._handles:
            raise HilLeaseUnavailable("lease is already held")
        if fcntl is None:
            raise HilLeaseUnavailable("POSIX file locking is unavailable")
        try:
            self._root.mkdir(parents=True, exist_ok=True)
            for key in (
                _lease_key("device", self._descriptor.name),
                _lease_key("serial", os.path.realpath(self._descriptor.port)),
            ):
                handle = (self._root / f"{key}.lock").open("a+", encoding="utf-8")
                try:
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError as error:
                    handle.close()
                    raise HilLeaseUnavailable("device lease is unavailable") from error
                self._handles.append(handle)
        except Exception:
            self.release()
            raise

    def release(self) -> None:
        handles, self._handles = self._handles, []
        for handle in reversed(handles):
            if fcntl is not None:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
            handle.close()


def _partition_tuple(partition: Partition) -> tuple[str, int, int, int, int]:
    return partition.label, partition.type, partition.subtype, partition.offset, partition.size


def validate_device_layout(descriptor: DeviceDescriptor, partitions: list[Partition]) -> Partition:
    """Require the complete official layout before any write can be planned."""
    selected = official_profile(descriptor.profile)
    if (
        descriptor.firmware_profile != selected.firmware_profile
        or descriptor.application_label != selected.application_label
    ):
        raise HilProfileMismatch("descriptor does not match official profile")
    observed = tuple(_partition_tuple(partition) for partition in partitions)
    if observed != selected.partition_signature:
        raise HilProfileMismatch("board partition layout does not match profile")
    application = partition_by_label(partitions, selected.application_label)
    if (
        application.type != 0
        or application.offset != selected.application_offset
        or application.size != selected.application_size
    ):
        raise HilProfileMismatch("application partition does not match profile")
    return application


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_application_image(
    build_directory: Path, descriptor: DeviceDescriptor, partitions: list[Partition]
) -> ApplicationImage:
    application = validate_device_layout(descriptor, partitions)
    binary = build_directory / "voicelife.bin"
    flasher = build_directory / "flasher_args.json"
    try:
        flash_files = json.loads(flasher.read_text(encoding="utf-8"))["flash_files"]
        matching_offsets = [int(str(offset), 0) for offset, name in flash_files.items() if name == "voicelife.bin"]
        size = binary.stat().st_size
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise HilConfigurationError("application build metadata is invalid") from error
    if matching_offsets != [application.offset] or not 0 < size <= application.size:
        raise HilProfileMismatch("application image does not match validated partition")
    return ApplicationImage(path=binary, offset=application.offset, size=size, sha256=_sha256(binary))


def application_flash_operations(port: Path, image: ApplicationImage, baud: int = 460800) -> tuple[FlashOperation, ...]:
    """Build only the write-and-verify operations for one validated app image."""
    common = (
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        str(port),
        "--baud",
        str(baud),
    )
    offset = hex(image.offset)
    return (
        FlashOperation(
            kind="write",
            offset=image.offset,
            argv=common
            + (
                "--before",
                "default-reset",
                "--after",
                "hard-reset",
                "write-flash",
                "--flash-mode",
                "dio",
                "--flash-size",
                "16MB",
                "--flash-freq",
                "80m",
                offset,
                str(image.path),
            ),
        ),
        FlashOperation(
            kind="verify",
            offset=image.offset,
            argv=common
            + ("--before", "default-reset", "--after", "hard-reset", "verify-flash", offset, str(image.path)),
        ),
    )
