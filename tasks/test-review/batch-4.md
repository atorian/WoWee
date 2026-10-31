# Test review, batch 4

Rules: AGENTS.md "Tests test behaviour" (plus the user decision that GPU code is tested on the GPU). All 60 files in the batch were read in full. Production use was checked with grep over `src/`, `include/`, `tools/` and `assets/shaders/`.

## 1. Files

| file | verdict | categories | note |
|---|---|---|---|
| tests/test_normal_map.cpp | OK | - | Hand-worked expected values for the Sobel/height map. |
| tests/test_normal_map_builder.cpp | OK | - | Queue order and refusal through the public builder API. |
| tests/test_normal_map_encoding.cpp | OK | (B low) | Uses its own BC7 mode-6 decoder written from the format spec, so the oracle is independent. Line 119 is set-then-get on `heightVariance`. |
| tests/test_npc_flags.cpp | whole file should go | A, B | Asserts every `NPC_FLAG_*` define against a literal and checks `NPC_FLAG_ANY_VENDOR` against its own definition. |
| tests/test_obj_parse.cpp | OK | - | The oracle is the OBJ format. Code under test is the editor CLI parser. |
| tests/test_ocean_fft.cpp | whole file should go | D | Compiles `ocean_spectrum.glsli` and `ocean_fft.glsli` for the CPU and re-implements `ocean_fft.comp`'s butterfly loop in the test. |
| tests/test_ocean_spectrum.cpp | problem | D | `OceanSea` tests are real CPU behaviour. The grid/spectrum tests run GPU-only shader functions compiled for the CPU. |
| tests/test_opcode_table.cpp | minor | B (low) | Load and lookup are fine. The last case is set-then-get on the global table pointer. |
| tests/test_opcode_tables.cpp | problem | A, F | Opcode numbers copied in from AzerothCore, and "a name exists in the JSON" checks. The duplicate-number check is a reasonable data lint. |
| tests/test_open_format_emitter.cpp | OK | - | Real DBC bytes in, JSON read back through `DBCFile`. |
| tests/test_open_formats.cpp | problem | F, B | Four cases never call production code: they write bytes, read them back, or compare two literals. The rest are good round-trip and hardening tests. |
| tests/test_override_add.cpp | whole file should go | D | Tests `resolveLikeAssetManager`, a copy of `AssetManager::resolveFile` kept in the test. |
| tests/test_packed_time.cpp | OK | - | The expected packing is the server's own `AppendPackedTime` formula. |
| tests/test_packet.cpp | minor | B, F (low) | Round trips are fine. The default and opcode constructor cases are getter-of-constructor-argument. |
| tests/test_particle_density_floor.cpp | OK | - | Literals come from the 3.3.5 client spec arithmetic. |
| tests/test_pass_ablation.cpp | OK | - | Phase walk and report text from fed frame times. |
| tests/test_perf_trace.py | OK | - | Drives `tools/perf_trace.py` on synthetic traces and checks the report and comparison. |
| tests/test_pet.cpp | OK | - | Context-isolated: links only `wowee_pet`, uses a recording gateway port and the real `EventBus`. |
| tests/test_pet_action.cpp | OK | - | Wire packing against literal protocol values. |
| tests/test_pet_domain.cpp | OK | - | `GameHarness` end to end. |
| tests/test_petition_domain.cpp | OK | - | `GameHarness` end to end. |
| tests/test_petitions.cpp | OK | - | Context-isolated, links only `wowee_petition`. |
| tests/test_pipeline_cache_file.cpp | OK | - | Save/load and refusal for another device, through the public API. |
| tests/test_placement_rotation.cpp | whole file should go | D, G | Includes no production code. It compares two local glm compositions, and the "doodad path composes XYZ" it describes no longer exists. |
| tests/test_placement_transform.cpp | OK | - | Independent oracle: the bounds Blizzard's MODF records carry. |
| tests/test_player_domain.cpp | minor | C (low) | Harness end to end. One case reads the raw field map. |
| tests/test_player_model_path.cpp | OK | - | - |
| tests/test_plural_escape.cpp | OK | - | - |
| tests/test_polygon_triangulate.cpp | minor | B (low) | One expected area is computed with the code-under-test helper `signedAreaX2`. |
| tests/test_pom_quality_one_scale.cpp | problem | A, B, C | Expected values come from the table under test, plus grep tests over `.cpp` source text. |
| tests/test_prewotlk_build_packets.cpp | OK | - | Built bytes checked against the wire layout. |
| tests/test_query_cache_domain.cpp | OK | - | Harness end to end. |
| tests/test_quest_domain.cpp | OK | - | Harness end to end. |
| tests/test_quest_giver_status.cpp | OK | - | Server `DIALOG_STATUS_*` values in, marker out. |
| tests/test_quest_progress.cpp | OK | - | - |
| tests/test_quest_rewards.cpp | OK | - | Byte-for-byte server layouts decoded. |
| tests/test_quest_text.cpp | OK | - | - |
| tests/test_race_gender_suffix.cpp | OK | - | Literals are archive file names. |
| tests/test_raid_mark_names.cpp | minor | A, C (low) | Asserts `kRaidMarkCount == 8`, and checks the `/mark` word list by calling `toLower(raidMarkName())` instead of running `/mark`. |
| tests/test_raid_target_packets.cpp | OK | - | - |
| tests/test_rain_audio.cpp | OK | - | `RainMix` gains, plus device-output RMS tests that are opt-in. |
| tests/test_range_allocator.cpp | OK | - | - |
| tests/test_realm_list.cpp | OK | - | - |
| tests/test_realm_patch_sync.cpp | OK | - | Fake transport, outcome checked on disk. |
| tests/test_realm_patches.cpp | problem | G | `parsePatchManifest` is never called outside tests. The name and directory rules are fine. |
| tests/test_realm_time_domain.cpp | OK | - | Harness end to end. |
| tests/test_recorded_entities.cpp | OK | - | `applyRecordedObjects` and `applySampledPosition` are what `core/scene_replay.cpp` calls. |
| tests/test_recording_size.cpp | OK | - | - |
| tests/test_reputation.cpp | minor | G, C (low) | One case covers a "Faction.dbc not ready" branch that production `FactionData::ready()` never takes. |
| tests/test_reputation_domain.cpp | minor | C (low) | Harness end to end. One case sets hostility maps through `GameHandler` setters instead of feeding the DBC. |
| tests/test_reputation_standing.cpp | problem | A, B | Most expected values are read from `kReputationStandings` itself. |
| tests/test_resource_telemetry.cpp | OK | - | - |
| tests/test_retire_queue.cpp | OK | - | - |
| tests/test_rt_build_budget.cpp | OK | - | - |
| tests/test_rt_bvh.cpp | problem | D, G | The closest-hit and tMax checks run `traceRtClosest`, a CPU traversal used only by tests. The GPU walks `rt_trace_sw.glsli`. |
| tests/test_rt_instance_changes.cpp | OK | (E low) | Counts reads and moves on a fake sink. That count is the behaviour (work skipped), and the drawn state is also checked. |
| tests/test_rt_light_composition.cpp | whole file should go | D | `#include`s `rt_light_composition.glsli` into C++ and tests it on the CPU. |
| tests/test_sample_pick.cpp | OK | - | - |
| tests/test_scene_pick.cpp | OK | - | - |
| tests/test_sea_bed.cpp | problem | D | `OverheadFrame` tests are fine. The fetch and bed tests run GPU-only shader functions, with a CPU copy of `sea_bed_fetch.comp`'s pass loop written in the test. |

Totals: OK 40, minor 7, problem 8, whole file should go 5 (60 files).

Category counts (files with a finding in that category): A 5, B 7, C 5, D 7, E 0, F 3, G 4, H 0. The context tests (`test_pet`, `test_petitions`, `test_reputation`) link only their own library.

## 2. Findings

### tests/test_rt_light_composition.cpp:13 - TEST_CASE "Full RT composes occluded ambient and indirect light" (and :26, :36)
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  namespace rt_math { ... glm::vec4 rtParams(0.0f);
  #include "../assets/shaders/rt_light_composition.glsli"
  }
  rtParams.x = 3; const auto full = rtAmbient(r, ambient);
  ```
- **Why:** The shader include is compiled as C++ with glm, and `rtParams` is faked as a global. The user decision of 2026-10-31 forbids exactly this. The test cannot catch a wrong uniform binding, wrong precision, or a `rtParams` that the real pipeline fills differently. It only checks the glm build of the same text. `rt_light_composition.glsli` is included only by `rt_lighting.glsli`, so no CPU production code runs it.
- **Replace with:** a `scene_render` scene with RT on and off, with a `check_*.py` asserting the occluded ambient and the shadow fallback in the picture.

### tests/test_ocean_fft.cpp:28 - helper `inverseFftLine`, used by TEST_CASEs at :95, :123, :179
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  #include "../assets/shaders/ocean_fft.glsli"
  /// ... as ocean_fft.comp takes a row in shared memory.
  for (int stage = 0; (1 << stage) < n; ++stage) { ... oceanStockhamButterfly(j, stage, n) ...
  ```
- **Why:** The test re-implements the compute shader's stage loop, ping-pong buffers and lane packing on the CPU. A bug in `ocean_fft.comp` itself is invisible here: a missing barrier, a wrong ping-pong swap, the row/column dispatch, or shared-memory indexing. Only the helper functions are shared with the GPU. None of `oceanEvolve`, `oceanSurfaceAt` or `oceanStockhamButterfly` is called from CPU production code.
- **Replace with:** a `scene_render` ocean scene checked for wave height/slope statistics and for periodicity over `kOceanRepeatSeconds` (picture or GPU readback).

### tests/test_override_add.cpp:51 - helper `resolveLikeAssetManager`, used by all four TEST_CASEs (:102, :114, :126, :137)
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  /// resolveFile's order, as src/pipeline/asset_manager.cpp implements it:
  std::string resolveLikeAssetManager(const fs::path& dataPath, const AssetManifest& manifest, ...
  if (fs::exists(overridePath + "/" + byPath)) return overridePath + "/" + byPath;
  ```
- **Why:** Every assertion runs against the test's own copy of `AssetManager::resolveFile` (asset_manager.cpp:155-183). If the real resolver reverts the "override taken as a path" step (the bug the header describes), every test still passes. The file's reason, "AssetManager cannot be built here", is stale: `test_rain_audio.cpp:132-138` builds a real `AssetManager` over `makeDataRoot(...)`.
- **Replace with:** `AssetManager::initialize(makeDataRoot(...))` on a tree whose override directory holds a file the manifest lacks, then assert that `readFile`/`fileExists` returns the pack's bytes.

### tests/test_placement_rotation.cpp:72 - TEST_CASE "An upright placement composes the same either way" (and :82, :96)
- **Category:** D, G
- **Severity:** high
- **Evidence:**
  ```cpp
  /// Z, then Y, then X - what WMOInstance::updateModelMatrix does.
  glm::mat4 composeZYX(const glm::vec3& r) { ... }
  /// X, then Y, then Z - what the doodad path does.
  ```
- **Why:** The file includes no production header. It tests that glm rotations do not commute, which holds whatever the client does. The "doodad path" it describes is gone: `m2_renderer.cpp:69` and `wmo_renderer.cpp:2161` both call `placementModelMatrix`, which `test_placement_transform.cpp` already tests against MODF bounds. The file cannot fail on any change to the client.
- **Replace with:** nothing. Delete it; `test_placement_transform.cpp` covers this.

### tests/test_npc_flags.cpp:19 - TEST_CASE "the npc flag run has no gaps or swaps" (and :41 "every vendor kind counts as a vendor")
- **Category:** A, B
- **Severity:** high
- **Evidence:**
  ```cpp
  CHECK(NPC_FLAG_VENDOR == 0x00000080u);
  CHECK(NPC_FLAG_SPIRIT_HEALER == 0x00004000u);
  CHECK((NPC_FLAG_ANY_VENDOR & NPC_FLAG_VENDOR) != 0u);
  ```
- **Why:** This is the textbook "test of constants" AGENTS.md forbids: it fails only when someone edits the define, and then they edit the test too. The second case restates how `NPC_FLAG_ANY_VENDOR` is defined. The bug the header describes (a vendor cursor that never drew) is behaviour this test does not exercise.
- **Replace with:** a `GameHarness` unit created with `UNIT_NPC_FLAGS = 0x80` (and one with only `0x200`), then assert that the game or interface reports it as a vendor. Do the same for gossip, flight master and spirit healer.

### tests/test_rt_bvh.cpp:83 - TEST_CASE "RT BVH closest hit matches a brute-force scan" (and :112, :128, :144)
- **Category:** D, G
- **Severity:** high (the traversal parts); the structure check is fine
- **Evidence:**
  ```cpp
  const RtHit got = traceRtClosest(bvh.nodes, tris, o, d, 1e30f);
  ```
  `grep traceRtClosest src include` finds only its definition (rt_bvh.cpp:223), and the GPU walks `assets/shaders/rt_trace_sw.glsli`.
- **Why:** The brute-force comparison validates a CPU traversal that the running client never calls. A bug in the shader's stack walk, slab test or child order is invisible to it. Only `checkStructure`, which reads the builder output that gets uploaded, tests shipped code.
- **Replace with:** keep `checkStructure` on the built BVH. Move closest-hit correctness to a `scene_render` RT scene. Delete `traceRtClosest` with its tests (dead code rule).

### tests/test_sea_bed.cpp:132 - helper `fetchByPasses`, used by TEST_CASEs :234, :246, :268 (and :299)
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  /// ... the effective fetch the GPU finds over it on a grid of n / 4 (sea_bed_fetch.comp)
  for (int step = 1; step < g; step *= 2) { ... seaBedFetchPass(radialFetch[j * g + i], there, step * texelYards);
  ```
- **Why:** The pass loop, grid rotation and sampling of `sea_bed_fetch.comp` are rewritten in the test. `seaBedFetchPass`, `seaBedIsShore` and `seaBedDepth` are called only from `.comp` shaders, never from `sea_bed.cpp`. A wrong dispatch, step order or image binding in the shader passes. The :299 case calls `seaBedDepth`, used only by `sea_bed_combine.comp`. The `OverheadFrame` cases at :46, :68 and :83 are fine CPU behaviour.
- **Replace with:** a `scene_render` lake or breakwater scene whose fetch map is read back or visible in the wave picture, checked by a `check_*.py`.

### tests/test_ocean_spectrum.cpp:58 - TEST_CASEs "still air raises no sea", "the sea is as steep as Cox and Munk's", :72, :83, :90, :236, :311, :329
- **Category:** D
- **Severity:** medium
- **Evidence:**
  ```cpp
  #include "../assets/shaders/ocean_spectrum.glsli"
  const double slopeSq = ... oceanModeHeightVariance(k, c, toward, speed, scale);
  const vec2 g = oceanGaussianPair(x, y, c);
  ```
- **Why:** `oceanModeHeightVariance`, `oceanWaveVector`, `oceanGaussianPair`, `oceanFetchFactor` and `oceanRepeatingAngularSpeed` are not called by `ocean_sea.cpp`. They run only in `ocean_spectrum.comp`/`ocean_evolve.comp`, so these cases check a CPU compile of GPU code. The `OceanSea` cases at :177, :206, :262 and :287 test the CPU's own model and are fine. The case at :160 compares `OceanSea` against a sum of the same header's functions: a consistency check, not an independent oracle.
- **Replace with:** keep the `OceanSea` cases. Move the spectrum-statistics claims (Cox-Munk slope, downwind share, Gaussian draw) to a GPU readback or `scene_render` check.

### tests/test_open_formats.cpp:35 - TEST_CASE "WOM1 binary format structure" (and :83 "WOM2 magic differs from WOM1", :87 "WOM3 magic is distinct", :94 "WOM rejects invalid magic")
- **Category:** F, B
- **Severity:** high
- **Evidence:**
  ```cpp
  REQUIRE(0x314D4F57 != 0x324D4F57); // WOM1 != WOM2
  // Can't call WoweeModelLoader::load without linking it,
  REQUIRE(m != 0x314D4F57);
  ```
- **Why:** None of the four calls production code. :35 writes bytes and reads back what it wrote. :83 and :87 compare two literals. :94 checks that 0xDEADBEEF is not a magic number. No bug in `WoweeModelLoader`, which the client uses (terrain_manager.cpp, wowee_model_fromm2.cpp), can fail them.
- **Replace with:** link `wowee_model.cpp` and assert that `WoweeModelLoader::load` decodes a hand-built WOM1/WOM2/WOM3 file (vertex count, name, texture path) and rejects a bad magic.

### tests/test_realm_patches.cpp:143 - TEST_CASEs "a well-formed manifest parses", :153, :166, :180, :192
- **Category:** G
- **Severity:** high
- **Evidence:** `grep parsePatchManifest src include tools` finds only the definition (realm_patches.cpp:131) and its declaration. Nothing calls it. The realm patch flow is `RealmPatchSync`/`planRealmPatches` over `EzWowPatch`.
- **Why:** These are good-quality tests of code the application does not run. Per the dead code rule, the function and its tests go. `isSafePatchFileName` (called from realm_patch_sync.cpp) and `realmPatchDirName` (called from application.cpp) are live, and their tests at :20-131 are fine.
- **Replace with:** delete `parsePatchManifest`, `PatchManifest` and these five cases.

### tests/test_reputation_standing.cpp:15 - TEST_CASEs "Each standing claims the value at its own floor", :23 "The bands meet", :32 "Standings are numbered", :41 "Exalted is a thousand wide", :70 SECTION "the id is one more than the index"
- **Category:** A, B
- **Severity:** medium
- **Evidence:**
  ```cpp
  REQUIRE(reputationStandingFor(band.floor).id == band.id);
  REQUIRE(kReputationStandings[i].id == i + 1);
  REQUIRE(exalted.ceiling - exalted.floor + 1 == 1000);
  CHECK(std::string(reputationRankName(i)) == kReputationStandings[i].name);
  ```
- **Why:** The expected values come from the table under test. If someone moves Honored's floor to 8000, every case still passes: the lookup agrees with the wrong table, and the ids and widths are constants copied into the test. The exalted case pins a struct field instead of the bar the player saw. The cases at :49, :56, :66-68 and :79 use literals and are fine.
- **Replace with:** literal standings to literal answers (2999 is Friendly, 3000 is Honored, 42999 is Exalted). Better still, `GetFactionInfo` through `GameHarness` for a faction at 42500 answers Exalted with 500 of 1000.

### tests/test_pom_quality_one_scale.cpp:67 - TEST_CASE "the settings schema offers exactly the levels the scale has" (and :83, :91; also :49 lines 51/55/56, and :59)
- **Category:** C, B, A
- **Severity:** medium
- **Evidence:**
  ```cpp
  const std::string src = readSource("src/ui/settings_schema.cpp");
  const size_t at = src.find("{\"parallaxquality\"");
  CHECK_FALSE(std::regex_search(readSource(file), table));
  CHECK(pomSamplesFor(-1) == kPomSampleCounts[0]);
  ```
- **Why:**
  - :67 and :83 grep the C++ source text. Reformatting the row breaks them, and they pass even if the runtime schema is built differently. `clientSettingsSchema(count)` exists as a runtime API (settings_schema.hpp:181).
  - :91 is a source lint (no `{16,32,64}` in four named files), not a test of behaviour. It belongs in a `tools/*_check.py` if anywhere.
  - :51, :55 and :56 take the expected values from the table under test. :59 checks a property of a constant table.
- **Replace with:** look up `parallaxquality` in `clientSettingsSchema()` and assert that its range and choice labels select 16/32/64 through `pomSamplesFor`, with literal expected values.

### tests/test_opcode_tables.cpp:68 - TEST_CASE "the WotLK numbers are what the server uses" (and :98 "the movement acknowledgements exist where they are sent")
- **Category:** A, F
- **Severity:** medium
- **Evidence:**
  ```cpp
  {"CMSG_PING", 476},  // 0x1DC
  CHECK(found->second == number);
  CHECK(wotlk.count("CMSG_MOVE_SET_CAN_FLY_ACK") == 1);
  ```
- **Why:** Sixteen numbers are copied from AzerothCore into the test and compared with `opcodes.json`. This is the "value must agree with the server" case, where AGENTS.md asks for the behaviour instead. The :98 case checks only that a key exists, so a wrong number passes. The collision check at :45 is a useful data property.
- **Replace with:** `GameHarness` on wotlk and tbc: receive `SMSG_MOVE_SET_CAN_FLY` (or `SMSG_MOVE_SET_FLIGHT`) and assert that the ack goes out on the expansion's ack opcode. For the duplicate check, prefer `OpcodeTable::loadFromJson` refusing a collision.

### tests/test_reputation.cpp:218 - TEST_CASE "Asking before Faction.dbc can be read does not give up on it"
- **Category:** G
- **Severity:** medium
- **Evidence:**
  ```cpp
  dbc.assets = false;
  CHECK(service.getFactionName(978).empty());
  ```
  Production `FactionData::ready()` (include/game/reputation_gateway.hpp:22) is `return true;`.
- **Why:** The test pins a "not loaded yet" retry branch that the running client never takes. AGENTS.md "Game data" also says such a branch must not exist: a missing table stops the game. Line 237 (`rep.byFaction().count(978)`) additionally reads an internal map where `standingOf` already gives the answer.
- **Replace with:** delete the case, then `IFactionData::ready()` and the branch at reputation_service.cpp:49.

### tests/test_raid_mark_names.cpp:28 - TEST_CASE "/mark accepts the words it always accepted"
- **Category:** A, C
- **Severity:** low
- **Evidence:**
  ```cpp
  REQUIRE(kRaidMarkCount == 8);
  CHECK(toLower(raidMarkName(static_cast<uint8_t>(i))) == kWords[i]);
  ```
- **Why:** The count is a constant. The case claims to cover what `/mark` accepts but redoes `/mark`'s lowercasing itself, so a change in group_commands.cpp:350-363 (for example comparing without lowercasing) passes.
- **Replace with:** run `/mark skull` and `/mark Skull` through the chat command and assert that the raid target packet is sent with icon 7.

### tests/test_polygon_triangulate.cpp:70 - TEST_CASE "a ring with a deep notch still comes out whole"
- **Category:** B
- **Severity:** low
- **Evidence:** `CHECK(trianglesArea(u, idx) == Catch::Approx(std::fabs(signedAreaX2(u)) * 0.5f));`
- **Why:** The expected area is computed with the module's own `signedAreaX2`. If that function were wrong in the way the triangulator uses it, both sides would move together.
- **Replace with:** the literal 78 (12x10 minus the 6x7 slot).

### tests/test_opcode_table.cpp:122 - TEST_CASE "Global active opcode table"
- **Category:** B
- **Severity:** low
- **Evidence:** `wowee::game::setActiveOpcodeTable(&table); REQUIRE(wowee::game::getActiveOpcodeTable() == &table);`
- **Why:** Set-then-get through a plain setter. The `wireOpcode` assertions after it are the behaviour.
- **Replace with:** keep only the `wireOpcode` checks.

### tests/test_packet.cpp:9 - TEST_CASE "Packet default constructor" (and :18 "Packet opcode constructor")
- **Category:** B, F
- **Severity:** low
- **Evidence:** `Packet p(0x1DC); REQUIRE(p.getOpcode() == 0x1DC);`
- **Why:** These read back constructor arguments and defaults. The round-trip and bounds cases cover behaviour.
- **Replace with:** drop them, or fold them into a read-past-end case.

### tests/test_player_domain.cpp:371 - TEST_CASE "the player's fields are what the create block held, with each update over it"
- **Category:** C
- **Severity:** low
- **Evidence:** `CHECK(fields.find(uf(UF::PLAYER_XP))->second == 420);`
- **Why:** This reads the raw field storage instead of what a caller reads. The same merge is already observable through `player().xp()` and `level()`.
- **Replace with:** assert `xp()`/`level()` after create plus update, and after relog.

### tests/test_reputation_domain.cpp:196 - TEST_CASE "a unit of a faction the player is at war with is hostile, and peace makes it not"
- **Category:** C
- **Severity:** low
- **Evidence:**
  ```cpp
  h.game().setFactionTemplateRepList({{kKurenaiTemplate, 2}});
  h.game().setFactionHostileMap({{kKurenaiTemplate, false}});
  ```
- **Why:** State is injected through `GameHandler` setters instead of the `FactionTemplate.dbc` rows that build it. The case at :246 shows the harness can feed the table.
- **Replace with:** give the template through `h.dbc("FactionTemplate.dbc", ...)` and `buildFactionHostilityMap`.

## 3. Good examples

- **tests/test_placement_transform.cpp:** an oracle independent of the code. Real MODF records carry Blizzard's computed world bounds, and the matrix under test has to reproduce them. It cannot be passed by restating the implementation, and it is the test that settled a five-times-wrong rotation order.
- **tests/test_pet_domain.cpp and tests/test_petition_domain.cpp:** `GameHarness` end to end. Server packets go in; what is read back is the handler accessor, `addonEvents`, the chat log and `lastSent`. Each has a relog case for per-character state.
- **tests/test_pet.cpp and tests/test_petitions.cpp:** context tests done right. They link only `wowee_pet`/`wowee_petition`, the gateway fake implements the port and records only what is sent, and the real `core::EventBus` delivers the events.
- **tests/test_realm_patch_sync.cpp:** the fake transport only decides what arrives. Every assertion is an observable outcome on disk (file contents, no `.partial` left, a resume offset) for the unhappy paths: damaged, overlong and dropped transfers.
- **tests/test_packed_time.cpp:** the expected values are built with the server's own `AppendPackedTime` formula, not the client's reader, and the inverse is checked by round trip instead of repeating constants.
