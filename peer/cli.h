#ifndef CLI_H
#define CLI_H

#include <stddef.h>

/* Bộ đệm stdin (chứa được nhiều dòng chờ xử lý) */
#define CLI_BUF 8192

/* Độ dài tối đa một dòng lệnh */
#define CLI_LINE_MAX 1024

/**
 * Bộ đệm stdin đọc bằng read() (dùng chung với poll()). Các dòng được lấy ra
 * từng dòng một bằng cli_next_line, nên vòng lặp chính có thể tạm dừng xử lý
 * lệnh (ví dụ lệnh wait) mà không mất các dòng đã đọc.
 */
typedef struct {
    char buf[CLI_BUF];
    size_t len;
    int eof;
} cli_t;

void cli_init(cli_t *cli);

/**
 * @function cli_read: Đọc thêm dữ liệu từ fd vào bộ đệm.
 *
 * @param cli: Bộ đệm.
 * @param fd: Thường là STDIN_FILENO.
 *
 * @return: 0 nếu bình thường, 1 nếu EOF, -1 nếu lỗi.
 */
int cli_read(cli_t *cli, int fd);

/**
 * @function cli_has_room: Bộ đệm còn chỗ để đọc thêm không.
 *
 * @param cli: Bộ đệm.
 *
 * @return: 1 nếu còn, 0 nếu đầy.
 */
int cli_has_room(const cli_t *cli);

/**
 * @function cli_next_line: Lấy một dòng đầy đủ ra khỏi bộ đệm (bỏ \r\n).
 * Sau EOF, phần cuối không có \n cũng được trả về như một dòng.
 *
 * @param cli: Bộ đệm.
 * @param out: Bộ đệm nhận, CLI_LINE_MAX byte; dòng dài hơn bị cắt.
 *
 * @return: 1 nếu có dòng, 0 nếu chưa có.
 */
int cli_next_line(cli_t *cli, char *out);

/**
 * @function cli_split: Tách dòng theo khoảng trắng (sửa line tại chỗ).
 * Đối số cuối cùng (thứ max_args) giữ nguyên phần còn lại của dòng,
 * để tên file có dấu cách vẫn dùng được.
 *
 * @param line: Dòng lệnh.
 * @param argv: Mảng nhận con trỏ đối số.
 * @param max_args: Kích thước argv.
 *
 * @return: Số đối số.
 */
int cli_split(char *line, char **argv, int max_args);

#endif /* CLI_H */
