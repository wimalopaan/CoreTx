#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture or verify the unmodified EdgeTX function bodies used by host tests."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "tests" / "upstream"
FUNCTIONS = {
    "radio/src/edgetx_helpers.h": ["divRoundClosest"],
    "radio/src/edgetx.h": [
        "calc100to256",
        "calc100toRESX",
        "calc1000toRESX",
    ],
    "radio/src/mixer.cpp": [
        "expou",
        "expo",
        "applyExpos",
        "applyLimits",
        "evalTrims",
        "getStickTrimValue",
        "getSourceTrimOrigin",
        "getSourceTrimValue",
        "channel_bit",
        "channel_dirty",
        "upper_channels_mask",
        "evalFlightModeMixes",
    ],
}
WATCHED = [
    *FUNCTIONS,
    "radio/src/dataconstants.h",
    "radio/src/datastructs_private.h",
    "radio/src/myeeprom.h",
]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def extract(text, name):
    # Keep offsets while hiding braces/signatures inside comments and literals.
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: re.sub(r"[^\n]", " ", match.group()),
        text,
        flags=re.DOTALL,
    )
    signature = re.compile(
        rf"(?m)^[^\n;{{}}#]*\b{re.escape(name)}\([^\n;{{}}]*\)[ \t\r\n]*\{{"
    )
    matches = list(signature.finditer(masked))
    if len(matches) != 1:
        raise ValueError(f"{name}: expected one definition, found {len(matches)}")
    start = matches[0].start()
    # EdgeTX uses unindented closing braces for these top-level functions.
    # This also handles alternative #if/#else bodies with unbalanced raw braces.
    end = re.search(r"(?m)^}[ \t]*$", masked[matches[0].end():])
    if end is None:
        raise ValueError(f"{name}: top-level closing brace not found")
    stop = matches[0].end() + end.end()
    return text[start:stop] + "\n", text.count("\n", 0, start) + 1


def capture(source):
    revision = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    subprocess.run(
        ["git", "-C", str(source), "diff", "--quiet", "HEAD", "--", *WATCHED],
        check=True,
    )
    header = (
        "// SPDX-License-Identifier: GPL-2.0-or-later\n"
        "// Copyright (C) EdgeTX; based on OpenTX, th9x, er9x and gruvin9x.\n"
        "// Unmodified function bodies, captured by tools/sync_edgetx_reference.py.\n"
        f"// EdgeTX revision: {revision}\n"
        "// This file is a test fixture, never part of coretx.\n\n"
    )
    chunks = [header]
    symbols = []
    for path, names in FUNCTIONS.items():
        text = (source / path).read_text()
        for name in names:
            body, line = extract(text, name)
            chunks.append(f'#line {line} "{path}"\n{body}\n')
            symbols.append({"path": path, "name": name, "sha256": digest(body.encode())})
    snapshot = "".join(chunks).encode()
    manifest = {
        "repository": "https://github.com/wimalopaan/edgetx",
        "revision": revision,
        "snapshot_sha256": digest(snapshot),
        "functions": symbols,
        "watched_files": {path: digest((source / path).read_bytes()) for path in WATCHED},
    }
    return snapshot, manifest


def check(source):
    manifest = json.loads((DESTINATION / "manifest.json").read_text())
    snapshot = (DESTINATION / "mixer.inc").read_bytes()
    if digest(snapshot) != manifest["snapshot_sha256"]:
        raise ValueError("snapshot changed; recapture from a reviewed EdgeTX revision")
    text = snapshot.decode()
    for symbol in manifest["functions"]:
        body, _ = extract(text, symbol["name"])
        if digest(body.encode()) != symbol["sha256"]:
            raise ValueError(f"captured function changed: {symbol['name']}")
    if source is not None:
        changed = [
            path for path, expected in manifest["watched_files"].items()
            if digest((source / path).read_bytes()) != expected
        ]
        if changed:
            raise ValueError("EdgeTX source differs; review before updating:\n  " +
                             "\n  ".join(changed))
    print(f"EdgeTX reference verified: {manifest['revision']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    parser.add_argument("--source", type=Path, help="Local EdgeTX Git checkout")
    args = parser.parse_args()
    try:
        if args.write:
            if args.source is None:
                parser.error("--write requires --source")
            snapshot, manifest = capture(args.source.resolve())
            DESTINATION.mkdir(parents=True, exist_ok=True)
            (DESTINATION / "mixer.inc").write_bytes(snapshot)
            (DESTINATION / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print(f"Captured EdgeTX reference: {manifest['revision']}")
        else:
            check(args.source.resolve() if args.source is not None else None)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"EdgeTX reference error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
