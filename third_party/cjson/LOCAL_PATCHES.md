# Local patches to vendored cJSON

This copy of cJSON carries small local modifications. When updating cJSON to a
newer upstream release, re-apply each patch below.

## 1. Thread-local `global_error` (issue #41)

**File:** `cJSON.c`, declaration of `global_error` (just above `cJSON_GetErrorPtr`).

Upstream declares the parse-error record as a process-global:

```c
static error global_error = { NULL, 0 };
```

AegisDB parses requests concurrently on worker threads, and every
`cJSON_ParseWithLength*` writes `global_error` — an unsynchronized data race
(flagged by ThreadSanitizer). The patch makes it thread-local via a portable
`CJSON_THREAD_LOCAL` qualifier (`_Thread_local` on C11+, `__thread` on GCC,
`__declspec(thread)` on MSVC, empty fallback otherwise):

```c
static CJSON_THREAD_LOCAL error global_error = { NULL, 0 };
```

This removes the race and keeps `cJSON_GetErrorPtr()` correct per-thread.