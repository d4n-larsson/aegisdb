/* Standalone driver for the fuzz targets, so the corpus and known crashers can
 * be replayed WITHOUT libFuzzer (hence under any compiler, e.g. gcc in CI's
 * per-PR gate). Each argument is a file; its bytes are fed once to the target's
 * LLVMFuzzerTestOneInput. A crash means a non-zero exit (or a sanitizer abort),
 * which is exactly what the regression gate checks for.
 *
 * Mirrors the well-known StandaloneFuzzTargetMain shipped with LLVM.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "fuzz-replay: cannot open %s\n", argv[i]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0) {
            fprintf(stderr, "fuzz-replay: cannot size %s\n", argv[i]);
            fclose(f);
            return 1;
        }
        uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, got);
        free(buf);
        fprintf(stderr, "fuzz-replay: ok %s (%zu bytes)\n", argv[i], got);
    }
    return 0;
}