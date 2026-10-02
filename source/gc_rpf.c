/* gc_rpf.c —— RPF7 单条目原位替换实现
 *
 * 核心算法（已验证于真实 889 MB update.rpf）：
 *   1. 读 header → entry_count / names_len / encryption
 *   2. 读 TOC（原样保留在内存，改完只 patch 目标条目）
 *   3. 找 gameconfig.xml → idx / on_disk / size / offset
 *   4. 读 offset*512 处 on_disk 字节 → zlib inflate
 *   5. 改 XML → deflate (level 9, wbits=-15)
 *   6. 写回原偏移 + 补零（补齐到 on_disk）
 *   7. patch TOC: entry+2 的 3 字节 = on_disk ；entry+8 的 4 字节 = size
 *   8. 重新解析 + inflate 比对
 *
 * ⚠️ 绝不改文件大小、绝不动其它条目
 */
#include "gc_rpf.h"
#include "gc_key_container.h"     /* ★ key → 正确容器路径 映射 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <zlib.h>

const char *gc_rpf_candidates[GC_NUM_CANDIDATES] = {
    "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update/update.rpf",
    "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update.rpf",
    "sdmc:/atmosphere/contents/0100B00B51230000/romfs/update/update.rpf",
};

/* 最近一次失败的原因（给界面显示） */
static char s_last_err[256] = {0};
const char *gc_rpf_last_error(void) { return s_last_err; }

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_err, sizeof(s_last_err), fmt, ap);
    va_end(ap);
}

/* 确保 sdmc: 可用。
 *
 * 🚨 关键教训：不能把 fsdevMountSdmc() 的返回值当致命判据！
 *   libnx 文档写的是「mounts the sdmc device **if accessible**」——
 *   如果 sdmc 已经挂载（本程序其它地方已在用 sdmc:/switch/...），
 *   重复挂载会返回 AlreadyMounted 之类的错误。
 *   第一版直接把失败当致命错误 ⇒ 明明能打开的文件也报"打开失败"。
 *
 * 正确做法：**先探测文件能不能打开**，能打开就直接用；
 *           只有打不开时才尝试挂载，然后再探测一次。
 *           挂载失败也不算致命（可能本来就挂好了，或路径真的不存在）。 */
static int s_sdmc_tried = 0;

static int try_mount_sdmc(void) {
    if (s_sdmc_tried) return 0;
    s_sdmc_tried = 1;
    Result rc = fsdevMountSdmc();
    /* 挂载失败不致命：可能已经挂载过，也可能是真的没 SD 卡。
     * 真正的判据是「文件能不能打开」。 */
    return R_SUCCEEDED(rc) ? 1 : 0;
}

/* ★★★ 889 MB 文件的 seek 必须用 64 位接口！
 *
 * newlib 的 FILE 结构里用 `_off_t _offset` 存当前偏移，
 * 而 `fseek(FILE*, long, int)` 的参数是 `long` —— 在 aarch64 上 long 是 64 位，
 * 但 newlib 内部可能把它截断成 32 位（取决于 _off_t 的宽度配置）。
 * 实测：对 932 MB 的文件用 fseek(0, SEEK_END)+ftell 拿到的偏移不可靠，
 * 导致后续 fread 失败 ⇒ 表现为「文件存在但打不开」。
 *
 * 正确做法：统一用 `fseeko(FILE*, off_t, int)` + `ftello()`（64 位语义）。
 */
static int       gcs_seek(FILE *f, long long off);   /* 前置声明 */
static long long gcs_size(FILE *f);

/* 探测文件是否存在 + 大小（用于给出精确诊断） */
static long probe_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long long sz = gcs_size(f);      /* ★ 64 位安全 */
    fclose(f);
    return (sz < 0) ? -1 : (long)sz;
}

/* ---------------------------------------------------------------- 小工具 */
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr_u32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}
static u64 rd_u64(const u8 *p) {
    return (u64)rd_u32(p) | ((u64)rd_u32(p + 4) << 32);
}

/* 定义（前置声明在上方） */
static int gcs_seek(FILE *f, long long off) {
    return fseeko(f, (off_t)off, SEEK_SET);
}
static long long gcs_size(FILE *f) {
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    off_t sz = ftello(f);
    return (long long)sz;
}

/* ---------------------------------------------------------------- 打开 */
int gc_rpf_open(const char *path, GcRpf *r) {
    memset(r, 0, sizeof(*r));
    strncpy(r->path, path, sizeof(r->path) - 1);
    set_err("");

    /* ★ 先直接探测文件；打不开才尝试挂载 sdmc（避免重复挂载误判为失败） */
    long psz = probe_file(path);
    if (psz < 0) {
        try_mount_sdmc();
        psz = probe_file(path);
    }
    if (psz < 0) {
        set_err("打不开文件: %s", path);
        return GC_ERR_OPEN;
    }
    if (psz < 1024) {
        set_err("文件太小 (%ld B): %s", psz, path);
        return GC_ERR_OPEN;
    }

    /* 🚨🚨 关键：必须用【可写】模式打开！
     *   第一版写的 "rb"（只读）⇒ 读一切正常，但 gc_rpf_write() 里的
     *   fwrite 在只读流上必然失败 ⇒ 实机报「写入失败 (rc=-8)」= GC_ERR_IO。
     *   正确：先试 "r+b"（读写、不截断、不创建）；
     *         失败（SD 写保护 / FAT 只读属性 / 文件系统不支持）才退回 "rb"，
     *         并把 writable 置 0，让界面能给出「只读打开」这种明确提示。 */
    r->fp = fopen(path, "r+b");
    if (r->fp) {
        r->writable = 1;
    } else {
        r->fp = fopen(path, "rb");
        r->writable = 0;
    }
    if (!r->fp) {
        set_err("fopen 失败 (%ld B 的文件打不开)", psz);
        return GC_ERR_OPEN;
    }

    u8 hdr[16];
    if (fread(hdr, 1, 16, r->fp) != 16) {
        set_err("读 header 失败"); fclose(r->fp); r->fp = NULL; return GC_ERR_FORMAT;
    }
    if (rd_u32(hdr) != GC_RPF_MAGIC) {
        set_err("不是 RPF7 档案 (magic=0x%08X, 期望 0x%08X)", rd_u32(hdr), GC_RPF_MAGIC);
        fclose(r->fp); r->fp = NULL; return GC_ERR_FORMAT;
    }

    r->entry_count = rd_u32(hdr + 4);
    r->names_len   = rd_u32(hdr + 8) & 0x0FFFFFFFu;
    r->encryption  = rd_u32(hdr + 12);
    r->name_shift  = (rd_u32(hdr + 8) >> 28) & 0x7;

    /* 合理性检查（889 MB 的 rpf 约 1368~2000 条目） */
    if (r->entry_count == 0 || r->entry_count > 200000) {
        set_err("条目数异常: %u", r->entry_count);
        fclose(r->fp); r->fp = NULL; return GC_ERR_FORMAT;
    }
    if (r->names_len == 0 || r->names_len > 8u * 1024 * 1024) {
        set_err("名字池长度异常: %u", r->names_len);
        fclose(r->fp); r->fp = NULL; return GC_ERR_FORMAT;
    }

    /* 文件大小 */
    long long fs = gcs_size(r->fp);       /* ★ 64 位安全（内部会 fseeko 到末尾）*/
    if (fs <= 0) {
        set_err("取文件大小失败 (ftello)");
        fclose(r->fp); r->fp = NULL; return GC_ERR_FORMAT;
    }
    r->file_size = (size_t)fs;

    /* 🚨🚨 关键：TOC 必须从偏移 0x10（header 之后）开始读！
     *   上面 gcs_size() 内部 fseeko 到了文件末尾，
     *   如果这里 seek 到 0，就会把 header 当成 TOC 的第 0 个条目，
     *   整张表错位 16 字节 ⇒ 名字全乱 ⇒ 「找不到 gameconfig.xml」。
     *   （踩过：用户实机上报「文件存在但打不开」，真凶就是这一行）*/
    if (gcs_seek(r->fp, 0x10) != 0) {
        set_err("seek 到 TOC 失败");
        fclose(r->fp); r->fp = NULL; return GC_ERR_IO;
    }

    /* TOC */
    size_t toc_len = (size_t)r->entry_count * 16;
    r->toc = (u8*)malloc(toc_len);
    if (!r->toc) {
        set_err("内存不足 (TOC 需要 %zu B)", toc_len);
        fclose(r->fp); r->fp = NULL; return GC_ERR_NOMEM;
    }
    if (fread(r->toc, 1, toc_len, r->fp) != toc_len) {
        set_err("读 TOC 失败 (%zu B)", toc_len);
        free(r->toc); fclose(r->fp); r->fp = NULL; return GC_ERR_IO;
    }

    /* 名字池 */
    r->names = (u8*)malloc(r->names_len + 1);
    if (!r->names) {
        set_err("内存不足 (名字池 %u B)", r->names_len);
        free(r->toc); fclose(r->fp); r->fp = NULL; return GC_ERR_NOMEM;
    }
    if (fread(r->names, 1, r->names_len, r->fp) != r->names_len) {
        set_err("读名字池失败");
        free(r->names); free(r->toc); fclose(r->fp); r->fp = NULL; return GC_ERR_IO;
    }
    r->names[r->names_len] = 0;

    set_err("");
    return GC_OK;
}

void gc_rpf_close(GcRpf *r) {
    if (r->fp)   { fclose(r->fp); r->fp = NULL; }
    if (r->toc)  { free(r->toc);  r->toc = NULL; }
    if (r->names){ free(r->names);r->names = NULL; }
}

/* ★ 路径自检：逐个探测候选路径 + 目录，把结果写进 out（上机排查用）
 *   返回成功的路径数 */
int gc_rpf_selftest(char *out, size_t out_sz) {
    size_t w = 0;
    int n_ok = 0;
    if (!out || out_sz == 0) return 0;
    out[0] = 0;

/* ★ 安全追加：snprintf 返回的是「假如空间够大会写的长度」而不是实际写入长度，
 *   若直接 `w += _n`，缓冲不足时 w 会越过 out_sz ⇒ 后续 out+w 变野指针 ⇒
 *   崩溃或只剩第一行（踩过：自检报告只显示了「sdmc 挂载」一行）。
 *   这里改为：只在确实写入成功且没截断时才推进 w，并始终保证 NUL 结尾。 */
#define APP(...) do {                                                       \
        if (w < out_sz) {                                                    \
            int _n = snprintf(out + w, out_sz - w, __VA_ARGS__);             \
            if (_n > 0) {                                                    \
                if ((size_t)_n >= out_sz - w) { w = out_sz - 1; }            \
                else                          { w += (size_t)_n; }           \
                out[w] = 0;                                                  \
            }                                                                \
        }                                                                    \
    } while (0)

    APP("sdmc 挂载: ");
    {
        Result rc = fsdevMountSdmc();
        APP("rc=0x%X%s\n", rc, R_SUCCEEDED(rc) ? " (成功)" : " (可能已挂载, 不影响)");
    }

    APP("\n目录探测:\n");
    const char *dirs[] = {
        "sdmc:/atmosphere",
        "sdmc:/atmosphere/contents",
        "sdmc:/atmosphere/contents/0100b00b51230000",
        "sdmc:/atmosphere/contents/0100b00b51230000/romfs",
        "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update",
    };
    for (int i = 0; i < 5; i++) {
        /* 用 opendir 判目录是否存在 */
        DIR *d = opendir(dirs[i]);
        if (d) { APP("  [有] %s\n", dirs[i]); closedir(d); }
        else   { APP("  [无] %s\n", dirs[i]); }
    }

    APP("\n候选文件:\n");
    for (int i = 0; i < GC_NUM_CANDIDATES; i++) {
        long sz = probe_file(gc_rpf_candidates[i]);
        if (sz >= 0) {
            /* ★ 同时报可写性：写入失败 (rc=-8/-9) 十有八九栽在这里 */
            FILE *wf = fopen(gc_rpf_candidates[i], "r+b");
            const char *wr = wf ? "可写" : "★ 只读";
            if (wf) fclose(wf);
            n_ok++;
            APP("  [%ld B] [%s] %s\n", sz, wr, gc_rpf_candidates[i]);
        } else {
            APP("  [打不开] %s\n", gc_rpf_candidates[i]);
        }
    }

    /* ★ 对第一个能打开的候选做「实际解析」——这是最有价值的诊断 */
    APP("\n实际解析 (第一个能打开的文件):\n");
    for (int i = 0; i < GC_NUM_CANDIDATES; i++) {
        long sz = probe_file(gc_rpf_candidates[i]);
        if (sz < 0) continue;

        FILE *f = fopen(gc_rpf_candidates[i], "rb");
        if (!f) { APP("  fopen 失败 (但探测到 %ld B!)\n", sz); break; }

        u8 hdr[16] = {0};
        size_t got = fread(hdr, 1, 16, f);
        fclose(f);
        if (got != 16) { APP("  只读到 %zu 字节 header\n", got); break; }

        u32 magic = rd_u32(hdr);
        u32 ec    = rd_u32(hdr + 4);
        u32 nl    = rd_u32(hdr + 8) & 0x0FFFFFFFu;
        u32 enc   = rd_u32(hdr + 12);
        APP("  header: %02X%02X%02X%02X %02X%02X%02X%02X ...\n",
            hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],hdr[5],hdr[6],hdr[7]);
        APP("  magic=0x%08X %s\n", magic,
            magic == GC_RPF_MAGIC ? "(OK)" : "★ 不是 RPF7!");
        APP("  条目数=%u  名字池=%u B  加密=0x%08X\n", ec, nl, enc);

        if (magic == GC_RPF_MAGIC && ec > 0 && ec < 200000 && nl > 0) {
            /* 试着完整打开 */
            GcRpf tmp;
            int rc = gc_rpf_open(gc_rpf_candidates[i], &tmp);
            APP("  gc_rpf_open: rc=%d %s\n", rc, rc == GC_OK ? "(成功)" : gc_rpf_last_error());
            if (rc == GC_OK) {
                GcEntry e;
                int rc2 = gc_rpf_find(&tmp, "gameconfig.xml", &e);
                if (rc2 == GC_OK) {
                    APP("  找到 gameconfig.xml: idx=%d on_disk=%u size=%u off=0x%X\n",
                        e.idx, e.on_disk, e.size, e.offset);
                    u8 *buf = NULL; size_t bl = 0;
                    int rc3 = gc_rpf_read(&tmp, &e, &buf, &bl);
                    APP("  解压: rc=%d, %zu B\n", rc3, bl);
                    if (rc3 == GC_OK && bl > 0) {
                        char h2[49] = {0};
                        for (int k = 0; k < 48 && k < (int)bl; k++) {
                            unsigned char c = buf[k];
                            h2[k] = (c >= 32 && c < 127) ? (char)c : '.';
                        }
                        APP("  内容开头: %s\n", h2);
                        size_t sa = 0, sb = 0;
                        int rc4 = gc_xml_find_span((char*)buf, bl, "switch", &sa, &sb);
                        APP("  找 switch 段: rc=%d %s\n", rc4,
                            rc4 == GC_OK ? "(成功)" : "★ 失败!");
                        if (rc4 != GC_OK) {
                            size_t aa = 0, ab = 0;
                            int rc5 = gc_xml_find_span((char*)buf, bl, "Any", &aa, &ab);
                            APP("  (对照) 找 Any 段: rc=%d\n", rc5);
                        }
                        free(buf);
                    } else if (rc3 == GC_OK) {
                        free(buf);
                    }
                } else {
                    APP("  ★ 找不到 gameconfig.xml (rc=%d, 共 %u 条目)\n", rc2, tmp.entry_count);
                    /* ★★★ 关键诊断：把条目名打出来，看名字池读得对不对 */
                    APP("  name_shift=%u names_off=%u names_len=%u enc=0x%08X\n",
                        tmp.name_shift, 0x10 + tmp.entry_count * 16, tmp.names_len, tmp.encryption);

                    APP("  --- 前 16 个条目 ---\n");
                    int shown = 0;
                    for (u32 k = 0; k < tmp.entry_count && shown < 16; k++) {
                        GcEntry t2;
                        if (gc_rpf_entry(&tmp, (int)k, &t2) != GC_OK) continue;
                        APP("   [%u] name_off=%u name='%s' %s disk=%u size=%u off=0x%X\n",
                            k, t2.name_off, t2.name,
                            t2.is_dir ? "DIR" : (t2.is_res ? "RES" : "BIN"),
                            t2.on_disk, t2.size, t2.offset);
                        shown++;
                    }

                    APP("  --- 名字含 config 的条目 ---\n");
                    int nc = 0;
                    for (u32 k = 0; k < tmp.entry_count; k++) {
                        GcEntry t2;
                        if (gc_rpf_entry(&tmp, (int)k, &t2) != GC_OK) continue;
                        if (t2.name[0] == 0) continue;
                        if (strstr(t2.name, "config") || strstr(t2.name, "Config") ||
                            strstr(t2.name, "CONFIG")) {
                            APP("   [%u] '%s' %s disk=%u size=%u\n", k, t2.name,
                                t2.is_dir ? "DIR" : "BIN", t2.on_disk, t2.size);
                            if (++nc >= 10) break;
                        }
                    }
                    if (nc == 0) APP("   (没有含 config 的条目!)\n");

                    APP("  --- 名字以 .xml 结尾的条目 ---\n");
                    int nx = 0;
                    for (u32 k = 0; k < tmp.entry_count; k++) {
                        GcEntry t2;
                        if (gc_rpf_entry(&tmp, (int)k, &t2) != GC_OK) continue;
                        size_t L = strlen(t2.name);
                        if (L > 4 && strcasecmp(t2.name + L - 4, ".xml") == 0) {
                            APP("   [%u] '%s'\n", k, t2.name);
                            if (++nx >= 10) break;
                        }
                    }
                    if (nx == 0) APP("   (没有 .xml 条目!)\n");

                    /* 统计：空名字 / 非 ASCII 名字 */
                    int n_empty = 0, n_nonascii = 0;
                    for (u32 k = 0; k < tmp.entry_count; k++) {
                        GcEntry t2;
                        if (gc_rpf_entry(&tmp, (int)k, &t2) != GC_OK) continue;
                        if (t2.name[0] == 0) { n_empty++; continue; }
                        for (const char *q = t2.name; *q; q++) {
                            if ((unsigned char)*q >= 0x80) { n_nonascii++; break; }
                        }
                    }
                    APP("  统计: 空名字 %d 个, 含非ASCII %d 个\n", n_empty, n_nonascii);
                }
                gc_rpf_close(&tmp);
            }
        } else {
            APP("  ★ header 不合理, 无法继续\n");
        }
        break;   /* 只诊断第一个能打开的 */
    }

    /* 顺带看看 /switch 能不能访问（对照，说明 sdmc 本身是通的） */
    APP("\n对照 (sdmc 是否可用):\n");
    {
        DIR *d = opendir("sdmc:/switch");
        APP("  sdmc:/switch  %s\n", d ? "[有]" : "[无]");
        if (d) closedir(d);
    }
#undef APP
    return n_ok;
}

/* ---------------------------------------------------------------- 解析条目 */
int gc_rpf_entry(const GcRpf *r, int i, GcEntry *e) {
    if (i < 0 || (u32)i >= r->entry_count) return GC_ERR_FORMAT;
    const u8 *p = r->toc + (size_t)i * 16;
    u64 q0 = rd_u64(p);

    memset(e, 0, sizeof(*e));
    e->idx      = i;
    e->name_off = (u32)(q0 & 0xFFFF);
    e->on_disk  = (u32)((q0 >> 16) & 0xFFFFFF);
    e->offset   = (u32)((q0 >> 40) & 0x7FFFFF);
    e->is_res   = (int)((q0 >> 63) & 1);
    e->size     = rd_u32(p + 8);
    e->first    = rd_u32(p + 8);
    e->count    = rd_u32(p + 12);
    e->is_dir   = (!e->is_res) && (e->offset == 0x7FFFFF);

    /* 名字 */
    if (i == 0 && e->is_dir) {
        e->name[0] = 0;
    } else {
        u32 o = e->name_off << r->name_shift;
        if (o < r->names_len) {
            const u8 *s = r->names + o;
            size_t maxn = r->names_len - o;
            size_t n = 0;
            while (n < maxn && s[n] != 0 && n < sizeof(e->name) - 1) { e->name[n] = (char)s[n]; n++; }
            e->name[n] = 0;
        }
    }
    return GC_OK;
}

/* 按文件名找（不含目录，遍历全部条目）
 * ★ 额外宽松匹配：有些版本的条目名可能带路径或大小写不同 */
int gc_rpf_find(GcRpf *r, const char *fname, GcEntry *out) {
    /* 第一轮：精确（忽略大小写）匹配纯文件名 */
    for (u32 i = 0; i < r->entry_count; i++) {
        GcEntry e;
        if (gc_rpf_entry(r, (int)i, &e) != GC_OK) continue;
        if (e.is_dir || e.is_res) continue;
        if (strcasecmp(e.name, fname) == 0) { *out = e; return GC_OK; }
    }
    /* 第二轮：名字里包含关键片段（排除目录项） */
    for (u32 i = 0; i < r->entry_count; i++) {
        GcEntry e;
        if (gc_rpf_entry(r, (int)i, &e) != GC_OK) continue;
        if (e.is_dir || e.is_res) continue;
        if (e.name[0] == 0) continue;
        if (strstr(e.name, fname) != NULL) { *out = e; return GC_OK; }
    }
    return GC_ERR_NOTFOUND;
}

/* ---------------------------------------------------------------- 读内容 */
/* ★ 健壮版解压：不依赖 e->size 的准确性。
 *   有些版本的 RPF 里 size 字段可能不准（或为 0），
 *   所以用「逐步扩大缓冲」的方式，直到 inflate 成功。 */
int gc_rpf_read(GcRpf *r, const GcEntry *e, u8 **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    u32 n = e->on_disk ? e->on_disk : e->size;
    if (n == 0) { set_err("条目长度为 0"); return GC_ERR_FORMAT; }
    if (n > 64u * 1024 * 1024) { set_err("条目过大 (%u B)", n); return GC_ERR_FORMAT; }

    u8 *raw = (u8*)malloc(n);
    if (!raw) { set_err("内存不足 (读 %u B)", n); return GC_ERR_NOMEM; }
    if (gcs_seek(r->fp, (long long)e->offset * 512) != 0) {
        set_err("seek 失败 (offset=0x%X)", e->offset);
        free(raw); return GC_ERR_IO;
    }
    size_t got = fread(raw, 1, n, r->fp);
    if (got != n) {
        set_err("只读到 %zu / %u 字节 (offset=0x%X)", got, n, e->offset);
        free(raw); return GC_ERR_IO;
    }

    /* 未压缩（on_disk==0 或 on_disk==size） */
    if (e->on_disk == 0 || e->on_disk == e->size) {
        *out = raw; *out_len = n;
        return GC_OK;
    }

    /* 压缩：逐步扩大缓冲直到成功
     * 起点 = max(size, n*8)，上限 64 MB */
    size_t cap = (e->size > n * 8) ? (size_t)e->size : (size_t)n * 8;
    if (cap < 4096) cap = 4096;

    for (int attempt = 0; attempt < 6; attempt++) {
        u8 *buf = (u8*)malloc(cap);
        if (!buf) { free(raw); set_err("内存不足 (解压缓冲 %zu B)", cap); return GC_ERR_NOMEM; }

        /* 先试 zlib 头（wbits=15） */
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        int ok = 0;
        if (inflateInit2(&zs, 15) == Z_OK) {
            zs.next_in = raw;  zs.avail_in  = n;
            zs.next_out = buf; zs.avail_out = (uInt)cap;
            int rc = inflate(&zs, Z_FINISH);
            if ((rc == Z_STREAM_END || rc == Z_OK) && zs.total_out > 0) {
                *out_len = zs.total_out; ok = 1;
            }
            inflateEnd(&zs);
        }
        /* 再试 raw deflate（wbits=-15） */
        if (!ok) {
            memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -15) == Z_OK) {
                zs.next_in = raw;  zs.avail_in  = n;
                zs.next_out = buf; zs.avail_out = (uInt)cap;
                int rc = inflate(&zs, Z_FINISH);
                if ((rc == Z_STREAM_END || rc == Z_OK) && zs.total_out > 0) {
                    *out_len = zs.total_out; ok = 1;
                }
                inflateEnd(&zs);
            }
        }
        if (ok) {
            free(raw);
            *out = buf;
            return GC_OK;
        }
        /* 失败：可能是缓冲不够，扩大一倍重试 */
        free(buf);
        if (cap >= 64u * 1024 * 1024) break;
        cap *= 2;
    }

    free(raw);
    set_err("解压失败 (on_disk=%u size=%u, 试到缓冲 %zu B)", e->on_disk, e->size, cap);
    return GC_ERR_INFLATE;
}

/* ★ 文件是否可写（"r+b" 打开成功才为 1） */
int gc_rpf_writable(const GcRpf *r) { return r ? r->writable : 0; }

/* 该条目到下一个条目起始的物理空隙（用于判断能否抬高 on_disk） */
int gc_rpf_gap(const GcRpf *r, const GcEntry *e) {
    u64 my_end = (u64)e->offset * 512 + (e->on_disk ? e->on_disk : e->size);
    u64 best = (u64)-1;
    for (u32 i = 0; i < r->entry_count; i++) {
        GcEntry o;
        if (gc_rpf_entry(r, (int)i, &o) != GC_OK) continue;
        if (o.is_dir || o.on_disk == 0) continue;
        u64 s = (u64)o.offset * 512;
        if (s >= my_end && s < best) best = s;
    }
    if (best == (u64)-1) return -1;
    u64 gap = best - my_end;
    return (gap > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)gap;
}

/* ★★★ 该条目真正能用的【物理可用空间】（字节）
 *
 * 🚨🚨 这是修「超出容量 6492」的关键函数。
 *
 * 原来的容量判据用的是 `e->on_disk`（当前值），但 on_disk 是【上次写入的结果】：
 *   写一次「极限」→ on_disk 从 7837 变成 7758
 *   再写一次 → 容量只剩 7758
 *   反复几次 → 容量越来越小 → 换个大点的预设就报「超出容量」
 * ⇒ 这是「自己把自己锁死」，跟预设本身毫无关系。
 *
 * 正确判据 = 从本条目起始到【下一个条目的起始】之间的全部空间：
 *   · 实测 gameconfig 后面有 93.5 MB 空隙（数据结束 0x24B0CE9D，下一条目 0x2A88B000）
 *   · on_disk 字段是 24 位 ⇒ 硬上限 16,777,215 B
 *   · 压缩后只有 ~7.5 KB ⇒ 余量约 2000 倍
 *
 * 返回: 可用字节数（已按 24 位上限截断）；出错返回 -1。
 */
int gc_rpf_room(const GcRpf *r, const GcEntry *e) {
    u64 start = (u64)e->offset * 512;
    if (start == 0) return -1;

    /* 找下一个物理起始位置（含未压缩条目，不含目录） */
    u64 best = (u64)-1;
    for (u32 i = 0; i < r->entry_count; i++) {
        GcEntry o;
        if (gc_rpf_entry(r, (int)i, &o) != GC_OK) continue;
        if (o.is_dir) continue;
        if (o.on_disk == 0 && o.size == 0) continue;   /* 空条目跳过 */
        u64 s = (u64)o.offset * 512;
        if (s > start && s < best) best = s;
    }

    u64 avail;
    if (best == (u64)-1) {
        /* 没有后继条目：用文件大小兜底 */
        if (r->file_size > start) avail = r->file_size - start;
        else                      return -1;
    } else {
        avail = best - start;
    }

    /* 不能超过 on_disk 字段的 24 位上限 */
    if (avail > GC_ON_DISK_MAX) avail = GC_ON_DISK_MAX;
    return (int)avail;
}

/* ---------------------------------------------------------------- 写内容 */
/* ---------------------------------------------------------------- 写 TOC */
/* 更新某条目的 offset / on_disk / size 三个字段（offset 是 23 位，单位扇区） */
static int gc_patch_toc(GcRpf *r, int idx, u32 offset, u32 on_disk, u32 size) {
    long ent_off = 0x10 + (long)idx * 16;
    u8 t[16];
    if (gcs_seek(r->fp, ent_off) != 0) return GC_ERR_IO;
    if (fread(t, 1, 16, r->fp) != 16) return GC_ERR_IO;

    u64 q0 = rd_u64(t);
    q0 = (q0 & ~(((u64)0xFFFFFF) << 16)) | ((u64)(on_disk & 0xFFFFFF) << 16);
    q0 = (q0 & ~(((u64)0x7FFFFF) << 40)) | ((u64)(offset & 0x7FFFFF) << 40);
    for (int i = 0; i < 8; i++) t[i] = (u8)((q0 >> (8 * i)) & 0xFF);
    wr_u32(t + 8, size);

    if (gcs_seek(r->fp, ent_off) != 0) return GC_ERR_IO;
    if (fwrite(t, 1, 16, r->fp) != 16) return GC_ERR_IO;
    fflush(r->fp);
    fsdevCommitDevice("sdmc");
    memcpy(r->toc + (size_t)idx * 16, t, 16);
    return GC_OK;
}

/* ------------------------------------------------------------ raw deflate */
/* RPF7 要求 raw deflate（wbits=-15）。用 zlib 头（78 da）会让游戏静默读不到。 */
static int gc_deflate_raw(const u8 *data, size_t len, u8 **out, size_t *outlen) {
    uLong bound = compressBound((uLong)len);
    u8 *blob = (u8*)malloc(bound);
    if (!blob) return GC_ERR_NOMEM;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(blob); return GC_ERR_DEFLATE;
    }
    zs.next_in   = (Bytef*)data;
    zs.avail_in  = (uInt)len;
    zs.next_out  = blob;
    zs.avail_out = (uInt)bound;
    int rc = deflate(&zs, Z_FINISH);
    uLong clen = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) { free(blob); return GC_ERR_DEFLATE; }
    *out = blob; *outlen = (size_t)clen;
    return GC_OK;
}

/* -------------------------------------------------- ★ 找空闲区（搬家用）
 *
 * 思路：把所有非目录条目按物理起始排序，相邻两条之间若有空档且全为 0x00，
 *       就是可用空闲区。返回第一个够大的空档的扇区号。
 *
 * 为什么必须校验「全零」：有些空档是上一个条目压缩数据的残留（未补零），
 * 直接覆盖虽然一般也无害，但全零校验能顺带避开「其实是别人数据」的误判。
 *
 * 🚨🚨🚨 v26 修复（2026-09-29 事故）：dlclist 搬家到扇区 0x17F2，砸了
 *   mpapartment/content.xml 的数据（扇区 0x17F0 起 1316B，跨 3 个扇区）。
 *   根因三连：
 *   ① 区间长度没按扇区向上取整 —— 1316B 只算 1316B，尾部 220B 缝隙被
 *      当成「空档」的一部分；
 *   ② 全零校验只查前 4KB —— 校验范围 ≠ 写入范围，写入区间可能横跨
 *      下一个条目的数据头；
 *   ③ 没有写入前的重叠二次校验。
 *   修法：① 区间按扇区取整 ② 全零校验覆盖整个写入区间 ③ write_ex
 *   搬家分支写之前再扫一遍 TOC 确认无重叠（gc_rpf_check_free）。
 */
u32 gc_rpf_find_free(GcRpf *r, u32 need) {
    if (need == 0) return 0;
    need = (need + 511u) & ~511u;                 /* 向上对齐到扇区 */

    /* 收集所有非目录条目的物理区间（★ 长度按扇区向上取整，v26 修复 ①） */
    typedef struct { u64 s, e; } Iv;
    Iv iv[1200];
    int n = 0;
    for (u32 i = 0; i < r->entry_count && n < 1200; i++) {
        GcEntry o;
        if (gc_rpf_entry(r, (int)i, &o) != GC_OK) continue;
        if (o.is_dir) continue;
        u64 ln = o.on_disk ? o.on_disk : o.size;
        if (ln == 0) continue;
        ln = (ln + 511u) & ~511u;                 /* ★ 占用按整扇区算 */
        iv[n].s = (u64)o.offset * 512;
        iv[n].e = iv[n].s + ln;
        n++;
    }
    /* 按起点排序 */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (iv[j].s < iv[i].s) { Iv t = iv[i]; iv[i] = iv[j]; iv[j] = t; }

    /* 扫描空档 */
    u64 cur = 0;
    u8 *probe = NULL;
    u32 probe_cap = 0;
    for (int i = 0; i <= n; i++) {
        u64 nxt = (i < n) ? iv[i].s : (u64)r->file_size;
        if (nxt > cur + need) {
            /* 候选空档 [cur, cur+need)。★ v26 修复 ②：全零校验覆盖
             * 整个写入区间（分块扫，不只前 4KB） */
            u64 chk_total = need;
            if (chk_total > nxt - cur) chk_total = nxt - cur;
            int zero = 1;
            u64 off = 0;
            while (off < chk_total && zero) {
                u32 chk = (u32)((chk_total - off < 4096) ? (chk_total - off) : 4096);
                if (probe_cap < chk) {
                    if (probe) free(probe);
                    probe_cap = chk;
                    probe = (u8*)malloc(probe_cap);
                }
                if (!probe) { zero = 0; break; }
                if (gcs_seek(r->fp, (long long)(cur + off)) != 0 ||
                    fread(probe, 1, chk, r->fp) != chk) {
                    zero = 0; break;
                }
                for (u32 k = 0; k < chk; k++) if (probe[k]) { zero = 0; break; }
                off += chk;
            }
            if (zero) {
                if (probe) free(probe);
                return (u32)(cur / 512);
            }
        }
        if (i < n && iv[i].e > cur) cur = iv[i].e;
    }
    if (probe) free(probe);
    return 0;
}

/* ★ v26 修复 ③：写入前重叠二次校验
 * 检查字节区间 [start, start+len) 是否与任何非目录条目的占用区间重叠。
 * 返回 1 = 空闲安全；0 = 有重叠（禁止写入）。 */
int gc_rpf_check_free(GcRpf *r, u32 sec, u32 need) {
    u64 start = (u64)sec * 512;
    u64 end   = start + ((need + 511u) & ~511u);
    for (u32 i = 0; i < r->entry_count; i++) {
        GcEntry o;
        if (gc_rpf_entry(r, (int)i, &o) != GC_OK) continue;
        if (o.is_dir) continue;
        u64 ln = o.on_disk ? o.on_disk : o.size;
        if (ln == 0) continue;
        u64 s = (u64)o.offset * 512;
        u64 e = s + ((ln + 511u) & ~511u);
        if (start < e && s < end) return 0;      /* 区间相交 */
    }
    return 1;
}

/* ------------------------------------------------------------ 写入（核心） */
/* 真正的落盘：把 blob 写到 sec*512，并在 [clen, clean_to) 补零，然后 patch TOC */
static int gc_write_at(GcRpf *r, int idx, u32 sec, const u8 *blob, u32 clen,
                       u32 clean_to, u32 usize) {
    if (gcs_seek(r->fp, (long long)sec * 512) != 0) {
        set_err("seek 到扇区 0x%X 失败", sec);
        return GC_ERR_IO;
    }
    errno = 0;
    if (fwrite(blob, 1, clen, r->fp) != clen) {
        set_err("写压缩块失败 (want=%u, errno=%d)", clen, errno);
        return GC_ERR_IO;
    }
    if (clean_to > clen) {
        u8 zero[4096];
        memset(zero, 0, sizeof(zero));
        u32 left = clean_to - clen;
        while (left > 0) {
            size_t w = left > sizeof(zero) ? sizeof(zero) : left;
            errno = 0;
            if (fwrite(zero, 1, w, r->fp) != w) {
                set_err("补零失败 (left=%u, errno=%d)", left, errno);
                return GC_ERR_IO;
            }
            left -= (u32)w;
        }
    }
    int rc = gc_patch_toc(r, idx, sec, clen, usize);
    if (rc != GC_OK) set_err("patch TOC 失败 (rc=%d)", rc);
    return rc;
}

/* ★★ 带溢出重定位的写入 */
int gc_rpf_write_ex(GcRpf *r, GcEntry *e, const u8 *data, size_t len,
                    int allow_relocate) {
    if (!r->writable) {
        set_err("文件是只读打开的 (SD 卡写保护 / FAT 只读属性?)");
        return GC_ERR_READONLY;
    }
    if (len > GC_ON_DISK_MAX) {
        set_err("内容 %zu B 超过 on_disk 的 16 MB 上限", len);
        return GC_ERR_TOOBIG;
    }

    u8 *blob = NULL; size_t clen = 0;
    int rc = gc_deflate_raw(data, len, &blob, &clen);
    if (rc != GC_OK) { set_err("deflate 失败 (rc=%d)", rc); return rc; }

    /* 物理可用空间（本条目起始 → 下一条目起始，按 24 位截断） */
    int room_i = gc_rpf_room(r, e);
    u32 room = (room_i > 0) ? (u32)room_i
                            : (u32)(e->on_disk ? e->on_disk : e->size);

    u32 sec = e->offset;
    u32 clean_to = (u32)clen;
    if (e->on_disk > clen) {
        u32 lim = (u32)clen + 1024u * 1024u;
        clean_to = (e->on_disk < lim) ? e->on_disk : lim;
    }

    if (clen > room) {
        /* ---------- 原位装不下：尝试搬家 ---------- */
        if (!allow_relocate) {
            set_err("压缩后 %zu B 超出物理可用空间 %u B（不允许搬家）", clen, room);
            free(blob);
            return GC_ERR_TOOBIG;
        }
        u32 nsec = gc_rpf_find_free(r, (u32)clen);
        if (nsec == 0) {
            set_err("原位余量 %u B 不够（需 %zu B），且文件里找不到空闲区",
                    room, clen);
            free(blob);
            return GC_ERR_TOOBIG;
        }
        /* ★ v26 修复 ③：搬家写入前的重叠二次校验。
         * find_free 的空档判断理论上已排除占用区，但 TOC 扫描与全零探针
         * 各自独立，任何一处偏差都会砸到别人数据（2026-09-29 事故：
         * dlclist 搬到 0x17F2 砸了 mpapartment/content.xml）。
         * 这里再扫一遍 TOC 做区间相交判定，双保险。 */
        if (!gc_rpf_check_free(r, nsec, (u32)clen)) {
            set_err("空闲区校验失败：扇区 0x%X 与已有条目重叠，拒绝写入（防覆盖）",
                    nsec);
            free(blob);
            return GC_ERR_TOOBIG;
        }
        sec = nsec;
        clean_to = (u32)clen;      /* 新位置只写实际长度，不补零（后面本来就是空闲区） */
    }

    rc = gc_write_at(r, e->idx, sec, blob, (u32)clen, clean_to, (u32)len);
    free(blob);
    if (rc != GC_OK) return rc;

    /* 回写结构体（搬家后 offset 变了，调用方要拿新值做校验/后续操作） */
    e->offset  = sec;
    e->on_disk = (u32)clen;
    e->size    = (u32)len;
    return GC_OK;
}

int gc_rpf_write(GcRpf *r, const GcEntry *e, const u8 *data, size_t len) {
    GcEntry tmp = *e;                 /* write_ex 会改 offset/on_disk/size */
    return gc_rpf_write_ex(r, &tmp, data, len, 0);
}


/* 写入后校验：重新读回并逐字节比对 */
int gc_rpf_verify(GcRpf *r, const GcEntry *e, const u8 *expect, size_t len) {
    GcEntry e2;
    if (gc_rpf_entry(r, e->idx, &e2) != GC_OK) return GC_ERR_FORMAT;
    u8 *back = NULL; size_t bl = 0;
    int rc = gc_rpf_read(r, &e2, &back, &bl);
    if (rc != GC_OK) return rc;
    int same = (bl == len) && (memcmp(back, expect, len) == 0);
    free(back);
    return same ? GC_OK : GC_ERR_IO;
}

/* ========================================================================= */
/* XML 工具                                                                   */
/* ========================================================================= */

/* 原地把 <!-- ... --> 替换成空格（保持长度不变，偏移不漂移） */
size_t gc_strip_comments(char *s, size_t len) {
    size_t i = 0;
    while (i + 3 < len) {
        if (s[i] == '<' && s[i+1] == '!' && s[i+2] == '-' && s[i+3] == '-') {
            size_t j = i + 4;
            while (j + 2 < len && !(s[j] == '-' && s[j+1] == '-' && s[j+2] == '>')) j++;
            if (j + 2 >= len) break;
            size_t end = j + 3;
            for (size_t k = i; k < end; k++) {
                if (s[k] != '\n' && s[k] != '\r') s[k] = ' ';
            }
            i = end;
        } else {
            i++;
        }
    }
    return len;   /* 长度不变 */
}

/* 真正删除注释（长度会变短）—— 用于瘦身腾空间
 * ⚠️ 会改变长度，所以必须在【所有 XML 编辑完成之后】才调用 */
size_t gc_drop_comments(char *s, size_t len) {
    size_t w = 0, i = 0;
    while (i < len) {
        if (i + 3 < len && s[i]=='<' && s[i+1]=='!' && s[i+2]=='-' && s[i+3]=='-') {
            size_t j = i + 4;
            while (j + 2 < len && !(s[j]=='-' && s[j+1]=='-' && s[j+2]=='>')) j++;
            if (j + 2 >= len) break;      /* 没找到结尾，停止 */
            i = j + 3;
        } else {
            s[w++] = s[i++];
        }
    }
    s[w] = 0;
    return w;
}

/* 再把「>   <」之间的空白压掉（更激进，能多省 ~950 B）
 * 只压纯空白，不碰属性内部；同样必须在编辑完成后调用 */
size_t gc_squeeze(char *s, size_t len) {
    size_t w = 0, i = 0;
    while (i < len) {
        if (s[i] == '>') {
            s[w++] = '>';
            i++;
            /* 跳过后续纯空白，但保留第一个换行（可读性） */
            size_t j = i;
            int nl = 0;
            while (j < len && (s[j]==' ' || s[j]=='\t' || s[j]=='\r' || s[j]=='\n')) {
                if (s[j] == '\n') nl = 1;
                j++;
            }
            if (j < len && s[j] == '<') {
                /* 全压掉（XML 不需要标签间空白） */
                i = j;
            } else {
                /* 不是标签间隔，原样保留 */
                while (i < j) s[w++] = s[i++];
                (void)nl;
            }
        } else {
            s[w++] = s[i++];
        }
    }
    s[w] = 0;
    return w;
}

/* 找 <Platforms>xxx</Platforms> 对应的 <Config ...> ... </Config> 区间
 * 用「深度计数」：从 <Platforms> 起，遇到 <Config type= 深度+1，</Config> 深度-1 */
int gc_xml_find_span(const char *xml, size_t len, const char *plat,
                     size_t *start, size_t *end) {
    char needle[64];
    snprintf(needle, sizeof(needle), "<Platforms>%s</Platforms>", plat);
    size_t nl = strlen(needle);

    size_t pos = 0, s = 0;
    int found = 0;
    while (pos + nl <= len) {
        if (memcmp(xml + pos, needle, nl) == 0) { s = pos; found = 1; break; }
        pos++;
    }
    if (!found) return GC_ERR_NOTFOUND;

    int depth = 0;
    size_t i = s;
    while (i + 8 <= len) {
        if (memcmp(xml + i, "<Config type=", 13) == 0) depth++;
        else if (memcmp(xml + i, "</Config>", 9) == 0) {
            depth--;
            if (depth == 0) { *start = s; *end = i + 9; return GC_OK; }
        }
        i++;
    }
    return GC_ERR_FORMAT;
}

/* 前置声明（定义在本文件下方） */
static size_t gc_ensure_container(char *xml, size_t cap, size_t *len,
                                  size_t span_a, size_t span_b,
                                  const char *path, int create_missing);

/* 在 [span_a, span_b) 内把 <key value="X" /> 改成 <key value="V" />。
 * 若不存在则插到【该 key 对应的容器】内（不是 </Config> 前！）。
 *
 * ★ 重要：插入会让段尾右移 ⇒ 调用方【必须】在每次插入后重定位区间，
 *   否则第二次插入会因 span_b 过期而 rfind 失败（踩过，6 套预设都只增了 1 项）。
 *   为降低调用方负担，这里提供 gc_xml_set_int_ex()：传入段名，内部自动重定位。
 *
 * inserted: 出参，1 = 新插入，0 = 替换 */
int gc_xml_set_int(char *xml, size_t cap, size_t *len,
                   size_t span_a, size_t span_b,
                   const char *key, int value, int *inserted) {
    if (inserted) *inserted = 0;
    size_t klen = strlen(key);

    /* 在段内搜 <key value=" */
    for (size_t i = span_a; i + klen + 10 < span_b; i++) {
        if (xml[i] != '<') continue;
        if (memcmp(xml + i + 1, key, klen) != 0) continue;
        /* 后面必须紧跟 ' ' 或 '>'，避免前缀误匹配 */
        char nxt = xml[i + 1 + klen];
        if (nxt != ' ' && nxt != '>') continue;
        /* 找 value=" */
        size_t j = i + 1 + klen;
        while (j + 8 < span_b && memcmp(xml + j, "value=\"", 7) != 0) {
            if (xml[j] == '>') break;
            j++;
        }
        if (j + 8 >= span_b || memcmp(xml + j, "value=\"", 7) != 0) continue;
        size_t v0 = j + 7;
        size_t v1 = v0;
        while (v1 < span_b && xml[v1] != '"') v1++;
        if (v1 >= span_b) continue;

        /* 替换数字 */
        char num[24];
        int nl = snprintf(num, sizeof(num), "%d", value);
        size_t oldl = v1 - v0;
        if ((size_t)nl == oldl) {
            memcpy(xml + v0, num, nl);
        } else if ((size_t)nl < oldl) {
            /* 变短：写数字 + 空格补齐（保持长度，偏移不漂移） */
            memcpy(xml + v0, num, nl);
            memset(xml + v0 + nl, ' ', oldl - nl);
        } else {
            /* 变长：需要 memmove 腾空间 */
            size_t need = nl - oldl;
            if (*len + need + 1 > cap) return GC_ERR_TOOBIG;
            memmove(xml + v1 + need, xml + v1, *len - v1 + 1);
            memcpy(xml + v0, num, nl);
            *len += need;
        }
        return GC_OK;
    }

    /* 不存在 -> 插到【正确的容器】内
     *
     * 🚨🚨 关键：绝不能一律插到 </Config> 前（= CGameConfig 直接子级）！
     *   那样会绕过 ConfigPopulation / ConfigModelInfo / ConfigExtensions 这些容器，
     *   游戏在对应容器里找不到该项 ⇒ 继续用 Any 段的值
     *   ⇒ 表现为「预设套了但没效果」（手动改已存在的项却有效）。
     *
     *   正确做法：查 GC_KEY_PATHS 拿到该 key 的容器路径，插到那个容器的 </Name> 前。 */
    size_t ins = (size_t)-1;
    const char *cpath = gc_key_container(key);

    if (cpath) {
        /* ★ 按容器路径插入；缺失的中间容器（如 switch 段没有 VehicleSpacing）就地创建。
         *   ⚠️ 这一步可能修改 xml（新建容器），所以要在 memmove 插入项之前完成。 */
        ins = gc_ensure_container(xml, cap, len, span_a, span_b, cpath, 1);
        if (ins != (size_t)-1) {
            /* 容器新建后，span_b 需要重新定位（段尾右移了） */
            /* 交给调用方 gc_xml_set_int_ex() 处理重定位即可 */
        }
    }
    if (ins == (size_t)-1) {
        /* 兜底：容器找不到且建不了时，退回到 </Config> 前（至少语法合法） */
        for (size_t i = *len > 9 ? *len - 9 : 0; i > span_a + 9; i--) {
            if (memcmp(xml + i, "</Config>", 9) == 0) { ins = i; break; }
        }
    }
    if (ins == (size_t)-1) return GC_ERR_FORMAT;

    /* 缩进：容器内的项用 20 空格（与 switch 段现有项一致），兜底用 6 空格 */
    char frag[256];
    int fl;
    if (cpath) {
        fl = snprintf(frag, sizeof(frag), "\n                    <%s value=\"%d\"/>", key, value);
    } else {
        fl = snprintf(frag, sizeof(frag), "\n      <%s value=\"%d\" />", key, value);
    }
    if (fl <= 0 || (size_t)fl >= sizeof(frag)) return GC_ERR_FORMAT;
    if (*len + (size_t)fl + 1 > cap) return GC_ERR_TOOBIG;
    memmove(xml + ins + fl, xml + ins, *len - ins + 1);
    memcpy(xml + ins, frag, fl);
    *len += fl;
    if (inserted) *inserted = 1;
    return GC_OK;
}


/* 查 key 对应的容器路径（找不到返回 NULL） */
const char *gc_key_container(const char *key) {
    for (int i = 0; i < GC_NUM_KEY_PATHS; i++) {
        if (strcmp(GC_KEY_PATHS[i].key, key) == 0) return GC_KEY_PATHS[i].path;
    }
    return NULL;
}

/* ★★ 在 [span_a, span_b) 里找容器 path 的 </Name> 位置；缺失的中间容器就地创建。
 *
 *   返回：插入点偏移（= 目标容器的 </Name> 之前）；
 *         -1 表示失败。
 *
 *   create_missing=1 时，若某级容器不存在，则在【上一级容器的末尾】新建它：
 *       <Parent>
 *         ...原有内容...
 *         <Child>
 *         </Child>          ← 返回这里，供继续往里插
 *       </Parent>
 *
 *   ⚠️ 为什么必须能创建：switch 段里【没有 VehicleSpacing 容器】，
 *      而 15 个 VehicleSpacing_N 项都在那里面 ⇒ 不创建就永远插不对位置。
 *
 *   注意：本函数会修改 xml（新增容器），调用方需保证 cap 足够、并更新 *len。
 *   为简化，这里用一个「先探测、后创建」的两遍策略：
 *     ① 若路径全部存在 → 直接返回插入点（不改 xml）
 *     ② 若有缺失 → 创建缺失层，返回最内层新容器的插入点
 */
static size_t gc_ensure_container(char *xml, size_t cap, size_t *len,
                                  size_t span_a, size_t span_b,
                                  const char *path, int create_missing) {
    /* ---- 先探测：逐级找容器，若某级缺失则记录 ---- */
    size_t cur_a = span_a, cur_b = span_b;
    const char *p = path;
    int depth_idx = 0;

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t nlen = slash ? (size_t)(slash - p) : strlen(p);
        if (nlen == 0 || nlen >= 96) return (size_t)-1;

        /* 找开标签 */
        size_t open = (size_t)-1;
        for (size_t i = cur_a; i + nlen + 2 < cur_b; i++) {
            if (xml[i] != '<') continue;
            if (i + 1 + nlen > *len) break;
            if (memcmp(xml + i + 1, p, nlen) != 0) continue;
            char nxt = xml[i + 1 + nlen];
            if (nxt != '>' && nxt != ' ' && nxt != '\t' && nxt != '\n' && nxt != '\r')
                continue;
            open = i;
            break;
        }

        if (open == (size_t)-1) {
            /* ★ 该级容器不存在 */
            if (!create_missing) return (size_t)-1;

            /* 在上一级（cur_a..cur_b）的闭合标签之前新建这个容器。
             *
             * 🚨 close_pos 的定位有两种情形，必须分开处理：
             *   情形 A：cur_b 正好是某闭合标签的起始（嵌套下钻时 close_at）
             *   情形 B：cur_b 是闭合标签【之后】（最外层 span_b = i+9）
             * 踩过：只用「从 cur_b 往前找 </」在情形 A 下会找到【子节点】的闭合，
             *      导致新容器建到了错误的位置（甚至建到子节点里面）。 */
            size_t close_pos = (size_t)-1;
            if (cur_b + 2 <= *len && memcmp(xml + cur_b, "</", 2) == 0) {
                close_pos = cur_b;                      /* 情形 A */
            } else {
                for (size_t i = cur_b; i > cur_a + 3; i--) {   /* 情形 B */
                    if (memcmp(xml + i - 2, "</", 2) == 0) { close_pos = i - 2; break; }
                }
            }
            if (close_pos == (size_t)-1) return (size_t)-1;

            /* 缩进按层级递进 */
            char indent[64];
            int ind = 20 + depth_idx * 4;
            if (ind > 56) ind = 56;
            for (int k = 0; k < ind; k++) indent[k] = ' ';
            indent[ind] = 0;

            char frag[256];
            int fl = snprintf(frag, sizeof(frag),
                              "\n%s<%s>\n%s</%s>", indent, p, indent, p);
            if (fl <= 0 || (size_t)fl >= sizeof(frag)) return (size_t)-1;
            if (*len + (size_t)fl + 1 > cap) return (size_t)-1;

            memmove(xml + close_pos + fl, xml + close_pos, *len - close_pos + 1);
            memcpy(xml + close_pos, frag, fl);
            *len += fl;

            /* 新容器 </Name> 的起始位置
             *   frag = "\n" + indent + "<" + name + ">" + "\n" + indent + "</" + name + ">"
             *   偏移 = 1 + ind + 1 + nlen + 1 + 1 + ind = 2*ind + nlen + 4
             * 🚨 之前写成 2*ind + nlen + 3，少 1 字节 ⇒ 插入点落在 '>' 上 ⇒ 位置全错 */
            size_t new_close = close_pos + (size_t)(2 * ind + (int)nlen + 4);

            if (!slash) {
                /* 这是最后一级 → 插入点就是它的 </Name> 前 */
                return new_close;
            }

            /* 还要继续往里建下一级：新容器的内部区间
             *   <Name> 的 '>' 之后 = close_pos + 1 + ind + 1 + nlen + 1 */
            cur_a = close_pos + (size_t)(ind + (int)nlen + 3);
            cur_b = new_close;
            p = slash + 1;
            depth_idx++;
            continue;
        }

        /* 找到开标签 → 定位它的闭合 */
        size_t after_open = open + 1 + nlen;
        while (after_open < *len && xml[after_open] != '>') after_open++;
        if (after_open >= *len) return (size_t)-1;
        after_open++;

        int d = 1;
        size_t i = after_open;
        size_t close_at = (size_t)-1;
        while (i + nlen + 3 < *len && i < cur_b) {
            if (xml[i] == '<') {
                if (i + 2 + nlen < *len &&
                    xml[i + 1] == '/' &&
                    memcmp(xml + i + 2, p, nlen) == 0 &&
                    xml[i + 2 + nlen] == '>') {
                    d--;
                    if (d == 0) { close_at = i; break; }
                    i += 3 + nlen;
                    continue;
                }
                if (i + 1 + nlen < *len &&
                    memcmp(xml + i + 1, p, nlen) == 0) {
                    char nx2 = xml[i + 1 + nlen];
                    if (nx2 == '>' || nx2 == ' ' || nx2 == '\t' ||
                        nx2 == '\n' || nx2 == '\r') {
                        size_t k = i + 1 + nlen;
                        int selfc = 0;
                        while (k < *len && xml[k] != '>') {
                            if (xml[k] == '/') selfc = 1;
                            k++;
                        }
                        if (!selfc) d++;
                        i = (k < *len) ? k + 1 : *len;
                        continue;
                    }
                }
            }
            i++;
        }
        if (close_at == (size_t)-1) return (size_t)-1;

        if (!slash) return close_at;   /* 最后一级 → 插入点 */

        cur_a = after_open;
        cur_b = close_at;
        p = slash + 1;
        depth_idx++;
    }
    return (size_t)-1;
}

/* 读 <key value="N" /> 的整数（找不到返回 GC_ERR_NOTFOUND） */
int gc_xml_get_int(const char *xml, size_t len,
                   size_t span_a, size_t span_b,
                   const char *key, int *out) {
    size_t klen = strlen(key);
    for (size_t i = span_a; i + klen + 10 < span_b && i + klen + 10 < len; i++) {
        if (xml[i] != '<') continue;
        if (memcmp(xml + i + 1, key, klen) != 0) continue;
        char nxt = xml[i + 1 + klen];
        if (nxt != ' ' && nxt != '>') continue;

        size_t j = i + 1 + klen;
        while (j + 8 < span_b && j + 8 < len && memcmp(xml + j, "value=\"", 7) != 0) {
            if (xml[j] == '>') break;
            j++;
        }
        if (j + 8 >= span_b || j + 8 >= len) continue;
        if (memcmp(xml + j, "value=\"", 7) != 0) continue;
        size_t v0 = j + 7;
        if (v0 >= len) continue;
        *out = atoi(xml + v0);
        return GC_OK;
    }
    return GC_ERR_NOTFOUND;
}

/* 统计段内 <xxx value= 的个数（用于显示「本段多少项」） */
int gc_xml_count(const char *xml, size_t len, size_t a, size_t b) {
    int n = 0;
    if (b > len) b = len;
    for (size_t i = a; i + 8 < b; i++) {
        if (xml[i] == '<' && memcmp(xml + i, "<Config type=", 13) == 0) continue;
        if (xml[i] != '<') continue;
        /* <Name value=" 形式 */
        size_t j = i + 1;
        if (!((xml[j] >= 'A' && xml[j] <= 'Z') || (xml[j] >= 'a' && xml[j] <= 'z') || xml[j] == '_'))
            continue;
        while (j < b && xml[j] != ' ' && xml[j] != '>' && xml[j] != '/') j++;
        if (j + 7 <= b && memcmp(xml + j, " value=", 7) == 0) n++;
    }
    return n;
}

/* ★ 带自动重定位的版本：内部每次操作后重算段区间，可安全连续调用。
 *   span_a / span_b 会被就地更新（下次调用直接用新值即可）。
 *   span_name = "switch" / "Any" 等 */
int gc_xml_set_int_ex(char *xml, size_t cap, size_t *len,
                      size_t *span_a, size_t *span_b,
                      const char *span_name,
                      const char *key, int value, int *inserted) {
    int ins = 0;
    int rc = gc_xml_set_int(xml, cap, len, *span_a, *span_b, key, value, &ins);
    if (rc != GC_OK) return rc;

    if (ins) {
        /* 插入会让段尾右移 -> 必须重定位，否则下次插入会失败 */
        size_t na = 0, nb = 0;
        if (gc_xml_find_span(xml, *len, span_name, &na, &nb) == GC_OK) {
            *span_a = na; *span_b = nb;
        } else {
            /* 兜底：至少把上界推到新长度 */
            *span_b = *len;
        }
    }
    if (inserted) *inserted = ins;
    return GC_OK;
}
