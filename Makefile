# Shortcuts for the development build. The build itself is CMake; this only
# spells out the commands that are easy to get wrong.
#
#   make dev        configure (once) and build the client into build-dev
#   make run-dev    build, then run the client with the game data it needs
#   make submodules fetch the two submodules the client cannot build without
#
# The dev build is the "dev" preset of CMakePresets.json: RelWithDebInfo, no
# link-time optimisation. The preset asks for Ninja; without Ninja the same
# settings are configured with Makefiles instead.

BUILD_DIR := build-dev
JOBS      ?= $(shell sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# The client reads its game data from WOW_DATA_PATH, or from Data/ beside the
# binary - and build-dev/bin/Data is the checkout's Data/, which holds no
# extraction. The asset builder writes the extraction to the per-user folder,
# so that is the default here. Set WOW_DATA_PATH to use another one. Server
# profiles (server_extensions/) are looked for in the same folder.
ifeq ($(shell uname -s),Darwin)
WOW_DATA_PATH ?= $(HOME)/Library/Application Support/Wowee/Data
else
WOW_DATA_PATH ?= $(or $(XDG_DATA_HOME),$(HOME)/.local/share)/wowee/Data
endif

# OpenSSL 4 from Homebrew is not on the compiler's default paths on macOS.
OPENSSL_ROOT := $(shell brew --prefix openssl@4 2>/dev/null)
CMAKE_EXTRA  := $(if $(OPENSSL_ROOT),-DOPENSSL_ROOT_DIR=$(OPENSSL_ROOT))

.PHONY: dev run-dev submodules

$(BUILD_DIR)/CMakeCache.txt:
	@if command -v ninja >/dev/null 2>&1; then \
		cmake --preset dev $(CMAKE_EXTRA); \
	else \
		cmake -S . -B $(BUILD_DIR) -G "Unix Makefiles" \
			-DCMAKE_BUILD_TYPE=RelWithDebInfo -DWOWEE_ENABLE_LTO=OFF -DWOWEE_BUILD_TESTS=ON \
			$(CMAKE_EXTRA); \
	fi

dev: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target wowee -j $(JOBS)

run-dev: dev
	@test -f "$(WOW_DATA_PATH)/expansions/wotlk/manifest.json" || \
		{ echo "No extracted game data in $(WOW_DATA_PATH): set WOW_DATA_PATH or run the asset builder"; exit 1; }
	cd $(BUILD_DIR)/bin && WOW_DATA_PATH="$(WOW_DATA_PATH)" ./wowee

submodules:
	git submodule update --init extern/imgui extern/vk-bootstrap
