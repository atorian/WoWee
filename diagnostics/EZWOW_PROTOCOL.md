# EZWoW protocol observations

Reference client: original WoW 3.3.5a build 12340. User explicitly authorized
inspection of their logged-in client and login exchange. Sources: the local
WoW.exe/ezLib disassembly, read-only executable-region inspection and WoWee's
locally recorded extension traffic. No authentication secrets are in the fixtures.

## Confirmed initialization sequence

The first trace reaching the runtime check contains these Warden plaintext frames:

1. MODULE_INITIALIZE, 57 bytes, three records. Each record is command 3,
   little-endian u16 body length, u32 checksum and body. Its checksum matches
   XOR of the five little-endian u32 words of SHA1(body).
   - 20-byte body: `01 00 02 00`, then four original MPQ function RVAs
     `0x24F80`, `0x218C0`, `0x22530`, `0x22910`.
   - 8-byte body: `04 00 00`, function RVA `0x9D1000`, flag 1.
   - 8-byte body: `01 01 00`, function RVA `0x14E720`, flag 1.
2. CHECKS_REQUEST, 11 bytes: empty string table, timing check, memory check
   at absolute address `0x4002A7`, length 1. Static original PE byte is `E0`.
3. Another MODULE_INITIALIZE, same MPQ/Lua records; final function changes
   from RVA `0x14E720` to standard performance-counter RVA `0x46AE20`.
4. CHECKS_REQUEST, 11 bytes: empty string table, timing check, memory check
   at `0xDD1000`, length 216. WoWee's static .zdata is zero on disk;
   the original logged-in client has executable code there.

There may be world packets interleaved with this sequence. The initial recorder
accepted only Warden commands 2/3 and world 0x41D, so absence from that recording
is not evidence that other packets were absent.

## Functions and channels

The original function at absolute address `0x54E720` registers battleground
handlers for these opcodes:
`23D 2D4 2E0 2E8 2E9 2EC 2ED 4C7 14B 4DE 4E0 4E4 4E5 4E6 4E1 4E8`.
The bootstrap carrier and bounded write semantics are confirmed below.

The bootstrap at `0xDD1000` calls WoW's FrameScript::Execute (`0x819210`), then
registers opcode `0x41D` with callback `0xDD104D`. That callback reads an executable
packet envelope at CDataStore buffer+6. That buffer starts with a two-byte
opcode, followed by a u32 extension command; +6 skips both. The complete world
body is `u32 command=0`, `u32 argumentLength`, arguments, `u32 codeLength`, code,
trailing bytes.
It copies code into an allocation and calls it with pointers to the arguments and
allocation metadata. WoWee parses the envelope but does not execute that code.

After native ezLib initialization, Core::onLoadGeneral replaces opcode `0x41D`
with `ezLib+0x5B280`, a different handler whose complete framing is not yet verified.
Patches::onLoadGeneral sends an empty packet with opcode `0x51F`
(`CMSG_PATCHES_REQUEST` in the library log).
The observed startup command in ezLib's log is ASCII `LE`; its native result is
`load existed OK/OK`. These log strings alone do not establish its wire response.

## Validation target

Translate the carrier into actual session bootstrap state, then implement the
verified loader/command behavior and subsequent patch exchange. Clear all state
on reconnect or auth-endpoint change. Success requires a real server response
containing the character list, not merely a passed parser/infrastructure test.

## Confirmed bootstrap carrier (second trace)

The exact carrier is opcode `0x2E9`, registered by the function above. Twenty-seven
16-byte bodies contain zero, a descending count (`0x3CDED` down to `0x3CDD3`),
and eight bytes of bootstrap data. The handler at `0x54B3F0` writes the available
GUID to `0xBEA170+(count-1)*8`. Missing CDataStore reads do not overwrite the
remaining target bytes; its huge decrement loop is unnecessary for translation.

The reconstructed 216-byte payload equals the separately inspected original
client's executable region exactly. SHA256:
`6a4b09a44aed29e1ae0e42237574018ee79c0a1923fafdfcdc768b5d841d286e`.
The built-in adapter accepts only those writes within the bootstrap range while
the initialization callback is active. It validates initialization checksums,
complete reception and this known callback version, then serves actual received
bytes after timing is restored. It never performs native out-of-bounds writes.
The executable loader on the subsequent `0x41D` channel still needs translation.

## Loader and patch request (third trace)

The server accepted the reconstructed bootstrap memory check, then sent a Lua
status update and an 8137-byte 0x41D body. Its command is 0; arguments occupy
37 bytes, followed by an 8088-byte position-independent Windows x86 loader.
Loader SHA256: `b9a800aeba89811023cf9b342a05edb8ee2bfea7ed47a374ecc2e6c8309f5c2b`.
Arguments: u32 filename length 9, `ezLib.dll`, u32 file size 4099008, u32 flags 0,
16-byte MD5 `DCCE8190B7E77A5822672222C7E8FE76`. There is no trailing data.

The loader validates the existing DLL, locates export `cmd`, invokes `LE` and
ignores its returned string. Core::onLoadGeneral at ezLib+0x5E3D0 installs custom
0x41D dispatch and calls Patches::onLoadGeneral, which sends empty 0x51F. The
built-in adapter recognizes this specific loader/manifest, installs its protocol
state and sends that patch request once per session. It does not execute the
loader, load the original DLL or send a fabricated "load existed OK/OK" response.
Patch negotiation and the remaining library functions still need translation;
this transition is tested locally but requires live validation.

The native Core::onServerBuckDataPacket reads the extension command at buffer+2
and data at buffer+6, dispatching commands 1..6 via a switch at ezLib+0x5B4CC:
1 -> +0xA4C80 (patches), 2 -> +0x80150, 3 -> +0x69CE0,
4 -> +0x5B000, 5 -> server double value, 6 -> +0x80330.
This confirms the two-byte internal opcode prefix independently of the loader.

### Patch metadata callback, before the next live capture

Native ezLib+0xA4C80 (`Patches::onOpcodeMetadata`) starts by reading a u8
record count, then for each row a u32 string length and the filename bytes. It
parses that name with the regex `(\\d+)(_lazy)?_(.*?)_?(enUS|enGB|koKR|frFR|deDE|zhCN|zhTW|esES|esMX|ruRU)?\\.`.
The original client log reports 33 records, then checks local MPQs and mounts
required patches. This establishes the initial fields only; remaining row
fields, checksums and completion messages need actual-packet validation.

## Fourth trace: manifest accepted by the transport

The translated loader's 0x51F request was accepted live. Before command 1, the
server restored its Lua callback to RVA 0x419210 and sent five MPQ hash checks
for ChamberofAspectsRed ADTs; WoWee found all five in its current asset source.
Command 1 is a 2144-byte body: u32 command 1, u8 count 33, then records
`u32 nameLength`, ASCII archive basename, `u32 archiveSize`, `u32 flags`,
16-byte full checksum and 16-byte fast checksum. All 33 records consume the body
exactly. The native callback reads the same name, size, flags and two 16-byte
values. Archive names include optional locale suffixes and a numerical priority.

The adapter now parses and stores this manifest atomically in the session. Names
are bounded basenames with an MPQ extension, without path separators, control
characters, traversal or duplicates. It sends no reply for metadata reception,
matching the reference callback. This does not mark patches as installed: mounting,
checksum validation, DBC diff handling and final readiness still require a real
implementation. Await the next live server exchange to identify the gate.

Native `getMpqMD5` (ezLib+0xA0950) computes the fast checksum by opening the
archive's `(attributes)` file and hashing its full contents with MD5 in chunks
of 65535 bytes. It is not a hash of an arbitrary prefix of the archive. The first
record's full checksum was independently verified against local `dbc_diff.MPQ`:
size 11182348, MD5 `903bfdcab045b6a78f123ec15dcaccda`.

WoWee sends its initial CMSG_CHAR_ENUM before the proprietary initialization.
The live check on 2026-10-11 advanced from command 1 through all DBC hash
queries. All queried DBC files were found; the unrelated AccLuaMaGeSancho addon
was absent. The server restored the regular Warden callbacks and left the
connection open, but no character list arrived.

The original diagnostic retry after 30 seconds returned seven real characters,
and the user selected one. Four traces then established an earlier boundary:
the final 57-byte Warden initialization sets the file callback to RVA 0x419210
and restores timing to 0x46AE20, after the patch manifest and DBC checks. Earlier
restores keep the file callback at 0x24F80 and must not trigger enumeration.

The adapter now queues one ordinary enumeration retry at that verified boundary.
The main thread consumes it after pending Warden responses; it sends only while
CHAR_LIST_REQUESTED, connected and not already retried. Consuming the hint even
when the list already arrived prevents stale retries after create/delete. New
bootstrap/session initialization clears hints. Checksums of all initialization
records must validate before queuing. The 30-second timer remains a fallback for
other realms. No extension readiness or patch-installation acknowledgement is
sent. Live validation on 2026-10-11 returned seven characters in two sessions:
17.403 seconds (13:36:14.582 to 13:36:31.985) and 14.715 seconds
(13:36:41.535 to 13:36:56.250), versus the earlier 30.233 seconds. The user
confirmed that the list appeared faster and Cyrillic names were readable.

## World-entry known titles (command 3)

Selecting a character produced body `030000000302008c008d00`, followed by the
previous adapter's explicit unsupported-command disconnect. The reference callback
at ezLib+0x69CE0 is `EzFixes::Titles::onOpcodeKnownTitles`: its data starts with
u8 operation and u16 count, then count u16 title mask IDs, all little endian.
Operation 1 adds, 2 removes, and 3 clears the set before adding the supplied IDs.
The actual message replaces the known set with bits 140 and 141. The native
callback sends no acknowledgement. The adapter now validates the entire body
before dispatching an event to GameHandler's known titles and firing
KNOWN_TITLES_UPDATE; unknown operations, count mismatches and trailing bytes fail
explicitly. A later live session accepted this update and entered the world
after the command 5 fix described below.

Validation of the title stage: the extension/protocol suite passes 2560 assertions in 15 cases on Linux; the four Windows Warden/extension suites pass. The font suite also passes on Windows with WOWEE_TEST_FONT_DIR set to the local client fonts, verifying Cyrillic glyphs, preserved Latin advances and delayed loading after the source buffer is freed.

## World-entry extended money (command 5)

After the title fix, a live world-entry attempt accepted command 3 and then
produced `05000000000000c0160a7a41`: u32 command 5 followed by one little-endian
IEEE754 double, 27304300.0. Native dispatch at ezLib+0x5B517 stores this value
at ezLib+0x3BBBD8; the Lua money accessor at +0x6C090 pushes that same double.
The notification threshold is 2147483647.0, and the associated sound is
LOOTWINDOWCOINSOUND. The received value is an extended copper balance, not a
clock. WoWee keeps it separately from stock update fields, emits PLAYER_MONEY
when it changes and clears it on disconnect or character selection. Parsing
requires exactly twelve bytes, a nonnegative finite integer no larger than
2^53-1, before integer conversion. This command produces no network acknowledgement.
World entry after commands 3 and 5 still needs live verification.

The combined tests pass with 2872 assertions in 18 protocol/extension cases on
Linux; all four Windows Warden/extension suites pass. At 13:25:12 on 2026-10-11
(Europe/Berlin), the live client accepted commands 3 and 5, received
SMSG_LOGIN_VERIFY_WORLD and entered IN_WORLD. Subsequent spell/talent packets
continued in that state; the protocol no longer aborts this world-entry stage.

The user subsequently confirmed successful entry into the game and direct
control of the character on 2026-10-11. Cyrillic names and the faster character
list were also confirmed by the user. This validates the recorded login path;
other undocumented extension commands and native DLL compatibility remain
separate from that result.
