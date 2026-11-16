#ifndef FRAME_H
#define FRAME_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

/**
 * Header 22 byte của mọi frame (dạng đã giải mã, theo thứ tự byte của máy).
 */
typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t reserved;
    uint32_t transfer_id;
    uint32_t chunk_id;
    uint32_t length;
    uint32_t crc32;
} frame_hdr_t;

/**
 * Mã trả về của hdr_unpack.
 */
enum {
    FRAME_OK = 0,
    FRAME_BAD_MAGIC = -1,
    FRAME_BAD_VERSION = -2,
    FRAME_TOO_LONG = -3
};

/**
 * @function hdr_init: Điền magic, version; các trường còn lại theo tham số,
 * length = crc32 = flags = 0.
 *
 * @param h: Header cần khởi tạo.
 * @param type: Loại thông điệp (enum msg_type).
 * @param transfer_id: Mã phiên, 0 nếu không thuộc phiên nào.
 * @param chunk_id: Số thứ tự chunk, 0 nếu không dùng.
 *
 * @return: void
 */
void hdr_init(frame_hdr_t *h, uint8_t type, uint32_t transfer_id,
              uint32_t chunk_id);

/**
 * @function hdr_pack: Ghi header ra 22 byte theo network byte order.
 *
 * @param h: Header nguồn.
 * @param out: Vùng nhớ đích, đúng HDR_SIZE byte.
 *
 * @return: void
 */
void hdr_pack(const frame_hdr_t *h, uint8_t out[HDR_SIZE]);

/**
 * @function hdr_unpack: Đọc 22 byte header và kiểm tra magic, version, length.
 *
 * @param in: 22 byte header nhận từ mạng.
 * @param h: Header đích.
 *
 * @return: FRAME_OK, FRAME_BAD_MAGIC, FRAME_BAD_VERSION hoặc FRAME_TOO_LONG.
 */
int hdr_unpack(const uint8_t in[HDR_SIZE], frame_hdr_t *h);

/**
 * Bộ ghi payload tuần tự. err = 1 nếu đã có lần ghi tràn cap.
 */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int err;
} wbuf_t;

void wbuf_init(wbuf_t *w, void *buf, size_t cap);
void put_u8(wbuf_t *w, uint8_t v);
void put_u16(wbuf_t *w, uint16_t v);
void put_u32(wbuf_t *w, uint32_t v);
void put_u64(wbuf_t *w, uint64_t v);
void put_bytes(wbuf_t *w, const void *p, size_t n);

/**
 * @function put_str: Ghi chuỗi dạng (u8 len, bytes), không kèm '\0'.
 *
 * @param w: Bộ ghi.
 * @param s: Chuỗi C; dài quá 255 byte thì đặt err.
 *
 * @return: void
 */
void put_str(wbuf_t *w, const char *s);

/**
 * Bộ đọc payload tuần tự. err = 1 nếu đã có lần đọc thiếu byte.
 */
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int err;
} rbuf_t;

void rbuf_init(rbuf_t *r, const void *p, size_t len);
uint8_t get_u8(rbuf_t *r);
uint16_t get_u16(rbuf_t *r);
uint32_t get_u32(rbuf_t *r);
uint64_t get_u64(rbuf_t *r);
int get_bytes(rbuf_t *r, void *out, size_t n);

/**
 * @function get_str: Đọc chuỗi (u8 len, bytes) vào out và thêm '\0'.
 *
 * @param r: Bộ đọc.
 * @param out: Bộ đệm đích.
 * @param cap: Dung lượng out; chuỗi dài >= cap thì báo lỗi.
 *
 * @return: Độ dài chuỗi, hoặc -1 nếu thiếu byte, chuỗi quá dài hoặc chứa '\0'.
 */
int get_str(rbuf_t *r, char *out, size_t cap);

/**
 * @function rbuf_done: Kiểm tra đã đọc hết payload và không có lỗi.
 *
 * @param r: Bộ đọc.
 *
 * @return: 1 nếu payload đúng khuôn (không thừa, không thiếu), 0 nếu không.
 */
int rbuf_done(const rbuf_t *r);

#endif /* FRAME_H */
