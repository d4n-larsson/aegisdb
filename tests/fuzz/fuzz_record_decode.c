/* Fuzz target: the binary log record codec (record.c).
 *
 * record_decode parses attacker-influenceable bytes — a malicious --restore
 * snapshot, a tampered append-only log, or a replication stream frame (no CRC
 * gate on the wire). A heap overflow here was the one CRITICAL finding of the
 * 2026-07-09 security review (embedding sizing overflow, fixed in PR#115 and
 * pinned by test_decode_rejects_embedding_overflow). This target keeps that
 * whole surface under continuous coverage-guided fuzzing.
 *
 * Contract: record_decode fills *out on success (0) and leaves nothing to free
 * on failure (-1). On success we also re-encode, exercising record_encode with
 * decoder-produced field sizes, then free everything so ASan flags any leak or
 * use-after-free.
 *
 * Built two ways (see Makefile): under libFuzzer (`make fuzz`) libFuzzer
 * supplies main() and drives this; for deterministic corpus replay
 * (`make fuzz-regress`) standalone_main.c supplies main().
 */
#include <stdint.h>
#include <stdlib.h>

#include "aegisdb/record.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    MemoryRecord out;
    record_init(&out);
    if (record_decode(data, size, &out) != 0)
        return 0; /* rejected malformed input — the common, intended path */

    /* Round-trip the accepted record: encode from the decoder's field sizes,
     * which is where an overflow slipped through before. */
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (record_encode(&out, &enc, &enc_len) == 0)
        free(enc);

    record_free(&out);
    return 0;
}