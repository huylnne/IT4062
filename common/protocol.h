#ifndef PROTOCOL_H
#define PROTOCOL_H

/**
 * Hằng số giao thức chia sẻ file qua Tracker (dùng chung tracker, peer, tests).
 * Định dạng payload từng loại thông điệp: docs/protocol.md
 */

/* Header frame cố định 22 byte */
#define PROTO_MAGIC 0xF5A1
#define PROTO_VERSION 1
#define HDR_SIZE 22

/* Payload tối đa của một frame */
#define MAX_PAYLOAD (64 * 1024)

/* Truyền file */
#define CHUNK_SIZE (16 * 1024)
#define WINDOW_W 8
#define MAX_SESSIONS_PER_SRC 2
#define MAX_FILE_SIZE (2ULL << 30)

/* Thời gian chờ (ms) */
#define ACK_TIMEOUT_MS 10000
#define MAX_RETRY 3
#define PARTIAL_FRAME_MS 5000
#define NO_PROGRESS_MS 30000

/* Hàng đợi gửi tối đa của một kết nối (byte) */
#define SENDQ_LIMIT (1024 * 1024)

/* Bit cờ trong header */
#define FLAG_LAST 0x01

/* Giới hạn chuỗi */
#define MIN_USERNAME 3
#define MAX_USERNAME 32
#define MAX_FILENAME 255

/**
 * Loại thông điệp (trường type của header).
 */
enum msg_type {
    MSG_JOIN = 0x01,
    MSG_QUIT = 0x02,
    MSG_SHARE = 0x03,
    MSG_UNSHARE = 0x04,
    MSG_LIST = 0x05,
    MSG_SEARCH = 0x06,
    MSG_GET = 0x07,

    MSG_OK = 0x10,
    MSG_ERR = 0x11,
    MSG_LIST_RESP = 0x12,

    MSG_SEND_REQ = 0x20,
    MSG_GET_OK = 0x21,
    MSG_BUSY = 0x22,

    MSG_CHUNK = 0x30,
    MSG_ACK = 0x31,
    MSG_NACK = 0x32,
    MSG_REQ_RANGE = 0x33,
    MSG_DONE = 0x34,

    MSG_CANCEL = 0x40,
    MSG_ABORT = 0x41,
    MSG_SOURCE_GONE = 0x42
};

/**
 * Mã lỗi trong payload của ERR.
 */
enum err_code {
    E_BAD_FRAME = 1,
    E_NOT_JOINED = 2,
    E_ALREADY_JOINED = 3,
    E_NAME_TAKEN = 4,
    E_BAD_USERNAME = 5,
    E_BAD_FILENAME = 6,
    E_BAD_SIZE = 7,
    E_ALREADY_SHARED = 8,
    E_NOT_FOUND = 9,
    E_OWNER_OFFLINE = 10,
    E_SELF_GET = 11,
    E_TIMEOUT = 12,
    E_NO_PROGRESS = 13,
    E_FILE_IO = 14,
    E_HASH_MISMATCH = 15,
    E_UNKNOWN_TYPE = 16,
    E_FULL = 17
};

#define ERR_CODE_MAX 17

#endif /* PROTOCOL_H */
