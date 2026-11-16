#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

/* Số chunk tối đa trong một danh sách tiêm lỗi */
#define FAULT_MAX 256

/**
 * Tuỳ chọn tiêm lỗi ở tầng ứng dụng (mục 4.6 đề cương).
 * drop/corrupt chỉ áp dụng cho lần gửi đầu của chunk (phía nguồn), để lần
 * gửi lại đi thật và chứng minh chỉ đúng chunk lỗi được gửi lại.
 * delay_ms làm chậm mọi ACK (phía nhận).
 */
typedef struct {
    uint32_t drop[FAULT_MAX];
    int ndrop;
    uint32_t corrupt[FAULT_MAX];
    int ncorrupt;
    int delay_ms;
} fault_t;

/**
 * @function fault_parse_list: Đọc danh sách chunk dạng "3,7,10-12".
 *
 * @param s: Chuỗi.
 * @param out: Mảng nhận (FAULT_MAX phần tử).
 * @param n: Số phần tử đọc được.
 *
 * @return: 0 nếu hợp lệ, -1 nếu sai cú pháp hoặc quá FAULT_MAX phần tử.
 */
int fault_parse_list(const char *s, uint32_t *out, int *n);

/**
 * @function fault_has: cid có trong danh sách không.
 *
 * @param list: Danh sách.
 * @param n: Số phần tử.
 * @param cid: Số thứ tự chunk.
 *
 * @return: 1 nếu có, 0 nếu không.
 */
int fault_has(const uint32_t *list, int n, uint32_t cid);

#endif /* FAULT_H */
