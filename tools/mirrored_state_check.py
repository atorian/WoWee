#!/usr/bin/env python3
"""A field declared both in GameHandler and in a domain handler, or in two
handlers.

    tools/mirrored_state_check.py

WHY

GameHandler was taken apart into domain handlers, and a field that moved often
left its old copy behind. The two then drift: one side writes its copy, the
other reads its own, and nothing fails - the reader simply sees a default.
Twelve were found at once on 2026-10-14, and most were live faults:

  * gcdTotal_: SpellHandler set it, GameHandler::getGCDRemaining read its own,
    so the interface's GCD sweep never showed.
  * arenaTeamRosters_: SocialHandler filled it, the Lua roster read and sorted
    GameHandler's empty one.
  * unitAurasCache_: party-stat auras were written into GameHandler's copy and
    read from SpellHandler's.
  * localLootState_, recentLootMoneyAnnounceCooldowns_, pendingQuestQueryIds_:
    tab targeting, the money line and the login quest resync each used the
    copy the other side never touched.

AGENTS.md: one copy of every piece of state. This holds it at zero.

WHAT IT CANNOT SEE

A second copy under a different name, and a mirror in a class outside these
headers. It matches members by name, so two handlers that each legitimately
keep a private `timer_` would be reported - rename one, which is clearer anyway.
"""
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADERS = ["game_handler", "chat_handler", "combat_handler", "inventory_handler",
           "movement_handler", "quest_handler", "social_handler", "spell_handler",
           "warden_handler", "entity_controller"]

# A data member: indented, a type, a name ending in `_`, an optional
# initialiser, a semicolon. Statements and declarations of functions do not
# end that way.
FIELD = re.compile(
    r"^\s+(?!return|using|friend|static\s+const)(?:mutable\s+)?[\w:<>,\s\*&]+?"
    r"\b(\w+_)\s*(?:=[^;]*|\{[^;]*\})?;\s*(?://.*)?$", re.M)

# The handlers each hold their owner and their siblings; those are references,
# not state.
NOT_STATE = ("Handler_", "owner_")


def is_dependency(declaration):
    """What a handler is built from, rather than state it keeps.

    A reference member (`PacketSender& sender_`) and a pointer fixed at
    construction (`AssetManager* const assets_`) cannot be written after the
    constructor, so there is no second copy for them to drift from - every
    handler built from narrow interfaces holds its own `sender_` and
    `frameXml_`, and that is the design (AGENTS.md, "A new domain handler").
    A bus subscription is the object's own registration and ends with it.
    """
    type_part = declaration.split("=")[0].split("{")[0]
    return ("&" in type_part or "* const" in type_part or
            "EventBus::Subscription" in type_part)


def main():
    owners = collections.defaultdict(set)
    for name in HEADERS:
        path = ROOT / "include/game" / f"{name}.hpp"
        if not path.exists():
            print(f"No {path.relative_to(ROOT).as_posix()}. The zero below means the scan broke.")
            return 1
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", path.read_text(errors="ignore"), flags=re.S)
        for match in FIELD.finditer(text):
            if match.group(1).endswith(NOT_STATE) or is_dependency(match.group(0)):
                continue
            owners[match.group(1)].add(name)

    mirrored = sorted((field, sorted(where)) for field, where in owners.items() if len(where) > 1)
    print(f"{len(owners)} fields across {len(HEADERS)} classes\n")
    print(f"{len(mirrored)} field(s) declared in more than one of them:")
    for field, where in mirrored:
        print(f"  {field:40} {', '.join(where)}")
    if not mirrored:
        print("  (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
