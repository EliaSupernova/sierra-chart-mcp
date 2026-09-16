# Contributing

Thank you for helping improve Sierra Chart MCP. Contributions should make local Sierra configuration and diagnostics safer, clearer, or more reliable.

The project is experimental local-use software and is not audited for unattended use.

## Non-negotiable scope

This project is not an execution system. Do not add order entry, cancellation, position management, account access, broker connectivity, credential forwarding, or trading automation. Do not describe the project as affiliated with Sierra Chart.

## Before opening a change

- Read the README, safety model, and limitations.
- Keep examples generic and use placeholders.
- Remove private paths, symbols, chartbooks, screenshots, credentials, and strategy details from reports and fixtures.
- Explain platform assumptions, failure modes, and how the change was verified.

## Checks

Run the relevant server typecheck/build/tests and launcher tests. For bridge changes, run the available compile and smoke checks. A passing build does not prove study attachment or rendered Sierra behavior; document those separately when relevant.

## Pull requests

Use the pull-request template. Keep each PR focused, describe user-visible safety impact, and include exact verification commands and results. Never commit generated private configuration or runtime data.
