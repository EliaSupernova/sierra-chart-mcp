# Safety model and privacy

This is experimental, local-use software and has not been audited for unattended use.

## Safety model

The server is configuration/diagnostics-only. It validates strict schemas, requires expected-target fields and idempotency keys for mutations, correlates responses by request ID, filters restricted response fields, and uses an atomic local queue at `<Sierra DataFilesFolder>/SierraMCP`. The bridge is manually attached so a person can verify runtime identity; one instance is the controller and chart-targeted settings require a non-controller instance on each controlled chart.

These controls reduce accidental changes; they do not make an untrusted machine, chartbook, plugin, or MCP client safe. Review requests and responses, use private filesystem permissions, and run only software you trust.

## Privacy

The project may access Sierra status, chartbook metadata, chart configuration, study/drawing state, data freshness, and diagnostic logs needed for a requested operation. Those materials can reveal sensitive activity. Store them locally, minimize retention, redact before sharing, and never include credentials or private paths in issues or examples.

No claim is made that the project provides anonymization, encryption at rest, multi-user isolation, or remote-service security. If you expose the queue or MCP process beyond the local machine, you own the authentication, authorization, transport, and network controls.

## Explicit exclusions

There is no order execution, order routing, trading automation, account access, position control, credential handling, or affiliation with Sierra Chart.
