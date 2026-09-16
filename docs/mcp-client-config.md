# MCP client configuration

This project is experimental local-use software and has not been audited for unattended use.

The server uses stdio. Replace every placeholder with a private, absolute path appropriate to your machine. Set `SIERRA_MCP_ROOT` explicitly to `<Sierra DataFilesFolder>/SierraMCP`, the fixed directory used by the bridge. Keep it out of shared repositories.

```json
{
  "mcpServers": {
    "sierra-mcp": {
      "command": "node",
      "args": ["<REPOSITORY>/server/dist/server.js"],
      "env": {
        "SIERRA_MCP_ROOT": "<SIERRA_DATA_FOLDER>/SierraMCP",
        "SIERRA_MCP_REQUEST_TIMEOUT_MS": "120000"
      }
    }
  }
}
```

On Wine, use the host Node executable plus the same physical `<Sierra DataFilesFolder>/SierraMCP` directory visible to the Wine process, or use the equivalent Wine command wrapper required by your environment. The bridge has no queue-root setting. Do not put credentials, broker settings, or account identifiers in this configuration.

Start with `sierra_status`, `sierra_diagnostics` (`capabilities` or `logs`), and `sierra_control` (`observe` or `health_check`). Treat `changed: true` as a report that a configuration mutation occurred, not as proof that Sierra rendered or behaved as intended.
