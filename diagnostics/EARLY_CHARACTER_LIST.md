# Early character list query

2026-10-12. The user reports roughly 15 seconds at “One moment”, absent in the original client.

## Evidence

The current `build/bin/logs/wowee.log` shows the first integrity check at
07:05:54.384, file checks starting at 07:05:54.868, and the last file check
ending at 07:06:08.720. The character screen appears around 07:06:09.
Individual file checks take milliseconds; successive batches are separated by
roughly 1.1 seconds. Parallelizing local hashes would remove little of this delay.

Read-only inspection of the installed ezLib release identifies
`Patches::processFinishDone` at RVA 0xA9430. Both reload and no-reload paths
converge at RVA 0xA9863. At RVA 0xA98C5 it passes the string `charselect`
(VA 0x20367270) to original WoW function 0x4D82A0.
`GlueXML/glueparent.lua:SetGlueScreen` shows the selected frame;
`CharacterSelect_OnShow` in `characterselect.lua` calls
`GetCharacterListUpdate()` while connected. The original ezLib log records a
relogin manifest at 18:53:42.782787 and completion at 18:53:42.785220.

WoWee previously sent a query before initialization and retried only after the
last verified callback restoration. That retry arrived after the entire file
check sequence. The native UI has an earlier ordinary character query.

## Change

The EZWoW adapter queues one early character-list hint after parsing its first
valid manifest. WardenHandler holds it until preparation and any pending integrity
response are complete. GameHandler consumes both early and final hints on the main
thread and sends an early CMSG_CHAR_ENUM only when connected and still awaiting
characters. When both hints are available in the same update, only the final
query is sent. The early query does not consume the final-boundary/30-second
fallback, send a readiness acknowledgement, or stop server checks.

Duplicate manifests cannot queue another early query. Bootstrap restart and
endpoint/reset state discard pending hints; errors suppress them. Hints are
consumed even when the character list has already arrived, avoiding later stale
refreshes after create/delete.

## Validation and limits

Windows protocol/extension regression suite: all 3007 assertions in 19 cases
passed. New cases cover a valid manifest, duplicates, malformed manifests,
bootstrap restart, subsequent errors, and preservation of the final retry.

`diagnostics/build-early-character-list.ps1` builds separate object files and
links a snapshot based on the validated glass/lamp client, preserving the primary
executable and runtime shaders. Output: `build/bin/wowee-early-character-list.exe`.

The server can still discard an early ordinary query; in that case the existing
final retry remains active.
Manifest receipt alone does not establish that all patches are mounted.

The isolated client link completed successfully. Runtime smoke check launched
`wowee-early-character-list.exe` with `WOWEE_LOG_FILE=early-character-list.log`
and `WOWEE_LOG_LEVEL=info`. It initialized the login screen, rendered frames,
and exposed a responding `Wowee` window. No login was submitted automatically.
The Computer Use screenshot call was unavailable because its Windows backend
rejected the WSL working directory (`sandboxCwd is not a local file URI`).
The journal and process checks established startup only, not live login speed.

## Live validation

The user manually signed in to the test client. First CMSG_CHAR_ENUM:
07:25:15.811. Manifest-triggered query: 07:25:22.281. SMSG_CHAR_ENUM with
five real characters: 07:25:22.468. Initial character list wait: **6.657 seconds**;
response after the early query: **0.187 seconds**. The same connection continued
answering DBC checks through 07:25:43.482, proving enumeration no longer waits
for those checks. A later ordinary refresh returned five characters in
0.763 seconds (07:25:40.768 to 07:25:41.531); this was not a fresh login.
The user closed the client cleanly at 07:25:59.293. No world entry was exercised.

The remaining initial wait consists of the preceding module/bootstrap/loader
exchange; packets in this session were separated by roughly 0.7–1.0 seconds
before the manifest, with local handlers taking milliseconds. This establishes
an improvement, not instant first login or parity with every original session.

The verified client is installed as `build/bin/wowee.exe`; previous executable:
`build/bin/wowee-before-early-character-list-20261012.exe`.
