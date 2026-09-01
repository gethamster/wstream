/* Microbench: the load-bearing claim is "pread pool beats mmap page-faults on
 * the first touch of a non-resident row." This measures both paths on the same
 * file and prints per-row latency + throughput so the README number is
 * reproducible instead of asserted.
 *
 *   make bench                       # default: 4 KiB rows, ~1 GiB file, 8 threads
 *   ./bench_wstream [row_bytes] [n_rows] [threads] [gather] [iters]
 *
 * CACHE STATE MATTERS. A freshly written file is warm in the page cache, so
 * both paths read from RAM and you measure memory bandwidth, not the SSD fault.
 * For the cold numbers the README cites, drop caches between build and run:
 *   macOS:  sudo purge
 *   Linux:  sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
 * The bench prints which regime it thinks it's in.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include "wstream.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* deterministic LCG so both paths gather the same index stream */
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

int main(int argc, char **argv) {
    size_t row_bytes = argc > 1 ? strtoul(argv[1], 0, 10) : 4096;
    size_t n_rows    = argc > 2 ? strtoul(argv[2], 0, 10) : (1ull << 30) / 4096;
    int    threads   = argc > 3 ? atoi(argv[3]) : 8;
    size_t gather    = argc > 4 ? strtoul(argv[4], 0, 10) : 32;
    int    iters     = argc > 5 ? atoi(argv[5]) : 2000;

    const char *path = "/tmp/wstream_bench.bin";
    size_t fsize = n_rows * row_bytes;
    printf("file %.2f GiB  |  %zu rows x %zu B  |  %d threads  |  "
           "%zu rows/gather x %d iters\n\n",
           fsize / 1073741824.0, n_rows, row_bytes, threads, gather, iters);

    /* write the file */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    unsigned char *rowbuf = malloc(row_bytes);
    memset(rowbuf, 0x5a, row_bytes);
    for (size_t i = 0; i < n_rows; i++)
        if (write(fd, rowbuf, row_bytes) != (ssize_t)row_bytes) { perror("write"); return 1; }
    fsync(fd);
    close(fd);
    free(rowbuf);

    double *lat = malloc(iters * sizeof(double));
    unsigned char *dst = malloc(gather * row_bytes);
    uint32_t *idx = malloc(gather * sizeof(uint32_t));

    /* ---- path A: ws_gather (pread pool), no resident prefix ---- */
    wstream *ws = ws_open(path, row_bytes, n_rows, 0, threads);
    if (!ws) { fprintf(stderr, "ws_open failed\n"); return 1; }
    uint32_t s = 0x1234567u;
    double first = 0;
    for (int it = 0; it < iters; it++) {
        for (size_t g = 0; g < gather; g++) idx[g] = lcg(&s) % n_rows;
        double t0 = now_ms();
        if (ws_gather(ws, idx, gather, dst) != 0) { fprintf(stderr, "gather err\n"); return 1; }
        lat[it] = now_ms() - t0;
        if (it == 0) first = lat[it];
    }
    ws_close(ws);

    qsort(lat, iters, sizeof(double), cmp_double);
    double sum = 0; for (int i = 0; i < iters; i++) sum += lat[i];
    double mean = sum / iters, p50 = lat[iters/2], p99 = lat[(int)(iters*0.99)];
    double per_row_us = mean / gather * 1000.0;
    double gbps = (gather * row_bytes) / (mean / 1e3) / 1e9;
    printf("ws_gather (pread pool)\n");
    printf("  first gather : %8.3f ms  (cold if caches were purged)\n", first);
    printf("  mean gather  : %8.3f ms   p50 %.3f  p99 %.3f\n", mean, p50, p99);
    printf("  per row      : %8.3f us\n", per_row_us);
    printf("  throughput   : %8.2f GB/s\n\n", gbps);

    /* ---- path B: mmap first-touch ---- */
    fd = open(path, O_RDONLY);
    unsigned char *map = mmap(0, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    s = 0x1234567u;                         /* same index stream as path A */
    double mfirst = 0;
    for (int it = 0; it < iters; it++) {
        for (size_t g = 0; g < gather; g++) idx[g] = lcg(&s) % n_rows;
        double t0 = now_ms();
        for (size_t g = 0; g < gather; g++)
            memcpy(dst + g * row_bytes, map + (size_t)idx[g] * row_bytes, row_bytes);
        lat[it] = now_ms() - t0;
        if (it == 0) mfirst = lat[it];
    }
    munmap(map, fsize);
    close(fd);

    qsort(lat, iters, sizeof(double), cmp_double);
    sum = 0; for (int i = 0; i < iters; i++) sum += lat[i];
    double mmean = sum / iters;
    printf("mmap (page-fault memcpy)\n");
    printf("  first gather : %8.3f ms  (cold if caches were purged)\n", mfirst);
    printf("  mean gather  : %8.3f ms   p50 %.3f  p99 %.3f\n",
           mmean, lat[iters/2], lat[(int)(iters*0.99)]);
    printf("  per row      : %8.3f us\n\n", mmean / gather * 1000.0);

    printf("note: mean gathers reuse cached pages — the cold cost lives in the "
           "first gather.\n      re-run with `sudo purge` (macOS) between builds "
           "to isolate the SSD fault.\n");

    free(lat); free(dst); free(idx);
    unlink(path);
    return 0;
}
