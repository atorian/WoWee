#!/usr/bin/env python3
"""Inspect locally recorded extension frames; never dump auth/key traffic."""
import argparse
from pathlib import Path
import struct
parser = argparse.ArgumentParser()
parser.add_argument("trace", type=Path)
parser.add_argument("--extract", type=Path, help="save selected frames for disassembly")
args = parser.parse_args()
world_channels = {0x41D,0x23D,0x2D4,0x2E0,0x2E8,0x2E9,0x2EC,0x2ED,0x4C7,0x14B,0x4DE,0x4E0,0x4E4,0x4E5,0x4E6,0x4E1,0x4E8}
raw = args.trace.read_bytes()
if len(raw) > 8 * 1024 * 1024 or raw[:4] != b"WPT1":
    raise SystemExit("Invalid trace header/size")
pos = 4
count = 0
while pos < len(raw):
    if len(raw) - pos < 12:
        raise SystemExit("Truncated trace record")
    channel, opcode, size = struct.unpack_from("<III", raw, pos)
    pos += 12
    if size > 1024 * 1024 or size > len(raw) - pos:
        raise SystemExit("Invalid trace record length")
    body = raw[pos:pos+size]
    pos += size
    allowed = ((channel == 1 and opcode == 0x2E6 and body and body[0] in (2, 3))
               or (channel == 2 and opcode in world_channels))
    if not allowed:
        raise SystemExit("Trace contains traffic outside the extension allowlist")
    count += 1
    print(f"{count}: channel={channel} opcode=0x{opcode:X} length={size}")
    if args.extract:
        args.extract.mkdir(parents=True, exist_ok=True)
        (args.extract / f"frame-{count:03}-{channel}-{opcode:x}.bin").write_bytes(body)
    if channel == 1 and body[0] == 2:
        end = 1
        strings = []
        while end < len(body):
            length = body[end]
            end += 1
            if not length:
                break
            if length > len(body) - end:
                raise SystemExit("Truncated Warden string table")
            strings.append(body[end:end+length])
            end += length
        for i, value in enumerate(strings):
            print(f"  string[{i+1}] length={len(value)}: {value!r}")
        print(f"  checks: {body[end:].hex()}")
    elif channel == 1:
        print(f"  initialize: {body.hex()}")
    elif opcode == 0x41D:
        if len(body) < 4:
            print("  missing extension command")
            continue
        command = struct.unpack_from("<I", body)[0]
        print(f"  command={command}")
        if command != 0:
            print(f"  command data: {body[4:260].hex()}")
            continue
        if len(body) < 12:
            print("  truncated executable envelope")
            continue
        offset = struct.unpack_from("<I", body, 4)[0]
        if offset > len(body) - 12:
            print("  invalid executable offset")
            continue
        length = struct.unpack_from("<I", body, 8+offset)[0]
        print(f"  arguments={offset} code={length} available={len(body)-12-offset}")
    else:
        print(f"  registered world channel: {body[:256].hex()}" +
              (" ..." if len(body) > 256 else ""))
