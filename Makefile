.PHONY: help test test-server test-launcher bridge-core-test dll-local

CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror
BUILD_DIR ?= build

help:
	@printf '%s\n' 'make test           Run server and launcher tests' 'make bridge-core-test  Build/run portable C++ core tests' 'make dll-local       Optional local DLL build; requires SIERRA_ACSIL_DIR and a MinGW C++ compiler'

test: test-server test-launcher bridge-core-test

test-server:
	npm --prefix server ci
	npm --prefix server test

test-launcher:
	npm --prefix launcher test

bridge-core-test:
	@mkdir -p "$(BUILD_DIR)"
	$(CXX) $(CXXFLAGS) -Ibridge tests/bridge_core_test.cpp bridge/BridgeCore.cpp -o "$(BUILD_DIR)/bridge_core_test"
	"$(BUILD_DIR)/bridge_core_test"

# Optional proprietary build. SIERRA_ACSIL_DIR must point to a local ACS_Source
# directory; this target is intentionally absent from the default test graph.
dll-local:
	@test -n "$(SIERRA_ACSIL_DIR)" || (printf '%s\n' 'SIERRA_ACSIL_DIR is required for dll-local' >&2; exit 2)
	@mkdir -p bin
	$(CXX) -std=c++17 -Wall -Wextra -Werror -shared -static -static-libgcc -static-libstdc++ -isystem "$(SIERRA_ACSIL_DIR)" -Ibridge bridge/BridgeCore.cpp bridge/SierraAdapter.cpp bridge/SierraMCPBridge.cpp bridge/BridgeExports.cpp -o bin/SierraMCPBridge_64.dll
