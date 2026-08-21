#!/usr/bin/env python3
"""Strict, sanitized evidence contract for the shared E2E runner."""

from __future__ import annotations

import json
import os
import re
import tempfile
from contextlib import suppress
from pathlib import Path
from typing import Any

HEX_ID = re.compile(r"^[0-9a-f]{32}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
HEX_COMMIT = re.compile(r"^[0-9a-f]{40}$")
HEX_FINGERPRINT = re.compile(r"^[0-9a-f]{16}$")
SAFE_NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
UTC_TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{3,6})?Z$")
JWT_PATTERN = re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{4,}\b")
SENSITIVE_KEY_PATTERN = re.compile(
    r"(?:^|_)(?:authorization|bearer|password|secret|ssid|token|wifi|device_id|user_id)(?:$|_)", re.IGNORECASE
)
SENSITIVE_VALUE_PATTERNS = (
    re.compile(r"\bauthorization\s*[:=]", re.IGNORECASE),
    re.compile(r"\bbearer\s+[A-Za-z0-9._~+/-]+", re.IGNORECASE),
    re.compile(r"\b(?:password|secret|ssid|token|wifi|device_id|user_id)\s*[:=]", re.IGNORECASE),
    JWT_PATTERN,
)

TOP_LEVEL_KEYS = frozenset(
    {
        "schema_version",
        "run_id",
        "correlation_id",
        "scope",
        "layer",
        "journey",
        "profile",
        "started_at",
        "finished_at",
        "duration_ms",
        "status",
        "failure_category",
        "failed_phase",
        "message_code",
        "stages",
        "assertions",
        "metrics",
        "cleanup",
        "hardware_verified",
        "hil",
    }
)
STAGE_KEYS = frozenset({"name", "status", "code"})
ASSERTION_KEYS = frozenset({"name", "passed", "code"})
CLEANUP_KEYS = frozenset({"status", "error_codes"})
HIL_KEYS = frozenset(
    {"firmware_sha256", "gateway_commit", "device_fingerprint", "readiness_markers", "pairing_markers"}
)
METRIC_KEYS = frozenset({"resource_count", "bound_port_count", "namespace_count"})
FAILURE_CATEGORIES = frozenset(
    {
        "configuration",
        "infrastructure",
        "product",
        "device",
        "lease",
        "external",
        "timeout",
        "interrupted",
        "cleanup",
    }
)
PHASE_ORDER = ("prepare", "run", "assert", "collect", "cleanup")
PHASES = frozenset(PHASE_ORDER)


class EvidenceValidationError(Exception):
    """Evidence did not satisfy the public allowlist contract."""

    def __init__(self, code: str = "evidence_validation_failed") -> None:
        super().__init__(code)
        self.code = code


class EvidenceWriteError(Exception):
    """Evidence could not be written atomically and privately."""

    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code


def _reject(condition: bool) -> None:
    if condition:
        raise EvidenceValidationError


def _safe_name(value: Any) -> bool:
    return isinstance(value, str) and SAFE_NAME.fullmatch(value) is not None


def _exact_keys(value: Any, expected: frozenset[str]) -> dict[str, Any]:
    _reject(not isinstance(value, dict) or set(value) != expected)
    return value


def scan_sensitive(value: Any) -> list[str]:
    """Return opaque finding codes without echoing sensitive input."""
    findings: set[str] = set()

    def visit(item: Any) -> None:
        if isinstance(item, dict):
            for key, child in item.items():
                if isinstance(key, str) and SENSITIVE_KEY_PATTERN.search(key):
                    findings.add("sensitive_key")
                visit(child)
        elif isinstance(item, (list, tuple)):
            for child in item:
                visit(child)
        elif isinstance(item, str) and any(pattern.search(item) for pattern in SENSITIVE_VALUE_PATTERNS):
            findings.add("sensitive_value")

    visit(value)
    return sorted(findings)


def _validate_stage(stage: Any) -> None:
    value = _exact_keys(stage, STAGE_KEYS)
    _reject(value["name"] not in PHASES)
    _reject(value["status"] not in {"passed", "failed", "skipped"})
    _reject(not _safe_name(value["code"]))


def _validate_assertion(assertion: Any) -> None:
    value = _exact_keys(assertion, ASSERTION_KEYS)
    _reject(not _safe_name(value["name"]))
    _reject(type(value["passed"]) is not bool)
    _reject(not _safe_name(value["code"]))


def _validate_metrics(metrics: Any) -> None:
    _reject(not isinstance(metrics, dict) or not set(metrics).issubset(METRIC_KEYS))
    for value in metrics.values():
        _reject(type(value) is not int or not 0 <= value <= 1_000_000)


def _validate_hil(hil: Any) -> None:
    value = _exact_keys(hil, HIL_KEYS)
    _reject(not isinstance(value["firmware_sha256"], str) or HEX_SHA256.fullmatch(value["firmware_sha256"]) is None)
    _reject(not isinstance(value["gateway_commit"], str) or HEX_COMMIT.fullmatch(value["gateway_commit"]) is None)
    _reject(
        not isinstance(value["device_fingerprint"], str)
        or HEX_FINGERPRINT.fullmatch(value["device_fingerprint"]) is None
    )
    _reject(value["readiness_markers"] != ["provisioned", "wifi_ready", "sntp_synced", "ready"])
    _reject(value["pairing_markers"] != ["scope_matched", "code_valid", "pending", "expired"])


def validate_evidence(document: dict[str, object]) -> None:
    """Validate the complete evidence allowlist and cross-field relations."""
    value = _exact_keys(document, TOP_LEVEL_KEYS)
    _reject(value["schema_version"] != 1)
    _reject(not isinstance(value["run_id"], str) or HEX_ID.fullmatch(value["run_id"]) is None)
    _reject(not isinstance(value["correlation_id"], str) or HEX_ID.fullmatch(value["correlation_id"]) is None)
    _reject(value["scope"] not in {"runner_contract_only", "hil_im_pairing"})
    _reject(value["layer"] not in {"host", "hil"})
    _reject(not _safe_name(value["journey"]) or not _safe_name(value["profile"]))
    _reject(not isinstance(value["started_at"], str) or UTC_TIMESTAMP.fullmatch(value["started_at"]) is None)
    _reject(not isinstance(value["finished_at"], str) or UTC_TIMESTAMP.fullmatch(value["finished_at"]) is None)
    _reject(type(value["duration_ms"]) is not int or not 0 <= value["duration_ms"] <= 86_400_000)
    _reject(value["status"] not in {"passed", "failed"})
    _reject(not _safe_name(value["message_code"]))
    _reject(type(value["hardware_verified"]) is not bool)
    if value["scope"] == "runner_contract_only":
        _reject(value["hardware_verified"] or value["hil"] is not None)
    else:
        _reject(
            value["layer"] != "hil"
            or value["journey"] != "im-pairing"
            or value["status"] != "passed"
            or value["hardware_verified"] is not True
        )
        _validate_hil(value["hil"])

    failure_category = value["failure_category"]
    failed_phase = value["failed_phase"]
    if value["status"] == "passed":
        _reject(failure_category is not None or failed_phase is not None)
    else:
        _reject(failure_category not in FAILURE_CATEGORIES or failed_phase not in PHASES)

    stages = value["stages"]
    _reject(not isinstance(stages, list) or len(stages) != len(PHASE_ORDER))
    for stage in stages:
        _validate_stage(stage)
    _reject(tuple(stage["name"] for stage in stages) != PHASE_ORDER)
    stage_statuses = {stage["name"]: stage["status"] for stage in stages}
    if value["status"] == "passed":
        _reject(any(status != "passed" for status in stage_statuses.values()))
    else:
        _reject(stage_statuses[failed_phase] != "failed")
        primary_failures = [phase for phase in PHASE_ORDER[:-1] if stage_statuses[phase] == "failed"]
        if failed_phase == "cleanup" and primary_failures:
            _reject(len(primary_failures) != 1)
            primary_index = PHASE_ORDER.index(primary_failures[0])
            for index, phase in enumerate(PHASE_ORDER[:-1]):
                expected = "passed" if index < primary_index else "skipped"
                if index == primary_index:
                    expected = "failed"
                _reject(stage_statuses[phase] != expected)
        else:
            failed_index = PHASE_ORDER.index(failed_phase)
            for index, phase in enumerate(PHASE_ORDER[:-1]):
                expected = "passed" if index < failed_index else "skipped"
                if phase == failed_phase:
                    expected = "failed"
                _reject(stage_statuses[phase] != expected)
            if failed_phase != "cleanup":
                _reject(stage_statuses["cleanup"] != "passed")

    assertions = value["assertions"]
    _reject(not isinstance(assertions, list) or len(assertions) > 64)
    for assertion in assertions:
        _validate_assertion(assertion)

    _validate_metrics(value["metrics"])
    cleanup = _exact_keys(value["cleanup"], CLEANUP_KEYS)
    _reject(cleanup["status"] not in {"passed", "failed"})
    error_codes = cleanup["error_codes"]
    _reject(not isinstance(error_codes, list) or len(error_codes) > 16)
    _reject(any(not _safe_name(code) for code in error_codes))
    _reject(bool(scan_sensitive(document)))


def canonical_json(document: dict[str, object]) -> str:
    validate_evidence(document)
    return json.dumps(document, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"


def _validated_destination(root: Path, destination: Path) -> tuple[Path, Path]:
    try:
        root = root.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise EvidenceWriteError("evidence_root_invalid") from error
    if not root.is_dir():
        raise EvidenceWriteError("evidence_root_invalid")
    if destination.is_symlink():
        raise EvidenceWriteError("evidence_destination_symlink")
    try:
        resolved_parent = destination.parent.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise EvidenceWriteError("evidence_parent_invalid") from error
    if resolved_parent != root:
        raise EvidenceWriteError("evidence_destination_outside_root")
    if not _safe_name(destination.stem) or destination.suffix != ".json":
        raise EvidenceWriteError("evidence_destination_invalid")
    return root, root / destination.name


def write_evidence(root: Path, destination: Path, document: dict[str, object]) -> None:
    """Validate then atomically write one private evidence document."""
    payload = canonical_json(document)
    root, destination = _validated_destination(root, destination)
    temporary: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(prefix=".evidence-", suffix=".tmp", dir=root)
        temporary = Path(temporary_name)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
        temporary = None
        directory_descriptor = os.open(root, os.O_RDONLY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except OSError as error:
        if temporary is not None:
            with suppress(OSError):
                temporary.unlink(missing_ok=True)
        code = "evidence_post_commit_sync_failed" if destination.exists() else "evidence_atomic_replace_failed"
        raise EvidenceWriteError(code) from error
