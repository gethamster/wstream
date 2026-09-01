/* wstream — engine-agnostic NVMe->memory weight residency streaming.
 *
 * One low-level primitive both pMLX and mlx-serve (or any engine) can link:
 * mmap-free row streaming off SSD with a pinned-resident prefix + a pread
 * thread-pool for the tail. No MLX, no Python, no C++ — plain C, caller owns
 * the destination buffer (so on Apple unified memory it drops straight into an
 * MLX bytesNoCopy buffer or a Zig slice).
 */
#ifndef WSTREAM_H
#define WSTREAM_H

#include <stddef.h>
#include <stdint.h>

#define WSTREAM_VERSION_MAJOR 0
#define WSTREAM_VERSION_MINOR 1
#define WSTREAM_VERSION_PATCH 0
#define WSTREAM_VERSION "0.1.0"

typedef struct wstream wstream;

/* Open a weight file laid out as n_rows fixed-size rows of row_bytes each.
 * The first `resident_rows` rows are read once and wired (mlock) — gathers of
 * those never touch the disk. The rest stream on demand via `threads` workers
 * (threads <= 0 defaults to 4).
 *
 * Fails (returns NULL) if the file is missing, is shorter than
 * n_rows*row_bytes, the resident prefix can't be read, or an allocation fails.
 * mlock failure is non-fatal (the prefix is still served from RAM). */
wstream *ws_open(const char *path, size_t row_bytes, size_t n_rows,
                 size_t resident_rows, int threads);

/* Gather `count` rows (by index) into `dst` (count*row_bytes, caller-owned).
 * Resident rows are copied from the wired prefix; non-resident rows are pread
 * in parallel across the pool. Blocks until all rows are in `dst`.
 *
 * Returns 0 on success, -1 if any index is out of range (>= n_rows) or any
 * row read failed short — on -1 the contents of `dst` are undefined.
 *
 * Reentrant: concurrent ws_gather calls on the SAME handle are safe (each call
 * carries its own completion latch). ws_close must not race a live gather. */
int ws_gather(wstream *ws, const uint32_t *indices, size_t count, void *dst);

/* Best-effort: hint the OS to read these non-resident rows ahead (F_RDADVISE on
 * macOS; no-op elsewhere). Non-blocking. Use for predictable keys (e.g. the
 * n-gram table). */
void ws_prefetch(wstream *ws, const uint32_t *indices, size_t count);

/* 1 if the row is in the wired resident prefix (a gather of it does zero IO). */
int ws_resident(wstream *ws, uint32_t index);

void ws_close(wstream *ws);

#endif
