# Test review, batch 5

Rules: AGENTS.md "Tests test behaviour" (plus "What runs on the GPU is tested on the GPU", "Dead code", "Through the front door").
Read-only review; nothing was built or run. "Used by the app" was checked with `git grep` over `src/ include/ tools/`.

## 1. Files

| file | verdict | categories | note |
|---|---|---|---|
| tests/test_sell_count.cpp | OK | - | Pure rule with literal expectations, used by 4 vendor sell sites. |
| tests/test_server_extension.cpp | OK | - | Real adapter process, byte payloads, bounded parsers; independent fixtures. |
| tests/test_session_domain.cpp | minor | C | GameHarness end to end; some cases seed entities straight into EntityManager instead of SMSG_UPDATE_OBJECT. |
| tests/test_settings_apply_on_load.cpp | whole file should go | C, F, A | Every case is a regex over C++ source text, not a run of the loader. |
| tests/test_settings_panel_layout.cpp | problem | D, C, A | Re-implements the Lua panel builder's layout walk in C++ and checks the copy. |
| tests/test_settings_schema_consistency.cpp | minor | C, A | Schema self-consistency is fine; source-text parse and row/category counts are not. |
| tests/test_shadow_cull_reuse.cpp | OK | - | Property test of a CPU function the M2 renderer calls; independent oracle. |
| tests/test_shadow_plane.cpp | whole file should go | D | `shadow_plane.glsli` compiled as C++; only the GPU runs it. |
| tests/test_shadow_shading.cpp | whole file should go | D | `shadow_shading.glsli` compiled as C++; only the GPU runs it. |
| tests/test_shapeshift_forms.cpp | OK | - | Behaviour of the form list; inputs use the code's SPELL_* ids (low risk). |
| tests/test_shared_rules.cpp | minor | G, B, F | One section tests a dead helper with its own formula; one case duplicates the schema-consistency file. |
| tests/test_shelter_probe.cpp | whole file should go | D | `shelter_probe.glsli` compiled as C++ via glsl_as_cpp; the probe runs on the GPU. |
| tests/test_sight_cache.cpp | OK | - | Counting fake world; asserts answers and query budget. |
| tests/test_simple_html.cpp | OK | - | Parser, literal inputs and outputs. |
| tests/test_sky_billboard.cpp | OK | - | CPU model matrix used by sky_bodies.cpp; checks projected shape. |
| tests/test_sky_celestial.cpp | minor | D | Sun/moon tables fine; last case tests `cloudLighting`, which only the cloud shader runs. |
| tests/test_sky_model_gate.cpp | OK | - | Real LightingManager over synthetic Light/LightParams/LightSkybox tables. |
| tests/test_spatial_grid.cpp | OK | - | Hand-worked cell maths; a few checks peek at grid storage (`grid.find`, `size`). |
| tests/test_spell_classification.cpp | OK | - | Pure rules, all used; literal data from Spell.dbc. |
| tests/test_spell_damage_log_layout.cpp | OK | - | Bytes in, decoded fields out, truncation refused. |
| tests/test_spell_description_eval.cpp | OK | - | Evaluator against a verbatim DBC row. |
| tests/test_spell_domain.cpp | minor | C | GameHarness end to end; two cases seed state through internal entry points. |
| tests/test_spell_icon_columns.cpp | minor | F | Passes with a WARN when no extracted data, so on CI it asserts nothing. |
| tests/test_sphere_map.cpp | whole file should go | D | `sphere_map.glsli` compiled as C++; m2.vert runs it on the GPU. |
| tests/test_spline.cpp | minor | A, F, G, B | Packet cases good; a flag-constant check, unit-length-only orientation checks, a test-only getter. |
| tests/test_spline_body.cpp | minor | B | Good head parsing, but the flag bits fed in are the code's own constants. |
| tests/test_srp.cpp | problem | F, G | No known-answer vector: sizes, non-zero and "differs" only; one case tests a setter nothing calls. |
| tests/test_stationery.cpp | OK | - | Literal Stationery.dbc rows. |
| tests/test_sun_direction.cpp | problem | G | Three cases test `sunDirectionFromLightDir`, which nothing in src/ calls. |
| tests/test_support.hpp | OK | - | Helper header, not a test. |
| tests/test_swim_wall.cpp | OK | - | Pure step rule used by camera_controller.cpp. |
| tests/test_taxi_map_projection.cpp | whole file should go | D | Tests a copy of `taxiNodeMapPos`'s chain written in the test; the real function is never called. |
| tests/test_taxi_packets.cpp | OK | - | Packet builders against the server's read order. |
| tests/test_terrain_bake.cpp | OK | - | Band and source selection, used by terrain_renderer.cpp. |
| tests/test_terrain_depth.cpp | OK | - | Cache reads and traversal results; `chunksTested` is a cost counter, acceptable. |
| tests/test_terrain_fog_band.cpp | OK | - | Literal distances. |
| tests/test_terrain_lod.cpp | minor | C | Good geometric properties; skirt checks hard-code the skirt vertex layout. |
| tests/test_terrain_slope_limit.cpp | whole file should go | G | `isWalkableNormal` is never called, and the app discards the normal (`(void)nz;`). |
| tests/test_terrain_tile_maps.cpp | OK | - | Pixel arrays as uploaded. |
| tests/test_text_edit.cpp | OK | - | Public editing API, literal strings. |
| tests/test_text_markup.cpp | minor | D, F | One section re-implements the edit box's backspace; one weak `size() <= 3`. |
| tests/test_text_tokens.cpp | OK | - | Literal in/out. |
| tests/test_text_wrap.cpp | OK | - | Literal in/out. |
| tests/test_texture_cache.cpp | OK | - | Fake fences, observable destruction, budget behaviour. |
| tests/test_tile_size_agreement.cpp | problem | A, B, D | Constant checks of TILE_SIZE/ZEROPOINT/PI; the axis case re-derives getTileBounds in the test. |
| tests/test_title_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_titles.cpp | OK | - | Context test, links only wowee_title, real EventBus. |
| tests/test_trade.cpp | OK | - | Context test, links only wowee_trade, real EventBus. |
| tests/test_trade_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_transport_components.cpp | problem | C, B, F | Drives ClockSync/Animator through 40 hand-set `ActiveTransport` fields; formula-copied and weak expectations. |

Totals: 28 OK, 10 minor, 5 problem, 7 whole file should go.

## 2. Findings

### tests/test_settings_apply_on_load.cpp:87-687 - every TEST_CASE in the file
- Category: C, F
- Severity: high
- Evidence:
  - `const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");`
  - `const std::regex name(R"RX("([a-z]+)")RX");` / `namesIn(panel, "ApplyKeys")`
  - `CHECK(handedToSomething(screen, bound->second));` (field name preceded by `(`, `,` or `=`)
  - `CHECK(loader.find("camera->setFov", load) == std::string::npos);`
  - `CHECK(authHeader.find("1200.0f") == std::string::npos);`
- Why: no case runs the loader, the panel or the renderer. The file is a regex lint over source text, so it fails on harmless refactors (renaming `kGraphicsApplyKeys`, moving a function, changing `settingsPanel_.` qualification, reformatting `out << "key=" << ...`) and passes real bugs (a field "handed to" the wrong setter, a clamp whose arguments are swapped, a value applied before services exist but written as an argument). `handedToSomething` accepts any `(`, `,` or `=` before the name, so `log(pendingFov)` counts as applied.
- Replace with: build `SettingsPanel`/`GameScreen` with fake renderer/camera services, load a config file with non-default values, and assert each target received its value (e.g. camera FOV == 75 after load); round-trip save→load for every schema key.

### tests/test_settings_apply_on_load.cpp:120 - TEST_CASE "the graphics defaults have one home"
- Category: A
- Severity: medium
- Evidence: `CHECK(wowee::ui::kDefaultViewDistance == 1900.0f);` `CHECK(wowee::ui::kDefaultGroundClutter == 70);` `CHECK(wowee::ui::kDefaultGroundClutter <= 150);`
- Why: copies the constants into the test; fails only when someone edits the default, and then they edit the test. The `<= 150` repeats a slider bound by hand.
- Replace with: with no config file, the settings panel and the login screen both show the schema's default for view distance and ground clutter (read through their public getters).

### tests/test_settings_panel_layout.cpp:97,158 - TEST_CASE "every settings panel fits in its two columns" / "a dropdown does not hang off the right of the panel"
- Category: D
- Severity: high
- Evidence:
  - `constexpr int kHeadingHeight = 32; constexpr int kCheckButtonHeight = 27; constexpr int kSliderHeight = 50;`
  - `// Walk the schema exactly as buildPanel does`
  - `const int twoWide = (out.panelWidth - margin * 2 - gap) / 2;` (the Lua `newLayout` re-implemented in C++)
  - `const int right = left + (layout.columnWidth - 60) + 25 * 2;`
- Why: the test lays the schema out with its own C++ copy of the Lua builder's column logic and hand-copied reserve heights, then checks the copy. If `buildPanel` changes how it reserves or wraps, the copy keeps passing. The file's own comment records this happening once (it checked COLUMN_* constants the builder had stopped using).
- Replace with: run `kWoweeOptionsPanelLua` through the headless FrameXML runner (`framexml_run`, already used to measure the 413x428 container) and assert every generated control's frame rect lies inside its panel.

### tests/test_settings_panel_layout.cpp:177 - TEST_CASE "the root panel's blocks do not sit inside each other"
- Category: C, A
- Severity: medium
- Evidence: `R"(SetPoint\("TOPLEFT",\s*-?[0-9]+,\s*(-[0-9]+)\)\s*--\s*needs\s+([0-9]+))"` / `REQUIRE(blocks.size() == 12);` / `constexpr int kPanelHeight = 428;`
- Why: checks hand-written `-- needs N` comments in the Lua source, not the laid-out frames; a comment that understates a block's height passes. `blocks.size() == 12` pins a count of the source.
- Replace with: same headless FrameXML run; assert the root panel's child frames do not overlap and end above 428.

### tests/test_settings_schema_consistency.cpp:271,282 - TEST_CASE "the source rows parse the way the compiled ones read" / "no row inherits a heading from a conditional one"
- Category: C
- Severity: medium
- Evidence: `const std::string src = wowee::test::slurp("src/ui/settings_schema.cpp");` / `} else if (line.rfind("    {\"", 0) == 0) {` / `CHECK(rows.size() <= count + 4);`
- Why: parses the schema's C++ source by indentation and preprocessor depth. Reformatting the table breaks it; it checks a source layout, not a behaviour.
- Replace with: if heading inheritance across `#ifndef NDEBUG` matters, make the inheritance impossible in code (each row names its section, or the schema builder resolves sections) and test `clientSettingsSchema` output.

### tests/test_settings_schema_consistency.cpp:321,336 - TEST_CASE "the schema has not lost rows" / "every panel the client offers still has settings on it"
- Category: A
- Severity: low
- Evidence: `CHECK(rows.size() >= 70);` / `CHECK(present.size() == 19);` plus a hand-copied list of 19 category names.
- Why: pins counts and names of the source table; the comments document repeated edits of the number whenever a panel moves.
- Replace with: drop; coverage of "a setting still works" belongs to a save/load/apply behaviour test per key.

### tests/test_shadow_plane.cpp:96 - TEST_CASE "A receiver's plane predicts the sun map's depth along its surface" (and the file's other three cases)
- Category: D
- Severity: high
- Evidence: `// The shader's own function, run on the CPU.` / `#include "../assets/shaders/shadow_plane.glsli"`
- Why: user decision 2026-10-31: a shader's behaviour is not tested by compiling it for the CPU. `shadowPlaneSlope` has no CPU caller (only shadow_receiver.glsli uses it); GPU precision, derivatives and the receiving pass are not exercised.
- Replace with: a `scene_render` scene with a sloped receiver under the sun map and a `tools/scene_render/check_*.py` that fails on acne/peter-panning stripes.

### tests/test_shadow_shading.cpp:18 - TEST_CASE "a full shadow leaves seventy percent of the lit colour" (and :24)
- Category: D
- Severity: high
- Evidence: `#include "../assets/shaders/shadow_shading.glsli"` / `CHECK(shadowLightFactor(0.0f) == Catch::Approx(0.7f));`
- Why: GPU-only code run on the CPU; the only src/ mention is a comment in renderer.cpp.
- Replace with: scene_render check that a fully shadowed patch is ~70 % of the lit patch's luminance and the map edge fades.

### tests/test_shelter_probe.cpp:39 - TEST_CASE "In an open field the camera is under the sky and sees all of it" (and the file's other five cases)
- Category: D
- Severity: high
- Evidence: `#include "rendering/glsl_as_cpp.hpp"` / `#include "../assets/shaders/shelter_probe.glsli"` / `float shelterTop(vec2 xy) { return gWorld(xy); }`
- Why: the probe runs in `shelter_probe.comp.glsl` on the GPU; the test replaces the shelter map texture fetch with a lambda, so the real sampling, texel layout and readback are untested.
- Replace with: scene_render scene with a roof and a street, read back the ShelterMap probe values (exposure, sky view) through the renderer.

### tests/test_sphere_map.cpp:28 - TEST_CASE "a surface facing the camera samples the centre of the sphere map" (and the file's other three cases)
- Category: D
- Severity: high
- Evidence: `#include "../assets/shaders/sphere_map.glsli"` / `sphere_map::sphereMapUV(kAhead, glm::vec3(0.0f, 0.0f, 1.0f))`
- Why: m2.vert.glsl is the only caller. The expected values are good (from GL_SPHERE_MAP), but they test a CPU compile of the include.
- Replace with: scene_render an M2 with a texcoord -1 batch facing the camera against a marked env map; check the centre texel is what is drawn.

### tests/test_sky_celestial.cpp:198 - TEST_CASE "Clear daylight cloud edges retain their illumination"
- Category: D
- Severity: high
- Evidence: `const auto lit = cc::cloudLighting(shape, ray, glm::vec3(0, 0, 1), 0.0f, ...);`
- Why: `cloud_coverage.hpp` includes `cloud_coverage.glsli` as C++. `coverageToward` has a CPU caller (renderer.cpp glare dimming), so the coverage cases are fine, but `cloudLighting`/`cloudShapeAt` have no CPU caller: this case tests the shader on the CPU.
- Replace with: extend `tools/scene_render/check_clouds.py` to fail on dark cloud rims in a clear-day scene.

### tests/test_shared_rules.cpp:371 - TEST_CASE "an item's colour by quality", SECTION "the six-digit form is the eight-digit one without its alpha"
- Category: G, B
- Severity: high
- Evidence: `CHECK(std::string(game::itemQualityColorHex(q)).substr(2) == game::itemQualityColorHexRGB(q));` and the implementation `return itemQualityColorHex(quality) + 2;`
- Why: `itemQualityColorHexRGB` has no caller outside this test (dead code), and the expected value is computed with the implementation's own formula, so it cannot fail.
- Replace with: delete the section and the function.

### tests/test_shared_rules.cpp:56 - TEST_CASE "the settings schema is something a panel can be built from"
- Category: F (duplicate)
- Severity: low
- Evidence: same key/label/category, default-in-range, enum labels == range and split-category checks as test_settings_schema_consistency.cpp:56-146.
- Why: two copies of the same invariants in two executables; a schema change has to be reasoned about twice.
- Replace with: keep one (the consistency file), delete this case.

### tests/test_srp.cpp:23-109 - TEST_CASEs "SRP initialize stores credentials" ... "SRP verifyServerProof rejects wrong proof"
- Category: F
- Severity: high
- Evidence: `REQUIRE_NOTHROW(srp.initialize("TEST", "PASSWORD"));` / `REQUIRE(A.size() == 32); REQUIRE(M1.size() == 20); REQUIRE(K.size() == 40);` / `REQUIRE_FALSE(allZero);` / `REQUIRE(m1a != m1b);` / `REQUIRE_FALSE(srp.verifyServerProof(fakeM2));`
- Why: the login proof is never checked against a known answer. A wrong `k`, wrong `u`, a missing salt in `x`, a byte-order slip in A or a broken K interleave all still give 32/20/40 bytes, non-zero A and differing M1 for differing passwords; `verifyServerProof` is only shown to reject, never to accept. Login breaking against a real realm would pass this file.
- Replace with: a known-answer vector from an AzerothCore/3.3.5a login trace (fixed client private `a` via a test seam, given B, g, N, salt, user, password) asserting exact A, M1, K, and that the server's M2 is accepted.

### tests/test_srp.cpp:111 - TEST_CASE "SRP setUseHashedK changes behavior"
- Category: G, F
- Severity: high
- Evidence: `srp.setUseHashedK(useHashed);` ... `REQUIRE(m1_default != m1_hashed);`
- Why: `setUseHashedK` is never called in src/ (only the member is read in srp.cpp:61), so this tests a branch the client cannot take; and "differs" holds for any change at all.
- Replace with: delete the setter, the branch and the test.

### tests/test_sun_direction.cpp:21,32,45 - TEST_CASEs "the sun is opposite the direction its light travels" / "a sun below the horizon stays below it" / "no sun given reads as straight up"
- Category: G
- Severity: high
- Evidence: `const glm::vec3 night = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, 1.0f));`; `git grep sunDirectionFromLightDir -- src include` finds only its definition.
- Why: the function these guard (the lens-flare mirror bug) has no caller in the client; only docs/SKY_SYSTEM.md claims the flare uses it. The tests pass whatever the flare does. Cases at :97 and :105 also route their checks through it.
- Replace with: delete the function and the three cases, or, if the flare should use it, test the flare's output (scene_render: no flare with the sun below the horizon).

### tests/test_taxi_map_projection.cpp:42-89 - TEST_CASEs "The middle of the continent is the middle of the map" ... "A node outside the continent still projects, off the map"
- Category: D
- Severity: high
- Evidence: `/// What taxiNodeMapPos does, in the order it does it.` / `glm::vec2 nodeToMapUV(...) { ... serverToCanonical ... canonicalToRender ... renderPosToMapUV(...) }`
- Why: the production chain is `taxiNodeMapPos` in src/addons/lua_system_api.cpp:5089, which the test never calls. The order the file says it pins is the test's own copy; reintroducing the skipped swap in `taxiNodeMapPos` passes every case. "Skipping the swap moves the marker" only compares two test-written chains.
- Replace with: drive `GetTaxiNodeInfo`/`TaxiNodePosition` (Lua) or `taxiNodeMapPos` made callable, with a TaxiNodes.dbc row and a WorldMapArea.dbc continent row via GameHarness `dbc(...)`, and assert the returned u,v literals.

### tests/test_terrain_slope_limit.cpp:31-60 - TEST_CASEs "Flat and gentle ground is walkable" ... "Slope is measured the same in every direction"
- Category: G
- Severity: high
- Evidence: `CHECK_FALSE(isWalkableNormal(normalZOf(55.0f)));`; `isWalkableNormal` has no caller outside its header; camera_controller.cpp:1010-1027 computes `nz` with `heightfieldNormalZ` and then `(void)nz;` with the comment "Until that is written, a steep hillside is climbable again".
- Why: the file header says the limit now applies to terrain; it does not. The tests pass while the behaviour they describe is absent from the game, and they fix our own 50-degree value without an original-client trace (AGENTS original-gameplay rule).
- Replace with: delete until the limit is applied to movement; then test it through movement (a step onto a 60-degree heightfield is refused), compared against an original-client trace.

### tests/test_tile_size_agreement.cpp:24,32,43,52,64 - TEST_CASEs "a tile is sixteen hundred thirds of a yard" ... "pi is written down once"
- Category: A, B
- Severity: high
- Evidence: `CHECK(wowee::core::coords::TILE_SIZE == Catch::Approx(1600.0f / 3.0f).epsilon(1e-6));` / `CHECK(tile / 16.0f == Catch::Approx(33.3333f)...)` / `CHECK(3.14159f != wowee::core::coords::PI);` / `CHECK(TWO_PI == 2.0f * PI);`
- Why: constants checked against their definition; the "chunk is a sixteenth" case divides the constant in the test and never touches the mesh generator; "truncated spelling is measurably different" checks arithmetic on a literal. The bug class named (a second spelling elsewhere) cannot be caught by testing the first.
- Replace with: delete; if two spellings are the concern, a duplicate-constant lint belongs in tools/, and tile maths is tested through `worldToTile`/mesh vertex positions at tile edges.

### tests/test_tile_size_agreement.cpp:98 - TEST_CASE "a tile's render-X edge comes from its Y index"
- Category: D
- Severity: medium
- Evidence: `const float maxRenderX = (32 - kTileY) * TILE_SIZE; const float maxRenderY = (32 - kTileX) * TILE_SIZE;`
- Why: the comment names `TerrainManager::getTileBounds` as the code that crossed the axes; the test re-derives the bounds itself and checks them against `worldToTile`. Re-crossing the axes in getTileBounds passes.
- Replace with: call `getTileBounds` (or the free function it should become) for tile (32,33) and assert both corners map back to (32,33) via `worldToTile`.

### tests/test_transport_components.cpp:66 - TEST_CASE "ClockSync: client animation advances localClockMs"
- Category: B, F
- Severity: medium
- Evidence: `REQUIRE(t.localClockMs > 0);` / `REQUIRE(pathTimeMs == t.localClockMs % path.spline.durationMs());`
- Why: the expected path time is the implementation's own modulo; the clock advance is checked only as "> 0" when 16 ms is knowable. Advancing by the wrong amount (e.g. seconds as ms) passes.
- Replace with: `localClockMs == 16` after dt 0.016, and `pathTimeMs` literal at a wrap (e.g. start at 3990, dt 0.02 → 10).

### tests/test_transport_components.cpp:121,745 - TEST_CASEs "ClockSync: processServerUpdate sets yaw and rotation" / "A borrowed route seeds no phase on the path it borrowed"
- Category: C, B
- Severity: medium
- Evidence: `sync.processServerUpdate(t, &path, pos, yaw, 10.0); REQUIRE(t.serverUpdateCount == 1); REQUIRE(t.serverYaw == Catch::Approx(1.5f)); REQUIRE(t.position == pos);`
- Why: echoes the arguments back out of internal `ActiveTransport` fields (`serverUpdateCount`, `serverYaw`, `hasServerYaw`). The test fixture hand-sets ~40 fields (`makeTransport`) that TransportManager normally owns, so a combination the manager never produces can pass or fail independently of the game.
- Replace with: drive `TransportManager` through its public API (or GameHarness with transport update packets) and read the transport's rendered position/rotation.

### tests/test_transport_components.cpp:106,553 - TEST_CASEs "ClockSync: reverse client animation decrements" / "Animator: Z clamping on non-world-coord client anim"
- Category: F
- Severity: low
- Evidence: `REQUIRE(t.localClockMs < 2000);` / `REQUIRE(t.position.z >= (t.basePosition.z - 2.0f));`
- Why: values are knowable (1500; the clamp floor) but only a direction/bound is checked; reversing by 1 ms or not clamping a shallower dip passes.
- Replace with: `localClockMs == 1500`; `position.z == basePosition.z - 2.0f` for the -50 key.

### tests/test_transport_components.cpp:166 - TEST_CASE "ClockSync: a transport facing its direction of travel is never flipped" (line 194)
- Category: C
- Severity: low
- Evidence: `REQUIRE(moving.serverYawAlignmentScore > 0);`
- Why: an internal heuristic score; the observable outcome (`serverYawFlipped180` false, rendered heading) is already asserted on the line before.
- Replace with: drop the score check; assert the rendered rotation matches the heading.

### tests/test_transport_components.cpp:390,498,527 - TEST_CASEs "Animator: Bravery / Moonspray / Elune's Blessing holds side-on at its dock dwell"
- Category: C
- Severity: low
- Evidence: test entries 176310/176244/181646 match `berthRunsParallel(uint32_t entry)` in src/game/transport_animator.cpp:23-26.
- Why: the tests pin a per-entry special case (AGENTS: no per-model special cases). They hold the hack in place rather than a rule that would cover every berth.
- Replace with: once the dock heading comes from data (route geometry through the berth, as the :662 chord case already does), test that rule with an unnamed route.

### tests/test_spline.cpp:27 - TEST_CASE "Pre-WotLK monster-move run mode has the protocol spline bit"
- Category: A
- Severity: medium
- Evidence: `REQUIRE(wowee::game::SplineFlag::PRE_WOTLK_RUNMODE == 0x00000100u);` then `isPreWotlkSplineWalking(SplineFlag::PRE_WOTLK_RUNMODE)`.
- Why: first line copies the constant; the second feeds the code's own constant back in, so a wrong bit passes both ways.
- Replace with: a pre-WotLK SMSG_MONSTER_MOVE body with flags literal 0x00000100 through the parser/movement handler, asserting the creature runs (and walks with 0).

### tests/test_spline.cpp:254,261 - TEST_CASEs "orientationFromTangent for forward direction" / "... vertical tangent"
- Category: F
- Severity: medium
- Evidence: `float length = glm::length(q); REQUIRE(length == Catch::Approx(1.0f).margin(0.01f));`
- Why: any unit quaternion passes, including identity for every input. transport_animator.cpp:286 sets hull rotation from it; a wrong heading is not caught.
- Replace with: tangent (1,0,0) gives a rotation that maps the model's forward axis onto +X (rotate a vector and compare).

### tests/test_spline.cpp:153,219 - TEST_CASEs "CatmullRomSpline duration calculation" / "time-closed path"
- Category: G, B
- Severity: low
- Evidence: `REQUIRE_FALSE(spline.isTimeClosed());` / `REQUIRE(spline.isTimeClosed());`
- Why: `isTimeClosed()` has no caller in src/; it is a constructor flag read back through a getter.
- Replace with: drop the getter checks; test what time-closing changes (evaluation near the wrap is continuous).

### tests/test_spline.cpp:205 - TEST_CASE "CatmullRomSpline clamping at boundaries"
- Category: F
- Severity: low
- Evidence: `// Before start should clamp ...` `auto before = spline.evaluatePosition(0);`
- Why: "before start" evaluates at 0, the same as the key test at :162; only the after-end half tests clamping.
- Replace with: keys starting at t=1000 and evaluate at 0.

### tests/test_spline_body.cpp:49 - constants feeding every TEST_CASE in the file
- Category: B
- Severity: low
- Evidence: `constexpr uint32_t kWotlkAnimation = SplineFlagWotlk::ANIMATION; // 0x00200000`
- Why: the file guards the expansion-specific animation bit, but feeds the code's constants into the parser; swapping or mistyping `SplineFlagWotlk::ANIMATION` passes. The oracle (AzerothCore MoveSplineFlag.h) is only in comments.
- Replace with: literal `0x00200000`, `0x00400000`, `0x00000800` from the server's header.

### tests/test_spell_icon_columns.cpp:34 - TEST_CASE "the loader reads every icon id to its icon path"
- Category: F
- Severity: low
- Evidence: `WARN("no extracted WotLK data under Data/expansions/wotlk, skipping"); return;`
- Why: passes green without asserting anything wherever the install's data is absent (CI, fresh clones).
- Replace with: a small literal SpellIcon.dbc via `wdbcFile` that always runs, keeping the real-data run as an extra.

### tests/test_text_markup.cpp:234 - TEST_CASE "erasing removes what draws as one character"
- Category: D, F
- Severity: low
- Evidence: `/// Backspace, as the edit box performs it` `std::string backspaceAt(...) { const size_t from = caretStepLeft(s, at); s.erase(from, at - from); ...}` / `REQUIRE(parseMarkup(s).size() <= 3);`
- Why: a copy of src/addons/lua_engine.cpp:10106's erase; if the edit box stops using `caretStepLeft` the test still passes. The `<= 3` bound does not detect a half escape.
- Replace with: drive an EditBox through the Lua engine with a link inserted and send Backspace; assert `GetText()`.

### tests/test_terrain_lod.cpp:162,207 - TEST_CASEs "the skirts reach below every level's edge, so no seam opens" / "no skirt hangs beside a hole"
- Category: C
- Severity: low
- Evidence: `mesh.skirtFirstVertex + edge * 9 + j` / `v == mesh.skirtFirstVertex + 2 * 9 + 0` / `CHECK(skirtTriangles == static_cast<size_t>(4 * (8 / strideOf(lod)) * 2));`
- Why: hard-codes the skirt vertex ordering and triangle count; a different but correct skirt layout fails.
- Replace with: find skirt vertices geometrically (below the surface edge at the same x,y) and check depth/coverage.

### tests/test_session_domain.cpp:276,341,369 - TEST_CASEs "a new map scope is shown every object already in view ..." / "an object the server destroys leaves view as what it is" / "a game object's custom animation is played ..."
- Category: C
- Severity: low
- Evidence: `h.game().getEntityManager().addEntity(entity->getGuid(), entity);` / `bobber->setField(6, ...)` / `h.game().world().announceObjectsInView();`
- Why: the objects are put straight into EntityManager instead of arriving by SMSG_UPDATE_OBJECT, and field 6 is a raw index, so a create-path bug that leaves them out of the manager or a wrong `OBJECT_FIELD_CREATED_BY` index is not seen.
- Replace with: `GameHarness::createObjectBody(...)` packets for each object, `fieldIndex(UF::OBJECT_FIELD_CREATED_BY)`.

### tests/test_spell_domain.cpp:286,333 - TEST_CASEs "rank replacement and unlearning refresh the open spell book" / "before WotLK the player's auras are the aura fields ..."
- Category: C
- Severity: low
- Evidence: `spells.addKnownSpell(133);` / `h.game().world().applyRecordedObjects(batch);`
- Why: state seeded through internal entry points rather than SMSG_INITIAL_SPELLS / SMSG_UPDATE_OBJECT; the rest of the file already uses the packet path.
- Replace with: `initialSpells({133})` and `valuesBody(...)` packets.

## 3. Good examples

- tests/test_trade.cpp: a bounded context tested on its own: links only `wowee_trade` (tests/CMakeLists.txt:822-823), real `core::EventBus`, a recording gateway as the port, assertions on events and outbound requests, including refusals and character switch.
- tests/test_trade_domain.cpp: the same area end to end through GameHarness: SMSG_TRADE_STATUS bytes in; chat lines, FrameXML events with arguments, and CMSG packets out.
- tests/test_sky_model_gate.cpp: the real `LightingManager` over Light/LightParams/LightSkybox tables built in the test, walked into like the game does; asserts what the renderer asks (`skyModelsHideProceduralSky`) and states its oracle (WoW.exe addresses).
- tests/test_texture_cache.cpp: a fake device whose fences the test passes by hand and textures whose destruction is recorded; asserts eviction order, budget and deferred GPU free, which are what callers depend on.
- tests/test_spell_damage_log_layout.cpp: builds the wire bytes from the server's layout and checks decoded fields, the crit bit and truncation refusal for both pre-WotLK readers, with no code constants in the expectations.
