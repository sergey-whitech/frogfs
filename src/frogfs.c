/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This is a read-only filesystem that uses a sorted hash table to locate
 * entries in a monolithic binary. The binary is generated by the mkfrogfs
 * tool that comes with this source distribution.
 */

#include "log.h"
#include "frogfs_config.h"
#include "frogfs_priv.h"
#include "frogfs_format.h"
#include "frogfs/frogfs.h"

#if defined(ESP_PLATFORM)
# include "esp_partition.h"
# include "spi_flash_mmap.h"
#endif

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


typedef struct frogfs_fs_t {
#if defined(ESP_PLATFORM)
    spi_flash_mmap_handle_t mmap_handle;
#endif
    const frogfs_head_t *head; /**< fs header pointer */
    const frogfs_hash_t *hash; /**< hash table pointer */
    const frogfs_dir_t *root; /**< root directory entry */
    int num_entries; /**< total number of file system entries */
} frogfs_fs_t;

// Returns the current or next highest multiple of 4.
static inline size_t align(size_t n)
{
    return ((n + 4 - 1) / 4) * 4;
}

// String hashing function.
static inline uint32_t djb2_hash(const char *s)
{
    unsigned long hash = 5381;

    while (*s) {
        /* hash = hash * 33 ^ c */
        hash = ((hash << 5) + hash) ^ *s++;
    }

    return hash;
}

frogfs_fs_t *frogfs_init(frogfs_config_t *conf)
{
    frogfs_fs_t *fs = calloc(1, sizeof(frogfs_fs_t));
    if (fs == NULL) {
        LOGE("calloc failed");
        return NULL;
    }

    LOGV("%p", fs);

    fs->head = (const void *) conf->addr;
    if (fs->head == NULL) {
#if defined (ESP_PLATFORM)
        esp_partition_subtype_t subtype = conf->part_label ?
                ESP_PARTITION_SUBTYPE_ANY :
                ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD;
        const esp_partition_t* partition = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, subtype, conf->part_label);

        if (partition == NULL) {
            LOGE("unable to find frogfs partition");
            goto err_out;
        }

        if (esp_partition_mmap(partition, 0, partition->size,
                SPI_FLASH_MMAP_DATA, (const void **)&fs->head,
                &fs->mmap_handle) != ESP_OK) {
            LOGE("mmap failed");
            goto err_out;
        }
#else
        LOGE("flash mmap not enabled and addr is NULL");
        goto err_out;
#endif
    }

    if (fs->head->magic != FROGFS_MAGIC) {
        LOGE("frogfs magic not found");
        goto err_out;
    }

    if (fs->head->ver_major != FROGFS_VER_MAJOR) {
        LOGE("frogfs major version mismatch. filesystem is v%d.%d and this "
                "library is v%d.%d", fs->head->ver_major, fs->head->ver_minor,
                FROGFS_VER_MAJOR, FROGFS_VER_MINOR);
        goto err_out;
    }

    fs->num_entries = fs->head->num_entries;
    fs->hash = (const void *) fs->head + sizeof(frogfs_head_t);
    fs->root = (const void *) fs->hash + (sizeof(frogfs_hash_t) * fs->num_entries);

    return fs;

err_out:
    frogfs_deinit(fs);
    return NULL;
}

void frogfs_deinit(frogfs_fs_t *fs)
{
    LOGV("%p", fs);

#if defined(ESP_PLATFORM)
    if (fs->mmap_handle) {
        spi_flash_munmap(fs->mmap_handle);
    }
#endif
    free(fs);
}

const frogfs_entry_t *frogfs_get_entry(const frogfs_fs_t *fs, const char *path)
{
    assert(fs != NULL);
    assert(path != NULL);

    while (*path == '/') {
        path++;
    }
    LOGV("'%s'", path);

    uint32_t hash = djb2_hash(path);
    LOGV("hash %08"PRIx32, hash);

    int first = 0;
    int last = fs->num_entries - 1;
    int middle;
    const frogfs_hash_t *e;

    while (first <= last) {
        middle = first + (last - first) / 2;
        e = &fs->hash[middle];
        if (e->hash == hash) {
            break;
        } else if (e->hash < hash) {
            first = middle + 1;
        } else {
            last = middle - 1;
        }
    }

    if (first > last) {
        LOGV("no match");
        return NULL;
    }

    /* move e to the first match */
    while (middle > 0) {
        e = fs->hash + middle;
        if ((e - 1)->hash != hash) {
            break;
        }
        middle--;
    }

    /* walk through canidates and look for a match */
    const frogfs_entry_t *entry;
    do {
        entry = (const void *) fs->head + e->offs;
        char *match = frogfs_get_path(fs, entry);
        if (strcmp(path, match) == 0) {
            free(match);
            LOGV("entry %d", middle);
            return entry;
        }
        free(match);
        entry++;
        middle++;
    } while ((middle < last) && (e->hash == hash));

    LOGW("unable to find entry");
    return NULL;
}

const char *frogfs_get_name(const frogfs_entry_t *entry)
{
    if (FROGFS_IS_DIR(entry)) {
        return (const void *) entry + 8 + (entry->child_count * 4);
    } else if (FROGFS_IS_FILE(entry) && !FROGFS_IS_COMP(entry)) {
        return (const void *) entry + 16;
    } else {
        return (const void *) entry + 20;
    }
}

char *frogfs_get_path(const frogfs_fs_t *fs, const frogfs_entry_t *entry)
{
    assert(entry != NULL);

    char *path = calloc(PATH_MAX, 1);
    size_t len = 0;
    if (!path) {
        return NULL;
    }

    if (entry->parent == 0) {
        return path;
    }

    while (entry->parent != 0 && len + entry->seg_sz + 1 < PATH_MAX - 1) {
        const frogfs_entry_t *parent = (const void *) fs->head + entry->parent;
        if ((const void *) parent == (const void *) fs->root) {
            memmove(path + entry->seg_sz, path, len);
            memcpy(path, frogfs_get_name(entry), entry->seg_sz);
            len += entry->seg_sz;
            break;
        } else {
            memmove(path + entry->seg_sz + 1, path, len + 1);
            path[0] = '/';
            memcpy(path + 1, frogfs_get_name(entry), entry->seg_sz);
            len += entry->seg_sz + 1;
        }
        entry = parent;
    }

    return path;
}

int frogfs_is_dir(const frogfs_entry_t *entry)
{
    return FROGFS_IS_DIR(entry);
}

int frogfs_is_file(const frogfs_entry_t *entry)
{
    return FROGFS_IS_FILE(entry);
}

void frogfs_stat(const frogfs_fs_t *fs, const frogfs_entry_t *entry,
        frogfs_stat_t *st)
{
    assert(fs != NULL);
    assert(entry != NULL);

    memset(st, 0, sizeof(*st));
    if (FROGFS_IS_DIR(entry)) {
        st->type = FROGFS_ENTRY_TYPE_DIR;
    } else {
        st->type = FROGFS_ENTRY_TYPE_FILE;
        const frogfs_file_t *file = (const void *) entry;
        st->compression = entry->compression;
        if (st->compression) {
            const frogfs_comp_t *comp = (const void *) entry;
            st->compressed_sz = comp->data_sz;
            st->size = comp->real_sz;
        } else {
            st->compressed_sz = file->data_sz;
            st->size = file->data_sz;
        }
    }
}

frogfs_fh_t *frogfs_open(const frogfs_fs_t *fs, const frogfs_entry_t *entry,
        unsigned int flags)
{
    assert(fs != NULL);
    assert(entry != NULL);

    if (FROGFS_IS_DIR(entry)) {
        return NULL;
    }

    const frogfs_file_t *file = (const void *) entry;

    frogfs_fh_t *fh = calloc(1, sizeof(frogfs_fh_t));
    if (fh == NULL) {
        LOGE("calloc failed");
        goto err_out;
    }

    LOGV("%p", fh);

    fh->fs = fs;
    fh->file = file;
    fh->data_start = (const void *) fs->head + file->data_offs;
    fh->data_ptr = fh->data_start;
    fh->data_sz = file->data_sz;
    fh->flags = flags;

    if (entry->compression == 0 || flags & FROGFS_OPEN_RAW) {
        fh->real_sz = file->data_sz;
        fh->decomp_funcs = &frogfs_decomp_raw;
    }
#if CONFIG_FROGFS_USE_DEFLATE == 1
    else if (entry->compression == FROGFS_COMP_ALGO_DEFLATE) {
        fh->real_sz = ((frogfs_comp_t *) file)->real_sz;
        fh->decomp_funcs = &frogfs_decomp_deflate;
    }
#endif
#if CONFIG_FROGFS_USE_HEATSHRINK == 1
    else if (entry->compression == FROGFS_COMP_ALGO_HEATSHRINK) {
        fh->real_sz = ((frogfs_comp_t *) file)->real_sz;
        fh->decomp_funcs = &frogfs_decomp_heatshrink;
    }
#endif
    else {
        LOGE("unknown compression type %d", entry->compression)
        goto err_out;
    }

    if (fh->decomp_funcs->open) {
        if (fh->decomp_funcs->open(fh, flags) < 0) {
            LOGE("decomp_funcs->fopen");
            goto err_out;
        }
    }

    return fh;

err_out:
    frogfs_close(fh);
    return NULL;
}

void frogfs_close(frogfs_fh_t *fh)
{
    if (fh == NULL) {
        /* do nothing */
        return;
    }

    if (fh->decomp_funcs && fh->decomp_funcs->close) {
        fh->decomp_funcs->close(fh);
    }

    LOGV("%p", fh);

    free(fh);
}

int frogfs_is_raw(frogfs_fh_t *fh)
{
    return !!(fh->flags & FROGFS_OPEN_RAW);
}

ssize_t frogfs_read(frogfs_fh_t *fh, void *buf, size_t len)
{
    assert(fh != NULL);

    if (fh->decomp_funcs->read) {
        return fh->decomp_funcs->read(fh, buf, len);
    }

    return -1;
}

ssize_t frogfs_seek(frogfs_fh_t *fh, long offset, int mode)
{
    assert(fh != NULL);

    if (fh->decomp_funcs->seek) {
        return fh->decomp_funcs->seek(fh, offset, mode);
    }

    return -1;
}

size_t frogfs_tell(frogfs_fh_t *fh)
{
    assert(fh != NULL);

    if (fh->decomp_funcs->tell) {
        return fh->decomp_funcs->tell(fh);
    }

    return -1;
}

size_t frogfs_access(frogfs_fh_t *fh, const void **buf)
{
    assert(fh != NULL);

    *buf = fh->data_start;
    return fh->data_sz;
}

frogfs_dh_t *frogfs_opendir(frogfs_fs_t *fs, const frogfs_entry_t *entry)
{
    assert(fs != NULL);

    if (entry != NULL && FROGFS_IS_FILE(entry)) {
        return NULL;
    }

    frogfs_dh_t *dh = calloc(1, sizeof(frogfs_dh_t));
    if (dh == NULL) {
        LOGE("calloc failed");
        return NULL;
    }

    dh->fs = fs;
    if (entry == NULL) {
        dh->dir = fs->root;
    } else {
        dh->dir = (const void *) entry;
    }

    return dh;
}

void frogfs_closedir(frogfs_dh_t *dh)
{
    if (dh == NULL) {
        return;
    }

    free(dh);
}

const frogfs_entry_t *frogfs_readdir(frogfs_dh_t *dh)
{
    const frogfs_entry_t *entry = NULL;

    if (dh->dir == NULL) {
        return NULL;
    }

    if (dh->index < dh->dir->entry.child_count) {
        entry = (const void *) dh->fs->head + dh->dir->children[dh->index];
        dh->index++;
    }

    return entry;
}

void frogfs_rewinddir(frogfs_dh_t *dh)
{
    assert(dh != NULL);

    dh->index = 0;
}

void frogfs_seekdir(frogfs_dh_t *dh, uint16_t loc)
{
    assert(dh != NULL);

    frogfs_rewinddir(dh);
    while (dh->index < loc) {
        frogfs_readdir(dh);
    }
}

uint16_t frogfs_telldir(frogfs_dh_t *dh)
{
    assert(dh != NULL);

    return dh->index;
}
