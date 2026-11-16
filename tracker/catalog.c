#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "catalog.h"

#include <string.h>

/* Giữ thứ tự thêm vào để LIST ổn định */
static cat_entry_t g_files[MAX_FILES];
static size_t g_count;

static long find_idx(const char *owner, const char *name)
{
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_files[i].owner, owner) == 0 &&
            strcmp(g_files[i].name, name) == 0)
            return (long)i;
    return -1;
}

static void remove_idx(size_t idx)
{
    memmove(&g_files[idx], &g_files[idx + 1],
            (g_count - idx - 1) * sizeof(g_files[0]));
    g_count--;
}

int catalog_add(const char *owner, const char *name, uint64_t size)
{
    if (find_idx(owner, name) >= 0)
        return CAT_EXISTS;
    if (g_count >= MAX_FILES)
        return CAT_FULL;

    cat_entry_t *e = &g_files[g_count++];
    strcpy(e->owner, owner);
    strcpy(e->name, name);
    e->size = size;
    return CAT_OK;
}

int catalog_remove(const char *owner, const char *name)
{
    long idx = find_idx(owner, name);
    if (idx < 0)
        return -1;
    remove_idx((size_t)idx);
    return 0;
}

size_t catalog_remove_owner(const char *owner)
{
    size_t removed = 0;
    size_t i = 0;

    while (i < g_count) {
        if (strcmp(g_files[i].owner, owner) == 0) {
            remove_idx(i);
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

const cat_entry_t *catalog_find(const char *owner, const char *name)
{
    long idx = find_idx(owner, name);
    return idx < 0 ? NULL : &g_files[idx];
}

size_t catalog_count(void)
{
    return g_count;
}

const cat_entry_t *catalog_at(size_t idx)
{
    return idx < g_count ? &g_files[idx] : NULL;
}
