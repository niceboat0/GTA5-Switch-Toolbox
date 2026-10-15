#ifndef TOOLBOX_SAVE_BACKUP_H
#define TOOLBOX_SAVE_BACKUP_H
/* Copy the current on-disk slot without overwriting an existing backup.
 * Returns 0 on success, -1 on any read/write/close failure. */
int save_backup_copy(const char *source, const char *destination);
#endif
