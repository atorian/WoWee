# Test review, batch 1 (49 files)

Rules applied: AGENTS.md "Tests test behaviour" (no constants, no tautologies, through the front door, a context tested on its own, GPU code tested on the GPU) plus the "Dead code" section. All 49 files were read in full. Where a finding depends on the code under test, the production code was opened and grepped (`src/`, `include/`, `tools/`).

## 1. Files

| file | verdict | categories | note |
|---|---|---|---|
| tests/test_cli_catalog_paths.cpp | OK | - | Pure path rule, literal expectations. |
| tests/test_cli_paths.cpp | OK | - | The oracle is a tree the test writes. |
| tests/test_client_timezone.cpp | OK | - | Expected offsets are real zones' values. |
| tests/test_client_transitions.cpp | minor | B | End to end through ClientHarness. One expectation reuses `kMaxAttempts`. |
| tests/test_collision_geometry.cpp | OK | - | Independent brute-force oracle. |
| tests/test_collision_world.cpp | minor | F | Good public-API tests. One out-param is read and never asserted. |
| tests/test_combat_domain.cpp | minor | C | GameHarness. Two cases inject update blocks past the packet decoder. |
| tests/test_combat_fsm.cpp | problem | C, F | Uses the test-only `setState` back door. Asserts internal state and not the animation. One case cannot fail. |
| tests/test_combat_text_filter.cpp | OK | - | Pure mapping with literal CVar names. |
| tests/test_contact_list.cpp | OK | - | Domain alone, checks the events it returns. |
| tests/test_contacts_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_contacts_service.cpp | OK | - | Recording gateway is the outgoing request, plus the real bus. |
| tests/test_cvar_store.cpp | OK | - | Round trip through the file. |
| tests/test_data_paths.cpp | OK | - | Sandbox filesystem as the oracle. |
| tests/test_dbc_layout_columns.cpp | minor | F | A good data oracle, but it passes silently when `Data/db` is absent, which is the case in every checkout. |
| tests/test_dbc_layouts.cpp | problem | A, F | Pins JSON column numbers and update-field indices to literals. |
| tests/test_dbc_loader.cpp | minor | G, C, F | Parser tests are fine. One tests a dead accessor and a few test internal accessors. |
| tests/test_dds_override.cpp | OK | - | Through `AssetManager::loadTexture`. |
| tests/test_dds_sidecar.cpp | OK | - | Parser with literal sizes. |
| tests/test_deferred_calls.cpp | OK | - | Observable order of applied calls. |
| tests/test_distance_fog.cpp | whole file should go | D, A | Compiles the production GLSL on the CPU. The first case pins default constants. |
| tests/test_distant_terrain.cpp | problem | D | The TerrainPlacementRefs and distant-map tests are fine. One case runs `aerial_perspective.glsli` on the CPU. |
| tests/test_duel_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_duels.cpp | OK | - | Domain plus service against the real bus, only `wowee_duel` linked. |
| tests/test_dungeon_finder.cpp | minor | B, E | Mostly good. A few set-then-read cases and one name echoed from the fake. |
| tests/test_dxt_block.cpp | OK | - | Expected values derived from the S3TC rules. |
| tests/test_editor_units.cpp | OK | - | Editor tool behaviour with literal expectations. |
| tests/test_entity.cpp | problem | B, A | About 15 setter/getter, default-value and constructor cases. ReportedMovement cases are fine. |
| tests/test_env_flag.cpp | problem | D, F | One case tests a lambda defined in the test. Another is misnamed and duplicates the default case. |
| tests/test_equipment_hash.cpp | minor | F | One case compares a pure function's result with itself. |
| tests/test_equipment_set_packets.cpp | whole file should go | D | Tests a copy of the production parser written in the test. The save case writes and reads its own packet. |
| tests/test_escape_action.cpp | minor | A, F, G | Good situation table. Asserts the size of its own array and pins a parameter the code ignores. |
| tests/test_event_bus.cpp | OK | - | Behaviour of the real bus. |
| tests/test_expansion_profile.cpp | OK | - | Real files in, parsed profile out. |
| tests/test_extract_progress.cpp | OK | - | Builds a real MPQ and drives the extractor. |
| tests/test_font_em_scale.cpp | OK | - | Font bytes built from the spec. |
| tests/test_footstep_paths.cpp | OK | - | Archive file names written in full. Acceptable as external data. |
| tests/test_format_lookup.cpp | minor | C | Round-trip cases are good. One case regex-parses `src/pipeline/*.cpp` source text. |
| tests/test_frame_budget.cpp | OK | - | Report text from known inputs. |
| tests/test_frame_pacer.cpp | OK | - | Timing behaviour. |
| tests/test_frame_pool.cpp | OK | - | Lifetime and allocation behaviour. |
| tests/test_frame_results.cpp | OK | - | Mailbox handoff behaviour. |
| tests/test_frame_targets.cpp | OK | - | Usable state plus the once-per-stretch message. |
| tests/test_frame_worker.cpp | OK | - | Ordering, back-pressure and failure propagation. |
| tests/test_framexml.cpp | problem | C, F | Parser cases are fine. About 70 emitter cases assert substrings of the generated Lua, not what the frames do. The shipped-file cases pass silently without data. |
| tests/test_framexml_takeover.cpp | problem | A, F | Pins an enum name and an element count. The "owned" checks can only fail through the environment. |
| tests/test_frustum.cpp | minor | G, C, F | Two cases test `containsPoint`, which nothing calls. One tests an internal helper. |
| tests/test_game_clock.cpp | OK | - | Literal hours from time arithmetic. |
| tests/test_game_data_domain.cpp | OK | - | GameHarness, stop-the-game contract. |

## 2. Findings

### tests/test_equipment_set_packets.cpp:53 - (whole file) "one equipment set reads back what was written", "an item guid of one means ignore the slot", "three sets do not run into each other", "a saved set is laid out the way the server reads it", "a new set's guid is one byte, not eight"
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  /// The reader under test, in the shape handleEquipmentSetList uses.
  std::vector<ParsedSet> readList(Packet& p) {
      ...
      if (itemGuid == 1) { es.ignoreSlotMask |= (1u << slot); ...
  ```
  and at 152-163 the save case builds the packet itself (`pkt.writePackedGuid(0); pkt.writeUInt32(3); ...`), then reads it back.
- **Why:** The test target links only `src/network/packet.cpp`. The parser being tested is a copy in the test file. The production `InventoryHandler::handleEquipmentSetList` (src/game/inventory_handler.cpp:2865) and the CMSG_EQUIPMENT_SET_SAVE builder (:2821) never run. A regression in either one would leave every case green, so the only code these cases exercise is `Packet` packed-guid I/O. No GameHarness test covers SMSG_EQUIPMENT_SET_LIST.
- **Replace with:** a GameHarness test. Receive SMSG_EQUIPMENT_SET_LIST holding three sets and read the sets through the inventory accessor. Then save a set and check the bytes of `lastSent(CMSG_EQUIPMENT_SET_SAVE)`.

### tests/test_distance_fog.cpp:5 - (file) "sky below the horizon...", "the sky dome adds no azimuth-dependent colour...", "sunlit air glows...", "fog leaves nearby surfaces clear...", "outdoor haze...", "thinning outdoor haze...", "thick distance haze...", "aerosol column...", "fog thickness strongly controls..."
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  // Run the production shader math, rather than a separate C++ approximation.
  namespace fog_math {
  using vec3 = glm::vec3; ...
  #include "../assets/shaders/sky_atmosphere.glsli"
  #include "../assets/shaders/sunlit_air.glsli"
  }
  ```
- **Why:** This is the case AGENTS.md forbids by name (user decision 2026-10-31: "a shader's behaviour is not tested by compiling it ... for the CPU"). glm stands in for GLSL here, so precision, built-in semantics and the pipeline around the shader are all replaced. A wrong uniform binding, a wrong fog parameter upload or a GPU precision difference would not show. The test only catches a change to the shared formula. Lines 102-106 (`ray == translated`) test glm, not the client.
- **Replace with:** `scene_render` scenes (a fogged valley and ridge, a sky below the horizon) checked by a `tools/scene_render/check_*.py` picture assertion.

### tests/test_distance_fog.cpp:20 - "zone light blending has no fallback sky or extra fog distance"
- **Category:** A
- **Severity:** medium
- **Evidence:** `CHECK(color == glm::vec3(0));` over `blendAccumulator()` fields; `CHECK(fallback.fogEnd == 1000.0f); CHECK(fallback.skyBand2Color.r == 1.0f);`
- **Why:** This copies default member initialisers into the test. It fails only when someone edits a default, and the test then gets edited with it.
- **Replace with:** blend two zone light rows with weights through `light_params_eval` and assert the blended fog end equals the weighted sum, with no fallback contribution.

### tests/test_distant_terrain.cpp:42 - "day haze approaches lit air while the night's authored tint survives"
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  namespace aerial { ... #include "../assets/shaders/aerial_perspective.glsli" }
  vec3 day=aerialScatteringColor(vec3(0,1,0),violet,neutral,sunlight,vec4(0,1,0,1));
  ```
- **Why:** This is a CPU copy of GPU code, the same violation as test_distance_fog. The shader runs through `aerial_frame.glsli` with its own inputs, and none of that is exercised.
- **Replace with:** a `scene_render` day/night pair over distant terrain with a picture check on haze colour.

### tests/test_env_flag.cpp:74 - "the range is a floor and a ceiling"
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  const auto clamp = [](long v, int lo, int hi) {
      return v < lo ? lo : (v > hi ? hi : static_cast<int>(v));
  };
  CHECK(clamp(0, 1, 512) == 1);
  ```
- **Why:** The test defines its own clamp and tests that. `wowee::core::envIntClamped` is never called with an out-of-range value. If the production code dropped its clamp, the test would still pass.
- **Replace with:** `setEnvVar("WOWEE_TEST_BUDGET", "0")`, then `"99999"`, then `"-5"` (`setEnvVar` and `unsetEnvVar` already exist in core/env.hpp and test_data_paths uses them). Assert `envIntClamped("WOWEE_TEST_BUDGET", 24, 1, 512)` returns 1, 512 and 1.

### tests/test_env_flag.cpp:70 - "a number in range is taken as written"
- **Category:** F
- **Severity:** medium
- **Evidence:** `CHECK(wowee::core::envIntClamped(nullptr, 24, 1, 512) == 24);`
- **Why:** A null key returns the default, so this asserts the default path, the same as line 89. A parser that ignored the variable's value would still pass. The name says "taken as written", but no value is written.
- **Replace with:** set the variable to `"300"` and assert 300.

### tests/test_entity.cpp:10 - "Entity default construction", "Entity GUID constructor" (20), "Entity position set/get" (25), "Unit construction and type" (279), "Unit name" (288), "Unit health" (295), "Unit level, entry, displayId" (328), "Unit faction and hostility" (376), "Unit mount display ID" (386), "GameObject construction" (406)
- **Category:** B
- **Severity:** medium
- **Evidence:**
  ```cpp
  u.setHealth(500); u.setMaxHealth(1000);
  REQUIRE(u.getHealth() == 500);
  ...
  u.setMountDisplayId(14374); REQUIRE(u.getMountDisplayId() == 14374);
  ```
- **Why:** These are set-then-get through plain setters and getters, plus defaults and the type a constructor tags. The only bug they catch is a setter that writes the wrong member, and the compiler plus any GameHarness test already covers that. "Entity field set/get/has" (263) and "Unit power by type" (306) are mostly the same, though the out-of-range clamp at 324-325 is real behaviour. "Player inherits Unit" (393) does guard a real shadowed-field regression and is worth keeping in some form.
- **Replace with:** SMSG_UPDATE_OBJECT for a unit with health, level and display id through GameHarness, then read the unit from the entity manager. Delete the plain accessor cases.

### tests/test_entity.cpp:373 - "Corpse state uses the real dynamic dead bit"
- **Category:** A
- **Severity:** low
- **Evidence:** `REQUIRE(UNIT_DYNFLAG_DEAD == 0x00000020);`
- **Why:** This asserts a constant against its own definition. The rest of the case (`isUnitCorpseState` with flags) is behaviour, but it passes the named constants instead of the wire value, so a wrong constant would also pass those checks.
- **Replace with:** call `isUnitCorpseState(0, 0, 0x20)` and `(100, 100, 0x08)` with the literal wire bits, and drop the constant assert.

### tests/test_entity.cpp:421 - "EntityManager add/get/has/remove", "EntityManager clear" (442), "EntityManager getEntities returns all" (459)
- **Category:** B
- **Severity:** low
- **Evidence:** `mgr.addEntity(1, unit); REQUIRE(mgr.getEntityCount() == 1); REQUIRE(mgr.hasEntity(1));`
- **Why:** These are CRUD checks on a map wrapper and restate the container. The null-reject case (452) and the spatial query (472) are real behaviour.
- **Replace with:** keep the null and spatial cases. Cover add and remove through SMSG_UPDATE_OBJECT create and SMSG_DESTROY_OBJECT in GameHarness.

### tests/test_combat_fsm.cpp:71 - "equipped fist weapons retain fist-weapon animations" (also 140 "stopSpellCast transitions to finalize", 174 "equipped thrown weapon...", 194 "a drawn wand...")
- **Category:** C
- **Severity:** medium
- **Evidence:** `fsm.setState(CombatFSM::State::COMBAT_IDLE);` and `fsm.setState(CombatFSM::State::SPELL_CASTING);`
- **Why:** `CombatFSM::setState` (include/rendering/animation/combat_fsm.hpp:70) has no production caller. `CharacterAnimator` only uses `onEvent`, `startSpellCast`, `stopSpellCast`, `triggerHitReaction`, `setStunned` and `resolve`. These tests reach states the animator may never produce. For example, they skip the UNSHEATHE step that `COMBAT_ENTER` goes through.
- **Replace with:** reach the state the way the animator does, with `onEvent(COMBAT_ENTER)` and `resolve` until it settles. Then assert the resolved `animId`. Remove `setState`.

### tests/test_combat_fsm.cpp:103 - "CombatFSM: stun does not override swimming"
- **Category:** F
- **Severity:** medium
- **Evidence:**
  ```cpp
  // The exact behavior depends on implementation, but stun should not
  // force an animation while swimming
  CHECK(fsm.getState() != CombatFSM::State::STUNNED);
  ```
- **Why:** The comment admits the expected outcome is unknown. The assertion checks an internal state, not the animation. Code that still returned `anim::STUN` while sitting in some other state would pass.
- **Replace with:** assert `out.valid == false` (or the swim animation, whichever is specified) and `out.animId != anim::STUN`.

### tests/test_combat_fsm.cpp:146 - "CombatFSM: hit reaction" (also 36 "INACTIVE by default", 155 "reset clears all state")
- **Category:** C, F
- **Severity:** low
- **Evidence:** `fsm.triggerHitReaction(anim::COMBAT_WOUND); CHECK(fsm.getState() == CombatFSM::State::HIT_REACTION);` (`caps` and `wl` are built and never used)
- **Why:** These assert the internal enum state, not what is played. A hit reaction that changed state but resolved to the wrong animation would pass.
- **Replace with:** `resolve(in, caps, wl).animId == anim::COMBAT_WOUND`, then the animation it returns to after the reaction.

### tests/test_dbc_layouts.cpp:119 - "CharacterFacialHairStyles geoset columns hold plausible variants"
- **Category:** A
- **Severity:** medium
- **Evidence:** `CHECK(g100 >= 6); ... CHECK((*fm)["RaceID"] == 0); CHECK((*fm)["SexID"] == 1); CHECK((*fm)["Variation"] == 2);`
- **Why:** The comment says "Pin the columns against the real files", but no DBC is opened. These are the JSON's numbers copied into the test. Editing the JSON means editing the test, and a column that is in range and still wrong (the LightIntBand class of bug) passes. test_dbc_layout_columns.cpp:148 already shows how to check by value.
- **Replace with:** read CharacterFacialHairStyles.dbc through the declared columns and assert every Geoset value is a small variant number (< 100), as test_dbc_layout_columns does for band counts.

### tests/test_dbc_layouts.cpp:157 - "UNIT_FIELD_CRITTER sits where the stock WotLK layout puts it"
- **Category:** A
- **Severity:** medium
- **Evidence:** `REQUIRE(j["UNIT_FIELD_HEALTH"].get<uint32_t>() == 24u); ... REQUIRE(j["UNIT_FIELD_CRITTER"].get<uint32_t>() == 10u);`
- **Why:** This asserts entries of update_fields.json against literals. AGENTS.md: "If a value must agree with the server ... feed the packet ... and check the behaviour that follows."
- **Replace with:** GameHarness: SMSG_UPDATE_OBJECT writing a critter guid at wire index 10 for the player. Assert the companion dismiss toggle sees it, or that CMSG_DISMISS_CRITTER carries that guid.

### tests/test_dbc_layouts.cpp:44 - "Spell layout exposes the fields the spell logic reads", "SpellRange layout exposes MaxRange" (74), "Spell field indices are distinct" (89)
- **Category:** F
- **Severity:** low
- **Evidence:** `REQUIRE(spell->field("RangeIndex") != kMissing);`, `REQUIRE(id != name);`
- **Why:** These are presence-only checks. Under the 2026-10-30/31 game-data rule, a missing column now stops the game when the data is opened, which `GameData::checkAll` covers (test_game_data_domain.cpp:47). "Distinct" checks four of many fields, and two columns that are distinct but swapped pass.
- **Replace with:** drop them in favour of `checkAll` per expansion, plus a GameHarness spell-range case (Battle Shout self-cast versus a 5-yard melee spell) fed from a `dbc(...)` row.

### tests/test_dbc_layout_columns.cpp:97 - "no layout names a column its DBC does not have", "the light bands' count column holds counts" (148), "the spell effect columns are the best match across expansions" (274)
- **Category:** F
- **Severity:** medium
- **Evidence:** `if (shipped.empty()) { WARN("Data/db has no readable DBC files here, skipping"); return; }` and `WARN(...); continue;` and `WARN(...); return;`
- **Why:** `Data/db` is not tracked by git (`git ls-files Data/db` is empty). In any clean checkout or CI run, all three cases report "passed" after testing nothing. The oracle is good, but the skip hides that nothing was checked.
- **Replace with:** `SKIP("...")` (as test_font_em_scale.cpp:140 does), so the run reports skipped and not passed.

### tests/test_dbc_loader.cpp:236 - "DBCFile getStringByOffset"
- **Category:** G
- **Severity:** high (tests dead code)
- **Evidence:** `REQUIRE(dbc.getStringByOffset(1) == "Offset5");`
- **Why:** `DBCFile::getStringByOffset` (include/pipeline/dbc_loader.hpp:121) has no caller in src/, include/ or tools/. Only this test uses it. AGENTS.md: dead code is deleted, not tested.
- **Replace with:** delete the method and the test. `getString` on a string column already covers the offset lookup.

### tests/test_dbc_loader.cpp:204 - "DBCFile getRecord returns pointer" (also 171 "DBCFile getStringView")
- **Category:** C
- **Severity:** low
- **Evidence:** `const uint8_t* rec = dbc.getRecord(0); ... std::memcpy(&val, rec, 4); REQUIRE(val == 0xAB);`
- **Why:** `getRecord` and `getStringView` are called only inside dbc_loader.cpp, as helpers of `getUInt32` and `getString` (one external use is `over.getRecord` in the merge). The public readers are already tested at 129 and 156.
- **Replace with:** nothing. These are covered by the `getUInt32` and `getString` cases. Consider making the helpers private.

### tests/test_dbc_loader.cpp:58 - "DBCFile default state"
- **Category:** F
- **Severity:** low
- **Evidence:** `REQUIRE_FALSE(dbc.isLoaded()); REQUIRE(dbc.getRecordCount() == 0);`
- **Why:** This asserts default member initialisers. The cases that matter here are "too small" and "wrong magic", which leave the file not loaded, and those exist.
- **Replace with:** delete it, or assert that a failed `load` leaves `isLoaded()` false after a successful earlier load.

### tests/test_framexml.cpp:82 - the `[framexml][emit]` cases, about 70 of them (for example 169 "an animation's parentKey goes on its group", 877 "Temporaries do not run into Lua's local-variable limit", 1278 "A button gets its font string before its text")
- **Category:** C
- **Severity:** medium
- **Evidence:**
  ```cpp
  const size_t group = lua.find(" = __w[1]:CreateAnimationGroup(");
  CHECK(has(lua, "__w[1].waitAndAnimOut = " + gvar));
  ...
  REQUIRE(has(r.lua, "local __w = {}"));  REQUIRE(locals == 1);
  ```
- **Why:** These assert the text of the generated Lua: variable names (`__w[1]`), helper names (`__WoweeTemplates`, `__WoweeFireOnLoad`), float formatting (`"5.000000, -7.000000"`) and the order of substrings. A refactor of the code generator that keeps behaviour, such as renaming `__w` or emitting through a helper, breaks dozens of cases. Wrong behaviour can also pass. Lua that contains `:SetID(1)` but never runs it, or that fails to compile, matches every substring check. The documented failures, such as "the whole chunk refuses to compile", "OnLoad ran before self.chatFrame was set" and "the tab is a sliver", are runtime outcomes these checks cannot see. The parser cases (27-78) are fine.
- **Replace with:** load the emitted chunk into the client's `LuaEngine`, as the `wowee_framexml_test` scenarios and ClientHarness do, and assert observable frame state. Examples: `CharacterFrame:IsShown() == false`, `PaperDollFrame:GetID() == 1`, `Alert.waitAndAnimOut.animOut ~= nil`, a 300-texture file compiles and creates `BigT299`, and `Tab1Text:GetText() == CHARACTER`.

### tests/test_framexml.cpp:286 - "characterframe.xml builds the frame the C key opens", "paperdollframe.xml builds the tab and gives it its id" (298)
- **Category:** F
- **Severity:** medium
- **Evidence:** `if (root.children.empty()) return;   // interface data not present`
- **Why:** `Data/interface/framexml` is not tracked, so these always pass in CI without checking anything. The helper's own comment (133-135) says passing by skipping "is worse than not having it". Line 295 (`has(r.lua, ":Hide()")`) would also pass on any hidden child frame anywhere in the file.
- **Replace with:** `SKIP(...)` when absent. When present, run the chunk and assert `CharacterFrame:IsShown()` is false.

### tests/test_framexml_takeover.cpp:39 - "The last element is still the last one" (also 62 inside "Every element is FrameXML's")
- **Category:** A
- **Severity:** medium
- **Evidence:** `REQUIRE(uiElementName(UiElement::Petition) == "petition");` and `CHECK(total == 52);`
- **Why:** These check a name-table entry and an enum count against literals. Adding element 53 fails the count, and someone bumps the number. The comment states the purpose is to make the loop's upper bound visible, which is a test-maintenance device, not behaviour.
- **Replace with:** iterate the element table the code exposes (or `kElements`) and drop both literals.

### tests/test_framexml_takeover.cpp:48 - "Every element is FrameXML's", "The bar's pieces are owned through the whole bar" (67), "Owning everything means suppressing nothing" (82)
- **Category:** F
- **Severity:** medium
- **Evidence:** `frameXmlOwns()` in src/ui/framexml_takeover.cpp:127 is `if (!frameXmlLoaded()) return false; if (frameXmlWasReleased(element)) return false; return true;`
- **Why:** With nothing released and the environment cleaned, every element is owned by construction, and `frameXmlSuppressedFrames()` skips every row for the same reason. These cases only fail if the environment variable is set. The paths that carry the real risk (an element released because its frames never arrived, and the suppression list it then produces) are untested.
- **Replace with:** release one element through the safety net (its frames never built), then assert `frameXmlOwns(element)` is false and its frames appear in `frameXmlSuppressedFrames()`. Also assert another element stays owned.

### tests/test_frustum.cpp:25 - "Frustum extractFromMatrix with identity", "Frustum containsPoint perspective" (34)
- **Category:** G
- **Severity:** high (tests dead code)
- **Evidence:** `REQUIRE(f.containsPoint(glm::vec3(0, 0, -10)));`
- **Why:** `Frustum::containsPoint` (src/rendering/frustum.cpp:77) has no caller outside frustum.cpp/.hpp. The renderers use `intersectsSphere` and `intersectsAABB`.
- **Replace with:** delete `containsPoint` and both cases. Express the far-plane and behind-camera points as `intersectsSphere(p, 0.01f)` cases if wanted.

### tests/test_frustum.cpp:11 - "Plane distanceToPoint" (also 106 "Frustum getPlane returns 6 planes")
- **Category:** C, F
- **Severity:** low
- **Evidence:** `Plane p(glm::vec3(0.0f, 1.0f, 0.0f), -5.0f); REQUIRE(p.distanceToPoint(...) == 5.0f);` and `// Access all 6 planes - should not crash ... REQUIRE(len == Approx(1.0f))`
- **Why:** Plane is the internal step of the intersect tests. The unit-normal check catches missing normalisation only indirectly. Normalisation matters for sphere radii, and that is better asserted through `intersectsSphere` near a side plane.
- **Replace with:** a sphere just outside and just inside a side plane at a known distance, through `intersectsSphere`.

### tests/test_escape_action.cpp:102 - "Each window closes itself when it is the only one open"
- **Category:** A, F
- **Severity:** low
- **Evidence:** `CHECK(sizeof(kCases) / sizeof(kCases[0]) == 15u);`
- **Why:** This counts the test's own array. It cannot fail on any change to `resolveEscape`. A new branch in the code does not change `kCases`, which is the opposite of the comment's claim.
- **Replace with:** delete the line. If coverage of every `EscapeAction` matters, iterate the enum, as the "Every action has a name" case does, and require one case per action.

### tests/test_escape_action.cpp:35 - "And then opens the game menu"
- **Category:** G
- **Severity:** low
- **Evidence:** `resolveAfterInterface(false, /*frameXmlOwnsMenu=*/true)` and `(false, false)` are both expected to be `ToggleInterfaceMenu`. The source has `(void)frameXmlOwnsMenu;`.
- **Why:** This pins a parameter the function ignores ("kept so the shape of the question survives"). AGENTS.md says code nothing uses is deleted.
- **Replace with:** remove the parameter and keep the two `interfaceClosedAPanel` cases.

### tests/test_format_lookup.cpp:136 - "the table names the extension each format actually writes"
- **Category:** C
- **Severity:** low
- **Evidence:** `std::regex magicRe(R"RX(constexpr char kMagic\[4\]\s*=...)RX"); ... directory_iterator(kPipelineDir)`
- **Why:** This regex-parses `.cpp` source text to compare two declarations of the same fact. It breaks if a loader spells the constant differently (for example `std::array`, or one shared header), while the behaviour is unchanged. It did catch a real bug, but the cause is two copies of one fact.
- **Replace with:** have the format table take each format's extension and magic from the loader's exported constants (one source), or write a file with each loader and look it up by its extension and magic.

### tests/test_combat_domain.cpp:132 - "Tab steps through enemies nearest first..." (also 293 in "the ghost flag makes the player a ghost...", 385 in "auto-attack reaches 8 yards...")
- **Category:** C
- **Severity:** low
- **Evidence:** `h.game().world().applyRecordedObjects(batch);` and `h.game().inventory().items().setEquipSlot(EquipSlot::RANGED, bow)`
- **Why:** This skips SMSG_UPDATE_OBJECT decoding and the inventory update path. GameHarness has `createObjectBody` and `valuesBody` (used in test_game_data_domain.cpp:196), and `playerFields` is used later in this same file.
- **Replace with:** `h.receive(SMSG_UPDATE_OBJECT, GameHarness::createObjectBody(...))` and `h.playerFields(...)`. Equip the bow through the inventory update fields.

### tests/test_dungeon_finder.cpp:142 - "The queue status puts the player in the queue and keeps its numbers" (also 265 "A finished run keeps its reward...")
- **Category:** B
- **Severity:** low
- **Evidence:** `s.waitTank = 120; ... f.queueStatusReceived(s); CHECK(f.queue().waitTank == 120);`
- **Why:** These are partly set-then-read of a stored struct. The state change and the returned events are real behaviour. The copied fields are not.
- **Replace with:** keep the state and event checks and drop the field echo (or check it where the interface reads it, through the Lua `GetLFGQueueStats` path).

### tests/test_dungeon_finder.cpp:373 - "The dungeon being queued for is named from the table"
- **Category:** E
- **Severity:** low
- **Evidence:** the fake's `dungeonName(48)` returns `"Blackfathom Deeps"`, and the test asserts `getCurrentLfgDungeonName() == "Blackfathom Deeps"`.
- **Why:** This asserts that the service returns what the fake was set to return. It does verify that the queued id is the one passed in, but only weakly.
- **Replace with:** a GameHarness test with an LFGDungeons.dbc row (as test_game_data_domain.cpp:154 does) and SMSG_LFG_QUEUE_STATUS. Assert the name the interface gets.

### tests/test_equipment_hash.cpp:61 - "wearing nothing is stable"
- **Category:** F
- **Severity:** low
- **Evidence:** `CHECK(hashEquipmentAppearance({}) == hashEquipmentAppearance({}));`
- **Why:** For a pure function this cannot fail unless the result is nondeterministic, and the comment's concern ("whatever the vector held before") is not set up.
- **Replace with:** assert that an empty outfit differs from one with a single item, or delete the case.

### tests/test_client_transitions.cpp:498 - "when every reconnect attempt fails the character leaves the world for the login screen"
- **Category:** B
- **Severity:** low
- **Evidence:** `CHECK(h.renewalsBegun() == wowee::core::WorldReconnect::kMaxAttempts);` while the comment says "Five attempts".
- **Why:** The expected value comes from the implementation's constant, so a wrong attempt count would pass.
- **Replace with:** the literal `5`, from the requirement.

### tests/test_collision_world.cpp:200 - "a building's floor is the highest under the probe, or the nearest the feet"
- **Category:** F
- **Severity:** low
- **Evidence:** `bool facesUp = false; world.wmoFloor(..., &normalZ, std::nanf(""), &facesUp); CHECK(normalZ == Approx(1.0f));`
- **Why:** `facesUp` is requested and never asserted, so a broken `facesUp` output would pass.
- **Replace with:** add `CHECK(facesUp)`, plus a case with a downward-facing surface.

## 3. Good examples worth citing

- **tests/test_dxt_block.cpp:** every expected value is derived from the S3TC rules (thirds, RGB565 maxima, little-endian index word), and each case names the realistic bug it catches (transposed indices, `>=` on the mode flag).
- **tests/test_collision_geometry.cpp:** the oracle is independent. A brute-force surface scan checks `pointAABBDistanceSq`, and a randomised winding sweep includes a guard (`hits > 20`) so the sweep cannot pass vacuously.
- **tests/test_collision_world.cpp:** it drives `CollisionWorld` through its public API with hand-built shapes and no device, and asserts floors, walls and sight lines as they are placed, moved, hidden and removed. This is the pattern AGENTS.md prescribes for that layer.
- **tests/test_contacts_domain.cpp / tests/test_duel_domain.cpp:** GameHarness end to end. Packets go in, then the test reads the state the interface reads, the addon events, the chat lines and the bytes of the reply packet, including the relog-does-not-inherit case.
- **tests/test_client_transitions.cpp:** ClientHarness drives real frames through login, map change, logout, disconnect and reconnect, and asserts what the player, the interface and the server see.
