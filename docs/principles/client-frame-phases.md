# One explicit client frame

Chosen for WoWee on 2026-10-15. Applies when adding per-frame client work.

`Application::runFrame` is the authoritative sequence: Input, Network, Game,
Events, Interface, Present, Render, Transitions, Pace. `framePlanFor(AppState)`
chooses the connection, simulation, world presentation and view for the frame.
Keep this an ordinary function; do not introduce a phase registration system.

GameHandler exposes separate network and simulation steps. Its combined
`update` remains available to callers such as GameHarness. Protocol polling
does not advance world simulation timers.

All interface actions and draw-list construction happen before Render.
Render records and submits graphics against the state already prepared.
Lua entry points check the client's current phase and reject Render calls.
World entry and teardown require Transitions. Initialization and shutdown
explicitly enter that phase too.

The single Pace phase uses the selected frame cap. With no explicit cap,
VSync is paced by presentation; do not add a fixed 60 Hz sleep. A lost mobile
surface selects a background cap through the same pacer.

Checklist:

- Preserve SDL, gamepad and Input sampling order.
- Separate structural extraction from changes in execution order.
- Keep Lua callbacks, UI buttons and widget mutation out of Render.
- Preserve the phase across nested loading work and exceptions.
- Put lifecycle mutations after submit, and all cap waits in Pace.
- Verify behavior with packet/Lua tests and compare frame profiles.

WorldLoader keeps its loading loop. Its network and game steps temporarily
mark their phases and restore Transitions; it never applies transitions from
inside a packet update. This does not introduce a shared background-work budget.
