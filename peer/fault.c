#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "fault.h"

#include <errno.h>
#include <stdlib.h>

int fault_parse_list(const char *s, uint32_t *out, int *n)
{
    const char *p = s;
    *n = 0;

    while (*p) {
        char *end;
        errno = 0;
        unsigned long a = strtoul(p, &end, 10);
        if (errno || end == p)
            return -1;
        unsigned long b = a;
        p = end;
        if (*p == '-') {
            p++;
            b = strtoul(p, &end, 10);
            if (errno || end == p || b < a)
                return -1;
            p = end;
        }
        for (unsigned long v = a; v <= b; v++) {
            if (*n >= FAULT_MAX)
                return -1;
            out[(*n)++] = (uint32_t)v;
        }
        if (*p == ',')
            p++;
        else if (*p != '\0')
            return -1;
    }
    return 0;
}

int fault_has(const uint32_t *list, int n, uint32_t cid)
{
    for (int i = 0; i < n; i++)
        if (list[i] == cid)
            return 1;
    return 0;
}
