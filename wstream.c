#include "wstream.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct { uint32_t index; void *dst; } job_t;

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

    /* per-gather completion latch */
    pthread_mutex_t dmtx;
    pthread_cond_t done_cv;
    size_t pending;

    int shutdown;
};

static void read_row(struct wstream *ws, uint32_t index, void *dst) {
    off_t off = (off_t)index * (off_t)ws->row_bytes;
    size_t got = 0;
    while (got < ws->row_bytes) {
        ssize_t r = pread(ws->fd, (char *)dst + got, ws->row_bytes - got, off + got);
        if (r <= 0) break;                 /* short/failed read: leave partial */
        got += (size_t)r;
    }
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

        read_row(ws, j.index, j.dst);

        pthread_mutex_lock(&ws->dmtx);
        if (--ws->pending == 0) pthread_cond_signal(&ws->done_cv);
        pthread_mutex_unlock(&ws->dmtx);
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
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (resident_rows > n_rows) resident_rows = n_rows;

    struct wstream *ws = calloc(1, sizeof *ws);
    if (!ws) { close(fd); return NULL; }
    ws->fd = fd;
    ws->row_bytes = row_bytes;
    ws->n_rows = n_rows;
    ws->resident_rows = resident_rows;

    if (resident_rows) {
        size_t total = resident_rows * row_bytes, got = 0;
        ws->resident_buf = malloc(total);
        while (got < total) {
            ssize_t r = pread(fd, ws->resident_buf + got, total - got, (off_t)got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        mlock(ws->resident_buf, total);    /* wire it; best-effort */
    }

    ws->nthreads = threads > 0 ? threads : 4;
    ws->qcap = 8192;
    ws->q = malloc(ws->qcap * sizeof(job_t));
    pthread_mutex_init(&ws->qmtx, NULL);
    pthread_cond_init(&ws->not_empty, NULL);
    pthread_cond_init(&ws->not_full, NULL);
    pthread_mutex_init(&ws->dmtx, NULL);
    pthread_cond_init(&ws->done_cv, NULL);
    ws->workers = malloc((size_t)ws->nthreads * sizeof(pthread_t));
    for (int i = 0; i < ws->nthreads; i++)
        pthread_create(&ws->workers[i], NULL, worker, ws);
    return ws;
}

int ws_resident(wstream *ws, uint32_t index) {
    return index < ws->resident_rows;
}

int ws_gather(wstream *ws, const uint32_t *idx, size_t count, void *dst) {
    unsigned char *out = dst;
    size_t nstream = 0;
    for (size_t i = 0; i < count; i++)
        if (idx[i] >= ws->resident_rows) nstream++;

    /* resident rows: straight copy from the wired prefix, zero IO */
    for (size_t i = 0; i < count; i++)
        if (idx[i] < ws->resident_rows)
            memcpy(out + i * ws->row_bytes,
                   ws->resident_buf + (size_t)idx[i] * ws->row_bytes,
                   ws->row_bytes);

    if (nstream == 0) return 0;

    pthread_mutex_lock(&ws->dmtx);
    ws->pending = nstream;
    pthread_mutex_unlock(&ws->dmtx);

    for (size_t i = 0; i < count; i++)
        if (idx[i] >= ws->resident_rows) {
            job_t j = { idx[i], out + i * ws->row_bytes };
            submit(ws, j);
        }

    pthread_mutex_lock(&ws->dmtx);
    while (ws->pending) pthread_cond_wait(&ws->done_cv, &ws->dmtx);
    pthread_mutex_unlock(&ws->dmtx);
    return 0;
}

void ws_prefetch(wstream *ws, const uint32_t *idx, size_t count) {
#ifdef __APPLE__
    for (size_t i = 0; i < count; i++) {
        if (idx[i] >= ws->resident_rows) {
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
    if (ws->resident_buf) {
        munlock(ws->resident_buf, ws->resident_rows * ws->row_bytes);
        free(ws->resident_buf);
    }
    free(ws->q);
    free(ws->workers);
    close(ws->fd);
    free(ws);
}
