#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
: "${SIERRA_ACSIL_DIR:?Set SIERRA_ACSIL_DIR to the Sierra ACS_Source directory}"
BRIDGE_DIR="${SIERRA_MCP_BRIDGE_DIR:-$REPO_ROOT/bridge}"
TEST_DIR="${SIERRA_MCP_TEST_DIR:-$REPO_ROOT/tests}"
OUTPUT_DIR="${SIERRA_MCP_OUTPUT_DIR:-$REPO_ROOT/bin}"
CXX="${SIERRA_MCP_CXX:-${CXX:-x86_64-w64-mingw32-g++}}"
HOST_CXX="${SIERRA_MCP_HOST_CXX:-${HOST_CXX:-c++}}"
OBJDUMP="${SIERRA_MCP_OBJDUMP:-${OBJDUMP:-x86_64-w64-mingw32-objdump}}"
WINE="${SIERRA_MCP_WINE:-${WINE:-}}"
WINEPREFIX_VALUE="${SIERRA_WINEPREFIX:-${WINEPREFIX:-}}"
WINE_DLL_PATH="${SIERRA_MCP_WINE_DLL_PATH:-}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sierra-mcp-bridge.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
mkdir -p "$OUTPUT_DIR"
OWNED=("$BRIDGE_DIR/BridgeCore.h" "$BRIDGE_DIR/BridgeCore.cpp" "$BRIDGE_DIR/SierraAdapter.h" "$BRIDGE_DIR/SierraAdapter.cpp" "$BRIDGE_DIR/SierraMCPBridge.cpp" "$BRIDGE_DIR/BridgeExports.cpp")
"$HOST_CXX" -std=c++17 -Wall -Wextra -Werror -I"$BRIDGE_DIR" "$TEST_DIR/bridge_core_test.cpp" "$BRIDGE_DIR/BridgeCore.cpp" -o "$BUILD_DIR/bridge_core_test"
"$BUILD_DIR/bridge_core_test"
if rg -n '(BuyEntry|BuyOrder|SellEntry|SellOrder|GetTradePosition|GetTradeAccount|TradeAccount|SetCurrentDataTradingService|SendOrdersToTradeService|SubmitOrder|ModifyOrder|CancelOrder|DTCClient)' "${OWNED[@]}"; then echo "forbidden Sierra call name found in owned bridge source" >&2; exit 1; fi
echo "forbidden_call_scan: PASS"
"$CXX" -std=c++17 -Wall -Wextra -Werror -shared -static -static-libgcc -static-libstdc++ -isystem "$SIERRA_ACSIL_DIR" -I"$BRIDGE_DIR" "$BRIDGE_DIR/BridgeCore.cpp" "$BRIDGE_DIR/SierraAdapter.cpp" "$BRIDGE_DIR/SierraMCPBridge.cpp" "$BRIDGE_DIR/BridgeExports.cpp" -o "$OUTPUT_DIR/SierraMCPBridge_64.dll"
echo "dll_build: PASS"
PE_REPORT="$BUILD_DIR/dll-headers.txt"
"$OBJDUMP" -p "$OUTPUT_DIR/SierraMCPBridge_64.dll" > "$PE_REPORT"
if rg -i "DLL Name: (libwinpthread|libstdc\\+\\+|libgcc)" "$PE_REPORT"; then echo "non-system runtime DLL dependency found" >&2; exit 1; fi
for symbol in scdll_DLLName scdll_DLLVersion scsf_SierraMCPBridge; do rg -q "$symbol" "$PE_REPORT"; done
echo "dll_dependency_and_export_scan: PASS"
"$CXX" -std=c++17 -Wall -Wextra -Werror -static -static-libgcc -static-libstdc++ -isystem "$SIERRA_ACSIL_DIR" "$TEST_DIR/bridge_dll_smoke.cpp" -o "$BUILD_DIR/bridge_dll_smoke.exe"
echo "smoke_build: PASS"
if [[ -n "$WINE" && -n "$WINEPREFIX_VALUE" && -n "$WINE_DLL_PATH" ]]; then WINEPREFIX="$WINEPREFIX_VALUE" WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 "$WINE" "$BUILD_DIR/bridge_dll_smoke.exe" "$WINE_DLL_PATH"; else echo "wine_smoke: SKIP (set SIERRA_MCP_WINE, SIERRA_WINEPREFIX, and SIERRA_MCP_WINE_DLL_PATH to enable)"; fi
