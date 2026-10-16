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

and, in GameHandler itself, the two ways state and notifications still go
around the domains rather than through them:

  * the xxxRef() accessors - a member handed out writable, so a handler edits
    GameHandler's state instead of owning its own - and the calls to them
    through owner_ in src/game;
  * the set*Callback setters - a notification wired to one listener, where
    the bus would let anyone hear it (AGENTS.md, "How the parts talk").

sweep_guard holds all of them where they stood; they only go down, as
handlers move to narrow dependencies and their state moves out of GameHandler.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# A constructor whose first (or only) parameter is the whole GameHandler.
CTOR = re.compile(r"^\s*(?:explicit\s+)?(\w+)\s*\(\s*GameHandler\s*&", re.M)
OWNER = re.compile(r"\bowner_\s*(?:\.|->)")
# A member function named ...Ref taking nothing, declared or defined.
REF_ACCESSOR = re.compile(r"\b(\w+Ref)\s*\(\s*\)\s*(?:const\s*)?(?:\{|;)")
REF_CALL = re.compile(r"\bowner_\s*(?:\.|->)\s*\w+Ref\s*\(\s*\)")
CALLBACK_SETTER = re.compile(r"\bvoid\s+(set\w*Callback)\s*\(")


def strip_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def main() -> int:
    handlers = []
    for path in sorted((ROOT / "include/game").rglob("*.hpp")):
        if path.name == "game_handler.hpp":
            continue
        for name in CTOR.findall(path.read_text(errors="ignore")):
            handlers.append((name, path.relative_to(ROOT).as_posix()))

    reaches = {}
    refCalls = 0
    for path in sorted((ROOT / "src/game").rglob("*.cpp")):
        text = path.read_text(errors="ignore")
        n = len(OWNER.findall(text))
        if n:
            reaches[path.relative_to(ROOT).as_posix()] = n
        refCalls += len(REF_CALL.findall(strip_comments(text)))

    header = ROOT / "include/game/game_handler.hpp"
    if not header.exists():
        print(f"No {header.relative_to(ROOT).as_posix()}. The zeros below mean the scan broke.")
        return 1
    gameHandler = strip_comments(header.read_text(errors="ignore"))
    refAccessors = sorted(set(REF_ACCESSOR.findall(gameHandler)))
    callbackSetters = sorted(set(CALLBACK_SETTER.findall(gameHandler)))

    print(f"{len(handlers)} domain handlers built from GameHandler&:")
    for name, rel in handlers:
        print(f"  {name:24} {rel}")
    print(f"\n{sum(reaches.values())} reaches through owner_ in src/game:")
    for rel, n in sorted(reaches.items(), key=lambda kv: -kv[1]):
        print(f"  {n:5}  {rel}")
    print(f"\n{len(refAccessors)} xxxRef() accessors in GameHandler")
    print(f"{refCalls} calls to them through owner_ in src/game")
    print(f"{len(callbackSetters)} set*Callback setters in GameHandler")
    return 0


if __name__ == "__main__":
    sys.exit(main())
