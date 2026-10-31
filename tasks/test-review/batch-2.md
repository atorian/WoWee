# Test review, batch 2

Rules applied: AGENTS.md "Tests test behaviour" (no constants, no tautologies, front door, a context tested on its own, what runs on the GPU is tested on the GPU). Read-only review; nothing was built or run. Dead-code check: every function and class these tests exercise is referenced from `src/`, `include/` or `tools/` outside the tests. The one exception is `JitWriteWindow::openWindows()`, which only the tests call. Context isolation: `test_group_model`, `test_group_service`, `test_guild_model`, `test_guild_service` and `test_guild_bank` each link only their own context library (tests/CMakeLists.txt:788-817), so there are no H findings.

## 1. Files

| File | Verdict | Categories | Note |
|---|---|---|---|
| tests/test_game_object_animation.cpp | OK | | Pure rule from WoW.exe, inputs/outputs symbolic. |
| tests/test_game_tables.hpp | OK | | Test helper (GivenTables/GivenLayouts), not a test. |
| tests/test_gameobject_query_layout.cpp | minor | F, C | Wire tests good; the truncation test asserts nothing, two others pin a wrong parse / the remaining-bytes size. |
| tests/test_gamepad.cpp | minor | A | Stick/trigger/pointer maths good; a few table-shape counts pin the scheme. |
| tests/test_gather_spells.cpp | minor | F | Negative cases only: `return false` / `return nullptr` passes the whole file. |
| tests/test_geoset_rules.cpp | minor | A, B | Mostly strong; two constant pins and one restated bit-pack formula. |
| tests/test_gltf_terrain_bin.cpp | OK | | Oracle is the glTF spec; editor tool code is live (tools/editor/cli_bake.cpp). |
| tests/test_gm_commands.cpp | minor | A, F | Completion tests fine; "table > 50" and "key commands exist" copy the table. |
| tests/test_gpu_memory.cpp | OK | | Literal VMA map in, classified bytes out. |
| tests/test_grass_biomes.cpp | minor | C | Reads `set.biomes_[1]` storage instead of `biome(2)`. |
| tests/test_grass_blade_layout.cpp | whole file should go | B, A | sizeof/offsetof literals, already `static_assert`ed in the header. |
| tests/test_grass_clearing.cpp | OK | | |
| tests/test_grass_cull_math.cpp | problem | D | Copies the cull shader's rejection loop into C++. |
| tests/test_grass_population.cpp | OK | | Property tests (world-anchored identity, slicing equivalence). |
| tests/test_grass_profile.cpp | OK | | |
| tests/test_grass_terrain.cpp | OK | | |
| tests/test_group_domain.cpp | OK | | GameHarness end to end, including relog. |
| tests/test_group_list_parse.cpp | OK | | Bytes per AzerothCore; regression for the LFG shape. |
| tests/test_group_model.cpp | OK | | Links wowee_group only. |
| tests/test_group_service.cpp | OK | | Recording gateway is the port boundary; links wowee_group only. |
| tests/test_guild_bank.cpp | OK | | Links wowee_guild_bank only. |
| tests/test_guild_bank_domain.cpp | OK | | GameHarness, event order checked. |
| tests/test_guild_bank_list.cpp | OK | | |
| tests/test_guild_domain.cpp | OK | | |
| tests/test_guild_event_money.cpp | OK | | |
| tests/test_guild_model.cpp | OK | | |
| tests/test_guild_roster_layout.cpp | minor | F | Truncation loop only checks `size() <= 2`. |
| tests/test_guild_service.cpp | OK | | |
| tests/test_host_image_copy.cpp | OK | | |
| tests/test_indoor_shadows.cpp | whole file should go | D | Tests only its own two replica functions, and the production code has moved on from them. |
| tests/test_install_probe.cpp | OK | | Real directory trees; code lives in tools/asset_manager. |
| tests/test_instance_index_set.cpp | OK | | Randomised against a rebuild oracle. Note: the `World::remove` driver re-states M2Renderer's swap-remove choreography. |
| tests/test_instance_random.cpp | OK | | |
| tests/test_integrity_hash.cpp | OK | | Independent oracle (TrinityCore checksum). |
| tests/test_interface_frame_scripts.cpp | OK | | ClientHarness + Lua, front door. |
| tests/test_interface_text_lines.cpp | OK | | |
| tests/test_interior_light.cpp | problem | D | Three tests `#include` interior_light.glsli and run the shader on the CPU. |
| tests/test_inventory_count.cpp | OK | | |
| tests/test_inventory_domain.cpp | OK | | |
| tests/test_inventory_slots.cpp | problem | A | Bank layout and wire-slot numbers asserted against the literals in inventory_slots.hpp. |
| tests/test_inventory_sort.cpp | OK | | Plan-vs-sort agreement is a good oracle; both paths are used by GameHandler::sortBags. |
| tests/test_item_query_layout.cpp | minor | B, F | Self-declared: bytes laid out from what the readers expect; high-bit test is weak. |
| tests/test_item_query_sockets.cpp | OK | | AzerothCore field order; fails on the old reading. |
| tests/test_item_stat_names.cpp | minor | B | One loop compares pointers that are equal by delegation. |
| tests/test_item_tables.cpp | OK | | GivenTables front door; stop asserted. |
| tests/test_jit_write.cpp | minor | A, C, F, G | Two real tests; two others check a constexpr flag and a test-only depth counter. |
| tests/test_job_group.cpp | OK | | |
| tests/test_json_table_scan.cpp | OK | | |
| tests/test_learn_talent_packet.cpp | OK | | Wire bytes vs server rule. |
| tests/test_lens_drops.cpp | minor | F | One branch passes when the drop disappears. |
| tests/test_lfg_domain.cpp | OK | | |
| tests/test_lfg_join_packet.cpp | OK | | |
| tests/test_light_bands.cpp | problem | A, F | 7 of 11 tests assert constants, JSON text or test-local literals. `lightBandSlot` tests are good. |
| tests/test_light_coords.cpp | minor | A, B, F | Zone-containment test is excellent; three side assertions are weak. |
| tests/test_light_headroom.cpp | minor | F | One block asserts `glm::min` on a local value. |
| tests/test_light_params_eval.cpp | OK | | |
| tests/test_light_volume_blend.cpp | OK | | Front door through LightingManager with given DBCs. |

Totals: OK 38, minor 13, problem 4, whole file should go 2 (57 files).

## 2. Findings

### tests/test_indoor_shadows.cpp:20 - whole file (8 TEST_CASEs)
- Category: D (REPLICA)
- Severity: high
- Evidence:
  ```cpp
  // Replicates the shadow params logic from Renderer::updatePerFrameUBO()
  static void applyShadowParams(GPUPerFrameData& fd, bool shadowsEnabled, float shadowDistance = 300.0f) {
  // Replicates the WMO interior shader logic:
  static float computeWmoShadowFactor(bool isInterior, float globalShadowEnabled, float rawShadow) {
  ```
- Why: every assertion checks the file's own two functions. Production has already moved away from them. `src/rendering/renderer.cpp:961` now writes `shadowParams = vec4(shadows, 0.0f, 1/SHADOW_MAP_SIZE, ...)`, so `.y` is no longer a distance-scaled bias, and `assets/shaders/wmo.frag.glsl` has no `isInterior` branch. Yet "Shadow bias scales with shadow distance" (line 93) still passes. The file can't catch any bug in the client.
- Replace with: a `scene_render` scene of a WMO interior group under sun shadows, with a `check_*.py` that the interior is not darkened. Delete the file.

### tests/test_grass_blade_layout.cpp:22 - "GrassBladeGPU matches the std430 GrassBlade" / :49 "GrassCullUniformsGPU matches the std140 uniform block"
- Category: B (compiler guarantee), A (offsets are literals copied from a comment)
- Severity: high
- Evidence:
  ```cpp
  REQUIRE(sizeof(GrassBladeGPU) == 64);
  REQUIRE(offsetof(GrassBladeGPU, facingWidthPhase) == 16);
  REQUIRE(offsetof(GrassCullUniformsGPU, cameraPos) == 96);
  ```
- Why: `include/rendering/grass_blade.hpp:46-77` already has `static_assert`s for exactly these sizes and offsets. A drift fails the build before this test runs. The test can't tell the C++ struct and the GLSL apart either, because the shader is never read.
- Replace with: nothing. Keep the `static_assert`s. Whether the shader reads the blade correctly is a `scene_render` grass scene.

### tests/test_grass_cull_math.cpp:29 - all three TEST_CASEs ("the cull keeps grass ahead...", "...rejects grass behind...", "no bearing keeps a different amount...")
- Category: D (CPU copy of shader code)
- Severity: high
- Evidence:
  ```cpp
  /// The rejection grass_cull.comp.glsl makes, verbatim: outside if any plane
  bool culledByShader(const Frustum& frustum, const glm::vec3& center, float radius) {
      if (glm::dot(plane.normal, center) + plane.distance < -radius) return true;
  ```
- Why: this is the forbidden pattern, a replica of `grass_cull.comp.glsl:84-88`. It has already drifted: the shader sizes the sphere as `max(height, width) * 0.75` around `root + height/2`, and the test uses a fixed radius of 0.3 at +0.2. The only production code exercised is `Frustum::extractFromMatrix`.
- Replace with: drive the CPU half through `Frustum::intersectsSphere` (public, include/rendering/frustum.hpp:63) for every bearing. The shader half goes in a `scene_render` grass scene turned through 16 bearings with a per-bearing coverage check.

### tests/test_interior_light.cpp:234 - "a doodad inside is lit by the WMO's ambient and its MODD colour from above" (also :253 "a model outdoors keeps the frame's ambient and the sun", :264 "a WMO with an all-zero ambient leaves the frame's ambient on models inside")
- Category: D
- Severity: high
- Evidence:
  ```cpp
  namespace interior_light {
  #include "../assets/shaders/interior_light.glsli"
  }
  REQUIRE(interior_light::interiorAmbient(gpu.ambient, frameAmbient) == mohd);
  ```
- Why: this compiles the shader include for the CPU, which the 2026-10-31 decision forbids. GPU precision, packing into push constants and the `insideInterior` test in character.frag.glsl:358 are never exercised. The rest of the file (CPU `unitInteriorLight`, `interiorDoodads`, MOHD/MODR parsing, `InteriorLightProbe`) is fine.
- Replace with: a `scene_render` scene with a doodad inside and outside a WMO interior, plus a check on the lit colours. Keep `packInteriorLight` covered there.

### tests/test_light_bands.cpp:148 - "fog distances are in the same unit as the light positions"
- Category: A, F
- Severity: high
- Evidence:
  ```cpp
  CHECK(raw[i] / wowee::rendering::LIGHT_COORD_UNITS_PER_YARD == Catch::Approx(yards[i]).epsilon(1e-3));
  CHECK(r > 1900.0f);
  ```
- Why: the first check only asserts the constant is 36. The second compares two test literals and can never fail. No code that converts a fog band runs. If `sampleLightParams` stopped dividing, this would still pass.
- Replace with: delete it. test_light_params_eval.cpp:64 already feeds FOG_END=12000 through `sampleLightParams` and expects 333.33.

### tests/test_light_bands.cpp:173 - "channel 2 is a switch, not the cloud density"
- Category: A, F
- Severity: high
- Evidence:
  ```cpp
  CHECK(wowee::rendering::LightParamsProfile::CELESTIAL_GLOW_THROUGH == 2);
  CHECK(wowee::rendering::LightParamsProfile::CLOUD_DENSITY == 3);
  for (float v : channel2) CHECK(v == 1.0f);
  ```
- Why: enum values against literals, followed by checks on the test's own array.
- Replace with: test_light_params_eval.cpp:83 ("the cloud curve is not read as fog density") already sets the band and reads `cloudDensity`. Add a case where channel 2 is set and `cloudDensity` stays at the fallback.

### tests/test_light_bands.cpp:193 - "a band colour has red in the high byte"
- Category: F (no production code at all)
- Severity: high
- Evidence:
  ```cpp
  const uint32_t noonDiffuse = 0xFFE0A9;
  CHECK(((noonDiffuse >> 16) & 0xFF) == 0xFF);
  ```
- Why: shifts and masks a literal inside the test. The decoder in LightingManager is never called, so swapping R and B in production would not fail this.
- Replace with: give `LightIntBand.dbc` a row of `0xFF0000` and assert `getLightingParams()` comes out red. test_light_volume_blend.cpp already has the scaffolding (`skyAt`).

### tests/test_light_bands.cpp:218 - "the sky gradient says where the channels are" (also :243 "channel 0 is the sunlight and channel 1 is the ambient", :280 "channel 7 is the fog and channel 6 is the smog above it")
- Category: A, F
- Severity: high
- Evidence:
  ```cpp
  CHECK(Profile::SKY_TOP_COLOR == 2);
  CHECK(Profile::DIFFUSE_COLOR == 0);
  CHECK(Profile::FOG_COLOR == 7);
  CHECK(luma(skyTop) < luma(skyMiddle));   // test-local arrays
  ```
- Why: enum constants against literals, plus luma and channel comparisons on arrays defined in the test. Wiring channel 6 to `fogColor` in LightingManager would still pass.
- Replace with: through `LightingManager` with given tables, put distinct colours in channels 0, 1, 2 and 7. Assert `diffuseColor`, `ambientColor`, `skyTopColor` and `fogColor` each get theirs.

### tests/test_light_bands.cpp:104 - "every expansion declares the band fields where they are" (also :128 "every expansion reads Light.dbc's weather columns in file order")
- Category: A
- Severity: medium
- Evidence:
  ```cpp
  const std::string body = json.substr(at, 220);
  CHECK(body.find("\"NumKeyframes\": 1") != std::string::npos);
  CHECK(body.find("\"LightParamsIDStorm\": 9") != std::string::npos);
  ```
- Why: substring search over the shipped `dbc_layouts.json` copies the data file into the test. The fixed 220-character window also breaks on reformatting. It catches a wrong layout entry, but only by repeating it.
- Replace with: load a real or literal LightIntBand row through the expansion's layout and assert the keyframe count and time that `LightingManager` reads. For the weather columns, use `selectLightParamsId` on a volume read through the layout.

### tests/test_inventory_slots.cpp:18 - "Each expansion's bank is where that expansion puts it" (also :36, :45)
- Category: A
- Severity: medium
- Evidence:
  ```cpp
  CHECK(vanilla.bagFirst == 63);
  CHECK(later.keyringFirst == 86);
  REQUIRE(firstBankBagInventorySlot() == 68);
  REQUIRE(backpackWireSlot(0) == 23);
  ```
- Why: `bankLayoutFor` is `classicLike ? BankLayout{24, 63, 6, 81} : BankLayout{28, 67, 7, 86}` (include/game/inventory_slots.hpp:70-72). The test repeats those literals. The comment cites the cores, but AGENTS.md says a value that must agree with the server is tested through what it does. Line 42 (`toWireSlot(firstBankBagInventorySlot()) == bankBagWireSlot(0)`) is a real round-trip and can stay.
- Replace with: GameHarness. Swap an item into the first bank bag slot via the Lua or `inventory()` API on Classic and WotLK and read the bag/slot bytes of `CMSG_SWAP_ITEM`.

### tests/test_gather_spells.cpp:15 - "a spell that is not a gather is not one" / :25 "anything but a base id selects nothing"
- Category: F
- Severity: medium
- Evidence:
  ```cpp
  CHECK_FALSE(isGatherRank(id));
  CHECK(gatherRanksForBase(2576, count) == nullptr);
  ```
- Why: only negatives. `isGatherRank` returning `false` always, or `gatherRanksForBase` returning `nullptr` always, passes both. Losing every rank, the failure the CMake comment names, is not caught. The file's comment admits the positive case belongs elsewhere, but no harness test exists for it.
- Replace with: a GameHarness test that casts a mining rank on a node, or at least `isMiningRank(<real rank id>)` alongside `gatherRanksForBase(<base>)` returning a non-empty list.

### tests/test_gameobject_query_layout.cpp:174 - "a truncated response stops rather than reading past the end"
- Category: F
- Severity: medium
- Evidence:
  ```cpp
  GameObjectQueryResponseParser::parse(packet, data);
  }
  SUCCEED("no read past the end at any length");
  ```
- Why: no assertion. Without ASan an over-read is invisible, and the result of `parse` is ignored. Compare test_guild_roster_layout.cpp:236, which at least checks a bound.
- Replace with: for each cut, `CHECK_FALSE(parse(...))` or `CHECK_FALSE(data.hasData)`, and check the read position never passes `size()`.

### tests/test_gameobject_query_layout.cpp:117 - "one string too few shifts every data field" (also :130 "what is left after the data fields says whether the count was right")
- Category: F, C
- Severity: low
- Evidence:
  ```cpp
  CHECK(data.data[0] != 0xA000);           // TBC reader fed a WotLK packet
  CHECK(packet.getRemainingSize() % 4 != 0);
  ```
- Why: the first pins a wrong answer, and would fail if TBC parsing got smarter. The second asserts on the packet's read cursor, an internal step, rather than on the warning the comment says the parser logs.
- Replace with: drop :117 (covered by the TBC/WotLK cases). For :130, assert the warning, e.g. via a log capture, or remove.

### tests/test_geoset_rules.cpp:223 - "the ears are the variant that has ears on it" (also :241 "the belt's base variant is the waist, not nothing")
- Category: A
- Severity: low
- Evidence:
  ```cpp
  CHECK(wowee::core::kGeosetDefaultEars == 702);
  CHECK(wowee::core::equipment::kBeltBase == 1801);
  ```
- Why: constants against their definitions (geoset_rules.hpp:35, :225). The neighbouring lines (`bare.count(kGeosetDefaultEars)`, `resolveGeoset(beltGeoset(0), legion) == 1801` at :280) already test behaviour.
- Replace with: delete the two constant lines; the stock-model `resolveGeoset` cases cover them.

### tests/test_geoset_rules.cpp:126 - "the appearance key packs three bytes and nothing else"
- Category: B
- Severity: low
- Evidence:
  ```cpp
  CHECK(appearanceKey(1, 1, 5) == ((1u << 16) | (1u << 8) | 5u));
  ```
- Why: restates the implementation (geoset_rules.hpp:127-129). The collision sections below it state the real requirement.
- Replace with: keep only the "no two characters share a key" and "a byte cannot reach another" sections.

### tests/test_gm_commands.cpp:15 - "GM command table is non-empty" (also :87 "Key GM commands exist in table")
- Category: A, F
- Severity: low
- Evidence:
  ```cpp
  REQUIRE(kGmCommands.size() > 50);
  CHECK(names.count("cheat god"));
  ```
- Why: copies table contents into the test. Fails only when someone edits the table.
- Replace with: completion behaviour (`gmCompletionsFor(".che")` contains ".cheat god"), which the file already does for .gm/.tele/.add.

### tests/test_gamepad.cpp:389 - "the rows with no command are the ones the interface has no word for" (also :112 "the scheme reaches the whole action bar")
- Category: A
- Severity: low
- Evidence:
  ```cpp
  CHECK(commandless.size() == 1);
  CHECK(plain.size() == 6);
  ```
- Why: pins the shape of the binding table. The real invariants (unique buttons/jobs, command/key agreement at :369) are already covered.
- Replace with: keep `commandless.count(LEFT_SHOULDER) == 1`; drop the exact counts.

### tests/test_item_query_layout.cpp:47 - "classic item query: no SoundOverrideSubclass" / "TBC item query: ..." (and :153 "an entry with the high bit set...")
- Category: B, F
- Severity: low
- Evidence:
  ```cpp
  // The bytes here are laid out from what the readers themselves expect, so this
  // proves a change preserves behaviour, not that the behaviour was ever right.
  parsers.parseItemQueryResponse(packet, data);   // result ignored
  CHECK(data.name.empty());
  ```
- Why: the expected layout comes from the code under test, a characterization test that admits it. The high-bit case ignores the return value and only checks a default-empty field, so a parser that read garbage into other fields would pass.
- Replace with: lay the bytes out from a server source (CMaNGOS/AzerothCore classic/TBC ItemPrototype writer), as test_item_query_sockets.cpp does. For the high bit, assert the "not found" result and that `entry` is masked.

### tests/test_item_stat_names.cpp:51 - "itemModStatName answers the extra stats the same way"
- Category: B
- Severity: low
- Evidence:
  ```cpp
  CHECK(itemModStatName(t) == itemStatName(t));   // const char* equality
  ```
- Why: `itemModStatName`'s `default:` returns `itemStatName(statType)` (item_text.hpp:142), so pointer equality holds by construction. Copying the strings into a second table would make it fail falsely. The literal checks after it carry the value.
- Replace with: compare `std::string` values only where a name exists, or keep just the literal checks.

### tests/test_jit_write.cpp:85 - "the window follows the mapping, not the platform" (also :103 "the window closes back to none")
- Category: A, F, C, G
- Severity: low
- Evidence:
  ```cpp
  SUCCEED("the module image is mapped MAP_JIT here");
  CHECK_FALSE(JitWriteWindow::required());           // constexpr
  CHECK(JitWriteWindow::openWindows() == 1);
  ```
- Why: :85 checks a compile-time flag, or nothing. :103 reads `openWindows()`, a depth counter only tests call (grep: no use in src/). The two MAP_JIT write tests above are the behaviour.
- Replace with: drop :103 and the `openWindows()` accessor. For :85, the behaviour is "constructing a window where no MAP_JIT exists does not trap", already exercised by `JitWriteWindow noop`.

### tests/test_grass_biomes.cpp:53 - "the biome table parses, skipping malformed entries"
- Category: C
- Severity: low
- Evidence:
  ```cpp
  REQUIRE(set.biomes_[1].name == "Wheatland");
  ```
- Why: reaches the storage vector, public but named as a member. Nothing outside grass_biomes.hpp uses it.
- Replace with: `REQUIRE(set.biome(2)->name == "Wheatland")`.

### tests/test_guild_roster_layout.cpp:224 - "a truncated roster stops rather than reading past the end"
- Category: F
- Severity: low
- Evidence:
  ```cpp
  GuildRosterParser::parse(packet, data);
  CHECK(data.members.size() <= 2);
  ```
- Why: the comment says it must not report success with garbage, but the return value is ignored. `size() <= 2` holds for nearly any over-read.
- Replace with: `CHECK_FALSE(parse(...))` for every cut short of the full packet, or assert the members that were fully present.

### tests/test_light_coords.cpp:24 - "a radius divided by the unit lands on the tile grid" (also :78, :106)
- Category: A, B, F
- Severity: low
- Evidence:
  ```cpp
  CHECK(19200.0f / LIGHT_COORD_UNITS_PER_YARD == Catch::Approx(wowee::core::coords::TILE_SIZE).epsilon(1e-4));
  CHECK(pos.x == Catch::Approx(wowee::core::coords::ZEROPOINT - 604416.0f / 36.0f) ...);
  CHECK(std::abs(564790.0f) > wowee::core::coords::ZEROPOINT);
  ```
- Why: :24 asserts the constant. :78 restates the conversion formula. :106 compares a literal with a constant and can't fail. The real-zone containment test at :35 is the behaviour and is excellent.
- Replace with: keep :35 and :86 (corner); delete the three lines above.

### tests/test_light_headroom.cpp:60 - inside "the hue survives, which is the whole point"
- Category: F
- Severity: low
- Evidence:
  ```cpp
  const glm::vec3 clipped = glm::min(before, glm::vec3(1.0f));
  CHECK(clipped.r == 1.0f);
  ```
- Why: asserts what `glm::min` does to a local value; no code under test.
- Replace with: delete lines 60-65; the ratio checks above are the behaviour.

### tests/test_lens_drops.cpp:96 - "Big drops run down the lens and leave beads behind"
- Category: F
- Severity: low
- Evidence:
  ```cpp
  if (later != lens.drops().end()) { CHECK(later->pos.y > startY); }
  else { SUCCEED("ran off the bottom of the lens"); }
  ```
- Why: a bug that deletes a running drop passes the else branch. The "leave beads behind" in the title is never asserted.
- Replace with: use a shorter step so the drop is still on the lens, and assert it moved down and a new small drop appeared above its start.

## 3. Good examples

- **tests/test_light_volume_blend.cpp**: goes through `LightingManager::initialize`/`update` with WDBC tables built in the test and reads `getLightingParams()`. Expected colours are the rows' own colours and their linear mix, from WoW.exe's rule rather than the implementation.
- **tests/test_group_domain.cpp**: GameHarness end to end. A server packet goes in; the test checks what the interface is told (addon events and their args), the chat line, the packet sent back, and that a relog leaves nothing of the previous character.
- **tests/test_item_query_sockets.cpp**: builds the packet field by field from AzerothCore's writer. The key assertion (`socketColor[1] == 4`) is named as the one that fails on the old colour-then-content reading.
- **tests/test_light_coords.cpp:35**: real Light.dbc rows checked against published world coordinates of the zones they light. The oracle is independent of the code, and the comment explains why the other axis orderings fail.
- **tests/test_item_tables.cpp**: drives `ui::factionNames`/`itemSets` through `GivenTables` and `GivenLayouts`. It asserts the stop on a missing or narrow column and that an unused table is never opened, which is the expansion-data rule in AGENTS.md, tested through the front door.
