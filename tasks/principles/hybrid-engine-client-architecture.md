# Hybrid engine and WoW client architecture

User choice: 2026-11-05. Applies to WoWee module boundaries and repository
organization. The user endorsed this direction; this does not request an
immediate repository-wide migration or fix every proposed directory name.

## Rule

Separate technical engine facilities (Foundation, Math, Platform, Assets,
Collision, Rendering, Audio) from WoW protocol, domain contexts and
presentation/UI. Keep client startup, lifetime management and dependency
wiring in Client, and authoring/inspection applications in Tools.

Preserve the existing bounded contexts and their ownership of state and
rules. Engine facilities do not depend on WoW or Client. Client wires the
implementations to the interfaces their consumers require.

## Reason

Technical facilities should be usable and testable independently of the
game client, while WoW-specific rules remain organized by their domain.
The directory layout should make these responsibilities visible and agree
with actual include and build dependencies.

## Practical checklist

- Identify the owner and intended layer before adding or moving code.
- Align public headers, CMake targets and dependency checks with boundaries.
- Preserve one producer for each resource and each piece of derived data.
- Keep domain tests independent; retain protocol and client integration tests
  for behaviour that crosses those boundaries.
- Treat ECS as a separate design decision, not a requirement of this layout.
- Attribute build improvements to reduced dependencies or avoided work;
  moving files alone does not establish a speedup.
- Preserve original gameplay behaviour and the Lua/FrameXML contract.
