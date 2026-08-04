import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "sqlite_board_probe.py"
SPEC = importlib.util.spec_from_file_location("sqlite_board_probe", MODULE_PATH)
probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe
SPEC.loader.exec_module(probe)


def partition_entry(label, type_value, subtype, offset, size, flags=0):
    return probe.PARTITION_ENTRY.pack(
        probe.PARTITION_MAGIC,
        type_value,
        subtype,
        offset,
        size,
        label.encode("ascii").ljust(16, b"\0"),
        flags,
    )


def partition_table_bytes():
    entries = b"".join(
        [
            partition_entry("otadata", 1, 0, 0xD000, 0x2000),
            partition_entry("ota_1", 0, 0x11, 0x410000, 0x3F0000),
            partition_entry("voicelife", 1, 0x82, 0xE00000, 0x200000),
        ]
    )
    return (entries + b"\xff" * probe.PARTITION_ENTRY.size).ljust(
        probe.PARTITION_TABLE_SIZE, b"\xff"
    )


def write_backup_manifest(directory, *, erased_slot=False):
    table_path = directory / "partition-table.bin"
    table_path.write_bytes(partition_table_bytes())
    data_path = directory / "voicelife.bin"
    data_path.write_bytes(b"data".ljust(0x200000, b"\0"))
    ota_path = directory / "otadata.bin"
    ota_path.write_bytes(b"ota".ljust(0x2000, b"\0"))
    slot_path = directory / "ota_1.bin"
    slot_path.write_bytes(
        b"\xff" * 0x3F0000 if erased_slot else b"slot!".ljust(0x3F0000, b"\0")
    )
    manifest = {
        "schema_version": 1,
        "chip": "esp32s3",
        "baud": 115200,
        "partition_table": {
            "file": table_path.name,
            "sha256": probe.sha256(table_path),
            "offset": probe.PARTITION_TABLE_OFFSET,
            "size": probe.PARTITION_TABLE_SIZE,
        },
        "artifacts": {
            "data": {
                "file": data_path.name,
                "sha256": probe.sha256(data_path),
                "partition": {
                    "label": "voicelife",
                    "type": 1,
                    "subtype": 0x82,
                    "offset": 0xE00000,
                    "size": 0x200000,
                    "flags": 0,
                },
            },
            "probe_slot": {
                "file": slot_path.name,
                "sha256": probe.sha256(slot_path),
                "erased": erased_slot,
                "partition": {
                    "label": "ota_1",
                    "type": 0,
                    "subtype": 0x11,
                    "offset": 0x410000,
                    "size": 0x3F0000,
                    "flags": 0,
                },
            },
            "otadata": {
                "file": ota_path.name,
                "sha256": probe.sha256(ota_path),
                "partition": {
                    "label": "otadata",
                    "type": 1,
                    "subtype": 0,
                    "offset": 0xD000,
                    "size": 0x2000,
                    "flags": 0,
                },
            },
        },
    }
    (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return table_path


class PartitionTableTest(unittest.TestCase):
    def test_parses_and_validates_expected_board_layout(self):
        content = b"".join(
            [
                partition_entry("otadata", 1, 0, 0xD000, 0x2000),
                partition_entry("ota_1", 0, 0x11, 0x410000, 0x3F0000),
                partition_entry("voicelife", 1, 0x82, 0xE00000, 0x200000),
            ]
        ) + b"\xff" * probe.PARTITION_ENTRY.size

        partitions = probe.parse_partition_table(content)
        layout = probe.validate_layout(partitions, "voicelife", "ota_1", 0x200000)

        self.assertEqual(layout["data"].offset, 0xE00000)
        self.assertEqual(layout["probe_slot"].size, 0x3F0000)

    def test_rejects_unknown_partition_magic(self):
        content = struct.pack("<H", 0x1234) + b"\0" * (probe.PARTITION_ENTRY.size - 2)
        with self.assertRaisesRegex(probe.ProbeError, "invalid partition entry magic"):
            probe.parse_partition_table(content)

    def test_rejects_non_ff_bytes_after_partition_terminator(self):
        content = partition_table_bytes()
        terminator = 3 * probe.PARTITION_ENTRY.size
        corrupted = content[:terminator] + b"\xff" * (probe.PARTITION_ENTRY.size - 1) + b"X" + content[
            terminator + probe.PARTITION_ENTRY.size :
        ]
        with self.assertRaisesRegex(probe.ProbeError, "after its terminator"):
            probe.parse_partition_table(corrupted)

    def test_rejects_duplicate_labels(self):
        content = b"".join(
            [
                partition_entry("voicelife", 1, 0x82, 0xE00000, 0x200000),
                partition_entry("voicelife", 1, 0x82, 0xC00000, 0x200000),
            ]
        )
        with self.assertRaisesRegex(probe.ProbeError, "labels are not unique"):
            probe.parse_partition_table(content)


class ResetSequenceTest(unittest.TestCase):
    def test_requires_both_external_reset_points_in_order(self):
        sequence = probe.ResetSequence()

        sequence.observe("PROBE_PHASE: phase=0 image=0123456789abcdef")
        self.assertEqual(sequence.observe("HOST_RESET_POINT: OPEN_TRANSACTION"), "reset")
        sequence.observe("PROBE_PHASE: phase=1 image=0123456789abcdef")
        self.assertEqual(sequence.observe("HOST_RESET_POINT: AFTER_COMMIT"), "reset")
        sequence.observe("PROBE_PHASE: phase=2 image=0123456789abcdef")
        self.assertEqual(sequence.observe("PROBE_RESULT: PASS"), "pass")

    def test_rejects_pass_without_commit_reset(self):
        sequence = probe.ResetSequence()
        sequence.observe("PROBE_PHASE: phase=0 image=0123456789abcdef")
        sequence.observe("HOST_RESET_POINT: OPEN_TRANSACTION")
        with self.assertRaisesRegex(probe.ProbeError, "without required resets"):
            sequence.observe("PROBE_RESULT: PASS")

    def test_rejects_probe_that_resumes_a_stale_phase(self):
        sequence = probe.ResetSequence()

        with self.assertRaisesRegex(probe.ProbeError, "unexpected probe phase: 1"):
            sequence.observe("PROBE_PHASE: phase=1 image=0123456789abcdef")

    def test_rejects_image_change_between_resets(self):
        sequence = probe.ResetSequence()
        sequence.observe("PROBE_PHASE: phase=0 image=0123456789abcdef")
        sequence.observe("HOST_RESET_POINT: OPEN_TRANSACTION")

        with self.assertRaisesRegex(probe.ProbeError, "probe image changed"):
            sequence.observe("PROBE_PHASE: phase=1 image=fedcba9876543210")

    def test_rejects_board_failure(self):
        with self.assertRaisesRegex(probe.ProbeError, "reported failure"):
            probe.ResetSequence().observe("PROBE_RESULT: FAIL step=rollback")

    def test_rejects_firmware_abort_without_waiting_for_timeout(self):
        with self.assertRaisesRegex(probe.ProbeError, "firmware aborted"):
            probe.ResetSequence().observe("ESP_ERROR_CHECK failed: ESP_FAIL")


class BaudTest(unittest.TestCase):
    def test_accepts_only_board_validated_baud(self):
        self.assertEqual(probe.stable_baud("115200"), 115200)
        with self.assertRaisesRegex(Exception, "只允许使用已验证的 115200"):
            probe.stable_baud("460800")


class EsptoolCompatibilityTest(unittest.TestCase):
    @mock.patch.object(probe, "run")
    def test_uses_cli_names_supported_by_esptool_4_and_5(self, run):
        probe.esptool(
            "/dev/cu.test",
            115200,
            ["read-flash", "0x8000", "0x1000", "/tmp/table.bin"],
            before="default-reset",
            after="hard-reset",
        )

        command = run.call_args.args[0]
        self.assertIn("default_reset", command)
        self.assertIn("hard_reset", command)
        self.assertIn("read_flash", command)


class RestoreTest(unittest.TestCase):
    def test_restores_data_then_original_probe_slot_and_otadata_last(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            table_path = write_backup_manifest(directory)
            args = SimpleNamespace(yes=True, directory=directory, port="/dev/cu.test", baud=115200)

            def copy_partition_table(_port, _baud, _offset, _size, destination, **_kwargs):
                destination.write_bytes(table_path.read_bytes())

            with mock.patch("builtins.print"), mock.patch.object(
                probe, "read_flash", side_effect=copy_partition_table
            ) as read_flash, mock.patch.object(probe, "esptool") as esptool, mock.patch.object(
                probe, "verify_flash"
            ):
                probe.restore(args)

            self.assertEqual(read_flash.call_args.kwargs["after"], "no-reset")
            operations = [call.args[2][0] for call in esptool.call_args_list]
            self.assertEqual(operations, ["write-flash", "write-flash", "write-flash"])
            offsets = [call.args[2][1] for call in esptool.call_args_list]
            self.assertEqual(offsets, [hex(0xE00000), hex(0x410000), hex(0xD000)])

    def test_restores_erased_probe_slot_with_erase_and_full_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            table_path = write_backup_manifest(directory, erased_slot=True)
            args = SimpleNamespace(yes=True, directory=directory, port="/dev/cu.test", baud=115200)

            def copy_partition_table(_port, _baud, _offset, _size, destination, **_kwargs):
                destination.write_bytes(table_path.read_bytes())

            with mock.patch("builtins.print"), mock.patch.object(
                probe, "read_flash", side_effect=copy_partition_table
            ), mock.patch.object(probe, "esptool") as esptool, mock.patch.object(
                probe, "verify_flash"
            ) as verify_flash:
                probe.restore(args)

            operations = [call.args[2][0] for call in esptool.call_args_list]
            self.assertEqual(operations, ["write-flash", "erase-region", "write-flash"])
            self.assertEqual(verify_flash.call_args_list[1].args[3], (directory / "ota_1.bin").resolve())


class BackupTest(unittest.TestCase):
    def test_backs_up_every_partition_that_the_probe_can_modify(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "backup"
            args = SimpleNamespace(
                directory=directory,
                port="/dev/cu.test",
                baud=115200,
                data_label="voicelife",
                probe_slot="ota_1",
                expected_data_size=0x200000,
            )

            def fake_read_flash(_port, _baud, offset, size, destination, **_kwargs):
                if offset == probe.PARTITION_TABLE_OFFSET:
                    destination.write_bytes(partition_table_bytes())
                elif offset == 0xE00000:
                    destination.write_bytes(b"data".ljust(size, b"\0"))
                elif offset == 0x410000:
                    destination.write_bytes(b"slot!".ljust(size, b"\0"))
                elif offset == 0xD000:
                    destination.write_bytes(b"ota".ljust(size, b"\0"))
                else:
                    self.fail(f"unexpected read offset {offset:#x} size={size}")

            with mock.patch("builtins.print"), mock.patch.object(
                probe, "read_flash", side_effect=fake_read_flash
            ), mock.patch.object(probe, "verify_flash"):
                probe.backup(args)

            manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(set(manifest["artifacts"]), {"data", "probe_slot", "otadata"})
            self.assertTrue((directory / "ota_1.bin").read_bytes().startswith(b"slot!"))


class WriteProbeTest(unittest.TestCase):
    def test_requires_exact_confirmed_probe_slot(self):
        args = SimpleNamespace(
            yes=True,
            confirm_inactive_slot="ota_0",
            probe_slot="ota_1",
            backup_directory=Path("/does/not/matter"),
            binary=Path("/does/not/matter.bin"),
            port="/dev/cu.test",
            baud=115200,
        )

        with self.assertRaisesRegex(probe.ProbeError, "exact --confirm-inactive-slot"):
            probe.write_probe(args)

    def test_refuses_to_write_if_the_partition_table_changed_after_backup(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_backup_manifest(directory)
            image = directory / "probe.bin"
            image.write_bytes(b"app")
            args = SimpleNamespace(
                yes=True,
                confirm_inactive_slot="ota_1",
                probe_slot="ota_1",
                backup_directory=directory,
                binary=image,
                port="/dev/cu.test",
                baud=115200,
            )

            def changed_table(_port, _baud, _offset, _size, destination, **_kwargs):
                destination.write_bytes(b"changed".ljust(probe.PARTITION_TABLE_SIZE, b"\xff"))

            with mock.patch.object(probe, "read_flash", side_effect=changed_table), mock.patch.object(
                probe, "esptool"
            ) as esptool, tempfile.TemporaryDirectory() as idf:
                otatool = Path(idf) / "components" / "app_update" / "otatool.py"
                otatool.parent.mkdir(parents=True)
                otatool.write_text("# test", encoding="utf-8")
                with mock.patch.dict(probe.os.environ, {"IDF_PATH": idf}, clear=True):
                    with self.assertRaisesRegex(probe.ProbeError, "partition table changed"):
                        probe.write_probe(args)

            esptool.assert_not_called()

    def test_refuses_missing_idf_path_before_flash_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            table_path = write_backup_manifest(directory)
            image = directory / "probe.bin"
            image.write_bytes(b"app")
            args = SimpleNamespace(
                yes=True,
                confirm_inactive_slot="ota_1",
                probe_slot="ota_1",
                backup_directory=directory,
                binary=image,
                port="/dev/cu.test",
                baud=115200,
            )

            def current_table(_port, _baud, _offset, _size, destination, **_kwargs):
                destination.write_bytes(table_path.read_bytes())

            with mock.patch.dict(probe.os.environ, {}, clear=True), mock.patch.object(
                probe, "read_flash", side_effect=current_table
            ), mock.patch.object(probe, "esptool") as esptool:
                with self.assertRaisesRegex(probe.ProbeError, "IDF_PATH is not set"):
                    probe.write_probe(args)

            esptool.assert_not_called()


class ManifestTest(unittest.TestCase):
    def test_rejects_manifest_without_probe_slot_backup(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_backup_manifest(directory)
            manifest_path = directory / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            del manifest["artifacts"]["probe_slot"]
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(probe.ProbeError, "probe_slot"):
                probe.load_manifest(directory)

    def test_rejects_partition_metadata_that_disagrees_with_backed_up_table(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_backup_manifest(directory)
            manifest_path = directory / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["artifacts"]["data"]["partition"]["offset"] = 0xA00000
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(probe.ProbeError, "partition metadata mismatch"):
                probe.load_manifest(directory)

    def test_rejects_symlinked_backup_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            write_backup_manifest(directory)
            target = directory / "outside.bin"
            target.write_bytes(b"outside")
            artifact = directory / "manifest.json"
            manifest = json.loads(artifact.read_text(encoding="utf-8"))
            (directory / "voicelife.bin").unlink()
            (directory / "voicelife.bin").symlink_to(target)
            manifest["artifacts"]["data"]["sha256"] = probe.sha256(target)
            artifact.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(probe.ProbeError, "must not be a symlink"):
                probe.load_manifest(directory)


if __name__ == "__main__":
    unittest.main()
