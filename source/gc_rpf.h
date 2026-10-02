/* gc_rpf.h —— RPF7 档案读写（专用于 gameconfig.xml 的纯字节原位替换）
 *
 * 设计要点：
 *   · 只做【单条目原位替换】，绝不重建容器
 *   · 文件大小、其它条目偏移全部不变
 *   · 目标条目是 zlib 压缩存储时，写回原偏移 + 补零
 *
 * RPF7 格式（实测确认）：
 *   header(16B) = u32 Magic(0x52504637) | u32 EntryCount | u32 NamesLen | u32 Encryption
 *   TOC: EntryCount × 16B
 *     qword0 = NameOffset:16 | OnDiskSize:24 | Offset:23 | IsResource:1
 *     dword8 = Size（解压后长度）
 *     dwordC = DecryptionTag / EntryCount（目录用）
 *   目录判定：非资源 且 Offset == 0x7FFFFF
 */
#ifndef GC_RPF_H
#define GC_RPF_H

#include <switch.h>
#include <stdio.h>

#define GC_RPF_PATH_MAX   512
#define GC_RPF_MAGIC      0x52504637u
#define GC_ON_DISK_MAX    16777215u    /* on_disk 是 24 位字段 => 16 MB 上限 */

/* 一个 RPF 条目 */
typedef struct {
    int  idx;              /* 条目索引（改 TOC 时用） */
    u32  name_off;
    u32  on_disk;          /* 压缩后占用字节数（0 = 未压缩） */
    u32  offset;           /* 扇区号，字节偏移 = offset * 512 */
    u32  size;             /* 解压后长度 */
    u32  first;            /* 目录：首条目索引 */
    u32  count;            /* 目录：条目数 */
    int  is_res;
    int  is_dir;
    char name[128];
} GcEntry;

/* 打开的 RPF */
typedef struct {
    FILE *fp;
    char  path[GC_RPF_PATH_MAX];
    u32   entry_count;
    u32   names_len;
    u32   encryption;
    u32   name_shift;
    u8   *toc;             /* entry_count × 16 字节原始 TOC（原样保留） */
    u8   *names;           /* 名字池 */
    size_t file_size;
    int   writable;        /* ★ 是否以可写模式打开（"r+b" 成功 = 1） */
} GcRpf;

/* 返回值约定：0 = 成功，负 = 失败 */
#define GC_OK              0
#define GC_ERR_OPEN       -1
#define GC_ERR_FORMAT     -2
#define GC_ERR_NOMEM      -3
#define GC_ERR_NOTFOUND   -4
#define GC_ERR_INFLATE    -5
#define GC_ERR_DEFLATE    -6
#define GC_ERR_TOOBIG     -7
#define GC_ERR_IO         -8
#define GC_ERR_READONLY   -9    /* ★ 文件是只读打开的（或 SD 卡写保护） */

int  gc_rpf_open(const char *path, GcRpf *r);
void gc_rpf_close(GcRpf *r);
const char *gc_rpf_last_error(void);     /* ★ 最近一次失败原因（给界面显示） */
int  gc_rpf_selftest(char *out, size_t out_sz);   /* ★ 路径自检（上机排查用） */
int  gc_rpf_entry(const GcRpf *r, int i, GcEntry *e);
int  gc_rpf_find(GcRpf *r, const char *fname, GcEntry *out);
int  gc_rpf_read(GcRpf *r, const GcEntry *e, u8 **out, size_t *out_len);
int  gc_rpf_gap(const GcRpf *r, const GcEntry *e);   /* 该条目到下一个条目的物理空隙 */
/* ★★ 该条目真正能用的物理可用空间（= 到下一条目起始，按 24 位截断）
 *    修「超出容量」的关键：容量判据必须用它，不能用当前 on_disk */
int  gc_rpf_room(const GcRpf *r, const GcEntry *e);
int  gc_rpf_writable(const GcRpf *r);                /* ★ 是否可写（0 = 只读打开） */
int  gc_rpf_write(GcRpf *r, const GcEntry *e, const u8 *data, size_t len);
/* ★★ 带【溢出重定位】的写入（DLC 的 dlclist.xml 必须用它）
 *    原位装不下时，自动把条目搬到文件里的空闲区并改 TOC 的 offset。
 *    背景：dlclist.xml 在 update.rpf 里只有 512 B 物理余量（已用 491 B），
 *          加 3 个 DLC 就爆 —— 必须能搬家，否则「DLC 导入」功能不可用。
 *    allow_relocate=0 时行为等同 gc_rpf_write（装不下就报错）。
 *    e 非 const：搬家后会回写新的 offset/on_disk/size 供调用方后续校验。 */
int  gc_rpf_write_ex(GcRpf *r, GcEntry *e, const u8 *data, size_t len,
                     int allow_relocate);
/* 找一块 >= need 字节的空闲区，返回扇区号（0 = 没找到） */
u32  gc_rpf_find_free(GcRpf *r, u32 need);
/* ★ v26：写入前重叠二次校验（1 = 空闲安全，0 = 与已有条目重叠，禁止写入） */
int  gc_rpf_check_free(GcRpf *r, u32 sec, u32 need);
int  gc_rpf_verify(GcRpf *r, const GcEntry *e, const u8 *expect, size_t len);

/* XML 工具（给 gameconfig 用） */
size_t gc_strip_comments(char *s, size_t len);       /* 原地把注释换成空格（长度不变） */
size_t gc_drop_comments(char *s, size_t len);        /* ★ 真删注释（长度变短，用于瘦身） */
size_t gc_squeeze(char *s, size_t len);              /* ★ 再压标签间空白（更激进） */
/* ★ 查 key 对应的 gameconfig 容器路径（如 "ConfigPopulation/VehicleSpacing"）
 *   新增项必须插进对应容器，否则游戏读不到（表现为"预设没效果"） */
const char *gc_key_container(const char *key);
int    gc_xml_find_span(const char *xml, size_t len, const char *plat,
                        size_t *start, size_t *end);
int    gc_xml_set_int(char *xml, size_t cap, size_t *len,
                      size_t span_a, size_t span_b,
                      const char *key, int value, int *inserted);
/* ★ 带自动重定位：可安全连续调用（插入后段区间会变） */
int    gc_xml_set_int_ex(char *xml, size_t cap, size_t *len,
                         size_t *span_a, size_t *span_b,
                         const char *span_name,
                         const char *key, int value, int *inserted);
int    gc_xml_get_int(const char *xml, size_t len,
                      size_t span_a, size_t span_b,
                      const char *key, int *out);
int    gc_xml_count(const char *xml, size_t len, size_t a, size_t b);

/* 默认路径候选（按顺序尝试） */
#define GC_NUM_CANDIDATES 3
extern const char *gc_rpf_candidates[GC_NUM_CANDIDATES];

#endif /* GC_RPF_H */
