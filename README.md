# Sierra Chart MCP

[![CI](https://github.com/EliaSupernova/sierra-chart-mcp/actions/workflows/ci.yml/badge.svg)](https://github.com/EliaSupernova/sierra-chart-mcp/actions/workflows/ci.yml)

> **Status:** Experimental, local-use software. It has not been audited for unattended use.

Sierra Chart MCP is a local Model Context Protocol server and launcher for **configuration and diagnostics only**. It lets an MCP client observe Sierra Chart, inspect chartbooks/charts/studies/drawings/data, read diagnostic information, and apply narrowly validated configuration changes through a local bridge.

> **Safety boundary:** This project has no order execution, trading, position, account, credential, or broker-control features. It is not affiliated with, endorsed by, or sponsored by Sierra Chart. Sierra Chart is a trademark of its respective owner.

## Architecture

```text
MCP client (stdio)
        |
   sierra-mcp server
        |
 local atomic queue (inbox/outbox/processing/archive/state)
        |
 ACSIL bridge study inside Sierra Chart
        |
 Sierra Chart runtime
```

The server validates tool inputs, assigns request IDs, filters restricted response fields, and waits for a matching bridge response. The bridge study always uses `<Sierra DataFilesFolder>/SierraMCP`; it has no queue-root input. Set `SIERRA_MCP_ROOT` in the server to that exact directory. Use one `Controller Instance=Yes` bridge in the controller chart. Any chart-targeted setting requires a non-controller bridge on each controlled chart.

## Supported tools

The public tool surface includes `sierra_status`, `sierra_chartbooks`, `sierra_charts`, `sierra_studies`, `sierra_drawings`, `sierra_data`, `sierra_diagnostics`, and `sierra_control`. Read operations are the default. Mutating operations require explicit targets, expected chartbook/chart identity where applicable, and idempotency keys.

## Quick start

1. Install Sierra Chart and the supported build prerequisites for your platform.
2. Build or obtain the ACSIL bridge study and add it manually to a controller chart.
3. Build the server and launcher with the repository instructions in [`docs/installation.md`](docs/installation.md).
4. Point your MCP client at the generated server entry point using placeholders as shown in [`docs/mcp-client-config.md`](docs/mcp-client-config.md).
5. Run the health checks and confirm the bridge heartbeat before using configuration tools.

Read the complete guides:

- [`docs/installation.md`](docs/installation.md) — Windows and Wine setup.
- [`docs/acsil-bootstrap.md`](docs/acsil-bootstrap.md) — one-time manual study bootstrap.
- [`docs/mcp-client-config.md`](docs/mcp-client-config.md) — client configuration examples.
- [`docs/safety-and-privacy.md`](docs/safety-and-privacy.md) — threat boundaries, data handling, and privacy.
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — evidence-led diagnosis.
- [`docs/limitations.md`](docs/limitations.md) — what this project does not prove or provide.

## Development

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Contributions must preserve the configuration/diagnostics-only scope and must not add order execution or account-control paths.

## License

MIT. See [`LICENSE`](LICENSE).
