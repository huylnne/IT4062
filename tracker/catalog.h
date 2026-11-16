#ifndef CATALOG_H
#define CATALOG_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

/* Số file tối đa trong catalog */
#define MAX_FILES 4096

/**
 * Một file đang được chia sẻ; khoá là cặp (owner, name).
 */
typedef struct {
    char owner[MAX_USERNAME + 1];
    char name[MAX_FILENAME + 1];
    uint64_t size;
} cat_entry_t;

/**
 * Mã trả về của catalog_add.
 */
enum {
    CAT_OK = 0,
    CAT_EXISTS = -1,
    CAT_FULL = -2
};

/**
 * @function catalog_add: Thêm (owner, name, size).
 *
 * @param owner: Username chủ file.
 * @param name: Tên file (đã kiểm tra hợp lệ).
 * @param size: Dung lượng.
 *
 * @return: CAT_OK, CAT_EXISTS hoặc CAT_FULL.
 */
int catalog_add(const char *owner, const char *name, uint64_t size);

/**
 * @function catalog_remove: Gỡ (owner, name).
 *
 * @param owner: Chủ file.
 * @param name: Tên file.
 *
 * @return: 0 nếu đã gỡ, -1 nếu không có.
 */
int catalog_remove(const char *owner, const char *name);

/**
 * @function catalog_remove_owner: Gỡ mọi file của owner (khi peer thoát).
 *
 * @param owner: Chủ file.
 *
 * @return: Số file đã gỡ.
 */
size_t catalog_remove_owner(const char *owner);

/**
 * @function catalog_find: Tìm (owner, name).
 *
 * @param owner: Chủ file.
 * @param name: Tên file.
 *
 * @return: Mục tìm thấy, hoặc NULL.
 */
const cat_entry_t *catalog_find(const char *owner, const char *name);

size_t catalog_count(void);
const cat_entry_t *catalog_at(size_t idx);

#endif /* CATALOG_H */
