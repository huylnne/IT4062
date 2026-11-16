#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "util.h"
#include "protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

int valid_username(const char *s)
{
    size_t n = strlen(s);
    if (n < MIN_USERNAME || n > MAX_USERNAME)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && ch != '_')
            return 0;
    }
    return 1;
}

int valid_filename(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > MAX_FILENAME)
        return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        return 0;
    return strpbrk(s, "/\\") == NULL;
}

static const struct {
    uint8_t type;
    const char *name;
} g_types[] = {
    { MSG_JOIN, "JOIN" },
    { MSG_QUIT, "QUIT" },
    { MSG_SHARE, "SHARE" },
    { MSG_UNSHARE, "UNSHARE" },
    { MSG_LIST, "LIST" },
    { MSG_SEARCH, "SEARCH" },
    { MSG_GET, "GET" },
    { MSG_OK, "OK" },
    { MSG_ERR, "ERR" },
    { MSG_LIST_RESP, "LIST_RESP" },
    { MSG_SEND_REQ, "SEND_REQ" },
    { MSG_GET_OK, "GET_OK" },
    { MSG_BUSY, "BUSY" },
    { MSG_CHUNK, "CHUNK" },
    { MSG_ACK, "ACK" },
    { MSG_NACK, "NACK" },
    { MSG_REQ_RANGE, "REQ_RANGE" },
    { MSG_DONE, "DONE" },
    { MSG_CANCEL, "CANCEL" },
    { MSG_ABORT, "ABORT" },
    { MSG_SOURCE_GONE, "SOURCE_GONE" }
};

#define N_TYPES (sizeof(g_types) / sizeof(g_types[0]))

/* Chỉ số = mã lỗi */
static const char *g_errs[ERR_CODE_MAX + 1] = {
    [E_BAD_FRAME] = "BAD_FRAME",
    [E_NOT_JOINED] = "NOT_JOINED",
    [E_ALREADY_JOINED] = "ALREADY_JOINED",
    [E_NAME_TAKEN] = "NAME_TAKEN",
    [E_BAD_USERNAME] = "BAD_USERNAME",
    [E_BAD_FILENAME] = "BAD_FILENAME",
    [E_BAD_SIZE] = "BAD_SIZE",
    [E_ALREADY_SHARED] = "ALREADY_SHARED",
    [E_NOT_FOUND] = "NOT_FOUND",
    [E_OWNER_OFFLINE] = "OWNER_OFFLINE",
    [E_SELF_GET] = "SELF_GET",
    [E_TIMEOUT] = "TIMEOUT",
    [E_NO_PROGRESS] = "NO_PROGRESS",
    [E_FILE_IO] = "FILE_IO",
    [E_HASH_MISMATCH] = "HASH_MISMATCH",
    [E_UNKNOWN_TYPE] = "UNKNOWN_TYPE",
    [E_FULL] = "FULL"
};

const char *msg_type_name(uint8_t type)
{
    for (size_t i = 0; i < N_TYPES; i++)
        if (g_types[i].type == type)
            return g_types[i].name;
    return "?";
}

int msg_type_from_name(const char *name)
{
    for (size_t i = 0; i < N_TYPES; i++)
        if (strcasecmp(g_types[i].name, name) == 0)
            return g_types[i].type;
    return -1;
}

const char *err_code_name(uint16_t code)
{
    if (code >= 1 && code <= ERR_CODE_MAX && g_errs[code])
        return g_errs[code];
    return "?";
}

int err_code_from_name(const char *name)
{
    for (int i = 1; i <= ERR_CODE_MAX; i++)
        if (g_errs[i] && strcasecmp(g_errs[i], name) == 0)
            return i;
    return -1;
}

void log_msg(const char *tag, const char *fmt, ...)
{
    char ts[16];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s [%s] ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
