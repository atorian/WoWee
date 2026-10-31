# Test review, batch 6

Rules applied: AGENTS.md "Tests test behaviour" (no constants, no tautologies, front door, a test must be able to fail, context isolation, GPU code tested on the GPU) and the user's global testing rules. Read-only review; nothing was built or run.

## 1. Files

| File | Verdict | Categories | Note |
|---|---|---|---|
| tests/test_transport_path_repo.cpp | minor | B, F | Taxi-route slice tests (408-552) are strong. Constructor read-back, compiler-generated move, and an empty-state "API works" test add nothing. Four spline tests via `PathEntry` (312-370) repeat `test_spline.cpp`. Hand-written `AssetManager` stubs (13-20) redefine production symbols so the TU links. |
| tests/test_ui_texture_cache.cpp | OK | - | Drives the cache through `request`/`pump` with injected loader/publisher. Asserts observable residency, fence gating and priority. |
| tests/test_uniform_slot_pool.cpp | OK | - | Fake device, but the asserts are outcomes: bytes not shared, alignment, reuse, failure. |
| tests/test_update_check.cpp | OK | - | Pure function, literal requirement cases. |
| tests/test_update_field_indices.cpp | minor | A, C | Oracle (AzerothCore) is independent, so it can catch a wrong slot. But it asserts table numbers, and it reads the JSON with its own regex instead of `UpdateFieldTable`. |
| tests/test_update_field_table.cpp | minor | A | Load check is fine. Offset pins are constants (derivation documented). |
| tests/test_use_item_packet.cpp | minor | B | Byte-level encoder/parser tests with literals are good. Classic/monster SECTIONs build the input with the enum under test. The file also holds chat-parser tests its name does not say. |
| tests/test_vertex_layouts.cpp | minor | A, B | Parsing the GLSL as an independent oracle is good. The `sizeof`/`offsetof` literal block and the `offsetof == offsetof` test are not. |
| tests/test_volumetric_fog_math.cpp | problem | D | `#include`s two `.glsli` shader files into C++ and tests them on the CPU. The user decision forbids this. `froxelCornerRays` round-trip (CPU code) is fine. |
| tests/test_warden_challenge.cpp | minor | A | RC4/SHA1 vectors are independent and good. Two lines pin opcode bytes of a data table. |
| tests/test_warden_hmac.cpp | OK | - | Compared against OpenSSL across every length. The bench is hidden. |
| tests/test_warden_memory.cpp | OK | - | Builds real PE files on disk and reads memory back through the public API. |
| tests/test_warden_reloc.cpp | OK | - | Pure helpers used by `warden_module.cpp:1010,1019`, with literal edge cases. |
| tests/test_water_mask.cpp | OK | - | Literal bit cases for each rule. |
| tests/test_water_reflection.cpp | OK | - | Asserts what is drawn (clip-space visibility), not matrix internals. |
| tests/test_water_surface_grid.cpp | OK | - | Literal cell math. Each test names its failure. |
| tests/test_water_visibility.cpp | OK | - | Small, behavioural. |
| tests/test_weather_domain.cpp | OK | - | GameHarness end to end: packet, state, bus event, addon event, relog. |
| tests/test_weather_mix.cpp | OK | - | Time-driven blend, asserted on outputs. |
| tests/test_who.cpp | OK | - | Links only `wowee_who` (CMakeLists 751-752). The gateway is the outbound port, so asserting what was sent is the outcome. |
| tests/test_who_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_widget_draw.cpp | OK | - | Headless ImGui, asserts the drawn rect height. |
| tests/test_widget_tree.cpp | problem | B, C, D, F | About 100 tests, most of them good layout/hit/draw-order behaviour. A handful read back what they set, replicate Lua-side logic, or assert internal marks. |
| tests/test_wind_field.cpp | problem | D, B | WindField clock/zone tests are good. Three tests compile `wind.glsli` on the CPU. One computes its bound from the implementation's private constants. |
| tests/test_wmo_batch_merge.cpp | OK | - | Literal grouping per reported case. |
| tests/test_wmo_draw_layer.cpp | OK | - | Trivial predicate, but each case is a stated rule. |
| tests/test_wmo_group_path.cpp | OK | - | Literal candidate lists. |
| tests/test_wmo_lighting.cpp | OK | - | Groups go through the real `WMOLoader`. Optional real-asset checks. |
| tests/test_wmo_minimap.cpp | OK | - | Expected values worked by hand from client rules. DBC driven through `RequiredTable`, including the stop path. |
| tests/test_wmo_night_glow.cpp | OK | - | Literal hours. |
| tests/test_world_load_progress.cpp | OK | - | State machine through `observe`. |
| tests/test_world_map.cpp | whole file should go | A, B, D, F | Almost every test checks a function written inside the test, a struct default, or `std::sort`. The UV replica is already stale against production. |
| tests/test_world_map_coordinate_projection.cpp | minor | F | Mostly good, public functions with literal cases. One test's name says the opposite of what it asserts, about a parameter production ignores. |
| tests/test_world_map_exploration_state.cpp | problem | B, F | Four of six tests cannot fail on a realistic bug. Only line 66 tests behaviour. |
| tests/test_world_map_map_resolver.cpp | minor | A, B | Resolver behaviour is good. One test pins a known id collision as if it were a requirement. |
| tests/test_world_map_view_state_machine.cpp | OK | - | Transitions through the public API. Line 7 (initial defaults) and line 159 (reads `duration` back to compute the step) are style-level only. |
| tests/test_world_map_zone_metadata.cpp | problem | A, F | Restates entries of a hand-written zone table. The label tests assert only "contains the name". |
| tests/test_world_socket.cpp | minor | C | Friend access writes private fields, calls private parse steps and asserts a private counter. The loopback test (98) shows the front door works. |
| tests/test_zone_light.cpp | OK | - | Real `LightingManager` over synthetic DBCs. Expectations come from the authored rows and the WoW.exe formula. |
| tests/test_zone_tiles.cpp | problem | A | Restates a hard-coded tile rectangle for one zone. |

## 2. Findings

### tests/test_transport_path_repo.cpp:54 - TEST_CASE "PathEntry is move-constructible"
- **Category:** B
- **Severity:** high
- **Evidence:** `game::PathEntry moved(std::move(entry)); REQUIRE(moved.pathId == 99);`. `PathEntry` declares only a value constructor (`include/game/transport_path_repository.hpp:96`), so the move is compiler-generated.
- **Why:** It checks what the compiler guarantees. No realistic code change makes it fail and still compile.
- **Replace with:** Delete it.

### tests/test_transport_path_repo.cpp:37 - TEST_CASE "PathEntry wraps CatmullRomSpline with metadata"
- **Category:** B
- **Severity:** low
- **Evidence:** `game::PathEntry entry(std::move(spline), 42, false, true, false); REQUIRE(entry.pathId == 42); REQUIRE(entry.fromDBC == true);`
- **Why:** It reads constructor arguments back through public fields. The only bug it could catch is swapped init-list members. `hasPathForEntry`/`hasUsableMovingPathForEntry` (163, 200) already exercise those flags through behaviour.
- **Replace with:** Delete it. The flag-driven repo tests cover it.

### tests/test_transport_path_repo.cpp:301 - TEST_CASE "hasTaxiPath and findTaxiPath"
- **Category:** F
- **Severity:** medium
- **Evidence:** `REQUIRE(repo.hasTaxiPath(100) == false); REQUIRE(repo.findTaxiPath(100, 0) == nullptr); // This just verifies the API works with empty data.`
- **Why:** An empty repository answering "none" catches nothing. The comment admits the real path (`loadTaxiPathNodeDBC`) is untested. That is also why the file stubs `AssetManager` (13-20).
- **Replace with:** Feed a TaxiPathNode table through `GivenTables`/`GameData`, then assert that `findTaxiPath(route, map)` returns that route's slice.

### tests/test_update_field_indices.cpp:47 - TEST_CASE "the WotLK field indices are what the server sends" (and 80-170)
- **Category:** A, C
- **Severity:** medium
- **Evidence:** `const std::regex pair(R"RX("(\w+)"\s*:\s*(\d+))RX");` (38) and `{"UNIT_FIELD_TARGET_LO", 18}, ... CHECK(found->second == index);`
- **Why:** The oracle is independent (AzerothCore's header), so a wrong slot is caught. That puts this above a pure constant copy. But it is still a table-equals-literal test. It parses the JSON with its own regex, so a name the real `UpdateFieldTable` loader skips (the silent failure `test_update_field_table.cpp` describes) passes here.
- **Replace with:** A GameHarness test. Send SMSG_UPDATE_OBJECT with a guid at the server's slot 18/19 and assert that the unit's target (or `UnitGUID("targettarget")`) reads it.

### tests/test_update_field_table.cpp:35 - TEST_CASE "WotLK mana-regen fields resolve to their wire offsets"
- **Category:** A
- **Severity:** low
- **Evidence:** `REQUIRE(table.index(UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) == 40);`
- **Why:** It goes through the real loader, which is good, but the assert is a number copied from a derivation. It catches a JSON edit, not a reader bug.
- **Replace with:** A GameHarness update that puts a float at the server's slot. Assert that `GetManaRegen()` returns it.

### tests/test_use_item_packet.cpp:148 - TEST_CASE "SMSG_MESSAGECHAT whispers ..." SECTION "Classic" (also 225-226)
- **Category:** B
- **Severity:** low
- **Evidence:** `packet.writeUInt8(static_cast<uint8_t>(ChatType::WHISPER)); packet.writeUInt32(static_cast<uint32_t>(ChatLanguage::UNIVERSAL));`
- **Why:** The input packet is built from the enum the parser maps through. If the Classic wire value for WHISPER were wrong in the enum, the test would still pass. The TBC/WotLK half of the same test (107-117) correctly uses literals and says why.
- **Replace with:** Write the server's literal byte for a Classic whisper, as lines 110-111 do.

### tests/test_vertex_layouts.cpp:76 - TEST_CASE "the WMO vertex is what the layout comment claims"
- **Category:** A, B
- **Severity:** medium
- **Evidence:** `STATIC_REQUIRE(sizeof(WMOVertex) == 64); STATIC_REQUIRE(offsetof(WMOVertex, normal) == 12);`
- **Why:** It copies the struct layout into the test. It fails only when someone edits the struct, and then the edit is correct. The shader-comparison tests in the same file (100-186) are what catch a real disagreement.
- **Replace with:** Delete it, or compare `kWmoVertexAttributes` offsets to `shadow.vert.glsl`'s inputs as the other tests do.

### tests/test_vertex_layouts.cpp:134 - TEST_CASE "the shadow pass reads position and texcoord from the real places"
- **Category:** B
- **Severity:** high
- **Evidence:** `CHECK(kWmoShadowVertexAttributes[0].offset == offsetof(WMOVertex, position));`. The table is defined as `.offset = offsetof(WMOVertex, position)` (`include/rendering/wmo_vertex.hpp:42`).
- **Why:** It restates the definition line by line. It is `offsetof == offsetof`.
- **Replace with:** Delete it, or check against an independent source: the shader's input names mapped to the struct field names.

### tests/test_volumetric_fog_math.cpp:27 - TEST_CASE "clear weather supplies thin air without drifting ground fog" (also 41, 60)
- **Category:** D
- **Severity:** high
- **Evidence:** lines 19-25: `namespace medium_math { ... #include "../assets/shaders/volumetric_medium.glsli" }`, then `medium_math::volumeMediumAt(...)`.
- **Why:** It is a CPU compile of shader code, which the user decision (2026-10-31) forbids. It cannot see what the GPU does differently: precision, `exp` and `max` semantics, the inject pass's inputs. The same session's commit `bc575f3b` removed the wake's CPU copy for exactly this reason.
- **Replace with:** A `scene_render` scene in clear, fog and sand weather, checked by a `tools/scene_render/check_*.py` that measures ground-fog density and tint.

### tests/test_volumetric_fog_math.cpp:153 - TEST_CASE "atmospheric transport is independent of subdivision" (also 166, 174, 185)
- **Category:** D
- **Severity:** high
- **Evidence:** lines 143-151: `#include "../assets/shaders/atmosphere_medium.glsli"`, then `integrateAtmosphereCell(...)`, `atmosphereDensity(...)`.
- **Why:** Same as above, for the integrate pass's medium. This is `volumetric_fog_integrate.comp.glsl:77`'s include, run on the CPU.
- **Replace with:** A `scene_render` acceptance check on the drawn picture. For example, horizon transmittance at the zone midpoint is about 0.5 and independent of froxel count.

### tests/test_volumetric_fog_math.cpp:101 - TEST_CASE "a fog cell is lit where the shaders will read it"
- **Category:** D (partial)
- **Severity:** low
- **Evidence:** `glm::vec3 rayAt(...) { const glm::vec3 top = glm::mix(rays[0], rays[1], uv.x); ... }` (80-84), described as "as the shaders and the inject pass respectively do".
- **Why:** `froxelCornerRays` is CPU code, so testing it is right. But the other half of the round trip is a test-side copy of the inject shader's bilinear mix. A change in the shader's corner order would not be seen.
- **Replace with:** Keep the CPU round trip, and add the `scene_render` check that shafts line up with their casters.

### tests/test_warden_challenge.cpp:30 - TEST_CASE "Standard Warden hash authenticates the client key ..." (lines 34-35)
- **Category:** A
- **Severity:** low
- **Evidence:** `CHECK(m.checkOpcodes[0] == 0xF3); // server MEM_CHECK`
- **Why:** It asserts a table entry equals a literal. The SHA1 check on line 33 is the meaningful part.
- **Replace with:** Decrypt a recorded check request with these opcodes and assert which check the module runs.

### tests/test_widget_tree.cpp:584 - TEST_CASE "A widget knows what type it is"
- **Category:** B
- **Severity:** high
- **Evidence:** `tree.get(button)->objectType = "Button"; ... REQUIRE(tree.get(button)->objectType == "Button");`
- **Why:** Set-then-get on a public field. The FrameXML behaviour the comment describes (`GetObjectType`, `IsObjectType`) is not exercised.
- **Replace with:** A Lua-engine test: `CreateFrame("Button")` returns "Button" from `GetObjectType()`, and `IsObjectType("Frame")` is true.

### tests/test_widget_tree.cpp:640 - TEST_CASE "Only a frame that asked for the wheel takes it"
- **Category:** B
- **Severity:** medium
- **Evidence:** `tree.get(scroll)->wheelEnabled = true; ... REQUIRE_FALSE(tree.get(child)->wheelEnabled); REQUIRE(tree.get(tree.get(child)->parent)->wheelEnabled);`
- **Why:** Only `hitTest == child` is behaviour. The two wheel assertions read back the test's own writes. The "walk up the dispatch" the comment describes is never called.
- **Replace with:** Call `hitTestWheel(50, 50)` and assert that it returns `scroll`, as test 1981 does. Or drive a wheel event through the dispatcher and assert that `scrollY` changed.

### tests/test_widget_tree.cpp:712 - TEST_CASE "Scroll frames are tracked as they are marked"
- **Category:** C
- **Severity:** low
- **Evidence:** `tree.markScrollFrame(a); ... REQUIRE(tree.scrollFrames().size() == 2);`
- **Why:** `scrollFrames()` is a performance index ("walking every widget ... is the kind of cost"). The test pins the cache's contents, not what the player sees. A double entry would show as a scroll range clamped twice, which is harmless.
- **Replace with:** Assert the observable: a frame marked twice still clamps `scrollY` to its child's range after one update.

### tests/test_widget_tree.cpp:935 - TEST_CASE "A message frame keeps its lines and drops the oldest"
- **Category:** D, B
- **Severity:** high
- **Evidence:** `w->messages.push_back({line, ...}); while (w->messages.size() > w->maxMessages) w->messages.pop_front();` followed by `REQUIRE(w->messages.front().text == "two");`
- **Why:** The test performs the trimming itself, as a copy of `src/addons/lua_engine.cpp:1482`, and then checks its own result. If AddMessage stopped trimming, the test would still pass. Only the "a frame with lines paints" half is behaviour.
- **Replace with:** Through the Lua engine: `SetMaxLines(3)`, four `AddMessage` calls, then assert that the first line read back is "two". Keep the draw-order half.

### tests/test_widget_tree.cpp:1862 - TEST_CASE "a model frame carries what it has been asked to try on"
- **Category:** B
- **Severity:** high
- **Evidence:** `tree.get(model)->tryOnItems.push_back({1234u, 5u}); ... CHECK(tree.get(model)->tryOnItems.size() == 2); ... clear(); CHECK(...empty());`
- **Why:** It tests `std::vector`. The behaviour lives in `lua_inventory_api.cpp:551-586`: TryOn replaces the item in the same slot, and Undress clears.
- **Replace with:** A Lua test: `DressUpModel:TryOn(item)` twice for the same slot leaves one entry, and `Undress()` empties it.

### tests/test_widget_tree.cpp:1910 - TEST_CASE "A rect that is not a number is never hit"
- **Category:** C
- **Severity:** low
- **Evidence:** `g->visible = true; g->left = 10.0f; ... b->left = std::numeric_limits<float>::quiet_NaN();`
- **Why:** It writes the layout's outputs (`visible`, `left`, `rectW`) directly instead of producing them through `layout`. That is acceptable for injecting a NaN, but it would pass even if layout could never produce one.
- **Replace with:** Set a NaN anchor offset (the saved-position case), call `layout`, then `hitTest`.

### tests/test_widget_tree.cpp:2068 - TEST_CASE "Two frames anchored to each other do not resolve for ever"
- **Category:** F
- **Severity:** low
- **Evidence:** `tree.resolveWidget(a); SUCCEED("resolving a cycle terminated");`
- **Why:** Termination is the behaviour, so it can fail (hang or stack overflow). Nothing is asserted about the result.
- **Replace with:** Also assert that the rects are finite (not NaN or inf), so a cycle cannot poison hit testing.

### tests/test_widget_tree.cpp:2520 - TEST_CASE "Zeroing a font string's height lets it be measured again"
- **Category:** C
- **Severity:** medium
- **Evidence:** `w->measuredText = w->text; ... tree.setHeight(fs, 0.0f); REQUIRE(w->measuredText.empty());`
- **Why:** `measuredText` is the renderer's private cache mark (`widget_renderer.cpp:260-312`). The test pins how the fix is implemented, not that the line comes back with a height. A different invalidation would fail it while behaviour stayed right.
- **Replace with:** In `test_widget_draw.cpp` style, with headless ImGui: set the text, draw, `SetHeight(0)`, set the same text, draw. Assert `rectH > 0` and that the line is in the draw order.

### tests/test_widget_tree.cpp:2978 - TEST_CASE "layout survives widgets created while it walks"
- **Category:** F
- **Severity:** medium
- **Evidence:** All 64 children are created before `tree.layout(...)`. The asserts are `CHECK(w->parent == parent);`.
- **Why:** The bug it names, a reallocation while the walk is in progress, needs a widget created during layout (for example from a resolve callback). That never happens here. `parent` is set at `create` and cannot change. The test passes with or without the fix.
- **Replace with:** Create widgets from inside the walk (a Lua OnSizeChanged or resolve hook, or a test hook on the tree). Assert no crash under ASan and that the new widget is laid out on the next pass.

### tests/test_wind_field.cpp:360 - TEST_CASE "A tree and its shadow sway together whichever way it faces" (also 390, 420)
- **Category:** D
- **Severity:** high
- **Evidence:** lines 18-38: `namespace wind_shader { ... #define WIND_PER_FRAME #include "../assets/shaders/wind.glsli" }`, then `wind_shader::foliageWindSway(...)`, `grassWindBend(...)`.
- **Why:** These are CPU replicas of shader code, which the user decision forbids. "Shadow and tree agree" is precisely a GPU-side property: two pipelines, push constants against a UBO.
- **Replace with:** A `scene_render` scene with a tree in wind at several yaws. A check script compares the tree's silhouette displacement with its shadow's, and checks the lean direction.

### tests/test_wind_field.cpp:160 - TEST_CASE "A storm raises the wind smoothly and it arrives in the time promised" (lines 177-182)
- **Category:** B
- **Severity:** medium
- **Evidence:** `const float maxStep = ((stormSpeed - calmSpeed) / WindField::kSpeedTimeConstant + WindField::kSpeedFloorRate) * kFrame + 1e-5f;` and the loop bound `WindField::kSpeedSettleSeconds`.
- **Why:** The bound is computed from the implementation's own tuning constants (`wind_field.hpp:159-161`). Retune them, and the test follows the code rather than a requirement.
- **Replace with:** Literal bounds from the requirement, for example "no frame moves more than 0.05 yd/s" and "within 15 s".

### tests/test_world_map.cpp:116 - TEST_CASE "UV projection: ..." (116-151, helper 97-114)
- **Category:** D
- **Severity:** high
- **Evidence:** `// Replicate the UV projection formula from renderPosToMapUV for standalone testing.` and `if (isContinent) { constexpr float kVOffset = -0.15f; v = ...; }`
- **Why:** It tests a copy, and the copy is already wrong. Production ignores `isContinent` (`src/rendering/world_map/coordinate_projection.cpp:74`, `(void)isContinent;`). `test_world_map_coordinate_projection.cpp:71` asserts no offset. This test asserts a -0.15 offset, and both pass.
- **Replace with:** Delete it. `test_world_map_coordinate_projection.cpp` already tests the real `renderPosToMapUV`.

### tests/test_world_map.cpp:166 - TEST_CASEs "Vanilla hides TBC and WotLK continents" ... and "Vanilla has no cosmic view entries" ... (166-185, 352-371)
- **Category:** D
- **Severity:** high
- **Evidence:** `static std::vector<uint32_t> filterContinentsByExpansion(...) { if (id == 530 && expansionLevel < 1) continue; ...}` and `static std::vector<CosmicMapEntry> buildCosmicMaps(int expLevel)`.
- **Why:** These functions exist only in the test. Production builds the cosmic view in `data_repository.cpp` and is never called.
- **Replace with:** Load a WorldMapContinent table per expansion through `data_repository` and assert which continents are listed.

### tests/test_world_map.cpp:197 - TEST_CASE "POI faction classification"
- **Category:** D
- **Severity:** high
- **Evidence:** `static Faction classifyFaction(uint32_t factionId) { if (factionId == 469) ...}`. The production check is inline in `layers/poi_marker_layer.cpp:35-38`.
- **Why:** It is a copy of the logic. Changing production cannot fail it.
- **Replace with:** Delete it, or test the layer's chosen colour for a 469/67/0 POI.

### tests/test_world_map.cpp:227 - TEST_CASEs "ZMP grid ..." and "HitRect ..." (227-334)
- **Category:** D, F
- **Severity:** high
- **Evidence:** `int col = std::clamp(static_cast<int>(mu * ZMP_SIZE), 0, ZMP_SIZE - 1); uint32_t areaId = grid[row * ZMP_SIZE + col];` and `bool inside = (px >= ov.hitRectLeft && ...)`. All of it is written in the test body. "ZMP grid returns 0 for empty cells" asserts that a zeroed `std::array` is zero.
- **Why:** No production code is called.
- **Replace with:** Delete them, and test the facade's hover result for a ZMP and overlay set.

### tests/test_world_map.cpp:401 - TEST_CASEs "Subzone hover ..." and "Zone label ..." (375-480)
- **Category:** D
- **Severity:** high
- **Evidence:** `static std::string findSmallestOverlay(...)` and `static std::string formatZoneLabel(...)`, with a test-local `TestFaction` enum.
- **Why:** These are replicas. The real label is `ZoneMetadata::formatLabel`, tested (weakly) in `test_world_map_zone_metadata.cpp`.
- **Replace with:** Delete them, and move the literal label expectations (`"Elwynn (1-10) [Alliance]"`) onto `ZoneMetadata::formatLabel`.

### tests/test_world_map.cpp:19 - TEST_CASEs "POI default-constructed is zeroed", "Zone default-constructed is valid", "OverlayEntry defaults" (19, 65, 206)
- **Category:** A, B
- **Severity:** high
- **Evidence:** `POI poi{}; REQUIRE(poi.id == 0); ...`
- **Why:** These restate default member initialisers.
- **Replace with:** Delete them.

### tests/test_world_map.cpp:33 - TEST_CASE "POI sorts by importance ascending"
- **Category:** B
- **Severity:** high
- **Evidence:** `std::sort(pois.begin(), pois.end(), [](const POI& a, const POI& b) { return a.importance < b.importance; });`
- **Why:** It tests `std::sort` with a comparator written in the test.
- **Replace with:** Delete it.

### tests/test_world_map.cpp:79 - TEST_CASE "Zone areaID==0 identifies continent"
- **Category:** F
- **Severity:** high
- **Evidence:** `continent.areaID = 0; ... REQUIRE(continent.areaID == 0); REQUIRE(zone.areaID != 0);`
- **Why:** It asserts what it just assigned.
- **Replace with:** Delete it. `isLeafContinent`/`continentZoneIndex` in the projection tests cover the rule.

### tests/test_world_map_coordinate_projection.cpp:71 - TEST_CASE "renderPosToMapUV: continent applies vertical offset"
- **Category:** F
- **Severity:** low
- **Evidence:** The name says an offset is applied. The body says `// No vertical offset - continent and zone UV should be identical` and asserts equality.
- **Why:** The name contradicts the assertion. The `isContinent` parameter it exercises is dead in production (`(void)isContinent;`).
- **Replace with:** Rename it, or remove the dead parameter and this test with it.

### tests/test_world_map_exploration_state.cpp:28 - TEST_CASEs at 28, 35, 45, 50, 56
- **Category:** B, F
- **Severity:** medium
- **Evidence:** `es.setServerMask(mask, true); REQUIRE(es.hasServerMask() == true);` (set-then-get). `"overlaysChanged tracks changes"` asserts only `overlaysChanged() == false` on a fresh object. `clearLocal()` is called on an empty state. "update with empty zones is safe" asserts that an empty result is empty.
- **Why:** None of these can fail on a realistic bug in `update`. The name at 45 promises change tracking and never triggers a change.
- **Replace with:** Extend the one real test (66). Assert that an overlay with its bit unset is not explored, that `overlaysChanged()` flips to true after an update that explores a new overlay, and that it does not flip on a second identical update.

### tests/test_world_map_map_resolver.cpp:246 - TEST_CASE "the world view and an unknown folder answer the same map id"
- **Category:** B, A
- **Severity:** low
- **Evidence:** `CHECK(folderToMapId("World") == folderToMapId("NoSuchPlace"));`, and line 78 `REQUIRE(folderToMapId("world") == static_cast<int>(UINT32_MAX));`
- **Why:** It pins a defect (a sentinel collision) as required behaviour. A fix that gives "World" a distinct id would fail it while the player-visible behaviour improved.
- **Replace with:** Assert only `isUiOnlyMapFolder` (lines 223-244 already do). Drop the sentinel equality.

### tests/test_world_map_zone_metadata.cpp:16 - TEST_CASEs "find returns valid data for known zones", "Contested zones", "Horde zones" (16-43), plus 50 and 79
- **Category:** A, F
- **Severity:** medium
- **Evidence:** `REQUIRE(elwynn->faction == ZoneFaction::Alliance);` against `add("Elwynn", 1, 10, ZoneFaction::Alliance);` (`zone_metadata.cpp:19`). At 50: `REQUIRE(label.find("Elwynn") != std::string::npos);`. At 79, double initialisation is "safe".
- **Why:** The first three copy entries of a hand-written table. The formatLabel test accepts any output containing the name, although the exact string is knowable.
- **Replace with:** `formatLabel("Elwynn", {1,10,Alliance}) == "Elwynn (1-10) [Alliance]"`, plus the Contested and Neutral variants, as literals from the UI requirement.

### tests/test_world_socket.cpp:44 - TEST_CASE "WotLK extended encrypted headers survive every TCP header split" (also 75, 89; access struct 6-22)
- **Category:** C
- **Severity:** medium
- **Evidence:** `socket.connected = true; socket.encryptionEnabled = encrypted; socket.receiveBuffer.insert(...); socket.tryParsePackets();` and `REQUIRE(WorldSocketTestAccess::decrypted(socket) == 5);`
- **Why:** The packet assertions are behaviour, but the setup writes private state and calls private steps. Line 60 asserts a private counter (`headerBytesDecrypted`), so a correct parser that tracked the header differently would fail it. The loopback test (98) shows the real `connect`/`update` path is usable.
- **Replace with:** Use a loopback peer, as test 98 does. Send the frame in split writes and assert only the delivered packets, which already prove the header was not decrypted twice.

### tests/test_zone_tiles.cpp:5 - TEST_CASE "Duskwood ADT coverage follows client map bounds"
- **Category:** A
- **Severity:** medium
- **Evidence:** `REQUIRE(isDuskwoodAdtTile(33, 52)); REQUIRE_FALSE(isDuskwoodAdtTile(34, 51));` against `tileX >= 33 && tileX <= 35 && tileY >= 52 && tileY <= 53` (`include/game/zone_manager.hpp:17-20`).
- **Why:** It restates a hand-coded rectangle for one zone. That rectangle is itself a per-zone special case, which AGENTS.md "Correct solutions" discourages. It fails only when the rectangle is edited.
- **Replace with:** Through `ZoneManager`: given an AreaTable with Duskwood's parentage, a position in a Duskwood tile resolves to zone 10, and a shared-bank tile resolves by parentage.

## 3. Good examples

- **tests/test_weather_domain.cpp:** the model GameHarness test. It plays an SMSG_WEATHER packet in, then reads the game's state, the bus event (`WeatherChanged` with `previous`), the addon event (`WEATHER_CHANGED` args) and the relog reset. The numbers come from AzerothCore's writer, not from the enum.
- **tests/test_who.cpp + tests/test_who_domain.cpp:** the context-isolated domain test (links only `wowee_who`, with a recording gateway and the real `EventBus`) paired with the end-to-end harness test. This is the split AGENTS.md asks for.
- **tests/test_vertex_layouts.cpp:100-186:** an independent oracle. It parses `assets/shaders/*.vert.glsl` and compares it with the C++ attribute table, without sharing code with the implementation. (Lines 76 and 134 in the same file are the counter-examples.)
- **tests/test_wmo_lighting.cpp:** builds MOGP/MOBA bytes and reads them through the real `WMOLoader`, so a loader regression fails it. Expectations are the client's rules. Optional real-asset checks pin Dalaran, Duskwood and Orgrimmar.
- **tests/test_uniform_slot_pool.cpp:** the fake device only supplies memory. Every assertion is an outcome (bytes written through one slot survive the others, offsets are aligned, a freed slot is reused before a new block), not a call log.
