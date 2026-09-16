# Limitations

This project is experimental and intended for local, supervised use. It has not been audited for unattended operation.

- A successful build or DLL smoke test does not prove ACSIL attachment, chart rendering, data correctness, or end-to-end runtime behavior.
- Process presence does not prove that Sierra is healthy, connected, current, or displaying the intended chart.
- File age and heartbeat checks are heuristics; clock changes, stalled processes, provider caching, and filesystem issues can mislead them.
- Wine adds a process and UI compatibility boundary. Window automation and native integrations may not behave like Windows.
- The local queue is not a remote API, authentication layer, multi-user service, or encrypted transport.
- Configuration mutations can still change or close charts and chartbooks. Use expected targets, idempotency keys, backups, and human review.
- Data freshness is not market-data entitlement validation and does not establish trading suitability.
- This software does not provide financial advice and does not execute orders.
