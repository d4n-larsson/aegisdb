/* 64-bit integer hash mixer, shared by the open-addressing maps in the
 * hash / semantic / HNSW indexes. */
#ifndef AEGISDB_HASH_MIX_H
#define AEGISDB_HASH_MIX_H

#include <stdint.h>

/* MurmurHash3 fmix64 finalizer: maps an id to a well-distributed 64-bit hash. */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

#endif /* AEGISDB_HASH_MIX_H */