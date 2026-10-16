# Reconnecting to the world server without losing the map

A dropped world connection used to end the character: `Disconnected` queued a
leave, `ClientLoop` destroyed the `CharacterSession` and its `MapScope` and went
to the login screen, and `UIScreenCallbackHandler::beginReconnect` logged in
again with the saved password hash and loaded the map from nothing.

## Status (2026-10-16)

Done on `claude/reconnect-keep-map-59ea34`, three commits, ready to merge:

- `201d507` the login server's reconnect handshake (`AuthHandler::reconnect`);
- `eed967c` the game: silence detector, refusals while the connection is gone,
  the last login kept, the party ended at each login;
- `2177a93` + the follow-up commit: the client rides the loss out in the world.

Build: `ninja -C build-dev` clean, no warnings. `ctest`: 257 of 258 pass,
`sweep_guard` included with no ceiling raised (`domain_shape_check` reaches
went 4213 -> 4209). The one failure is `m2_billboard_model`, which SKIPs (and
so fails) in a worktree whose `Data/` has no extracted WotLK assets; it fails
the same way on the base commit here and passes in the main checkout.

## Who owns what

| Piece | Owner | Layer |
|---|---|---|
| Proving the last session's key (`RECONNECT_CHALLENGE`/`PROOF`) | `auth::AuthHandler::reconnect` | auth |
| Where the realm is now (realm list after the proof) | `core::LoginServerRenewal` over `AuthHandler` (Application); a scripted stand-in in `ClientHarness` | client |
| The last world login (host, port, key, account, build, realm id) | `GameHandler::lastWorldLogin()` / `connect(WorldLogin)` | game (session) |
| Losing the connection: socket error, failure in the world, or silence with an unanswered ping | `GameHandler::loseWorldConnection` publishes `Disconnected` | game (session) |
| Refusing commands that need the server while it is gone | `GameHandler::worldCommandAllowed`, folded into each command's existing in-world check | game |
| What a login resets | the ordinary login path (`disconnect` + `selectCharacter`); domains holding server-sent state end it on `events::CharacterLoggingIn` | game |
| Attempts, back-off, deadlines | `core::WorldReconnect` | client |
| Keeping the world, the session swap, placing the player | `ClientLoop` (Transitions), `WorldEntryCallbackHandler::placeAfterReconnect` | client |
| What is drawn of the entities across the gap | `EntitySpawner::holdPresented/shown/releaseHeld`, state in the map's `SpawnState` | client |

## The flow

1. **Lost.** `Disconnected` is a `Leave{Reconnect}`. In Transitions, when a
   character stands on a map (`IN_GAME` or `LOADING_WORLD`) and the client has a
   renewal, `ClientLoop` keeps the world: the spawner holds everything it
   presents, `game.disconnect()` drops the connection, Warden and the game's
   world state exactly as a logout does (its despawns reach held entities and
   remove nothing), and a `WorldReconnect` starts. The leave does not count as
   the world ending (`ClientLoop::worldEnding`), so the interface is not told
   the character left and keeps its key bindings: movement goes on.
2. **Reconnecting.** Each attempt renews the session at the login server (no
   password), connects to the world server at the realm's current address,
   waits for the character list and logs the same character in through
   `selectCharacter`. Deadlines: renewal 20 s, greeting 15 s, character list
   60 s (none in a login queue), entering 30 s. Attempts start 5 s x n after
   the failed one started (an attempt that spent its time timing out goes
   again at once). Five attempts. "Connection lost. Reconnecting (attempt N of
   5)..." is drawn over the world. Casts, item use, loot, trade, chat, NPCs and
   objects are refused at once with "No connection to the server." and nothing
   is queued; movement is local and not sent (said once per stretch).
3. **Back in.** `SMSG_LOGIN_VERIFY_WORLD` publishes `WorldEntered{initial}`.
   The old interface hears `PLAYER_LEAVING_WORLD` in that frame's Interface
   phase; in Transitions:
   - same map, `IN_GAME`: the next `CharacterSession` takes the `MapScope`
     (`releaseMap`/`adoptMap`), the old session ends (saved variables written,
     Lua closed), the player is placed at the server's position like an in-map
     teleport and the distance walked is logged (WARNING over 0.5 yd), and the
     new interface loads in the next Interface phase as at a login. Held
     entities the server shows again keep their instances; three seconds after
     the entry the rest are released.
   - another map, or a load that never finished: the old session ends and the
     entry is an ordinary map load.
4. **Given up.** A refusal from the login server, the character missing from
   the list, or the fifth attempt failing: `Leave{Login}` with the reason; the
   interface hears the character leave and the client goes to the login
   screen as a disconnect always did.

## The silence detector

In the world, ten seconds with no packet sends a ping; ten more with no answer
is a lost connection (ERROR with the length of the silence). Only time spent
listening counts: a frame counts for at most 0.25 s, so a load holding the
frame is not silence, and nothing counts between `SMSG_NEW_WORLD` and the
client's acknowledgement. It replaces the old wall-clock "RX SILENCE" warnings,
which only logged.

## Removed

The credential-based reconnect: `UIScreenCallbackHandler`'s reconnect rounds,
`AuthScreen::requestReconnect`, its status context and `hasCredentials`,
`CharacterScreen::enterWorldWhenListed`, and `ClientFrontEnd::beginReconnect`.
The realm address parsing the realm screen shared with it is
`core::resolveRealmAddress`, used by both connects.

## Verified (ctest)

- `test_auth_reconnect`, the real `AuthHandler` and TCP socket against a login
  server on the loopback interface: the challenge names the account, the proof
  is `R2 = SHA1(account, R1, challenge, K)`, success is AUTHENTICATED with the
  same key and the realm list can be asked; an error answer to the challenge or
  the proof, and the connection closed on the proof, are refusals with the
  reason; an unreachable server is not a refusal.
- `test_session_domain` (`GameHarness`): silence past an unanswered ping is
  lost once; a quiet server that answers, three 30 s frames, and a pending map
  transfer are not; while lost, cast/chat/loot/NPC are refused and told, and
  none is sent after logging in again; after a reconnect no cooldown, aura,
  party or backpack item of the last session is left (the party one failed
  before the fix - it also survived a relog to another character).
- `test_client_transitions` (`ClientHarness`):
  - lost in the world, back on the same map: the same `MapScope` object, never
    a `LOADING_WORLD` frame, the creature the server shows again presented in
    every frame, the one it does not removed after the hold, the interface told
    `PLAYER_LEAVING_WORLD` once and loaded anew with none of its Lua state;
  - back on another map: an ordinary load onto map 1;
  - every attempt failing: five renewals, then the login screen with nothing of
    the world left; a refused renewal: the login screen after one;
  - a timed-out attempt is followed by the next at once;
  - the server gone silent with the socket open: reconnecting, same map;
  - walking while reconnecting moves the character; after the login it stands
    where the server has it, drawn and in the game's movement info;
  - a cast while reconnecting is refused in the interface and never sent.
  With the harness given no renewal (the old behaviour) all reconnect cases
  fail; with the spawner's hold disabled the creature checks fail; with the
  old end-to-start pause the timed-out attempt case fails.
- Native client (`build-dev\bin\wowee.exe`): starts to the login screen, no
  errors. Not logged in: that needs the player's account on their server.

Found on the way: `ClientHarness` destroyed the spawner before the game, whose
destructor's `disconnect()` then despawned into it - a crash one run in thirty
when a test ended with creatures in view. Its destructor now ends the game
session first, as `Application::shutdown` does.

## Not measured, and what to watch

- The live case: no real login server or world server was used. In
  particular the login server's answer to `RECONNECT_PROOF` on the user's
  realm, and whether the world server lets the account in again while it still
  holds the dropped session (TrinityCore kicks the old one; the retries cover
  an "already logged in" in between).
- Transports: the game's logical transports survive a same-map reconnect (the
  map scope, which clears them, stays); the server's creates for them are
  registered again. Not exercised by a test.
- Pre-TBC login servers answer a successful reconnect proof with two bytes,
  not four; the socket frames four for success, so on those servers the
  renewal times out (20 s) and the attempt fails. WotLK and TBC are fine.
- `ClientHarness` waits on world loads by frame count while the loader's map
  preparation runs on real worker threads, so a heavily loaded machine can
  time a world entry out (seen once while another test process was hung). Not
  new; worth a frame-clocked loader stand-in.
- One run of `test_client_transitions` hung at process exit after every case
  passed, in the same contended period; 40 later runs did not. The static
  thread pools are joined at exit; not investigated further.

### Measuring "network down for 5 s -> the character under control in under 3 s"

Movement never stops during a reconnect, so what to time is how long after the
network returns the world answers again (server-confirmed: a cast goes out,
NPCs answer).

1. Run `build-dev\bin\wowee.exe` from `build-dev\bin` with
   `WOWEE_LOG_LEVEL=info`, log in and stand somewhere.
2. Cut the network for 5 s. Either keeps the client running:
   - as administrator: `Disable-NetAdapter -Name "<adapter>" -Confirm:$false;
     Start-Sleep 5; Enable-NetAdapter -Name "<adapter>"` (Windows usually
     resets the connection: the socket path), or
   - a firewall rule blocking `wowee.exe` for 5 s
     (`New-NetFirewallRule -DisplayName wowee-cut -Program <path>\wowee.exe
     -Direction Outbound -Action Block`, then `Remove-NetFirewallRule
     -DisplayName wowee-cut`). TCP usually survives a 5 s block, so expect no
     reconnect at all: that is a pass.
3. Read `build-dev\bin\logs\wowee.log`: the times of `World connection lost`,
   each `Reconnecting to the world server: attempt N`, `Reconnected after X s`
   and `Back in the world on map ...`. The figure is `Back in the world` minus
   the moment the network came back; under 3 s is the target. Walk with W the
   whole time: the character should never stop moving.
4. If the first attempt fails while the network is down, the second starts 5 s
   after the first started; a 5 s outage therefore puts it about when the
   network returns.

Leaving the worktree: `build-dev\bin\Data` is a junction to
`D:\games\WoWee\Data`; delete the junction itself (`cmd /c rmdir`) before
removing the worktree. The `extern/imgui`, `extern/vk-bootstrap` and
`extern/FidelityFX-FSR2` here are plain copies, not junctions.
