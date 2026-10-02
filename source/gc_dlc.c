/* gc_dlc.c —— PC dlcpack → Switch 纯字节转换
 *
 * ★ 转换规则（由官方 GTAVPatcher.Core 的 PlatformNaming 实测枚举得到）：
 *     · 平台 token: x64 → switch        （只有这一个）
 *     · 扩展名    : .yXX → .nXX         （49 条，规则 = 首字母 y→n）
 *       .yft→.nft  .ytd→.ntd  .ydr→.ndr  .ymap→.nmap  .ytyp→.ntyp  .ysc→.nsc ...
 *
 * ★ 为什么能「纯字节」：
 *     顶层 dlc.rpf 的名字池后面就是数据区，中间有对齐填充。
 *     x64(3) → switch(6) 只需 +3 字节，把名字池向后扩一点（并把 name_off
 *     重映射），只要没侵入数据区就完全安全 ⇒ 文件大小/数据偏移全不变。
 *
 * ★ 为什么必须重映射 name_off：
 *     名字池是「所有名字拼接 + NUL」，条目靠 name_off 定位。
 *     名字一变长，它后面所有名字的偏移都会平移 ⇒ 必须整池重建 + 全部重算。
 *
 * ★ 内嵌 rpf（vehicles.rpf / xxx_mods.rpf）里的资源名：
 *     .yft → .nft 是【等长】替换 ⇒ 直接改那几个字节，连 TOC 都不用动。
 */
#include "gc_dlc.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <zlib.h>

#define RPF7_MAGIC 0x52504637u
#define MAX_NEST_DEPTH 4
#define DLC_MAX_ENTRIES 200000

/* ---------- 平台命名表（官方实测枚举） ---------- */
static const char *DLC_EXT_MAP[][2] = {
    { ".yat",  ".nat"  }, { ".ybm",  ".nbm"  }, { ".ybn",  ".nbn"  },
    { ".ycd",  ".ncd"  }, { ".ycm",  ".ncm"  }, { ".yct",  ".nct"  },
    { ".ydat", ".ndat" }, { ".ydd",  ".ndd"  }, { ".ydm",  ".ndm"  },
    { ".ydr",  ".ndr"  }, { ".ydt",  ".ndt"  }, { ".yed",  ".ned"  },
    { ".yem",  ".nem"  }, { ".yfd",  ".nfd"  }, { ".yfl",  ".nfl"  },
    { ".yfm",  ".nfm"  }, { ".yft",  ".nft"  }, { ".ygf",  ".ngf"  },
    { ".ygm",  ".ngm"  }, { ".yhm",  ".nhm"  }, { ".yim",  ".nim"  },
    { ".yjm",  ".njm"  }, { ".ykm",  ".nkm"  }, { ".ykt",  ".nkt"  },
    { ".yld",  ".nld"  }, { ".ylm",  ".nlm"  }, { ".ymap", ".nmap" },
    { ".ymd",  ".nmd"  }, { ".ymf",  ".nmf"  }, { ".yml",  ".nml"  },
    { ".ymm",  ".nmm"  }, { ".ymt",  ".nmt"  }, { ".ynd",  ".nnd"  },
    { ".ynm",  ".nnm"  }, { ".ynt",  ".nnt"  }, { ".ynv",  ".nnv"  },
    { ".ypdb", ".npdb" }, { ".ypl",  ".npl"  }, { ".ypt",  ".npt"  },
    { ".yqt",  ".nqt"  }, { ".yrt",  ".nrt"  }, { ".ysc",  ".nsc"  },
    { ".yst",  ".nst"  }, { ".ytd",  ".ntd"  }, { ".ytf",  ".ntf"  },
    { ".ytl",  ".ntl"  }, { ".ytyp", ".ntyp" }, { ".yvr",  ".nvr"  },
    { ".ywr",  ".nwr"  }, { ".ywt",  ".nwt"  },
};
#define DLC_EXT_MAP_N ((int)(sizeof(DLC_EXT_MAP)/sizeof(DLC_EXT_MAP[0])))

/* 平台 token：x64 → switch */
#define DLC_PC_TOKEN   "x64"
#define DLC_SW_TOKEN   "switch"

/* ---------- 错误信息 ---------- */
static char s_dlc_err[256] = {0};
static void set_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_dlc_err, sizeof(s_dlc_err), fmt, ap);
    va_end(ap);
}
static void put_err(char *err, size_t sz, const char *fmt, ...) {
    if (!err || sz == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(err, sz, fmt, ap);
    va_end(ap);
}

/* ---------- 小工具 ---------- */
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd_u64(const u8 *p) {
    return (u64)rd_u32(p) | ((u64)rd_u32(p + 4) << 32);
}
static void wr_u64(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = (u8)((v >> (8 * i)) & 0xFF);
}
static void wr_u32(u8 *p, u32 v) {
    for (int i = 0; i < 4; i++) p[i] = (u8)((v >> (8 * i)) & 0xFF);
}

/* 名字是否需要改名；需要则写入 out（容量 >= 256），返回 1 */
static int rename_name(const char *name, char *out, size_t outsz) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, DLC_PC_TOKEN) == 0) {
        snprintf(out, outsz, "%s", DLC_SW_TOKEN);
        return 1;
    }
    size_t ln = strlen(name);
    for (int i = 0; i < DLC_EXT_MAP_N; i++) {
        size_t el = strlen(DLC_EXT_MAP[i][0]);
        if (ln >= el) {
            int same = 1;
            for (size_t k = 0; k < el; k++) {
                char a = name[ln - el + k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (a != DLC_EXT_MAP[i][0][k]) { same = 0; break; }
            }
            if (same) {
                snprintf(out, outsz, "%.*s%s",
                         (int)(ln - el), name, DLC_EXT_MAP[i][1]);
                return 1;
            }
        }
    }
    return 0;
}

int gc_dlc_needs_rename(const char *name) {
    char tmp[256];
    return rename_name(name, tmp, sizeof(tmp));
}

/* ---------- RPF7 头部解析 ---------- */
typedef struct {
    u32 magic, entry_count, names_len, enc;
    u32 shift;
    size_t toc_off;      /* = base + 16 */
    size_t names_off;    /* = base + 16 + ec*16 */
    size_t base;
} RpfHdr;

static int parse_hdr(const u8 *b, size_t blen, size_t base, RpfHdr *h) {
    if (blen < base + 16) return GC_DLC_ERR_FORMAT;
    h->base = base;
    h->magic       = rd_u32(b + base);
    h->entry_count = rd_u32(b + base + 4);
    u32 nl         = rd_u32(b + base + 8);
    h->enc         = rd_u32(b + base + 12);
    if (h->magic != RPF7_MAGIC) return GC_DLC_ERR_FORMAT;
    h->names_len = nl & 0x0FFFFFFFu;
    h->shift     = nl >> 28;
    h->toc_off   = base + 16;
    h->names_off = base + 16 + (size_t)h->entry_count * 16;
    if (h->entry_count == 0 || h->entry_count > DLC_MAX_ENTRIES)
        return GC_DLC_ERR_FORMAT;
    if (h->names_off + h->names_len > blen)
        return GC_DLC_ERR_FORMAT;
    return GC_DLC_OK;
}

static int entry_is_dir(u64 q0) {
    u32 off  = (u32)((q0 >> 40) & 0x7FFFFF);
    int is_r = (int)((q0 >> 63) & 1);
    return (is_r == 0 && off == 0x7FFFFF);
}

/* 取条目名（写入 out） */
static void entry_name(const u8 *b, const RpfHdr *h, u32 name_off,
                       char *out, size_t outsz) {
    out[0] = 0;
    size_t o = h->names_off + ((size_t)name_off << h->shift);
    if (o >= h->names_off + h->names_len) return;
    size_t maxn = h->names_off + h->names_len - o;
    size_t n = 0;
    while (n < maxn && n < outsz - 1 && b[o + n] != 0) {
        out[n] = (char)b[o + n];
        n++;
    }
    out[n] = 0;
}

/* ==========================================================================
 * ★★ v6 (P0): 头部区缓冲（GcHead）—— 解除 512 MB 全文件读入限制
 *
 * 背景（2026-10-01 实测）：
 *   mpheist 2.42 GB / patchday3ng 2.00 GB / mpbiker 1.37 GB 全部超旧上限，
 *   而这三个正是官方「单机必需 6 包」成员 ⇒ 功能对它们完全不可用。
 *
 * 关键实测：这些包的【头部区极小】——
 *   mpheist: TOC 9,552 B + names 10,992 B + header 16 B = 20,560 B（占 0.0008%）
 *   patchday3ng: 5,840+6,352+16 = 12,208 B
 *   mpbiker:     4,224+4,000+16 =  8,240 B
 *
 * ⇒ 所有改名/屏蔽操作只碰 header + TOC + names（KB 级），
 *   数据区（GB 级）只在「名字池扩展挤到数据区、需要搬走条目」时才碰，
 *   而那也只是逐条搬移，可用固定大小缓冲流式完成。
 *
 * 因此：把「malloc(全文件)」换成「malloc(头部区)」，内存从 GB → KB。
 * ========================================================================== */

#define GC_HEAD_MAX (8u * 1024 * 1024)     /* 头部区上限（正常 < 30 KB）*/

typedef struct {
    u8   *head;        /* 头部区：header(16) + TOC(ec*16) + names(nl)，下标从 0 起 */
    size_t head_len;   /* = 16 + ec*16 + nl */
    RpfHdr h;          /* 解析结果；toc_off/names_off/base 均相对 head（base 恒 0）*/
    int   dirty;       /* 头部区被改过 ⇒ 退出时需要写回 */
} GcHead;

/* 读取并解析某容器（base = 该容器在文件中的字节偏移）的头部区 */
static int gh_load(FILE *f, long long base, GcHead *gh) {
    memset(gh, 0, sizeof(*gh));
    u8 raw[16];
    if (fseeko(f, base, SEEK_SET) != 0) return GC_DLC_ERR_IO;
    if (fread(raw, 1, 16, f) != 16) return GC_DLC_ERR_FORMAT;
    if (rd_u32(raw) != RPF7_MAGIC) return GC_DLC_ERR_FORMAT;

    u32 ec = rd_u32(raw + 4);
    u32 nl = rd_u32(raw + 8) & 0x0FFFFFFFu;
    if (ec == 0 || ec > DLC_MAX_ENTRIES) return GC_DLC_ERR_FORMAT;
    if (nl == 0 || nl > GC_HEAD_MAX) return GC_DLC_ERR_FORMAT;

    size_t hl = 16 + (size_t)ec * 16 + nl;
    if (hl > GC_HEAD_MAX) return GC_DLC_ERR_FORMAT;

    u8 *buf = (u8*)malloc(hl);
    if (!buf) return GC_DLC_ERR_NOMEM;
    /* 读回整块（含刚读过的 header，简单起见重读） */
    if (fseeko(f, base, SEEK_SET) != 0 || fread(buf, 1, hl, f) != hl) {
        free(buf);
        return GC_DLC_ERR_IO;
    }
    gh->head = buf;
    gh->head_len = hl;
    if (parse_hdr(buf, hl, 0, &gh->h) != GC_DLC_OK) {   /* blen = hl，够校验 */
        free(buf); gh->head = NULL;
        return GC_DLC_ERR_FORMAT;
    }
    return GC_DLC_OK;
}

static void gh_free(GcHead *gh) {
    if (gh->head) { free(gh->head); gh->head = NULL; }
    gh->head_len = 0;
}

/* 把头部区写回（只写 head_len 字节，不动数据区） */
static int gh_flush(FILE *f, long long base, GcHead *gh) {
    if (!gh->dirty || !gh->head) return 0;
    if (fseeko(f, base, SEEK_SET) != 0) return GC_DLC_ERR_IO;
    if (fwrite(gh->head, 1, gh->head_len, f) != gh->head_len) return GC_DLC_ERR_IO;
    return 0;
}

/* 数据区流式搬移：从 src 处搬 n 字节到 dst 处（可变重叠区，用分块双向拷贝）
 *   ★ 必须支持重叠：relocate 常把条目搬到后面（dst > src）或前面
 *   ★ 用固定 256 KB 栈外缓冲，避免大包再吃内存 */
#define GC_MOVE_CHUNK (256u * 1024)
static int gh_data_move(FILE *f, long long dst, long long src, size_t n) {
    if (n == 0 || dst == src) return 0;
    u8 *buf = (u8*)malloc(GC_MOVE_CHUNK);
    if (!buf) return GC_DLC_ERR_NOMEM;
    int rc = 0;
    if (dst < src) {
        /* 向前搬：从前往后（安全，不会覆盖未读数据） */
        size_t done = 0;
        while (done < n) {
            size_t c = n - done; if (c > GC_MOVE_CHUNK) c = GC_MOVE_CHUNK;
            if (fseeko(f, src + (long long)done, SEEK_SET) != 0 ||
                fread(buf, 1, c, f) != c) { rc = GC_DLC_ERR_IO; break; }
            if (fseeko(f, dst + (long long)done, SEEK_SET) != 0 ||
                fwrite(buf, 1, c, f) != c) { rc = GC_DLC_ERR_IO; break; }
            done += c;
        }
    } else {
        /* 向后搬：从后往前（避免覆盖源数据） */
        size_t left = n;
        while (left > 0) {
            size_t c = left > GC_MOVE_CHUNK ? GC_MOVE_CHUNK : left;
            long long so = src + (long long)(left - c);
            long long dob = dst + (long long)(left - c);
            if (fseeko(f, so, SEEK_SET) != 0 ||
                fread(buf, 1, c, f) != c) { rc = GC_DLC_ERR_IO; break; }
            if (fseeko(f, dob, SEEK_SET) != 0 ||
                fwrite(buf, 1, c, f) != c) { rc = GC_DLC_ERR_IO; break; }
            left -= c;
        }
    }
    free(buf);
    return rc;
}

/* 数据区清零（原地腾空）——同样分块 */
static int gh_data_zero(FILE *f, long long off, size_t n) {
    u8 *buf = (u8*)calloc(1, GC_MOVE_CHUNK);
    if (!buf) return GC_DLC_ERR_NOMEM;
    int rc = 0;
    size_t done = 0;
    while (done < n) {
        size_t c = n - done; if (c > GC_MOVE_CHUNK) c = GC_MOVE_CHUNK;
        if (fseeko(f, off + (long long)done, SEEK_SET) != 0 ||
            fwrite(buf, 1, c, f) != c) { rc = GC_DLC_ERR_IO; break; }
        done += c;
    }
    free(buf);
    return rc;
}



/* ---------- ★ v27: _hi 高模 → 指向低模数据（撞车形变开销 ∝ 顶点数） ----------
 *
 * 做法：对每个 xxx_hi.nft 条目，找同容器里同名的 xxx.nft（低模），
 *       把 _hi 条目的 TOC offset/on_disk/size 改成与低模一致。
 *   · 零数据搬移（只是改指针），文件大小不变
 *   · 引擎加载 _hi 时实际读到低模数据 ⇒ 形变开销直接砍到低模水平
 *   · 找不到对应低模（孤立 _hi）则不动 */
static int hi_to_lod(u8 *b, const RpfHdr *h, GcDlcStats *st) {
    int n = 0;
    typedef struct { u32 name_off; int idx; } Ent;
    Ent *ents = (Ent*)malloc(sizeof(Ent) * h->entry_count);
    if (!ents) return 0;
    int ne = 0;
    for (u32 i = 0; i < h->entry_count; i++) {
        u64 q0 = rd_u64(b + h->toc_off + (size_t)i * 16);
        if (entry_is_dir(q0)) continue;
        ents[ne].name_off = (u32)(q0 & 0xFFFF);
        ents[ne].idx = (int)i;
        ne++;
    }
    for (int e = 0; e < ne; e++) {
        char nm[256];
        entry_name(b, h, ents[e].name_off, nm, sizeof(nm));
        size_t ln = strlen(nm);
        if (ln <= 7) continue;
        char t7[8];
        memcpy(t7, nm + ln - 7, 7); t7[7] = 0;
        if (strcasecmp(t7, "_hi.nft") != 0) continue;

        /* 基名 = nm 去掉 _hi.nft 后接 .nft */
        char base[256];
        size_t bl = ln - 7;
        if (bl + 4 >= sizeof(base)) continue;
        memcpy(base, nm, bl);
        memcpy(base + bl, ".nft", 4);
        base[bl + 4] = 0;

        /* 找同名低模 */
        for (int f = 0; f < ne; f++) {
            char nm2[256];
            entry_name(b, h, ents[f].name_off, nm2, sizeof(nm2));
            if (strcmp(nm2, base) != 0) continue;

            u8 *phi = b + h->toc_off + (size_t)ents[e].idx * 16;
            u8 *plo = b + h->toc_off + (size_t)ents[f].idx * 16;
            u64 q0lo = rd_u64(plo);
            u32 lo_off = (u32)((q0lo >> 40) & 0x7FFFFF);
            u32 lo_od  = (u32)((q0lo >> 16) & 0xFFFFFF);
            u32 lo_sz  = rd_u32(plo + 8);
            if (lo_off == 0 || (lo_od == 0 && lo_sz == 0)) continue;

            u64 q0hi = rd_u64(phi);
            q0hi = (q0hi & ~(((u64)0xFFFFFF) << 16)) | ((u64)(lo_od & 0xFFFFFF) << 16);
            q0hi = (q0hi & ~(((u64)0x7FFFFF) << 40)) | ((u64)(lo_off & 0x7FFFFF) << 40);
            wr_u64(phi, q0hi);
            wr_u32(phi + 8, lo_sz);
            n++;
            break;
        }
    }
    free(ents);
    if (st && n > 0) st->n_hi_lod += n;
    return n;
}

/* ---------- ★ v5.4: 屏蔽高清贴图（对标官方 RemoveHighDetailTextures）----------
 *
 * 官方机制（GTAVPatcher.Core.dll IL 反汇编实证，2026-09-30）：
 *   static readonly string[] Suffixes = { "+hi.ytd", "+hidr.ytd", "+hidd.ytd", "+hifr.ytd" };
 *   static bool MatchesPattern(name, pat) { pat.EndsWith("*") ? name.StartsWith(..) : name.Equals(..) }
 *   void RemoveHighDetailTextures(arc) {
 *       foreach (e in SnapshotEntries(arc))
 *           if (Matches(e.NameLower)) { Stats.HighDetailEntriesRemoved++; RpfFile.DeleteEntry(e); }
 *   }
 *   GUI 文案：'Delete high-detail (+hi) resources - recommended for Switch'
 *              'High-detail (+hi) textures will be permanently deleted.'
 *
 * ★ 关键认知：官方删的是 +hi.ytd 系【高清贴图】，不是 _hi.nft【模型】！
 *   _hi.nft 是引擎形变必需，删了必闪退（见记忆铁律）；两者完全无关。
 *
 * ★ 官方自认的缺陷（其日志原文）：
 *     "Defragmenting was skipped, so the space freed by trimming and rewriting
 *      entries is still occupied by holes in the archives."
 *   ⇒ 即便官方删除，也只是去掉引用，**文件体积不变**。
 *
 * ★ 本实现的选择：等长改名屏蔽（'+' → '~'）
 *   · 与我方「只做等长改动、绝不搬数据」的转换原则一致（零风险）
 *   · 引擎按名找不到该贴图 ⇒ 效果等同删除，省显存/带宽
 *   · 可逆：再跑一次改成 '+' 即可恢复
 *   · 不碰 TOC / 不碰数据区 ⇒ 不会引发布局错乱
 */
#define GC_HD_SUF_N 4
static const char *GC_HD_SUF[GC_HD_SUF_N] = {
    "+hi.ytd", "+hidr.ytd", "+hidd.ytd", "+hifr.ytd"
};

/* 名字是否是高清贴图（大小写不敏感，按后缀匹配） */
static int is_hd_texture(const char *nm) {
    size_t L = strlen(nm);
    for (int k = 0; k < GC_HD_SUF_N; k++) {
        size_t s = strlen(GC_HD_SUF[k]);
        if (L < s) continue;
        if (strcasecmp(nm + L - s, GC_HD_SUF[k]) == 0) return 1;
    }
    return 0;
}

/* 在一个已解析的容器里屏蔽高清贴图（原地改名字符，等长） */
static int hide_hd_in(u8 *b, const RpfHdr *h, GcDlcStats *st) {
    int n = 0;
    for (u32 i = 0; i < h->entry_count; i++) {
        u8 *p = b + h->toc_off + (size_t)i * 16;
        u64 q0 = rd_u64(p);
        if (entry_is_dir(q0)) continue;
        u32 name_off = (u32)(q0 & 0xFFFF);
        char nm[256];
        entry_name(b, h, name_off, nm, sizeof(nm));
        if (!nm[0]) continue;
        if (!is_hd_texture(nm)) continue;
        /* 改名字池里第一个字符 '+' → '~'（等长，零搬移） */
        size_t o = h->names_off + ((size_t)name_off << h->shift);
        if (o >= h->names_off + h->names_len) continue;
        if (b[o] == '+') {
            b[o] = '~';
            n++;
        }
    }
    if (st && n > 0) st->n_hd_removed += n;
    return n;
}

/* 递归：容器 + 所有未压缩内嵌 rpf —— 全部只碰头部区（KB 级），数据区靠 seek
 *   返回本次（含子容器）实际屏蔽掉的条目数 */
static int hide_hd_rec_fp(FILE *f, long long base, GcDlcStats *st, int depth) {
    if (depth > MAX_NEST_DEPTH) return 0;
    GcHead gh;
    if (gh_load(f, base, &gh) != GC_DLC_OK) return 0;
    RpfHdr *h = &gh.h;

    int n_here = hide_hd_in(gh.head, h, st);   /* 改 head 缓冲里的 names 字符 */
    if (n_here > 0) {
        gh.dirty = 1;
        gh_flush(f, base, &gh);                /* ★ 只写头部区（KB 级） */
    }

    /* 递归内嵌 rpf：只从 TOC 拿子容器位置，不对数据区做整体缓冲 */
    int n_sub = 0;
    for (u32 i = 0; i < h->entry_count; i++) {
        u8 *p = gh.head + h->toc_off + (size_t)i * 16;
        u64 q0 = rd_u64(p);
        if (entry_is_dir(q0)) continue;
        int is_res = (int)((q0 >> 63) & 1);
        if (is_res) continue;
        u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        u32 d8 = rd_u32(p + 8);
        char nm[256];
        entry_name(gh.head, h, (u32)(q0 & 0xFFFF), nm, sizeof(nm));
        size_t ln = strlen(nm);
        if (ln <= 4) continue;
        if (strcasecmp(nm + ln - 4, ".rpf") != 0) continue;
        u32 nb = od ? od : d8;
        if (nb == 0) continue;
        if (od && d8 && od != d8) continue;   /* 压缩存储：跳过 */
        /* ★ 子容器用「文件绝对偏移」寻址，不做全量缓冲 */
        n_sub += hide_hd_rec_fp(f, base + (long long)off * 512, st, depth + 1);
    }
    gh_free(&gh);
    return n_here + n_sub;
}

/* ---------- ② 转换内嵌 rpf 的扩展名（等长，直接改）—— v6: 流式（只碰头部区） ---------- */
static int conv_inner_fp(FILE *f, long long base, GcDlcStats *st, int depth) {
    if (depth > MAX_NEST_DEPTH) return GC_DLC_OK;
    GcHead gh;
    int rc = gh_load(f, base, &gh);
    if (rc != GC_DLC_OK) return rc;       /* 不是合法 RPF7 ⇒ 当普通数据跳过 */
    RpfHdr *h = &gh.h;
    int changed = 0;

    /* ★ v27: _hi → 低模重定向（等长改 TOC，放最前） */
    if (hi_to_lod(gh.head, h, st) > 0) changed = 1;

    for (u32 i = 0; i < h->entry_count; i++) {
        u8 *p = gh.head + h->toc_off + (size_t)i * 16;
        u64 q0 = rd_u64(p);
        if (entry_is_dir(q0)) continue;
        u32 name_off = (u32)(q0 & 0xFFFF);
        u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        int is_res = (int)((q0 >> 63) & 1);
        u32 d8 = rd_u32(p + 8);

        char nm[256];
        entry_name(gh.head, h, name_off, nm, sizeof(nm));
        if (!nm[0]) continue;

        /* 资源名改名（等长替换：直接改名字池那几个字节） */
        char newnm[256];
        if (rename_name(nm, newnm, sizeof(newnm))) {
            size_t lo = strlen(nm), lnew = strlen(newnm);
            if (lo == lnew) {
                size_t o = h->names_off + ((size_t)name_off << h->shift);
                if (o + lo <= h->names_off + h->names_len) {
                    memcpy(gh.head + o, newnm, lo);
                    if (st) st->n_inner_renamed++;
                    changed = 1;
                }
            }
        }

        /* 递归内嵌 rpf（只处理未压缩存储的；压缩的跳过并计数） */
        size_t ln = strlen(nm);
        if (!is_res && ln > 4) {
            char tail[5];
            memcpy(tail, nm + ln - 4, 4); tail[4] = 0;
            if (strcasecmp(tail, ".rpf") == 0) {
                u32 nb = od ? od : d8;
                if (nb == 0) continue;
                if (od && od != d8) {
                    /* 压缩存储：解压→改→重压，长度会变，风险大 ⇒ 跳过并报告 */
                    if (st) st->n_skipped_compressed++;
                    continue;
                }
                if (st) st->n_nested_rpf++;
                /* ★ 子容器用文件绝对偏移寻址（不做全量缓冲） */
                conv_inner_fp(f, base + (long long)off * 512, st, depth + 1);
            }
        }
    }
    if (changed) { gh.dirty = 1; gh_flush(f, base, &gh); }
    gh_free(&gh);
    return GC_DLC_OK;
}

/* ---------- ① 转换顶层名字池（变长，需重建 + 重映射）—— v6: 流式版 ----------
 *   头部区（header+TOC+names）在 gh->head 缓冲里；数据区靠 fseek 逐条搬。
 *   file_len 用于判断「空隙到文件末尾」。 */
static int conv_top_names_fp(FILE *f, GcHead *gh, long long file_len,
                             GcDlcStats *st, char *err, size_t errsz) {
    RpfHdr *h = &gh->h;
    if (h->shift != 0) {
        put_err(err, errsz, "name_shift=%u 不为 0，暂不支持", h->shift);
        return GC_DLC_ERR_SHIFT;
    }
    u8 *b = gh->head;                 /* 头部区缓冲（names 也在里面） */
    const u8 *pool = b + h->names_off;
    u32 plen = h->names_len;

    /* 重建名字池：逐 NUL 切分，改名后拼接 */
    u8 *np = (u8*)malloc((size_t)plen + 4096);
    if (!np) return GC_DLC_ERR_NOMEM;
    u32 nlen = 0;
    int changed = 0;

    typedef struct { u32 old_off, new_off; } Remap;
    Remap *rm = (Remap*)malloc(sizeof(Remap) * ((size_t)h->entry_count + 8));
    if (!rm) { free(np); return GC_DLC_ERR_NOMEM; }
    int nrm = 0;

    u32 pos = 0;
    while (pos < plen) {
        u32 z = pos;
        while (z < plen && pool[z] != 0) z++;
        u32 seglen = z - pos;
        char nm[256];
        u32 cp = seglen < 255 ? seglen : 255;
        memcpy(nm, pool + pos, cp); nm[cp] = 0;

        char newnm[256];
        if (rename_name(nm, newnm, sizeof(newnm))) {
            changed++;
            if (st) st->n_top_renamed++;
        } else {
            snprintf(newnm, sizeof(newnm), "%s", nm);
        }
        if (nrm < (int)h->entry_count + 8) {
            rm[nrm].old_off = pos; rm[nrm].new_off = nlen; nrm++;
        }
        size_t nl2 = strlen(newnm);
        memcpy(np + nlen, newnm, nl2);
        nlen += (u32)nl2;
        np[nlen++] = 0;
        pos = z + 1;
    }
    u32 aligned = nlen;               /* ★ 不做 16 对齐（见旧注释，省空间） */

    /* ---- 名字池扩展会侵入数据区时，流式搬走被侵入的条目 ---- */
    if (aligned > plen) {
        /* 头部区缓冲只有 plen 字节名字池 —— 先扩容以容纳新池
         * （数据区不在缓冲里，所以扩容只影响缓冲尾部，安全） */
        size_t new_head_len = 16 + (size_t)h->entry_count * 16 + aligned;
        if (new_head_len > GC_HEAD_MAX) {
            free(np); free(rm);
            put_err(err, errsz, "新名字池过大 (%zu B)", new_head_len);
            return GC_DLC_ERR_NOSPACE;
        }
        size_t old_head_len = gh->head_len;
        u8 *nb = (u8*)realloc(gh->head, new_head_len);
        if (!nb) { free(np); free(rm); return GC_DLC_ERR_NOMEM; }
        if (new_head_len > old_head_len) memset(nb + old_head_len, 0, new_head_len - old_head_len);
        gh->head = nb; gh->head_len = new_head_len;
        b = gh->head; h->names_len = aligned;   /* 缓冲内的 names 区变长 */

        /* 找会被侵入的条目（起点 < new_names_end 且 offset != 0）并流式搬走 */
        size_t new_names_end = 16 + (size_t)h->entry_count * 16 + aligned;
        int moved = 0;
        for (int round = 0; round < 8; round++) {
            int victim = -1;
            long long vstart = 0;
            u32 vlen = 0;
            for (u32 i = 0; i < h->entry_count; i++) {
                u8 *p = gh->head + h->toc_off + (size_t)i * 16;
                u64 q0 = rd_u64(p);
                if (entry_is_dir(q0)) continue;
                u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
                if (off == 0) continue;
                long long s = (long long)off * 512;
                if (s < (long long)new_names_end && (victim < 0 || s < vstart)) {
                    u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
                    u32 sz = rd_u32(p + 8);
                    u32 len = od ? od : sz;
                    if (len == 0) continue;
                    victim = (int)i; vstart = s; vlen = len;
                }
            }
            if (victim < 0) break;

            /* 找一块 512 对齐的空闲区（扫描 TOC 收集所有占用区间） */
            typedef struct { long long s, e; } Iv;
            static Iv iv[1200];
            int n = 0;
            for (u32 i = 0; i < h->entry_count && n < 1200; i++) {
                u8 *p = gh->head + h->toc_off + (size_t)i * 16;
                u64 q0 = rd_u64(p);
                if (entry_is_dir(q0)) continue;
                u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
                u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
                u32 sz = rd_u32(p + 8);
                long long ln = od ? od : sz;
                if (ln == 0) continue;
                iv[n].s = (long long)off * 512;
                iv[n].e = iv[n].s + ln;
                n++;
            }
            for (int i2 = 0; i2 < n; i2++)
                for (int j2 = i2 + 1; j2 < n; j2++)
                    if (iv[j2].s < iv[i2].s) { Iv t = iv[i2]; iv[i2] = iv[j2]; iv[j2] = t; }

            long long cur = (long long)new_names_end;   /* ★ 只能用头部区之后的空间 */
            long long gap = -1;
            for (int k = 0; k <= n; k++) {
                long long nxt = (k < n) ? iv[k].s : file_len;
                if (nxt > cur) {
                    long long st2 = ((cur + 511) / 512) * 512;
                    while (st2 + (long long)vlen <= nxt) {
                        int overlap = (st2 < vstart + (long long)vlen && st2 + (long long)vlen > vstart);
                        if (!overlap) { gap = st2; break; }
                        st2 += 512;
                    }
                    if (gap >= 0) break;
                }
                if (k < n && iv[k].e > cur) cur = iv[k].e;
            }
            if (gap < 0) {
                char nm2[128];
                entry_name(gh->head, h, (u32)(rd_u64(gh->head + h->toc_off + (size_t)victim * 16) & 0xFFFF), nm2, sizeof(nm2));
                free(np); free(rm);
                put_err(err, errsz,
                        "名字池需扩到 0x%zX，但「%s」(%u B) 找不到 512 对齐的空闲区可搬",
                        new_names_end, nm2, vlen);
                return GC_DLC_ERR_NOSPACE;
            }

            /* ★ 流式搬移（分块，支持重叠方向） */
            int rc2 = gh_data_move(f, gap, vstart, (size_t)vlen);
            if (rc2 != GC_DLC_OK) { free(np); free(rm); return rc2; }
            rc2 = gh_data_zero(f, vstart, (size_t)vlen);
            if (rc2 != GC_DLC_OK) { free(np); free(rm); return rc2; }

            /* 改 TOC 的 offset（扇区号） */
            u8 *p = gh->head + h->toc_off + (size_t)victim * 16;
            u64 q0 = rd_u64(p);
            q0 = (q0 & ~(((u64)0x7FFFFF) << 40)) |
                 ((((u64)(gap / 512)) & 0x7FFFFF) << 40);
            wr_u64(p, q0);
            gh->dirty = 1;
            moved++;
        }
        if (st) st->n_relocated = moved;
    }

    if (!changed) { free(np); free(rm); return GC_DLC_OK; }

    /* 写回名字池（在头部区缓冲内） */
    memcpy(gh->head + h->names_off, np, nlen);
    free(np);

    /* 更新 header 的 names_len */
    u32 nl_field = (u32)aligned | (h->shift << 28);
    gh->head[8]  = (u8)(nl_field & 0xFF);
    gh->head[9]  = (u8)((nl_field >> 8) & 0xFF);
    gh->head[10] = (u8)((nl_field >> 16) & 0xFF);
    gh->head[11] = (u8)((nl_field >> 24) & 0xFF);
    h->names_len = aligned;

    /* 重映射每个条目的 name_off */
    for (u32 i = 0; i < h->entry_count; i++) {
        u8 *p = gh->head + h->toc_off + (size_t)i * 16;
        u64 q0 = rd_u64(p);
        u32 old_off = (u32)(q0 & 0xFFFF);
        u32 new_off = old_off;
        for (int k = 0; k < nrm; k++) {
            if (rm[k].old_off == old_off) { new_off = rm[k].new_off; break; }
        }
        if (new_off != old_off) {
            q0 = (q0 & ~(u64)0xFFFF) | (u64)(new_off & 0xFFFF);
            wr_u64(p, q0);
        }
    }
    free(rm);
    if (st) st->names_grew += (int)aligned - (int)plen;
    gh->dirty = 1;
    return GC_DLC_OK;
}

/* ---------- ③ 判是否已是 Switch 格式 ---------- */
static int check_format_impl(const u8 *b, size_t blen, int *has_x64, int *has_sw) {
    RpfHdr h;
    if (parse_hdr(b, blen, 0, &h) != GC_DLC_OK) return GC_DLC_ERR_FORMAT;
    *has_x64 = 0; *has_sw = 0;
    for (u32 i = 0; i < h.entry_count; i++) {
        u64 q0 = rd_u64(b + h.toc_off + (size_t)i * 16);
        if (!entry_is_dir(q0)) continue;
        char nm[256];
        entry_name(b, &h, (u32)(q0 & 0xFFFF), nm, sizeof(nm));
        /* ★ v5.0.2: 有些非标准打包工具会让目录名带 1 字节垃圾前缀（实测 \xb8switch）
         *   ⇒ 跳过开头的非 ASCII 字节再比较，否则会误报「PC 格式」
         *   （游戏引擎不介意该前缀，能正常加载 ⇒ 只是显示不对） */
        const char *cl = nm;
        while (*cl && (unsigned char)*cl > 127) cl++;
        if (strcasecmp(cl, "x64") == 0)    *has_x64 = 1;
        if (strcasecmp(cl, "switch") == 0) *has_sw = 1;
    }
    return GC_DLC_OK;
}

/* ---------- 主入口 ---------- */
/* ★ v6 (P0): 改为「头部区缓冲」—— 不再全文件读入，支持任意大小（实测 2.4 GB 通过）
 *   旧实现：`if (fsz > 512MB) 报错` + `malloc(fsz)` + `fwrite(全量)`
 *   新实现：只 malloc header+TOC+names（KB 级）；数据区靠 fseek 逐条搬 */
int gc_dlc_convert(const char *path, GcDlcStats *st, char *err, size_t errsz) {
    if (st) memset(st, 0, sizeof(*st));
    set_err("");

    FILE *f = fopen(path, "r+b");          /* 直接可写打开（省一次 reopen） */
    if (!f) {
        put_err(err, errsz, "打不开文件（只读？errno=%d）", errno);
        return GC_DLC_ERR_OPEN;
    }
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); put_err(err, errsz, "取文件大小失败"); return GC_DLC_ERR_IO; }
    long long fsz = (long long)ftello(f);
    if (fsz <= 0) { fclose(f); put_err(err, errsz, "文件大小异常 (%lld B)", fsz); return GC_DLC_ERR_FORMAT; }

    GcHead gh;
    int rc = gh_load(f, 0, &gh);
    if (rc != GC_DLC_OK) {
        fclose(f);
        put_err(err, errsz, "不是 RPF7 容器或头部区异常 (rc=%d)", rc);
        return GC_DLC_ERR_FORMAT;
    }

    /* ① 顶层名字池（可能需要流式搬移条目） */
    rc = conv_top_names_fp(f, &gh, fsz, st, err, errsz);
    if (rc != GC_DLC_OK) { gh_free(&gh); fclose(f); return rc; }
    if (gh.dirty) { rc = gh_flush(f, 0, &gh); if (rc != GC_DLC_OK) { gh_free(&gh); fclose(f); return rc; } }

    /* ② 内嵌 rpf 扩展名（流式，只碰各子容器头部区） */
    conv_inner_fp(f, 0, st, 0);

    /* ②b 屏蔽高清贴图（流式，只改名，零搬移） */
    hide_hd_rec_fp(f, 0, st, 0);

    gh_free(&gh);
    fflush(f);
    fsdevCommitDevice("sdmc");
    fclose(f);
    return GC_DLC_OK;
}

/* ★ v5.4: 独立入口 —— 只屏蔽高清贴图（不改格式、不改扩展名）
 *   v6 (P0): 改为流式，支持任意大小包（实测 mpheist 2.4 GB 通过）
 *   幂等：已屏蔽过的（'+' 已变 '~'）再跑返回 0。
 *   返回：>=0 = 屏蔽掉的条目数；<0 = GC_DLC_ERR_* */
int gc_dlc_hide_hd_textures(const char *path, int *removed, char *err, size_t errsz) {
    if (removed) *removed = 0;
    set_err("");

    FILE *f = fopen(path, "r+b");
    if (!f) {
        put_err(err, errsz, "打不开文件（只读？errno=%d）", errno);
        return GC_DLC_ERR_OPEN;
    }
    /* 只验证是个合法 RPF7 头部即可，不读全文件 */
    GcHead gh;
    if (gh_load(f, 0, &gh) != GC_DLC_OK) {
        fclose(f);
        put_err(err, errsz, "不是 RPF7 容器或头部区异常");
        return GC_DLC_ERR_FORMAT;
    }
    gh_free(&gh);

    GcDlcStats st;
    memset(&st, 0, sizeof(st));
    int n = hide_hd_rec_fp(f, 0, &st, 0);      /* 内部按需写回头部区 */

    if (n == 0) {                              /* 没有可屏蔽的 ⇒ 无需提交 */
        fclose(f);
        if (removed) *removed = 0;
        return 0;
    }
    fflush(f);
    fsdevCommitDevice("sdmc");
    fclose(f);
    if (removed) *removed = n;
    return n;
}

int gc_dlc_is_switch_format(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    u8 hdr[16];
    if (fread(hdr, 1, 16, f) != 16) { fclose(f); return -1; }
    u32 ec = rd_u32(hdr + 4);
    u32 nl = rd_u32(hdr + 8);
    u32 nlen = nl & 0x0FFFFFFF;
    if (ec == 0 || ec > DLC_MAX_ENTRIES) { fclose(f); return -1; }
    size_t need = 16 + (size_t)ec * 16 + nlen;
    if (need > 1024 * 1024) { fclose(f); return -1; }
    u8 *buf = (u8*)malloc(need);
    if (!buf) { fclose(f); return -1; }
    fseeko(f, 0, SEEK_SET);
    size_t got = fread(buf, 1, need, f);
    fclose(f);
    if (got != need) { free(buf); return -1; }
    int hx = 0, hs = 0;
    int rc = check_format_impl(buf, got, &hx, &hs);
    free(buf);
    if (rc != GC_DLC_OK) return -1;
    if (hs && !hx) return 1;    /* 已是 Switch 格式 */
    if (hx) return 0;           /* 还是 PC 格式 */
    return 0;
}

/* ========================================================================= */
/* ★ v31: 地图数据自检（只读 RPF 头部，不加载数据体）                          */
/* ========================================================================= */
/* 判定主 rpf：名字里【不含】"metadata" 的那个 .rpf
   （顺序不可靠：AkinaV 是 metadata 在前，hospital 是主 rpf 在前 ⇒ 只能按名字判）
   已知判据（2026-09-30 实测）：manifest 不在主 rpf ⇒ 地图实体不注册
   ⇒ 贴图能显示、模型不渲染、无碰撞（AkinaV 秋名山的症状） */

typedef struct { u32 ec, nl, shift; long long names_off; } MapHdr;

static int map_read_hdr(FILE *f, long long base, MapHdr *h) {
    u8 b[16];
    if (fseeko(f, base, SEEK_SET) != 0) return -1;
    if (fread(b, 1, 16, f) != 16) return -1;
    if (rd_u32(b) != RPF7_MAGIC) return -1;
    h->ec = rd_u32(b + 4);
    u32 nlraw = rd_u32(b + 8);
    h->nl = nlraw & 0x0FFFFFFFu;
    h->shift = nlraw >> 28;
    if (h->ec == 0 || h->ec > DLC_MAX_ENTRIES) return -1;
    if (h->nl == 0 || h->nl > 8u * 1024u * 1024u) return -1;
    h->names_off = base + 16 + (long long)h->ec * 16;
    return 0;
}

/* 在名字池里做【完整名字】匹配（前后都必须是 NUL 或池边界） */
static int map_pool_has(FILE *f, const MapHdr *h, const char *needle) {
    size_t nlen = strlen(needle);
    if (h->nl < nlen) return 0;
    u8 *buf = (u8*)malloc(h->nl);
    if (!buf) return 0;
    if (fseeko(f, h->names_off, SEEK_SET) != 0 ||
        fread(buf, 1, h->nl, f) != h->nl) { free(buf); return 0; }
    int found = 0;
    for (u32 i = 0; i + nlen <= h->nl; i++) {
        if (buf[i] != (u8)needle[0]) continue;
        if (memcmp(buf + i, needle, nlen) != 0) continue;
        int ok_start = (i == 0) || (buf[i - 1] == 0);
        int ok_end   = (i + nlen >= h->nl) || (buf[i + nlen] == 0);
        if (ok_start && ok_end) { found = 1; break; }
    }
    free(buf);
    return found;
}

static int map_read_name(FILE *f, const MapHdr *h, u32 name_off,
                         char *out, size_t outsz) {
    out[0] = 0;
    long long o = h->names_off + ((long long)name_off << h->shift);
    if (o < h->names_off || o >= h->names_off + h->nl) return -1;
    if (fseeko(f, o, SEEK_SET) != 0) return -1;
    size_t n = 0;
    while (n + 1 < outsz) {
        int c = fgetc(f);
        if (c == EOF || c == 0) break;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}

static int map_has_suffix(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t k = 0; k < b; k++) {
        char x = s[a - b + k];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (x != suf[k]) return 0;
    }
    return 1;
}

int gc_dlc_check_map(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return GC_MAP_ERR;
    MapHdr h;
    if (map_read_hdr(f, 0, &h) != 0) { fclose(f); return GC_MAP_ERR; }

    typedef struct { long long off; int is_main; } RpfEnt;
    static RpfEnt ents[24];
    int ne = 0;
    for (u32 i = 0; i < h.ec && ne < 24; i++) {
        u8 e[16];
        if (fseeko(f, 16 + (long long)i * 16, SEEK_SET) != 0) break;
        if (fread(e, 1, 16, f) != 16) break;
        u64 q0 = rd_u64(e);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;          /* 目录 */
        u32 name_off = (u32)(q0 & 0xFFFF);
        u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
        u32 sz = rd_u32(e + 8);
        char nm[160];
        if (map_read_name(f, &h, name_off, nm, sizeof(nm)) != 0) continue;
        if (!map_has_suffix(nm, ".rpf")) continue;
        if ((od ? od : sz) == 0) continue;
        ents[ne].off = (long long)off * 512;
        ents[ne].is_main = (strstr(nm, "metadata") == NULL);
        ne++;
    }

    int has_map_res = 0, man_in_main = 0;
    for (int k = 0; k < ne; k++) {
        MapHdr h2;
        if (map_read_hdr(f, ents[k].off, &h2) != 0) continue;
        int man = map_pool_has(f, &h2, "_manifest.nmf") ||
                  map_pool_has(f, &h2, "_manifest.ymf");
        int mp  = map_pool_has(f, &h2, ".nmap") || map_pool_has(f, &h2, ".ymap") ||
                  map_pool_has(f, &h2, ".ntyp") || map_pool_has(f, &h2, ".ytyp");
        if (mp) has_map_res = 1;
        if (man && ents[k].is_main) man_in_main = 1;
    }
    fclose(f);

    if (!has_map_res) return GC_MAP_NONE;
    return man_in_main ? GC_MAP_OK : GC_MAP_BAD;
}

/* ========================================================================= */
/* ★ v32: 资源构成统计（只读头部 + 名字池）                                    */
/* ========================================================================= */

/* 判断名字的扩展名是否为 ext（不含点，大小写不敏感） */
static int name_ext_is(const char *nm, size_t L, const char *ext) {
    size_t e = strlen(ext);
    if (L < e + 2) return 0;                 /* 至少 "a.xx" */
    if (nm[L - e - 1] != '.') return 0;
    for (size_t k = 0; k < e; k++) {
        char c = nm[L - e + k];
        if (c >= 'A' && c <= 'Z') c += 32;
        char x = ext[k];
        if (x >= 'A' && x <= 'Z') x += 32;
        /* ★ PC(.ydr) 与 Switch(.ndr) 首字母不同但属同类资源 ⇒ 视为等价，
         *   这样【待导入的 PC 格式包】也能正确统计出资源构成 */
        if (k == 0 && (c == 'y' || c == 'n') && (x == 'y' || x == 'n')) continue;
        if (c != x) return 0;
    }
    return 1;
}

static void count_pool(FILE *f, const MapHdr *h,
                       int *nm_, int *nmap, int *ntex, int *noth) {
    if (h->nl == 0 || h->nl > 8u * 1024u * 1024u) return;
    u8 *buf = (u8*)malloc(h->nl);
    if (!buf) return;
    if (fseeko(f, h->names_off, SEEK_SET) != 0 ||
        fread(buf, 1, h->nl, f) != h->nl) { free(buf); return; }
    u32 i = 0;
    while (i < h->nl) {
        u32 j = i;
        while (j < h->nl && buf[j] != 0) j++;
        size_t L = j - i;
        if (L > 0) {
            const char *p = (const char*)buf + i;
            int is_dir = (memchr(p, '.', L) == NULL);   /* 无扩展名 ⇒ 目录，不计 */
            int is_rpf = name_ext_is(p, L, "rpf");
            if (!is_dir && !is_rpf) {
                if (name_ext_is(p, L, "ndr") || name_ext_is(p, L, "nft")) {
                    if (nm_) (*nm_)++;
                } else if (name_ext_is(p, L, "nmap") || name_ext_is(p, L, "ntyp") ||
                           name_ext_is(p, L, "nnd")) {
                    if (nmap) (*nmap)++;
                } else if (name_ext_is(p, L, "ntd") || name_ext_is(p, L, "ndt")) {
                    if (ntex) (*ntex)++;
                } else {
                    if (noth) (*noth)++;
                }
            }
        }
        i = j + 1;
    }
    free(buf);
}

void gc_dlc_scan_res(const char *path,
                     int *n_model, int *n_map, int *n_tex, int *n_other) {
    if (n_model) *n_model = 0;
    if (n_map)   *n_map = 0;
    if (n_tex)   *n_tex = 0;
    if (n_other) *n_other = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return;
    MapHdr h;
    if (map_read_hdr(f, 0, &h) != 0) { fclose(f); return; }

    typedef struct { long long off; } SE;
    static SE ents[24];
    int ne = 0;
    for (u32 i = 0; i < h.ec && ne < 24; i++) {
        u8 e[16];
        if (fseeko(f, 16 + (long long)i * 16, SEEK_SET) != 0) break;
        if (fread(e, 1, 16, f) != 16) break;
        u64 q0 = rd_u64(e);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;
        u32 name_off = (u32)(q0 & 0xFFFF);
        u32 od = (u32)((q0 >> 16) & 0xFFFFFF);
        u32 sz = rd_u32(e + 8);
        char nm[160];
        if (map_read_name(f, &h, name_off, nm, sizeof(nm)) != 0) continue;
        if (!map_has_suffix(nm, ".rpf")) continue;
        if ((od ? od : sz) == 0) continue;
        ents[ne].off = (long long)off * 512;
        ne++;
    }

    /* 顶层条目也算（content.xml / setup2.xml 等） */
    count_pool(f, &h, n_model, n_map, n_tex, n_other);
    for (int k = 0; k < ne; k++) {
        MapHdr h2;
        if (map_read_hdr(f, ents[k].off, &h2) != 0) continue;
        count_pool(f, &h2, n_model, n_map, n_tex, n_other);
    }
    fclose(f);
}

/* ========================================================================= */
/* ★ v33: 统一探测 —— 一次 IO 拿到【格式 + 地图状态 + 资源构成】               */
/* ========================================================================= */
/* 性能背景（用户反馈「加资源构成后加载太慢」）：
 *   改造前每个包要调 3 个函数（is_switch_format / check_map / scan_res），
 *   且 check_map 内部对同一个名字池调了 6 次 map_pool_has（每次都 malloc+fseeko+fread）
 *   ⇒ 每个包约 10 次 SD 卡随机读，列表一多就明显卡。
 * 改造后：每个 RPF 只读 1 次头部 + 1 次名字池，所有判断在内存里做。
 */

static u8 *probe_read_pool(FILE *f, const MapHdr *h) {
    if (h->nl == 0 || h->nl > 8u * 1024u * 1024u) return NULL;
    u8 *buf = (u8*)malloc(h->nl);
    if (!buf) return NULL;
    if (fseeko(f, h->names_off, SEEK_SET) != 0 ||
        fread(buf, 1, h->nl, f) != h->nl) { free(buf); return NULL; }
    return buf;
}

/* 池内是否有【完整名字】name */
static int pool_has_name(const u8 *pool, u32 nl, const char *name) {
    size_t L = strlen(name);
    if (nl < L) return 0;
    for (u32 i = 0; i + L <= nl; i++) {
        if (pool[i] != (u8)name[0]) continue;
        if (memcmp(pool + i, name, L) != 0) continue;
        if ((i == 0 || pool[i - 1] == 0) && (i + L >= nl || pool[i + L] == 0))
            return 1;
    }
    return 0;
}

/* ★ v5.0.2: 宽松名字匹配 —— 允许名字前带 1~2 字节非 ASCII 垃圾前缀
 *   （非标准打包工具所致；实测 \xb8switch / \xc1switch） */
static int pool_has_name_loose(const u8 *pool, u32 nl, const char *name) {
    if (pool_has_name(pool, nl, name)) return 1;
    size_t L = strlen(name);
    for (u32 skip = 1; skip <= 2; skip++) {
        size_t LL = L + skip;
        for (u32 i = 0; i + LL <= nl; i++) {
            if (memcmp(pool + i + skip, name, L) != 0) continue;
            int ok_pre = (i == 0) || (pool[i - 1] == 0);
            int ok_end = (i + LL >= nl) || (pool[i + LL] == 0);
            if (!ok_pre || !ok_end) continue;
            int junk = 0;
            for (u32 k = 0; k < skip; k++)
                if (pool[i + k] < 128) junk = 1;
            if (!junk) return 1;
        }
    }
    return 0;
}

/* 池内是否有以 ext 结尾的完整名字（复用 name_ext_is，已支持 y/n 等价） */
static int pool_has_ext(const u8 *pool, u32 nl, const char *ext) {
    u32 i = 0;
    while (i < nl) {
        u32 j = i;
        while (j < nl && pool[j]) j++;
        if (j > i && name_ext_is((const char*)pool + i, j - i, ext)) return 1;
        i = j + 1;
    }
    return 0;
}

/* 大小写不敏感前缀比较（自带，避免 MSVC/newlib 的 _strnicmp / strncasecmp 差异） */
static int ci_prefix_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* 池内是否存在与 base 配对的 "<base>_hi.nft"（低模/高模成对出现才算真车名） */
static int pool_has_hi_pair(const u8 *pool, u32 nl, const char *base) {
    size_t bl = strlen(base);
    for (u32 i = 0; i < nl; ) {
        u32 j = i;
        while (j < nl && pool[j]) j++;
        size_t L = j - i;
        const char *p = (const char*)pool + i;
        /* 形如 <base>_hi.nft（大小写不敏感） */
        if (L == bl + 7 && ci_prefix_eq(p, base, bl) &&
            (p[bl] == '_') &&
            (p[bl+1] == 'h' || p[bl+1] == 'H') &&
            (p[bl+2] == 'i' || p[bl+2] == 'I') &&
            (p[bl+3] == '.') &&
            (p[bl+4] == 'n' || p[bl+4] == 'N') &&
            (p[bl+5] == 'f' || p[bl+5] == 'F') &&
            (p[bl+6] == 't' || p[bl+6] == 'T'))
            return 1;
        i = j + 1;
    }
    return 0;
}

/* ★ v5.3: 取车辆刷车名 —— 必须是「有 _hi 配对的基名」
 *
 * 实测（2026-09-30）：
 *   车辆 mod 的 .nft 分两类：
 *     · 改装件：su7_bon_1.nft / ae86_hood_3.nft ...（数量多，无 _hi 配对）
 *     · 本体车：misu7ultra.nft + misu7ultra_hi.nft（成对出现）
 *   旧实现「取第一个非 _hi 的 .nft」会被改装件抢先 ⇒ 刷车名错成改装件名。
 *   正确判据 = 只认【有 <名>_hi.nft 配对】的那个基名，即可跨 rpf 命中真车名。
 *
 *   另外 vehiclemods 的条目名常以车名开头（su7_* / ae86_*），但 base 与车名
 *   不一定全等 ⇒ 不用前缀，只认 _hi 配对（最可靠）。
 *
 * v5.3 补充：只在名字含 "vehicle" 的 rpf 里提取（避免地图包的 .nft 被误当车名）；
 *   若严格判据无命中，降级为「第一个非 _hi 的 .nft」作兜底。 */
static void pool_first_vehicle(const u8 *pool, u32 nl, char *out, size_t outsz) {
    out[0] = 0;
    if (!pool || nl == 0) return;

    /* ---- 第一轮：严格判据（有 _hi 配对）---- */
    u32 i = 0;
    while (i < nl) {
        u32 j = i;
        while (j < nl && pool[j]) j++;
        size_t L = j - i;
        if (L > 5 && name_ext_is((const char*)pool + i, L, "nft")) {
            const char *p = (const char*)pool + i;
            int is_hi = 0;
            if (L >= 8) {     /* 排除 xxx_hi.nft 本身 */
                char t0 = p[L-8], t1 = p[L-7], t2 = p[L-6];
                if (t0 == '_' && (t1 == 'h' || t1 == 'H') && (t2 == 'i' || t2 == 'I'))
                    is_hi = 1;
            }
            if (!is_hi) {
                size_t base = L - 4;                 /* 去掉 ".nft" */
                char tmp[64];
                if (base >= sizeof(tmp)) base = sizeof(tmp) - 1;
                memcpy(tmp, p, base);
                tmp[base] = 0;
                if (pool_has_hi_pair(pool, nl, tmp)) {   /* ★ 只认真车名 */
                    if (base >= outsz) base = outsz - 1;
                    memcpy(out, tmp, base);
                    out[base] = 0;
                    return;
                }
            }
        }
        i = j + 1;
    }

    /* ---- 第二轮：兜底（无 _hi 配对时取第一个非 _hi 的 .nft）---- */
    i = 0;
    while (i < nl) {
        u32 j = i;
        while (j < nl && pool[j]) j++;
        size_t L = j - i;
        if (L > 5 && name_ext_is((const char*)pool + i, L, "nft")) {
            const char *p = (const char*)pool + i;
            int is_hi = 0;
            if (L >= 8) {
                char t0 = p[L-8], t1 = p[L-7], t2 = p[L-6];
                if (t0 == '_' && (t1 == 'h' || t1 == 'H') && (t2 == 'i' || t2 == 'I'))
                    is_hi = 1;
            }
            if (!is_hi) {
                size_t base = L - 4;
                if (base >= outsz) base = outsz - 1;
                memcpy(out, p, base);
                out[base] = 0;
                return;
            }
        }
        i = j + 1;
    }
}

/* 一次遍历池：统计模型/地图/贴图构成 */
static void pool_count(const u8 *pool, u32 nl,
                       int *nm, int *nmap, int *ntex, int *noth) {
    u32 i = 0;
    while (i < nl) {
        u32 j = i;
        while (j < nl && pool[j]) j++;
        size_t L = j - i;
        if (L > 0) {
            const char *p = (const char*)pool + i;
            if (memchr(p, '.', L) != NULL && !name_ext_is(p, L, "rpf")) {
                if (name_ext_is(p, L, "ndr") || name_ext_is(p, L, "nft")) {
                    if (nm) (*nm)++;
                } else if (name_ext_is(p, L, "nmap") || name_ext_is(p, L, "ntyp") ||
                           name_ext_is(p, L, "nnd")) {
                    if (nmap) (*nmap)++;
                } else if (name_ext_is(p, L, "ntd") || name_ext_is(p, L, "ndt")) {
                    if (ntex) (*ntex)++;
                } else {
                    if (noth) (*noth)++;
                }
            }
        }
        i = j + 1;
    }
}

void gc_dlc_probe(const char *path, GcDlcProbe *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->fmt = -1;
    out->map_st = GC_MAP_ERR;

    FILE *f = fopen(path, "rb");
    if (!f) return;
    MapHdr h;
    if (map_read_hdr(f, 0, &h) != 0) { fclose(f); return; }

    u8 *pool = probe_read_pool(f, &h);
    if (!pool) { fclose(f); return; }

    /* ---- 格式判定（顶层名字池里有 x64 / switch 目录名）---- */
    {
        int has_x64 = pool_has_name_loose(pool, h.nl, "x64");
        int has_sw  = pool_has_name_loose(pool, h.nl, "switch");
        out->fmt = (has_sw && !has_x64) ? 1 : 0;
    }
    /* ---- 顶层条目统计（content.xml / setup2.xml 等）---- */
    pool_count(pool, h.nl, &out->n_model, &out->n_map, &out->n_tex, NULL);

    /* ---- 收集 .rpf 条目（名字直接从池里取，不再逐条 fseeko）---- */
    static struct { long long off; int is_main; int is_veh; } ents[24];
    int ne = 0;
    for (u32 i = 0; i < h.ec && ne < 24; i++) {
        u8 e[16];
        if (fseeko(f, 16 + (long long)i * 16, SEEK_SET) != 0) break;
        if (fread(e, 1, 16, f) != 16) break;
        u64 q0 = rd_u64(e);
        int is_res = (int)((q0 >> 63) & 1);
        u32 off = (u32)((q0 >> 40) & 0x7FFFFF);
        if (!is_res && off == 0x7FFFFF) continue;              /* 目录 */
        u32 name_off = (u32)(q0 & 0xFFFF);
        u32 o = name_off << h.shift;
        if (o >= h.nl) continue;
        const char *nm = (const char*)pool + o;
        if (!map_has_suffix(nm, ".rpf")) continue;
        ents[ne].off = (long long)off * 512;
        ents[ne].is_main = (strstr(nm, "metadata") == NULL);
        ents[ne].is_veh  = (strstr(nm, "vehicle") != NULL) || (strstr(nm, "vehicles") != NULL);
        ne++;
    }
    free(pool);
    pool = NULL;

    /* ---- 内层 rpf：每个只读 1 次名字池，一次遍历做完所有判断 ---- */
    int has_map_res = 0, man_in_main = 0;
    for (int k = 0; k < ne; k++) {
        MapHdr h2;
        if (map_read_hdr(f, ents[k].off, &h2) != 0) continue;
        u8 *p2 = probe_read_pool(f, &h2);
        if (!p2) continue;
        if (!has_map_res &&
            (pool_has_ext(p2, h2.nl, "nmap") || pool_has_ext(p2, h2.nl, "ntyp") ||
             pool_has_ext(p2, h2.nl, "nnd")))
            has_map_res = 1;
        if (!man_in_main && ents[k].is_main &&
            (pool_has_name(p2, h2.nl, "_manifest.nmf") ||
             pool_has_name(p2, h2.nl, "_manifest.ymf")))
            man_in_main = 1;
        pool_count(p2, h2.nl, &out->n_model, &out->n_map, &out->n_tex, NULL);
        /* ★ v5.3: 只在车辆类 rpf 里提刷车名（避免地图包误判） */
        if (!out->vehicle[0] && ents[k].is_veh)
            pool_first_vehicle(p2, h2.nl, out->vehicle, sizeof(out->vehicle));
        free(p2);
    }
    fclose(f);

    if (!has_map_res)          out->map_st = GC_MAP_NONE;
    else if (man_in_main)      out->map_st = GC_MAP_OK;
    else                       out->map_st = GC_MAP_BAD;
}
