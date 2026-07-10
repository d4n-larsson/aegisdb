# Quickstart: AegisDB

Get a server running and store your first memory in a couple of minutes.

## Prerequisites

- Linux (primary target) with GCC 11+ or Clang 14+
- One of: CMake 3.20+ **or** GNU Make
- Python 3.8+ (optional, for the example client below)

No external libraries are required.

## Build

With CMake (canonical):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or with Make:

```bash
make
```

Either way the server binary is produced at `build/aegisdb`.

## Run the server

```bash
./build/aegisdb --data-dir ./data --port 9470 --embedding-dim 1024
```

Expected startup log:

```text
2026-06-28 12:00:00.000 INFO  [aegisdb] AegisDB 0.1.0 starting (log level: info)
2026-06-28 12:00:00.000 WARN  [aegisdb] no auth tokens configured; the server accepts unauthenticated requests from anyone who can reach the port
2026-06-28 12:00:00.000 INFO  [aegisdb] recovery complete: N records loaded
2026-06-28 12:00:00.000 INFO  [aegisdb] listening on 0.0.0.0:9470
2026-06-28 12:00:00.000 INFO  [aegisdb] data directory: ./data
```

The `WARN` line appears only because this example starts without
`--auth-token`/`--auth-token-file`; configure a token to remove it. Each line is
`<timestamp> <LEVEL> [aegisdb] <message>` on stderr. Set the
verbosity with `--log-level error|warn|info|debug` (or the `AEGISDB_LOG_LEVEL`
environment variable; the flag wins). `debug` adds per-connection and
per-request detail.

Common flags (see `--help` for the full list):

| Flag | Default | Notes |
|------|---------|-------|
| `--data-dir <path>` | `./data` | Where the append-only log and indexes live |
| `--port <n>` | `9470` | TCP listen port |
| `--embedding-dim <n>` | `384` | Vector length; must match the embeddings your client sends |
| `--log-level <level>` | `info` | `error`, `warn`, `info`, or `debug` (also `$AEGISDB_LOG_LEVEL`) |
| `--auth-token <token>` | — | Require this bearer token (repeatable) |
| `--auth-token-file <path>` | — | Load accepted tokens, one per line |

With no `--auth-token`/`--auth-token-file`, the server runs **without authentication**.
Tokens are sent in plaintext, so run the server behind an encrypted channel
(VPN, SSH tunnel, or TLS proxy) when auth is enabled. To encrypt the data on
disk, add `--encryption-key-file <path>` (mint a key with `aegisdb gen-key`) —
see the README's *Encryption at rest*.

## Talk to it with the built-in client

The `aegisdb` binary is also a client, so you don't need `nc` or hand-written
JSON. It prints the server's JSON reply and exits `0` on an ok response.

```bash
aegisdb client ping
aegisdb client put --type semantic --tags user,preference "User likes coffee"
aegisdb client get 1
aegisdb client search --tags user --top-k 10
aegisdb client stats
```

Connection settings come from `--host`/`--port`/`--token` or the environment
(`$AEGIS_HOST` / `$AEGIS_PORT` / `$AEGIS_TOKEN`, defaulting to `127.0.0.1` /
`9470` / none). Running in Docker? `docker exec aegisdb aegisdb client stats`.

To onboard a tenant, mint a token (the plaintext is shown once; the server
stores only its hash):

```bash
$ aegisdb gen-token --namespace acme --scope rw
sha256$… acme rw     # append this line to your --auth-token-file
token: 9f3c…         # set AEGIS_TOKEN to this on the client side
```

## Verify with netcat

If you'd rather speak the raw protocol, it is one JSON object per line over TCP.
`nc -q 1` sends a line and waits one second for the reply.

**Ping** (always works, even with auth enabled):

```bash
echo '{"operation":"ping"}' | nc -q 1 localhost 9470
```

```json
{"ok":true,"version":"0.1.0","phase":4}
```

**Insert an episodic memory**:

```bash
echo '{"operation":"insert","type":"episodic","tags":["user","preference"],"data":"User likes coffee"}' | nc -q 1 localhost 9470
```

Returns `"ok":true` with the assigned `id` and timestamps.

**Get by ID** (replace `42` with the returned id):

```bash
echo '{"operation":"get","id":42}' | nc -q 1 localhost 9470
```

**Search by tag**:

```bash
echo '{"operation":"search","tags":["user"],"match":"all","top_k":10}' | nc -q 1 localhost 9470
```

**Search by time range**:

```bash
echo '{"operation":"search","start_time":0,"end_time":9999999999999,"top_k":10}' | nc -q 1 localhost 9470
```

When auth is enabled, add a `"token"` field to every request except `ping`:

```bash
echo '{"operation":"get","id":42,"token":"YOUR_TOKEN"}' | nc -q 1 localhost 9470
```

## Verify with Python

```python
import socket, json

def request(payload: dict) -> dict:
    with socket.create_connection(("localhost", 9470)) as s:
        s.sendall((json.dumps(payload) + "\n").encode())
        return json.loads(s.recv(65536).decode())

print(request({"operation": "ping"}))
print(request({
    "operation": "insert",
    "type": "episodic",
    "tags": ["demo"],
    "data": "Hello from quickstart",
}))
```

## Test crash recovery

The log is the source of truth — indexes rebuild from it on startup.

1. Insert several records and note their ids.
2. Kill the server hard: `kill -9 <pid>`.
3. Restart it with the same `--data-dir`.
4. `get` each id — every record returns intact.

## Back up and restore

The log is append-only, so a backup is a consistent copy of its durable prefix.
Take one online, without stopping the server, via the admin-only `snapshot` op:

```sh
echo '{"operation":"snapshot","name":"nightly","token":"<admin-token>"}' \
  | nc -q1 localhost 9470
# -> writes ./data/snapshots/nightly/ (memory.log, metadata.db, manifest.json)
```

Restore it into an **empty** data dir with the one-shot `--restore` mode, then
start the server normally — recovery rebuilds every index from the log:

```sh
aegisdb --restore ./data/snapshots/nightly --data-dir ./restored --embedding-dim 1024
aegisdb --data-dir ./restored --embedding-dim 1024
```

`--embedding-dim` must match the snapshot (recorded in its `manifest.json`);
`--restore` refuses to overwrite an existing database. See the
[wire-protocol reference](wire-protocol.md#snapshot) for the full contract.

## Where to next

- [Wire protocol reference](wire-protocol.md) — every operation, field, and error code.
- [Claude Code integration](../integrations/claude-code/) — an MCP server and hooks
  that give Claude Code long-term memory backed by AegisDB.