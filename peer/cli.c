#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

void cli_init(cli_t *cli)
{
    cli->len = 0;
    cli->eof = 0;
}

int cli_has_room(const cli_t *cli)
{
    return cli->len < sizeof(cli->buf);
}

int cli_read(cli_t *cli, int fd)
{
    if (!cli_has_room(cli))
        return 0;

    ssize_t k = read(fd, cli->buf + cli->len, sizeof(cli->buf) - cli->len);
    if (k < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    if (k == 0) {
        cli->eof = 1;
        return 1;
    }
    cli->len += (size_t)k;
    return 0;
}

int cli_next_line(cli_t *cli, char *out)
{
    char *nl = memchr(cli->buf, '\n', cli->len);
    size_t take;

    if (nl) {
        take = (size_t)(nl - cli->buf) + 1;
    } else if (cli->eof && cli->len > 0) {
        take = cli->len;
    } else if (!cli_has_room(cli)) {
        /* Dòng dài hơn cả bộ đệm: cắt ra như một dòng để không kẹt */
        take = cli->len;
    } else {
        return 0;
    }

    size_t n = take;
    while (n > 0 && (cli->buf[n - 1] == '\n' || cli->buf[n - 1] == '\r'))
        n--;
    if (n >= CLI_LINE_MAX)
        n = CLI_LINE_MAX - 1;
    memcpy(out, cli->buf, n);
    out[n] = '\0';

    memmove(cli->buf, cli->buf + take, cli->len - take);
    cli->len -= take;
    return 1;
}

int cli_split(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (argc < max_args) {
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        if (argc == max_args) {
            /* Đối số cuối lấy hết phần còn lại, bỏ khoảng trắng cuối dòng */
            char *end = p + strlen(p);
            while (end > p && isspace((unsigned char)end[-1]))
                *--end = '\0';
            break;
        }
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = '\0';
    }
    return argc;
}
