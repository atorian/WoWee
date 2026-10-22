# Prepare interface resources before opening windows

Applies to interface window art, spell/item icons and macro icons in WoWee.

User choice (2026-10-22): begin background preparation of shared window resources
once authenticated at character selection. Character-dependent icons and macros
start during world entry. Queue all spellbook ranks, pet spell icons and held item icons,
including pages and bags that have never been opened.

Rule: the draw/layout path requests or reads cached resources; workers perform
file reads, decoding and upload preparation. Publish an image only after its GPU
copy completes. Use the common texture budget/cache and shared worker pool.
Do not replace background loading with a small per-frame synchronous allowance.

Reason: even one cold read, decode or fence wait can stall an entire frame.
Preparing closed panels and unseen icons removes that work from first use.

Checklist:
- Begin shared art discovery at character selection, without running character Lua.
- Defer character icons/macros until login, then continue after inventory/spell updates.
- Normalize file names and share resources between FrameXML, HUD and legacy panels.
- Keep pending separate from failed; log failures once and do not retry every frame.
- Bound outstanding worker/GPU work and give visible requests priority.
- Join outstanding work before changing datasets or destroying the device.
- Verify cold preparation and first opening with real assets and GPU validation.
