/* AegisDB core type definitions (T006). */
#ifndef AEGISDB_TYPES_H
#define AEGISDB_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Filesystem path sizing. Derived path buffers and path-holding struct members
 * use AEGIS_PATH_MAX (matches Linux PATH_MAX); the configured data directory is
 * capped well below it (AEGIS_DATA_DIR_MAX) so appending a fixed suffix onto it
 * can never overflow a AEGIS_PATH_MAX buffer. AEGIS_IO_BUF_SIZE is the stream
 * copy/scan window used by log/db/restore. */
#define AEGIS_PATH_MAX 4096
#define AEGIS_DATA_DIR_MAX 1024
#define AEGIS_IO_BUF_SIZE 65536

/* Three-way compare (-1/0/+1) without branches; type-generic over any operands
 * that support < and >. Both arguments are evaluated twice, so pass simple
 * lvalues/values with no side effects (all current callers pass locals). */
#define AEGIS_CMP3(x, y) (((x) > (y)) - ((x) < (y)))

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