# Sierra MCP v2 launcher/watchdog

Dependency-free, read-only-by-default diagnostics for native Windows and Wine-hosted Sierra Chart. It never reads or forwards credentials.

```sh
node cli.js status --config ./config.json
node cli.js health --data /path/to/Data --queue-root /path/to/Data/SierraMCP
node cli.js watch-once --data /path/to/Data
```

`health` exits 1 when unhealthy. Checks cover Sierra process presence, queue directories, heartbeat freshness, data-folder free bytes, and recursive `.scid` activity. `open` is gated by `--allow-action` and requires an explicit `--executable`. `restart` is intentionally not implemented, and there is no automatic watchdog restart loop.

Copy `config.sample.json` to a private, local config and override paths with CLI flags or `SIERRA_MCP_*` environment variables. The sample contains placeholders only.

Tests use Node's built-in test runner and fake adapters/temp directories:

```sh
npm test
```
