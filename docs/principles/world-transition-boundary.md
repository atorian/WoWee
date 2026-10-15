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
- Loading is an ordinary `LOADING_WORLD` frame. New entries end the current
  `MapScope` (cancelling and joining its preparation) and build the next one in
  Transitions; disconnect/logout/exit end it through the same queue. Nothing
  per-map is cleared by hand.
- The map belongs to the character: `core::CharacterSession` (the interface,
  its Lua state and `InterfaceState`, the spell presentation, the per-character
  frame state, and the `MapScope`) is created at the first map entry after a
  login and destroyed by logout/disconnect/exit, in Transitions. A map change
  replaces only its `MapScope`. The interface hears `PLAYER_LEAVING_WORLD` in
  the Interface phase before (`leaveWorldInterface`); the destructor writes the
  saved variables under the character's name and closes the Lua state inside
  an Interface scope. Nothing per-character is cleared by hand either.
  Never pump a second frame or apply a transition from `GameHandler::update`.
- Test packet input through `GameHarness`: state survives `pump()` and changes
  only when the client issues the transition command afterward.
