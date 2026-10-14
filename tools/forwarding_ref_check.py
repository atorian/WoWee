#!/usr/bin/env python3
"""Writable accessors that reach round the forwarding their readers go through.

    tools/forwarding_ref_check.py

WHY

GameHandler was split into per-subject handlers, and what is left of it forwards:
every getter asks the sub-handler when there is one and only falls back to its
own member when there is not. In practice the sub-handler is always there, so
the member behind the fallback is never written and never read.

Next to those getters sit accessors that hand out a writable reference, for the
few places that edit a list in place rather than replacing it. Those were left
returning the member directly. A caller then writes to a list that nothing
displays, and the write is not lost in a way anybody can see: no error, no log
line, the panel simply never changes.

Two were live when this was written. Sorting an auction house column reordered
GameHandler's empty copy while the rows on screen came from the inventory
handler's, so clicking a column header did nothing. The backfill that fills in
a mail's sender once the name packet arrives wrote to the same empty copy, so
mail from a player whose name had not yet resolved kept a blank sender.

WHAT IT DOES

The handlers now always exist, so the fallback branch is gone and with it the
first way to read the wrong copy. What is left to look for:

  * a fallback branch coming back - `xHandler_ ? ... : ...` or
    `if (xHandler_)` on a handler that is never null (AGENTS.md forbids them);
  * a member handed out by a writable Ref() whose getter forwards, and which
    nothing in GameHandler reads, so an edit through the reference is lost;
  * a writer that changes a member whose getter forwards.

WHAT IT CANNOT SEE

Forwarding written in some other shape than `return xHandler_->name(`, and the
same split in a class other than GameHandler. It also cannot tell a deliberate
local cache from a mistake, which is why it reports rather than judges.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "include/game/game_handler.hpp"
SOURCES = sorted((ROOT / "src/game").glob("game_handler*.cpp"))

# `auto& xRef() { return x_; }` - a writable reference to this class's member.
WRITABLE_REF = re.compile(r"(\w+Ref)\(\)\s*(?:const\s*)?\{\s*return (\w+_?);")

# A branch on whether a handler exists. They always do; the branch is either
# dead or the fallback to a stale copy this sweep was written about.
FALLBACK = re.compile(r"\b\w+Handler_\s*\?|\bif\s*\(\s*!?\s*\w+Handler_\s*\)")

# The getter a member is read through, as a forward: `return xHandler_->getX(`.
FORWARD = re.compile(r"\breturn\s+\w+Handler_->(\w+)\(")


def main():
    if not HEADER.exists() or not SOURCES:
        print("Found no game handler. The zero below means the scan broke.")
        return 1

    header = HEADER.read_text(errors="ignore")
    refs = {}
    for match in WRITABLE_REF.finditer(header):
        refs.setdefault(match.group(2), []).append(match.group(1))

    source = "".join(p.read_text(errors="ignore") for p in SOURCES)
    fallbacks = []
    for path in [HEADER, *SOURCES]:
        for n, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
            if FALLBACK.search(re.sub(r"//.*", "", line)):
                fallbacks.append(f"{path.relative_to(ROOT).as_posix()}:{n}: {line.strip()}")

    # The getters, inline in the header or defined in the sources, whose body
    # is a forward. Both are how a member's value is read.
    forwarded_getters = set(re.findall(
        r"\bGameHandler::(\w+)\([^)]*\)[^{;]*\{\s*return\s+\w+Handler_->", source))
    forwarded_getters |= set(re.findall(
        r"\b(\w+)\([^)]*\)\s*(?:const\s*)?\{\s*return\s+\w+Handler_->", header))
    forwarding = set(FORWARD.findall(header + source))

    if not refs or not forwarded_getters:
        print("Recognised no accessors at all. The zero below means the scan broke.")
        return 1

    # A second shape, and the one that got past the first. The pattern above
    # needs the forwarding getter to *fall back to the same member*, which is
    # the tidy case. getGossipPois forwarded to QuestHandler and fell back to a
    # `static const ... empty` instead - so gossipPois_ never appeared in the
    # fallback at all, and nothing here associated the two. That is worse
    # rather than better: the member is not merely a stale fallback, it is
    # never returned by anything.
    #
    # So: a member handed out by a writable Ref() whose getter forwards, and
    # which GameHandler's own sources never read. Something edits it through
    # the reference and nothing can ever see the edit. movement_handler cleared
    # the gossip points of interest that way, and the markers stayed on the map.
    unread = []
    for member in sorted(refs):
        stem = member.rstrip("_")
        getter = "get" + stem[:1].upper() + stem[1:]
        if getter not in forwarded_getters and getter not in forwarding:
            continue
        if re.search(r"[^\w.>]" + re.escape(member) + r"\b(?!\s*=[^=])", source):
            continue
        unread.append((member, getter))

    print(f"{len(refs)} member(s) handed out by a writable Ref()")
    print(f"{len(forwarded_getters)} getter(s) that forward to a sub-handler\n")
    print(f"{len(fallbacks)} fallback branch(es) on a handler that always exists:")
    for line in fallbacks:
        print(f"  {line}")
    if not fallbacks:
        print("  (none)")

    # A third shape: a *writer* that touches a member whose getter forwards,
    # without telling the sub-handler. The first two arms look at the member;
    # this one looks at the method, so it catches a writer whose member is read
    # elsewhere in GameHandler and therefore never appears as dead.
    #
    # resetDbcCaches was this. It is called when the active expansion changes
    # and cleared local copies of the talent and taxi caches, while the getters
    # beside them forward to SpellHandler and MovementHandler - so a switch left
    # the previous expansion's talents and flight points live, and the comment
    # above the call said the opposite.
    stale_writers = []
    for m in re.finditer(r"\bGameHandler::(\w+)\([^)]*\)[^{;]*\{(?:\.\w+\s*=\s*)?", source):
        depth, j, started = 0, m.end() - 1, False
        while j < len(source):
            if source[j] == "{":
                depth += 1
                started = True
            elif source[j] == "}":
                depth -= 1
                if started and depth == 0:
                    j += 1
                    break
            j += 1
        name, body = m.group(1), source[m.end():j]
        if name in forwarded_getters or "Handler_->" in body:
            continue
        for w in re.finditer(r"[^\w.>](\w+_)\s*(?:=[^=]|\.clear\(|\.push_back|\.insert)", body):
            member = w.group(1)
            if member.endswith("Handler_"):
                continue
            stem = member.rstrip("_")
            getter = "get" + stem[:1].upper() + stem[1:]
            if getter in forwarded_getters:
                stale_writers.append((name, member, getter))
                break

    print(f"\n{len(stale_writers)} writer(s) that change a member whose reader forwards:")
    if not stale_writers:
        print("  (none)")
    for name, member, getter in stale_writers:
        print(f"  {name}()")
        print(f"      writes {member}, but {getter}() forwards to a sub-handler "
              f"- the change is invisible to every reader")

    print(f"\n{len(unread)} member(s) edited through a reference nothing reads:")
    if not unread:
        print("  (none)")
    for member, getter in unread:
        print(f"  {member}")
        print(f"      handed out writable, while {getter}() forwards to a "
              f"sub-handler - the edit goes nowhere anything can see")
    return 0


if __name__ == "__main__":
    sys.exit(main())
