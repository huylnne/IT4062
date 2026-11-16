#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/*
 * Unit test cho common/: frame, wbuf/rbuf, crc32, sha256, valid_*.
 */

#include "crc32.h"
#include "frame.h"
#include "protocol.h"
#include "sha256.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total;
static int g_fail;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_total++;                                                        \
        if (!(cond)) {                                                    \
            g_fail++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static void test_header(void)
{
    frame_hdr_t h, g;
    uint8_t buf[HDR_SIZE];

    hdr_init(&h, MSG_CHUNK, 0x01020304, 0x0A0B0C0D);
    h.flags = FLAG_LAST;
    h.length = 0x00001234;
    h.crc32 = 0xCAFEBABE;
    hdr_pack(&h, buf);

    /* Kiểm tra đúng vị trí và thứ tự byte theo bảng mục 4.1 */
    const uint8_t want[HDR_SIZE] = {
        0xF5, 0xA1, 0x01, MSG_CHUNK, FLAG_LAST, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x0A, 0x0B, 0x0C, 0x0D,
        0x00, 0x00, 0x12, 0x34, 0xCA, 0xFE, 0xBA, 0xBE
    };
    CHECK(memcmp(buf, want, HDR_SIZE) == 0);

    CHECK(hdr_unpack(buf, &g) == FRAME_OK);
    CHECK(g.type == MSG_CHUNK && g.flags == FLAG_LAST);
    CHECK(g.transfer_id == 0x01020304 && g.chunk_id == 0x0A0B0C0D);
    CHECK(g.length == 0x1234 && g.crc32 == 0xCAFEBABE);

    buf[0] = 0x00;
    CHECK(hdr_unpack(buf, &g) == FRAME_BAD_MAGIC);
    buf[0] = 0xF5;
    buf[2] = 2;
    CHECK(hdr_unpack(buf, &g) == FRAME_BAD_VERSION);
    buf[2] = 1;

    h.length = MAX_PAYLOAD;
    hdr_pack(&h, buf);
    CHECK(hdr_unpack(buf, &g) == FRAME_OK);
    h.length = MAX_PAYLOAD + 1;
    hdr_pack(&h, buf);
    CHECK(hdr_unpack(buf, &g) == FRAME_TOO_LONG);
}

static void test_payload(void)
{
    uint8_t buf[64];
    wbuf_t w;
    rbuf_t r;
    char s[16];

    wbuf_init(&w, buf, sizeof(buf));
    put_u8(&w, 7);
    put_u16(&w, 0xBEEF);
    put_u32(&w, 0xDEADBEEF);
    put_u64(&w, 0x0123456789ABCDEFULL);
    put_str(&w, "alice");
    CHECK(!w.err && w.len == 1 + 2 + 4 + 8 + 1 + 5);

    rbuf_init(&r, buf, w.len);
    CHECK(get_u8(&r) == 7);
    CHECK(get_u16(&r) == 0xBEEF);
    CHECK(get_u32(&r) == 0xDEADBEEF);
    CHECK(get_u64(&r) == 0x0123456789ABCDEFULL);
    CHECK(get_str(&r, s, sizeof(s)) == 5 && strcmp(s, "alice") == 0);
    CHECK(rbuf_done(&r));

    /* Đọc quá cuối payload */
    CHECK(get_u8(&r) == 0 && r.err);

    /* Chuỗi dài hơn cap */
    rbuf_init(&r, buf + 15, 6);
    CHECK(get_str(&r, s, 4) < 0);

    /* Khai len lớn hơn phần còn lại */
    uint8_t bad[] = { 10, 'a', 'b' };
    rbuf_init(&r, bad, sizeof(bad));
    CHECK(get_str(&r, s, sizeof(s)) < 0);

    /* Chuỗi chứa '\0' bị từ chối */
    uint8_t nul[] = { 3, 'a', 0, 'b' };
    rbuf_init(&r, nul, sizeof(nul));
    CHECK(get_str(&r, s, sizeof(s)) < 0);

    /* Payload thừa byte thì rbuf_done = 0 */
    rbuf_init(&r, buf, 3);
    get_u8(&r);
    CHECK(!rbuf_done(&r));

    /* Ghi tràn */
    uint8_t tiny[3];
    wbuf_init(&w, tiny, sizeof(tiny));
    put_u32(&w, 1);
    CHECK(w.err);
}

static void test_crc32(void)
{
    CHECK(crc32_calc("123456789", 9) == 0xCBF43926u);
    CHECK(crc32_calc("", 0) == 0);
    uint32_t c = crc32_update(0, "1234", 4);
    c = crc32_update(c, "56789", 5);
    CHECK(c == 0xCBF43926u);
}

static int sha_hex_eq(const void *data, size_t n, const char *hex)
{
    sha256_ctx_t ctx;
    uint8_t d[SHA256_LEN];
    char out[2 * SHA256_LEN + 1];
    sha256_init(&ctx);
    sha256_update(&ctx, data, n);
    sha256_final(&ctx, d);
    sha256_hex(d, out);
    return strcmp(out, hex) == 0;
}

static void test_sha256(void)
{
    CHECK(sha_hex_eq("", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_hex_eq("abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m56 =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha_hex_eq(m56, strlen(m56),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* 1 triệu chữ 'a', cập nhật từng mẩu lệch 64 byte */
    sha256_ctx_t ctx;
    uint8_t d[SHA256_LEN];
    char out[2 * SHA256_LEN + 1];
    char block[1000];
    memset(block, 'a', sizeof(block));
    sha256_init(&ctx);
    for (int i = 0; i < 1000; i++)
        sha256_update(&ctx, block, sizeof(block));
    sha256_final(&ctx, d);
    sha256_hex(d, out);
    CHECK(strcmp(out, "cdc76e5c9914fb9281a1c7e284d73e67"
                      "f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_validate(void)
{
    CHECK(valid_username("alice"));
    CHECK(valid_username("a_1"));
    CHECK(!valid_username("ab"));
    CHECK(!valid_username("bad name"));
    CHECK(!valid_username("tên"));
    CHECK(!valid_username("abcdefghijabcdefghijabcdefghijabc")); /* 33 */
    CHECK(valid_username("abcdefghijabcdefghijabcdefghijab"));   /* 32 */

    CHECK(valid_filename("report.pdf"));
    CHECK(valid_filename("my file (1).txt"));
    CHECK(!valid_filename(""));
    CHECK(!valid_filename("."));
    CHECK(!valid_filename(".."));
    CHECK(!valid_filename("../../etc/passwd"));
    CHECK(!valid_filename("a\\b"));

    char longname[300];
    memset(longname, 'x', sizeof(longname));
    longname[255] = '\0';
    CHECK(valid_filename(longname));
    longname[255] = 'x';
    longname[256] = '\0';
    CHECK(!valid_filename(longname));
}

static void test_names(void)
{
    CHECK(strcmp(msg_type_name(MSG_REQ_RANGE), "REQ_RANGE") == 0);
    CHECK(msg_type_from_name("source_gone") == MSG_SOURCE_GONE);
    CHECK(msg_type_from_name("nope") == -1);
    CHECK(strcmp(err_code_name(E_NAME_TAKEN), "NAME_TAKEN") == 0);
    CHECK(err_code_from_name("unknown_type") == E_UNKNOWN_TYPE);
}

int main(void)
{
    test_header();
    test_payload();
    test_crc32();
    test_sha256();
    test_validate();
    test_names();

    printf("unit_test: %d/%d PASS\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
