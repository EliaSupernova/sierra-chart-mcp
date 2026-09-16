# Installation

## Requirements

Use a supported Sierra Chart installation, a C++ build environment for ACSIL, Node.js for the server and launcher, and an MCP client that supports local stdio servers. The bridge always uses `<Sierra DataFilesFolder>/SierraMCP`; keep that directory on a local filesystem with sufficient free space.

## Windows

1. Install Sierra Chart normally and note the private path to its executable and Data directory.
2. Build the ACSIL bridge using Sierra Chart's supported custom-study workflow.
3. Start Sierra Chart and complete the manual bootstrap in [`acsil-bootstrap.md`](acsil-bootstrap.md).
4. Install server dependencies, build the server, and run the launcher health check.
5. Add the stdio server to your MCP client using [`mcp-client-config.md`](mcp-client-config.md).

Do not paste real paths into public documentation. Use `<SIERRA_EXECUTABLE>`, `<SIERRA_DATA_FOLDER>`, and `<SIERRA_DATA_FOLDER>/SierraMCP` in examples.

## macOS or Linux via Wine

Sierra Chart is a Windows application. Run it under a supported Wine setup, then treat the Wine prefix, executable, and Data directory as private implementation details. The bridge writes to `<Sierra DataFilesFolder>/SierraMCP`. The MCP server must explicitly set `SIERRA_MCP_ROOT` to that same directory, using the path as it is visible to the server.

Validate each layer independently: Sierra process, queue directories, bridge heartbeat, data freshness, and MCP stdio round-trip. A Wine process being visible is not proof that its chart window is controllable or that the study is attached.

## Build and verification

```sh
npm --prefix server ci
npm --prefix server run build
npm --prefix server test
npm --prefix launcher test
make bridge-core-test
```

The optional DLL build requires a local ACSIL source directory and MinGW-compatible tools:

```sh
SIERRA_ACSIL_DIR=<SIERRA_ACS_SOURCE> \
SIERRA_MCP_CXX=<MINGW_CXX> \
SIERRA_MCP_OBJDUMP=<MINGW_OBJDUMP> \
./scripts/build-bridge.sh
```

Alternatively use `make dll-local` with `SIERRA_ACSIL_DIR` and a MinGW C++ compiler selected through `CXX`. The script can additionally run its Wine smoke check when `SIERRA_MCP_WINE`, `SIERRA_WINEPREFIX`, and `SIERRA_MCP_WINE_DLL_PATH` are set.

## Launcher configuration

Copy `launcher/config.sample.json` to a private file and replace placeholders. The launcher can check process presence, queue layout, bridge heartbeat age, data activity, and free space. Its explicit `open` action is gated by `--allow-action` and requires an executable. `restart` is intentionally not implemented; there is no automatic watchdog loop.
