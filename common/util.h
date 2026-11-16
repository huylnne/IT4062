#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

/**
 * @function valid_username: 3..32 ký tự, chỉ gồm chữ, số, gạch dưới.
 *
 * @param s: Chuỗi cần kiểm tra.
 *
 * @return: 1 nếu hợp lệ, 0 nếu không.
 */
int valid_username(const char *s);

/**
 * @function valid_filename: 1..255 byte, không chứa '/' hoặc '\\',
 * không phải "." hay "..".
 *
 * @param s: Chuỗi cần kiểm tra.
 *
 * @return: 1 nếu hợp lệ, 0 nếu không.
 */
int valid_filename(const char *s);

/**
 * @function msg_type_name: Tên loại thông điệp, ví dụ "JOIN".
 *
 * @param type: Mã type.
 *
 * @return: Chuỗi tĩnh, "?" nếu không biết.
 */
const char *msg_type_name(uint8_t type);

/**
 * @function msg_type_from_name: Tra mã type từ tên (không phân biệt hoa thường).
 *
 * @param name: Tên, ví dụ "join".
 *
 * @return: Mã type, hoặc -1 nếu không có.
 */
int msg_type_from_name(const char *name);

/**
 * @function err_code_name: Tên mã lỗi bỏ tiền tố E_, ví dụ "NAME_TAKEN".
 *
 * @param code: Mã lỗi.
 *
 * @return: Chuỗi tĩnh, "?" nếu không biết.
 */
const char *err_code_name(uint16_t code);

/**
 * @function err_code_from_name: Tra mã lỗi từ tên (không phân biệt hoa thường).
 *
 * @param name: Tên, ví dụ "NAME_TAKEN".
 *
 * @return: Mã lỗi, hoặc -1 nếu không có.
 */
int err_code_from_name(const char *name);

/**
 * @function log_msg: In một dòng log ra stderr kèm giờ và nhãn.
 *
 * @param tag: Nhãn, ví dụ "tracker".
 * @param fmt: Định dạng kiểu printf.
 *
 * @return: void
 */
void log_msg(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* UTIL_H */
