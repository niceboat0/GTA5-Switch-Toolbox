/* gc_script.c —— ★ v5.0: GTA V Switch 脚本 mod（.nsc）安装
 *
 * 流程（2026-09-30 用 HotCoffee-NX 实测验证，Python 版跑通后移植）：
 *   1. 读磁盘上的 .nsc → raw deflate 压缩（wbits=-15，与 RPF 规范一致）
 *   2. 打开 update2.rpf，解析顶层 TOC/names，定位 script_rel.rpf
 *   3. 读 script_rel.rpf 的内层 TOC/names
 *   4. 同名条目 ⇒ 替换；新名字 ⇒ 新增
 *   5. 落点优先用 script_rel.rpf 之后的【无引用空隙】（不改文件大小）；
 *      空隙不够则追加到文件末尾（会扩大文件）
 *   6. 🚨 先写 names 再写 TOC（新 TOC 会覆盖原 names 尾部）
 *   7. 扩大 script_rel.rpf 的 size 覆盖新数据 + 同步外层条目
 *   8. fsdevCommitDevice("sdmc") 落盘
 *
 * 🚨 铁律：
 *   · .nsc 必须存成 is_res=0 的普通压缩文件条目（不是 RSC7 资源）
 *   · 绝不覆盖任何既有字节 —— 只写「空隙」或「文件末尾」
 *   · 落点必须 512 对齐（RPF 的 offset 字段是扇区号）
 */
#include "gc_script.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <zlib.h>

#define RPF7_MAGIC 0x52504637u
#define MAX_TOP_ENT 4096
#define MAX_REL_ENT 4096

/* ---------------- 小工具 ---------------- */
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd_u64(const u8 *p) {
    return (u64)rd_u32(p) | ((u64)rd_u32(p + 4) << 32);
}
static void wr_u32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}
static void wr_u64(u8 *p, u64 v) {
    wr_u32(p, (u32)(v & 0xFFFFFFFFu));
    wr_u32(p + 4, (u32)(v >> 32));
}
/* ★ v5.6: 记录最近一次错误 —— gc_script_list 失败时调用者拿不到 err 缓冲，
 *   需要一个可随时查询的 "last error" 供 UI 显示（否则失败时界面一片空白，
 *   用户根本不知道是"没装"还是"读不到"）。 */
static char s_last_err[256] = {0};
static void set_err(char *err, size_t sz, const char *fmt, ...) {
    va_list ap;
    char tmp[256];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    snprintf(s_last_err, sizeof(s_last_err), "%s", tmp);
    if (!err || !sz) return;
    snprintf(err, sz, "%s", tmp);
}
const char *gc_script_last_error(void) { return s_last_err; }

/* ★ v6.2: RSC7 资源条目识别 —— 官方 .nsc 的真实形态
 *
 * 实测（用户提供的原版 update2.rpf 的 script_rel.rpf）：
 *   官方 error_listener.nsc        =    718 B  is_res=1  size=131072
 *   官方 achievement_controller.nsc= 31,460 B  is_res=1  size=6272
 *   官方 shop_controller.nsc       = 856,627 B  is_res=1  size=8519682
 *   ⇒ 官方脚本在 RPF 里是【RSC7 资源条目】，不是普通压缩文件条目！
 *
 * RSC7 结构：16 字节头 + raw deflate 流
 *   偏移 0: "RSC7"
 *   偏移 4: version (12)
 *   偏移 8: 虚拟大小 —— ★ 恰好等于 RPF TOC 的 size 字段（已验证 3/3 命中）
 *   偏移 12: 0xC0000000 风格标志
 *
 * 而第三方 mod（MEGATARD / HotCoffee）的 .nsc 是【裸 Switch payload】，
 *   头 = a8 b3 ?? ?? ?? 7f 00 00，必须存成 is_res=0 普通条目。
 *   （HotCoffee README 原话：不要做成 RPF 资源条目、不要加 RSC7 外壳）
 *
 * ⇒ 写入时必须按数据类型设置 is_res 位，否则引擎解析方式不匹配 ⇒ 崩。 */
static int is_rsc7(const u8 *d, size_t n) {
    return n >= 16 && d[0] == 'R' && d[1] == 'S' && d[2] == 'C' && d[3] == '7';
}

/* 统一释放：RSC7 分支里 comp == raw（不重复压缩），不能重复 free */
static void free_bufs(u8 *raw, u8 *comp) {
    if (comp && comp != raw) free(comp);
    if (raw) free(raw);
}

/* raw deflate 压缩；失败返回 NULL */
static u8 *deflate_raw(const u8 *in, u32 inlen, u32 *outlen) {
    uLongf cap = compressBound(inlen) + 64;
    u8 *out = (u8*)malloc(cap);
    if (!out) return NULL;
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(out); return NULL;
    }
    s.next_in = (Bytef*)in; s.avail_in = inlen;
    s.next_out = out;       s.avail_out = (uInt)cap;
    int rc = deflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&s); free(out); return NULL; }
    *outlen = (u32)s.total_out;
    deflateEnd(&s);
    return out;
}

/* ---------------- 头部读取 ---------------- */
typedef struct {
    u32 ec, nl, shift;
    u32 enc;               /* ★ P1: 头部加密类型（header+12） */
    long long names_off;   /* 绝对文件偏移 */
} Hdr;

static int read_hdr(FILE *f, long long base, Hdr *h) {
    u8 b[16];
    if (fseeko(f, base, SEEK_SET) != 0) return -1;
    if (fread(b, 1, 16, f) != 16) return -1;
    if (rd_u32(b) != RPF7_MAGIC) return -1;
    h->ec = rd_u32(b + 4);
    u32 raw = rd_u32(b + 8);
    h->nl = raw & 0x0FFFFFFFu;
    h->shift = raw >> 28;
    h->enc = rd_u32(b + 12);            /* ★ P1: 读加密类型 */
    if (h->ec == 0 || h->ec > MAX_TOP_ENT) return -1;
    if (h->nl == 0 || h->nl > 4u * 1024u * 1024u) return -1;
    h->names_off = base + 16 + (long long)h->ec * 16;
    return 0;
}

/* 在给定 names 缓冲里取完整名字 */
static void name_in(const u8 *names, u32 nl, u32 noff, u32 shift, char *out, size_t outsz) {
    out[0] = 0;
    u32 o = noff << shift;
    if (o >= nl) return;
    u32 n = 0;
    while (o + n < nl && names[o + n] && n < outsz - 1) { out[n] = (char)names[o + n]; n++; }
    out[n] = 0;
}

/* ---------------- 空隙查找 ---------------- */
typedef struct { long long s, e; } Iv;
static int cmp_iv(const void *a, const void *b) {
    long long x = ((const Iv*)a)->s, y = ((const Iv*)b)->s;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

/* ---------------- 核心：读 script_rel.rpf 位置 ---------------- */
static int locate_rel(FILE *f, long long *out_base, long long *out_size, long *out_idx) {
    Hdr h;
    if (read_hdr(f, 0, &h) != 0) return -1;
    u8 *toc = (u8*)malloc((size_t)h.ec * 16);
    u8 *names = (u8*)malloc(h.nl);
    if (!toc || !names) { free(toc); free(names); return -1; }
    if (fseeko(f, 16, SEEK_SET) != 0 || fread(toc, 1, (size_t)h.ec * 16, f) != (size_t)h.ec * 16) {
        free(toc); free(names); return -1;
    }
    if (fseeko(f, h.names_off, SEEK_SET) != 0 || fread(names, 1, h.nl, f) != h.nl) {
        free(toc); free(names); return -1;
    }
    int found = -1;
    for (u32 i = 0; i < h.ec; i++) {
        u64 q0 = rd_u64(toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        if (off == 0) continue;
        char nm[160];
        name_in(names, h.nl, (u32)(q0 & 0xFFFF), h.shift, nm, sizeof(nm));
        if (strcasecmp(nm, GC_SCRIPT_REL_RPF) == 0) {
            u32 d8 = rd_u32(toc + i * 16 + 8);
            u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
            *out_base = (long long)off * 512;
            *out_size = od ? od : d8;
            if (out_idx) *out_idx = (long)i;
            found = 0;
            break;
        }
    }
    free(toc); free(names);
    return found;
}

/* 找 base+osize 之后最近的条目起点（= 可用空隙上界） */
static long long next_entry_after(FILE *f, long long want, long long file_sz) {
    Hdr h;
    if (read_hdr(f, 0, &h) != 0) return file_sz;
    u8 *toc = (u8*)malloc((size_t)h.ec * 16);
    if (!toc) return file_sz;
    if (fseeko(f, 16, SEEK_SET) != 0 || fread(toc, 1, (size_t)h.ec * 16, f) != (size_t)h.ec * 16) {
        free(toc); return file_sz;
    }
    long long best = file_sz;
    for (u32 i = 0; i < h.ec; i++) {
        u64 q0 = rd_u64(toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        if (off == 0) continue;
        long long s = (long long)off * 512;
        if (s >= want && s < best) best = s;
    }
    free(toc);
    return best;
}

/* ---------------- 读 script_rel.rpf 的内层信息 ---------------- */
typedef struct {
    Hdr h;
    u8 *toc;      /* ec*16 */
    u8 *names;    /* nl */
    int idx_same; /* 已存在的同名条目索引，-1 = 无 */
    int root_first, root_count;
} RelInfo;

static void rel_free(RelInfo *ri) { free(ri->toc); free(ri->names); ri->toc = ri->names = NULL; }

static int rel_load(FILE *f, long long base, RelInfo *ri, const char *want_name) {
    memset(ri, 0, sizeof(*ri));
    ri->idx_same = -1;
    if (read_hdr(f, base, &ri->h) != 0) return -1;
    if (ri->h.ec > MAX_REL_ENT) return -1;
    ri->toc = (u8*)malloc((size_t)ri->h.ec * 16);
    ri->names = (u8*)malloc(ri->h.nl);
    if (!ri->toc || !ri->names) { rel_free(ri); return -1; }
    if (fseeko(f, base + 16, SEEK_SET) != 0 ||
        fread(ri->toc, 1, (size_t)ri->h.ec * 16, f) != (size_t)ri->h.ec * 16) { rel_free(ri); return -1; }
    if (fseeko(f, base + 16 + (long long)ri->h.ec * 16, SEEK_SET) != 0 ||
        fread(ri->names, 1, ri->h.nl, f) != ri->h.nl) { rel_free(ri); return -1; }

    for (u32 i = 0; i < ri->h.ec; i++) {
        u64 q0 = rd_u64(ri->toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) {
            ri->root_first = (int)rd_u32(ri->toc + i * 16 + 8);
            ri->root_count = (int)rd_u32(ri->toc + i * 16 + 12);
            continue;
        }
        if (want_name && want_name[0]) {
            char nm[GC_SCRIPT_NAME_MAX];
            name_in(ri->names, ri->h.nl, (u32)(q0 & 0xFFFF), ri->h.shift, nm, sizeof(nm));
            if (strcasecmp(nm, want_name) == 0) ri->idx_same = (int)i;
        }
    }
    return 0;
}

/* ---------------- 对外：列出已装脚本 ---------------- */
int gc_script_list(GcScriptItem *out, int max) {
    s_last_err[0] = 0;
    FILE *f = fopen(GC_SCRIPT_UPDATE2, "rb");
    if (!f) {
        set_err(NULL, 0, "打不开 update2.rpf (errno=%d)", errno);
        return GC_SCRIPT_ERR_OPEN;
    }
    long long base, osize;
    if (locate_rel(f, &base, &osize, NULL) != 0) {
        fclose(f);
        set_err(NULL, 0, "update2.rpf 里找不到 %s", GC_SCRIPT_REL_RPF);
        return GC_SCRIPT_ERR_NOREL;
    }
    RelInfo ri;
    if (rel_load(f, base, &ri, NULL) != 0) {
        fclose(f);
        set_err(NULL, 0, "读取 %s 内层头部失败（偏移 %lld）",
                GC_SCRIPT_REL_RPF, base);
        return GC_SCRIPT_ERR_NOREL;
    }
    /* 🚨🚨 v5.7: 必须遍历【全部】条目统计总数！
     *   旧代码 `i < ri.h.ec && n < max` ⇒ 一旦 n 到 max 就 break，
     *   而真实 update2 有 1027 个 .nsc ⇒ 返回值被截断成 128，
     *   调用方以为「RPF 里只有 128 个」⇒ 排后面的脚本被误判「未安装」。
     *   现在：out 满了继续计数不写，返回值 = 真实总数。 */
    int n = 0;                 /* 真实总数 */
    int nw = 0;                /* 实际写出的条数 */
    for (u32 i = 0; i < ri.h.ec; i++) {
        u64 q0 = rd_u64(ri.toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        char nm[GC_SCRIPT_NAME_MAX];
        name_in(ri.names, ri.h.nl, (u32)(q0 & 0xFFFF), ri.h.shift, nm, sizeof(nm));
        size_t L = strlen(nm);
        if (L < 4 || strcasecmp(nm + L - 4, ".nsc") != 0) continue;
        n++;
        if (out && nw < max) {
            snprintf(out[nw].name, GC_SCRIPT_NAME_MAX, "%s", nm);
            out[nw].is_res  = is_res;
            out[nw].on_disk = (u32)((q0 >> 16) & 0xFFFFFF);
            out[nw].size    = rd_u32(ri.toc + i * 16 + 8);
            nw++;
        }
    }
    rel_free(&ri);
    fclose(f);
    snprintf(s_last_err, sizeof(s_last_err),
             "OK: %s 里共 %d 个 .nsc 条目（返回前 %d 条）",
             GC_SCRIPT_REL_RPF, n, nw);
    return n;                  /* ★ 返回真实总数，不是写出条数 */
}

/* ★ v5.7: 按名精确查询 —— 一次遍历，内存 O(1)，不受 RPF 条目总数限制。
 *   返回 RPF 内 .nsc 总数（<0 = 出错），并把 q[i].installed 填好。
 *   🚨 判定「已装」必须用这个（gc_script_list 会因 max 截断而漏判）。 */
int gc_script_query(GcScriptQuery *q, int n, char *err, size_t errsz) {
    for (int i = 0; i < n; i++) q[i].installed = 0;
    s_last_err[0] = 0;
    FILE *f = fopen(GC_SCRIPT_UPDATE2, "rb");
    if (!f) {
        set_err(err, errsz, "打不开 update2.rpf (errno=%d)", errno);
        return GC_SCRIPT_ERR_OPEN;
    }
    long long base, osize;
    if (locate_rel(f, &base, &osize, NULL) != 0) {
        fclose(f);
        set_err(err, errsz, "update2.rpf 里找不到 %s", GC_SCRIPT_REL_RPF);
        return GC_SCRIPT_ERR_NOREL;
    }
    RelInfo ri;
    if (rel_load(f, base, &ri, NULL) != 0) {
        fclose(f);
        set_err(err, errsz, "读取 %s 内层头部失败", GC_SCRIPT_REL_RPF);
        return GC_SCRIPT_ERR_NOREL;
    }
    int total = 0;
    for (u32 i = 0; i < ri.h.ec; i++) {
        u64 q0 = rd_u64(ri.toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        char nm[GC_SCRIPT_NAME_MAX];
        name_in(ri.names, ri.h.nl, (u32)(q0 & 0xFFFF), ri.h.shift, nm, sizeof(nm));
        size_t L = strlen(nm);
        if (L < 4 || strcasecmp(nm + L - 4, ".nsc") != 0) continue;
        total++;
        for (int k = 0; k < n; k++) {
            if (q[k].installed) continue;
            if (strcasecmp(nm, q[k].name) == 0) q[k].installed = 1;
        }
    }
    rel_free(&ri);
    fclose(f);
    snprintf(s_last_err, sizeof(s_last_err),
             "OK: %s 里共 %d 个 .nsc 条目", GC_SCRIPT_REL_RPF, total);
    return total;
}

int gc_script_is_installed(const char *entry_name) {
    GcScriptItem items[GC_SCRIPT_MAX];
    int n = gc_script_list(items, GC_SCRIPT_MAX);
    if (n < 0) return n;
    for (int i = 0; i < n; i++)
        if (strcasecmp(items[i].name, entry_name) == 0) return 1;
    return 0;
}

/* ---------------- 对外：安装 ---------------- */
int gc_script_install(const char *nsc_path, const char *entry_name,
                      char *err, size_t errsz) {
    if (!nsc_path || !nsc_path[0]) { set_err(err, errsz, "路径为空"); return GC_SCRIPT_ERR_READ; }

    /* 条目名：留空则取文件名 */
    char ename[GC_SCRIPT_NAME_MAX];
    if (entry_name && entry_name[0]) {
        snprintf(ename, sizeof(ename), "%s", entry_name);
    } else {
        const char *p = strrchr(nsc_path, '/');
        p = p ? p + 1 : nsc_path;
        snprintf(ename, sizeof(ename), "%s", p);
    }

    /* ---- 1. 读 .nsc ---- */
    FILE *r = fopen(nsc_path, "rb");
    if (!r) { set_err(err, errsz, "打不开 %s (errno=%d)", nsc_path, errno); return GC_SCRIPT_ERR_READ; }
    fseeko(r, 0, SEEK_END);
    long long rlen = ftello(r);
    fseeko(r, 0, SEEK_SET);
    if (rlen <= 0 || rlen > 8 * 1024 * 1024) {
        fclose(r); set_err(err, errsz, ".nsc 大小异常 (%lld B)", rlen);
        return GC_SCRIPT_ERR_READ;
    }
    u8 *raw = (u8*)malloc((size_t)rlen);
    if (!raw) { fclose(r); set_err(err, errsz, "内存不足"); return GC_SCRIPT_ERR_READ; }
    if (fread(raw, 1, (size_t)rlen, r) != (size_t)rlen) {
        free(raw); fclose(r); set_err(err, errsz, "读取不完整"); return GC_SCRIPT_ERR_READ;
    }
    fclose(r);

    /* ---- 2. 判定数据类型：RSC7 资源条目 还是 裸 Switch payload ----
     *   RSC7 ⇒ 原样写（内部已是 deflate），TOC 置 is_res=1
     *   裸   ⇒ 先 raw deflate 压缩，TOC 置 is_res=0 */
    int as_res = is_rsc7(raw, (size_t)rlen);
    u32 clen = 0;
    u8 *comp = NULL;
    u32 tsize = 0;                 /* TOC 的 size 字段 */
    if (as_res) {
        comp  = raw;               /* 不压缩，直接写；free 时由 free_bufs 去重 */
        clen  = (u32)rlen;
        tsize = rd_u32(raw + 8);   /* RSC7 头的虚拟大小 = TOC size */
        if (tsize == 0) {
            free_bufs(raw, comp);
            set_err(err, errsz, "RSC7 头的虚拟大小为 0（文件损坏？）");
            return GC_SCRIPT_ERR_READ;
        }
        printf("[script] %s: RSC7 资源条目 %u B (虚拟 %u B)\n", ename, clen, tsize);
    } else {
        comp = deflate_raw(raw, (u32)rlen, &clen);
        if (!comp) { free_bufs(raw, comp); set_err(err, errsz, "压缩失败"); return GC_SCRIPT_ERR_ZLIB; }
        tsize = (u32)rlen;
    }

    /* ---- 3. 打开 update2.rpf ---- */
    FILE *f = fopen(GC_SCRIPT_UPDATE2, "r+b");
    if (!f) {
        free_bufs(raw, comp);
        set_err(err, errsz, "打不开 update2.rpf (errno=%d)", errno);
        return GC_SCRIPT_ERR_OPEN;
    }
    fseeko(f, 0, SEEK_END);
    long long fsz = ftello(f);

    long long base = 0, osize = 0;
    long rel_idx = -1;
    if (locate_rel(f, &base, &osize, &rel_idx) != 0) {
        free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "找不到 %s", GC_SCRIPT_REL_RPF);
        return GC_SCRIPT_ERR_NOREL;
    }

    RelInfo ri;
    if (rel_load(f, base, &ri, ename) != 0) {
        free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "读取内层头部失败");
        return GC_SCRIPT_ERR_NOREL;
    }

    /* ---- 4. 落点：优先用 script_rel.rpf 之后的空隙 ---- */
    long long gap_start = base + osize;
    long long gap_end = next_entry_after(f, gap_start, fsz);
    /* 需要的连续空间：err/替换数据 + 新增条目（若有） */
    u32 need = (clen + 511u) & ~511u;
    int is_new = (ri.idx_same < 0);
    long long need_total = (long long)need;           /* 替换时只需放数据 */
    long long gap = gap_end - gap_start;
    long long write_at;
    int extend_file = 0;

    if (gap >= need_total && (gap_start % 512) == 0) {
        write_at = gap_start;
    } else {
        /* 追加到文件末尾（需 512 对齐） */
        write_at = (fsz + 511) / 512 * 512;
        extend_file = 1;
    }
    long long rel_off = write_at - base;
    if (rel_off < 0 || (rel_off / 512) > 0x7FFFFF) {
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "落点越界");
        return GC_SCRIPT_ERR_SPACE;
    }
    u32 new_osize = (u32)(rel_off + need);
    if (new_osize < osize) new_osize = (u32)osize;   /* 不缩 */

    printf("[script] %s: 压缩 %u -> %u B, 落点 rel=%lld (空隙 %lld B)\n",
           ename, (u32)rlen, clen, rel_off, gap);

    /* ---- 5. 写入数据 ---- */
    if (fseeko(f, write_at, SEEK_SET) != 0) {
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "定位失败"); return GC_SCRIPT_ERR_IO;
    }
    if (fwrite(comp, 1, clen, f) != clen) {
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "写入数据失败 (errno=%d)", errno); return GC_SCRIPT_ERR_IO;
    }
    u32 pad = need - clen;
    if (pad) {
        static const u8 zeros[512] = {0};
        u32 left = pad;
        while (left > 0) {
            u32 k = left > 512 ? 512 : left;
            if (fwrite(zeros, 1, k, f) != k) {
                rel_free(&ri); free_bufs(raw, comp); fclose(f);
                set_err(err, errsz, "写入填充失败"); return GC_SCRIPT_ERR_IO;
            }
            left -= k;
        }
    }

    /* ---- 6. 改内层 TOC / names ---- */
    u32 sec_new = (u32)(rel_off / 512);
    u32 new_ec = ri.h.ec;
    u8 *new_names = ri.names;
    u32 new_nl = ri.h.nl;
    u8 *new_toc = NULL;

    if (!is_new) {
        /* 替换：只改该条目的指针/大小 */
        new_toc = ri.toc;
        u8 *p = new_toc + ri.idx_same * 16;
        u64 q0 = rd_u64(p);
        u32 noff = (u32)(q0 & 0xFFFF);
        q0 = (u64)noff
           | ((u64)(clen & 0xFFFFFF) << 16)
           | ((u64)(sec_new & 0x7FFFFF) << 40)
           | (as_res ? (1ULL << 63) : 0ULL);   /* ★ v6.2: RSC7 ⇒ is_res=1 */
        wr_u64(p, q0);
        wr_u32(p + 8, tsize);          /* size：RSC7 用虚拟大小，裸用解压后长度 */
        wr_u32(p + 12, 0);
    } else {
        /* 新增：TOC +1、names 追加 */
        new_ec = ri.h.ec + 1;
        size_t add = strlen(ename) + 1;
        new_names = (u8*)malloc(ri.h.nl + add);
        if (!new_names) { rel_free(&ri); free_bufs(raw, comp); fclose(f);
                          set_err(err, errsz, "内存不足"); return GC_SCRIPT_ERR_SPACE; }
        memcpy(new_names, ri.names, ri.h.nl);
        memcpy(new_names + ri.h.nl, ename, add);
        new_nl = ri.h.nl + (u32)add;

        new_toc = (u8*)malloc((size_t)new_ec * 16);
        if (!new_toc) { free(new_names); rel_free(&ri); free_bufs(raw, comp); fclose(f);
                        set_err(err, errsz, "内存不足"); return GC_SCRIPT_ERR_SPACE; }
        memcpy(new_toc, ri.toc, (size_t)ri.h.ec * 16);
        u64 q0 = (u64)(ri.h.nl & 0xFFFF)
               | ((u64)(clen & 0xFFFFFF) << 16)
               | ((u64)(sec_new & 0x7FFFFF) << 40)
               | (as_res ? (1ULL << 63) : 0ULL);   /* ★ v6.2: RSC7 ⇒ is_res=1 */
        wr_u64(new_toc + ri.h.ec * 16, q0);
        wr_u32(new_toc + ri.h.ec * 16 + 8, tsize);
        wr_u32(new_toc + ri.h.ec * 16 + 12, 0);
        /* 根目录 count +1 */
        if (ri.root_first >= 0)
            wr_u32(new_toc + ri.root_first * 16 + 12, (u32)(ri.root_count + 1));
    }

    /* 头部尺寸检查：不能侵占首个数据 */
    long long new_head_end = 16 + (long long)new_ec * 16 + new_nl;
    long long first_data = -1;
    for (u32 i = 0; i < new_ec; i++) {
        u64 q0 = rd_u64(new_toc + i * 16);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        if (off == 0) continue;
        long long s = (long long)off * 512;
        if (first_data < 0 || s < first_data) first_data = s;
    }
    if (first_data >= 0 && new_head_end > first_data) {
        if (new_names != ri.names) free(new_names);
        if (new_toc != ri.toc) free(new_toc);
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "头部扩容需平移数据（%lld > %lld），暂不支持",
                new_head_end, first_data);
        return GC_SCRIPT_ERR_SPACE;
    }

    /* 🚨 先写 names 再写 TOC */
    if (fseeko(f, base + 16 + (long long)new_ec * 16, SEEK_SET) == 0 &&
        fwrite(new_names, 1, new_nl, f) != new_nl) {
        if (new_names != ri.names) free(new_names);
        if (new_toc != ri.toc) free(new_toc);
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "写 names 失败"); return GC_SCRIPT_ERR_IO;
    }
    if (fseeko(f, base + 16, SEEK_SET) == 0 &&
        fwrite(new_toc, 1, (size_t)new_ec * 16, f) != (size_t)new_ec * 16) {
        if (new_names != ri.names) free(new_names);
        if (new_toc != ri.toc) free(new_toc);
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "写 TOC 失败"); return GC_SCRIPT_ERR_IO;
    }
    /* 内层 header：条目数 + names 长度（高 4 位是 name_shift，原样保留） */
    u8 hb[8];
    wr_u32(hb, new_ec);
    wr_u32(hb + 4, (u32)(((u32)ri.h.shift << 28) | (new_nl & 0x0FFFFFFFu)));
    if (fseeko(f, base + 4, SEEK_SET) == 0 && fwrite(hb, 1, 8, f) != 8) {
        if (new_names != ri.names) free(new_names);
        if (new_toc != ri.toc) free(new_toc);
        rel_free(&ri); free_bufs(raw, comp); fclose(f);
        set_err(err, errsz, "写内层 header 失败"); return GC_SCRIPT_ERR_IO;
    }

    /* ---- 7. 外层：script_rel.rpf 的 size 扩大到覆盖新数据 ---- */
    if (rel_idx >= 0 && new_osize != osize) {
        u8 sb[4];
        wr_u32(sb, new_osize);
        if (fseeko(f, 0x10 + (long long)rel_idx * 16 + 8, SEEK_SET) == 0)
            (void)fwrite(sb, 1, 4, f);
    }

    if (new_names != ri.names) free(new_names);
    if (new_toc != ri.toc) free(new_toc);
    rel_free(&ri);
    free_bufs(raw, comp);

    fflush(f);
    fsdevCommitDevice("sdmc");
    fclose(f);

    printf("[script] 安装完成: %s (%s)\n", ename, is_new ? "新增" : "替换");
    (void)extend_file;
    return GC_SCRIPT_OK;
}

/* ========================================================================= */
/* ★ v5.2: 卸载（删除）已装脚本                                              */
/* ========================================================================= */
/* 做法：从 script_rel.rpf 的内层 TOC 里【移除该条目】，后续条目前移，根目录 count-1。
 *   · names 池不动（那几十字节浪费掉，换来零风险 —— 名字池是共享的，
 *     真要删还得重排全部 name_off）
 *   · 数据区不动（不回收空间，与「改成无效扩展名」等价的效果）
 *   · 🚨 TOC 条目顺序即目录索引（根目录的 first/count 引用索引）
 *     ⇒ 删中间条目后只需 count-1，first 不用动 ✓
 *
 * 🚨🚨 v5.6 修复（严重：删除后游戏卡在加载故事模式）：
 *   RPF7 布局 = header(16) + TOC(ec×16) + names池。
 *   【names 池的起始位置由 ec 决定】⇒ ec 减 1 后，names 池必须【整体前移 16 字节】！
 *   旧实现只改了 ec 和 TOC，没搬 names ⇒ 引擎按新 ec 读 names 时错位 16 字节
 *   ⇒ script_rel.rpf 解析失败 ⇒ 卡加载。
 *   （安装侧没这个问题：new_ec = ec+1 时 names 写到 base+16+new_ec*16，天然后移 ✓）
 */
int gc_script_uninstall(const char *entry_name, char *err, size_t errsz) {
    if (!entry_name || !entry_name[0]) { set_err(err, errsz, "名字为空"); return GC_SCRIPT_ERR_READ; }
    FILE *f = fopen(GC_SCRIPT_UPDATE2, "r+b");
    if (!f) { set_err(err, errsz, "打不开 update2.rpf (errno=%d)", errno); return GC_SCRIPT_ERR_OPEN; }

    long long base = 0, osize = 0;
    long rel_idx = -1;
    if (locate_rel(f, &base, &osize, &rel_idx) != 0) {
        fclose(f); set_err(err, errsz, "找不到 %s", GC_SCRIPT_REL_RPF);
        return GC_SCRIPT_ERR_NOREL;
    }
    RelInfo ri;
    if (rel_load(f, base, &ri, entry_name) != 0) {
        fclose(f); set_err(err, errsz, "读取内层头部失败");
        return GC_SCRIPT_ERR_NOREL;
    }

    /* ★★ P1 (2026-10-01): 加密头防护 —— 官方铁律
     *   GTAVPatcher 源码原话：
     *     "archive headers must still be normalised to OPEN before entries can be
     *      deleted — NG headers cannot be written back."
     *   ⇒ 删条目（改 TOC + 搬 names 池）只在 OPEN 头下安全。
     *     NG(0x0FEFFFFF) 写不回去；AES 等其它类型语义未验证 ⇒ 一律拒绝。 */
    if (ri.h.enc == 0x0FEFFFFFu) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz,
                "内层是 NG 加密头（0x0FEFFFFF），不支持删除条目"
                "（官方限制：NG 头无法写回）");
        return GC_SCRIPT_ERR_READ;
    }
    if (ri.h.enc != 0x4E45504Fu) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz,
                "内层头是 0x%08X（非 OPEN 0x4E45504F），删除条目需先转 OPEN",
                ri.h.enc);
        return GC_SCRIPT_ERR_READ;
    }

    if (ri.idx_same < 0) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "没有这个条目：%s", entry_name);
        return GC_SCRIPT_ERR_READ;
    }
    if (ri.h.ec <= 1) {          /* 只剩根目录，不允许全删 */
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "内层条目太少，拒绝删除");
        return GC_SCRIPT_ERR_READ;
    }
    /* ★ v5.6 防护：根目录（条目 0）绝对不能删 —— 删了整个容器就废了 */
    if (ri.idx_same == ri.root_first || ri.idx_same == 0) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "拒绝删除根目录条目（内部错误）");
        return GC_SCRIPT_ERR_READ;
    }

    u32 new_ec = ri.h.ec - 1;
    /* ---- ① TOC：移除该条目，后续整体前移 ---- */
    memmove(ri.toc + (size_t)ri.idx_same * 16,
            ri.toc + (size_t)(ri.idx_same + 1) * 16,
            (size_t)(ri.h.ec - ri.idx_same - 1) * 16);

    /* ★ v5.6: root_first / root_count 是【TOC 索引】，必须跟着前移修正。
     *   若 root_first 在被删条目之后 ⇒ 索引 -1。
     *   然后再写 count-1。（顺序很重要：先修 first，再改 count） */
    int rf = ri.root_first, rc = ri.root_count;
    if (rf > (int)ri.idx_same) rf -= 1;
    if (rf >= 0 && rf < (int)new_ec && rc > 0) {
        wr_u32(ri.toc + (size_t)rf * 16 + 8,  (u32)rf);        /* first 保持不变语义 */
        wr_u32(ri.toc + (size_t)rf * 16 + 12, (u32)(rc - 1));  /* count -1 */
    }

    /* 🚨🚨 ② 把 names 池整体前移 16 字节 —— 这是 v5.6 修的关键 bug。
     *   新 names 起点 = base + 16 + new_ec*16（比原来早 16 字节） */
    u8 *names_out = ri.names;      /* 原地前移，不需要额外内存 */
    if (fseeko(f, base + 16 + (long long)new_ec * 16, SEEK_SET) != 0) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "定位 names 池失败");
        return GC_SCRIPT_ERR_IO;
    }
    if (fwrite(names_out, 1, ri.h.nl, f) != ri.h.nl) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "写 names 池失败");
        return GC_SCRIPT_ERR_IO;
    }
    /* ③ 写 TOC（在 names 之后写，避免被 names 覆盖） */
    if (fseeko(f, base + 16, SEEK_SET) != 0 ||
        fwrite(ri.toc, 1, (size_t)new_ec * 16, f) != (size_t)new_ec * 16) {
        rel_free(&ri); fclose(f);
        set_err(err, errsz, "写回 TOC 失败");
        return GC_SCRIPT_ERR_IO;
    }
    /* ④ 内层 header：条目数（names 长度与 shift 原样保留） */
    {
        u8 hb[8];
        wr_u32(hb, new_ec);
        wr_u32(hb + 4, (u32)(((u32)ri.h.shift << 28) | (ri.h.nl & 0x0FFFFFFFu)));
        if (fseeko(f, base + 4, SEEK_SET) != 0 || fwrite(hb, 1, 8, f) != 8) {
            rel_free(&ri); fclose(f);
            set_err(err, errsz, "写内层 header 失败");
            return GC_SCRIPT_ERR_IO;
        }
    }
    rel_free(&ri);

    fflush(f);
    fsdevCommitDevice("sdmc");
    fclose(f);
    printf("[script] 已删除 %s（内层条目 %u）\n", entry_name, new_ec);
    return GC_SCRIPT_OK;
}

/* ========================================================================= */
/* ★ v6.2 (2026-10-01): 还原官方脚本                                          */
/* ========================================================================= */
/* 背景：装第三方脚本 mod 时，mod 会【替换 stock 同名脚本】达到加载目的
 *   （MEGATARD / 经典 ragemenu 借 achievement_controller.nsc + shop_controller.nsc
 *    两个壳；HotCoffee 借 error_listener.nsc）。
 *   用户想卸干净时会卡住：直接「删除」会把 stock 脚本删掉 ⇒ 游戏异常。
 *
 * 本功能：把 romfs 内置的【官方原版】写回 RPF，覆盖被改的条目。
 *
 * ★★ 2026-10-01 重大修正（v6.2）：
 *   旧版内置的 error_listener.nsc 是【误从 HotCoffee 包里拷来的模组加载器】
 *   （927 B，内含 simple_zombies 引用）—— 点「还原」反而会把热咖啡装上！
 *   现状已改为从【用户提供的原版 update2.rpf 的 script_rel.rpf】直接提取：
 *
 *     条目                         on_disk    is_res  RSC7虚拟size   sha1
 *     error_listener.nsc              718      1       131072     3342ffe3
 *     achievement_controller.nsc   31,460      1         6272     9f2481df
 *     shop_controller.nsc         856,627      1      8519682     e7d9230f
 *
 *   🚨 官方 .nsc 全部是 is_res=1 的 RSC7 资源条目（16B 'RSC7' 头 + deflate），
 *      而 mod 的 .nsc 是 is_res=0 的裸 Switch payload。二者不能混：
 *      写入时 is_res 位必须跟着数据类型走，否则引擎按错误方式解析 ⇒ 崩。
 *      （gc_script_install 已按 RSC7 头自动分流） */
static const struct { const char *name; const char *romfs; } S_STOCK[] = {
    { "error_listener.nsc",         "romfs:/stock_scripts/error_listener.nsc" },
    { "achievement_controller.nsc", "romfs:/stock_scripts/achievement_controller.nsc" },
    { "shop_controller.nsc",        "romfs:/stock_scripts/shop_controller.nsc" },
};
#define S_STOCK_N ((int)(sizeof(S_STOCK)/sizeof(S_STOCK[0])))

int gc_script_stock_count(void) { return S_STOCK_N; }

const char *gc_script_stock_name(int i) {
    if (i < 0 || i >= S_STOCK_N) return NULL;
    return S_STOCK[i].name;
}

int gc_script_restore_stock(const char *entry_name, char *err, size_t errsz) {
    if (!entry_name || !entry_name[0]) {
        set_err(err, errsz, "条目名为空");
        return GC_SCRIPT_ERR_READ;
    }

    const char *src = NULL;
    for (int i = 0; i < S_STOCK_N; i++) {
        if (strcasecmp(S_STOCK[i].name, entry_name) == 0) { src = S_STOCK[i].romfs; break; }
    }
    if (!src) {
        set_err(err, errsz, "「%s」没有内置原版副本", entry_name);
        return GC_SCRIPT_ERR_READ;
    }

    /* 先确认内置文件存在且非空 —— 否则给出明确提示，别让 install 报奇怪的错 */
    FILE *rf = fopen(src, "rb");
    if (!rf) {
        set_err(err, errsz, "内置原版缺失：%s（本 NRO 未打包该资源）", src);
        return GC_SCRIPT_ERR_READ;
    }
    fseeko(rf, 0, SEEK_END);
    long long sz = (long long)ftello(rf);
    fclose(rf);
    if (sz <= 16) {
        set_err(err, errsz, "内置原版文件异常（%lld B）：%s", sz, src);
        return GC_SCRIPT_ERR_READ;
    }

    /* 复用安装逻辑（同名条目 ⇒ 替换；RSC7 自动走 is_res=1 分支） */
    int rc = gc_script_install(src, entry_name, err, errsz);
    if (rc == GC_SCRIPT_OK) {
        printf("[script] 已还原官方 %s（%lld B）\n", entry_name, sz);
    }
    return rc;
}

/* ========================================================================= */
/* ★ v6.2: 内置模组 —— 打进 NRO romfs，一键安装，免插卡拷文件                 */
/* ========================================================================= */
/* ★★ 2026-10-01 晚【重要更正】：ragemenu 只需要【一个壳】！
 *
 *   ragemenu.nsc              本体（新建条目）
 *   achievement_controller.nsc 借壳加载器（字节码里含 joaat("ragemenu")=0x4BD26EEF ×4）
 *
 *   ❌ 早先照抄 MEGATARD 的 `how to install.txt`（它列了 3 个文件，含
 *      shop_controller.nsc），把 shop_controller 也打进内置包 —— **多余**。
 *      实证（用户指出 + 字节码复核）：
 *        ① ragemenu.nsc / achievement_controller.nsc 的 payload 里
 *           **完全没有 joaat("shop_controller")=0x39DA738B 的引用**
 *        ② 用户实际装机用的 `GTA5switch修改器版本0.9.9开菜单 L+↓.zip`
 *           里**只有 achievement_controller.nsc + ragemenu.nsc 两个文件**
 *        ③ 作者仓库里那份 shop_controller.nsc（2,129,920 B）躺在
 *           `MEGATARD_nsc/_stock_ref/` 里 —— 那是**给 SC-CL 做 PC→Switch
 *           转换时当"同版本 stock 参考"用的**（拷头 8 字节 / 0x18 处 4 字节），
 *           **不是要装进游戏的**。且它与官方解压出的 payload 内容并不相同。
 *      ⇒ 装模组只写 2 个条目。NRO 体积因此省下 2.03 MB。
 *
 * HotCoffee-NX 也是【一对】文件，但★有 cinnamon / combo 两个版本★：
 *   error_listener.nsc         加载器（含 joaat("simple_zombies")×4）
 *   simple_zombies.nsc         本体 —— 文件名是「HotCoffee 的槽位名」，
 *                              不是「只有丧尸」！
 *
 *   ★★ 实测（下载两个 zip 对比）：
 *      cinnamon_zh  simple_zombies.nsc = 204,128 B sha1 9f98a542  ← 只有热咖啡
 *      combo_zh     simple_zombies.nsc = 262,144 B sha1 f88b0271  ← 热咖啡 + 丧尸
 *      两个包的 error_listener.nsc 完全一样（927 B, sha1 ac947f5b）
 *   上游 README：「两个版本的 simple_zombies.nsc 是同一个位置，只能装一个」
 *   ⇒ 下面两项【互斥】，装一个会覆盖另一个。 */
typedef struct { const char *entry; const char *src; } GcModFile;

static const GcModFile MF_MEGATARD[] = {
    { "ragemenu.nsc",               "romfs:/builtin_mods/megatard/ragemenu.nsc" },
    { "achievement_controller.nsc", "romfs:/builtin_mods/megatard/achievement_controller.nsc" },
};
static const GcModFile MF_CLASSIC[] = {
    { "ragemenu.nsc",               "romfs:/builtin_mods/classic/ragemenu.nsc" },
    { "achievement_controller.nsc", "romfs:/builtin_mods/classic/achievement_controller.nsc" },
};
/* 热咖啡 + 丧尸（combo 版） */
static const GcModFile MF_HC_COMBO[] = {
    { "error_listener.nsc",         "romfs:/builtin_mods/hotcoffee/error_listener.nsc" },
    { "simple_zombies.nsc",         "romfs:/builtin_mods/hotcoffee/simple_zombies_combo.nsc" },
};
/* 只有热咖啡（cinnamon 版） */
static const GcModFile MF_HC_PLAIN[] = {
    { "error_listener.nsc",         "romfs:/builtin_mods/hotcoffee/error_listener.nsc" },
    { "simple_zombies.nsc",         "romfs:/builtin_mods/hotcoffee/simple_zombies.nsc" },
};

#define MF_N(a) ((int)(sizeof(a)/sizeof(a[0])))

typedef struct {
    const char *id;
    const char *title;
    const char *title_en;   /* ★ v6.3: 英文显示名 */
    const char *author;
    const char *note;
    const char *note_en;    /* ★ v6.3: 英文备注 */
    const GcModFile *files;
    int n;
} GcBuiltinModDef;

static const GcBuiltinModDef S_MODS[] = {
    { "megatard",  "MEGATARD v0.9.9 汉化", "MEGATARD v0.9.9 (ZH)", "Geekmaxxer",
      "与「经典 ragemenu」互斥（都占 ragemenu）",
      "Mutually exclusive with Classic ragemenu (both use the ragemenu slot)",
      MF_MEGATARD,  MF_N(MF_MEGATARD) },
    { "classic",   "经典 ragemenu 简体 v13", "Classic ragemenu v13 (ZH)", "maritoguionyo",
      "与「MEGATARD」互斥（都占 ragemenu）",
      "Mutually exclusive with MEGATARD (both use the ragemenu slot)",
      MF_CLASSIC,   MF_N(MF_CLASSIC) },
    { "hc_combo",  "热咖啡 + 丧尸", "Hot Coffee + Zombies", "Je11yb0ne/CinnamonCoffee",
      "含丧尸模组；与下一项互斥（都占 simple_zombies）",
      "Includes the zombies mod; mutually exclusive with the next entry (both use simple_zombies)",
      MF_HC_COMBO,  MF_N(MF_HC_COMBO) },
    { "hc_plain",  "热咖啡（不含丧尸）", "Hot Coffee (no zombies)", "Je11yb0ne/CinnamonCoffee",
      "与上一项互斥（都占 simple_zombies）",
      "Mutually exclusive with the previous entry (both use simple_zombies)",
      MF_HC_PLAIN,  MF_N(MF_HC_PLAIN) },
};
#define S_MODS_N ((int)(sizeof(S_MODS)/sizeof(S_MODS[0])))

int gc_script_builtin_count(void) { return S_MODS_N; }

const GcBuiltinMod *gc_script_builtin(int i) {
    if (i < 0 || i >= S_MODS_N) return NULL;
    /* 借着 GcBuiltinMod 的内存布局逐字段返回（同前缀结构，安全） */
    static GcBuiltinMod out;
    out.id       = S_MODS[i].id;
    out.title    = S_MODS[i].title;
    out.title_en = S_MODS[i].title_en;
    out.author   = S_MODS[i].author;
    out.note     = S_MODS[i].note;
    out.note_en  = S_MODS[i].note_en;
    out.n_files  = S_MODS[i].n;
    return &out;
}

const char *gc_script_builtin_entry(int i, int k) {
    if (i < 0 || i >= S_MODS_N) return NULL;
    if (k < 0 || k >= S_MODS[i].n) return NULL;
    return S_MODS[i].files[k].entry;
}

int gc_script_builtin_install(int i, char *err, size_t errsz) {
    if (i < 0 || i >= S_MODS_N) {
        set_err(err, errsz, "内置模组索引越界 (%d)", i);
        return GC_SCRIPT_ERR_READ;
    }
    const GcBuiltinModDef *m = &S_MODS[i];

    for (int k = 0; k < m->n; k++) {
        const GcModFile *mf = &m->files[k];
        /* 先查资源在不在，给出比 install 更清楚的定位 */
        FILE *tf = fopen(mf->src, "rb");
        if (!tf) {
            set_err(err, errsz, "第 %d/%d 个文件的内置资源缺失：%s",
                    k + 1, m->n, mf->src);
            return GC_SCRIPT_ERR_READ;
        }
        fclose(tf);

        char sub[224]; sub[0] = 0;
        int rc = gc_script_install(mf->src, mf->entry, sub, sizeof(sub));
        if (rc != GC_SCRIPT_OK) {
            set_err(err, errsz, "装 %s 失败（第 %d/%d 个文件 %s）：%s",
                    m->title, k + 1, m->n, mf->entry, sub[0] ? sub : "未知错误");
            return rc;
        }
        printf("[script] 内置模组 %s: %s 已写入\n", m->id, mf->entry);
    }
    set_err(err, errsz, "OK: %s 已安装（%d 个文件）", m->title, m->n);
    return GC_SCRIPT_OK;
}
