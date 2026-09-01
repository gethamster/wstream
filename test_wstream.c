/* Correctness test: build a file of known rows, then verify
 *   1. a mixed resident+streamed gather is byte-exact,
 *   2. an out-of-range index is rejected (ws_gather returns -1),
 *   3. concurrent gathers on one handle stay byte-exact (reentrancy). */
#include "wstream.h"

#include <fcntl.h>
#include <pthread.h>
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

/* Verify a gathered buffer against the known pattern. Returns mismatch count. */
static int verify(const unsigned char *dst, const uint32_t *idx, size_t count) {
    int bad = 0;
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < ROW_BYTES; j++)
            if (dst[i * ROW_BYTES + j] != expected_byte(idx[i], j)) bad++;
    return bad;
}

/* --- concurrency probe: many threads gathering the same random rows --- */
#define THREADS 8
#define GATHER  64
typedef struct { wstream *ws; int bad; } probe_t;

static void *probe(void *arg) {
    probe_t *p = arg;
    uint32_t idx[GATHER];
    unsigned char dst[GATHER * ROW_BYTES];
    for (int r = 0; r < 200; r++) {
        for (int i = 0; i < GATHER; i++)
            idx[i] = ((uint32_t)(r * 2654435761u + i * 40503u)) % N_ROWS;
        if (ws_gather(p->ws, idx, GATHER, dst) != 0) { p->bad++; continue; }
        p->bad += verify(dst, idx, GATHER);
    }
    return NULL;
}

int main(void) {
    const char *path = "/tmp/wstream_test.bin";

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

    /* 1. mixed resident + streamed gather, like a decode-step lookup */
    uint32_t idx[16] = {0, 5, 39999, 40000, 40001, 99999, 12345, 88888,
                        7, 40040, 3, 91000, 250, 65535, 40000, 99998};
    const size_t count = 16;
    ws_prefetch(ws, idx, count);
    unsigned char *dst = malloc(count * ROW_BYTES);
    memset(dst, 0xAB, count * ROW_BYTES);
    if (ws_gather(ws, idx, count, dst) != 0) {
        fprintf(stderr, "FAIL: gather returned error\n"); return 1;
    }
    int resident_seen = 0, stream_seen = 0;
    for (size_t i = 0; i < count; i++)
        ws_resident(ws, idx[i]) ? resident_seen++ : stream_seen++;
    int bad = verify(dst, idx, count);
    if (bad) { fprintf(stderr, "FAIL: %d mismatched bytes\n", bad); return 1; }
    free(dst);

    /* 2. out-of-range index must be rejected, not served silently */
    uint32_t oob[2] = { 10, N_ROWS };   /* second is past the end */
    unsigned char small[2 * ROW_BYTES];
    if (ws_gather(ws, oob, 2, small) == 0) {
        fprintf(stderr, "FAIL: out-of-range index was not rejected\n"); return 1;
    }

    /* 3. concurrent gathers on one handle stay correct (reentrancy) */
    pthread_t th[THREADS];
    probe_t pr[THREADS];
    for (int i = 0; i < THREADS; i++) {
        pr[i] = (probe_t){ ws, 0 };
        pthread_create(&th[i], NULL, probe, &pr[i]);
    }
    int concurrent_bad = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        concurrent_bad += pr[i].bad;
    }
    if (concurrent_bad) {
        fprintf(stderr, "FAIL: %d errors under concurrent gather\n", concurrent_bad);
        return 1;
    }

    ws_close(ws);
    unlink(path);

    printf("PASS: byte-exact (%d resident, %d streamed), out-of-range rejected, "
           "%d concurrent gathers clean\n", resident_seen, stream_seen,
           THREADS * 200);
    return 0;
}
