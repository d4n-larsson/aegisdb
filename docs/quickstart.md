# Quickstart: AegisDB


## Prerequisites

- Linux (primary target) with GCC 11+ or Clang 14+
- CMake 3.20+
- `make` or `ninja`
- Python 3.8+ (optional, for integration test scripts)

## Build (once source is implemented)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

## Start the Server

```bash
./build/aegisdb --data-dir ./data --port 9470
```

Expected startup log:

```text
[aegisdb] listening on 0.0.0.0:9470
[aegisdb] data directory: ./data
[aegisdb] recovery complete: N records loaded
```

## Verify with netcat

**Ping**:

```bash
echo '{"operation":"ping"}' | nc -q 1 localhost 9470
```

Expected:

```json
{"ok":true,"version":"0.1.0","phase":1}
```

**Insert episodic memory**:

```bash
echo '{"operation":"insert","type":"episodic","tags":["user","preference"],"data":"User likes coffee"}' | nc -q 1 localhost 9470
```

Expected: `ok: true` with assigned `id` and timestamps.

**Get by ID** (replace `42` with returned id):

```bash
echo '{"operation":"get","id":42}' | nc -q 1 localhost 9470
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

## Test Crash Recovery

1. Insert several records.
2. Kill the server (`kill -9 <pid>`).
3. Restart `./build/aegisdb`.
4. `get` each ID — all records MUST return intact (SC-003, FR-005).

## Phase 2+ Commands (after implementation)

**Time-range search**:

```bash
echo '{"operation":"search","start_time":0,"end_time":9999999999999,"top_k":10}' | nc -q 1 localhost 9470
```

**Tag search**:

```bash
echo '{"operation":"search","tags":["user"],"match":"all","top_k":10}' | nc -q 1 localhost 9470
```

## Project Layout Reference

```text
src/           # C source (server, storage, protocol, query)
include/       # Public headers
tests/         # unit/, integration/, contract/
data/          # Runtime data (gitignored)
```

## Next Steps

- Run `/speckit-tasks` to generate implementation tasks from this plan.
- Ratify project constitution via `/speckit-constitution` before large-scale implementation.
