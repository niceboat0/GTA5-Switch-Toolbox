/* gc_script.h —— ★ v5.0: GTA V Switch 版【脚本 mod（.nsc）】安装
 *
 * 背景（2026-09-30 实测 HotCoffee-NX）：
 *   脚本 mod 与 dlcpack 完全不同 —— 装在
 *     <SD>:/atmosphere/contents/0100b00b51230000/romfs/update/update2.rpf
 *       └─ switch/levels/gta5/script/script_rel.rpf
 *   其中 .nsc 必须是【普通压缩文件条目】(is_res=0, raw deflate)，
 *   不能是 RPF 资源条目、不能加 RSC7 外壳。
 *   文件名 = 脚本名（编译时烙入 .nsc）⇒ 不能改名。
 *
 * 两种操作：
 *   替换 —— 磁盘上的 .nsc 与 RPF 里已有条目同名 ⇒ 覆盖数据
 *   新增 —— 新名字 ⇒ 追加条目
 */
#ifndef GC_SCRIPT_H
#define GC_SCRIPT_H

#include <switch.h>
#include <stdio.h>

#define GC_SCRIPT_MAX 256
#define GC_SCRIPT_NAME_MAX 96

/* update2.rpf 与内层脚本容器 */
#define GC_SCRIPT_UPDATE2 \
    "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update/update2.rpf"
#define GC_SCRIPT_REL_RPF "script_rel.rpf"

/* 一个已装脚本条目 */
typedef struct {
    char name[GC_SCRIPT_NAME_MAX];  /* 条目名（含 .nsc） */
    u32  on_disk;                   /* RPF 里占的字节（压缩后） */
    u32  size;                      /* 解压后大小 */
    int  is_res;                    /* 1=资源条目(原版) 0=普通文件条目(mod) */
} GcScriptItem;

/* ★ v5.7: 批量查询项 —— 按名精确查询，不受 RPF 条目总数限制
 *   （原 gc_script_list 受 max 截断：真实 update2 有 1027 个 .nsc，
 *     排在前 128 之外的脚本会被误判为「未安装」） */
typedef struct {
    char name[GC_SCRIPT_NAME_MAX];  /* 输入：要查的名字 */
    int  installed;                 /* 输出：1=在 RPF 里 0=不在 */
} GcScriptQuery;

/* 列出 script_rel.rpf 里已有的 .nsc 条目；返回【总数】（可能 > max，
 *   调用方自行 min(n, max) 遍历 out）。负值=出错 */
int gc_script_list(GcScriptItem *out, int max);

/* ★ v5.7: 一次遍历完成：返回 RPF 内 .nsc 总数（<0=出错），
 *   并把 q[i].installed 填好。内存 O(1)、不受条数限制 ⇒ 判定「已装」用它，
 *   别用 gc_script_list（会被 max 截断）。 */
int gc_script_query(GcScriptQuery *q, int n, char *err, size_t errsz);

/* 安装一个 .nsc
 *   nsc_path   : 磁盘路径（会被读入并压缩）
 *   entry_name : RPF 里的条目名（留空则取文件名）
 *   err/errsz  : 错误信息缓冲（可 NULL）
 * 返回 0=成功；<0 失败：
 *   -1 打不开 update2.rpf  -2 找不到 script_rel.rpf  -3 空间不足/布局异常
 *   -4 读取 .nsc 失败      -5 压缩失败               -6 写入失败 */
#define GC_SCRIPT_OK            0
#define GC_SCRIPT_ERR_OPEN     -1
#define GC_SCRIPT_ERR_NOREL    -2
#define GC_SCRIPT_ERR_SPACE    -3
#define GC_SCRIPT_ERR_READ     -4
#define GC_SCRIPT_ERR_ZLIB     -5
#define GC_SCRIPT_ERR_IO       -6

int gc_script_install(const char *nsc_path, const char *entry_name,
                      char *err, size_t errsz);

/* 查某个名字是否已装（1=已装 0=未装 <0 出错） */
int gc_script_is_installed(const char *entry_name);

/* ★ v5.6: 最近一次操作的详情（成功时是 "OK: ... 共 N 个"，失败时是原因）。
 *   gc_script_list 失败时调用者拿不到 err 缓冲 ⇒ 用它把原因显示到界面上。 */
const char *gc_script_last_error(void);

/* ★ v5.2: 卸载（删除）已装脚本 —— 只从 TOC 移除条目 + 根目录 count-1，
 *   names 池与数据区不动（浪费几十字节，换取绝对安全）。
 *   返回 GC_SCRIPT_OK 或负错误码。 */
int gc_script_uninstall(const char *entry_name, char *err, size_t errsz);

/* ★ v6.2 (2026-10-01): 还原官方脚本 —— 把 romfs 内置的原版写回 RPF。
 *
 * 适用场景：某些脚本 mod（MEGATARD / HotCoffee）会替换 stock 同名脚本
 *   （借壳加载）来达到启动目的。卸载时若直接「删除」，会把 stock 也删掉
 *   ⇒ 游戏异常。用本函数覆盖回官方原版即可干净还原。
 *
 * ★★ 2026-10-01 重大修正：旧版内置的 error_listener.nsc 是【误拷的 HotCoffee
 *   加载器】(927 B)，点「还原」反而会装上热咖啡！已换成从【用户提供的原版
 *   update2.rpf】提取的真·官方文件。且官方 .nsc 全部是 RSC7 资源条目
 *   （is_res=1），不是普通条目 —— 写入逻辑见 gc_script_install 的 RSC7 分支。
 *
 * 内置 3 个官方原版（打包进 NRO 的 romfs 里，路径 stock_scripts/）：
 *   error_listener.nsc          718 B  RSC7  (size 131072)
 *   achievement_controller.nsc  31,460 B RSC7 (size 6272)
 *   shop_controller.nsc         856,627 B RSC7 (size 8519682)
 * 返回 GC_SCRIPT_OK 或负错误码（同 gc_script_install）。 */
int gc_script_restore_stock(const char *entry_name, char *err, size_t errsz);

/* ★ v6.2: 脚本页「还原官方」可用的条目名（给 UI 列按钮用） */
int gc_script_stock_count(void);
const char *gc_script_stock_name(int i);      /* i 越界返回 NULL */

/* ========================================================================= */
/* ★ v6.2: 内置模组（打进 NRO romfs，一键安装，免插卡拷文件）                 */
/* ========================================================================= */
/* 一个内置模组 = 若干 (RPF 条目名, romfs 资源路径) 对。
 * 安装时逐个调用 gc_script_install（自动处理 RSC7 / 裸条目两种形态）。 */
#define GC_MOD_MAX 8

typedef struct {
    const char *id;       /* 内部标识，如 "megatard" */
    const char *title;    /* 界面显示名（中文，≤24 字符） */
    const char *title_en; /* ★ v6.3: 英文显示名（g_lang_en=1 时用，可为 NULL=回退 title） */
    const char *author;   /* 原作者署名 */
    const char *note;     /* 一句话备注（互斥关系 / 含什么） */
    const char *note_en;  /* ★ v6.3: 英文备注（可为 NULL=回退 note） */
    int         n_files;  /* 涉及文件数 */
} GcBuiltinMod;

int gc_script_builtin_count(void);
const GcBuiltinMod *gc_script_builtin(int i);          /* i 越界返回 NULL */

/* 第 i 个模组的第 k 个文件会写入的 RPF 条目名；越界返回 NULL。
 * 界面用它提示「将覆盖 XXX」。 */
const char *gc_script_builtin_entry(int i, int k);

/* 一键安装第 i 个内置模组（写它的全部文件）。
 * 返回 GC_SCRIPT_OK；失败返回负错误码，err 里带「第 k/n 个文件」定位信息。 */
int gc_script_builtin_install(int i, char *err, size_t errsz);

#endif /* GC_SCRIPT_H */
