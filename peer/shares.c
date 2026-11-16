#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "shares.h"

#include <stdio.h>
#include <string.h>

static share_t g_shares[MAX_SHARES];
static size_t g_count;

int shares_add(const char *name, const char *path, uint64_t size)
{
    if (g_count >= MAX_SHARES)
        return -1;
    share_t *s = &g_shares[g_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->size = size;
    return 0;
}

const share_t *shares_find(const char *name)
{
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_shares[i].name, name) == 0)
            return &g_shares[i];
    return NULL;
}

void shares_remove(const char *name)
{
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_shares[i].name, name) == 0) {
            memmove(&g_shares[i], &g_shares[i + 1],
                    (g_count - i - 1) * sizeof(g_shares[0]));
            g_count--;
            return;
        }
    }
}

size_t shares_count(void)
{
    return g_count;
}

const share_t *shares_at(size_t idx)
{
    return idx < g_count ? &g_shares[idx] : NULL;
}
