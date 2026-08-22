# AegisDB v0.5.5 — the MCP server starts again 🔌

A fix release for the **Claude Code integration**. If you install
`aegisdb-mcp` fresh — the `uvx aegisdb-mcp` path in the README and in the
`.mcp.json` that `aegisdb-init` writes — v0.5.4 and earlier fail to start the MCP
server. This release fixes that. **The C server is unchanged**: no API,
wire-protocol, or on-disk format changes, and there is nothing to migrate. If
your Claude Code memory already works, you are not affected.

## The break

`aegisdb-mcp` depended on `mcp>=1.0`, which now resolves to **mcp 2.0.0**. The
2.0 SDK removed `mcp.server.fastmcp` (FastMCP) in favour of
`mcp.server.mcpserver.MCPServer`, so the server's import failed and the process
exited immediately with:

```
[aegis-mcp] the 'mcp' package is required to run the server: pip install mcp
```

— which was doubly unhelpful, since `mcp` *was* installed. An existing
environment with an older `mcp` already pinned hid the problem completely, so it
only ever reproduced on a **new** install: precisely the first thing a new user
does.

## Fixes

- **`mcp>=1.0,<2`.** The cap restores a working install today. Porting
  `server.py` to the 2.x `MCPServer` API is separate work; the cap will be
  lifted in that change.
- **An honest error message.** The startup failure now distinguishes "the SDK
  isn't installed" from "the installed SDK is too new to provide FastMCP", and
  names the version it found instead of telling you to install what you have.

## First-contact CI gate

The break shipped in a release because nothing tested the path a new user takes:
the C suites never touch the container image, and the integration's contract
tests call `MemoryTools` in-process, so neither the installed console scripts nor
the MCP stdio protocol were exercised anywhere.

`make first-contact` (and the `first-contact` workflow) now walks the documented
quickstart from scratch on every PR — `docker run` the image, the README's
`aegisdb client` commands, the wire protocol from the *host*, a clean install,
`aegisdb-init`, the MCP server over stdio JSON-RPC, and finally the recall hook,
asserting that a memory saved through MCP comes back as injected context. A
second job runs the same walk against the **published** image and package, so a
broken release is caught by CI rather than by a new user.

## Operator notes

- **Upgrading the server is optional** — the binary is functionally identical to
  v0.5.4. Pull the new image if you want the version string to match.
- **Upgrading the integration is the point.** `uvx` resolves the new version
  automatically; a pinned setup wants `uvx aegisdb-mcp@0.5.5`, and an editable
  install wants a re-install to pick up the dependency cap.
- If you hit the old error, `pip install 'mcp<2'` fixes an existing environment
  in place.

*No migration required. MIT licensed. Built in C17, no runtime dependencies.*