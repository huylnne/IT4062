#ifndef SHARES_H
#define SHARES_H

#include "protocol.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* Số file tối đa một peer chia sẻ */
#define MAX_SHARES 256

/**
 * Một file mình đang chia sẻ: tên công bố -> đường dẫn thật trên đĩa.
 */
typedef struct {
    char name[MAX_FILENAME + 1];
    char path[PATH_MAX];
    uint64_t size;
} share_t;

/**
 * @function shares_add: Thêm file đã được Tracker chấp nhận.
 *
 * @param name: Tên công bố.
 * @param path: Đường dẫn tuyệt đối.
 * @param size: Dung lượng lúc chia sẻ.
 *
 * @return: 0 nếu thành công, -1 nếu đầy.
 */
int shares_add(const char *name, const char *path, uint64_t size);

/**
 * @function shares_find: Tìm theo tên công bố.
 *
 * @param name: Tên.
 *
 * @return: Mục, hoặc NULL.
 */
const share_t *shares_find(const char *name);

/**
 * @function shares_remove: Gỡ theo tên công bố.
 *
 * @param name: Tên.
 *
 * @return: void
 */
void shares_remove(const char *name);

size_t shares_count(void);
const share_t *shares_at(size_t idx);

#endif /* SHARES_H */
