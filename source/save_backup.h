#ifndef TOOLBOX_SAVE_BACKUP_H
#define TOOLBOX_SAVE_BACKUP_H
#include <stddef.h>
typedef struct {
    const char *stage;
    int code;
    char path[640];
} SaveIoError;
/* Copy the current on-disk slot without overwriting an existing backup.
 * Returns 0 on success, -1 on any read/write/close failure. */
int save_backup_copy(const char *source, const char *destination);
int save_backup_copy_ex(const char *source, const char *destination, SaveIoError *error);
/* Create an exclusive snapshot, including missing parent directories. */
int save_backup_create(const char *source, const char *root, const char *stamp,
                       char *destination, size_t capacity, SaveIoError *error);
int save_io_write(const char *path, const void *data, size_t size, SaveIoError *error);
/* Only the detected game save directory is a settings write target. */
int save_settings_write(const char *directory, const void *data, size_t size, SaveIoError *error);
#endif
