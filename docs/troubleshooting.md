# Troubleshooting

Diagnose from the bottom up and record the exact command, exit code, and timestamp.

## No response from the MCP server

Confirm the built server entry point, Node.js version, private `SIERRA_MCP_ROOT`, and MCP client's stdio logs. Run the server's typecheck/build/tests, then try a read-only status call. Check that the queue directories exist and are writable by the intended processes.

## Bridge heartbeat is stale or missing

Confirm Sierra is running, one controller study is attached with `Controller Instance=Yes`, and any chart-targeted chart has its own non-controller bridge study. The bridge always writes to `<Sierra DataFilesFolder>/SierraMCP`; confirm the server's `SIERRA_MCP_ROOT` points to that same directory. Inspect `state/bridge-heartbeat`, then check filesystem free space and permissions. Reattach the study manually if identity is uncertain.

## Data appears stale

Check Sierra's own connection and data status, then compare the relevant data-file activity against the launcher's configured age threshold. A running process is not enough. Avoid interpreting stale data as a strategy or market conclusion.

## Configuration change was rejected

Verify the target chartbook and chart number, expected identity fields, idempotency key, and strict input types. Read the returned warnings and request ID. Do not retry a mutation blindly; determine whether it was applied before retrying.

## Wine-specific symptoms

Separate host-path visibility, Wine-prefix access, Sierra process state, ACSIL study calculation, queue activity, and MCP transport. A visible Wine window or loaded DLL alone does not prove the complete path.

## Privacy-safe reports

Redact private paths, symbols, chartbook names, screenshots, strategy details, credentials, account data, and raw logs unless a private security report specifically requires them.
