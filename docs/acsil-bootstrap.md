# One-time ACSIL study bootstrap

The bridge study is intentionally attached by a human. It always uses `<Sierra DataFilesFolder>/SierraMCP` and has no queue-root input. Set the server's `SIERRA_MCP_ROOT` to that same directory. Use one bridge with `Controller Instance=Yes` in the controller chart. Any chart-targeted setting requires a non-controller bridge on each controlled chart.

1. Build the bridge study with Sierra Chart's ACSIL toolchain.
2. Open the intended controller chartbook manually in Sierra Chart.
3. Add the bridge study to the controller chart, set `Allow Configuration Changes=Yes` and `Controller Instance=Yes`, then enable it. The controller can bootstrap `sierra_studies.add`, or users may add the study manually.
4. Set the server's `SIERRA_MCP_ROOT` to `<Sierra DataFilesFolder>/SierraMCP` as resolved by the server. Do not configure a queue root in the study; no such input exists.
5. For every chart that needs chart-targeted settings, add and enable a bridge with `Allow Configuration Changes=Yes` and `Controller Instance=No`.
6. Confirm the study is calculating and that its heartbeat appears under the queue `state` area.
7. Run `health` and a read-only MCP call. Check the request ID and response before trying configuration changes.

If any identity or heartbeat check is ambiguous, stop and remove the study manually. Do not edit chartbook binaries to bypass attachment or identity checks.
