#!/usr/bin/env python3
"""Real ESP32-S3 provisioning and pairing HIL adapter for the shared runner."""

from __future__ import annotations

import hashlib
import json
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from collect_linx_e2e_evidence import hil_readiness_markers, hil_readiness_status
from e2e_hil_device import (
    ApplicationImage,
    DeviceDescriptor,
    DeviceLease,
    HilConfigurationError,
    HilLeaseUnavailable,
    HilProfileMismatch,
    Partition,
    application_flash_operations,
    load_application_image,
    load_device_descriptor,
    validate_device_layout,
)
from e2e_runner import AssertionResult, FailureCategory, RunContext, RunnerDeadlineExceeded, RunnerFailure
from start_im_pairing import PairingLifecycle, PairingLifecycleError

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class TemporaryIdentity:
    device_id: str = field(repr=False)
    user_id: str = field(repr=False)
    token: bytearray = field(repr=False)
    gateway_commit: str


class HilHardware(Protocol):
    def inspect(self, descriptor: DeviceDescriptor, temporary_directory: Path) -> list[Partition]: ...

    def build(self, descriptor: DeviceDescriptor) -> Path: ...

    def image(
        self, build_directory: Path, descriptor: DeviceDescriptor, partitions: list[Partition]
    ) -> ApplicationImage: ...

    def flash(self, descriptor: DeviceDescriptor, image: ApplicationImage, timeout_s: float) -> None: ...

    def register(self, run_id: str) -> TemporaryIdentity: ...

    def revoke(self, identity: TemporaryIdentity) -> None: ...

    def provision(self, descriptor: DeviceDescriptor, identity: TemporaryIdentity, timeout_s: float) -> None: ...

    def reboot_and_readiness(self, descriptor: DeviceDescriptor, timeout_s: float) -> list[dict[str, object]]: ...

    def pair(
        self, descriptor: DeviceDescriptor, identity: TemporaryIdentity, timeout_s: float
    ) -> list[dict[str, str]]: ...

    def recover(self, descriptor: DeviceDescriptor) -> None: ...


def device_fingerprint(device_id: str, run_id: str) -> str:
    return hashlib.sha256(f"{run_id}:{device_id}".encode()).hexdigest()[:16]


def _runner_failure(error: Exception) -> RunnerFailure:
    if isinstance(error, HilConfigurationError):
        return RunnerFailure(FailureCategory.CONFIGURATION, "hil_configuration_invalid")
    if isinstance(error, HilLeaseUnavailable):
        return RunnerFailure(FailureCategory.DEVICE, "device_lease_unavailable")
    if isinstance(error, HilProfileMismatch):
        return RunnerFailure(FailureCategory.DEVICE, "device_profile_mismatch")
    raise error


class HilPairingAdapter:
    """Execute the real HIL flow through injected hardware boundary adapters."""

    def __init__(self, descriptor_path: Path, lease_root: Path, *, hardware: HilHardware) -> None:
        self._descriptor_path = descriptor_path
        self._lease_root = lease_root
        self._hardware = hardware
        self._descriptor: DeviceDescriptor | None = None
        self._lease: DeviceLease | None = None
        self._partitions: list[Partition] = []
        self._image: ApplicationImage | None = None
        self._identity: TemporaryIdentity | None = None
        self._readiness: list[dict[str, object]] = []
        self._pairing_markers: list[str] = []
        self._device_fingerprint = ""
        self.lease_held = False

    def prepare(self, context: RunContext) -> None:
        try:
            descriptor = load_device_descriptor(self._descriptor_path, context.config.profile)
            lease = DeviceLease(descriptor, self._lease_root)
            lease.acquire()
            self.lease_held = True
            self._descriptor = descriptor
            self._lease = lease
            context.cleanup.push("hil-device-lease", self._release_lease)
            self._partitions = self._hardware.inspect(descriptor, context.temporary_directory)
            validate_device_layout(descriptor, self._partitions)
        except (HilConfigurationError, HilLeaseUnavailable, HilProfileMismatch) as error:
            raise _runner_failure(error) from error

    def _release_lease(self) -> None:
        if self._lease is not None:
            self._lease.release()
        self.lease_held = False

    def _recover(self) -> None:
        if self._descriptor is not None:
            self._hardware.recover(self._descriptor)

    def _revoke(self) -> None:
        if self._identity is not None:
            self._hardware.revoke(self._identity)

    def run(self, context: RunContext) -> dict[str, object]:
        if self._descriptor is None:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "hil_device_not_prepared")
        try:
            build_directory = self._hardware.build(self._descriptor)
            self._image = self._hardware.image(build_directory, self._descriptor, self._partitions)
            self._hardware.flash(self._descriptor, self._image, context.remaining())
            identity = self._hardware.register(context.run_id)
            self._identity = identity
            self._device_fingerprint = device_fingerprint(identity.device_id, context.run_id)
            context.cleanup.push("gateway-device-revoke", self._revoke)
            context.cleanup.push("hil-device-recovery", self._recover)
            self._hardware.provision(self._descriptor, identity, context.remaining())
            self._readiness = self._hardware.reboot_and_readiness(self._descriptor, context.remaining())
            ready, _ = hil_readiness_status(self._readiness)
            if not ready:
                raise RunnerFailure(FailureCategory.PRODUCT, "hil_readiness_incomplete")
            lifecycle = PairingLifecycle(identity.device_id, identity.user_id)
            try:
                for event in self._hardware.pair(self._descriptor, identity, context.remaining()):
                    lifecycle.observe(event)
            except PairingLifecycleError as error:
                raise RunnerFailure(FailureCategory.PRODUCT, "pairing_lifecycle_invalid") from error
            if not lifecycle.complete:
                raise RunnerFailure(FailureCategory.PRODUCT, "pairing_lifecycle_incomplete")
            self._pairing_markers = lifecycle.public_markers
            return {"readiness_complete": True, "pairing_complete": True}
        except (HilConfigurationError, HilLeaseUnavailable, HilProfileMismatch) as error:
            raise _runner_failure(error) from error

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        values = result if isinstance(result, dict) else {}
        return [
            AssertionResult(
                name="readiness_complete",
                passed=values.get("readiness_complete") is True,
                code="ok" if values.get("readiness_complete") is True else "incomplete",
            ),
            AssertionResult(
                name="pairing_complete",
                passed=values.get("pairing_complete") is True,
                code="ok" if values.get("pairing_complete") is True else "incomplete",
            ),
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        if self._descriptor is None or self._image is None or self._identity is None:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "hil_evidence_incomplete")
        return {
            "scope": "hil_im_pairing",
            "hardware_verified": all(assertion.passed for assertion in assertions),
            "firmware_sha256": self._image.sha256,
            "gateway_commit": self._identity.gateway_commit,
            "device_fingerprint": self._device_fingerprint,
            "readiness_markers": hil_readiness_markers(self._readiness),
            "pairing_markers": list(self._pairing_markers),
            "metrics": {"resource_count": 4},
        }


class RealHilHardware:
    """Production adapters that reuse existing build, flash, provisioning and pairing scripts."""

    def __init__(self, server: str, server_directory: str, gateway_origin: str, user_id: str) -> None:
        self._server = server
        self._server_directory = server_directory
        self._gateway_origin = gateway_origin
        self._user_id = user_id

    def _run(self, command: list[str], timeout_s: float, *, input_text: str | None = None) -> str:
        try:
            result = subprocess.run(
                command,
                cwd=ROOT,
                input=input_text,
                text=True,
                capture_output=True,
                timeout=max(0.1, timeout_s),
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise RunnerDeadlineExceeded from error
        if result.returncode != 0:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "hil_command_failed")
        return result.stdout

    def inspect(self, descriptor: DeviceDescriptor, temporary_directory: Path) -> list[Partition]:
        try:
            from sqlite_board_probe_io import read_layout

            return read_layout(descriptor.port.as_posix(), 115200, temporary_directory / "partition-table.bin")
        except Exception as error:
            raise RunnerFailure(FailureCategory.DEVICE, "device_layout_unavailable") from error

    def build(self, descriptor: DeviceDescriptor) -> Path:
        from firmware import build

        try:
            return build(descriptor.firmware_profile)
        except Exception as error:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "firmware_build_failed") from error

    def image(
        self, build_directory: Path, descriptor: DeviceDescriptor, partitions: list[Partition]
    ) -> ApplicationImage:
        return load_application_image(build_directory, descriptor, partitions)

    def flash(self, descriptor: DeviceDescriptor, image: ApplicationImage, timeout_s: float) -> None:
        for operation in application_flash_operations(descriptor.port, image):
            self._run(list(operation.argv), timeout_s)

    def _remote(self, script: str, timeout_s: float = 180.0) -> str:
        return self._run(["ssh", "-o", "BatchMode=yes", self._server, "bash", "-s"], timeout_s, input_text=script)

    def register(self, run_id: str) -> TemporaryIdentity:
        from provision_device import server_register_script, validate_credential

        device_id = f"e2e-{run_id}"
        script = server_register_script(self._server_directory, self._user_id, device_id=device_id, include_commit=True)
        stdout = self._remote(script)
        lines = [line for line in stdout.splitlines() if line.startswith("{")]
        if len(lines) != 1:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "device_registration_failed")
        try:
            document = json.loads(lines[0])
            gateway_commit = document.pop("gatewayCommit")
            credential = validate_credential(document, self._user_id, require_uuid=False)
            return TemporaryIdentity(
                credential["deviceId"],
                credential["userId"],
                bytearray(credential["deviceToken"].encode()),
                gateway_commit,
            )
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "device_registration_invalid") from error

    def revoke(self, identity: TemporaryIdentity) -> None:
        from provision_device import server_revoke_script

        try:
            self._remote(server_revoke_script(self._server_directory, identity.device_id))
        finally:
            for index in range(len(identity.token)):
                identity.token[index] = 0

    def provision(self, descriptor: DeviceDescriptor, identity: TemporaryIdentity, timeout_s: float) -> None:
        from provision_device import run_with_getpass

        try:
            run_with_getpass(
                ROOT / "scripts" / "provision_im_config.py",
                [
                    "--port",
                    str(descriptor.port),
                    "--gateway-origin",
                    self._gateway_origin,
                    "--device-id",
                    identity.device_id,
                    "--user-id",
                    identity.user_id,
                    "--force",
                    "--timeout",
                    str(timeout_s),
                ],
                identity.token.decode("ascii"),
            )
        except SystemExit as error:
            raise RunnerFailure(FailureCategory.DEVICE, "usb_provisioning_failed") from error

    def reboot_and_readiness(self, descriptor: DeviceDescriptor, timeout_s: float) -> list[dict[str, object]]:
        return self._read_serial_readiness(descriptor, timeout_s)

    def _read_serial_readiness(self, descriptor: DeviceDescriptor, timeout_s: float) -> list[dict[str, object]]:
        from collect_linx_e2e_evidence import parse_im_signal

        try:
            import serial
        except ImportError as error:
            raise RunnerFailure(FailureCategory.CONFIGURATION, "pyserial_unavailable") from error
        signals: list[dict[str, object]] = [{"signal": "provisioned"}]
        deadline = time.monotonic() + timeout_s
        with serial.Serial(descriptor.port, 115200, timeout=0.2, write_timeout=2) as device:
            device.dtr = False
            device.rts = True
            time.sleep(0.15)
            device.rts = False
            while time.monotonic() < deadline:
                signal = parse_im_signal(device.readline().decode("utf-8", "replace"))
                if signal is not None:
                    signals.append(signal)
                    if signal["signal"] == "ready" or signal["signal"] in {
                        "degraded",
                        "provision_failure",
                        "startup_failure",
                    }:
                        return signals
        return signals

    def pair(self, descriptor: DeviceDescriptor, identity: TemporaryIdentity, timeout_s: float) -> list[dict[str, str]]:
        return self._pair_over_serial(descriptor, timeout_s)

    def _pair_over_serial(self, descriptor: DeviceDescriptor, timeout_s: float) -> list[dict[str, str]]:
        from start_im_pairing import parse_pairing_line, trigger_payload

        try:
            import serial
        except ImportError as error:
            raise RunnerFailure(FailureCategory.CONFIGURATION, "pyserial_unavailable") from error
        events: list[dict[str, str]] = []
        deadline = time.monotonic() + timeout_s
        with serial.Serial(descriptor.port, 115200, timeout=0.2, write_timeout=2) as device:
            device.write(trigger_payload(1))
            device.flush()
            while time.monotonic() < deadline:
                event = parse_pairing_line(device.readline())
                if event is None:
                    continue
                events.append(event)
                if event.get("status") in {
                    "expired",
                    "confirmed",
                    "cancelled",
                    "not_found",
                    "timed_out",
                    "credential_rejected",
                    "failed",
                }:
                    return events
        return events

    def recover(self, descriptor: DeviceDescriptor) -> None:
        try:
            import serial
        except ImportError as error:
            raise RunnerFailure(FailureCategory.CONFIGURATION, "pyserial_unavailable") from error
        with serial.Serial(descriptor.port, 115200, timeout=0.2, write_timeout=2) as device:
            device.dtr = False
            device.rts = True
            time.sleep(0.15)
            device.rts = False
