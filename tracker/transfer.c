#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "transfer.h"
#include "netio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static transfer_t *g_list[MAX_TRANSFERS];
static size_t g_count;
static uint32_t g_next_id = 1;

transfer_t *transfer_new(peer_t *src, peer_t *dst, const char *name,
                         uint64_t size)
{
    if (g_count >= MAX_TRANSFERS)
        return NULL;
    transfer_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    /* Bỏ qua 0 (nghĩa là "không thuộc phiên") và id còn đang dùng khi quay vòng */
    do {
        t->id = g_next_id++;
    } while (t->id == 0 || transfer_find(t->id));

    t->src = src;
    t->dst = dst;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->size = size;
    t->last_progress_ms = now_ms();
    g_list[g_count++] = t;
    return t;
}

transfer_t *transfer_find(uint32_t id)
{
    for (size_t i = 0; i < g_count; i++)
        if (g_list[i]->id == id)
            return g_list[i];
    return NULL;
}

void transfer_close(transfer_t *t)
{
    for (size_t i = 0; i < g_count; i++) {
        if (g_list[i] == t) {
            memmove(&g_list[i], &g_list[i + 1],
                    (g_count - i - 1) * sizeof(g_list[0]));
            g_count--;
            free(t);
            return;
        }
    }
}

int transfer_count_src(const peer_t *src)
{
    int n = 0;
    for (size_t i = 0; i < g_count; i++)
        if (g_list[i]->src == src)
            n++;
    return n;
}

size_t transfer_count(void)
{
    return g_count;
}

transfer_t *transfer_at(size_t idx)
{
    return idx < g_count ? g_list[idx] : NULL;
}
