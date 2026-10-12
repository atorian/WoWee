# Server protocols and compatibility adapters

WoWee selects compatibility by the **authentication hostname and port**, so two
realms behind the same login endpoint share a profile. Switching to a different
endpoint starts a fresh runtime and discards the previous Warden state and PE
image. Selecting a server without a profile uses the normal client behavior.

## Current capabilities and limits

The client can prepare a declared server library and adapter, verify their SHA256
checksums, download missing artifacts over HTTPS into a separate per-server cache,
and run an adapter in a separate process. Preparation runs off the render thread.
Warden memory reads consult the adapter first; unhandled main-client reads fall
back to that session's PE image. A named DLL's relative address never falls back
to WoW.exe. Startup failure, crash, malformed replies and three-second IPC timeouts
produce a connection error instead of continuing with invented runtime bytes.
Profiles can also declare `runtime_ranges` (module name, address, size). Unhandled
reads overlapping those ranges fail explicitly, preventing a static zero-filled
region from silently substituting for required dynamic state.

ABI v1 provides session initialization and memory reads. It is a working
**compatibility foundation**, not automatic compatibility with arbitrary WoW DLLs.
Libraries that hook original WoW functions, execute Lua, or introduce custom
network packets also need their own adapter and additional client integrations.
A raw server DLL does not have to export this ABI: its adapter does.

### Built-in protocol adapters

A profile can select `"protocol": "ezwow"` instead of a native `adapter`/`library`.
This mode needs no DLL or helper process. `ServerProtocolAdapter` owns the server's
custom packet meanings and runtime memory; the existing world socket continues to
frame/encrypt outgoing messages. Credentials remain outside the adapter. Sessions
and errors reset on endpoint changes. Unknown protocol names and mismatched builds
fail explicitly.

The current EZWoW adapter parses the verified executable envelope on `0x41D`:
`u32 command=0`, `u32 argumentLength`, arguments, `u32 codeLength`, code, optional
trailing bytes. Lengths are little endian. Native CDataStore prepends a two-byte
opcode; the bootstrap's buffer+6 skips that opcode and the extension command.
Empty code, overflow, truncation and bodies larger than 1 MiB are rejected.

For the verified 8088-byte loader and ezLib release, the adapter translates its
initial transport step into one empty `CMSG_PATCHES_REQUEST` (0x51F) per session.
It runs no downloaded code and sends no invented DLL-success response. Command 1 parses and stores a bounded patch manifest in the session without
claiming that patches are mounted. Remaining commands have their own framing
and remain unsupported until translated.

For local diagnosis, `"trace": true` records selected traffic in the profile's
`trace/session-*.wpt`, capped at 8 MiB per session and 1 MiB per record. Files start
with `WPT1`; each record is `[u32 channel][u32 opcode][u32 length][body]`, little
endian. Channel 1 accepts only decrypted Warden commands 2 (checks) and 3
(initialization); channel 2 accepts opcode `0x41D` and the 16 battleground opcodes registered by
the original function at `0x54E720` (an EZWoW initialization callback). This
explicit set allows the bootstrap carrier to be identified while normal packet
dispatch continues unchanged. Auth packets, SRP
keys, module keys and challenge seeds are excluded by an explicit allowlist.
Traces remain local and may contain server-supplied scripts or check names.

Example diagnostic profile:

```json
{"version":1,"provider":"EZWoW","build":12340,
 "protocol":"ezwow","trace":true,
 "runtime_ranges":[{"address":14487552,"size":216}]}
```

### EZWoW status

EZWoW login is **not yet fixed**. The original 32-bit `ezLib` exposes `cmd` and
calls original WoW functions at fixed addresses. Loading it in a 64-bit client
cannot reproduce those functions. No source code or SDK is available locally.

A read-only inspection of the user's successfully logged-in original client on
2026-10-11 confirmed `ezLib` at `0x20000000` and nonzero executable code in the
216-byte region at `0xDD1000`. The region contains a bootstrap that registers a
handler for opcode `0x41D`; it is not simply a static DLL-data buffer. Its first
routine calls original WoW at `0x819210` and `0x6B0B80`; the second allocates memory,
copies code from a packet and invokes it. This does not establish the full `cmd`
contract or the source of the bootstrap. Do not replay this memory snapshot as a
substitute for the library runtime or execute unknown server code in WoWee.

Full EZWoW support needs a verified bootstrap/command contract, adapters for its
WoW callbacks and custom packet lifecycle, and a real login reaching
`SMSG_CHAR_ENUM`. The built-in adapter now reconstructs the bootstrap from its actual 0x2E9
messages in each session, verifies the known callback version and serves reads
from that received buffer. The initial loader transport step is translated; patch mounting and other commands remain incomplete. A real list of seven characters
was received after a bounded enumeration retry. Command 3 updates known titles through client state and was accepted live.
Command 5 now supplies an authoritative extended copper balance separately from
stock update fields. A live session on 2026-10-11 accepted both commands and
received SMSG_LOGIN_VERIFY_WORLD, entering IN_WORLD. The enumeration retry now
uses the verified final Warden callback restoration instead of waiting for the
diagnostic 30-second timeout. Two live sessions returned seven characters in
17.4 and 14.7 seconds, compared with the previous 30.2 seconds; the user also
confirmed readable Cyrillic names, successful world entry and character control.

On 2026-10-12, comparison with the native patch-completion callback and
CharacterSelect_OnShow identified an earlier ordinary character-list query.
The adapter now queues that query after its first valid manifest, on the main
thread after pending integrity replies, while retaining the final-boundary
retry. In one live session five characters arrived in 6.657 seconds, just
0.187 seconds after this early query, while DBC checks continued. This avoids
waiting for all checks before showing characters; manifest receipt still does
not claim patch mounting or instant first login.

The next update reuses the exact known Warden module from the local cache only
after validating its announced size, MD5 and current module key. Missing or
corrupt caches retain the normal download. An additional ordinary query at the
verified bootstrap-installation boundary can overlap earlier initialization too.
Two fresh live sessions on 2026-10-12 returned seven characters in 1.219 and
1.140 seconds with module-cache reuse; the second also reached IN_WORLD.
Manifest/final retries and the full Warden challenge/check sequence remain.

### EZWoW patch downloads

Read from the original `ezLib` 1.7.2 (`Patches.cpp`) on 2026-10-12. Command 1
lists `name, size, flags, md5, fastMd5`; `md5` is the whole file's MD5. Names are
`<order>[_lazy]_<name>[_<locale>].MPQ`; archives for another locale are skipped
and the file is kept without its prefix. `downloadMpq` does not use HTTP: it
calls the native XFER-initiate setup (`0x465550`) and hooks the native XFER
reply (`0x8cde60`, called at `0x8ca4e5`) so that, on the **login** connection,
`[0xFF][u8 length][server name]` precedes the stock `XFER_ACCEPT` (0x32) or
`XFER_RESUME` (0x33, u64 offset). The file then arrives as stock `XFER_DATA`
(0x31, u16 length, bytes).

`RealmPatchSync` (`src/core/realm_patch_sync.cpp`) does the same over the auth
connection, which stays open after realm selection. Archives go to
`<user data>/realm_patches/<realmPatchDirName(login host, port)>/`, download to
`.partial`, resume from it, and move into place only after the MD5 matches.
Checked MD5s are cached by size and modification time in `checksums.json`.
Mounting the archives and applying `dbc_diff.MPQ` are not done yet.

## Local profile

The default root is `Data/server_extensions`, relative to the resource directory.
`WOWEE_SERVER_EXTENSIONS_DIR` can select another root. The subdirectory is
`serverExtensionKey(host, port)` from `game/server_extension.hpp`: trimmed,
ASCII-case-insensitive hostname, readable prefix, port and a SHA256-derived suffix.
Profiles are local configuration; the client never chooses executable libraries
from an arbitrary Warden download or a remote manifest.

Each subdirectory contains `profile.json`:

```json
{
  "version": 1,
  "provider": "Example server",
  "build": 12340,
  "host": "../../../wowee_extension_host_x86.exe",
  "adapter": {
    "path": "adapter.dll",
    "sha256": "<64 lowercase hex characters>",
    "url": "https://downloads.example.org/adapter.dll"
  },
  "library": {
    "path": "server.dll",
    "sha256": "<64 lowercase hex characters>",
    "url": "https://downloads.example.org/server.dll"
  },
  "client": {
    "path": "WoW.exe",
    "sha256": "<64 lowercase hex characters>"
  },
  "runtime_ranges": [{"address": 14487552, "size": 216}],
  "options": {}
}
```

`host`, `client`, `options` and artifact URLs are optional. A relative path is
resolved against the profile directory; an absolute path refers to an explicitly
chosen existing installation. A missing local artifact can be downloaded from its
configured HTTPS URL; a local file with the wrong checksum is rejected. Downloads
are limited to 64 MiB, redirects are not followed, and the certificate and hostname
must validate. A new version requires an updated local profile with its new digest;
there is no inferred update endpoint for EZWoW.

Without `host`, WoWee uses the `wowee_extension_host` executable next to the client.
The host and adapter must have the same architecture. Use a separately built x86
host for a 32-bit adapter/library; the main client remains 64-bit. A process
boundary isolates crashes and memory addresses; it is not an OS security sandbox.
Only libraries explicitly declared in a locally trusted profile should run.

## Adapter contract and build

See `include/game/server_extension_api.h`. Export the undecorated symbol
`wowee_extension_api`, accept ABI version 1, and return a correctly sized API table.
`start` receives the endpoint, build, verified library/client paths and options.
The strings remain valid until `stop` in the host. Credentials and SRP session keys
are not provided. `read_memory` returns HANDLED only for fully produced runtime
bytes, UNHANDLED for addresses it does not own, or ERROR with `last_error` details.
Paths and text use UTF-8. Stdout logging is redirected away from the IPC stream.

Build a 32-bit Windows host with an installed Visual Studio C++ workload/SDK:

```text
cmake -S tools/server_extension_host -B build-extension-x86 -A Win32
cmake --build build-extension-x86 --config Release
```

Copy the resulting executable next to WoWee under a distinct x86 name and select
it in the profile. The separate CMake project needs no game renderer or OpenSSL.
The local machine has partial MSVC files but no registered Visual Studio instance;
that command could not build x86 here. The 64-bit host is built and exercised.

Tests use a dedicated fixture adapter, not EZWoW data. They exercise real DLL
loading in a child, stdout isolation, session counters across server changes,
endpoint normalization, bad checksums/builds, missing adapters, required runtime
ranges, Unicode paths, crashes and timeouts. The adapter suite runs on Windows and Linux; all four Windows
Warden/extension CTest suites passed.
Run `ctest -R "^(server_extension|warden_)" --output-on-failure`. Optional
`WOWEE_TEST_EXTENSION_HOST` and `WOWEE_TEST_EXTENSION_ADAPTER` select a matching
host/fixture of another architecture for the same tests.

The protocol tests additionally cover DLL-free preparation with a nonexistent host,
stock-server reset, malformed/overflowing executable offsets, required-bootstrap
failures without output substitution, and the trace allowlist/8 MiB cap. The
combined extension/protocol suite passed 2872 assertions in 18 cases on Linux.
All four Windows Warden/extension CTest suites passed with the final-initialization
retry, manifest, title and money parsers. Retry tests reject early, corrupt and
truncated initializations, consume the hint once and clear it on session changes.

The first recorded EZWoW session contains two 57-byte Warden initialization
messages. Both choose Lua function RVA `0x9D1000`; the first changes the timing
function to RVA `0x14E720` and the second restores `0x46AE20`. The former function
is the original battleground-handler registration routine. Checks read one byte
at `0x4002A7`, then 216 bytes at `0xDD1000`. This establishes an initialization
sequence, but not yet the bootstrap installation carrier.

A subsequent trace identifies the carrier: 27 short `MSG_BATTLEGROUND_PLAYER_POSITIONS`
(`0x2E9`) bodies, each `[u32 zero][u32 descendingCount][8 bytes]`. The original
handler writes the available GUID to `0xBEA170 + (count-1)*8`. Counts
`0x3CDED .. 0x3CDD3` deliver all 216 bootstrap bytes, identical to the original
client's observed region. WoWee implements these writes in a bounded per-session
buffer, validates full reception and the SHA256 of the supported callback code,
and marks it installed when the second initialization restores the timing
function. Memory responses use the received bytes, not the test fixture or a
stored runtime snapshot. Partial/changed deliveries fail without static fallback.

Bootstrap installation tests passed on Linux: the combined suite now has 165
assertions in 11 cases. They verify current-session delivery, partial reads,
fresh initialization clearing old bytes, changed code rejection, incomplete
delivery, initialization checksum failure and truncated initialization records.

Protocol callbacks for gameplay state are distinct from outgoing network replies.
The known-title event runs synchronously on world packet dispatch, after full
validation, and updates the client's existing title API without an invented
server acknowledgement. Disconnecting clears titles; each session owns its
callbacks. Native preparation and Warden workers do not dispatch this event.
