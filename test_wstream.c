/* Minimal correctness test: build a file of known rows, gather a mix of
 * resident + streamed indices, verify every byte. */
#include "wstream.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROW_BYTES 160u        /* mirrors the pMLX n-gram row width */
#define N_ROWS    100000u
#define RESIDENT  40000u      /* 40% wired, 60% streamed */

static unsigned char expected_byte(uint32_t row, size_t j) {
    return (unsigned char)((row * 2654435761u + (uint32_t)j) & 0xff);
}

int main(void) {
    const char *path = "/tmp/wstream_test.bin";

    /* write the file */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    unsigned char *row = malloc(ROW_BYTES);
    for (uint32_t i = 0; i < N_ROWS; i++) {
        for (size_t j = 0; j < ROW_BYTES; j++) row[j] = expected_byte(i, j);
        if (write(fd, row, ROW_BYTES) != (ssize_t)ROW_BYTES) { perror("write"); return 1; }
    }
    close(fd);
    free(row);

    wstream *ws = ws_open(path, ROW_BYTES, N_ROWS, RESIDENT, 8);
    if (!ws) { fprintf(stderr, "ws_open failed\n"); return 1; }

    /* a gather that mixes resident + streamed rows, like a decode-step lookup */
    uint32_t idx[16] = {0, 5, 39999, 40000, 40001, 99999, 12345, 88888,
                        7, 40040, 3, 91000, 250, 65535, 40000, 99998};
    const size_t count = 16;

    ws_prefetch(ws, idx, count);   /* hint the streamed ones */

    unsigned char *dst = malloc(count * ROW_BYTES);
    memset(dst, 0xAB, count * ROW_BYTES);
    ws_gather(ws, idx, count, dst);

    int bad = 0, resident_seen = 0, stream_seen = 0;
    for (size_t i = 0; i < count; i++) {
        ws_resident(ws, idx[i]) ? resident_seen++ : stream_seen++;
        for (size_t j = 0; j < ROW_BYTES; j++) {
            unsigned char got = dst[i * ROW_BYTES + j];
            unsigned char exp = expected_byte(idx[i], j);
            if (got != exp) {
                if (bad < 5)
                    fprintf(stderr, "MISMATCH row %u byte %zu: got %02x exp %02x\n",
                            idx[i], j, got, exp);
                bad++;
            }
        }
    }

    free(dst);
    ws_close(ws);
    unlink(path);

    if (bad) { fprintf(stderr, "FAIL: %d mismatched bytes\n", bad); return 1; }
    printf("PASS: %zu rows gathered byte-exact (%d resident, %d streamed)\n",
           count, resident_seen, stream_seen);
    return 0;
}
