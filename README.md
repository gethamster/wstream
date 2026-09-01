# wstream

A tiny, engine-agnostic C library for NVMe→memory weight residency streaming.
The reusable primitive under "stream experts / n-gram tables off SSD" — pulled
out so any engine (pMLX, mlx-serve, …) links the same thing instead of each
re-implementing the plumbing.

## What it is (and isn't)

- Plain C. No MLX, no Python, no C++. ~230 LOC.
- A file is `n_rows` fixed-size rows. The first `resident_rows` are read once and
  wired (`mlock`); the rest stream on demand through a `pread` thread-pool.
- **pread pool, not mmap page-faults** — the fault path costs ~5 ms/first-touch;
  a real read-ahead pool is ~0.7 ms (measured on this class of workload).
- Caller owns the destination buffer. On Apple unified memory that buffer is
  already device-addressable, so `ws_gather` writes straight into an MLX
  `bytesNoCopy` buffer or a Zig slice — no copy, no framework coupling.

## API

```c
wstream *ws_open(path, row_bytes, n_rows, resident_rows, threads);
int      ws_gather(ws, indices, count, dst);   // resident=copy, tail=pread pool
void     ws_prefetch(ws, indices, count);      // F_RDADVISE hint, non-blocking
int      ws_resident(ws, index);               // zero-IO row?
void     ws_close(ws);
```

## Build

```
make test     # compiles + runs the byte-exact correctness test
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
