# World-entry disconnect: WotLK server header framing

The active `build/bin/wowee.exe` repeatedly closed its own socket immediately
on world entry. In `build/bin/logs/wowee.log`, the 18:10:28.434 map transition
is followed at 18:10:28.562 by `World packet framing desync: oversized packet
size=32902`. Seven attempts in the current log and two in the earlier fast-login
log show the same error. The later generic "Server closed connection" message
is a consequence of that local close.

The receive parser assumed a four-byte header and rejected sizes above 0x8000.
WotLK server packets larger than 0x7fff use a five-byte header: a high-bit
marker, three big-endian size bytes (marker excluded), and a two-byte
little-endian opcode. All five header bytes participate in the RC4 stream.
Reference: [TrinityCore 3.3.5 ServerPktHeader](https://github.com/TrinityCore/TrinityCore/blob/3.3.5/src/server/game/Server/Protocol/ServerPktHeader.h).

The parser now reads the extra byte only when present and decrypts it once,
retaining the decrypted-header count while awaiting a fragmented header/body.
It uses a 32-bit payload size, shifts the opcode/payload offsets, bounds the
whole frame to the existing 8 MiB receive-buffer cap, and preserves four-byte
Classic/TBC headers. Buffered packets also drain after the parsing budget is
exhausted even when no further TCP bytes arrive.

Validation: `test_world_socket` passed 325 assertions in four cases: encrypted
extended packets above 32 KiB and 64 KiB, every header split, repeated partial
body parsing and the following encrypted normal packet, legacy high-bit size,
malformed/oversized headers, and a real loopback TCP burst drained across
multiple updates without another receive.

A real realm login must still be verified after restarting the new executable.
The Windows computer-use tool could not initialize in this WSL checkout
(`sandboxCwd is not a local file URI: file:///mnt/d/games/WoWee`).

The Windows client linked successfully from an immutable snapshot of 465
object files. Startup smoke validation produced a screenshot, reported no
errors, and logged a clean shutdown. The validated binary was installed as
`build/bin/wowee.exe`; the previous executable is preserved as
`build/bin/wowee-before-world-socket-fix.exe`. Installed SHA256:
`331182493da157cc9acb173dbf42d8916b60b37ac8d71403dcdad3fd59a0939c`.
The existing running process still uses its old mapped executable and must be
restarted for this correction to take effect.
