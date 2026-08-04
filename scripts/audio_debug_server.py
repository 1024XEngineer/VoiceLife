#!/usr/bin/env python3
"""Capture raw PCM sent over UDP and save it as a WAV file.

Adapted from 78/xiaozhi-esp32 scripts/audio_debug_server.py (MIT). The
VoiceLife version makes bind address, port, format, and output explicit.
"""

from __future__ import annotations

import argparse
import socket
import wave
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument("--channels", type=int, choices=(1, 2), default=1)
    parser.add_argument("--sample-width", type=int, choices=(1, 2, 3, 4), default=2)
    parser.add_argument("--output", type=Path, default=Path("audio-debug.wav"))
    args = parser.parse_args()

    received = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind((args.bind, args.port))
        with wave.open(str(args.output), "wb") as output:
            output.setnchannels(args.channels)
            output.setsampwidth(args.sample_width)
            output.setframerate(args.sample_rate)
            print(f"监听 udp://{args.bind}:{args.port}，按 Ctrl+C 停止")
            try:
                while True:
                    packet, _ = server.recvfrom(65535)
                    output.writeframesraw(packet)
                    received += len(packet)
            except KeyboardInterrupt:
                pass
    print(f"已写入 {args.output}，PCM 数据 {received} 字节")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
