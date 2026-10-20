# Unit test infrastructure using Catch2 v3 (amalgamated)

# Catch2 amalgamated as a library target
add_library(catch2_main STATIC
    ${CMAKE_SOURCE_DIR}/extern/catch2/catch_amalgamated.cpp
)
target_include_directories(catch2_main PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/catch2
)
# Catch2 v3 needs C++17 minimum
target_compile_features(catch2_main PUBLIC cxx_std_17)

# ── ASAN / UBSan propagation ────────────────────────────────
# Collect all test target names so we can apply sanitizer flags at the end.
set(ALL_TEST_TARGETS "")

# Helper: register a test target for ASAN/UBSan if enabled.
# Also links Winsock on Windows: several tests pull in networking sources
# (auth, socket code) whose symbols live in ws2_32 there rather than libc.
# Linking it for every test keeps a new test from breaking the Windows build.
macro(register_test_target _target)
    target_compile_definitions(${_target} PRIVATE WOWEE_PHASE_CHECKS_FATAL=1)
    # A global property, not a list variable. This macro is called both at file
    # scope and from inside wowee_add_test, which is a function -- and
    # list(APPEND) inside a function writes to that function's scope and is
    # discarded on return. Every test registered through wowee_add_test was
    # therefore missing from the list: 79 of ~144 targets were in it, and the
    # sanitizer loop that walks it silently skipped the rest.
    set_property(GLOBAL APPEND PROPERTY WOWEE_ALL_TEST_TARGETS ${_target})
    if(WIN32)
        target_link_libraries(${_target} PRIVATE ws2_32)
    endif()
endmacro()

# Shared source files used across multiple tests: see wowee_test_common below,
# defined once TEST_INCLUDE_DIRS exists.

# Include directories matching the main target
# GLM, resolved the three ways find_package can present it.
#
# It is not vendored: it comes from find_package(glm QUIET), and its include
# path arrives with the imported target rather than through any directory this
# file lists. Three shapes exist in the wild - GLM 1.0 exposes the headers as
# glm::glm-header-only and may build glm::glm as a real library beside it, and
# some distributions provide neither target, only GLM_INCLUDE_DIRS. A target
# that asks for glm::glm alone gets nothing in the third case and fails on
# glm/glm.hpp.
#
# Linux has a system copy under /usr/include and compiles either way, so this
# only ever shows up somewhere else - which is what makes it easy to leave
# broken. The main target and the editor have walked all three branches since
# they were written; this is the same ladder, once, for the tests.
function(wowee_test_link_glm target)
    if(TARGET glm::glm-header-only)
        target_link_libraries(${target} PRIVATE glm::glm-header-only)
    elseif(TARGET glm::glm)
        target_link_libraries(${target} PRIVATE glm::glm)
    elseif(glm_FOUND)
        target_include_directories(${target} PRIVATE ${GLM_INCLUDE_DIRS})
    endif()
endfunction()

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
set(TEST_SYSTEM_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/extern
)

# Shared sources every test may opt into, compiled once.
#
# These used to be a plain source list spliced into each test's add_executable,
# which meant one object file per test that asked for it: logger.cpp alone was
# compiled 112 times in a clean build. An OBJECT library builds it once and the
# generator expression below splices the same object into each executable, so
# every existing ${TEST_COMMON_SOURCES} call site keeps working untouched.
#
# It stays a source-list-shaped variable rather than a link library on purpose.
# The opt-in property the wowee_add_test comment relies on - that a test which
# does not ask for the logger fails to link if its subject starts using one -
# only survives while omitting the variable omits the symbols.
add_library(wowee_test_common OBJECT
    ${CMAKE_SOURCE_DIR}/src/core/logger.cpp
)
target_include_directories(wowee_test_common PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(wowee_test_common SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_features(wowee_test_common PRIVATE cxx_std_20)
set(TEST_COMMON_SOURCES $<TARGET_OBJECTS:wowee_test_common>)

# Helper: a test executable with the scaffolding every one of them wants.
#
# Each test was registered by hand in nine lines of which six never varied:
# the two include-directory calls, catch2_main, glm, add_test and
# register_test_target. Copying one block to make the next is how a test came
# to link sources it did not need and how another came to miss its own.
#
# Sources are passed in full and nothing is added: several tests deliberately
# do not link TEST_COMMON_SOURCES, because a pure-domain header that starts
# using the logger should fail to link in its standalone test rather than
# quietly work. Adding it for everyone would remove that.
#
# The ctest name is the target with "test_" stripped, which is what every block
# already wrote by hand.
#
#   wowee_add_test(test_foo SOURCES test_foo.cpp ${TEST_COMMON_SOURCES})
#
# Tests that also need SDL3, OpenSSL or Vulkan keep their own block: the link
# set is the interesting part of those.
function(wowee_add_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${target} ${ARG_SOURCES})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDE_DIRS})
    target_include_directories(${target} SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE catch2_main)
    wowee_test_link_glm(${target})
    string(REGEX REPLACE "^test_" "" _name ${target})
    add_test(NAME ${_name} COMMAND ${target})
    register_test_target(${target})
endfunction()

# A test of the packet layer: its own source, the common set, and the layer
# from wowee_protocol. The parsers pull in the rest of the layer through their
# shared base, so a test of any one packet needs all of it - linked, not
# compiled again for every test as it was.
function(wowee_add_packet_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${target} ${target}.cpp ${TEST_COMMON_SOURCES} ${ARG_SOURCES})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDE_DIRS})
    target_include_directories(${target} SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE catch2_main wowee_protocol SDL3::SDL3)
    wowee_test_link_glm(${target})
    if(TARGET Vulkan::Vulkan)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endif()
    string(REGEX REPLACE "^test_" "" _name ${target})
    add_test(NAME ${_name} COMMAND ${target})
    register_test_target(${target})
endfunction()



# The event bus: delivery, subscription lifetime, posting across threads.
wowee_add_test(test_event_bus
    SOURCES test_event_bus.cpp ${TEST_COMMON_SOURCES})

# The resource telemetry behind the performance HUD's graphs and the resource
# log line: its history, its frame summaries, and the warnings a crash log
# should end with, once per crossing. No logger: the owner logs what it says.
wowee_add_test(test_resource_telemetry
    SOURCES test_resource_telemetry.cpp
            ${CMAKE_SOURCE_DIR}/src/core/resource_telemetry.cpp
            ${CMAKE_SOURCE_DIR}/src/core/frame_budget.cpp)

# The frame budget the profile, the benchmark and scene_render report: the
# frame time's spread and hitches, each stage's tail, and stages nested under
# the one they are part of. No logger: the owner logs the lines.
wowee_add_test(test_frame_budget
    SOURCES test_frame_budget.cpp
            ${CMAKE_SOURCE_DIR}/src/core/frame_budget.cpp)

# WOWEE_BENCHMARK's run: when the flight starts and ends, where the camera
# goes, and what the report covers. No client.
wowee_add_test(test_benchmark_run
    SOURCES test_benchmark_run.cpp
            ${CMAKE_SOURCE_DIR}/src/core/benchmark_run.cpp
            ${CMAKE_SOURCE_DIR}/src/core/frame_budget.cpp)

# The pipeline cache kept between runs: checked against the device, written
# whole or not at all.
wowee_add_test(test_pipeline_cache_file
    SOURCES test_pipeline_cache_file.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/pipeline_cache_file.cpp)
target_link_libraries(test_pipeline_cache_file PRIVATE wowee_base)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(test_pipeline_cache_file PRIVATE Vulkan::Vulkan)
endif()

# Which textures go up by host image copy: none it would keep out of video memory.
wowee_add_test(test_host_image_copy
    SOURCES test_host_image_copy.cpp)

# The ranges the terrain's shared geometry pages hand out and take back.
wowee_add_test(test_range_allocator
    SOURCES test_range_allocator.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/range_allocator.cpp)

# Which of the two memory pressure levels a share of free RAM is.
wowee_add_test(test_memory_pressure
    SOURCES test_memory_pressure.cpp)
target_link_libraries(test_memory_pressure PRIVATE wowee_base)

# What the video memory is used for, from the allocator's detailed map: each
# allocation by its usage flags, in VRAM or system memory. No device.
wowee_add_test(test_gpu_memory
    SOURCES test_gpu_memory.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/gpu_memory.cpp)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(test_gpu_memory PRIVATE Vulkan::Vulkan)
endif()

# The renderers' one texture cache: one entry per key, one budget, eviction of
# only what nothing references, nothing freed before its frames have fenced.
# The device is a fake; no GPU.
wowee_add_test(test_texture_cache
    SOURCES test_texture_cache.cpp ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/rendering/texture_cache.cpp)

# Frames recorded only against a complete set of frame targets; a failure to
# make one said once per stretch with the result, the size and the budget.
wowee_add_test(test_frame_targets
    SOURCES test_frame_targets.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/frame_targets.cpp)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(test_frame_targets PRIVATE Vulkan::Vulkan)
endif()

# Normal/height maps encoded for the GPU: BC7 blocks that read back close to
# what was derived, a mip chain down to one texel.
wowee_add_test(test_normal_map_encoding
    SOURCES test_normal_map_encoding.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/normal_map_encoding.cpp)

# Normal maps built on the builder's own few workers from a bounded queue,
# nearest first. The derivation is a stand-in.
wowee_add_test(test_normal_map_builder
    SOURCES test_normal_map_builder.cpp ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/rendering/normal_map_builder.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/normal_map_encoding.cpp)

# What a vendor sell asks for. Header-only arithmetic, no game state.
wowee_add_test(test_sell_count
    SOURCES test_sell_count.cpp)

# Stock-style hill climbing regression test. Run directly with:
#   cmake --build build --target test_hill_climbing && ctest --test-dir build -R hill_climbing
wowee_add_test(test_hill_climbing
    SOURCES test_movement_limits.cpp)

# ── test_spell_description_eval ─────────────────────────────
# The expression language spell descriptions are written in. Sinister Strike's
# tooltip read "percent>% of your normal weapon damage" because nothing read
# SpellDescriptionVariables.dbc and the formatter had no branch for "$<".
wowee_add_test(test_spell_description_eval
    SOURCES test_spell_description_eval.cpp
            ${CMAKE_SOURCE_DIR}/src/game/spell_description_eval.cpp)

# ── test_asset_inventory ────────────────────────────────────
# What is installed and whether it can be used. The client used to start with
# nothing extracted and fail far from the cause, never saying there were no
# assets or where it had looked. Cheap on purpose: the extraction manifest is
# thirty megabytes and nothing here parses it.
wowee_add_test(test_asset_inventory
    SOURCES test_asset_inventory.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/asset_inventory.cpp)

# ── test_data_paths ─────────────────────────────────────────
# Where the assets go, which the client and the asset manager have to agree on
# and did not: one looked in a macOS-only place, the other wrote beside the
# terminal's working directory. Also what counts as installed, since the
# manifest is written last and a folder without one is a run that stopped.
# config_paths for where the executable is, which is where the assets go.
wowee_add_test(test_data_paths
    SOURCES test_data_paths.cpp
            ${CMAKE_SOURCE_DIR}/src/core/config_paths.cpp
            ${TEST_COMMON_SOURCES})

# ── test_log_privacy ────────────────────────────────────────
# The home directory, and with it the account name, written as ~ in the log.
wowee_add_test(test_log_privacy
    SOURCES test_log_privacy.cpp)

# ── test_asset_profiles ─────────────────────────────────────
# The asset sets on offer. Pinned because the upgrades used to be written per
# base and so were only ever offered on Wrath, and because the plan's ordering
# is load-bearing: an upscale before the models arrive sharpens nothing.
wowee_add_test(test_asset_profiles
    SOURCES test_asset_profiles.cpp
            ${CMAKE_SOURCE_DIR}/tools/asset_manager/profiles.cpp)
target_include_directories(test_asset_profiles PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/asset_manager)

# ── test_asset_pack ─────────────────────────────────────────
# A pack is a file people send each other, so its reader is handed archives
# nobody here wrote. The round trip is checked, and so is the refusal: a zip
# entry names where its own contents land, and "../.." is a name.
wowee_add_test(test_asset_pack
    SOURCES test_asset_pack.cpp
            ${CMAKE_SOURCE_DIR}/tools/asset_manager/pack.cpp)
target_include_directories(test_asset_pack PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/asset_manager)
target_link_libraries(test_asset_pack PRIVATE ZLIB::ZLIB)

# ── test_model_import ───────────────────────────────────────
# The pass that brings a later client's models into this one's format. It once
# matched the name inside a model against the name of the file it would replace
# and converted nothing at all, so the path matching is pinned here along with
# every refusal - a model written without its skin or its textures is worse than
# the one it replaced.
wowee_add_test(test_model_import
    SOURCES test_model_import.cpp
            ${CMAKE_SOURCE_DIR}/tools/asset_manager/model_import.cpp)
target_include_directories(test_model_import PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/asset_manager)

# ── test_cli_catalog_paths ──────────────────────────────────
# Working a catalog's base name back out of a path the user typed. Pure header
# in tools/editor, so the test reaches into that directory for it.
add_executable(test_cli_catalog_paths test_cli_catalog_paths.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_cli_catalog_paths PRIVATE
    ${TEST_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/tools/editor)
target_include_directories(test_cli_catalog_paths SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_cli_catalog_paths PRIVATE catch2_main)
add_test(NAME cli_catalog_paths COMMAND test_cli_catalog_paths)
register_test_target(test_cli_catalog_paths)

# ── test_shared_rules ───────────────────────────────────────
# The rules this codebase collapsed out of many copies into one. A rule with a
# single home is a rule one mistake breaks everywhere, so the cases that were
# actually wrong in one of the copies are pinned here.
# Pure rules only - the item texture paths and the settings vocabulary are
# header-only, and skinPathForM2 comes from m2_loader, which knows nothing about
# where bytes come from.
wowee_add_test(test_shared_rules
    SOURCES test_shared_rules.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/settings_schema.cpp
            ${TEST_COMMON_SOURCES})

# ── test_geoset_rules ────────────────────────────────────────
# Choosing a geoset when the model lacks the one asked for. Pure header, so the
# test needs nothing but Catch.
add_executable(test_geoset_rules test_geoset_rules.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_geoset_rules PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_geoset_rules SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_geoset_rules PRIVATE catch2_main)
add_test(NAME geoset_rules COMMAND test_geoset_rules)
register_test_target(test_geoset_rules)

# ── test_framexml ───────────────────────────────────────────
add_executable(test_framexml
    test_framexml.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/framexml_emitter.cpp
)
target_include_directories(test_framexml PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_framexml SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_framexml PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_framexml PRIVATE catch2_main)
add_test(NAME framexml COMMAND test_framexml)
register_test_target(test_framexml)

# ── test_framexml_takeover ──────────────────────────────────
# Which interface draws what in the configuration a run actually gets. The
# transition's state is "FrameXML draws all of it", and that rested on a list
# of forty-nine names and a grouping rule agreeing about fifty-two elements,
# in a file where either can be edited alone. An element falling out is not an
# error - this client just draws its own version again.
add_executable(test_framexml_takeover
    test_framexml_takeover.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/framexml_takeover.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_framexml_takeover PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_framexml_takeover SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_framexml_takeover PRIVATE catch2_main)
add_test(NAME framexml_takeover COMMAND test_framexml_takeover)
register_test_target(test_framexml_takeover)

# ── test_lua_handler_globals ────────────────────────────────
# The pre-3.0 handler globals, which are a pure function of the Lua stack and
# so need no game, window or interface - while the fault they prevent is the
# largest one this client can have: `this` nil on the first line of every
# OnLoad in a 1.12 interface, so every frame it declares reaches the screen at
# once and none of them where it belongs.
add_executable(test_lua_handler_globals
    test_lua_handler_globals.cpp
    ${CMAKE_SOURCE_DIR}/src/addons/lua_handler_globals.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_lua_handler_globals PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_lua_handler_globals SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_lua_handler_globals PRIVATE catch2_main lua51)
wowee_test_link_glm(test_lua_handler_globals)
add_test(NAME lua_handler_globals COMMAND test_lua_handler_globals)
register_test_target(test_lua_handler_globals)

# Verify process termination separately from Catch's signal handling, and the
# shipping policy against interleaved errors (the logger's adjacent dedupe
# alone must not make this test pass).
foreach(policy IN ITEMS fatal release)
    add_executable(frame_phase_${policy} frame_phase_probe.cpp ${TEST_COMMON_SOURCES})
    target_include_directories(frame_phase_${policy} PRIVATE ${TEST_INCLUDE_DIRS})
    wowee_test_link_glm(frame_phase_${policy})
    if(policy STREQUAL "fatal")
        target_compile_definitions(frame_phase_${policy} PRIVATE WOWEE_PHASE_CHECKS_FATAL=1)
        set(fatal_checks ON)
    else()
        target_compile_definitions(frame_phase_${policy} PRIVATE NDEBUG)
        set(fatal_checks OFF)
    endif()
    add_test(NAME frame_phase_${policy} COMMAND ${CMAKE_COMMAND}
        -DPROBE=$<TARGET_FILE:frame_phase_${policy}> -DFATAL_CHECKS=${fatal_checks}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_frame_phase.cmake)
endforeach()

# ── test_lua_generic_for ────────────────────────────────────
# `for k, v in t do` over a plain table - the Lua 5.0 form 1.12's FrameXML is
# written in. It raised on the vendored 5.1, and raising while a file is read
# takes the whole file with it: six of FrameXML's died on this one construct,
# the player, target and party frames among them.
add_executable(test_lua_generic_for
    test_lua_generic_for.cpp
)
target_include_directories(test_lua_generic_for PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_lua_generic_for SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_lua_generic_for PRIVATE catch2_main lua51)
wowee_test_link_glm(test_lua_generic_for)
add_test(NAME lua_generic_for COMMAND test_lua_generic_for)
register_test_target(test_lua_generic_for)

# ── test_lua_arg_coercion ───────────────────────────────────
# The lenient numeric argument, which is a pure function of the Lua stack and
# so needs no game, window or interface - while the fault it prevents is
# invisible at every other level: a raise inside a click handler is swallowed,
# so an auction search that never sent looked exactly like one that found
# nothing.
add_executable(test_lua_arg_coercion
    test_lua_arg_coercion.cpp
)
target_include_directories(test_lua_arg_coercion PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_lua_arg_coercion SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_lua_arg_coercion PRIVATE catch2_main lua51)
wowee_test_link_glm(test_lua_arg_coercion)
add_test(NAME lua_arg_coercion COMMAND test_lua_arg_coercion)
register_test_target(test_lua_arg_coercion)

# ── framexml_emit ────────────────────────────────────────────
# Not a test: a one-file dump of what the emitter produces, for answering
# questions about emitted Lua without starting the client.
add_executable(framexml_emit
    ${CMAKE_SOURCE_DIR}/tools/framexml_emit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/framexml_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/xml_parser.cpp
)
target_include_directories(framexml_emit PRIVATE ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_include_directories(framexml_emit SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(framexml_emit PRIVATE lua51)

# ── test_settings_schema_consistency ─────────────────────────
# Three readers build controls out of the schema and none can notice a row that
# contradicts itself. A dropdown whose range says three entries while its
# labels name two offers two and writes indices the labels cannot name.
add_executable(test_settings_schema_consistency
    test_settings_schema_consistency.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/settings_schema.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_settings_schema_consistency PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_settings_schema_consistency SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_settings_schema_consistency PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_settings_schema_consistency PRIVATE catch2_main)
add_test(NAME settings_schema_consistency COMMAND test_settings_schema_consistency)
register_test_target(test_settings_schema_consistency)

# ── test_settings_panel_layout ───────────────────────────────
# The options panels are generated from the schema by Lua, and nothing else
# checks the arithmetic between them. A control laid out past the bottom of a
# panel registers, refreshes and answers correctly - it is just not on screen.
add_executable(test_settings_panel_layout
    test_settings_panel_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/settings_schema.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_settings_panel_layout PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_settings_panel_layout SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_settings_panel_layout PRIVATE catch2_main)
add_test(NAME settings_panel_layout COMMAND test_settings_panel_layout)
register_test_target(test_settings_panel_layout)

# ── test_addon_lua_snippets ──────────────────────────────────
# The Lua this client injects into the interface lives as C++ string literals,
# which nothing compiles until the client runs - and a syntax error there is a
# false from executeString and a line in a warning-only log. This asks Lua
# whether they parse.
add_executable(test_addon_lua_snippets
    test_addon_lua_snippets.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_addon_lua_snippets PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_addon_lua_snippets SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_addon_lua_snippets PRIVATE catch2_main lua51)
add_test(NAME addon_lua_snippets COMMAND test_addon_lua_snippets)
register_test_target(test_addon_lua_snippets)

# ── framexml_compiles ───────────────────────────────────────
# Not a unit test: it emits every real FrameXML file and asks Lua itself
# whether the result compiles. The unit tests above check cases someone
# thought of; this checks all 126, and a file that will not compile is lost
# whole rather than degraded. Skipped when the game data is not present.
add_executable(framexml_compile_check
    ${CMAKE_SOURCE_DIR}/tools/framexml_compile_check.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/framexml_emitter.cpp
)
target_include_directories(framexml_compile_check PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(framexml_compile_check SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(framexml_compile_check PRIVATE lua51)
add_test(NAME framexml_compiles COMMAND framexml_compile_check
         ${CMAKE_SOURCE_DIR}/Data/interface/framexml)
set_tests_properties(framexml_compiles PROPERTIES SKIP_RETURN_CODE 77)

# The Blizzard addons go through the same emitter and were never checked by it.
# Their XML is where the load-on-demand panels come from - a file that will not
# compile takes its whole panel with it, and only when someone opens it.
add_test(NAME addon_xml_compiles COMMAND framexml_compile_check
         ${CMAKE_SOURCE_DIR}/Data/interface/addons)
set_tests_properties(addon_xml_compiles PROPERTIES SKIP_RETURN_CODE 77)

# ── Key bindings, run in the real interface ────────────────────────────────
# A key goes down and comes up through the client's own binding dispatch into
# the real FrameXML, and what the interface then did is asked of it. Only with
# the headless runner built (WOWEE_BUILD_FRAMEXML_RUN) and an expansion's
# interface on disk; each test fails the run with the number of checks that
# raised.
set(WOWEE_FRAMEXML_TEST_DATA "${CMAKE_SOURCE_DIR}/Data/expansions/wotlk" CACHE PATH
    "An expansion directory whose interface the framexml_binding tests load")
if(WOWEE_BUILD_FRAMEXML_RUN AND EXISTS "${WOWEE_FRAMEXML_TEST_DATA}/interface/FrameXML")
    function(wowee_framexml_test name)
        set(_dir "${CMAKE_CURRENT_BINARY_DIR}/framexml_run/${name}")
        file(MAKE_DIRECTORY "${_dir}")
        add_test(NAME ${name} COMMAND framexml_run "${WOWEE_FRAMEXML_TEST_DATA}" ${ARGN}
                 WORKING_DIRECTORY "${_dir}")
        # A config corner of its own, so no binding or setting one run saved
        # is in force in the next.
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "WOWEE_CONFIG_ROOT=${_dir}/config")
    endfunction()

    # bindings.xml marks the action buttons runOnUp: the press pushes the
    # button and the release performs it. The release is the key's, and runs
    # the command its press ran even when shift went down in between.
    wowee_framexml_test(framexml_binding_release_runs_the_pressed_command
        "--lua:SetBinding('Q', 'MULTIACTIONBAR1BUTTON1')"
        "--lua:SetBinding('SHIFT-Q', 'MULTIACTIONBAR1BUTTON2')"
        "--bind:Q"
        "--lua:assert(MultiBarBottomLeftButton1:GetButtonState() == 'PUSHED', 'the press did not push the button')"
        "--bindup:SHIFT-Q"
        "--lua:assert(MultiBarBottomLeftButton1:GetButtonState() == 'NORMAL', 'the release did not reach the button the press pushed')")

    # A binding without runOnUp runs once a press, not again on the release.
    wowee_framexml_test(framexml_binding_runs_once_without_run_on_up
        "--lua:opened = 0 ToggleFriendsFrame = function() opened = opened + 1 end"
        "--lua:SetBinding('O', 'TOGGLESOCIAL')"
        "--bind:O"
        "--bindup:O"
        "--lua:assert(opened == 1, 'TOGGLESOCIAL ran ' .. opened .. ' times for one press')")

    # The main bar's keys are the interface's bindings like any other: = is
    # ACTIONBUTTON12, pushed on the press and released on the release.
    wowee_framexml_test(framexml_binding_action_bar_key
        "--bind:="
        "--lua:assert(ActionButton12:GetButtonState() == 'PUSHED', 'the = key did not push action button 12')"
        "--bindup:="
        "--lua:assert(ActionButton12:GetButtonState() == 'NORMAL', 'releasing = did not release action button 12')")

    # A macro on the bar runs its body, from the key and from UseAction.
    wowee_framexml_test(framexml_binding_macro_on_action_button
        "--player"
        "--lua:PickupMacro(CreateMacro('Shout', 1, '/say for the alliance', nil)) PlaceAction(1) ClearCursor()"
        "--ran-macro:"
        "--bind:1"
        "--bindup:1"
        "--ran-macro:/say for the alliance"
        "--lua:UseAction(1)"
        "--ran-macro:/say for the alliance")

    # Movement goes through the bindings too: W walks while held, shift
    # held with it changes nothing, and Q given to the action bar is no
    # longer a strafe.
    wowee_framexml_test(framexml_binding_movement_keys
        "--lua:moves = '' for _, f in ipairs({'MoveForward', 'StrafeLeft'}) do _G[f .. 'Start'] = function() moves = moves .. '+' .. f end _G[f .. 'Stop'] = function() moves = moves .. '-' .. f end end"
        "--bind:SHIFT-W"
        "--bindup:W"
        "--lua:assert(moves == '+MoveForward-MoveForward', 'W held with shift: ' .. moves)"
        "--lua:SetBinding('Q', 'MULTIACTIONBAR1BUTTON1')"
        "--bind:Q"
        "--bindup:Q"
        "--lua:assert(moves == '+MoveForward-MoveForward', 'Q bound to the action bar still strafes: ' .. moves)")

    # And what those bindings hold reaches the client: held while the key is,
    # by command, the arrows beside WASD, everything let go when the window
    # loses the keyboard.
    wowee_framexml_test(framexml_binding_movement_reaches_the_client
        "--bind:W"
        "--moving:Forward"
        "--bind:SHIFT-SPACE"
        "--moving:Forward,JumpOrAscend"
        "--bindup:SPACE"
        "--bindup:W"
        "--moving:"
        "--bind:UP"
        "--bind:LEFT"
        "--moving:Forward,TurnLeft"
        "--release-all"
        "--moving:"
        "--lua:SetBinding('Q', 'MULTIACTIONBAR1BUTTON1')"
        "--bind:Q"
        "--moving:"
        "--bindup:Q")

    # The mouse is bound like a keyboard: the wheel zooms because CAMERAZOOMIN
    # and CAMERAZOOMOUT are bound to it, and an account that gives the wheel
    # and the side buttons to the action bar casts with them and zooms on what
    # it moved zoom to. Walking is TOGGLERUN's, on numpad divide by default.
    wowee_framexml_test(framexml_binding_mouse_and_walk
        "--bind:MOUSEWHEELUP"
        "--bindup:MOUSEWHEELUP"
        "--bind:MOUSEWHEELDOWN"
        "--bindup:MOUSEWHEELDOWN"
        "--bind:MOUSEWHEELDOWN"
        "--bindup:MOUSEWHEELDOWN"
        "--zoomed:-1"
        "--lua:SetBinding('MOUSEWHEELUP', 'ACTIONBUTTON10') SetBinding('PAGEUP', 'CAMERAZOOMIN') SetBinding('BUTTON4', 'ACTIONBUTTON7')"
        "--bind:MOUSEWHEELUP"
        "--lua:assert(ActionButton10:GetButtonState() == 'PUSHED', 'the wheel did not push action button 10')"
        "--bindup:MOUSEWHEELUP"
        "--zoomed:0"
        "--bind:PAGEUP"
        "--bindup:PAGEUP"
        "--zoomed:1"
        "--bind:BUTTON4"
        "--lua:assert(ActionButton7:GetButtonState() == 'PUSHED', 'BUTTON4 did not push action button 7')"
        "--bindup:BUTTON4"
        "--lua:assert(ActionButton7:GetButtonState() == 'NORMAL', 'releasing BUTTON4 did not release action button 7')"
        "--bind:NUMPADDIVIDE"
        "--bindup:NUMPADDIVIDE"
        "--walk-toggles:1")

    # The extra bars are FrameXML's, and its own Action Bars page shows them:
    # the page is offered with all four bars and the lock, and ticking a bar
    # puts it on screen and keeps it as the character's.
    wowee_framexml_test(framexml_action_bars_page
        "--player"
        "--lua:assert(InterfaceOptionsActionBarsPanel and not InterfaceOptionsActionBarsPanel.hidden, 'the Action Bars page is not offered')"
        "--lua:for _, n in ipairs({'BottomLeft', 'BottomRight', 'Right', 'RightTwo', 'LockActionBars', 'AlwaysShowActionBars'}) do assert(_G['InterfaceOptionsActionBarsPanel' .. n]:IsShown(), n .. ' is not on the Action Bars page') end"
        "--lua:assert(not MultiBarBottomRight:IsShown(), 'the bottom right bar was on before it was ticked')"
        "--lua:InterfaceOptionsActionBarsPanelBottomRight:Click()"
        "--lua:assert(MultiBarBottomRight:IsShown(), 'ticking Bottom Right did not show the bar')"
        "--lua:assert(select(2, GetActionBarToggles()), 'the bottom right bar is not kept as shown')")
endif()

# ── test_taxi_map_projection ────────────────────────────────
# The order of the coordinate conversions behind a flight map marker. Silent
# when wrong - every marker still lands somewhere, just not where it belongs.
wowee_add_test(test_taxi_map_projection
    SOURCES test_taxi_map_projection.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/coordinate_projection.cpp)

# ── test_inventory_slots ─────────────────────────────────────────
# Where the bank, its bags and the keyring sit. A wrong slot here moves an
# item somewhere real that nobody asked for, so the numbers are pinned.
add_executable(test_inventory_slots test_inventory_slots.cpp)
target_include_directories(test_inventory_slots PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_inventory_slots SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_inventory_slots PRIVATE catch2_main)
add_test(NAME inventory_slots COMMAND test_inventory_slots)
register_test_target(test_inventory_slots)

# ── test_jit_write ───────────────────────────────────────────────
# Writing to a MAP_JIT mapping, which Apple Silicon refuses until the thread
# asks. The Warden module image is mmap'd that way and then zeroed, and the
# zeroing raised SIGBUS - a crash with no bad pointer in it, found by fuzzing
# the loader with bytes a server could send.
add_executable(test_jit_write test_jit_write.cpp)
target_include_directories(test_jit_write PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_jit_write SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_jit_write PRIVATE catch2_main)
add_test(NAME jit_write COMMAND test_jit_write)
register_test_target(test_jit_write)

# ── test_warden_reloc ────────────────────────────────────────────
# Where a Warden relocation entry is allowed to point. The module comes from
# the server, so both of these decide where an attacker-supplied number may be
# written; one of them let a four-byte write land at image + 0xFFFFFFFF.
add_executable(test_warden_reloc test_warden_reloc.cpp)
target_include_directories(test_warden_reloc PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_warden_reloc SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_warden_reloc PRIVATE catch2_main)
add_test(NAME warden_reloc COMMAND test_warden_reloc)
register_test_target(test_warden_reloc)

# ── sweep_guard ──────────────────────────────────────────────────
# The fast interface sweeps, run as a test so they stop depending on someone
# remembering. Each fault they catch raises nothing, logs nothing and fails no
# other test: a panel drawn twice, a binding answering short, a manifest entry
# resolving to no file, bootstrap Lua replacing the binding beneath it. What
# they cost is measured in tools/sweep_guard.py's docstring rather than here;
# the slower reports stay manual.
if(Python3_Interpreter_FOUND)
    add_test(NAME sweep_guard
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sweep_guard.py)
endif()

# ── unicorn_stub_compiles ────────────────────────────────────────
# Unicorn is optional and every build here has it, so the stub branch the
# Warden emulator falls back to compiles in no configuration anyone runs. It
# rotted that way: issue #115 was a stub defining a method the header does
# not declare, found by someone building without Unicorn and invisible to us.
# Parses the two translation units with the define removed. A second or two.
if(Python3_Interpreter_FOUND)
    add_test(NAME unicorn_stub_compiles
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tools/unicorn_stub_check.py
                     ${CMAKE_BINARY_DIR})
endif()

# ── upscale_mip_colour ──────────────────────────────────────────
# Pillow's resize weights colour by alpha, so an RGBA resize returns black
# wherever a sheet is transparent. That silently emptied every mip level below
# the base of 727 upscaled sidecars; it showed up only once an opaque M2 batch
# stopped keying its alpha away, as black holes through Silverpine's trunks at
# any distance. One line wide and invisible until someone walks a chain by
# hand. Instant.
if(Python3_Interpreter_FOUND)
    add_test(NAME upscale_mip_colour
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tools/upscale_mip_check.py)
endif()

# ── android_data_profiles ───────────────────────────────────────
# The profiles in tools/android decide what an 18 GB extraction is cut down to
# for a phone. A pattern that stops matching drops files silently and only says
# so on a device. It has happened once already: * crossed a separator and a
# trailing /** was compared literally, so one exclusion matched nothing at all.
# Needs no extraction, so it runs here rather than by hand. Instant.
if(Python3_Interpreter_FOUND)
    add_test(NAME android_data_profiles
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tools/android/test_data_profiles.py)
endif()

# ── test_chat_types ─────────────────────────────────────────────
# A chat type is a byte off the wire and the event name is built from it, so a
# value missing from the enum is a line of chat that never appears - no error,
# no log. The whole run between LOOT and the battleground block was absent once.
wowee_add_test(test_chat_types
    SOURCES test_chat_types.cpp)

# ── test_chat_window_background ─────────────────────────────────
# The chat panel is a white image the interface tints, so the colour this client
# saves is the whole of what it looks like - and it saved white at full alpha,
# which covers the chat with the text lost in it. Fixing the default left every
# install that had already written that white unfixed, and it came back as a
# report after the fix had shipped.
wowee_add_test(test_chat_window_background
    SOURCES test_chat_window_background.cpp)

# ── test_text_tokens ────────────────────────────────────────
# The server writes the player as a blank - "$N, you have done well" - and the
# client fills it in. Nothing did on the chat path, so every scripted NPC in the
# game addressed the player as "$N". Pure: the substitution takes the four facts
# it needs about the reader rather than a client.
add_executable(test_text_tokens test_text_tokens.cpp ${CMAKE_SOURCE_DIR}/src/game/text_tokens.cpp)
target_include_directories(test_text_tokens PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_text_tokens SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_text_tokens PRIVATE catch2_main)
add_test(NAME text_tokens COMMAND test_text_tokens)
register_test_target(test_text_tokens)

# ── test_reputation_standing ────────────────────────────────
# Header-only and constexpr, so this needs nothing linked: the point is that the
# thresholds have one home now and stay put.
add_executable(test_reputation_standing test_reputation_standing.cpp)
target_include_directories(test_reputation_standing PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_reputation_standing SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_reputation_standing PRIVATE catch2_main)
add_test(NAME reputation_standing COMMAND test_reputation_standing)
register_test_target(test_reputation_standing)

# ── test_widget_tree ────────────────────────────────────────
add_executable(test_widget_tree
    test_widget_tree.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/widget_tree.cpp
)
target_include_directories(test_widget_tree PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_widget_tree SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_widget_tree PRIVATE catch2_main)
add_test(NAME widget_tree COMMAND test_widget_tree)
register_test_target(test_widget_tree)

# ── test_text_edit ──────────────────────────────────────────
wowee_add_test(test_text_edit SOURCES
    test_text_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/text_edit.cpp
)

# ── test_escape_action ──────────────────────────────────────
add_executable(test_escape_action
    test_escape_action.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/escape_action.cpp
)
target_include_directories(test_escape_action PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_escape_action SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_escape_action PRIVATE catch2_main)
add_test(NAME escape_action COMMAND test_escape_action)
register_test_target(test_escape_action)

# ── test_placement_rotation ─────────────────────────────────
wowee_add_test(test_placement_rotation
    SOURCES test_placement_rotation.cpp)

# ── test_water_mask ─────────────────────────────────────────
add_executable(test_water_mask
    test_water_mask.cpp
)
target_include_directories(test_water_mask PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_water_mask SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_water_mask PRIVATE catch2_main)
add_test(NAME water_mask COMMAND test_water_mask)
register_test_target(test_water_mask)

# ── test_addon_globals ──────────────────────────────────────
add_executable(test_addon_globals
    test_addon_globals.cpp
)
target_include_directories(test_addon_globals PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_addon_globals SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_addon_globals PRIVATE catch2_main)
add_test(NAME addon_globals COMMAND test_addon_globals)
register_test_target(test_addon_globals)

# ── test_monster_move_facing ─────────────────────────────────
# SMSG_MONSTER_MOVE's move-type byte and the facing after it, which three
# expansion parsers used to read for themselves. A wrong length here misreads
# every field that follows.
wowee_add_test(test_monster_move_facing
    SOURCES test_monster_move_facing.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
            ${CMAKE_SOURCE_DIR}/src/game/spline_packet.cpp)

# Real receive parser: extended headers, fragmented RC4 streams and legacy framing.
wowee_add_test(test_world_socket
    SOURCES test_world_socket.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/network/world_socket.cpp
            ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
            ${CMAKE_SOURCE_DIR}/src/auth/rc4.cpp
            ${CMAKE_SOURCE_DIR}/src/auth/vanilla_crypt.cpp
            ${CMAKE_SOURCE_DIR}/src/auth/crypto.cpp
            ${CMAKE_SOURCE_DIR}/src/game/opcode_table.cpp)
target_link_libraries(test_world_socket PRIVATE OpenSSL::Crypto Threads::Threads)

# ── test_packet ──────────────────────────────────────────────
add_executable(test_packet
    test_packet.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)
target_include_directories(test_packet PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_packet SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_packet PRIVATE catch2_main)
add_test(NAME packet COMMAND test_packet)
register_test_target(test_packet)

# ── test_bit_packet ──────────────────────────────────────────
add_executable(test_bit_packet
    test_bit_packet.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)
target_include_directories(test_bit_packet PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_bit_packet SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_bit_packet PRIVATE catch2_main)
add_test(NAME bit_packet COMMAND test_bit_packet)
register_test_target(test_bit_packet)

# Standard Warden challenge and RC4 transition (no downloaded module needed).
add_executable(test_warden_challenge
    test_warden_challenge.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/warden_crypto.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/crypto.cpp
)
target_include_directories(test_warden_challenge PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_warden_challenge SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_warden_challenge PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME warden_challenge COMMAND test_warden_challenge)
register_test_target(test_warden_challenge)

# Warden PE lookup must use the expansion selected by the asset manager.
add_executable(test_warden_memory
    test_warden_memory.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/warden_memory.cpp
)
target_include_directories(test_warden_memory PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_warden_memory SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_warden_memory PRIVATE catch2_main OpenSSL::Crypto)
add_test(NAME warden_memory COMMAND test_warden_memory)
register_test_target(test_warden_memory)

# ── test_srp ─────────────────────────────────────────────────
add_executable(test_srp
    test_srp.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/auth/srp.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/big_num.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/crypto.cpp
)
target_include_directories(test_srp PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_srp SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_srp PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME srp COMMAND test_srp)
register_test_target(test_srp)

# ── test_m2_ribbon_tracks ────────────────────────────────────
# A ribbon emitter's alpha and visibility tracks, whose keys are fixed16 and
# bytes rather than floats. Real-asset coverage; skips when unextracted.
add_executable(test_m2_ribbon_tracks
    test_m2_ribbon_tracks.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
)
target_include_directories(test_m2_ribbon_tracks PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_ribbon_tracks SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_m2_ribbon_tracks PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_m2_ribbon_tracks PRIVATE catch2_main)
wowee_test_link_glm(test_m2_ribbon_tracks)
add_test(NAME m2_ribbon_tracks COMMAND test_m2_ribbon_tracks)
register_test_target(test_m2_ribbon_tracks)

# ── test_m2_texture_transform ────────────────────────────────
# A texture animation's rotation keys as the loader reads them, and the UV
# transform the doodad shader is handed.
add_executable(test_m2_texture_transform
    test_m2_texture_transform.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
)
target_include_directories(test_m2_texture_transform PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_texture_transform SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_texture_transform PRIVATE catch2_main)
wowee_test_link_glm(test_m2_texture_transform)
add_test(NAME m2_texture_transform COMMAND test_m2_texture_transform)
register_test_target(test_m2_texture_transform)

# ── test_shapeshift_forms ────────────────────────────────────
# The stance bar's three questions, which used to be three different lists.
wowee_add_test(test_shapeshift_forms
    SOURCES test_shapeshift_forms.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/shapeshift_forms.cpp)

# ── test_tile_size_agreement ─────────────────────────────────
# The ADT tile size, which three headers each declared and two files spelled
# with two fewer decimals. Pins the value against its own arithmetic.
wowee_add_test(test_tile_size_agreement
    SOURCES test_tile_size_agreement.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/terrain_mesh.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_terrain_lod ─────────────────────────────────────────
# The far levels of a terrain chunk: the same ground with fewer triangles,
# holes kept open, and skirts that close the seam between two levels.
wowee_add_test(test_terrain_lod
    SOURCES test_terrain_lod.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/terrain_mesh.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_chunk_surface ───────────────────────────────────────
# Where a point inside a terrain chunk sits in the world. The clutter and
# doodad scatterers each had their own copy, including the crossed axes.
wowee_add_test(test_chunk_surface
    SOURCES test_chunk_surface.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/terrain_mesh.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_spline_body ─────────────────────────────────────────
# The body of SMSG_MONSTER_MOVE, whose head two parsers shared and neither
# covered. Checked against AzerothCore's own WriteCommonMonsterMovePart.
wowee_add_test(test_spline_body
    SOURCES test_spline_body.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/spline_packet.cpp
            ${CMAKE_SOURCE_DIR}/src/network/packet.cpp)

# ── test_spline ──────────────────────────────────────────────
wowee_add_test(test_spline
    SOURCES test_spline.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
            ${CMAKE_SOURCE_DIR}/src/game/spline_packet.cpp
            ${CMAKE_SOURCE_DIR}/src/network/packet.cpp)

# ── test_transport_path_repo ─────────────────────────────────
wowee_add_test(test_transport_path_repo
    SOURCES test_transport_path_repo.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/transport_path_repository.cpp
            ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp)

# ── test_opcode_table ────────────────────────────────────────
add_executable(test_opcode_table
    test_opcode_table.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/opcode_table.cpp
)
target_include_directories(test_opcode_table PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_opcode_table SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_opcode_table PRIVATE catch2_main)
add_test(NAME opcode_table COMMAND test_opcode_table)
register_test_target(test_opcode_table)

# ── test_equipment_set_packets ───────────────────────────────
# Every guid in the equipment-set family is packed and all twenty were read and
# written as fixed eight-byte values - plus an ignore mask that the server does
# not send at all. Saving a set could never have worked.
add_executable(test_equipment_set_packets
    test_equipment_set_packets.cpp
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)
target_include_directories(test_equipment_set_packets PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_equipment_set_packets SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_equipment_set_packets PRIVATE catch2_main)
add_test(NAME equipment_set_packets COMMAND test_equipment_set_packets)
register_test_target(test_equipment_set_packets)

# ── test_auction_sort_packet ─────────────────────────────────
# The sort block on CMSG_AUCTION_LIST_ITEMS was a hardcoded zero, and the
# browse tab re-queries rather than sorting locally, so its column headers did
# nothing at all. Two orderings have to agree and they run opposite ways.
wowee_add_test(test_auction_sort_packet
    SOURCES test_auction_sort_packet.cpp
            ${CMAKE_SOURCE_DIR}/src/network/packet.cpp)

# ── test_achievement_criteria ────────────────────────────────
# The criteria progress record. Its counter is a packed guid and was read as a
# fixed uint64, which put every field behind it out of place - and in the login
# message, where records repeat until a sentinel, took the whole list with it.
add_executable(test_achievement_criteria
    test_achievement_criteria.cpp
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)
target_include_directories(test_achievement_criteria PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_achievement_criteria SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_achievement_criteria PRIVATE catch2_main)
add_test(NAME achievement_criteria COMMAND test_achievement_criteria)
register_test_target(test_achievement_criteria)

# ── test_taxi_packets ────────────────────────────────────────
# The multi-hop flight request. A field the server does not read was being
# written between the guid and the node count, so the count was read as a node
# and every route longer than one hop was refused.
wowee_add_packet_test(test_taxi_packets)

# ── test_use_item_packet ─────────────────────────────────────
wowee_add_packet_test(test_use_item_packet)

# ── test_normal_map ────────────────────────────────────────────────────
# The normal-map pass the character and WMO renderers used to duplicate. Both
# copies lived inside a renderer class and needed a Vulkan context to reach, so
# this is its first test.
wowee_add_test(test_normal_map
    SOURCES test_normal_map.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/rendering/normal_map.cpp)

# ── test_spatial_grid ────────────────────────────────────────
# The uniform grid the M2 and WMO renderers narrow collision queries with. Both
# had their own copy, private to a renderer and unreachable without a Vulkan
# context, so this is its first test.
wowee_add_test(test_spatial_grid
    SOURCES test_spatial_grid.cpp
            ${TEST_COMMON_SOURCES})

# ── test_cata_movement ─────────────────────────────────────────────────
# The 109 Cataclysm movement layouts derived from a 4.3.4 core, and the reader
# that executes them. Runs from the source dir for the derived JSON.
add_executable(test_cata_movement
    test_cata_movement.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/cata_movement.cpp
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp)
target_include_directories(test_cata_movement PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_cata_movement SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_cata_movement PRIVATE catch2_main)
wowee_test_link_glm(test_cata_movement)
add_test(NAME cata_movement COMMAND test_cata_movement
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
register_test_target(test_cata_movement)

# ── test_base_fallback ─────────────────────────────────────────────────
# Whether one client's asset tree may answer for another's missing files.
add_executable(test_base_fallback
    test_base_fallback.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_base_fallback PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_base_fallback SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_base_fallback PRIVATE catch2_main)
add_test(NAME base_fallback COMMAND test_base_fallback)
register_test_target(test_base_fallback)

# ── test_override_add ──────────────────────────────────────────────────
# A pack may add files, not only replace them. An addition has no manifest
# entry to resolve against, and resolution used to stop there.
add_executable(test_override_add
    test_override_add.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/asset_manifest.cpp)
target_include_directories(test_override_add PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_override_add SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_override_add PRIVATE catch2_main)
wowee_test_link_glm(test_override_add)
add_test(NAME override_add COMMAND test_override_add)
register_test_target(test_override_add)

# ── test_asset_manifest_load ───────────────────────────────────────────
# The 32 MB asset manifest, pinned so a change to how it is read cannot change
# what it read. Skips when Data/manifest.json is not in the checkout.
add_executable(test_asset_manifest_load
    test_asset_manifest_load.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/asset_manifest.cpp)
target_include_directories(test_asset_manifest_load PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_asset_manifest_load SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_asset_manifest_load PRIVATE catch2_main)
wowee_test_link_glm(test_asset_manifest_load)
add_test(NAME asset_manifest_load COMMAND test_asset_manifest_load
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
register_test_target(test_asset_manifest_load)

# ── test_macro_text ──────────────────────────────────────────
# Reading a macro body. The action bar and the chat panel each had their own
# copy of the line walk, neither with a test.
wowee_add_test(test_macro_text
    SOURCES test_macro_text.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/ui/macro_text.cpp)

# ── test_water_surface_grid ──────────────────────────────────
# Projecting a world position onto a water surface's grid, which
# water_renderer.cpp did in four places and never tested.
wowee_add_test(test_water_surface_grid
    SOURCES test_water_surface_grid.cpp
            ${TEST_COMMON_SOURCES})

# ── test_dxt_block ───────────────────────────────────────────
# The colour half of a DXT block, decoded three times in blp_loader.cpp and
# never tested; the BLP tests stop at header validation.
wowee_add_test(test_dxt_block
    SOURCES test_dxt_block.cpp
            ${TEST_COMMON_SOURCES})

# ── test_vertex_layouts ──────────────────────────────────────
# The WMO and terrain vertex layouts against the shaders that read them. Each
# layout was stated three or four times, once with the stride and every offset
# as a literal; the shader source is the outside oracle.
add_executable(test_vertex_layouts
    test_vertex_layouts.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_vertex_layouts PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_vertex_layouts SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_vertex_layouts PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_vertex_layouts PRIVATE catch2_main)
wowee_test_link_glm(test_vertex_layouts)
add_test(NAME vertex_layouts COMMAND test_vertex_layouts)
register_test_target(test_vertex_layouts)

# ── test_movement_flags ──────────────────────────────────────
# What the movement flag word means. Two handlers spelled these out against
# their own copy of the flags; they now both ask MovementInfo. Header-only, so
# no sources beyond the common set.
wowee_add_test(test_movement_flags
    SOURCES test_movement_flags.cpp
            ${TEST_COMMON_SOURCES})

# ── test_attacker_state_layout ─────────────────────────────────────────
# SMSG_ATTACKERSTATEUPDATE for classic and TBC, pinned before the shared head
# was factored out. Same link set as the other two layout tests.
wowee_add_packet_test(test_attacker_state_layout)

# ── test_spell_damage_log_layout ───────────────────────────────────────
# SMSG_SPELLNONMELEEDAMAGELOG before WotLK, pinned before the Classic and TBC
# readers were merged. The expansions differ by one field - overkill - and
# reading it where it is absent shifts everything after it by four bytes.
wowee_add_packet_test(test_spell_damage_log_layout)

# ── test_prewotlk_build_packets ────────────────────────────────────────
# The three client packets Classic and TBC build identically, pinned before
# the builders were merged. Each is a WotLK packet minus a leading packed GUID
# or a trailing uint32, and sending the extra bytes fails silently.
wowee_add_packet_test(test_prewotlk_build_packets)

# ── test_movement_payload_layout ───────────────────────────────────────
# The MSG_MOVE_* payload read back as each expansion writes it: pitch, fall
# time and jump data included, flags mapped to MovementFlags.
wowee_add_packet_test(test_movement_payload_layout)

# ── test_item_query_layout ─────────────────────────────────────────────
# SMSG_ITEM_QUERY_SINGLE_RESPONSE for classic and TBC, pinned before the two
# readers were merged. Same link set as test_char_enum_layout.
wowee_add_packet_test(test_item_query_layout)

# ── test_guild_bank_list ───────────────────────────────────────────────
# SMSG_GUILD_BANK_LIST, whose tab-name block is optional and was gated on the
# wrong condition - so a tab query that answered with the names attached had
# every field after them read a block early, and the vault drew empty.
wowee_add_packet_test(test_guild_bank_list)

# ── test_guild_event_money ─────────────────────────────────────────────
# SMSG_GUILD_EVENT past type 13: the guild bank's events, and the balance they
# carry as sixteen hex digits. They used to reach guild chat verbatim.
wowee_add_packet_test(test_guild_event_money)

# ── test_gameobject_query_layout ───────────────────────────────────────
# SMSG_GAMEOBJECT_QUERY_RESPONSE, parsed three times, differing only in how
# many strings sit before the twenty-four data fields.
wowee_add_packet_test(test_gameobject_query_layout)

# ── test_guild_roster_layout ───────────────────────────────────────────
# SMSG_GUILD_ROSTER, parsed by three separate functions with no coverage at
# all. Same link set as test_char_enum_layout, and for the same reason.
wowee_add_packet_test(test_guild_roster_layout)

# ── test_char_enum_layout ──────────────────────────────────────────────
# SMSG_CHAR_ENUM, parsed by three separate functions with no coverage at all.
# Links the same set test_taxi_packets does: the classic and TBC parsers pull
# in the rest of the packet layer through their shared base.
wowee_add_packet_test(test_char_enum_layout)

# ── test_loot_response_packet ─────────────────────────────────────────
# The quest-item section is WotLK's, and whether the parser looks for it comes
# from this client's configured expansion rather than from the realm. A
# mismatch used to lose the whole response - regular items included - so a
# chest opened and no window came up.
wowee_add_packet_test(test_loot_response_packet)

# ── test_battlefield_status_packet ─────────────────────────────────────
# SMSG_BATTLEFIELD_STATUS, whose header was read two bytes short.
wowee_add_packet_test(test_battlefield_status_packet)

# ── test_lfg_join_packet ─────────────────────────────────────
# Every field of CMSG_LFG_JOIN, which was wrong in every field and silent.
wowee_add_packet_test(test_lfg_join_packet)

# ── test_learn_talent_packet ─────────────────────────────────
# The rank field counts from zero on the wire and from one everywhere else.
wowee_add_packet_test(test_learn_talent_packet)

# ── test_item_query_sockets ─────────────────────────────────
# The socket block of the item query, which the layout sweep cannot reach.
wowee_add_packet_test(test_item_query_sockets)

# ── test_item_stat_names ────────────────────────────────────
# Which of the two stat-name tables answers for the five primaries. A random
# suffix rolls raw ITEM_MOD types and was reading the table that names only the
# extras, so every animal suffix listed no stats at all.
wowee_add_test(test_item_stat_names SOURCES test_item_stat_names.cpp)

# ── test_mail_packets ───────────────────────────────────────
wowee_add_packet_test(test_mail_packets)

# ── test_raid_target_packets ────────────────────────────────
wowee_add_packet_test(test_raid_target_packets)

# ── test_entity ──────────────────────────────────────────────
wowee_add_test(test_entity
    SOURCES test_entity.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/entity.cpp
            ${CMAKE_SOURCE_DIR}/src/game/reported_movement.cpp
            ${CMAKE_SOURCE_DIR}/src/math/spline.cpp)

# ── test_dbc_loader ──────────────────────────────────────────
add_executable(test_dbc_loader
    test_dbc_loader.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_layout.cpp
)
target_include_directories(test_dbc_loader PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_dbc_loader SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_dbc_loader PRIVATE catch2_main)
add_test(NAME dbc_loader COMMAND test_dbc_loader)
register_test_target(test_dbc_loader)

# ── test_m2_structs ──────────────────────────────────────────
wowee_add_test(test_m2_structs
    SOURCES test_m2_structs.cpp
            ../src/rendering/m2_model_classifier.cpp)

# ── test_m2_color_alpha ──────────────────────────────────────
# Loads the peasant lumberjack carry models (WotLK v264 + vanilla v256) and
# pins the M2 color-alpha track parsing that hides the duplicate wood bundle.
add_executable(test_m2_color_alpha
    test_m2_color_alpha.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_m2_color_alpha PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_color_alpha SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_m2_color_alpha PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_m2_color_alpha PRIVATE catch2_main)
wowee_test_link_glm(test_m2_color_alpha)
add_test(NAME m2_color_alpha COMMAND test_m2_color_alpha)
register_test_target(test_m2_color_alpha)

# ── test_m2_footstep_events ──────────────────────────────────
# Loads real WotLK v264 + vanilla v256 models and pins the $FSD footfall
# event parsing that syncs footstep audio to foot-strike keyframes.
add_executable(test_m2_footstep_events
    test_m2_footstep_events.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_m2_footstep_events PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_footstep_events SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_m2_footstep_events PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_m2_footstep_events PRIVATE catch2_main)
wowee_test_link_glm(test_m2_footstep_events)
add_test(NAME m2_footstep_events COMMAND test_m2_footstep_events)
register_test_target(test_m2_footstep_events)

# ── test_blp_loader ──────────────────────────────────────────
add_executable(test_blp_loader
    test_blp_loader.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
)
target_include_directories(test_blp_loader PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_blp_loader SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_blp_loader PRIVATE catch2_main)
add_test(NAME blp_loader COMMAND test_blp_loader)
register_test_target(test_blp_loader)

# ── test_dds_sidecar ─────────────────────────────────────────
# The block-compressed override the upscale pass writes. A DDS header read
# wrongly is not an error - it is a texture at the wrong size, or a mip chain
# that runs off the end of the file.
add_executable(test_dds_sidecar
    test_dds_sidecar.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/dds_loader.cpp
)
target_include_directories(test_dds_sidecar PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_dds_sidecar SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_dds_sidecar PRIVATE catch2_main)
add_test(NAME dds_sidecar COMMAND test_dds_sidecar)
register_test_target(test_dds_sidecar)

# ── test_dds_override ────────────────────────────────────────
# The sidecar as AssetManager::loadTexture hands it over: RGBA8 to a caller
# that did not ask for blocks. The character renderer did not, was handed
# them anyway, and drew every upscaled glow on a weapon as a white card.
wowee_add_test(test_dds_override
    SOURCES test_dds_override.cpp)
target_link_libraries(test_dds_override PRIVATE wowee_pipeline wowee_base)

# ── test_asset_file_cache ────────────────────────────────────
# A texture read to be uploaded is not kept a second time in the file cache;
# a model file read for its own sake is.
wowee_add_test(test_asset_file_cache
    SOURCES test_asset_file_cache.cpp)
target_link_libraries(test_asset_file_cache PRIVATE wowee_pipeline wowee_base)

# ── test_blp_alpha_scan ──────────────────────────────────────
# M2 textures were decoded to RGBA8 so something could walk every fourth byte
# and learn whether the texture had any transparency. The block scan that
# replaced it has to answer the same, on real files rather than on the cases
# someone thought to write down.
add_executable(test_blp_alpha_scan
    test_blp_alpha_scan.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
)
target_include_directories(test_blp_alpha_scan PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_blp_alpha_scan SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_blp_alpha_scan PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_blp_alpha_scan PRIVATE catch2_main)
add_test(NAME blp_alpha_scan COMMAND test_blp_alpha_scan)
register_test_target(test_blp_alpha_scan)

# ── test_blp_alpha_silhouette ────────────────────────────────
# A batch marked opaque does not read its texture's alpha, and M2 atlases carry
# alpha belonging to other layers. Keying on it put a hole in the middle of
# every Silverpine trunk, so the scan that tells a real silhouette from a
# leftover has to hold on both populations.
add_executable(test_blp_alpha_silhouette
    test_blp_alpha_silhouette.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
)
target_include_directories(test_blp_alpha_silhouette PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_blp_alpha_silhouette SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_blp_alpha_silhouette PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_blp_alpha_silhouette PRIVATE catch2_main)
add_test(NAME blp_alpha_silhouette COMMAND test_blp_alpha_silhouette)
register_test_target(test_blp_alpha_silhouette)

# ── test_frustum ─────────────────────────────────────────────
wowee_add_test(test_frustum
    SOURCES test_frustum.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/frustum.cpp)

# ── test_rt_bvh ──────────────────────────────────────────────
# The hierarchy the software ray tracer walks, against a brute-force scan.
wowee_add_test(test_rt_bvh
    SOURCES test_rt_bvh.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/rt_bvh.cpp)

# ── test_recording_size ──────────────────────────────────────
# Header-only: the size a screen recording is encoded at.
wowee_add_test(test_recording_size
    SOURCES test_recording_size.cpp)

# ── test_volumetric_fog_math ─────────────────────────────────
# Header-only: where the fog volume's cells are, against where the shaders
# look for them.
wowee_add_test(test_volumetric_fog_math
    SOURCES test_volumetric_fog_math.cpp)

# ── test_packed_time ─────────────────────────────────────────
# Header-only domain logic: the one uint32 that carries a whole date, which
# four separate readers in this tree got wrong in four different ways.
add_executable(test_packed_time
    test_packed_time.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_packed_time PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_packed_time SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_packed_time PRIVATE catch2_main)
add_test(NAME packed_time COMMAND test_packed_time)
register_test_target(test_packed_time)

# ── test_game_clock ──────────────────────────────────────────
# Header-only: the realm's time of day, run on from the hour the server sent.
add_executable(test_game_clock
    test_game_clock.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_game_clock PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_game_clock SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_game_clock PRIVATE catch2_main)
add_test(NAME game_clock COMMAND test_game_clock)
register_test_target(test_game_clock)

# ── test_animation_ids ───────────────────────────────────────
add_executable(test_animation_ids
    test_animation_ids.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/animation_ids.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
)
target_include_directories(test_animation_ids PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_animation_ids SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_animation_ids PRIVATE catch2_main)
add_test(NAME animation_ids COMMAND test_animation_ids)
register_test_target(test_animation_ids)

# ── test_locomotion_fsm ──────────────────────────────────────
add_executable(test_locomotion_fsm
    test_locomotion_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/locomotion_fsm.cpp
)
target_include_directories(test_locomotion_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_locomotion_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_locomotion_fsm PRIVATE catch2_main)
add_test(NAME locomotion_fsm COMMAND test_locomotion_fsm)
register_test_target(test_locomotion_fsm)

# ── test_chat_filters ────────────────────────────────────────
add_executable(test_chat_filters
    test_chat_filters.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_chat_filters PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_chat_filters SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_chat_filters PRIVATE catch2_main)
add_test(NAME chat_filters COMMAND test_chat_filters)
register_test_target(test_chat_filters)

# ── test_nameplate_stacking ──────────────────────────────────
add_executable(test_nameplate_stacking
    test_nameplate_stacking.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_nameplate_stacking PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_nameplate_stacking SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_nameplate_stacking PRIVATE catch2_main)
add_test(NAME nameplate_stacking COMMAND test_nameplate_stacking)
register_test_target(test_nameplate_stacking)

# ── test_combat_text_filter ──────────────────────────────────
add_executable(test_combat_text_filter
    test_combat_text_filter.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_combat_text_filter PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_combat_text_filter SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_combat_text_filter PRIVATE catch2_main)
add_test(NAME combat_text_filter COMMAND test_combat_text_filter)
register_test_target(test_combat_text_filter)

# ── test_combat_fsm ──────────────────────────────────────────
add_executable(test_combat_fsm
    test_combat_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/combat_fsm.cpp
)
target_include_directories(test_combat_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_combat_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_combat_fsm PRIVATE catch2_main)
add_test(NAME combat_fsm COMMAND test_combat_fsm)
register_test_target(test_combat_fsm)

# ── test_activity_fsm ────────────────────────────────────────
add_executable(test_activity_fsm
    test_activity_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/activity_fsm.cpp
)
target_include_directories(test_activity_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_activity_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_activity_fsm PRIVATE catch2_main)
add_test(NAME activity_fsm COMMAND test_activity_fsm)
register_test_target(test_activity_fsm)

# NPC animator tests removed - NpcAnimator replaced by generic CharacterAnimator

# ── test_anim_capability ─────────────────────────────────────
# Header-only struct tests - no source files needed
add_executable(test_anim_capability
    test_anim_capability.cpp
)
target_include_directories(test_anim_capability PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_anim_capability SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_anim_capability PRIVATE catch2_main)
add_test(NAME anim_capability COMMAND test_anim_capability)
register_test_target(test_anim_capability)

# ── test_indoor_shadows ──────────────────────────────────────
wowee_add_test(test_indoor_shadows
    SOURCES test_indoor_shadows.cpp)

# ── test_zone_tiles ────────────────────────────────────────────
wowee_add_test(test_zone_tiles
    SOURCES test_zone_tiles.cpp)

# ── test_transport_components ────────────────────────────────
wowee_add_test(test_transport_components
    SOURCES test_transport_components.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/transport_clock_sync.cpp
            ${CMAKE_SOURCE_DIR}/src/game/transport_animator.cpp
            ${CMAKE_SOURCE_DIR}/src/math/spline.cpp)

# ── test_world_map ────────────────────────────────────────────
wowee_add_test(test_world_map
    SOURCES test_world_map.cpp)

# ── test_world_map_coordinate_projection ──────────────────────
wowee_add_test(test_world_map_coordinate_projection
    SOURCES test_world_map_coordinate_projection.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/coordinate_projection.cpp)

# ── test_world_map_map_resolver ───────────────────────────────
wowee_add_test(test_world_map_map_resolver
    SOURCES test_world_map_map_resolver.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/map_resolver.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/coordinate_projection.cpp
            ${TEST_COMMON_SOURCES})

# ── test_world_map_view_state_machine ─────────────────────────
wowee_add_test(test_world_map_view_state_machine
    SOURCES test_world_map_view_state_machine.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/view_state_machine.cpp)

# ── test_world_map_exploration_state ──────────────────────────
wowee_add_test(test_world_map_exploration_state
    SOURCES test_world_map_exploration_state.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/exploration_state.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/coordinate_projection.cpp)

# ── test_world_map_zone_metadata ──────────────────────────────
wowee_add_test(test_world_map_zone_metadata
    SOURCES test_world_map_zone_metadata.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/world_map/zone_metadata.cpp)

# ── test_text_wrap ───────────────────────────────────────────
add_executable(test_text_wrap
    test_text_wrap.cpp
)
target_include_directories(test_text_wrap PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_text_wrap SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_text_wrap PRIVATE catch2_main)
add_test(NAME text_wrap COMMAND test_text_wrap)
register_test_target(test_text_wrap)

# ── test_text_markup ─────────────────────────────────────────
add_executable(test_text_markup
    test_text_markup.cpp
)
target_include_directories(test_text_markup PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_text_markup SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_text_markup PRIVATE catch2_main)
add_test(NAME text_markup COMMAND test_text_markup)
register_test_target(test_text_markup)

# ── test_simple_html ─────────────────────────────────────────
add_executable(test_simple_html
    test_simple_html.cpp
)
target_include_directories(test_simple_html PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_simple_html SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_simple_html PRIVATE catch2_main)
add_test(NAME simple_html COMMAND test_simple_html)
register_test_target(test_simple_html)

# ── test_link_hit ────────────────────────────────────────────
add_executable(test_link_hit
    test_link_hit.cpp
)
target_include_directories(test_link_hit PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_link_hit SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_link_hit PRIVATE catch2_main)
add_test(NAME link_hit COMMAND test_link_hit)
register_test_target(test_link_hit)

# ── test_minimap_projection ──────────────────────────────────
add_executable(test_minimap_projection
    test_minimap_projection.cpp
)
target_include_directories(test_minimap_projection PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_minimap_projection SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_minimap_projection PRIVATE catch2_main)
wowee_test_link_glm(test_minimap_projection)
add_test(NAME minimap_projection COMMAND test_minimap_projection)
register_test_target(test_minimap_projection)
register_test_target(test_text_wrap)

# ── test_chat_markup_parser ──────────────────────────────────
add_executable(test_chat_markup_parser
    test_chat_markup_parser.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/ui/chat/chat_markup_parser.cpp
)
target_include_directories(test_chat_markup_parser PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_chat_markup_parser SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/imgui
)
target_link_libraries(test_chat_markup_parser PRIVATE catch2_main)
add_test(NAME chat_markup_parser COMMAND test_chat_markup_parser)
register_test_target(test_chat_markup_parser)

# ── test_font_em_scale ───────────────────────────────────────
# The head/hhea read that makes a font height mean an em, as FrameXML means it,
# rather than the ascender-to-descender span ImGui fits into it. Absolute byte
# offsets into a file this client did not write, so they are pinned.
add_executable(test_font_em_scale
    test_font_em_scale.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/interface_fonts.cpp
)
target_include_directories(test_font_em_scale PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_font_em_scale SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/imgui
)
target_link_libraries(test_font_em_scale PRIVATE catch2_main imgui)
add_test(NAME font_em_scale COMMAND test_font_em_scale)
register_test_target(test_font_em_scale)

# ── test_quest_text ──────────────────────────────────────────
# Which of a quest's five strings holds the line a finished quest shows. The
# pick used to be "whichever is longer", which answered a different question.
wowee_add_test(test_quest_text SOURCES test_quest_text.cpp)

# ── test_interface_text_lines ────────────────────────────────
# How many lines a label takes, measured on demand rather than read off the
# last frame. The quest tracker sizes every row from that answer, and a stale
# zero drew each wrapped objective over the row beneath it.
add_executable(test_interface_text_lines
    test_interface_text_lines.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/interface_fonts.cpp
)
target_include_directories(test_interface_text_lines PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_interface_text_lines SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/imgui
)
target_link_libraries(test_interface_text_lines PRIVATE catch2_main imgui)
add_test(NAME interface_text_lines COMMAND test_interface_text_lines)
register_test_target(test_interface_text_lines)

# ── test_npc_flags ───────────────────────────────────────────
# UNIT_NPC_FLAGS bit by bit. A wrong one is invisible - the mask just always
# answers no - and NPC_FLAG_VENDOR was 0x04, a bit nothing sets, so no merchant
# ever read as one.
wowee_add_test(test_npc_flags SOURCES test_npc_flags.cpp)

# ── test_scene_pick ──────────────────────────────────────────
# Which of ScenePick's answers a caller wants. closestGuid and resolve()
# disagree exactly where a wide object stands beside an NPC, which is where the
# vendor cursor was asking the wrong one.
wowee_add_test(test_scene_pick SOURCES test_scene_pick.cpp)

# ── test_sight_cache ─────────────────────────────────────────
# Line of sight for nameplates and chat bubbles: answered at once for a new
# unit, refreshed a few a frame for a crowd.
wowee_add_test(test_sight_cache SOURCES test_sight_cache.cpp)

# ── test_instance_index_set ──────────────────────────────────
# The M2 renderer's animated, particle and light sets, kept right across
# swap-removes, batch removals and hiding instead of rebuilt over every
# instance: they must match a rebuild after any of them.
wowee_add_test(test_instance_index_set SOURCES test_instance_index_set.cpp)

# ── test_chunked_vector ──────────────────────────────────────
# The M2 renderer's instance list: growing it moves nothing already in it,
# which is what made a tile's doodads cost 11-22 ms whenever the list doubled.
wowee_add_test(test_chunked_vector SOURCES test_chunked_vector.cpp)

# ── test_retire_queue ────────────────────────────────────────
# GPU objects released once the last frame that may use them has completed,
# by frame number, from any thread - what a render thread recording from a
# snapshot needs of every unload.
wowee_add_test(test_retire_queue SOURCES test_retire_queue.cpp)

# ── test_instance_random ─────────────────────────────────────
# Each M2 instance's own random numbers: independent of the others, so
# instances can be updated in parallel and still behave the same.
wowee_add_test(test_instance_random SOURCES test_instance_random.cpp)

# ── test_deferred_calls ──────────────────────────────────────
# The scene's changes wait for the frame's apply point: made then, in the
# order asked, those asked while applying included; a left world's dropped.
wowee_add_test(test_deferred_calls SOURCES test_deferred_calls.cpp)

# ── test_uniform_slot_pool ───────────────────────────────────
# Materials' uniform buffers as slots of a few large buffers: live slots never
# overlap, freed ones are reused before a block is made, offsets are aligned.
wowee_add_test(test_uniform_slot_pool
    SOURCES test_uniform_slot_pool.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/uniform_slot_pool.cpp
            ${TEST_COMMON_SOURCES})
if(TARGET Vulkan::Vulkan)
    target_link_libraries(test_uniform_slot_pool PRIVATE Vulkan::Vulkan)
endif()

# ── test_plural_escape ───────────────────────────────────────
# WoW's |4singular:plural; escape, which nothing resolved - so the logout
# prompt read "5 |4Second:Seconds; until logout" and seventy-four other
# strings did the same.
add_executable(test_plural_escape
    test_plural_escape.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/plural_escape.cpp
)
target_include_directories(test_plural_escape PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_plural_escape SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_plural_escape PRIVATE catch2_main)
add_test(NAME plural_escape COMMAND test_plural_escape)
register_test_target(test_plural_escape)

# ── test_macro_evaluator ─────────────────────────────────────
add_executable(test_macro_evaluator
    test_macro_evaluator.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/ui/chat/macro_evaluator.cpp
)
target_include_directories(test_macro_evaluator PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_macro_evaluator SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_macro_evaluator PRIVATE catch2_main)
add_test(NAME macro_evaluator COMMAND test_macro_evaluator)
register_test_target(test_macro_evaluator)


# ── test_gm_commands ─────────────────────────────────────────
add_executable(test_gm_commands
    test_gm_commands.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_gm_commands PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_gm_commands SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_gm_commands PRIVATE catch2_main)
add_test(NAME gm_commands COMMAND test_gm_commands)
register_test_target(test_gm_commands)

# ── test_open_formats ────────────────────────────────────────
wowee_add_test(test_open_formats
    SOURCES test_open_formats.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_building.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_collision.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_terrain_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
            ${TEST_COMMON_SOURCES})

# ── test_open_format_emitter ─────────────────────────────────
# Locks in the asset_extract → wowee open-format conversion path.
add_executable(test_open_format_emitter
    test_open_format_emitter.cpp
    ${CMAKE_SOURCE_DIR}/tools/asset_extract/open_format_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_model.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_building.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_collision.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_open_format_emitter PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_include_directories(test_open_format_emitter SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_open_format_emitter PRIVATE catch2_main)
wowee_test_link_glm(test_open_format_emitter)
add_test(NAME open_format_emitter COMMAND test_open_format_emitter)
register_test_target(test_open_format_emitter)

# ── test_frame_pacer ─────────────────────────────────────────
# Frame timing at 144Hz, where a millisecond is a seventh of the budget.
wowee_add_test(test_frame_pacer
    SOURCES test_frame_pacer.cpp
            ${CMAKE_SOURCE_DIR}/src/core/frame_pacer.cpp
            ${TEST_COMMON_SOURCES})
if(WIN32)
    # Same winmm the client links, for the same two timer calls.
    target_link_libraries(test_frame_pacer PRIVATE winmm)
endif()

# ── test_update_check ────────────────────────────────────────
# Version ordering for the update notice. No network: the ordering is the
# part that decides whether anybody is told anything.
add_executable(test_update_check
    test_update_check.cpp
    ${CMAKE_SOURCE_DIR}/src/core/update_check.cpp
    ${CMAKE_SOURCE_DIR}/src/core/https_get.cpp
    ${CMAKE_SOURCE_DIR}/src/core/config_paths.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_update_check PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated   # version.hpp, written by wowee_version
)
if(TARGET wowee_version)
    add_dependencies(test_update_check wowee_version)
endif()
target_include_directories(test_update_check SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_update_check PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
wowee_test_link_glm(test_update_check)
add_test(NAME update_check COMMAND test_update_check)
register_test_target(test_update_check)

# ── test_realm_patches ───────────────────────────────────────
# What a realm may name and where its archives may land. No network: the
# naming and the parsing are the part that decides whether a manifest can
# reach outside the directory its realm was given.
add_executable(test_realm_patches
    test_realm_patches.cpp
    ${CMAKE_SOURCE_DIR}/src/core/realm_patches.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_realm_patches PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_include_directories(test_realm_patches SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_realm_patches PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
wowee_test_link_glm(test_realm_patches)
add_test(NAME realm_patches COMMAND test_realm_patches)
register_test_target(test_realm_patches)

# ── test_extract_progress ────────────────────────────────────
# Extraction is the long stage of a build, and the only one that can say how
# far through it is. Needs StormLib: it builds a real archive to extract.
if(STORMLIB_LIBRARY AND STORMLIB_INCLUDE_DIR)
    add_executable(test_extract_progress
        test_extract_progress.cpp
        ${CMAKE_SOURCE_DIR}/tools/asset_extract/extractor.cpp
        ${CMAKE_SOURCE_DIR}/tools/asset_extract/path_mapper.cpp
        ${CMAKE_SOURCE_DIR}/tools/asset_extract/manifest_writer.cpp
        ${CMAKE_SOURCE_DIR}/tools/asset_extract/open_format_emitter.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/dds_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_model.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_building.cpp
        ${CMAKE_SOURCE_DIR}/src/pipeline/wowee_collision.cpp
        ${CMAKE_SOURCE_DIR}/src/rendering/m2_model_classifier.cpp
        ${TEST_COMMON_SOURCES}
    )
    target_include_directories(test_extract_progress PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}
        ${STORMLIB_INCLUDE_DIR}
    )
    target_include_directories(test_extract_progress SYSTEM PRIVATE
        ${TEST_SYSTEM_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/extern/nlohmann
    )
    target_link_libraries(test_extract_progress PRIVATE catch2_main)
    wowee_test_link_glm(test_extract_progress)
    wowee_link_stormlib(test_extract_progress)
    # The one test that can skip its whole body: on Linux the archive it
    # writes to set itself up aborts inside Ubuntu's StormLib, so the case
    # SKIPs. Catch2 then exits 4 for "no tests ran" and ctest called that a
    # failure. Only this test needs the flag, and only this test gets it -
    # elsewhere an empty run is a test that lost its cases, which should fail.
    add_test(NAME extract_progress
             COMMAND test_extract_progress --allow-running-no-tests)
    register_test_target(test_extract_progress)
endif()

# ── test_camera ──────────────────────────────────────────────
wowee_add_test(test_camera
    SOURCES test_camera.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/camera.cpp
            ${TEST_COMMON_SOURCES})

# ── test_install_probe ───────────────────────────────────────
# Which folder the archives are in, given whatever somebody pointed at. Links
# the probe alone: it is deliberately free of the extractor and the MPQ
# library, because the question is about directories.
add_executable(test_install_probe
    test_install_probe.cpp
    ${CMAKE_SOURCE_DIR}/tools/asset_manager/install_probe.cpp)
target_include_directories(test_install_probe PRIVATE ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/asset_manager)
target_include_directories(test_install_probe SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_install_probe PRIVATE catch2_main)
add_test(NAME install_probe COMMAND test_install_probe)
register_test_target(test_install_probe)

# ── test_gamepad ─────────────────────────────────────────────
# The stick arithmetic and the binding table. The table sits in its own file
# for exactly this reason: what applies it drives the camera and ImGui, and
# none of that is needed to ask whether a button has been bound twice.
add_executable(test_gamepad
    test_gamepad.cpp
    ${CMAKE_SOURCE_DIR}/src/core/gamepad.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/gamepad_bindings.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_gamepad PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_gamepad SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_gamepad PRIVATE catch2_main SDL3::SDL3)
wowee_test_link_glm(test_gamepad)
add_test(NAME gamepad COMMAND test_gamepad)
register_test_target(test_gamepad)

# ── test_camera_pivot ────────────────────────────────────────
# Header-only: the pivot arithmetic is a static member, so this needs SDL3's
# headers (camera_controller.hpp includes them) but none of its symbols.
add_executable(test_camera_pivot test_camera_pivot.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_camera_pivot PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_camera_pivot SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_camera_pivot PRIVATE catch2_main SDL3::SDL3)
wowee_test_link_glm(test_camera_pivot)
add_test(NAME camera_pivot COMMAND test_camera_pivot)
register_test_target(test_camera_pivot)

# ── test_gltf_terrain_bin ────────────────────────────────────
# The BIN chunk of a terrain .glb, packed byte for byte by two exporters.
# Checked against the glTF 2.0 layout rather than against the old code.
add_executable(test_gltf_terrain_bin
    test_gltf_terrain_bin.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_gltf_terrain_bin PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_gltf_terrain_bin SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_gltf_terrain_bin PRIVATE catch2_main)
wowee_test_link_glm(test_gltf_terrain_bin)
add_test(NAME gltf_terrain_bin COMMAND test_gltf_terrain_bin)
register_test_target(test_gltf_terrain_bin)

# ── test_editor_units (SQL escape, quest validation, …) ─────
add_executable(test_editor_units
    test_editor_units.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/sql_exporter.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/npc_spawner.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/quest_editor.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/content_pack.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/editor_brush.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/object_placer.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_editor_units PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_editor_units SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_editor_units PRIVATE catch2_main)
# npc_spawner.hpp pulls in <glm/glm.hpp>; link it when available so the
# test target builds on hosts where glm is system-installed (Homebrew etc).
wowee_test_link_glm(test_editor_units)
add_test(NAME editor_units COMMAND test_editor_units)
register_test_target(test_editor_units)
register_test_target(test_editor_units)

# ── test_expansion_profile ──────────────────────────────────
add_executable(test_expansion_profile
    test_expansion_profile.cpp
    ${CMAKE_SOURCE_DIR}/src/game/expansion_profile.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_expansion_profile PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_expansion_profile SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_expansion_profile PRIVATE catch2_main)
add_test(NAME expansion_profile COMMAND test_expansion_profile)
register_test_target(test_expansion_profile)

# ── test_buff_bar_layout ────────────────────────────────────
# Header-only layout arithmetic - no ImGui needed.
add_executable(test_buff_bar_layout
    test_buff_bar_layout.cpp
)
target_include_directories(test_buff_bar_layout PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_buff_bar_layout SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_buff_bar_layout PRIVATE catch2_main)
add_test(NAME buff_bar_layout COMMAND test_buff_bar_layout)
register_test_target(test_buff_bar_layout)

# ── test_dbc_layouts ────────────────────────────────────────
# Reads the layout files shipped in Data/expansions, so it needs the source dir.
add_executable(test_dbc_layouts
    test_dbc_layouts.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
)
target_include_directories(test_dbc_layouts PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_dbc_layouts SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_dbc_layouts PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_dbc_layouts PRIVATE catch2_main)
add_test(NAME dbc_layouts COMMAND test_dbc_layouts)
register_test_target(test_dbc_layouts)

# ── test_update_field_table ─────────────────────────────────
add_executable(test_update_field_table
    test_update_field_table.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/update_field_table.cpp
)
target_include_directories(test_update_field_table PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_update_field_table SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_update_field_table PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_update_field_table PRIVATE catch2_main)
add_test(NAME update_field_table COMMAND test_update_field_table)
register_test_target(test_update_field_table)

# ── test_pet_action ─────────────────────────────────────────
add_executable(test_pet_action
    test_pet_action.cpp
)
target_include_directories(test_pet_action PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_pet_action SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_pet_action PRIVATE catch2_main)
add_test(NAME pet_action COMMAND test_pet_action)
register_test_target(test_pet_action)

# ── test_spell_classification ───────────────────────────────
add_executable(test_spell_classification
    test_spell_classification.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_spell_classification PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_spell_classification SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_spell_classification PRIVATE catch2_main)
add_test(NAME spell_classification COMMAND test_spell_classification)
register_test_target(test_spell_classification)

# ── test_realm_list ─────────────────────────────────────────
add_executable(test_realm_list
    test_realm_list.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/auth_packets.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/auth_opcodes.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/crypto.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/big_num.cpp
)
target_include_directories(test_realm_list PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_realm_list SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_realm_list PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME realm_list COMMAND test_realm_list)
register_test_target(test_realm_list)

# ── test_auth_reconnect ──────────────────────────────────────
# The login server's reconnect handshake through the real AuthHandler and its
# TCP socket, against a login server the test runs on the loopback interface.
# The handler is a member of the client's archive; the linker takes it and
# what it reaches.
wowee_add_test(test_auth_reconnect SOURCES test_auth_reconnect.cpp)
target_link_libraries(test_auth_reconnect PRIVATE wowee_client)

# ── test_widget_draw ─────────────────────────────────────────
# WidgetRenderer::draw in a headless ImGui frame: what it settles in the tree
# before it paints. The renderer is a member of the client's archive.
wowee_add_test(test_widget_draw SOURCES test_widget_draw.cpp)
target_link_libraries(test_widget_draw PRIVATE wowee_client)

# ── test_quest_rewards ──────────────────────────────────────
wowee_add_packet_test(test_quest_rewards)

# ── test_inventory_sort ─────────────────────────────────────
add_executable(test_inventory_sort
    test_inventory_sort.cpp
    ${CMAKE_SOURCE_DIR}/src/game/inventory.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_inventory_sort PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_inventory_sort SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_inventory_sort PRIVATE catch2_main)
add_test(NAME inventory_sort COMMAND test_inventory_sort)
register_test_target(test_inventory_sort)

# ── test_quest_progress ─────────────────────────────────────
add_executable(test_quest_progress
    test_quest_progress.cpp
)
target_include_directories(test_quest_progress PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_quest_progress SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_quest_progress PRIVATE catch2_main)
add_test(NAME quest_progress COMMAND test_quest_progress)
register_test_target(test_quest_progress)

# ── ASAN / UBSan for test targets ────────────────────────────
if(WOWEE_ENABLE_ASAN AND NOT MSVC)
    foreach(_t IN LISTS ALL_TEST_TARGETS)
        target_compile_options(${_t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
    endforeach()
    # catch2_main must also be compiled with the same flags
    target_compile_options(catch2_main PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(catch2_main PRIVATE -fsanitize=address,undefined)
    # ...and so must wowee_test_common, for the same reason: its objects are
    # linked straight into the sanitized test executables, and mixing
    # instrumented and uninstrumented objects is how ASAN starts reporting
    # nonsense.
    target_compile_options(wowee_test_common PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    message(STATUS "Test targets: ASAN + UBSan ENABLED")
endif()

# ── test_m2_sway ─────────────────────────────────────────────
# Header-only and pure: what a model's sway parameters come out as, and how
# tall it reports itself. plantHeight was filled in only on the paths that
# sway, so anything else told the shader nothing about its own height.
wowee_add_test(test_m2_sway
    SOURCES test_m2_sway.cpp)

# ── test_wind_field ──────────────────────────────────────────
# The one wind over the trees, their shadows, the cloth and the grass: two
# clients reading the same realm clock in the same zone blow the same wind,
# a change eases in without a tree jumping, and wind_zones.json's rows match
# the places they name.
wowee_add_test(test_wind_field
    SOURCES test_wind_field.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/wind_field.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wind_zones.cpp
            ${TEST_COMMON_SOURCES})

# ── test_shadow_cull_reuse ───────────────────────────────────
# A shadow map's cached cull holds while the map has moved by less than the
# slack, and then drops no caster the new map keeps.
wowee_add_test(test_shadow_cull_reuse
    SOURCES test_shadow_cull_reuse.cpp)

# ── test_terrain_bake ────────────────────────────────────────
# Where the far ground is drawn from its bake: its layers near, the bake far,
# both across the band, and a slope keeping its layers as long as its
# steepness says.
wowee_add_test(test_terrain_bake
    SOURCES test_terrain_bake.cpp)

# ── test_shadow_plane ────────────────────────────────────────
# The plane a shadow receiver is compared in, from its surface and the map
# alone: along the surface it predicts the sun's and the units' map depth,
# whichever way and however long the normal, and edge-on it stays bounded.
wowee_add_test(test_shadow_plane
    SOURCES test_shadow_plane.cpp)

# ── test_weather_mix ─────────────────────────────────────────
# The weather as it is drawn: a shower coming over in the time it is given,
# rain turning to snow through both, a storm the realm asks for at once
# arriving at once.
wowee_add_test(test_weather_mix
    SOURCES test_weather_mix.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/weather_mix.cpp)

# ── test_shelter_probe ───────────────────────────────────────
# What the shelter map says of the camera's place, as its probe works it out
# on the GPU: under a roof or not, and how much of the sky a street's walls
# leave it.
wowee_add_test(test_shelter_probe
    SOURCES test_shelter_probe.cpp)

# ── test_lens_drops ──────────────────────────────────────────
# What the weather leaves on the camera's lens: rain gathering while the
# camera looks into it and drying under a roof, big drops running down,
# snow melting to water and frosting the edges, grit driven by the wind.
wowee_add_test(test_lens_drops
    SOURCES test_lens_drops.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/lens_drops.cpp)

# ── test_liquid_scene ────────────────────────────────────────
# The map's liquid: where the water stands and how high, which body each
# stretch of it belongs to as tiles stream in and out, and what the water
# renderer is told about it.
wowee_add_test(test_liquid_scene
    SOURCES test_liquid_scene.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/liquid_scene.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
            ${TEST_COMMON_SOURCES})

# ── test_liquid_types ────────────────────────────────────────
# What each liquid is: LiquidType.dbc through its layout, the client's rule
# for a WMO group's liquid, MCLQ's ocean - and what the scene then says.
wowee_add_test(test_liquid_types
    SOURCES test_liquid_types.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/liquid_scene.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types_dbc.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_layout.cpp
            ${TEST_COMMON_SOURCES})

# ── test_ocean_spectrum ──────────────────────────────────────
# The wind's sea as the ocean's compute shaders draw it: steep, downwind,
# its waves as long and as fast as the sea's.
wowee_add_test(test_ocean_spectrum
    SOURCES test_ocean_spectrum.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/ocean_sea.cpp)

# ── test_water_wake ──────────────────────────────────────────
# The water's answer to what moves through it: water's speeds, Kelvin's wedge.
wowee_add_test(test_water_wake
    SOURCES test_water_wake.cpp)

# ── test_sea_bed ─────────────────────────────────────────────
# What lies under the water: the map's views, and which buildings are its bed.
wowee_add_test(test_sea_bed
    SOURCES test_sea_bed.cpp)

# ── test_ocean_fft ───────────────────────────────────────────
# The sea's surface out of its spectrum, by the compute shaders' own FFT.
wowee_add_test(test_ocean_fft
    SOURCES test_ocean_fft.cpp)

# ── test_water_reflection ────────────────────────────────────
# The mirrored camera sees what stands above the water and nothing of the bed.
wowee_add_test(test_water_reflection
    SOURCES test_water_reflection.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/water_reflection.cpp
            ${TEST_COMMON_SOURCES})

# ── test_sphere_map ──────────────────────────────────────────
# The sphere environment map coordinates m2.vert.glsl computes, run from the
# shader's own include. The incident vector was negated, which put a surface
# nearly facing the camera at the rim of the map instead of near its centre.
wowee_add_test(test_sphere_map
    SOURCES test_sphere_map.cpp)

# ── test_m2_classifier_foliage ───────────────────────────────
# Pins the wind-sway classification against substring collisions: foliage
# tokens are matched inside concatenated names, so "thorn" landed inside
# Stranglethorn and made every troll ruin sway like a tree.
wowee_add_test(test_m2_classifier_foliage
    SOURCES test_m2_classifier_foliage.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/m2_model_classifier.cpp)

# ── test_terrain_slope_limit ────────────────────────────────
# The 50 degree limit as it applies to the ground. It governed WMO and M2
# floors; the terrain query answers a height and no normal, so it was never
# applied there and any mountain could be walked straight up.
wowee_add_test(test_terrain_slope_limit
    SOURCES test_terrain_slope_limit.cpp)

# ── test_particle_density_floor ─────────────────────────────
# M2 particle emission, flight and layout against the 3.3.5a client's own
# arithmetic: pools, rate and distance LOD, FBlocks, spawn geometry, the
# emitter's quarter turn, gravity, drag and wind, size and spin.
wowee_add_test(test_particle_density_floor
    SOURCES test_particle_density_floor.cpp)

# ── test_camera_boom ────────────────────────────────────────
# The third-person camera's distance: pulled in on a wall, eased back out.
wowee_add_test(test_camera_boom
    SOURCES test_camera_boom.cpp)

# ── test_game_object_animation ──────────────────────────────
# A GameObject's animation for its state: Closed, Opened and the ways between.
wowee_add_test(test_game_object_animation
    SOURCES test_game_object_animation.cpp)

# ── test_m2_combiner ────────────────────────────────────────
# How an M2 batch combines its textures, by the client's rules.
wowee_add_test(test_m2_combiner
    SOURCES test_m2_combiner.cpp)

# ── test_local_lights ───────────────────────────────────────
# Which point lights reach a model, a WMO group or a terrain chunk.
wowee_add_test(test_local_lights
    SOURCES test_local_lights.cpp)

# ── test_shadow_shading ─────────────────────────────────────
# How a surface takes a shadow, by the client's receiving shaders.
wowee_add_test(test_shadow_shading
    SOURCES test_shadow_shading.cpp)

# ── test_m2_billboard_model ─────────────────────────────────
# Mana Shield's card through the renderer's own bone update.
wowee_add_test(test_m2_billboard_model
    SOURCES test_m2_billboard_model.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp
            ../src/rendering/m2_model_classifier.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/texture_cache.cpp
            ${TEST_COMMON_SOURCES})
target_compile_definitions(test_m2_billboard_model PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ── test_m2_billboard ───────────────────────────────────────
# Billboard bones, spherical and locked to one axis, as the client turns them.
wowee_add_test(test_m2_billboard
    SOURCES test_m2_billboard.cpp)

# ── test_m2_star_layer ──────────────────────────────────────
# Which batch of a sky model is its stars. The clouds, planets and nebulae
# of the same dome sit in the same directory, one texture holds both stars
# and clouds, and a floor doodad in Ulduar is named for stars without being
# a sky - so suppressing the wrong one deletes the sky instead of its stars.
wowee_add_test(test_m2_star_layer
    SOURCES test_m2_star_layer.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/m2_model_classifier.cpp)

# ── test_calendar_packet ────────────────────────────────────
add_executable(test_calendar_packet
    test_calendar_packet.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
    ${CMAKE_SOURCE_DIR}/src/game/calendar_data.cpp
)
target_include_directories(test_calendar_packet PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_calendar_packet SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_calendar_packet PRIVATE catch2_main SDL3::SDL3 ZLIB::ZLIB
                                                 OpenSSL::SSL OpenSSL::Crypto)
wowee_test_link_glm(test_calendar_packet)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(test_calendar_packet PRIVATE Vulkan::Vulkan)
endif()
add_test(NAME calendar_packet COMMAND test_calendar_packet)
register_test_target(test_calendar_packet)

# ── test_calendar_month ─────────────────────────────────────
add_executable(test_calendar_month test_calendar_month.cpp)
target_include_directories(test_calendar_month PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_calendar_month SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_calendar_month PRIVATE catch2_main)
add_test(NAME calendar_month COMMAND test_calendar_month)
register_test_target(test_calendar_month)

# ── test_catalog_entry_key ───────────────────────────────────
# Which field of a catalog entry is its id. Four commands guessed it from the
# field's name and disagreed; the format table now declares it. The oracle is
# each format's own --info handler, so the test reads the editor sources.
add_executable(test_catalog_entry_key
    test_catalog_entry_key.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/cli_format_table.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_catalog_entry_key PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_catalog_entry_key SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_compile_definitions(test_catalog_entry_key PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_catalog_entry_key PRIVATE catch2_main)
add_test(NAME catalog_entry_key COMMAND test_catalog_entry_key)
register_test_target(test_catalog_entry_key)

# ── test_catalog_subprocess ──────────────────────────────────
# How the catalog searches reach a file: they re-invoke this executable with
# the format's --info flag, started directly rather than through a shell. The
# child is child_echo, which says back the argument it was given.
add_executable(test_child_echo child_echo.cpp)
add_executable(test_catalog_subprocess
    test_catalog_subprocess.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/cli_subprocess.cpp
    ${TEST_COMMON_SOURCES})
add_dependencies(test_catalog_subprocess test_child_echo)
target_compile_definitions(test_catalog_subprocess PRIVATE
    CHILD_ECHO_PATH="$<TARGET_FILE:test_child_echo>")
target_include_directories(test_catalog_subprocess PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_catalog_subprocess SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_catalog_subprocess PRIVATE catch2_main)
add_test(NAME catalog_subprocess COMMAND test_catalog_subprocess)
register_test_target(test_catalog_subprocess)

# ── test_cli_paths ───────────────────────────────────────────
# Finding the files of one format under a directory. Eleven commands walked
# the tree themselves for .wom alone. The oracle is a tree the test writes.
add_executable(test_cli_paths
    test_cli_paths.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_cli_paths PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_cli_paths SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_cli_paths PRIVATE catch2_main)
add_test(NAME cli_paths COMMAND test_cli_paths)
register_test_target(test_cli_paths)

# ── test_obj_parse ───────────────────────────────────────────
# Reading a Wavefront OBJ, which two importers each implemented. The oracle
# is the OBJ format: 1-based indices, negative indices relative to the pool
# as it stands, and three independently indexed pools.
add_executable(test_obj_parse
    test_obj_parse.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_obj_parse PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_obj_parse SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_obj_parse PRIVATE catch2_main)
wowee_test_link_glm(test_obj_parse)
add_test(NAME obj_parse COMMAND test_obj_parse)
register_test_target(test_obj_parse)

# ── test_model_bounds ────────────────────────────────────────
# The box and radius a .wom carries. Eighteen places computed them three
# different ways. The oracle is what the field means: M2Loader sets it from
# the M2 header's boundingRadius, measured from the model origin.
wowee_add_test(test_model_bounds
    SOURCES test_model_bounds.cpp
            ${TEST_COMMON_SOURCES})

# ── test_format_lookup ───────────────────────────────────────
# Finding a format by extension, which three commands each implemented.
# --fix-magic renames on the answer. The oracle is the table itself: every
# row's extension has to find that row.
add_executable(test_format_lookup
    test_format_lookup.cpp
    ${CMAKE_SOURCE_DIR}/tools/editor/cli_format_table.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_format_lookup PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tools/editor
)
target_include_directories(test_format_lookup SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_format_lookup PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_format_lookup PRIVATE catch2_main)
add_test(NAME format_lookup COMMAND test_format_lookup)
register_test_target(test_format_lookup)

# ── test_asset_token_name ────────────────────────────────────
# Which part of an asset path a token check may read. A directory name
# poisons every token under it: Item/TextureComponents/LegLowerTexture
# spells "glow", so every leg and boot texture was being colour-keyed.
wowee_add_test(test_asset_token_name
    SOURCES test_asset_token_name.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/m2_model_classifier.cpp)

# ── test_pom_quality_one_scale ──────────────────────────────
# Parallax quality is one scale and everywhere it is offered has to agree.
# The login screen offered two entries of a three entry setting and wrote the
# wrong index for the two it did offer, so its "High" asked for Medium.
add_executable(test_pom_quality_one_scale
    test_pom_quality_one_scale.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_pom_quality_one_scale PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_pom_quality_one_scale SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_pom_quality_one_scale PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_pom_quality_one_scale PRIVATE catch2_main)
add_test(NAME pom_quality_one_scale COMMAND test_pom_quality_one_scale)
register_test_target(test_pom_quality_one_scale)

# ── test_settings_apply_on_load ──────────────────────────────
# A graphics setting read from the config file has to reach something.
# Loading only filled the panel's pending fields, so a view distance chosen
# at the login screen was shown on the slider and never applied.
add_executable(test_settings_apply_on_load
    test_settings_apply_on_load.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/settings_schema.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_settings_apply_on_load PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_settings_apply_on_load SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_settings_apply_on_load PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_settings_apply_on_load PRIVATE catch2_main)
add_test(NAME settings_apply_on_load COMMAND test_settings_apply_on_load)
register_test_target(test_settings_apply_on_load)

# ── test_light_coords ────────────────────────────────────────
# Where Light.dbc says a light volume is. The scale was 1.0 against a file
# storing thirty-sixths of a yard on mirrored, swapped, corner-origin axes,
# so no volume matched anywhere and every zone fell back to default light.
wowee_add_test(test_light_coords
    SOURCES test_light_coords.cpp
            ${TEST_COMMON_SOURCES})

# ── test_swim_wall ─────────────────────────────────────────────────────
# The shoreline rule a swimmer meets. Header-only, so it links nothing: a
# heightmap is a lambda here.
wowee_add_test(test_swim_wall SOURCES test_swim_wall.cpp)

# ── test_light_bands ─────────────────────────────────────────
# Which LightParams row and channel a light band belongs to, and where its
# fields are. The block mapping was off by one in both terms and the field
# indices by one, so 12024 of 15300 colour bands read as empty.
add_executable(test_light_bands
    test_light_bands.cpp
    test_distance_fog.cpp
    test_rt_light_composition.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_light_bands PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_light_bands SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_light_bands PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_light_bands PRIVATE catch2_main)
wowee_test_link_glm(test_light_bands)
add_test(NAME light_bands COMMAND test_light_bands)
register_test_target(test_light_bands)

# ── test_sky_model_gate ──────────────────────────────────────
# When the original client's sky models stand in for its own stars, sun,
# moons and clouds, and when they are drawn over them: LightingManager over
# Light, LightParams and LightSkybox files made up in a data root. Links the
# pipeline library for the AssetManager and the DBC reader.
add_executable(test_sky_model_gate
    test_sky_model_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/lighting_manager.cpp)
target_include_directories(test_sky_model_gate PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_sky_model_gate SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_sky_model_gate PRIVATE catch2_main wowee_pipeline wowee_base)
wowee_test_link_glm(test_sky_model_gate)
add_test(NAME sky_model_gate COMMAND test_sky_model_gate)
register_test_target(test_sky_model_gate)

# ── test_zone_light ──────────────────────────────────────────
# A zone is lit by the Light.dbc volumes over it and nothing that knows its
# id: LightingManager over a light volume made up in a data root, stood in
# under Tirisfal's, Silverpine's and Duskwood's zone ids.
add_executable(test_zone_light
    test_zone_light.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/lighting_manager.cpp)
target_include_directories(test_zone_light PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_zone_light SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_zone_light PRIVATE catch2_main wowee_pipeline wowee_base)
wowee_test_link_glm(test_zone_light)
add_test(NAME zone_light COMMAND test_zone_light)
register_test_target(test_zone_light)

# ── test_light_volume_blend ──────────────────────────────────
# A point lit as WoW.exe lights it: its map's global light with the volumes
# it stands in laid over it, farthest first, each by its linear falloff.
add_executable(test_light_volume_blend
    test_light_volume_blend.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/lighting_manager.cpp)
target_include_directories(test_light_volume_blend PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_light_volume_blend SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_light_volume_blend PRIVATE catch2_main wowee_pipeline wowee_base)
wowee_test_link_glm(test_light_volume_blend)
add_test(NAME light_volume_blend COMMAND test_light_volume_blend)
register_test_target(test_light_volume_blend)

# ── test_light_headroom ──────────────────────────────────────
# Keeping ambient + diffuse inside 1.0. 82% of the client's LightParams
# rows exceed it at noon, and per-channel clipping shifts hue: Mulgore's
# grass went searing yellow, Teldrassil's canopies magenta.
wowee_add_test(test_light_headroom
    SOURCES test_light_headroom.cpp
            ${TEST_COMMON_SOURCES})

# ── test_sky_celestial ───────────────────────────────────────
# The sun as WoW.exe shows it: the disc's own path under the horizon at
# night, its size, the glare's hours, weather, and looking into it.
wowee_add_test(test_sky_celestial
    SOURCES test_sky_celestial.cpp)

# ── test_sky_billboard ───────────────────────────────────────
# The sun and moon quads face the eye, so their discs are round on screen.
# They lay in the world's horizontal plane and the sun at 37 degrees came
# out a flat oval.
wowee_add_test(test_sky_billboard
    SOURCES test_sky_billboard.cpp)

# ── test_light_params_eval ───────────────────────────────────
# Which LightParams row a volume names for the weather, what a row with
# missing bands gives, and how volumes blend when one names a missing row.
# A missing fog band put the fog's end at 0.03 yards.
wowee_add_test(test_light_params_eval
    SOURCES test_light_params_eval.cpp)

# ── test_camera_water_depth ──────────────────────────────────
# Whether the water over the camera is the water it is in. The 400-unit
# query found a lake over a dry cave and lit the cave as under water.
wowee_add_test(test_camera_water_depth
    SOURCES test_camera_water_depth.cpp)

# ── test_interior_light ──────────────────────────────────────
# The light of a model inside a WMO interior group. Doodads and characters
# inside a WMO took the sun and the zone's LightParams instead of the
# building's MOHD ambient; the shaders now light each model by where it
# stands, from interior_light.glsli.
wowee_add_test(test_interior_light
    SOURCES test_interior_light.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
            ${TEST_COMMON_SOURCES})

# ── test_m2_view_distance ────────────────────────────────────
# How far one M2 instance draws. Three separate widenings - a size factor,
# an animation factor and the game object floor - each pushed past the view
# distance the player asked for, so trees and mailboxes hung in the air
# beyond where the terrain and the WMOs stop.
add_executable(test_m2_view_distance
    test_m2_view_distance.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_view_distance PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_view_distance SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_view_distance PRIVATE catch2_main)
add_test(NAME m2_view_distance COMMAND test_m2_view_distance)
register_test_target(test_m2_view_distance)

# ── test_light_volume_order ──────────────────────────────────
# The client's rule for laying light volumes over each other: each one's
# weight, the order they go down in, and what each ends up contributing.
add_executable(test_light_volume_order
    test_light_volume_order.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_light_volume_order PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_light_volume_order SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_light_volume_order PRIVATE catch2_main)
wowee_test_link_glm(test_light_volume_order)
add_test(NAME light_volume_order COMMAND test_light_volume_order)
register_test_target(test_light_volume_order)

# Sky clock/opacity and real Dalaran UV mask regression.
add_executable(test_m2_sky_material test_m2_sky_material.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_sky_material PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_sky_material SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_m2_sky_material PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_m2_sky_material PRIVATE catch2_main)
wowee_test_link_glm(test_m2_sky_material)
add_test(NAME m2_sky_material COMMAND test_m2_sky_material)
register_test_target(test_m2_sky_material)

# ── test_m2_blend_mode ───────────────────────────────────────
# Whether an M2 batch is alpha tested. Additive batches were forced onto
# the cutout pipeline when their texture had no alpha, which drew every
# glow card as a flat opaque disc.
add_executable(test_m2_blend_mode
    test_m2_blend_mode.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_blend_mode PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_blend_mode SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_blend_mode PRIVATE catch2_main)
add_test(NAME m2_blend_mode COMMAND test_m2_blend_mode)
register_test_target(test_m2_blend_mode)

# ── test_m2_glow_card ────────────────────────────────────────
# What the renderer does with a glow-card batch. Three decisions the two
# render passes each made for themselves, in expressions identical down to
# the constants; a batch is skipped by one pass and drawn by the other, so
# drift there draws the card twice or never.
add_executable(test_m2_glow_card
    test_m2_glow_card.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_glow_card PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_glow_card SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_glow_card PRIVATE catch2_main)
add_test(NAME m2_glow_card COMMAND test_m2_glow_card)
register_test_target(test_m2_glow_card)

# ── test_m2_color_track ──────────────────────────────────────
# Where an M2 colour track's first key is. WotLK nests an array per
# sequence; vanilla points straight at the values. Reading one as the other
# gives a plausible colour rather than a failure.
add_executable(test_m2_color_track
    test_m2_color_track.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_color_track PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_color_track SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_color_track PRIVATE catch2_main)
add_test(NAME m2_color_track COMMAND test_m2_color_track)
register_test_target(test_m2_color_track)

# ── test_dbc_layout_columns ──────────────────────────────────
# Every column a layout names has to exist in the file it describes. A DBC
# asked for a field past the end answers zero for every row, forever, which
# is how the light bands read as empty for as long as they did.
add_executable(test_dbc_layout_columns
    test_dbc_layout_columns.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_dbc_layout_columns PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_dbc_layout_columns SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_dbc_layout_columns PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_dbc_layout_columns PRIVATE catch2_main)
add_test(NAME dbc_layout_columns COMMAND test_dbc_layout_columns)
register_test_target(test_dbc_layout_columns)

# ── test_update_field_indices ────────────────────────────────
# Update field slot numbers against AzerothCore's UpdateFields.h. Reading
# the wrong slot returns a neighbouring field, usually zero for a guid, so
# the feature looks unsent rather than misread.
add_executable(test_update_field_indices
    test_update_field_indices.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_update_field_indices PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_update_field_indices SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_update_field_indices PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_update_field_indices PRIVATE catch2_main)
add_test(NAME update_field_indices COMMAND test_update_field_indices)
register_test_target(test_update_field_indices)

# ── test_opcode_tables ───────────────────────────────────────
# Opcode numbers and the structure around them. All 1306 WotLK numbers were
# checked against AzerothCore's Opcodes.h and agreed; this holds them there
# and pins the _extends inheritance the smaller tables rely on.
add_executable(test_opcode_tables
    test_opcode_tables.cpp
    ${TEST_COMMON_SOURCES})
target_include_directories(test_opcode_tables PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_opcode_tables SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_opcode_tables PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_opcode_tables PRIVATE catch2_main)
add_test(NAME opcode_tables COMMAND test_opcode_tables)
register_test_target(test_opcode_tables)

# ── test_movement_flag_bits ──────────────────────────────────
# Movement flag bits against AzerothCore's UnitDefines.h. FEATHER_FALL and
# WATER_WALK sat on PENDING_STOP and PENDING_STRAFE_STOP, so the client
# asserted a pending stop in every packet while those states were active.
wowee_add_test(test_movement_flag_bits
    SOURCES test_movement_flag_bits.cpp
            ${TEST_COMMON_SOURCES})

# ── test_spell_icon_columns ──────────────────────────────────
# SpellIcon.dbc through the loader every caller uses, over the install's data.
# A wrong column answers empty for every row rather than failing, so the test
# is what the loader hands back from the real file. Skips without game data.
add_executable(test_spell_icon_columns
    test_spell_icon_columns.cpp)
target_include_directories(test_spell_icon_columns PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_spell_icon_columns SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_spell_icon_columns PRIVATE
    WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_spell_icon_columns PRIVATE catch2_main wowee_pipeline)
wowee_test_link_glm(test_spell_icon_columns)
add_test(NAME spell_icon_columns COMMAND test_spell_icon_columns)
register_test_target(test_spell_icon_columns)

# ── test_json_table_scan ─────────────────────────────────────
# The scanner both expansion table loaders now share. The value parser is the
# point: stoul on "0x12" in base ten answers 0 without raising, and 0 is a
# real opcode and a real update field index.
wowee_add_test(test_json_table_scan
    SOURCES test_json_table_scan.cpp
            ${TEST_COMMON_SOURCES})

# ── test_auction_slot_filter ─────────────────────────────────
# The auction house's equipment-slot filter. FrameXML numbers the rows it is
# given from one, so the number that comes back is a position in the subset
# offered for that class rather than in the whole slot table.
wowee_add_test(test_auction_slot_filter
    SOURCES test_auction_slot_filter.cpp
            ${TEST_COMMON_SOURCES})

# ── test_player_model_path ───────────────────────────────────
# Which body a character wears. Gender alone does not answer it: a nonbinary
# character picks one, and the preview, the world spawn and the skin textures
# each read that choice differently.
wowee_add_test(test_player_model_path
    SOURCES test_player_model_path.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/character.cpp)

# ── test_equipment_hash ──────────────────────────────────────
# The fingerprint a cache redraws on when the outfit changes. Two copies
# existed, each deciding for itself which fields change how an item looks.
wowee_add_test(test_equipment_hash
    SOURCES test_equipment_hash.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/character.cpp)

# ── test_quest_giver_status ──────────────────────────────────
# DIALOG_STATUS_* and the mark it draws. Five of the eleven values had no name,
# and a value with no name drew nothing at all.
wowee_add_test(test_quest_giver_status
    SOURCES test_quest_giver_status.cpp
            ${TEST_COMMON_SOURCES})

# ── test_collision_geometry ──────────────────────────────────
# The ray, box and reach tests every collision query is built from. Both
# renderers had their own copy, and a difference moves where the world is solid.
wowee_add_test(test_collision_geometry
    SOURCES test_collision_geometry.cpp
            ${TEST_COMMON_SOURCES})

# ── test_collision_world ─────────────────────────────────────
# The solid world of a map: floors, walls, sight lines and interiors of
# buildings, doodads and terrain, as they are placed, moved, hidden and leave.
wowee_add_test(test_collision_world
    SOURCES test_collision_world.cpp)
target_link_libraries(test_collision_world PRIVATE wowee_collision)

# ── test_race_gender_suffix ──────────────────────────────────
# The two-letter race code per-race models are suffixed with. Written out twice
# - the helmet resolver and the shoulder attachment - and a wrong suffix reads
# as "no per-race cut" rather than as an error.
wowee_add_test(test_race_gender_suffix
    SOURCES test_race_gender_suffix.cpp
            ${TEST_COMMON_SOURCES})

# ── test_melee_anim_chains ───────────────────────────────────
# The attack animation fallback chains, which were written out twice - in the
# controller that plays them and in the probe that reports what a model can do.
wowee_add_test(test_melee_anim_chains
    SOURCES test_melee_anim_chains.cpp
            ${TEST_COMMON_SOURCES})

# ── test_sample_pick ─────────────────────────────────────────
# Which of a sound bank's samples to play. Three banks wrote out the same
# loop, and its one rule - choose among the loaded ones - is a one-character
# difference from silence.
wowee_add_test(test_sample_pick
    SOURCES test_sample_pick.cpp
            ${TEST_COMMON_SOURCES})

# ── test_footstep_paths ──────────────────────────────────────
# The names the footstep sounds are stored under. Six families, none derivable
# from another, and a name that stops matching the archive is silent.
wowee_add_test(test_footstep_paths
    SOURCES test_footstep_paths.cpp
            ${TEST_COMMON_SOURCES})

# ── test_sun_direction ───────────────────────────────────────
# The sky mirrored a sun below the horizon back up into it, and the lens flare
# is the only thing that asks where the sun is - so the mirror invented one for
# it to draw around at night.
wowee_add_test(test_sun_direction
    SOURCES test_sun_direction.cpp
            ${TEST_COMMON_SOURCES})

# ── test_wmo_night_glow ──────────────────────────────────────
# When F_SIDN windows and lamps glow, by the client's own table over the day.
wowee_add_test(test_wmo_night_glow
    SOURCES test_wmo_night_glow.cpp
            ${TEST_COMMON_SOURCES})

# ── test_wmo_lighting ────────────────────────────────────────
# Which light each batch of a WMO group is drawn with: by its MOBA run, its
# group, its material and the model's path, as WoW.exe picks it. Groups are
# read through the loader; the real Dalaran, Duskwood and Orgrimmar cases run
# when the WotLK data is extracted.
wowee_add_test(test_wmo_lighting
    SOURCES test_wmo_lighting.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/wmo_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp
            ${TEST_COMMON_SOURCES})
target_compile_definitions(test_wmo_lighting PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ── test_placement_transform ─────────────────────────────────
# The model matrix a doodad or a building placement becomes. Nothing on flat
# ground can tell the six composition orders apart, so this checks four real
# placements that carry pitch or roll against the world-space bounding boxes
# their own MODF records hold.
wowee_add_test(test_placement_transform
    SOURCES test_placement_transform.cpp
            ${TEST_COMMON_SOURCES})

# ── test_wmo_batch_merge ─────────────────────────────────────
# Which WMO group batches draw as one. A merged batch is drawn with its first
# batch's material, so opaque and blended materials on one texture, or two
# banners with their own pivots, must stay apart.
wowee_add_test(test_wmo_batch_merge
    SOURCES test_wmo_batch_merge.cpp
            ${TEST_COMMON_SOURCES})

# ── test_wmo_group_path ──────────────────────────────────────
# The names a WMO's group file might carry. Five loaders tried different
# numbers of them, so a building spelled ".WMO" loaded its interior on some
# paths and silently did not on others.
wowee_add_test(test_wmo_group_path
    SOURCES test_wmo_group_path.cpp
            ${TEST_COMMON_SOURCES})

# ── test_env_flag ────────────────────────────────────────────
# Reading a boolean out of an environment variable. Five files had their own
# copy and two rules: one read "off" as enabled because it tested only the
# first character.
wowee_add_test(test_env_flag
    SOURCES test_env_flag.cpp
            ${TEST_COMMON_SOURCES})

# ── test_inventory_count ─────────────────────────────────────
# How many of an item the bags hold. Counted in five places with two rules:
# the Lua bindings read an occupied slot with a zero stack as one item and the
# client's own counters read it as none, so a reagent could be in the bags and
# absent from the crafting check at the same time.
wowee_add_test(test_inventory_count
    SOURCES test_inventory_count.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/game/inventory.cpp)

# ── test_gather_spells ───────────────────────────────────────
# The mining and herbalism rank ids, which were written out four times across
# two files. A list that gains a rank in one copy and not the others leaves the
# player unable to gather with that rank, silently.
wowee_add_test(test_gather_spells
    SOURCES test_gather_spells.cpp
            ${TEST_COMMON_SOURCES})

# ── test_group_list_parse ────────────────────────────────────
# SMSG_GROUP_LIST built as the server writes it and read back, in both its
# shapes. The dungeon finder shape was read a byte long - a seventh field that
# is never sent - so every member of every finder group came out shifted and
# the member count was three bytes of the real count plus a letter of a name.
wowee_add_packet_test(test_group_list_parse)

# ── test_stationery ──────────────────────────────────────────
# The letterhead a letter is written on. Answering nil for it drew the mail
# body's dark brown font on an empty black frame.
wowee_add_test(test_stationery
    SOURCES test_stationery.cpp
            ${TEST_COMMON_SOURCES})

# ── test_polygon_triangulate ─────────────────────────────────
# Ear clipping for quest blob outlines, which are regularly concave. The
# check is that the triangles cover the ring's own area and no more.
wowee_add_test(test_polygon_triangulate
    SOURCES test_polygon_triangulate.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/polygon_triangulate.cpp
            ${TEST_COMMON_SOURCES})

# ── test_raid_mark_names ─────────────────────────────────────
# The eight raid marker names, which /mark matches lowercase and the minimap
# prints capitalised. They were written out three times; the words the
# command accepts are now derived, so they are pinned here.
wowee_add_test(test_raid_mark_names
    SOURCES test_raid_mark_names.cpp
            ${TEST_COMMON_SOURCES})

# ── test_adt_truncation ──────────────────────────────────────
# What the ADT parser does with a file that lies about its own sizes. The
# chunk readers took a bare pointer and a length as separate arguments and
# trusted every caller's arithmetic; one of those sums wrapped, turning a
# four-byte MCAL into a memcpy length of 1.8e19. Malformed input only -- run
# under --asan, where an overrun fails rather than passing unnoticed.
wowee_add_test(test_adt_truncation
    SOURCES test_adt_truncation.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_blp_truncation ──────────────────────────────────────
# What the BLP loader does with a texture that lies about its own sizes. The
# decompressors derived how much source to read from width and height and
# bounded only the destination, so a 4096x4096 DXT1 header with an eight-byte
# mip level read two megabytes out of an eight-byte buffer. Malformed input
# only -- run under --asan, where the overrun fails rather than passing.
wowee_add_test(test_blp_truncation
    SOURCES test_blp_truncation.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp)

# ── test_grass_blade_layout ────────────────────────────
# The C++ and GLSL views of a grass blade have to agree byte for byte, and a
# disagreement is silent: it compiles, passes validation, and renders the wrong
# thing. Header-only on purpose -- grass_blade.hpp needs nothing but glm, so
# this stays a fast test rather than one that drags in a Vulkan context.
wowee_add_test(test_grass_blade_layout
    SOURCES test_grass_blade_layout.cpp)

# ── test_adt_alpha ─────────────────────────────────────
# MCAL alpha maps, and the row and column at the end of a four-bit one that are
# not data. Left alone they put a hard line between every pair of chunks.
wowee_add_test(test_adt_alpha
    SOURCES test_adt_alpha.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_grass_terrain ─────────────────────────────────
# Whether grass grows at a point, answered from the ADT's own ground-effect
# ids rather than from a classifier of ours. Headless: the chunks are built by
# hand, so this needs no device, no tile and no world.
wowee_add_test(test_grass_terrain
    SOURCES test_grass_terrain.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_terrain.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_profile.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/terrain_mesh.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_alpha.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/adt_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/liquid_types.cpp)

# ── test_grass_population ──────────────────────────────
# Blades generated from terrain suitability. Most of these cases are one
# property from different angles: a blade's identity comes from its world
# lattice cell and nothing else, so rebuilding the population as the player
# moves cannot shuffle the field.
wowee_add_test(test_grass_population
    SOURCES test_grass_population.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_population.cpp)

# ── test_grass_profile ─────────────────────────────────
# What grass looks like on a patch of ground, from the detail doodads the map
# places there. The asset names carry the classification, so nothing here knows
# what a zone is - a profile needing a per-zone correction would be the wrong
# shape entirely.
wowee_add_test(test_grass_profile
    SOURCES test_grass_profile.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_profile.cpp)

# ── test_grass_biomes ──────────────────────────────────
# The per-zone look table: parsing, subzone-over-zone matching, and that an
# override multiplies the scales and replaces the colours it names while
# leaving everything else the effect data derived.
wowee_add_test(test_grass_biomes
    SOURCES test_grass_biomes.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_biomes.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_profile.cpp)

# ── test_grass_clearing ────────────────────────────────
# The clearings the placed world presses into the grass: box distance, the
# cleared ring, the ease band, and the minimum across overlapping sources.
wowee_add_test(test_grass_clearing
    SOURCES test_grass_clearing.cpp
            ${CMAKE_SOURCE_DIR}/src/pipeline/grass_clearing.cpp)

# ── test_grass_cull_math ───────────────────────────────
# The frustum arithmetic the grass cull performs, run for a camera turned all
# the way round. Grass vanishing in one world direction is a claim about this
# and nothing else, and it is cheaper to measure than to reason about.
wowee_add_test(test_grass_cull_math
    SOURCES test_grass_cull_math.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/frustum.cpp)

# ── test_pass_ablation ───────────────────────────────
# The phase walk and the arithmetic of the ablation report. It is the only
# instrument that can attribute a frame on a driver that resolves every
# in-pass GPU timestamp to the same clock, so there is nothing else to check
# its numbers against.
wowee_add_test(test_pass_ablation
    SOURCES test_pass_ablation.cpp
            ${CMAKE_SOURCE_DIR}/src/rendering/pass_ablation.cpp)

# ── Sanitizers, once every test target exists ────────────────
# This must be the last thing in this file. The loop walks ALL_TEST_TARGETS,
# which register_test_target appends to as each test is declared, so the
# equivalent block higher up only ever saw the targets above it -- 47 tests are
# declared after it. `./test.sh --asan` was building those uninstrumented and
# reporting them as passing.
#
# Re-applying to targets the earlier block already covered is harmless: the
# flags are identical and repeat on the command line.
get_property(ALL_TEST_TARGETS GLOBAL PROPERTY WOWEE_ALL_TEST_TARGETS)
list(LENGTH ALL_TEST_TARGETS _wowee_test_target_count)
message(STATUS "Test targets registered: ${_wowee_test_target_count}")

if(WOWEE_ENABLE_ASAN AND NOT MSVC)
    foreach(_t IN LISTS ALL_TEST_TARGETS)
        target_compile_options(${_t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

# A precompiled header for the test targets was tried here and removed: with
# ~140 executables each generating its own PCH, building them cost as much as
# the re-parsing it saved (950.8 s of CPU against 945.2 s without). The main
# target benefits because it is one target over 413 objects; the tests are the
# opposite shape. Do not re-add it without measuring.

# Real child-process adapter lifecycle, crash/timeout handling and server isolation.
add_library(server_extension_fixture SHARED server_extension_fixture.cpp)
target_include_directories(server_extension_fixture PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_executable(test_server_extension test_server_extension.cpp
    ${CMAKE_SOURCE_DIR}/src/game/server_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/game/server_extension.cpp
    ${CMAKE_SOURCE_DIR}/src/core/https_get.cpp)
target_include_directories(test_server_extension PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_server_extension SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_server_extension PRIVATE
    EXTENSION_HOST_PATH="$<TARGET_FILE:wowee_extension_host>"
    EXTENSION_FIXTURE_PATH="$<TARGET_FILE:server_extension_fixture>")
target_link_libraries(test_server_extension PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(test_server_extension PRIVATE ws2_32)
endif()
add_dependencies(test_server_extension wowee_extension_host server_extension_fixture)
add_test(NAME server_extension COMMAND test_server_extension)
register_test_target(test_server_extension)

# Signed M2 UV mapping and the actual Dalaran sculpture's reflection layers.
add_executable(test_m2_material test_m2_material.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/m2_loader.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_m2_material PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_material SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(test_m2_material PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_m2_material PRIVATE catch2_main)
wowee_test_link_glm(test_m2_material)
add_test(NAME m2_material COMMAND test_m2_material)
register_test_target(test_m2_material)

# ── test_realm_patch_sync ────────────────────────────────────
# Keeping a realm's patch directory in step with its announced list, against
# a faked transfer: resume, damage, overrun and a dropped connection.
add_executable(test_realm_patch_sync
    test_realm_patch_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/core/realm_patch_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/core/realm_patches.cpp
    ${TEST_COMMON_SOURCES}
)
target_include_directories(test_realm_patch_sync PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_include_directories(test_realm_patch_sync SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
)
target_link_libraries(test_realm_patch_sync PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
wowee_test_link_glm(test_realm_patch_sync)
add_test(NAME realm_patch_sync COMMAND test_realm_patch_sync)
register_test_target(test_realm_patch_sync)

# ── test_archive_layer ───────────────────────────────────────
# A realm's patch archives above the extracted data: which copy of a file is
# read, and a table read with the realm's over_ diff merged in. Links the
# pipeline library for the AssetManager; the MPQ half needs StormLib.
add_executable(test_archive_layer test_archive_layer.cpp)
target_include_directories(test_archive_layer PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_archive_layer SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_archive_layer PRIVATE catch2_main wowee_pipeline wowee_base)
wowee_test_link_glm(test_archive_layer)
if(STORMLIB_LIBRARY AND STORMLIB_INCLUDE_DIR)
    target_sources(test_archive_layer PRIVATE ${CMAKE_SOURCE_DIR}/src/pipeline/mpq_archive_layer.cpp)
    target_include_directories(test_archive_layer PRIVATE ${STORMLIB_INCLUDE_DIR})
    target_compile_definitions(test_archive_layer PRIVATE WOWEE_HAVE_STORMLIB)
    if(WOWEE_STORMLIB_SERR)
        target_compile_definitions(test_archive_layer PRIVATE WOWEE_STORMLIB_SERR=1)
    endif()
    wowee_link_stormlib(test_archive_layer)
endif()
add_test(NAME archive_layer COMMAND test_archive_layer)
register_test_target(test_archive_layer)

# ── test_warden_hmac ─────────────────────────────────────────
# The keyed-once HMAC-SHA1 used by the code-pattern search, against OpenSSL.
add_executable(test_warden_hmac
    test_warden_hmac.cpp
)
target_include_directories(test_warden_hmac PRIVATE
    ${TEST_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_include_directories(test_warden_hmac SYSTEM PRIVATE
    ${TEST_SYSTEM_INCLUDE_DIRS}
)
target_link_libraries(test_warden_hmac PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME warden_hmac COMMAND test_warden_hmac)
register_test_target(test_warden_hmac)

# ── test_rt_build_budget ─────────────────────────────────────
# How much ray-tracing geometry a frame builds, from measured GPU cost.
add_executable(test_rt_build_budget test_rt_build_budget.cpp)
target_include_directories(test_rt_build_budget PRIVATE ${TEST_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})
target_include_directories(test_rt_build_budget SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_rt_build_budget PRIVATE catch2_main)
add_test(NAME rt_build_budget COMMAND test_rt_build_budget)
register_test_target(test_rt_build_budget)

# ── The game, end to end ─────────────────────────────────────
# GameHarness: the real game from wowee_game, talking WotLK to a server that
# is the test - packets in, and what the game sent back, told the interface
# and announced on the event bus out. No renderer, no sound, no interface, no
# account. A test per domain follows a path all the way through, which is
# where state one side writes and another reads from a different copy shows.
add_library(wowee_game_harness STATIC game_harness.cpp)
target_include_directories(wowee_game_harness PUBLIC ${TEST_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(wowee_game_harness SYSTEM PUBLIC ${TEST_SYSTEM_INCLUDE_DIRS})
target_compile_definitions(wowee_game_harness PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(wowee_game_harness PUBLIC wowee_game catch2_main)
wowee_test_link_glm(wowee_game_harness)

foreach(_domain chat spell inventory quest social session account_data action_bar pet reputation realm_time weather)
    wowee_add_test(test_${_domain}_domain SOURCES test_${_domain}_domain.cpp)
    target_link_libraries(test_${_domain}_domain PRIVATE wowee_game_harness)
endforeach()

# The loading screen keeps its former terrain/warmup readiness criteria.
wowee_add_test(test_world_load_progress SOURCES test_world_load_progress.cpp)
wowee_add_test(test_job_group SOURCES test_job_group.cpp)

wowee_add_test(test_addon_event_queue SOURCES test_addon_event_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/addons/lua_handler_globals.cpp
    ${CMAKE_SOURCE_DIR}/src/addons/lua_event_args.cpp)
target_include_directories(test_addon_event_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_addon_event_queue PRIVATE wowee_game_harness lua51)
target_compile_definitions(test_addon_event_queue PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ── test_character_session ───────────────────────────────────
# What one character in the world leaves for the next: the real LFG bindings
# (whose ticks and folds used to be function statics) run in one character's
# Lua state and then in the next one's, the identity a relog's session takes
# from the game, and the saved variables keyed by it. The LFG bindings are the
# ones that link without the whole interface; the rest share their accessor.
wowee_add_test(test_character_session SOURCES test_character_session.cpp
    ${CMAKE_SOURCE_DIR}/src/addons/toc_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/addons/lua_lfg_api.cpp)
target_include_directories(test_character_session SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/extern/lua-5.1.5/src)
target_link_libraries(test_character_session PRIVATE wowee_game_harness lua51)
target_compile_definitions(test_character_session PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# ── The client, end to end ───────────────────────────────────
option(WOWEE_BUILD_ENTITY_ASSET_RELOAD_TEST "Build the native GPU entity cache reload regression" OFF)
if(WOWEE_BUILD_ENTITY_ASSET_RELOAD_TEST AND NOT ANDROID)
    add_executable(entity_asset_reload_test entity_asset_reload_main.cpp)
    target_link_libraries(entity_asset_reload_test PRIVATE wowee_client)
    wowee_test_link_glm(entity_asset_reload_test)
    wowee_apply_client_flags(entity_asset_reload_test)
    add_test(NAME entity_asset_reload COMMAND entity_asset_reload_test
             WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    set_tests_properties(entity_asset_reload PROPERTIES SKIP_RETURN_CODE 77)
    register_test_target(entity_asset_reload_test)
endif()

# ClientHarness: the client's own frame and world transitions (ClientLoop) over
# GameHarness's game, with a window that is a flag, input from a queue and no
# renderer. It links the client's archive, wowee_client, so nothing of the
# client is compiled again: the linker takes the members the harness reaches -
# the loop, the character's session and map, the spawner, the world loader and
# entry, the player's view and the Lua interface - and what those reference,
# ImGui included (the controls ask it who has the keyboard). No window is
# opened and no Vulkan device is made.
add_library(wowee_client_harness STATIC client_harness.cpp)
target_include_directories(wowee_client_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(wowee_client_harness PRIVATE WOWEE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(wowee_client_harness PUBLIC wowee_game_harness wowee_client)

wowee_add_test(test_client_transitions SOURCES test_client_transitions.cpp)
target_link_libraries(test_client_transitions PRIVATE wowee_client_harness)
# The client's objects are built for a link-time-optimised link in Release.
if(_ipo_supported AND WOWEE_ENABLE_LTO)
    set_property(TARGET test_client_transitions PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# What the interface's frames are told after layout - OnShow, OnHide,
# OnSizeChanged - through the character's own interface.
wowee_add_test(test_interface_frame_scripts SOURCES test_interface_frame_scripts.cpp)
target_link_libraries(test_interface_frame_scripts PRIVATE wowee_client_harness)
if(_ipo_supported AND WOWEE_ENABLE_LTO)
    set_property(TARGET test_interface_frame_scripts PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# ── wowee_loading_test_client ────────────────────────────────
option(WOWEE_BUILD_RENDER_CLIENT_PROBE "Build native interface/render lifecycle GPU probe" OFF)
wowee_add_test(test_frame_results SOURCES test_frame_results.cpp)
if(WOWEE_BUILD_RENDER_CLIENT_PROBE AND NOT ANDROID)
    add_executable(render_client_probe render_client_probe.cpp)
    target_link_libraries(render_client_probe PRIVATE wowee_client_objects)
    wowee_test_link_glm(render_client_probe)
    wowee_apply_client_flags(render_client_probe)
endif()

# The native loading fixture: the client's own objects with a main of its own
# that watches the frames (loading_frame_probe.cpp). Built with the client in
# every build, so a client change compiles once and both programs link; the
# ordinary wowee has none of this. Not a ctest: it needs a window, a GPU and a
# copied profile - run it through tools/test_loading_frames.ps1.
if(NOT ANDROID)
    add_executable(wowee_loading_test_client
        loading_test_client_main.cpp
        loading_frame_probe.cpp)
    target_include_directories(wowee_loading_test_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(wowee_loading_test_client PRIVATE wowee_client_objects)
    wowee_test_link_glm(wowee_loading_test_client)
    wowee_apply_client_flags(wowee_loading_test_client)
endif()

# ── test_cvar_store ──────────────────────────────────────────
# The CVars the game, the interface and the addons share: one name whatever
# its case, and a value that survives the client closing.
wowee_add_test(test_cvar_store
    SOURCES test_cvar_store.cpp
            ${TEST_COMMON_SOURCES}
            ${CMAKE_SOURCE_DIR}/src/core/cvar_store.cpp)

# ── test_m2_cull_grid ────────────────────────────────────────
# The M2 view and shadow passes reject whole columns of the world before
# testing instances; a column must never be rejected while an instance in it
# would have passed.
add_executable(test_m2_cull_grid test_m2_cull_grid.cpp ${CMAKE_SOURCE_DIR}/src/rendering/frustum.cpp)
target_include_directories(test_m2_cull_grid PRIVATE ${TEST_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})
target_include_directories(test_m2_cull_grid SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_cull_grid PRIVATE catch2_main)
wowee_test_link_glm(test_m2_cull_grid)
add_test(NAME m2_cull_grid COMMAND test_m2_cull_grid)
register_test_target(test_m2_cull_grid)

# ── The libraries' sources from their libraries, never a copy ─
# Keep this last, after every test is declared. A test that names a source one
# of these libraries builds has that source taken out of its own list and links
# the library instead. Each test used to compile what it needed itself: the
# packet layer was built 21 times over, about a third of a full build's work,
# and a new test copying its neighbour's list would bring that back.
set(_wowee_test_libraries
    wowee_game     WOWEE_GAME_SOURCES
    wowee_net      WOWEE_NET_SOURCES
    wowee_protocol WOWEE_PROTOCOL_SOURCES
    wowee_base     WOWEE_BASE_SOURCES
)
get_property(_wowee_test_targets GLOBAL PROPERTY WOWEE_ALL_TEST_TARGETS)
while(_wowee_test_libraries)
    list(POP_FRONT _wowee_test_libraries _library _sources_var)
    set(_library_paths "")
    foreach(_source IN LISTS ${_sources_var})
        list(APPEND _library_paths ${CMAKE_SOURCE_DIR}/${_source})
    endforeach()
    foreach(_test IN LISTS _wowee_test_targets)
        get_target_property(_test_sources ${_test} SOURCES)
        set(_own_sources "")
        set(_needs_library FALSE)
        foreach(_source IN LISTS _test_sources)
            if(_source IN_LIST _library_paths)
                set(_needs_library TRUE)
            else()
                list(APPEND _own_sources ${_source})
            endif()
        endforeach()
        if(_needs_library)
            set_property(TARGET ${_test} PROPERTY SOURCES ${_own_sources})
            target_link_libraries(${_test} PRIVATE ${_library})
        endif()
    endforeach()
endwhile()

wowee_add_test(test_frame_worker SOURCES test_frame_worker.cpp)
