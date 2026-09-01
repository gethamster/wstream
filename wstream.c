#include "wstream.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per-gather completion latch. Stack-allocated by each ws_gather call so
 * concurrent gathers on one handle don't share state (reentrant). */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t done;
    size_t pending;
    int err;                          /* set if any row in this gather read short */
} gather_group;

typedef struct { uint32_t index; void *dst; gather_group *grp; } job_t;

struct wstream {
    int fd;
    size_t row_bytes, n_rows, resident_rows;
    unsigned char *resident_buf;      /* wired prefix, resident_rows*row_bytes */

    int nthreads;
    pthread_t *workers;

    /* bounded ring queue of stream jobs */
    job_t *q;
    size_t qcap, qhead, qtail, qcount;
    pthread_mutex_t qmtx;
    pthread_cond_t not_empty, not_full;

    int shutdown;
};

/* Returns 0 on a full row, -1 on short/failed read. */
static int read_row(struct wstream *ws, uint32_t index, void *dst) {
    off_t off = (off_t)index * (off_t)ws->row_bytes;
    size_t got = 0;
    while (got < ws->row_bytes) {
        ssize_t r = pread(ws->fd, (char *)dst + got, ws->row_bytes - got, off + got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void *worker(void *arg) {
    struct wstream *ws = arg;
    for (;;) {
        pthread_mutex_lock(&ws->qmtx);
        while (ws->qcount == 0 && !ws->shutdown)
            pthread_cond_wait(&ws->not_empty, &ws->qmtx);
        if (ws->qcount == 0 && ws->shutdown) {
            pthread_mutex_unlock(&ws->qmtx);
            return NULL;
        }
        job_t j = ws->q[ws->qhead];
        ws->qhead = (ws->qhead + 1) % ws->qcap;
        ws->qcount--;
        pthread_cond_signal(&ws->not_full);
        pthread_mutex_unlock(&ws->qmtx);

        int rc = read_row(ws, j.index, j.dst);

        pthread_mutex_lock(&j.grp->mtx);
        if (rc != 0) j.grp->err = 1;
        if (--j.grp->pending == 0) pthread_cond_signal(&j.grp->done);
        pthread_mutex_unlock(&j.grp->mtx);
    }
}

static void submit(struct wstream *ws, job_t j) {
    pthread_mutex_lock(&ws->qmtx);
    while (ws->qcount == ws->qcap)
        pthread_cond_wait(&ws->not_full, &ws->qmtx);
    ws->q[ws->qtail] = j;
    ws->qtail = (ws->qtail + 1) % ws->qcap;
    ws->qcount++;
    pthread_cond_signal(&ws->not_empty);
    pthread_mutex_unlock(&ws->qmtx);
}

wstream *ws_open(const char *path, size_t row_bytes, size_t n_rows,
                 size_t resident_rows, int threads) {
    if (!row_bytes || !n_rows) return NULL;
    if (n_rows > SIZE_MAX / row_bytes) return NULL;   /* file-size overflow */

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    /* Reject a file too short to hold n_rows*row_bytes rather than serving
     * silent zero/partial rows later. */
    struct stat st;
    if (fstat(fd, &st) != 0 || (uintmax_t)st.st_size < (uintmax_t)n_rows * row_bytes) {
        close(fd);
        return NULL;
    }
    if (resident_rows > n_rows) resident_rows = n_rows;

    struct wstream *ws = calloc(1, sizeof *ws);
    if (!ws) { close(fd); return NULL; }
    ws->fd = fd;
    ws->row_bytes = row_bytes;
    ws->n_rows = n_rows;
    ws->resident_rows = resident_rows;
    ws->nthreads = threads > 0 ? threads : 4;
    ws->qcap = 8192;

    if (resident_rows) {
        size_t total = resident_rows * row_bytes, got = 0;
        ws->resident_buf = malloc(total);
        if (!ws->resident_buf) goto fail;
        while (got < total) {
            ssize_t r = pread(fd, ws->resident_buf + got, total - got, (off_t)got);
            if (r <= 0) goto fail;    /* truncated/failed resident load */
            got += (size_t)r;
        }
        mlock(ws->resident_buf, total);    /* wire it; best-effort */
    }

    ws->q = malloc(ws->qcap * sizeof(job_t));
    ws->workers = malloc((size_t)ws->nthreads * sizeof(pthread_t));
    if (!ws->q || !ws->workers) goto fail;

    pthread_mutex_init(&ws->qmtx, NULL);
    pthread_cond_init(&ws->not_empty, NULL);
    pthread_cond_init(&ws->not_full, NULL);

    int started = 0;
    for (; started < ws->nthreads; started++)
        if (pthread_create(&ws->workers[started], NULL, worker, ws) != 0)
            break;
    if (started != ws->nthreads) {
        /* roll back the pool cleanly, then destroy sync primitives below */
        pthread_mutex_lock(&ws->qmtx);
        ws->shutdown = 1;
        pthread_cond_broadcast(&ws->not_empty);
        pthread_mutex_unlock(&ws->qmtx);
        for (int i = 0; i < started; i++) pthread_join(ws->workers[i], NULL);
        pthread_mutex_destroy(&ws->qmtx);
        pthread_cond_destroy(&ws->not_empty);
        pthread_cond_destroy(&ws->not_full);
        goto fail;
    }
    return ws;

fail:
    if (ws->resident_buf) {
        munlock(ws->resident_buf, resident_rows * row_bytes);
        free(ws->resident_buf);
    }
    free(ws->q);
    free(ws->workers);
    free(ws);
    close(fd);
    return NULL;
}

int ws_resident(wstream *ws, uint32_t index) {
    return index < ws->resident_rows;
}

int ws_gather(wstream *ws, const uint32_t *idx, size_t count, void *dst) {
    unsigned char *out = dst;

    /* Validate before touching disk: an out-of-range index would otherwise
     * pread past EOF and hand back a silent short/zero row. */
    size_t nstream = 0;
    for (size_t i = 0; i < count; i++) {
        if (idx[i] >= ws->n_rows) return -1;
        if (idx[i] >= ws->resident_rows) nstream++;
    }

    /* resident rows: straight copy from the wired prefix, zero IO */
    for (size_t i = 0; i < count; i++)
        if (idx[i] < ws->resident_rows)
            memcpy(out + i * ws->row_bytes,
                   ws->resident_buf + (size_t)idx[i] * ws->row_bytes,
                   ws->row_bytes);

    if (nstream == 0) return 0;

    gather_group grp = { .pending = nstream, .err = 0 };
    pthread_mutex_init(&grp.mtx, NULL);
    pthread_cond_init(&grp.done, NULL);

    for (size_t i = 0; i < count; i++)
        if (idx[i] >= ws->resident_rows)
            submit(ws, (job_t){ idx[i], out + i * ws->row_bytes, &grp });

    pthread_mutex_lock(&grp.mtx);
    while (grp.pending) pthread_cond_wait(&grp.done, &grp.mtx);
    int err = grp.err;
    pthread_mutex_unlock(&grp.mtx);

    pthread_mutex_destroy(&grp.mtx);
    pthread_cond_destroy(&grp.done);
    return err ? -1 : 0;
}

void ws_prefetch(wstream *ws, const uint32_t *idx, size_t count) {
#ifdef __APPLE__
    for (size_t i = 0; i < count; i++) {
        if (idx[i] < ws->n_rows && idx[i] >= ws->resident_rows) {
            struct radvisory ra;
            ra.ra_offset = (off_t)idx[i] * (off_t)ws->row_bytes;
            ra.ra_count = (int)ws->row_bytes;
            fcntl(ws->fd, F_RDADVISE, &ra);
        }
    }
#else
    (void)ws; (void)idx; (void)count;
#endif
}

void ws_close(wstream *ws) {
    if (!ws) return;
    pthread_mutex_lock(&ws->qmtx);
    ws->shutdown = 1;
    pthread_cond_broadcast(&ws->not_empty);
    pthread_mutex_unlock(&ws->qmtx);
    for (int i = 0; i < ws->nthreads; i++)
        pthread_join(ws->workers[i], NULL);
    pthread_mutex_destroy(&ws->qmtx);
    pthread_cond_destroy(&ws->not_empty);
    pthread_cond_destroy(&ws->not_full);
    if (ws->resident_buf) {
        munlock(ws->resident_buf, ws->resident_rows * ws->row_bytes);
        free(ws->resident_buf);
    }
    free(ws->q);
    free(ws->workers);
    close(ws->fd);
    free(ws);
}
