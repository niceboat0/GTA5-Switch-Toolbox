#include "save.h"
#include <string.h>
#include <stdlib.h>
#include <mbedtls/aes.h>

/* ---------------------------------------------------------------------------
 * Public Rockstar AES-256-ECB keys (verified from hzhreal/HTOS rstar_crypt.py)
 * PC_KEY is used for GTA V PC saves; PS4_KEY for PS4 saves. The Switch port
 * is reported by the user to share the PC SGTA format, hence PC_KEY.
 * ------------------------------------------------------------------------- */
const uint8_t GTA5_PC_KEY[32] = {
    0x46, 0xed, 0x8d, 0x3f, 0x94, 0x35, 0xe4, 0xec,
    0x12, 0x2c, 0xb2, 0xe2, 0xaf, 0x97, 0xc5, 0x7e,
    0x4c, 0x5a, 0x8c, 0x30, 0x92, 0xc7, 0x84, 0x4e,
    0x11, 0xc6, 0x86, 0xff, 0x41, 0xdf, 0x41, 0x0f
};

const uint8_t GTA5_PS4_KEY[32] = {
    0x16, 0x85, 0xff, 0xa3, 0x8d, 0x01, 0x0f, 0x0d,
    0xfe, 0x66, 0x1c, 0xf9, 0xb5, 0x57, 0x2c, 0x50,
    0x0d, 0x80, 0x26, 0x48, 0xdb, 0x37, 0xb9, 0xed,
    0x0f, 0x48, 0xc5, 0x73, 0x42, 0xc0, 0x22, 0xf5
};

static const uint8_t GTA5_HEADER_MAGIC[4] = { 'P','S','I','N' };

/* Per-character 4-byte identifier; the money value sits 4 bytes after it. */
static const uint8_t MONEY_ID[3][4] = {
    { 0x44, 0xBD, 0x69, 0x82 }, /* Franklin */
    { 0x03, 0x24, 0xC3, 0x1D }, /* Michael  */
    { 0x8D, 0x75, 0x04, 0x7D }  /* Trevor   */
};

/* ---------------------------------------------------------------------------
 * JOOAT checksum (Rockstar custom). uint32, wraps mod 2^32.
 * ------------------------------------------------------------------------- */
static uint32_t jooat(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t num = seed;
    for (size_t i = 0; i < len; i++) {
        int32_t ch = (int32_t)(int8_t)data[i]; /* signed char */
        num = (uint32_t)(num + ch);
        num = (uint32_t)(num + (num << 10));
        num = (uint32_t)(num ^ (num >> 6));
    }
    num = (uint32_t)(num + (num << 3));
    num = (uint32_t)(num ^ (num >> 11));
    num = (uint32_t)(num + (num << 15));
    return num;
}

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* ---------------------------------------------------------------------------
 * AES-256-ECB via mbedtls. `encrypt` 1 = encrypt, 0 = decrypt.
 * ------------------------------------------------------------------------- */
static int aes256_ecb(const uint8_t *in, size_t len, const uint8_t *key,
                      uint8_t *out, int encrypt)
{
    if (len % 16 != 0) return -1;
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc;
    if (encrypt)
        rc = mbedtls_aes_setkey_enc(&ctx, key, 256);
    else
        rc = mbedtls_aes_setkey_dec(&ctx, key, 256);
    if (rc != 0) { mbedtls_aes_free(&ctx); return -2; }

    for (size_t i = 0; i < len; i += 16) {
        if (encrypt)
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in + i, out + i);
        else
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in + i, out + i);
    }
    mbedtls_aes_free(&ctx);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Title checksum: lives at offset 0x104 in the plaintext header.
 * seed = jooat(reverse(file[0:4]), 0); chks = jooat(file[4:0x104], seed).
 * ------------------------------------------------------------------------- */
static void fix_title_checksum(uint8_t *file)
{
    uint8_t seed_data[4] = { file[3], file[2], file[1], file[0] };
    uint32_t seed = jooat(seed_data, 4, 0);
    uint32_t chks = jooat(file + 4, 0x100, seed);
    write_u32_be(file + 0x104, chks);
}

/* ---------------------------------------------------------------------------
 * Block "CHKS" checksums inside the body (plaintext). Faithful port of the
 * HTOS rstar_crypt logic. For each "CHKS\x00" occurrence:
 *   header_size = u32be at chunk+4
 *   data_length = u32be at chunk+8
 *   block = body[chunk + header_size - data_length : chunk + header_size]
 *   zero 8 bytes at block offset (data_length - header_size + 8)
 *   new_hash = jooat(block, 0x3FAC7125)
 *   write new_hash (u32be) at body[chunk+12]
 * ------------------------------------------------------------------------- */
static void fix_block_checksums(uint8_t *body, size_t body_len)
{
    size_t chunk = 0;
    while (chunk + 5 <= body_len) {
        if (body[chunk]   == 'C' && body[chunk+1] == 'H' &&
            body[chunk+2] == 'K' && body[chunk+3] == 'S' &&
            body[chunk+4] == 0x00) {

            if (chunk + 12 > body_len) break;
            uint32_t header_size = read_u32_be(body + chunk + 4);
            uint32_t data_length  = read_u32_be(body + chunk + 8);

            int64_t start = (int64_t)chunk + (int64_t)header_size - (int64_t)data_length;
            int64_t end   = (int64_t)chunk + (int64_t)header_size;
            if (start < 0) start = 0;
            if (end   > (int64_t)body_len) end = (int64_t)body_len;
            if (end - start <= 0) { chunk += 5; continue; }

            size_t blen = (size_t)(end - start);
            uint8_t *blk = (uint8_t *)malloc(blen);
            if (!blk) { chunk += 5; continue; }

            memcpy(blk, body + start, blen);
            int64_t zoff = (int64_t)data_length - (int64_t)header_size + 8;
            if (zoff >= 0 && (size_t)zoff + 8 <= blen)
                memset(blk + zoff, 0, 8);

            uint32_t h = jooat(blk, blen, 0x3FAC7125u);
            free(blk);

            write_u32_be(body + chunk + 12, h);
            chunk += 5;
        } else {
            chunk++;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int gta5_detect(const uint8_t *file, size_t size,
                gta5_platform *out_platform, size_t *out_start,
                int *out_already_decrypted)
{
    if (size < GTAV_PS4_OFFSET + 4) return -1;

    const uint8_t *at_pc = file + GTAV_PC_OFFSET;

    if (memcmp(at_pc, GTA5_HEADER_MAGIC, 4) == 0) {
        /* Already decrypted (PC). */
        *out_platform = GTA5_PLATFORM_PC;
        *out_start = GTAV_PC_OFFSET;
        *out_already_decrypted = 1;
        return 0;
    }
    if (memcmp(at_pc, "\x00\x00\x00\x00", 4) == 0) {
        /* PS4 save: magic is at 0x114 instead. */
        *out_platform = GTA5_PLATFORM_PS4;
        *out_start = GTAV_PS4_OFFSET;
        *out_already_decrypted = 0;
        return 0;
    }
    /* Default: PC encrypted. */
    *out_platform = GTA5_PLATFORM_PC;
    *out_start = GTAV_PC_OFFSET;
    *out_already_decrypted = 0;
    return 0;
}

int gta5_decrypt_body(uint8_t *file, size_t size,
                      gta5_platform platform, size_t start)
{
    if (size <= start) return -1;
    size_t body_len = size - start;
    if (body_len % 16 != 0) return -2;

    const uint8_t *key = (platform == GTA5_PLATFORM_PS4) ? GTA5_PS4_KEY : GTA5_PC_KEY;
    uint8_t *body = file + start;

    /* Decrypt in-place. */
    if (aes256_ecb(body, body_len, key, body, 0) != 0) return -3;

    /* Sanity: decrypted body must begin with "PSIN". */
    if (memcmp(body, GTA5_HEADER_MAGIC, 4) != 0) return -4;
    return 0;
}

int gta5_encrypt_body(uint8_t *file, size_t size,
                      gta5_platform platform, size_t start)
{
    if (size <= start) return -1;
    size_t body_len = size - start;
    if (body_len % 16 != 0) return -2;

    const uint8_t *key = (platform == GTA5_PLATFORM_PS4) ? GTA5_PS4_KEY : GTA5_PC_KEY;
    uint8_t *body = file + start;

    /* Must currently be plaintext (begins with PSIN). */
    if (memcmp(body, GTA5_HEADER_MAGIC, 4) != 0) return -3;

    fix_title_checksum(file);
    fix_block_checksums(body, body_len);

    if (aes256_ecb(body, body_len, key, body, 1) != 0) return -4;
    return 0;
}

long gta5_find_money(const uint8_t *file, size_t size, size_t start,
                     gta5_char who)
{
    if (who > GTA5_CHAR_TREVOR) return -1;
    if (size <= start + 4) return -1;

    const uint8_t *body = file + start;
    size_t body_len = size - start;

    /* Search for the first occurrence of the 4-byte identifier. */
    for (size_t i = 0; i + 4 <= body_len; i++) {
        if (memcmp(body + i, MONEY_ID[who], 4) == 0) {
            /* Money value is 4 bytes after the identifier. */
            size_t moff = i + 4;
            if (moff + 4 > body_len) return -1;
            return (long)(start + moff);
        }
    }
    return -1;
}

int gta5_get_money(const uint8_t *file, size_t size, size_t start,
                   gta5_char who, uint32_t *out)
{
    long off = gta5_find_money(file, size, start, who);
    if (off < 0) return -1;
    *out = read_u32_be(file + off);
    return 0;
}

int gta5_set_money(uint8_t *file, size_t size, size_t start,
                   gta5_char who, uint32_t value)
{
    if (value > GTA5_MONEY_LIMIT) value = GTA5_MONEY_LIMIT;
    long off = gta5_find_money(file, size, start, who);
    if (off < 0) return -1;
    write_u32_be(file + off, value);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Generic field primitives (v2.0)
 * ------------------------------------------------------------------------- */
long gta5_find_id(const uint8_t *file, size_t size, size_t start,
                  const uint8_t *id, size_t id_len)
{
    if (!file || !id || id_len == 0 || size <= start || size - start < id_len)
        return -1;
    for (size_t i = start; i + id_len <= size; i++) {
        if (memcmp(file + i, id, id_len) == 0)
            return (long)i;
    }
    return -1;
}

long gta5_find_value(const uint8_t *file, size_t size, size_t start,
                     const uint8_t *id, size_t id_len, size_t value_after)
{
    long m = gta5_find_id(file, size, start, id, id_len);
    if (m < 0) return -1;
    long voff = m + (long)value_after;
    if (voff < 0 || (size_t)voff >= size) return -1;
    return voff;
}

static int pack_bounds(size_t size, long off, int width, size_t *need)
{
    if (width != 1 && width != 2 && width != 4 && width != 8) return -1;
    if (off < 0) return -1;
    size_t u = (size_t)off;
    size_t n = (size_t)width;
    if (n > size || u > size - n) return -1;
    *need = n;
    return 0;
}

int gta5_read_value(const uint8_t *file, size_t size, long off,
                    int width, int endian, uint64_t *out)
{
    size_t n;
    if (!file || !out) return -1;
    if (pack_bounds(size, off, width, &n) != 0) return -1;
    const uint8_t *p = file + (size_t)off;
    uint64_t v = 0;
    if (endian) { /* little-endian */
        for (size_t i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    } else {     /* big-endian */
        for (size_t i = 0; i < n; i++) v = (v << 8) | p[i];
    }
    *out = v;
    return 0;
}

int gta5_write_value(uint8_t *file, size_t size, long off,
                     int width, int endian, uint64_t value)
{
    size_t n;
    if (!file) return -1;
    if (pack_bounds(size, off, width, &n) != 0) return -1;
    uint8_t *p = file + (size_t)off;
    if (endian) {
        for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(value >> (8 * i));
    } else {
        for (size_t i = 0; i < n; i++) p[n - 1 - i] = (uint8_t)(value >> (8 * i));
    }
    return 0;
}

size_t gta5_scan_value(const uint8_t *file, size_t size, size_t start,
                       int width, int endian, uint64_t value,
                       long *offs, size_t cap)
{
    if (!file || (width != 1 && width != 2 && width != 4 && width != 8)) return 0;
    size_t n = (size_t)width, cnt = 0;
    for (size_t i = start; i + n <= size; i++) {
        uint64_t v = 0;
        if (endian) {
            for (size_t b = 0; b < n; b++) v |= (uint64_t)file[i + b] << (8 * b);
        } else {
            for (size_t b = 0; b < n; b++) v = (v << 8) | file[i + b];
        }
        if (v == value) {
            if (offs && cnt < cap) offs[cnt] = (long)i;
            cnt++;
        }
    }
    return cnt;
}
