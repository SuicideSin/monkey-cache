/* vim: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */

#define _GNU_SOURCE
#include <stdlib.h>
#include <math.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>

#include "MKPlugin.h"

#include "cache_file.h"
#include "pipe_buf.h"
#include "utils.h"

#include "constants.h"

void cache_file_process_init() {}

void cache_file_thread_init() {
    file_table = table_alloc();
}

void cache_file_exit() {
    table_free(file_table);
}

void cache_file_free(struct cache_file_t *file) {
    if (!file) {
        return;
    }

    if (file->fd != -1) close(file->fd);
    if (file->buf.data != (void *) -1) munmap(file->buf.data, file->buf.len);


    pipe_buf_free(file->cache_headers);


    struct mk_list *reqs = &file->cache;
    struct mk_list *curr, *next;

    mk_list_foreach_safe(curr, next, reqs) {
        struct pipe_buf_t *buf = mk_list_entry(curr, struct pipe_buf_t, _head);
        mk_list_del(&buf->_head);
        pipe_buf_free(buf);
    }

    mk_api->mem_free(file);
}

struct cache_file_t *cache_file_get(const char *uri) {
    struct cache_file_t *file = table_get(file_table, uri);
    if (file) {
        gettimeofday(&file->last_accessed, NULL);
    }
    return file;
}

void file_table_reset(const char *uri) {
    struct cache_file_t *file = table_del(file_table, uri);
    if (file) {
        file->zombie = 1;
        if (file->pending_reqs == 0)
            cache_file_free(file);
    }
}

void cache_file_reset(const char *uri) {
    file_table_reset(uri);
    return;
}

void *cache_file_tick_cb(const char *key, void *val, void *state) {
    (void) key;
    struct cache_file_t *file = val;
    struct timeval tmp;
    gettimeofday(&tmp, NULL);

    int ms = get_time_diff_ms(file->last_accessed, tmp);

    // evict the file in case its 5 secs old
    if (ms > MAX_FILE_IDLE && file->evictable) {
        PLUGIN_TRACE("evicting url: %s\n", file->uri);
        cache_file_reset(file->uri);
    }

    return state;
}
// cleanup unused files
void cache_file_tick() {
    table_each(file_table, cache_file_tick_cb, NULL);
}

struct cache_file_t *cache_file_tmp(const char *uri, mk_pointer *ptr) {

    struct cache_file_t *file = NULL;

    file_table_reset(uri);

    mk_bug(cache_file_get(uri) != NULL);


    char tmpfile[] = "/tmp/monkey-cache-XXXXXX";
    int fd = mkostemp(tmpfile, O_NOATIME | O_NONBLOCK);
    if (fd == -1) {
        perror("cannot open the file!");
        return NULL;
    }

    unlink(tmpfile);

    // set the initial file size
    lseek(fd, ptr->len, SEEK_SET);
    if (write(fd, "", 1) < 0) {
        perror("cannot create a tmp file with required length!");
        close(fd);
        return NULL;
    }
    lseek(fd, 0, SEEK_SET);

    int mmap_len =
        ceil((double) ptr->len / MMAP_SIZE) * MMAP_SIZE;

    void *mmap_ptr = mmap(NULL, mmap_len,
        PROT_WRITE, MAP_PRIVATE, fd, 0);


    if (mmap_ptr == (void *) -1) {
        close(fd);
        perror("cannot create an mmap!");
        return NULL;
    }

    // may be better to replace it with series of vmsplice and splice
    // in case of large file uploads
    memcpy(mmap_ptr, ptr->data, ptr->len);

    file = mk_api->mem_alloc(sizeof(struct cache_file_t));
    strncpy(file->uri, uri, MAX_URI_LEN);
    file->uri[MAX_URI_LEN - 1] = '\0';

    mk_list_init(&file->cache);

    file->size = ptr->len;

    file->buf.data = mmap_ptr;
    file->buf.len = mmap_len;

    // do not want this file to be evicted, as that would
    // incur data loss
    file->evictable = 0;


    file->cache_headers = pipe_buf_new();
    file->header_len = 0;

    // file file buffer with empty pipes for now
    long len = file->size;
    struct pipe_buf_t *tmp;
    while (len > 0) {
        tmp = pipe_buf_new();

        mk_list_add(&tmp->_head, &file->cache);

        len -= tmp->cap;
    }

    gettimeofday(&file->last_accessed, NULL);
    if (table_add(file_table, file->uri, file) < 0) {
        cache_file_free(file);
        return NULL;
    }

    return file;
}

struct cache_file_t *cache_file_new(const char *path, const char *uri) {
    struct cache_file_t *file;

    struct stat st;
    if (stat(path, &st) == -1 || S_ISDIR(st.st_mode)) {
        return NULL;
    }

    if (st.st_size <= 0)
        return NULL;


    // another check in case its been already added
    file = table_get(file_table, uri);

    if (!file) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd == -1) {
            perror("cannot open the file!");
            return NULL;
        }

        int mmap_len =
            ceil((double) st.st_size / MMAP_SIZE) * MMAP_SIZE;

        void *mmap_ptr = mmap(NULL, mmap_len,
            PROT_READ, MAP_PRIVATE, fd, 0);

        if (mmap_ptr == (void *) -1) {
            close(fd);
            perror("cannot create an mmap!");
            return NULL;
        }

        file = mk_api->mem_alloc(sizeof(struct cache_file_t));
        strncpy(file->uri, uri, MAX_URI_LEN);
        file->uri[MAX_URI_LEN - 1] = '\0';

        mk_list_init(&file->cache);

        file->size = st.st_size;

        file->buf.data = mmap_ptr;
        file->buf.len = mmap_len;


        file->cache_headers = pipe_buf_new();
        file->header_len = 0;

        // file file buffer with empty pipes for now
        long len = file->size;
        struct pipe_buf_t *tmp;
        while (len > 0) {
            tmp = pipe_buf_new();

            mk_list_add(&tmp->_head, &file->cache);

            len -= tmp->cap;
        }

        file->zombie = 0;
        file->pending_reqs = 0;


        file->evictable = 1;

        gettimeofday(&file->last_accessed, NULL);
        if (table_add(file_table, file->uri, file) < 0) {
            cache_file_free(file);
            return NULL;
        }
    }

    return file;
}
