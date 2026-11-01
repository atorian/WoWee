# Test review, batch 3 (55 files)

Verdict scale: **OK** = no finding; **minor** = low-severity findings only; **problem** = at least one medium/high finding, the rest of the file is sound; **whole file should go** = the file as written tests constants and should be replaced by a behaviour test.

## 1. Files

| file | verdict | categories | note |
|---|---|---|---|
| tests/test_light_volume_order.cpp | OK | - | Pure functions with literal weights/shares from the client rule; order-stability property test. |
| tests/test_lightning_storm.cpp | OK | - | Drives the public storm API; asserts visibility, thunder delivery, ground termination. |
| tests/test_link_hit.cpp | problem | D | Case 2 lays out runs and hit-tests with a local copy of the draw advance and `WidgetTree::linkAt`. |
| tests/test_liquid_scene.cpp | OK | - | Tiles in, heights/bodies/listener notifications out. |
| tests/test_liquid_types.cpp | OK | - | LiquidType.dbc rows and MCLQ bytes in, scene answers out. Good. |
| tests/test_local_lights.cpp | problem | G, D, C | `localLightAttenuation` is test-only and a CPU copy of `local_lights.glsli`; `localLightSlot` (decoder) is also test-only. |
| tests/test_locomotion_fsm.cpp | minor | F | "STAY when WALK_BACKWARDS missing" only checks `out.valid`. |
| tests/test_log_privacy.cpp | OK | - | Literal in/out strings. |
| tests/test_loot.cpp | OK | - | Context test: links only `wowee_loot`, real `EventBus`, recording gateway. |
| tests/test_loot_domain.cpp | OK | - | GameHarness end to end. Good example. |
| tests/test_loot_response_packet.cpp | OK | - | Byte buffers through the parser. |
| tests/test_lua_arg_coercion.cpp | OK | - | Lua stack in, value out; class tokens are known game data. |
| tests/test_lua_generic_for.cpp | OK | - | VM behaviour via Lua source. |
| tests/test_lua_handler_globals.cpp | OK | - | Real dispatch through `pcallScript`/`lua_WoweeFireOnLoad`. (L218 is a trivial set-then-get, harmless.) |
| tests/test_m2_anim_clock.cpp | OK | - | Literal times. |
| tests/test_m2_billboard.cpp | OK | - | Geometric properties of the output matrix. |
| tests/test_m2_billboard_model.cpp | minor | C | Real asset, but drives `computeBoneMatrices` from `m2_renderer_internal.h`. |
| tests/test_m2_blend_mode.cpp | minor | A | Mostly good; asserts the internal `alphaTest` codes 2/3/4 that only the shader interprets. |
| tests/test_m2_classifier_foliage.cpp | OK | - | Real asset names in, flags out. |
| tests/test_m2_color_alpha.cpp | minor | F | Asset cases `SUCCEED` (pass) when data is missing instead of `SKIP`. |
| tests/test_m2_color_track.cpp | problem | A, G, F | First case asserts offset constants; two of the three functions are used only by the test. |
| tests/test_m2_combiner.cpp | OK | - | Client shader-selection rules with literal cases. |
| tests/test_m2_cull_grid.cpp | problem | D | Oracle `instanceCasts` is a hand copy of the `insideFootprint` lambda in `M2Renderer::renderShadow`. |
| tests/test_m2_footstep_events.cpp | minor | F | Expected values come from an independent Python parser (good); passes vacuously without data. |
| tests/test_m2_glow_card.cpp | OK | - | Decision functions with literal cases. |
| tests/test_m2_material.cpp | problem | B, G | Vulkan blend state restated field by field; `m2TextureWeightOpacity` is used only by the test. |
| tests/test_m2_ribbon_tracks.cpp | minor | F | Range-only checks; vacuous pass when there is no data or no ribbons. |
| tests/test_m2_sky_material.cpp | OK | - | Tracks and real asset in, opacity out. |
| tests/test_m2_star_layer.cpp | OK | - | Real texture paths in, flag out. |
| tests/test_m2_structs.cpp | problem (most of file should go) | B, F, C | 12 of 16 cases (L92-248) are set-then-get, default values or arithmetic done in the test. |
| tests/test_m2_sway.cpp | minor | A | Asserts the shader's numeric mode codes and the 0.3 amplitude. |
| tests/test_m2_texture_transform.cpp | problem | G, D | `m2UVTranslation`, `m2ApplyUVTransform` and `m2UVTransformIsIdentity` are test-only; `m2ApplyUVTransform` is the shader's row multiply in C++. |
| tests/test_m2_view_distance.cpp | minor | D | One case builds its input with a copy of M2Renderer's base-distance formula. |
| tests/test_macro_evaluator.cpp | OK | - | Fakes are state stubs; asserts the chosen action/target. |
| tests/test_macro_text.cpp | OK | - | Literal macro text in, lines out. |
| tests/test_mail_domain.cpp | OK | - | GameHarness end to end. |
| tests/test_mail_packets.cpp | OK | - | Wire bytes through parsers/builders. |
| tests/test_mailbox.cpp | OK | - | Context test: links only `wowee_mail`, real bus. Good example. |
| tests/test_mcp_http.cpp | OK | - | Parser, auth rules, real socket round trip. |
| tests/test_mcp_main_thread_queue.cpp | OK | - | Threading behaviour. |
| tests/test_mcp_server.cpp | OK | - | JSON-RPC in, spec-defined responses out. |
| tests/test_melee_anim_chains.cpp | minor | A | First case asserts table heads; the other cases are real invariants. |
| tests/test_memory_pressure.cpp | OK | - | Threshold classification. |
| tests/test_minimap_projection.cpp | problem | D | `minimap_display.frag.glsl` rewritten in C++ (`shaderSampleUV`) and used as the oracle. |
| tests/test_minimap_tile_index.cpp | OK | - | Literal .trs in, file names out. |
| tests/test_minimap_zoom.cpp | OK | - | Through Lua `Minimap:SetZoom` and the CVar file. Good example. |
| tests/test_model_bounds.cpp | minor | F | A few lines assert glm arithmetic done in the test. |
| tests/test_model_import.cpp | OK | - | Fake source, real files written and read back. Good example. |
| tests/test_monster_move_facing.cpp | OK | - | Bytes in, consumed length and fields out. |
| tests/test_movement_domain.cpp | problem | C, F | Transport case calls loader steps directly and checks only `hasPathForEntry`; other cases inject decoded `UpdateBlock`s. |
| tests/test_movement_flag_bits.cpp | whole file should go | A | Asserts enum bit values and the membership of the `kLocomotionFlags` constant. |
| tests/test_movement_flags.cpp | minor | B | Expected values are built from the same enum the predicates read. |
| tests/test_movement_limits.cpp | problem | A, G, B, F | First case asserts a constant and dead helpers; second case tests only the test's own arithmetic. |
| tests/test_movement_payload_layout.cpp | problem | B | Most cases round-trip each parser's own writer into its own reader. |
| tests/test_nameplate_stacking.cpp | minor | F | The "gives up" case asserts only `y <= 400`. |

## 2. Findings

### tests/test_local_lights.cpp:50 - "a light falls off by the fixed client attenuation"
- **Category:** G, D
- **Severity:** high
- **Evidence:**
  ```cpp
  CHECK(localLightAttenuation(10.0f) == Catch::Approx(1.0f / (7.0f + 3.0f)));
  ```
  `localLightAttenuation` is never called in `src/`. The attenuation that runs is `assets/shaders/local_lights.glsli:17`: `ndl / (0.7 * dist + 0.03 * dist * dist)`.
- **Why:** This tests a CPU copy of shader code that the application never runs. A wrong constant in the shader passes.
- **Replace with:** delete `localLightAttenuation` and this test. If the falloff matters, check it on a `scene_render` picture of a lamp at known distances.

### tests/test_minimap_projection.cpp:44 - "a marker lands on the ground the shader draws there"
- **Category:** D
- **Severity:** high
- **Evidence:**
  ```cpp
  /// minimap_display.frag.glsl, in C++. ...
  glm::vec2 shaderSampleUV(float px, float py, float mapRadius, float zoomK, float rotation) {
  const float zoomK = v.viewRadius / (kTileSize * 3.0f);  // "the one Minimap::render computes"
  ```
- **Why:** The oracle is a CPU replica of the fragment shader plus a copy of `Minimap::render`'s zoom formula, which the user decision forbids. A change to the shader's axis convention is not seen, and the replica can drift from the shader.
- **Replace with:** a `scene_render`/minimap picture check that a marker pixel lands on a known ground feature. Keep the round-trip and scale cases (L80-136), which test the CPU projection alone.

### tests/test_m2_color_track.cpp:51 - "the record and track offsets differ by version"
- **Category:** A, G
- **Severity:** high
- **Evidence:**
  ```cpp
  CHECK(m2ColorTrackKeysOffset(true) == 12);
  CHECK(m2ColorAlphaTrackOffset(true) == 20);
  CHECK(m2ColorRecordSize(false) == 56);
  ```
  `m2ColorAlphaTrackOffset` and `m2ColorRecordSize` are not used anywhere in `src/` (only defined in `include/pipeline/m2_color_track.hpp`).
- **Why:** These copy the constants from the header. Two of the functions are dead code.
- **Replace with:** delete the dead functions and the case. The byte-buffer cases at L62-171 already pin the keys offset through behaviour.

### tests/test_m2_material.cpp:101 - "transparency combos resolve reordered and invisible tracks"
- **Category:** G
- **Severity:** high
- **Evidence:**
  ```cpp
  CHECK(m2TextureWeightOpacity(weights, lookup, 0) == 0.75f);
  ```
  The only callers of `m2TextureWeightOpacity` are this file (here and in the asset case at L154-159).
- **Why:** This tests dead code. The running renderer uses `m2BatchOpacity`/`m2SampleBatchOpacity` (`m2_sky_material.hpp`).
- **Replace with:** delete the function and these assertions. The Dalaran-fountain weight checks belong on `m2BatchOpacity`.

### tests/test_m2_structs.cpp:92 - "M2Sequence fields are default-initialized" (and the cases through L248)
- **Category:** B, F
- **Severity:** high
- **Evidence:**
  ```cpp
  mat.blendMode = 2; REQUIRE(mat.blendMode == 2);                  // L168-169
  tex.filename = "...Hogger.blp"; REQUIRE(tex.filename == "...");  // L144-145
  glm::vec3 center = (model.boundMin + model.boundMax) * 0.5f;     // L192, computed in the test
  ```
- **Why:** Twelve cases (M2Sequence defaults, M2Bone, M2Vertex layout, M2Texture, M2Batch, M2Material, bounding box, M2ParticleEmitter, M2Attachment, collections, ...) are set-then-get, default member initialisers, or arithmetic that never calls production code. None can fail on a loader or renderer bug. `isValid`/`hasData` (L102, L175) are trivial.
- **Replace with:** delete L92-248. Loader behaviour is already tested from bytes in test_m2_color_alpha, test_m2_texture_transform and others.

### tests/test_movement_flag_bits.cpp:34 - "each flag is the bit the server means by it" (and L60, L67)
- **Category:** A
- **Severity:** high
- **Evidence:**
  ```cpp
  CHECK(bits(MovementFlags::FORWARD) == 0x00000001);
  CHECK(bits(MovementFlags::WATER_WALK) == 0x10000000);
  CHECK(bits(MovementFlags::FEATHER_FALL) != kPendingStop);
  ```
- **Why:** This is the example AGENTS.md forbids: enum values copied into the test. It fails only when someone edits the enum, and then they edit the test too.
- **Replace with:** a GameHarness test. Receive `SMSG_MOVE_WATER_WALK`, make the player move, and check that the sent `MSG_MOVE_*` flag word has bit 0x10000000 set and 0x4000/0x8000 clear.

### tests/test_movement_flag_bits.cpp:79 - "no two flags share a bit"
- **Category:** A
- **Severity:** medium
- **Evidence:**
  ```cpp
  CHECK((bit & (bit - 1)) == 0);
  ... seen.find(bit) ...
  ```
- **Why:** It checks the enum definition against itself. It catches a duplicate typo, but not a wrong-but-unique bit, which is the bug the file was written for.
- **Replace with:** the same packet round trip as above, with several states active at once.

### tests/test_movement_flag_bits.cpp:132 - "vertical motion counts in both directions" (and L139, L151, L166)
- **Category:** A
- **Severity:** medium
- **Evidence:**
  ```cpp
  CHECK((kLocomotionFlags & bits(MovementFlags::DESCENDING)) != 0);
  ```
- **Why:** It asserts which bits a constant mask contains. The bug it names (heartbeats throttled for a descending player) is never driven, so a site that stops using `kLocomotionFlags` passes.
- **Replace with:** GameHarness: set the player swimming down with no other flags, advance past the throttle, and assert heartbeats (`MSG_MOVE_HEARTBEAT`) are sent; a stationary player gets none.

### tests/test_movement_limits.cpp:7 - "stock hill climbing limits are shared by all surfaces"
- **Category:** A, G, B
- **Severity:** high
- **Evidence:**
  ```cpp
  REQUIRE(kMaxWalkableSlopeDegrees == 50.0f);
  REQUIRE(isWalkableNormal(kMinWalkableNormalZ));
  REQUIRE(isReachableStep(kMaxStepUp));
  ```
  `isWalkableNormal`, `isReachableStep` and `kMaxWalkableSlopeDegrees` are not used in `src/`.
- **Why:** A constant check on dead helpers. The boundaries are the code's own constants, so a wrong `kMinWalkableNormalZ` still passes.
- **Replace with:** delete the dead helpers. Test walkability through `CollisionWorld`/camera controller behaviour on a 49-degree versus 51-degree slope, with expectations from original-client traces.

### tests/test_movement_limits.cpp:22 - "a walkable slope out-climbs the step-up budget in a long frame"
- **Category:** F
- **Severity:** medium
- **Evidence:**
  ```cpp
  auto riseOverFrame = [](float speed, float frame) { return speed * frame * kSteepestRisePerYard; };
  CHECK(riseOverFrame(14.0f, 1.0f / 20.0f) > kMaxStepUp);
  ```
- **Why:** No production function is called. The only thing it checks is the magnitude of `kMaxStepUp`. It documents a hazard but cannot catch the grounding bug it describes.
- **Replace with:** drive the grounding step with a 50 ms frame at mount speed on a 50-degree slope and assert the player stays on the surface.

### tests/test_m2_material.cpp:21 - "modulation uses destination RGB and keeps its alpha" (and L34 "NoAlphaAdd adds the whole texel")
- **Category:** B
- **Severity:** medium
- **Evidence:**
  ```cpp
  CHECK(state.srcColorBlendFactor == VK_BLEND_FACTOR_DST_COLOR);
  CHECK(state.dstColorBlendFactor == (twice ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_ZERO));
  ```
  This mirrors `include/rendering/m2_material.hpp:47-53` line for line.
- **Why:** It restates the implementation. Blending is GPU behaviour, and any intended change edits both places.
- **Replace with:** a `scene_render` check that a Mod2x layer over a mid-grey background darkens or brightens the expected pixels.

### tests/test_m2_cull_grid.cpp:42 - "a column is kept whenever one of its instances would cast"
- **Category:** D
- **Severity:** medium
- **Evidence:**
  ```cpp
  // The shadow pass's per-instance test, as M2Renderer::renderShadow writes it.
  bool instanceCasts(...) { const float margin = boundRadiusTimesScale / shadowRadius * 1.5f; ...
  ```
  This copies the `insideFootprint` lambda at `src/rendering/m2_renderer_render.cpp:2604-2613`, and omits its perspective branch.
- **Why:** The safety property is checked against a replica. If renderShadow's per-instance test widens (or the perspective path is used), column culling can drop casters and this still passes.
- **Replace with:** extract the per-instance footprint test into a function that renderShadow calls, and use that function as the oracle. Or run `WOWEE_M2_CULL_CHECK` in a `scene_render` scene.

### tests/test_m2_texture_transform.cpp:90 - "a translation slides the texture" (and L97, L110, L120, L130)
- **Category:** G, D
- **Severity:** medium
- **Evidence:**
  ```cpp
  const auto t = m2UVTranslation({0.25f, -0.5f});
  const glm::vec2 uv = m2ApplyUVTransform(t, {0.0f, 0.0f});
  ```
  `m2UVTranslation`, `m2ApplyUVTransform` and `m2UVTransformIsIdentity` have no caller in `src/`. `m2ApplyUVTransform` is the shader's row multiply in C++.
- **Why:** L90 tests a dead helper outright. The others read production's `m2UVTransform`/`m2SampleUVTransform` output through a CPU copy of shader code, so a row-order mismatch with the shader passes.
- **Replace with:** keep the assertions on the rows `m2SampleUVTransform` returns (literal row values). Delete the test-only helpers. Check the visible effect with a `scene_render` scrolling-texture scene.

### tests/test_movement_payload_layout.cpp:66 - "WotLK movement payload keeps the jump" (and L72, L134, L145)
- **Category:** B
- **Severity:** medium
- **Evidence:**
  ```cpp
  parsers.writeMovementPayload(packet, written);
  REQUIRE(parsers.readMovementPayload(incoming, read));
  CHECK(read.jumpSinAngle == written.jumpSinAngle);
  ```
- **Why:** Each reader is checked against its own writer. A symmetric error (fields swapped or omitted in both) passes; the file header admits it is "not an outside oracle". L97 shows the right shape: literal wire bytes.
- **Replace with:** literal byte buffers in each expansion's wire order (from a capture or the server source) for the read side, and literal expected bytes for the write side.

### tests/test_link_hit.cpp:125 - "a chat line's link is found by a click on its text"
- **Category:** D
- **Severity:** medium
- **Evidence:**
  ```cpp
  /// Mirrors drawMarkupText's advance ...
  for (auto it = filed.rbegin(); it != filed.rend(); ++it) { if (clickX >= it->x0 && ...) return it->link; }
  ```
- **Why:** The layout advance and the hit test are copies. Production has `WidgetTree::addLinkRect`/`linkAt` (`include/ui/widget_tree.hpp:711-718`) and the advance in `widget_renderer.cpp:633`. A bug in `linkAt` (for example the wrong search order) or in the real advance passes.
- **Replace with:** file the rects with `WidgetTree::addLinkRect` and query `linkAt`. Better, draw the line through the widget renderer in a client harness and click.

### tests/test_movement_domain.cpp:292 - "a ship sails the path TransportAnimation.dbc gives its entry, and a boat its TaxiPathNode.dbc path"
- **Category:** C, F
- **Severity:** medium
- **Evidence:**
  ```cpp
  transports.loadTransportAnimationDBC(h.gameData());
  CHECK(transports.hasPathForEntry(20808));
  ```
- **Why:** The test calls the loader step itself instead of letting world entry load it, and it checks only that a path exists, not that the ship is where the path puts it. Wrong timing or interpolation of the path passes.
- **Replace with:** spawn the transport game object with entry 20808 through `SMSG_UPDATE_OBJECT`, advance 10 s, and assert its position is (100, 0, 0).

### tests/test_movement_domain.cpp:95 - "the player rides the transport the server names..." (helpers L58-86; also L22, L201)
- **Category:** C
- **Severity:** low
- **Evidence:**
  ```cpp
  h.game().world().applyRecordedObjects(batch);
  h.game().getEntityManager().addEntity(...);
  ```
- **Why:** These cases inject decoded `UpdateBlock`s and entities and skip the `SMSG_UPDATE_OBJECT` decoder. A decoder bug in the transport fields (`onTransport`, offsets) is not covered.
- **Replace with:** `h.receive(SMSG_UPDATE_OBJECT, ...)` built with the harness's create/movement body helpers.

### tests/test_local_lights.cpp:38 - "only the grid cells around a receiver are searched"
- **Category:** C
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(localLightCellRange(30.0f, 0.0f) == std::array<int, 2>{1, 2});
  ```
  `localLightCellRange` is called only by `pickLocalLights`.
- **Why:** It pins an internal step. The same case already checks the observable result at L44-47.
- **Replace with:** keep only the `pickLocalLights` assertions.

### tests/test_local_lights.cpp:10 - "a receiver takes the nearest lights, nearest first"
- **Category:** D
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(localLightSlot(p, k) == expected[k]);
  ```
  `localLightSlot` has no caller in `src/`; it repeats the unpack in `local_lights.glsli:26`.
- **Why:** The packing is checked through a copy of the shader's decode, so a packing order the shader does not share passes.
- **Replace with:** assert the raw `uvec4` words as literals (for example `p[0] == (5u << 16 | 1u)`), which is the contract with the shader.

### tests/test_m2_blend_mode.cpp:180 - "an opaque foliage batch is cut out only where its alpha is a silhouette" (and L195, L210)
- **Category:** A
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(s.alphaTest == 2);
  CHECK(s.alphaTest == 4);
  CHECK(s.alphaTest == 3);
  ```
- **Why:** These are internal shader codes (`m2_blend_mode.hpp:161`). The behaviour, a 224/255 cut for alpha key, lives in the shader. The `forceCutout` and pipeline assertions in the same cases are fine.
- **Replace with:** a `scene_render` alpha-key picture. Or name the codes (an enum) and assert the meaning, not the number.

### tests/test_m2_sway.cpp:43 - "the sway modes are unchanged by that"
- **Category:** A
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(sway.mode == 3);
  CHECK(sway.amp == Catch::Approx(0.3f));
  ```
- **Why:** These are shader switch codes and a tuning constant. The visible sway is GPU behaviour. The `plantHeight`/`refHeight` assertions are fine.
- **Replace with:** a `scene_render` banner check (top fixed, bottom moving), or at least an enum for `mode`.

### tests/test_m2_color_alpha.cpp:83 - "Sharpened Blade sparkle preserves authored pulse timing" (same pattern: test_m2_footstep_events.cpp:39/60/74, test_m2_material.cpp:58/112/164/187/209, test_m2_sky_material.cpp:30/133/156, test_m2_ribbon_tracks.cpp:51/92)
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  if (data.empty()) { SUCCEED("model asset not extracted; ..."); return; }
  ```
- **Why:** A run without data reports these as passed. `test_m2_billboard_model.cpp:16` uses `SKIP`, which is honest.
- **Replace with:** `SKIP(...)`.

### tests/test_m2_ribbon_tracks.cpp:51 - "a ribbon model's tracks parse into usable ranges"
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(value >= -0.001f); CHECK(value <= 1.001f);
  ...
  if (model.ribbonEmitters.empty()) { SUCCEED(...); return; }
  ```
- **Why:** Range-only checks, with an early pass when there are no ribbons. An empty parse passes the second case too.
- **Replace with:** literal expected key values for one emitter, taken from an independent parser, as test_m2_footstep_events does.

### tests/test_m2_color_track.cpp:110 - "reading a vanilla track as WotLK finds the wrong place"
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  if (asWotlk) CHECK(wotlkAt != vanillaAt);
  ```
- **Why:** The assertion is conditional, so the case passes with no check executed when the parser refuses.
- **Replace with:** assert the specific outcome for this buffer (it refuses, or it returns a known offset).

### tests/test_m2_material.cpp:93 - "authored priorities sort stably without reordering the batches"
- **Category:** B
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(batches[1].priorityPlane == 2);
  ```
  The input is `const` and passed by `const&`.
- **Why:** The compiler already guarantees this. The order assertion on L96 is fine.
- **Replace with:** drop the line.

### tests/test_m2_view_distance.cpp:58 - "Environment detail can thin the clutter but not push it past the ground"
- **Category:** D
- **Severity:** low
- **Evidence:**
  ```cpp
  // The base the scale produces, as M2Renderer computes it.
  auto baseFor = [](float detail) { ... dist = scale * 1000.0f; ... };
  ```
- **Why:** The input comes from a copy of renderer code, so a change in how the renderer derives the base is not covered. The assertions still exercise `m2InstanceMaxDistSq` properly.
- **Replace with:** pass literal base distances (500, 1500), or move the base computation into a tested function.

### tests/test_melee_anim_chains.cpp:39 - "each chain asks first for what the weapon actually is"
- **Category:** A
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(chain(MeleeChain::OneHand).front() == ATTACK_1H);
  ```
- **Why:** It reads table entries back. The invariant cases (L55-122) are good.
- **Replace with:** for a model holding a given set of sequences, assert what `resolveMeleeAnimId` plays (for example, dagger plus a model with ATTACK_1H_PIERCE plays the pierce).

### tests/test_locomotion_fsm.cpp:136 - "LocomotionFSM: STAY when WALK_BACKWARDS missing from caps"
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  // Should still resolve something - falls back to walk
  REQUIRE(out.valid);
  ```
- **Why:** Any animation passes, including STAND.
- **Replace with:** `CHECK(out.animId == anim::WALK)`.

### tests/test_m2_billboard_model.cpp:14 - "Mana Shield's card faces the camera whichever way its unit faces"
- **Category:** C
- **Severity:** low
- **Evidence:**
  ```cpp
  #include "rendering/m2_renderer_internal.h"
  rendering::computeBoneMatrices(gpu, inst, 350.0f, 0.0f, &view);
  ```
- **Why:** It hand-fills `M2ModelGPU` and calls an internal renderer step. The assertion (card normal toward camera) is a good behaviour check.
- **Replace with:** a `scene_render` scene with the model at several yaws. Acceptable as is until then.

### tests/test_model_bounds.cpp:60 - "the sphere is no larger than it has to be" (L71-72; also L86 in "a model centred on its origin is where the formulas agree")
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  const float halfDiagonal = glm::length(bounds.max - bounds.min) * 0.5f;
  CHECK(halfDiagonal == Catch::Approx(1.7320508f));
  ```
- **Why:** This checks glm arithmetic done in the test. The `bounds.radius` assertions next to it are the real ones.
- **Replace with:** drop the halfDiagonal assertions, or turn them into comments.

### tests/test_movement_limits.cpp:154 - SECTION "a gap test would have called the Orgrimmar ramp a tunnel"
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  REQUIRE(26.0326f - 24.698f > kStepUp);
  ```
  L223 in the Undercity section is the same kind of check.
- **Why:** Literal arithmetic; no code under test is called.
- **Replace with:** a comment.

### tests/test_movement_flags.cpp:42 - "each single-flag predicate reads its own bit"
- **Category:** B
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(withFlags(bit(MovementFlags::ROOT)).isPlayerRooted());
  ```
- **Why:** The input is built from the same enum the predicate reads, so a wrong enum value passes. The "everything else set" case at L51 does catch a predicate pointed at the wrong constant.
- **Replace with:** literal wire bits (`withFlags(0x00000800)`), taken from the server definition.

### tests/test_nameplate_stacking.cpp:65 - "the lift gives up rather than spinning"
- **Category:** F
- **Severity:** low
- **Evidence:**
  ```cpp
  CHECK(y <= 400.0f);
  ```
- **Why:** Almost any return value satisfies it. Only a hang would fail.
- **Replace with:** assert the work bound (the result equals the value after the guard's maximum number of lifts), or that the result is finite and above the 40-plate pile.

## 3. Good examples

1. **tests/test_loot_domain.cpp.** End to end through `GameHarness`. Real `SMSG_LOOT_*` bytes go in. The test asserts the addon events and their arguments (`LOOT_SLOT_CLEARED "2"`), the chat lines, the bytes of `CMSG_LOOT_RELEASE`/`CMSG_LOOT_ROLL`, and that nothing carries over to the next character.
2. **tests/test_mailbox.cpp** (and **tests/test_loot.cpp**). The context is tested on its own: CMake links only `wowee_mail`/`wowee_loot`, the bus is the real `EventBus`, and a recording gateway stands in for the server. The tests assert events and requests sent, not internals.
3. **tests/test_minimap_zoom.cpp.** Drives `Minimap:SetZoom` from Lua through the client harness. It asserts what FrameXML sees (`GetZoom`, `MINIMAP_UPDATE_ZOOM` counts) and that the CVar file persists across a restart.
4. **tests/test_model_import.cpp.** Fakes only the outside source and asserts real files written, removed or left alone, plus header bytes read back (version 264, cleared texture names).
5. **tests/test_liquid_types.cpp.** Puts a literal LiquidType.dbc and literal MCLQ bytes through the real table and loader, then asks the scene what kind of liquid is at a point. The oracle (row 21 is slime) comes from the data, not the code.
