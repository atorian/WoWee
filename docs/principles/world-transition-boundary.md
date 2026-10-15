# Apply world transitions after frame submission

Applies to logout, disconnect, world entry, teleport and map transfer.
Chosen for WoWee on 2026-10-15.

Game handlers publish facts through `game_events.hpp`. Client subscribers
record transition requests; they do not unload the interface, release world
resources, load another map or start reconnecting from an event handler.
`Application::runFrame` applies requests in its Transitions phase after rendering
and submission.

The game and renderer may still hold references to the outgoing world while
processing packets or recording a frame. Synchronous event delivery does not
make those references safe to destroy. `NEW_WORLD` therefore also holds later
packets until the client applies its transfer at this boundary.

Checklist when changing a transition:

- Keep the event subscriber limited to recording a request.
- Apply teardown, entry and reconnect in the phase after submission.
- Let logout/disconnect/exit supersede pending world entries.
- Loading is an ordinary `LOADING_WORLD` frame. New entries cancel or supersede
  pending preparation in Transitions; disconnect/logout/exit use the same queue.
  Never pump a second frame or apply a transition from `GameHandler::update`.
- Test packet input through `GameHarness`: state survives `pump()` and changes
  only when the client issues the transition command afterward.
