#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GTA V PC / PS4 save format constants.
 * Source: public Rockstar save-crypto research (hzhreal/HTOS, gta5view).
 * Plaintext header lives in [0 : start_offset]; the body [start_offset : EOF]
 * is AES-256-ECB encrypted. After decryption the body begins with "PSIN". */

#define GTA5_MONEY_LIMIT   0x7FFFFFFFu   /* 2,147,483,647 */
#define GTAV_PC_OFFSET     0x108u
#define GTAV_PS4_OFFSET    0x114u
#define GTA5_HEADER_LEN    260u          /* 0x104 */

typedef enum {
    GTA5_PLATFORM_PC  = 0,
    GTA5_PLATFORM_PS4 = 1
} gta5_platform;

typedef enum {
    GTA5_CHAR_FRANKLIN = 0,
    GTA5_CHAR_MICHAEL  = 1,
    GTA5_CHAR_TREVOR   = 2
} gta5_char;

/* Public Rockstar AES-256-ECB keys (32 bytes each). */
extern const uint8_t GTA5_PC_KEY[32];
extern const uint8_t GTA5_PS4_KEY[32];

/* Detect platform / encryption state from the raw file buffer.
 * Returns 0 on success. *out_platform and *out_start set accordingly.
 * *out_already_decrypted is 1 if the body already starts with "PSIN". */
int gta5_detect(const uint8_t *file, size_t size,
                gta5_platform *out_platform, size_t *out_start,
                int *out_already_decrypted);

/* AES-256-ECB decrypt the encrypted body in-place.
 * file[0:start] is untouched (plaintext header). */
int gta5_decrypt_body(uint8_t *file, size_t size,
                      gta5_platform platform, size_t start);

/* Re-encrypt the (already modified) plaintext body in-place:
 *  - fix title checksum at offset 0x104 (plaintext header)
 *  - recompute every block "CHKS" checksum inside the body
 *  - AES-256-ECB encrypt the body
 * Call this ONLY after the body is plaintext (post-decrypt or already-decrypted). */
int gta5_encrypt_body(uint8_t *file, size_t size,
                     gta5_platform platform, size_t start);

/* Locate the 4-byte money value for a character inside the (decrypted) body.
 * Returns the absolute offset within `file`, or -1 if not found. */
long gta5_find_money(const uint8_t *file, size_t size, size_t start,
                     gta5_char who);

/* Read current money (call after decrypt). Returns 0 on success. */
int gta5_get_money(const uint8_t *file, size_t size, size_t start,
                   gta5_char who, uint32_t *out);

/* Write money (call on decrypted body, before encrypt). Value is capped
 * to GTA5_MONEY_LIMIT. Returns 0 on success. */
int gta5_set_money(uint8_t *file, size_t size, size_t start,
                   gta5_char who, uint32_t value);

/* ---------------------------------------------------------------------------
 * Generic field primitives (v2.0). These let the tool edit ANY feature whose
 * byte signature is known WITHOUT hardcoding version-specific offsets.
 *   - "pattern" style: a fixed byte string (id) appears right before the
 *     value field. Money works exactly like this (value 4 bytes after id).
 *   - value is stored with configurable width (1/2/4/8) and endianness.
 * All offsets are absolute within the whole save file buffer.
 * ------------------------------------------------------------------------- */

/* Find the first occurrence of `id` at or after `start`.
 * Returns the absolute offset of the match, or -1 if not found. */
long gta5_find_id(const uint8_t *file, size_t size, size_t start,
                  const uint8_t *id, size_t id_len);

/* Find the value field that belongs to `id`: the match of `id` plus
 * `value_after` bytes. Returns absolute offset of the field or -1. */
long gta5_find_value(const uint8_t *file, size_t size, size_t start,
                     const uint8_t *id, size_t id_len, size_t value_after);

/* Read / write an integer value at an absolute offset.
 * width: 1,2,4,8  endian: 0 = big-endian, 1 = little-endian.
 * Returns 0 on success, -1 on out-of-range / bad width. */
int gta5_read_value(const uint8_t *file, size_t size, long off,
                    int width, int endian, uint64_t *out);
int gta5_write_value(uint8_t *file, size_t size, long off,
                     int width, int endian, uint64_t value);

/* Scan [start, size) and record every absolute offset whose `width`-byte
 * big/little-endian value equals `value`. Returns match count; up to
 * `cap` offsets are stored in `offs`. */
size_t gta5_scan_value(const uint8_t *file, size_t size, size_t start,
                       int width, int endian, uint64_t value,
                       long *offs, size_t cap);

#ifdef __cplusplus
}
#endif
