# Security policy

## Scope

Sierra Chart MCP is a local configuration and diagnostics bridge. It must never execute orders or expose trading, position, account, broker, credential, or DTC controls. The MCP server runs over stdio and communicates with the ACSIL study through a local atomic queue.

## Reporting a vulnerability

Please do not open a public issue for a security-sensitive report. Use the repository's private security reporting channel, if configured, or contact the maintainers privately with:

- affected version and platform;
- minimal reproduction steps;
- expected and observed behavior;
- logs with credentials, personal data, and private paths removed.

We will acknowledge reports when practicable, investigate reproducibly, and coordinate disclosure with the reporter. Do not test against systems you do not own or have permission to assess.

## Operator guidance

Keep the queue directory private, review every configuration change, use least-privilege filesystem permissions, and do not place secrets in chartbook names, tool arguments, logs, or issue reports. Treat Sierra data and diagnostic logs as potentially sensitive.
