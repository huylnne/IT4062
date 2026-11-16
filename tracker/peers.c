#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "peers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static peer_t *g_peers[MAX_PEERS];
static size_t g_count;

peer_t *peers_add(int fd, const char *addr)
{
    if (g_count >= MAX_PEERS)
        return NULL;

    peer_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    conn_init(&p->conn, fd, SENDQ_LIMIT);
    snprintf(p->addr, sizeof(p->addr), "%s", addr);
    g_peers[g_count++] = p;
    return p;
}

void peers_remove(size_t idx)
{
    if (idx >= g_count)
        return;
    peer_t *p = g_peers[idx];
    conn_close(&p->conn);
    free(p);
    memmove(&g_peers[idx], &g_peers[idx + 1],
            (g_count - idx - 1) * sizeof(g_peers[0]));
    g_count--;
}

peer_t *peers_find_user(const char *name)
{
    for (size_t i = 0; i < g_count; i++)
        if (g_peers[i]->username[0] && !g_peers[i]->closing &&
            strcmp(g_peers[i]->username, name) == 0)
            return g_peers[i];
    return NULL;
}

size_t peers_count(void)
{
    return g_count;
}

peer_t *peers_at(size_t idx)
{
    return idx < g_count ? g_peers[idx] : NULL;
}
