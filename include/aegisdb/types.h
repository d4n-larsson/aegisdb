/* AegisDB core type definitions (T006). */
#ifndef AEGISDB_TYPES_H
#define AEGISDB_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Memory classes. Values are stable: persisted in the log encoding. */
typedef enum {
    MEM_WORKING = 0,  /* RAM only, overwritable/evictable */
    MEM_EPISODIC = 1, /* Disk, immutable after insert */
    MEM_SEMANTIC = 2  /* Disk, updateable (new log entry supersedes) */
} MemoryType;

/* Parse a wire-protocol type string ("working"|"episodic"|"semantic").
 * Returns 0 on success and writes *out; returns -1 on unknown value. */
int memory_type_from_string(const char *s, MemoryType *out);

/* Stable lowercase name for a MemoryType, or "unknown". */
const char *memory_type_to_string(MemoryType t);

#endif /* AEGISDB_TYPES_H */