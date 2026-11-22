#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report linked STM32 memory use and compiler-reported stack frames."""

import argparse
from pathlib import Path
import subprocess


def command(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--size", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--stack-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    sizes = command(args.size, "--format=berkeley", args.elf).splitlines()[1].split()
    text, data, bss = map(int, sizes[:3])
    symbols = {}
    for line in command(args.nm, "--defined-only", "--format=posix", args.elf).splitlines():
        name, _, value, *_ = line.split()
        symbols[name] = int(value, 16)
    flash = text + data
    ram = data + bss
    stack = symbols["__stack_size"]
    flash_limit = symbols["__flash_size"]
    ram_limit = symbols["__ram_size"]
    if flash > flash_limit or ram + stack > ram_limit:
        raise ValueError("Firmware exceeds the flash/RAM budget")

    frames = []
    for path in sorted(args.stack_dir.rglob("*.su")):
        for line in path.read_text().splitlines():
            name, size, kind = line.rsplit("\t", 2)
            frames.append((int(size), kind, name))
    if not frames:
        raise ValueError("No compiler stack-usage records found")

    lines = [
        "STM32F072xB / Cortex-M0 / integer-only RC core",
        f"Flash (.text + .data): {flash} / {flash_limit} bytes",
        f"Static RAM (.data + .bss): {ram} / {ram_limit} bytes",
        f"Reserved stack: {stack} bytes",
        f"RAM remaining after static data and stack: {ram_limit - ram - stack} bytes",
        "",
        "Largest compiler-reported stack frames:",
    ]
    for size, kind, name in sorted(frames, reverse=True)[:8]:
        lines.append(f"  {size} bytes ({kind}): {name}")
    lines += [
        "",
        "Stack frames are not a worst-case call-chain bound.",
        "Library frames, exception entry and hardware measurements are not included.",
    ]
    report = "\n".join(lines) + "\n"
    args.output.write_text(report)
    print(report, end="")


if __name__ == "__main__":
    main()
