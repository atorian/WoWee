# Test review, batch 0 (63 files)

Rules applied: AGENTS.md "Tests test behaviour" (A constant, B tautology, C internals, D replica, E mock-echo, F cannot fail / weak, G dead code, H context isolation). "Dead code" was checked by grepping `src/`, `include/` and `tools/` for callers outside the definition file.

## 1. Files

| file | verdict | categories | note |
|---|---|---|---|
| tests/framexml_macro_icons.lua | OK | - | Real FrameXML and native APIs: macro icon resolution through the action button. |
| tests/framexml_spellbook.lua | OK | - | Real spellbook FrameXML with learned spells: rank filter, paging, drag picks the real rank. |
| tests/test_account_data_domain.cpp | minor | B | GameHarness end to end. Expected slot ids come from the `AccountDataType` enum, and the macro action-button wire value restates the formula. |
| tests/test_achievement_criteria.cpp | problem | D | The list-walk test copies the production `SMSG_ALL_ACHIEVEMENT_DATA` loop into the test. |
| tests/test_achievement_domain.cpp | OK | - | GameHarness: packets in, panel state, addon events and chat lines out. |
| tests/test_achievements.cpp | OK | - | Context test, links only `wowee_achievement`, real bus. |
| tests/test_action_bar_domain.cpp | minor | B | GameHarness. The update-field index comes from the client's own table (`fieldIndex(UF::PLAYER_FIELD_BYTES)`). |
| tests/test_activity_fsm.cpp | problem | C, F, G, B | Asserts the internal `State` enum, uses a test-only `setState` backdoor, checks only `out.valid`, and tests `reset`/`isActive`, which the app never calls. |
| tests/test_addon_event_queue.cpp | OK | - | Queue order and the real castingbarframe.lua. `UnitCastingInfo` is a hand-written stand-in (low). |
| tests/test_addon_globals.cpp | OK | - | Pure scanner with literal inputs. Used by addon_manager. |
| tests/test_addon_lua_snippets.cpp | minor | F | One good behaviour test (retired controls). The other four only check that the Lua parses. |
| tests/test_adt_alpha.cpp | OK | - | Hand-built MCAL with literal expected texels. |
| tests/test_adt_truncation.cpp | problem | F | `REQUIRE_NOTHROW` on a function that never throws. A test named "rejects" asserts nothing about rejection. The last two cases are good. |
| tests/test_agent_controls.cpp | OK | - | Public API, literal timings. |
| tests/test_anim_capability.cpp | whole file should go | A, B | Asserts default member initializers and a trivial factory's fields. |
| tests/test_animation_ids.cpp | problem | A, F | Enum values against literals; five `validateAgainstDBC` tests with no assertion at all. |
| tests/test_archive_layer.cpp | OK | - | Through `AssetManager`, with real MPQs. |
| tests/test_arena_domain.cpp | OK | - | GameHarness. |
| tests/test_arena_teams.cpp | OK | - | Context only (`wowee_arena`). |
| tests/test_asset_file_cache.cpp | OK | - | Cache stats are the observable here; also used by resource_monitor. |
| tests/test_asset_inventory.cpp | OK | - | Real directory trees, literal expectations. |
| tests/test_asset_manifest_load.cpp | minor | F | Uses `SUCCEED` + `return` when Data/manifest.json is absent, so CI shows green instead of a skip. |
| tests/test_asset_pack.cpp | minor | F | `files >= 3` where the count is knowable. |
| tests/test_asset_profiles.cpp | minor | F | `bases().size() >= 4` is weak; the rest is fine. |
| tests/test_asset_token_name.cpp | problem | G | `assetNameHasToken` and `assetNameHasWordToken` have no production caller. |
| tests/test_attacker_state_layout.cpp | OK | - | Literal byte layout. The header admits the oracle is "what these readers expect", not the server (low). |
| tests/test_auction_domain.cpp | OK | - | GameHarness. |
| tests/test_auction_house.cpp | OK | - | Context only (`wowee_auction`). |
| tests/test_auction_slot_filter.cpp | minor | A | Behaviour cases are good. Several cases assert the shape of the static `kAuctionSlots` table (uniqueness, bounds, token prefix, row counts). |
| tests/test_auction_sort_packet.cpp | OK | - | Sort block bytes against AzerothCore's reader. |
| tests/test_auth_reconnect.cpp | OK | - | Real `AuthHandler` over a loopback socket. |
| tests/test_auth_session_packet.cpp | OK | - | Nonces precomputed in Python; byte offsets from Wow.exe. |
| tests/test_base_fallback.cpp | OK | - | Pure decision, used by asset_manager. |
| tests/test_battlefield_status_packet.cpp | OK | - | Bytes in AzerothCore's order. |
| tests/test_battleground_domain.cpp | OK | - | GameHarness. |
| tests/test_battlegrounds.cpp | OK | - | Context only. |
| tests/test_benchmark_run.cpp | OK | - | Public API, report text. |
| tests/test_bit_packet.cpp | problem | G | The bit-stream and guid-mask API serves only the Cataclysm movement reader, which nothing calls. The byte-path test at :192 is the one live case. |
| tests/test_blp_alpha_scan.cpp | minor | F | Good differential oracle, but returns with a WARN instead of SKIP when Data/ is missing. |
| tests/test_blp_alpha_silhouette.cpp | OK | - | Hand-built blocks with measured literals, plus a differential check. |
| tests/test_blp_loader.cpp | problem | B, G, F | Set-then-get on `isValid`; `vector::push_back`; non-null checks on name helpers with no production caller. |
| tests/test_blp_truncation.cpp | problem | F | `REQUIRE_NOTHROW` only, on a loader that never throws. The mip-wrap case is good. |
| tests/test_buff_bar_layout.cpp | whole file should go | G, B | `computeBuffBarLayout` and `minimapReservedWidth` have no production caller. |
| tests/test_calendar.cpp | OK | - | Context only. |
| tests/test_calendar_domain.cpp | OK | - | GameHarness. |
| tests/test_calendar_month.cpp | minor | B | One expected value comes from `weekdayOf`, the function under test. |
| tests/test_calendar_packet.cpp | problem | B | The holiday array lengths in the test packet come from the parser's own constants, so the "26 dates" bug the comment describes cannot fail the test. |
| tests/test_camera.cpp | minor | F | Guard tests are fine. The ±89° case only asserts `isfinite`. |
| tests/test_camera_boom.cpp | minor | B | Expected values are built from `kCameraBoomPad` / `kCameraBoomWheelSpeed`, and the fade input from the implementation's formula. |
| tests/test_camera_pivot.cpp | OK | - | Measured skeleton literals. |
| tests/test_camera_water_depth.cpp | OK | - | Pure, literal. |
| tests/test_cata_movement.cpp | whole file should go | G, D, A, F | `readCataMovement` has no caller. The test's own writer mirrors the reader ("agrees by construction"). |
| tests/test_catalog_entry_key.cpp | problem | C | Three tests regex-scan editor source text instead of running the `--info` handlers. The `entryPrimaryKey` tests are good. |
| tests/test_catalog_subprocess.cpp | OK | - | Real child process. |
| tests/test_char_enum_layout.cpp | OK | - | Server-order bytes for all three expansions. |
| tests/test_character_session.cpp | problem | D, F | Builds the per-character `InterfaceState` itself and never touches `core::CharacterSession`. |
| tests/test_chat_domain.cpp | OK | - | Model GameHarness file. |
| tests/test_chat_filters.cpp | OK | - | Pure, used by chat_handler. |
| tests/test_chat_markup_parser.cpp | whole file should go | G | `ChatMarkupParser` is never constructed or called outside its own .cpp. |
| tests/test_chat_types.cpp | whole file should go | A | Every case asserts enum values against hex literals. |
| tests/test_chat_window_background.cpp | minor | B | Expected value is the source constant `kChatBackgroundDefault`; the property tests are good. |
| tests/test_chunk_surface.cpp | OK | - | Literal heights, plane exactness. |
| tests/test_chunked_vector.cpp | OK | - | Observable relocation count and addresses. |

## 2. Findings

### tests/test_activity_fsm.cpp:124 - TEST_CASE "ActivityFSM: reset clears all state"
- Category: G, C
- Severity: high
- Evidence:
  ```cpp
  fsm.reset();
  CHECK(fsm.getState() == ActivityFSM::State::NONE);
  CHECK_FALSE(fsm.isActive());
  ```
- Why: `ActivityFSM::reset()` and `isActive()` have no caller in src/ or include/. The production callers use only `startEmote`, `cancelEmote`, `onEvent`, `resolve`, `setStandState`, `startLooting`, `stopLooting`, `isEmoteActive`, `setSeatedLoopAnimation` and one log line of `getState`. This tests code the running app never executes.
- Replace with: delete it, together with `reset`/`isActive` if nothing else needs them.

### tests/test_activity_fsm.cpp:47 / :62 / :78 - "emote cancelled by movement", "looting starts and stops", "sit stand state triggers sit down"
- Category: C, F
- Severity: medium
- Evidence:
  ```cpp
  auto out = fsm.resolve(in, caps);          // :55, never read
  CHECK(fsm.getState() == ActivityFSM::State::NONE);
  REQUIRE(out.valid);                         // :71, :88 - the only check on output
  CHECK(fsm.getState() == ActivityFSM::State::LOOT_END);
  ```
- Why: the behaviour is which animation `resolve` returns. These tests read the private state machine's enum instead, which could be renamed or restructured while the animations stay right. Where they do read output they check only `valid`, so a wrong anim id (KNEEL_LOOP instead of SIT_GROUND_DOWN, or still EMOTE_WAVE after moving) passes.
- Replace with: assert `resolve(...).animId` and `loop` (for example SIT_GROUND_DOWN then SITTING, LOOT then KNEEL_END, and no emote id once moving).

### tests/test_activity_fsm.cpp:91 - "consumption loop preserves sit-down transition"
- Category: C
- Severity: medium
- Evidence: `fsm.setState(ActivityFSM::State::SITTING);`
- Why: `ActivityFSM::setState` is a public setter that only tests call. The test jumps the machine past the transition it is supposed to verify, so it cannot catch a sit-down one-shot that never completes into the eating loop.
- Replace with: drive the transition through `resolve` with `haveAnimState`/`currentAnimTime` past the sit-down duration, then assert EATING_LOOP.

### tests/test_activity_fsm.cpp:111 - "cancel emote explicitly"
- Category: B, G
- Severity: low
- Evidence: `fsm.startEmote(anim::EMOTE_DANCE, true); CHECK(fsm.getEmoteAnimId() == anim::EMOTE_DANCE);`
- Why: set-then-get through `getEmoteAnimId`, which has no production caller.
- Replace with: after `cancelEmote`, assert that `resolve` no longer yields EMOTE_DANCE.

### tests/test_anim_capability.cpp:7 / :28 / :44 / :51 - whole file
- Category: A, B
- Severity: high
- Evidence:
  ```cpp
  CHECK(caps.resolvedStand == 0);   // ...16 fields
  CHECK_FALSE(caps.hasStand);       // ...11 flags
  auto out = AnimOutput::ok(42, true); CHECK(out.animId == 42);
  ```
- Why: these assert struct default member initializers and a two-line factory's fields, which copies the header into the test. It fails only when someone edits the default, and then they edit the test too.
- Replace with: delete. The behaviour of an empty capability set (a model with no animations) belongs in an FSM/animator test asserting what is played.

### tests/test_animation_ids.cpp:149 / :153 / :174 - "ANIM_COUNT matches expected value", "Animation constant IDs ...", "IDs above 145 follow 3.3.5's AnimationData.dbc"
- Category: A
- Severity: high
- Evidence:
  ```cpp
  REQUIRE(anim::ANIM_COUNT == 506);
  REQUIRE(anim::STAND == 0);  REQUIRE(anim::DEATH == 1); ...
  REQUIRE(anim::OPENED == 149); REQUIRE(anim::DESTROY == 150); ...
  ```
- Why: this is the exact pattern AGENTS.md forbids (`CHECK(SPELL_MOD_OP_COUNT == 32)`). The values do have an outside source (AnimationData.dbc). The rule says to feed the data and check the behaviour.
- Replace with: load a real or literal AnimationData.dbc row set and assert that the animator plays the row the DBC names for the purpose (for example the sequence chosen for "Opened" is DBC id 149), or have `validateAgainstDBC` return its mismatches and assert there are none against the real DBC.

### tests/test_animation_ids.cpp:84-147 - five "validateAgainstDBC with ..." tests
- Category: F
- Severity: high
- Evidence:
  ```cpp
  // Should complete without crashing - all IDs match
  anim::validateAgainstDBC(*dbc);
  ```
- Why: there is no assertion about `validateAgainstDBC`. It returns void and only logs. The REQUIREs check the test's own DBC builder (`getRecordCount`). A validator that reports nothing, or reports everything, passes.
- Replace with: have it return (or let the test capture) the missing/extra ids, then assert `{3}` missing for `{0,1,2,4,5,...}` and `{500,600,1000}` extra.

### tests/test_animation_ids.cpp:52-80 - "nameFromId returns correct names ..."
- Category: A
- Severity: medium
- Evidence: `REQUIRE(std::string(anim::nameFromId(anim::STAND)) == "STAND");`
- Why: asserts table entries equal their own identifiers. `nameFromId` is used only to format two log lines in `validateAgainstDBC`.
- Replace with: drop it, or fold it into the validator test above (the log/result names the first missing id).

### tests/test_achievement_criteria.cpp:90 - "a list of records walks to its sentinel"
- Category: D
- Severity: medium
- Evidence:
  ```cpp
  while (p.hasRemaining(4)) {
      uint32_t id = p.readUInt32();
      if (id == 0xFFFFFFFF) break;
      CriteriaProgressRecord rec;
      if (!readCriteriaProgressTail(p, rec)) break;
  ```
- Why: this is a copy of the loop in `src/game/game_handler_packets.cpp:828-834`. The test exercises its own loop, so a bug in the production loop (wrong sentinel, `continue` instead of `break`, dropped `push_back`) is not caught. The single-record cases above are fine.
- Replace with: `SMSG_ALL_ACHIEVEMENT_DATA` with three mixed-width records through GameHarness, asserting `achievements().getCriteriaProgress()` (test_achievement_domain.cpp:64 already does this with one record; extend it to three).

### tests/test_adt_truncation.cpp:64 / :93 / :153 - "survives a truncated file", "survives a self-inconsistent MCNK", "rejects a file of random bytes"
- Category: F
- Severity: medium
- Evidence:
  ```cpp
  REQUIRE_NOTHROW(ADTLoader::load({}));
  ...
  TEST_CASE("ADT loader rejects a file of random bytes", ...) { ... REQUIRE_NOTHROW(ADTLoader::load(data)); }
  ```
- Why: `ADTLoader::load` contains no `throw`, so `REQUIRE_NOTHROW` can fail only by crashing, and only under ASan does an overrun show. The outcome is knowable: empty input gives `!isLoaded()`, and an inconsistent MCNK gives a chunk with no alpha map or heights. "rejects random bytes" asserts no rejection, and `load` sets `loaded = true` for any non-empty input, so the name is false.
- Replace with: assert the returned terrain, for example `CHECK_FALSE(load({}).isLoaded())` and `chunks[0].alphaMap.empty()` for the MCAL-smaller-than-header case (as the good cases at :164 and :180 do).

### tests/test_blp_truncation.cpp:74 / :85 / :130 - "survives a truncated file", "survives a mip level smaller than its format needs", "survives random bytes"
- Category: F
- Severity: medium
- Evidence: `REQUIRE_NOTHROW(BLPLoader::load(data));`
- Why: `blp_loader.cpp` has zero `throw`s, so this is the same cannot-fail pattern. A 4096x4096 DXT1 with 8 bytes of mip has a knowable answer.
- Replace with: `CHECK_FALSE(BLPLoader::load(data).isValid())`, as the good case at :119 already does.

### tests/test_blp_loader.cpp:10 / :19 / :27 - "BLPImage default is invalid", "isValid with data", "isValid requires non-empty data"
- Category: B, A
- Severity: medium
- Evidence: `img.width = 64; img.height = 64; img.data.resize(...); REQUIRE(img.isValid());` and `REQUIRE(img.format == BLPFormat::UNKNOWN);`
- Why: sets public fields and reads them back through a one-line predicate, and asserts default member values. A real decode is never involved.
- Replace with: load a literal tiny BLP (as test_asset_file_cache.cpp:20 builds one) and assert `isValid()`, width and height.

### tests/test_blp_loader.cpp:55 / :65 - "getFormatName returns non-null", "getCompressionName returns non-null"
- Category: G, F
- Severity: high
- Evidence: `REQUIRE(BLPLoader::getFormatName(BLPFormat::UNKNOWN) != nullptr);`
- Why: `getFormatName` and `getCompressionName` have no caller outside blp_loader.cpp/.hpp. Non-null is also an assertion no realistic bug breaks.
- Replace with: delete both, and the two functions.

### tests/test_blp_loader.cpp:78 - "BLPImage mipmap storage"
- Category: B
- Severity: high
- Evidence: `img.mipmaps.push_back(...); ×3  REQUIRE(img.mipmaps.size() == 3);`
- Why: tests `std::vector::push_back`. It cannot fail on any BLP bug.
- Replace with: delete. Mip count belongs in a decode test of a literal BLP with three mips.

### tests/test_buff_bar_layout.cpp:21-141 - whole file
- Category: G, B
- Severity: high
- Evidence: `const auto l = computeBuffBarLayout(k1080pW, k1080pH, 1.0f, 5, 0);` and at :110 `const float expected = M::kWindowPadding + 6 * l.iconSize + 5 * l.iconSpacing;`
- Why: `computeBuffBarLayout` and `minimapReservedWidth` are referenced only in include/ui/buff_bar_layout.hpp and this test. combat_ui.cpp includes the header but calls neither. settings_panel.hpp reads only `BuffBarMetrics` scale constants. The test at :107 also recomputes the width with the implementation's formula.
- Replace with: delete the file and the two functions (dead code rule). Keep the constants that settings_panel uses.

### tests/test_bit_packet.cpp:18-190 - all but "the byte paths are unchanged when no bit is ever written"
- Category: G
- Severity: high
- Evidence: header comment: "None of the three expansions this client speaks today reads a single bit". `p.writeGuidMask(guid, kMaskOrder); p.readGuidMask(decoded, kMaskOrder);`
- Why: `writeBit`, `writeBits`, `writeGuidMask`, `writeGuidBytes`, `readGuidMask` and `guidFromBytes` have no production caller. `readBit`/`readBits`/`GuidBytes` are used only by src/game/cata_movement.cpp, and its entry point `readCataMovement` is called by nothing. The round-trip tests are also writer-against-reader of the same class. The test at :192 guards the live byte paths and is worth keeping.
- Replace with: delete with the bit API until a Cataclysm opcode is actually read. Keep :192.

### tests/test_cata_movement.cpp:125-223 - whole file
- Category: G, D, A, F
- Severity: high
- Evidence:
  ```cpp
  // A writer built beside the reader agrees with it by construction
  Packet build(const std::vector<MovementElement>& sequence, const Sample& s) { ... }
  CHECK(sequences.size() == 109);
  if (!std::filesystem::exists(kSequencesPath)) { SUCCEED(...); return; }
  ```
- Why:
  - `readCataMovement`, `CataMovementSequences` and `movementElementFromName` have no caller outside cata_movement.cpp/.hpp, so this is dead code.
  - The 60-line `build()` mirrors the reader's gating and inversion, which the header admits.
  - `size() == 109` is a count constant.
  - A missing data file passes silently.
- Replace with: delete until the reader is wired. When it is, test it against a captured 4.3.4 packet (bytes from a realm trace) through GameHarness on the cata expansion.

### tests/test_chat_markup_parser.cpp:11-207 - whole file
- Category: G
- Severity: high
- Evidence: `ChatMarkupParser parser; auto segs = parser.parse("Hello world");`
- Why: `ChatMarkupParser` is never constructed and `parse`/`parseWowColor` are never called outside src/ui/chat/chat_markup_parser.cpp. chat_panel.hpp includes the header and declares a `std::vector<ChatSegment>` cache that nothing fills. The app's chat goes through FrameXML. Dead code, so its tests go with it.
- Replace with: delete the file with `ChatMarkupParser`/`ChatMarkupRenderer` and the unused cache field.

### tests/test_chat_types.cpp:19 / :35 / :47 / :62 - whole file
- Category: A
- Severity: high
- Evidence:
  ```cpp
  REQUIRE(static_cast<int>(ChatType::LOOT) == 0x1B);
  REQUIRE(static_cast<int>(ChatType::MONEY) == 0x1C);
  ```
- Why: enum values against literals, which AGENTS.md names as forbidden. The real bug (a missing type made "CHAT_MSG_MONEY" never fire) is behaviour, and this file cannot see it: an enum entry spelled correctly but missing from the name switch passes here.
- Replace with: GameHarness `SMSG_MESSAGECHAT` with type byte 0x1C fires `CHAT_MSG_MONEY`; `sendChatMessage(PARTY)` writes type 2 in `CMSG_MESSAGECHAT`.

### tests/test_asset_token_name.cpp:49-120 - "a directory cannot make an asset something it is not", "an asset named for what it is still matches", "a token spanning the separator", "a token is not matched as the tail of a longer word"
- Category: G
- Severity: high
- Evidence: `CHECK_FALSE(assetNameHasToken("Item\\TextureComponents\\LegLowerTexture\\...", "glow"));` and `CHECK_FALSE(assetNameHasWordToken("environment\\stars\\hellfireskynebula01.blp", "fire"));`
- Why: `assetNameHasToken` and `assetNameHasWordToken` are defined in m2_model_classifier.cpp but called from nowhere (grep across src/, include/ and tools/). The header says the texture colour-key hint uses them; it no longer does, so a regression of that hint (the Hellfire sky flicker) would not fail these tests. The `assetTokenName` and `classifyM2Model` cases (:30, :40, :134) are live.
- Replace with: delete the two helpers and their cases. Test the colour-key decision through whatever now makes it (its public function, or a scene_render check of the Hellfire sky).

### tests/test_calendar_packet.cpp:31-48 (used by :101 "A whole calendar reads back field for field")
- Category: B
- Severity: medium
- Evidence:
  ```cpp
  for (int i = 0; i < CalendarHoliday::kNumDates; ++i) { ...
  for (int i = 0; i < CalendarHoliday::kNumDurations; ++i) { ...
  // Read 25 dates instead of 26 and this row is four bytes out
  ```
- Why: the test packet's array lengths come from the parser's own constants. If `kNumDates` became 25, both writer and reader use 25 and the test passes, though the comment at :156 claims the test catches exactly that.
- Replace with: literal `26`, `10`, `10` (the server's MAX_HOLIDAY_DATES/DURATIONS/FLAGS) in `writeHoliday`.

### tests/test_catalog_entry_key.cpp:158 / :176 / :223 - "every --info flag ... is answered by a handler", "every flag derived ... is answered too", "a declared primary key is the field its own handler writes first"
- Category: C
- Severity: medium
- Evidence:
  ```cpp
  R"RX(strcmp\(argv\[i\],\s*"(--info-[a-z0-9-]+)"\))RX"
  R"RX(for\s*\(\s*const auto&\s*(\w+)\s*:\s*\w+\.entries\s*\))RX"
  ```
- Why: these regex-scan the editor's C++ source for `strcmp(argv[i], ...)` and loop shapes instead of running the commands. A handler dispatched another way (a table, `std::string_view ==`), or one that emits fields through a helper, breaks the test with behaviour unchanged. A flag that appears in source but is unreachable passes.
- Replace with: run the editor binary (as test_catalog_subprocess.cpp does with `runChildCapture`) with each table row's `--info` flag on a sample file, and assert exit 0 and that the first entry's first key equals `primaryKey`.

### tests/test_character_session.cpp:73 / :96 - "the next character's dungeon finder starts with nothing the last one chose", "a reload keeps ..."
- Category: D, F
- Severity: medium
- Evidence:
  ```cpp
  // B logs in on the same client and gets a session of its own.
  InterfaceState stateB;
  CharacterLua b(h.game(), &stateB);
  ```
- Why: the header says `core::CharacterSession` owns the per-character state, yet the test never creates one. The test itself makes a fresh `InterfaceState` for B, which is the step under test. If `CharacterSession` reused A's state on relog, this still passes. It does catch LFG bindings that keep function statics. The check at :164 (`savedVariablesPerCharacterPath("Bob") != ...("Alice")`) holds for any two names (F).
- Replace with: drive a relog through `core::CharacterSession` (client_harness.cpp already constructs one) and assert `GetLFDChoiceEnabledState()` is empty for the second character.

### tests/test_camera_boom.cpp:23 / :51 / :61 - "A wall pulls the camera in at once", "The wheel moves the camera at its own speed", "The player fades as the camera comes close"
- Category: B
- Severity: low
- Evidence:
  ```cpp
  CHECK(placed == Catch::Approx(4.0f - kCameraBoomPad));
  CHECK(current == Catch::Approx(10.0f + kCameraBoomWheelSpeed * 0.5f));
  cameraCloseFade(nearPlane + 1.0f / 360.0f + (kCameraCloseFadeDistance - 1.0f / 360.0f) * 0.5f, ...)
  ```
- Why: expected values use the source constants (1/9 yd, 8.33 yd/s), so a wrong constant goes unnoticed, and the header says they come from WoW.exe. The fade input copies the implementation's private `kStart` formula. The check at :30 reads `ease.elapsed`, an internal field.
- Replace with: literals from the original client: `4.0f - 1.0f/9.0f`, `10.0f + 8.33f*0.5f`, and a literal half-way distance.

### tests/test_account_data_domain.cpp:106 / :144 / :231 / :295
- Category: B
- Severity: low
- Evidence: `CHECK(requestedSlots(h) == std::vector<uint32_t>{static_cast<uint32_t>(AccountDataType::GlobalBindings)});` and `CHECK(set->readUInt32() == (0x41000000u | (mine & 0x00FFFFFFu)));`
- Why: the wire slot number comes from the client's enum. If `GlobalBindings` held the wrong value, client and test agree. The macro button value restates the encoding expression.
- Replace with: the literal slot numbers `2` and `5`, and `CHECK(lastUpload(...)...)` with the literal `"NONE"` instead of `kUnboundCommand`.

### tests/test_action_bar_domain.cpp:63
- Category: B
- Severity: low
- Evidence: `const uint16_t field = wowee::game::fieldIndex(wowee::game::UF::PLAYER_FIELD_BYTES);`
- Why: the update-field index comes from the client's own table, so a wrong index in update_fields.json is invisible.
- Replace with: the literal WotLK index of PLAYER_FIELD_BYTES.

### tests/test_auction_slot_filter.cpp:87 / :102 / :118 / :154 / :196
- Category: A
- Severity: low
- Evidence: `CHECK(slots[i] != slots[j]);`, `CHECK(std::string(kAuctionSlots[i].token).rfind("INVTYPE_", 0) == 0);`, `CHECK(count == 15);`
- Why: these assert properties of the static table's contents (uniqueness, bounds, token prefix, row count) rather than what a search sends. The literal-id cases (:20, :27, :60, :76, :164) carry the behaviour.
- Replace with: GameHarness `QueryAuctionItems(..., invTypeIndex)` and assert the `auctionSlotID` in `CMSG_AUCTION_LIST_ITEMS`.

### tests/test_addon_lua_snippets.cpp:165 / :175 / :185 / :197 - "... script parses"
- Category: F
- Severity: low
- Evidence: `compileError(wowee::addons::kWoweeOptionsPanelLua, ...); CHECK(err.empty());`
- Why: parse only. A snippet that parses but calls a missing function, or does nothing, passes. It does catch the documented syntax-error bug, so keep it as a smoke check.
- Replace with: a `framexml_run` case per snippet asserting its effect (for example the chat-style click hides the chat box).

### tests/test_asset_manifest_load.cpp:69, tests/test_blp_alpha_scan.cpp:43/:84, tests/test_blp_alpha_silhouette.cpp:166
- Category: F
- Severity: low
- Evidence: `SUCCEED("Data/manifest.json is not in this checkout; nothing to load."); return;` / `WARN("no Data directory ..."); return;`
- Why: with no data on the machine these report a pass, not a skip. CMake already maps Catch2's exit code 4 to SKIP, which these bypass. Also, :97 in blp_alpha_scan passes trivially when `compared == 0`.
- Replace with: `SKIP("...")`.

### tests/test_chat_window_background.cpp:21 / :77
- Category: B
- Severity: low
- Evidence: `CHECK(r == kChatBackgroundDefault[0]);`
- Why: expected value is the source constant. The property test at :27 covers the actual requirement (not white-and-solid).
- Replace with: the literal default (0, 0, 0, 0.25).

### tests/test_calendar_month.cpp:65
- Category: B
- Severity: low
- Evidence: `CHECK(now.firstWeekday == weekdayOf(8, 1, 2026));`
- Why: computes the expected value with the code under test.
- Replace with: `== 7` (1 Aug 2026 is a Saturday).

### tests/test_asset_pack.cpp:108 / :113 / :119
- Category: F
- Severity: low
- Evidence: `CHECK(wrote.files >= 3);`
- Why: the count is knowable (3 files written), so `>=` hides a duplicated or extra entry.
- Replace with: `== 3` (or the exact count including pack.json if that is intended).

### tests/test_asset_profiles.cpp:37, tests/test_camera.cpp:66
- Category: F
- Severity: low
- Evidence: `CHECK(bases().size() >= 4);` / `REQUIRE(std::isfinite(r.x));` at 89° pitch
- Why: weak bounds where a value is knowable (the right vector at pitch 89° and yaw 0 is a known unit vector).
- Replace with: exact ids/vectors.

## 3. Good examples

- **tests/test_chat_domain.cpp**: packets into the real game through GameHarness, then addon events, chat history and the outgoing `CMSG_MESSAGECHAT` read back. Per-character reset and DBC-driven names are checked via `h.dbc(...)`. This is the pattern AGENTS.md cites.
- **tests/test_auth_reconnect.cpp**: the real `AuthHandler` against a loopback login server. The bytes on the wire and the handler's reported outcome are asserted, with refusal and unreachable kept as distinct results.
- **tests/test_battlefield_status_packet.cpp** and **tests/test_char_enum_layout.cpp**: input bytes are built in the server's order with literal values, explicitly not with the client's writer, so drift from the wire fails.
- **tests/test_blp_alpha_scan.cpp**: a differential oracle (block scan against full decode over real files), independent of both implementations. It should SKIP rather than return when data is absent.
- **tests/framexml_spellbook.lua**: runs the real FrameXML spellbook with a learned-spell fixture and asserts what the player sees (rank labels, page clamp, the spell picked up by dragging).
