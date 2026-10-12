# Warden cache and earlier character query

2026-10-12. The user reports that the manifest-stage retry remains slow.
The previous 6.657-second result was one session; the later primary-client log
has much longer gaps and UI stalls, so that result is not a universal login time.

## Changes

WoWee previously wrote the 18,756-byte standard Warden module to its cache but
always answered MODULE_USE with MODULE_MISSING. For the exact known module hash
and current module key, it now reads the cached bytes, verifies their announced
size and MD5, and sends MODULE_OK directly. HASH_REQUEST and both RC4 key changes
still run normally. Unknown modules/keys, missing/corrupt/changed-size caches use
the download path. The read is bounded to 1 MiB and is read-only.

EZWoW additionally queues an ordinary character-list query when the fully received,
hash-verified bootstrap is installed and timing is restored. It waits for any
pending Warden replies and uses the same main-thread connected/awaiting-list guard.
Manifest and final-check retries remain available if the server ignores this
request. This adds at most one ordinary query at a protocol boundary, without
an extension-readiness acknowledgement or skipped checks.

The GameHandler log now identifies a generic protocol checkpoint, since the
hint can come from bootstrap installation or the manifest. Timing comparisons
must use first CMSG_CHAR_ENUM through the actual SMSG_CHAR_ENUM, and distinguish
ordinary refreshes from fresh sessions.

## Validation

Windows protocol/extension tests: 3015 assertions in 19 cases passed.
Windows Warden/cache/RC4 tests: 27 assertions in 5 cases passed.
The local cached module independently matches its filename MD5 and size.
New tests cover cache absence, wrong hash/content, wrong/zero/oversized length,
and the independent RC4 vector with MODULE_OK but no MODULE_MISSING byte.
Protocol tests consume the bootstrap hint, then verify the separate manifest
hint and final fallback remain available.

`build-fast-session.ps1` uses separate objects and an immutable snapshot based
on the previous installed client; output `build/bin/wowee-fast-session.exe`.
`run-fast-session.ps1` records `build/bin/logs/fast-session.log` at info level.
Two fresh live sessions returned seven actual characters:

| Session | First CMSG_CHAR_ENUM | Actual SMSG_CHAR_ENUM | Wait |
| --- | --- | --- | --- |
| First | 07:43:57.696 | 07:43:58.915 | 1.219 s |
| Second | 07:44:17.449 | 07:44:18.589 | 1.140 s |

Both sessions logged reuse of the verified 18,756-byte module, answered the
standard challenge, and continued integrity checks after the list. The second
session reached SMSG_LOGIN_VERIFY_WORLD at 07:44:28.503 and processed subsequent
IN_WORLD packets. The first unsuccessful authentication preceded these sessions
and did not exercise Warden. Existing FrameXML/PetActionBar errors appeared after
world entry; they are outside this login change.

This establishes fast cached-module login on the tested endpoint. Packet timing
can vary; a cold/missing/corrupt module cache still takes the download path.
The bootstrap query may be ignored, so the manifest/final retries remain.
The new build is installed as build/bin/wowee.exe, with the prior build preserved
as build/bin/wowee-before-fast-session-20261012.exe. The separate test process can
continue running without replacing its executable.
