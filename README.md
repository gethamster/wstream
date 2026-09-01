# wstream

[![ci](https://github.com/gethamster/wstream/actions/workflows/ci.yml/badge.svg)](https://github.com/gethamster/wstream/actions/workflows/ci.yml)

A small C library that streams fixed-size rows from an SSD-resident file into
memory you own, with a pinned hot prefix and a `pread` thread pool for the
rest. Built for inference engines that keep large weight tables, such as MoE
experts or n-gram embeddings, on disk.

Released by [Hamster Research](https://tryhamster.com/research) · [tryhamster.com](https://tryhamster.com)

## How it works

The file is `n_rows` rows of `row_bytes` each.

- The first `resident_rows` rows are read once at open and pinned with `mlock`,
  so gathering them is a memcpy with no IO.
- Every other row is fetched on demand by `threads` workers issuing `pread` in
  parallel.
- `ws_gather` writes rows straight into a buffer you own. On Apple unified
  memory that buffer can be device-visible, so there is no extra copy.

mmap takes one synchronous page fault per row. The pool issues every read in a
gather at once, so on a cold cache sixteen serial faults of several milliseconds each
become one parallel round trip.

## Install

One `.c` and one `.h`, no dependencies. C11, POSIX, Linux and macOS.

Vendor it (recommended): copy `wstream.c` and `wstream.h` into your tree and
compile them with your code.

Or install a static library:

```
make install                 # libwstream.a + wstream.h into /usr/local
cc yourcode.c -lwstream
```

Or build a shared library for FFI from Python, Zig, or anything with `dlopen`:

```
make dylib                   # libwstream.dylib on macOS, libwstream.so on Linux
```

The header has `extern "C"` guards, so C++ projects link it directly.

## Quick start

```c
#include "wstream.h"

wstream *ws = ws_open("table.bin", row_bytes, n_rows, resident_rows, /*threads=*/4);
if (!ws) { /* missing or short file, or allocation failure */ }

uint32_t idx[16] = { /* row indices for this step */ };
unsigned char *dst = your_buffer;            /* 16 * row_bytes bytes, caller-owned */

ws_prefetch(ws, next_idx, 16);               /* optional readahead hint for next step */
if (ws_gather(ws, idx, 16, dst) != 0) { /* out-of-range index or short read */ }

ws_close(ws);
```

## API

| Function | What it does |
|---|---|
| `ws_open(path, row_bytes, n_rows, resident_rows, threads)` | Opens the file, pins the first `resident_rows`, starts `threads` workers (0 means 4). Returns `NULL` if the file is missing or shorter than `n_rows * row_bytes`, the resident load fails, or an allocation fails. |
| `ws_gather(ws, indices, count, dst)` | Copies `count` rows into `dst` (`count * row_bytes` bytes). Resident rows are memcpy'd, the rest are read in parallel. Blocks until done. Returns 0, or -1 on an out-of-range index or short read, in which case `dst` is undefined. |
| `ws_prefetch(ws, indices, count)` | Non-blocking readahead hint for non-resident rows. `F_RDADVISE` on macOS, `posix_fadvise` on Linux. |
| `ws_resident(ws, index)` | Returns 1 if the row is in the pinned prefix. |
| `ws_close(ws)` | Stops the pool and frees everything. Must not overlap a running gather. |

Guarantees:

- Concurrent `ws_gather` calls on one handle are safe, since each call carries
  its own completion latch.
- Reads retry on `EINTR`, so a signal in the host process does not fail a
  gather.
- No silent partial rows. Every failure comes back through the return value.

## When it helps

wstream is for tables that do not fit in RAM.

| Regime | mmap | wstream |
|---|---|---|
| Table fits in RAM (warm page cache) | Fast, no pool overhead | About the same |
| Table larger than RAM (cold faults) | One multi-millisecond fault per row, serialized | One parallel round trip per gather; pinned rows need no IO |

If your table fits in memory, mmap is fine and wstream will not make decode
faster. If it does not fit, mmap stalls on every lookup, and wstream makes the
disk-resident configuration usable.

Measure cold before deciding. `make bench` compares both paths on your
hardware. Drop caches first (`sudo purge` on macOS, `echo 3 >
/proc/sys/vm/drop_caches` on Linux), otherwise you are measuring RAM.

## Getting the most out of it

- Order rows hot-first. Access is usually skewed, so put the most-hit rows at
  the front of the file and set `resident_rows` to cover them. Those lookups
  never touch disk.
- Batch. One gather of many rows costs less per row than many small gathers;
  per-row cost drops by about a third between 8 and 128 rows per call. Gather a whole
  speculative draft tree at once.
- Prefetch predictable rows. `ws_prefetch` costs nothing on the pool. Use it
  for rows you know you will need next step instead of reading and discarding
  them to warm the cache.
- Match threads to rows per gather. More workers than rows adds wakeup
  overhead. Four threads suit a 16-row gather; re-sweep if you batch larger.

## Build and test

```
make test      # byte-exact, out-of-range, and 1600-gather concurrency test
make bench     # pread pool vs mmap first-touch latency
make dylib     # shared library
make install   # static library and header into $(PREFIX), default /usr/local
```

CI runs the test on Linux and macOS, plus ThreadSanitizer, ASan/UBSan, and a
`-pedantic -Werror` build.

## Roadmap

Under discussion. None are required by the current API.

- Library-allocated pinned arena as an alternative to a caller-owned `dst`
- RDMA backend behind the same `ws_gather` interface
- An eviction policy above the resident prefix
- io_uring, `F_NOCACHE`, and direct-IO alignment
- Residency chosen by a saliency map rather than a prefix

## License

MIT.
