#!/usr/bin/env python3
"""Domain handlers built from the whole GameHandler.

    tools/domain_shape_check.py

WHY

A domain handler is meant to be built from what it uses - a PacketSender to
send on, the FrameXmlEvents channel, core::EventBus, the query interfaces it
asks - and nothing else (AGENTS.md, "A new domain handler"). The older ones
take GameHandler& and reach through it with owner_. for anything at all:
the socket, another handler's state, the facade that forwards to it. Each
such reach is a dependency nobody declared, and a new handler written by
copying an old one inherits all of them.

WHAT IT DOES

Counts, in the game layer:

  * the handler classes in include/game whose constructor takes GameHandler&;
  * the reaches through owner_ in src/game.

sweep_guard holds both where they stood when the check was written; they only
go down, as handlers move to narrow dependencies.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# A constructor whose first (or only) parameter is the whole GameHandler.
CTOR = re.compile(r"^\s*(?:explicit\s+)?(\w+)\s*\(\s*GameHandler\s*&", re.M)
OWNER = re.compile(r"\bowner_\s*(?:\.|->)")


def main() -> int:
    handlers = []
    for path in sorted((ROOT / "include/game").rglob("*.hpp")):
        if path.name == "game_handler.hpp":
            continue
        for name in CTOR.findall(path.read_text(errors="ignore")):
            handlers.append((name, path.relative_to(ROOT).as_posix()))

    reaches = {}
    for path in sorted((ROOT / "src/game").rglob("*.cpp")):
        n = len(OWNER.findall(path.read_text(errors="ignore")))
        if n:
            reaches[path.relative_to(ROOT).as_posix()] = n

    print(f"{len(handlers)} domain handlers built from GameHandler&:")
    for name, rel in handlers:
        print(f"  {name:24} {rel}")
    print(f"\n{sum(reaches.values())} reaches through owner_ in src/game:")
    for rel, n in sorted(reaches.items(), key=lambda kv: -kv[1]):
        print(f"  {n:5}  {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
