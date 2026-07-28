# AegisDB v0.5.6 — current MCP SDK, and the tests that were missing 🧪

A compatibility-and-testing release for the **Claude Code integration**. Nothing
in v0.5.5 is broken, so this is not urgent: take it if you want the integration
to coexist with the current MCP SDK, or if you pinned `mcp<2` by hand and would
like to drop the pin. **The C server is unchanged** — not just compatible, but
the same code as v0.5.4: no `src/`, `include/`, or `Dockerfile` changes, so the
published image differs only in its version string.

## MCP SDK 2.x support

v0.5.5 fixed the broken install by capping the SDK at `mcp<2`. That works, but
it fixes the problem by holding the SDK back — fine inside an isolated `uvx`
run, and a genuine conflict in any shared environment that also needs mcp 2.x.
The cap is now lifted properly:

- **`server.py` supports both SDK majors.** mcp 2.0 renamed `FastMCP` to
  `MCPServer` and moved it to `mcp.server.mcpserver`. For this binding that is
  the whole difference — the constructor, the `@tool()` decorator (same JSON
  Schema from the same type hints), `run()` over stdio, and the wire result for a
  dict-returning tool are identical on both. So the binding prefers 2.x and falls
  back to 1.x instead of forcing anyone pinned to 1.x to upgrade.
- **`mcp>=1.0,<3`.** The ceiling is deliberate and stays: an unbounded
  `mcp>=1.0` is exactly what let 2.0's rename break every fresh install in
  v0.5.4. It will be raised only after the next major is checked against a real
  server start.
- **`serverInfo.version` now reports the `aegisdb-mcp` version** on 2.x, whose
  own default is an empty string. (1.x has no such parameter and keeps reporting
  the SDK's version, as before.)

## The tests that were missing

Both of the above shipped a bug or went uncovered because the paths a user
actually takes were untested. That is what changed:

- **Both SDK majors are gated.** A fresh dependency resolve only ever exercises
  the newest major — which is how the 2.0 rename reached a release. The
  first-contact walk now runs once per major (`--mcp-spec` pins the SDK under
  test), starting a real server on each.
- **The integration's 137 tests now run in CI.** They never had: `ci.yml` covered
  the C server and the Prometheus exporter, and its pull-request path filter did
  not include `integrations/claude-code/**` at all, so a change confined to that
  tree triggered no test job. Both the mcp break and its fix landed with no
  coverage beyond the quickstart walk.
- **The suite is gated on both ends of `requires-python = ">=3.10"`.** The floor
  was an untested claim — the same shape of bug as the unbounded `mcp>=1.0`.
- **A missing binary now fails instead of skipping.** The contract and
  integration tests launch the server from `../../build` and skip themselves when
  it is absent, so two thirds of the suite could have quietly stopped running
  while CI stayed green.

## Operator notes

- **Upgrading the server is optional** — the binary is built from the same
  sources as v0.5.4 and v0.5.5. Pull the new image only if you want the version
  string to line up.
- **Upgrading the integration is the point.** `uvx` picks up the new version
  automatically; a pinned setup wants `uvx aegisdb-mcp@0.5.6`, and an editable
  install wants a re-install to widen the dependency range.
- **Either SDK major works.** If you are on mcp 1.x, nothing changes and there is
  nothing to do. If you pinned `mcp<2` yourself after v0.5.4, you can drop it.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*