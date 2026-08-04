"""SQLite 实板探针串口监控与硬件复位。"""

from __future__ import annotations

import time

try:
    from scripts.sqlite_board_probe_protocol import ProbeError, ResetSequence
except ModuleNotFoundError:
    from sqlite_board_probe_protocol import ProbeError, ResetSequence


def reset_via_en(device) -> None:
    """通过 USB-UART 的 EN 线注入一次硬件复位。"""

    device.rts = True
    time.sleep(0.15)
    device.rts = False


def monitor(args) -> None:
    """监视探针输出，并在约定阶段之间执行 EN 复位。"""

    try:
        import serial
    except ImportError as error:
        raise ProbeError(
            "pyserial is required; use the ESP-IDF Python environment"
        ) from error

    device = serial.Serial(port=None, baudrate=args.baud, timeout=0.1)
    device.dtr = False
    device.rts = False
    device.port = args.port
    device.open()
    sequence = ResetSequence()
    deadline = time.monotonic() + args.timeout
    buffered = bytearray()
    try:
        while time.monotonic() < deadline:
            chunk = device.read(device.in_waiting or 1)
            if not chunk:
                continue
            buffered.extend(chunk)
            while b"\n" in buffered:
                raw_line, _, remainder = buffered.partition(b"\n")
                buffered = bytearray(remainder)
                line = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                print(line, flush=True)
                action = sequence.observe(line)
                if action == "reset":
                    point = sequence.reset_points[-1]
                    print(f"HOST_ACTION: EN_RESET point={point}", flush=True)
                    reset_via_en(device)
                elif action == "pass":
                    print(
                        f"HOST_RESULT: PASS resets={','.join(sequence.reset_points)}",
                        flush=True,
                    )
                    return
    finally:
        device.close()
    raise ProbeError(f"probe timeout after reset points {sequence.reset_points}")
