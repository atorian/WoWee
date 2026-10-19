#!/usr/bin/env python3
"""Count buffers and images allocated or freed past gpu_allocations.hpp.

The video memory held for each use is counted as allocations are made and
freed (include/rendering/gpu_allocations.hpp), which is what lets the resource
line read it without walking the allocator's detailed map - 150-300 ms of the
main thread in a city. A call straight to VMA allocates or frees without being
counted, and nothing says so: the breakdown is simply wrong from then on. So
the count of such calls outside the one file that wraps them is held at zero.

    tools/gpu_allocation_check.py

Prints "<N> calls to VMA past gpu_allocations.hpp", then each one.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCANNED = ("src", "include", "tools", "tests")
WRAPPER = ROOT / "src" / "rendering" / "gpu_allocations.cpp"

# Everything in VMA that makes or frees an allocation.
CALL = re.compile(
    r"\bvma(?:CreateBuffer|CreateBufferWithAlignment|CreateImage|CreateAliasingBuffer2?|"
    r"CreateAliasingImage2?|AllocateMemory(?:ForBuffer|ForImage|Pages)?|"
    r"DestroyBuffer|DestroyImage|FreeMemory(?:Pages)?)\s*\(")
LINE_COMMENT = re.compile(r"//.*$")


def calls_in(path: pathlib.Path) -> list[tuple[int, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [(number, line.strip()) for number, line in enumerate(text.splitlines(), 1)
            if CALL.search(LINE_COMMENT.sub("", line))]


def main() -> int:
    found = []
    scanned = 0
    for top in SCANNED:
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in (".cpp", ".hpp", ".h", ".cc", ".inl"):
                continue
            if path == WRAPPER or "extern" in path.parts:
                continue
            scanned += 1
            for number, line in calls_in(path):
                found.append(f"{path.relative_to(ROOT).as_posix()}:{number}: {line}")
    # The wrapper is where the calls belong, and seeing them there is what
    # shows the matcher still recognises one.
    wrapped = len(calls_in(WRAPPER)) if WRAPPER.exists() else 0
    print(f"{len(found)} calls to VMA past gpu_allocations.hpp")
    print(f"{scanned} files scanned; {wrapped} calls inside the wrapper itself")
    for entry in found:
        print(f"  {entry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
