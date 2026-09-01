# wstream

[![ci](https://github.com/gethamster/wstream/actions/workflows/ci.yml/badge.svg)](https://github.com/gethamster/wstream/actions/workflows/ci.yml)

A tiny, engine-agnostic C library for NVMe→memory weight residency streaming.
The reusable primitive under "stream experts / n-gram tables off SSD" — pulled
out so any engine (pMLX, mlx-serve, …) links the same thing instead of each
re-implementing the plumbing.

Released by **Hamster Research** — [tryhamster.com/research](https://tryhamster.com/research) · [tryhamster.com](https://tryhamster.com)

## What it is (and isn't)

- Plain C11, POSIX (Linux + macOS). No MLX, no Python, no dependencies. Under
  300 lines. `extern "C"` guarded, so C++ engines link it directly.
- A file is `n_rows` fixed-size rows. The first `resident_rows` are read once and
  wired (`mlock`); the rest stream on demand through a `pread` thread-pool.
- **pread pool, not mmap page-faults** — on a cold cache the mmap fault path
  serializes one synchronous first-touch per row, while the pool issues the
  reads in parallel. Warm, the two are comparable (mmap even wins — no pool
  overhead); the win is on the cold first touch. Reproduce on your hardware with
  `make bench` (drop caches first: `sudo purge` on macOS).
- Caller owns the destination buffer. On Apple unified memory that buffer is
  already device-addressable, so `ws_gather` writes straight into an MLX
  `bytesNoCopy` buffer or a Zig slice — no copy, no framework coupling.
- **Reentrant.** Concurrent `ws_gather` calls on the same handle are safe — each
  carries its own completion latch — so a multi-threaded decode loop can share
  one `wstream`. (`ws_close` must not race a live gather.)
- **Fails loud.** `ws_open` rejects a file shorter than `n_rows*row_bytes` or a
  failed resident load; `ws_gather` returns `-1` on an out-of-range index or a
  short read instead of handing back a silent zero/partial row. Reads retry on
  `EINTR`, so a signal in the host process never fails a gather.
- **CI** runs the byte-exact + reentrancy test on Linux and macOS, plus
  ThreadSanitizer, ASan/UBSan, and a `-pedantic -Werror` build.

## When it helps (and when it doesn't)

Be clear-eyed about what this buys you.

**Warm cache: roughly nothing.** If the table fits in RAM, the OS page cache
already serves it fast. In pMLX the disk-PLE gather is ~1% of a decode step
warm, and switching it to wstream measured neutral end-to-end (+0.6%, CI
includes zero). Making the pool faster can't move that number, so don't reach
for wstream expecting a warm tokens/sec win.

**Cold cache / memory pressure: this is the whole point.** The moment
model + table exceeds RAM, the page cache thrashes and every mmap lookup
becomes a serial cold page-fault, ~5 ms each. Sixteen rows per token is
~80 ms of stalls, several times a whole decode step, and the disk-resident
config becomes unusable. wstream collapses those sixteen serial faults into
one parallel round trip, and a wired `resident_rows` prefix makes the hot
rows zero-IO even when everything else is cold.

So wstream is a **capability lever, not a speed lever**: it lets you run a
disk-resident table, a bigger model, or streamed MoE experts on hardware that
could not otherwise hold them. Size it by the regime you actually run in,
and measure cold (`make bench` after dropping caches) before deciding.

To get the most out of it:

- **Order rows hot-first.** Row access is usually Zipfian. Lay the file out so
  the most-hit rows come first and set `resident_rows` to cover them; those
  lookups never touch disk. This is a file-layout convention, not a feature.
- **Batch.** One `ws_gather` of many rows amortizes better than many small
  ones (per-row cost roughly halves from 8 to 128 rows/gather). Gather a whole
  speculative draft tree in one call.
- **Prefetch predictable keys.** `ws_prefetch` is a non-blocking readahead
  hint with no pool occupancy. Use it for rows you know you'll need next step
  instead of reading and discarding them to warm the cache.
- **Size threads to rows/gather.** More workers than rows just adds wakeup
  overhead; ~4 threads for a 16-row gather, re-sweep if you batch bigger.

## API

```c
wstream *ws_open(path, row_bytes, n_rows, resident_rows, threads);  // NULL on error
int      ws_gather(ws, indices, count, dst);   // 0 ok, -1 bad index / short read
void     ws_prefetch(ws, indices, count);      // readahead hint (F_RDADVISE / fadvise)
int      ws_resident(ws, index);               // zero-IO row?
void     ws_close(ws);
```

## Using it in your engine

wstream is a single `.c` + `.h`, so add it whichever way fits your build:

**Vendor the source** (simplest — recommended): copy `wstream.c` and `wstream.h`
into your project and compile them with the rest of your code. No dependency, no
install step.

**Or install a linkable library:**

```
make install                 # -> /usr/local/lib/libwstream.a  +  /usr/local/include/wstream.h
cc yourcode.c -lwstream      # then link it
```

Either way the interface is just the header:

```c
#include "wstream.h"

wstream *ws = ws_open("weights.bin", row_bytes, n_rows, resident_rows, /*threads=*/8);
uint32_t idx[16] = { /* the rows you need this step */ };
ws_gather(ws, idx, 16, dst);   // dst is YOURS: count * row_bytes, caller-owned
ws_close(ws);
```

`dst` is caller-owned, so on Apple unified memory you point it straight at an MLX
`bytesNoCopy` buffer or a Zig/C slice — wstream never allocates your weights, so
there's no framework coupling.

**From a non-C language (Python, etc.):** build the shared lib with `make dylib`
(→ `libwstream.dylib` / `.so`) and `dlopen` it from your FFI. pmlx ships a ~40-line
ctypes binding (`pmlx/io/wstream.py`) you can copy as a starting point.

## Build

```
make test     # compiles + runs the byte-exact / reentrancy correctness test
make bench     # pread-pool vs mmap first-touch latency (see cold-cache note above)
```

## Scope / co-design notes

v0 skeleton, meant to be co-owned. Deliberately out of scope for now, to be
speced together:

- device-buffer ownership option (lib-allocated wired arena vs caller `dst`)
- RDMA backend behind the same `ws_gather` surface (David D.'s ask)
- an eviction/hot-cache policy above the resident prefix (grow-only LRU)
- io_uring / F_NOCACHE tuning, alignment for direct IO
- residency by a saliency map, not just a prefix (REAP-ordered rows)

The resident-prefix + pread-pool split is the load-bearing decision; everything
else layers on top.
