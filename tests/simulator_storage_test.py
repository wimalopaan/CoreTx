#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib


def run(executable, path, mode, success=True):
    result = subprocess.run(
        [executable, "--state", str(path), mode],
        capture_output=True,
        text=True,
        env={**os.environ, "SDL_VIDEODRIVER": "dummy"},
        timeout=10,
    )
    if (result.returncode == 0) != success:
        raise AssertionError(f"{result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="coretx-simulator-storage-") as directory:
        path = Path(directory) / "state.bin"
        run(sys.argv[1], path, "--calibration-smoke-test")
        committed = path.read_bytes()
        assert len(committed) == 1024
        edited = bytearray(committed)
        first_input = 16 + 2 + 8 * 8
        packed = int.from_bytes(edited[first_input:first_input + 7], "little")
        packed = (packed & ~(0xFF << 27)) | ((73 + 100) << 27)
        edited[first_input:first_input + 7] = packed.to_bytes(7, "little")
        # Format 10 appends curves/selectors, five-byte tank settings and packed roles.
        curve_extension_bytes = 4 * 8 + 8 + 8 + 5 + (8 + 3) // 4
        trim_offset = 16 + int.from_bytes(edited[6:8], "little") - 5 - curve_extension_bytes
        edited[trim_offset:trim_offset + 4] = bytes((17, 233, 0, 127))
        edited[12:16] = zlib.crc32(edited[:12] + edited[16:]).to_bytes(4, "little")
        committed = bytes(edited)
        path.write_bytes(committed)
        message = run(sys.argv[1], path, "--storage-smoke-test")
        assert "Loaded storage slot" in message
        assert path.read_bytes() == committed

        path.write_bytes(committed + committed[:100])
        message = run(sys.argv[1], path, "--storage-smoke-test")
        assert "slot 1: incomplete slot" in message
        repaired = path.read_bytes()
        assert len(repaired) == 2048 and repaired[:1024] == committed

        oversized = repaired + b"\x00"
        path.write_bytes(oversized)
        message = run(sys.argv[1], path, "--storage-smoke-test", success=False)
        assert "exceeds the two-slot format" in message
        assert path.read_bytes() == oversized

        corrupted = bytes(2048)
        path.write_bytes(corrupted)
        message = run(sys.argv[1], path, "--storage-smoke-test", success=False)
        assert "neither storage slot is valid" in message
        assert path.read_bytes() == corrupted

        future = bytearray(committed)
        future[4:6] = (int.from_bytes(committed[4:6], "little") + 1).to_bytes(2, "little")
        future[12:16] = zlib.crc32(future[:12] + future[16:]).to_bytes(4, "little")
        newer = bytes(future) + committed
        path.write_bytes(newer)
        message = run(sys.argv[1], path, "--storage-smoke-test", success=False)
        assert "unsupported storage version" in message
        assert path.read_bytes() == newer
    print("SDL storage integration tests passed")


if __name__ == "__main__":
    main()
