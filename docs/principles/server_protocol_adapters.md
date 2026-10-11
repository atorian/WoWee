# Reusable server protocol adapters

Chosen by the user on 2026-10-11 for WoWee server compatibility.

Apply when a private realm requires a proprietary client extension or additional
messages during login/gameplay. Implement shared transport, lifecycle and bounded
parsing in WoWee, with a separate adapter for each server's verified protocol.
Prefer implementing the required protocol in client code over making native WoW
DLLs a mandatory dependency. Keep native libraries as an explicit compatibility
option when a verified interface is available.

This lets the same client support additional servers without scattering each
server's assumptions through authentication, Warden and rendering. Shared transport
does not imply that arbitrary undocumented server protocols work automatically.

Before declaring support:
- Obtain an authorized reference exchange and identify messages and state changes.
- Parse lengths and fields defensively; isolate endpoint/build-specific behavior.
- Reset protocol state, queues and dynamic memory when the endpoint changes.
- Test required callbacks and failure paths, then verify a real character list.
- Record unsupported messages explicitly; do not replace runtime behavior with
  captured integrity bytes or invent successful responses.
