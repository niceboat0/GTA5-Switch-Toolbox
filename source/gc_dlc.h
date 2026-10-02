/* gc_dlc.h —— PC dlcpack → Switch 纯字节转换
 *
 * 背景：用户直接把 PC 版 add-on 车辆 mod 放进 dlcpacks/ 会导致游戏闪退
 *       （游戏去 switch/levels/... 找车，包里却是 x64/levels/... 且是 .yft）
 *
 * 本模块做「原地纯字节转换」：
 *   · 只改名字池 + TOC 的 name_off，绝不碰数据区 ⇒ 文件大小、数据偏移全不变
 *   · 不重建容器、不重新压缩资源
 */
#ifndef GC_DLC_H
#define GC_DLC_H

#include <switch.h>
#include <stdio.h>

/* 转换结果统计 */
typedef struct {
    int n_top_renamed;          /* 顶层改名条目数 */
    int n_inner_renamed;        /* 内嵌 rpf 改名条目数 */
    int n_nested_rpf;           /* 处理的内嵌 rpf 个数 */
    int n_skipped_compressed;   /* 跳过的压缩内嵌 rpf 个数 */
    int n_relocated;            /* ★ 为腾名字池空间而搬走的条目数 */
    int names_grew;             /* 名字池增长字节数 */
    int n_files;                /* 总处理文件数 */
    int n_hi_lod;               /* ★ v27: _hi 高模重定向到低模的条目数 */
    int n_hd_removed;           /* ★ v5.4: 屏蔽掉的高清贴图数（+hi.ytd 系） */
} GcDlcStats;

/* 返回值 */
#define GC_DLC_OK              0
#define GC_DLC_ERR_OPEN       -1
#define GC_DLC_ERR_FORMAT     -2   /* 不是 RPF7 */
#define GC_DLC_ERR_NOMEM      -3
#define GC_DLC_ERR_NOSPACE    -4   /* 名字池扩展需要搬条目，但找不到空闲区 */
#define GC_DLC_ERR_IO         -5
#define GC_DLC_ERR_SHIFT      -6   /* name_shift != 0，不支持 */

/* ★ 把 PC 格式的 dlc.rpf 原地转成 Switch 格式
 *   path  : dlc.rpf 路径
 *   st    : 输出统计（可为 NULL）
 *   err   : 错误信息缓冲
 *   返回 GC_DLC_OK 或负错误码
 *   幂等：已是 Switch 格式的包会「0 处改名」正常返回 */
int gc_dlc_convert(const char *path, GcDlcStats *st, char *err, size_t errsz);

/* 这个名字是否需要平台改名（x64 / .yXX） */
int gc_dlc_needs_rename(const char *name);

/* 判「包是否已经是 Switch 格式」（顶层有 switch 目录、无 x64 目录） */
int gc_dlc_is_switch_format(const char *path);

/* ★ v5.4: 屏蔽高清贴图（+hi.ytd / +hidr.ytd / +hidd.ytd / +hifr.ytd）
 *   对标官方 GTAVPatcher 的 RemoveHighDetailTextures（「recommended for Switch」）。
 *   实现 = 名字首字符 '+' → '~'（等长改名屏蔽，零数据搬移、可逆）。
 *   注意：官方删的是【高清贴图】，不是 _hi.nft【模型】（后者删了必闪退）。
 *   removed 输出实际屏蔽条数（可为 NULL）。
 *   返回 >=0 = 屏蔽条数；<0 = GC_DLC_ERR_*。幂等。 */
int gc_dlc_hide_hd_textures(const char *path, int *removed, char *err, size_t errsz);

/* ★ v31: 地图数据自检 —— 判断地图类 dlcpack 能否被引擎正常加载
 *
 * 背景（2026-09-30 实测 AkinaV 秋名山）：
 *   ymap（地图实体）靠 `_manifest.nmf` 驱动注册。
 *   若 manifest 放在附属 rpf（如 xxx_metadata.rpf）而【不在主 rpf】里，
 *   则地图实体一个都不注册 ⇒ 表现为【贴图能显示、模型完全不渲染、无碰撞】。
 *   把 manifest 挪进主 rpf 后模型立即正常 ⇒ 该判据成立。
 *
 * 返回：
 *   GC_MAP_NONE  0  无地图资源（纯车辆/道具包，不涉及本问题）
 *   GC_MAP_OK    1  有地图资源，且 manifest 在主 rpf 里（正常）
 *   GC_MAP_BAD   2  有地图资源，但 manifest 不在主 rpf（★ 会出上述症状）
 *   GC_MAP_ERR  -1  读不了
 * 只读 RPF 头部区域，不加载数据体 ⇒ 大包也很快。 */
#define GC_MAP_NONE   0
#define GC_MAP_OK     1
#define GC_MAP_BAD    2
#define GC_MAP_ERR   -1
int gc_dlc_check_map(const char *path);

/* ★ v32: 统计 dlcpack 的资源构成（模型/地图数据/贴图/其他），供列表展示
 *   只读 RPF 头部与名字池，不加载数据体 ⇒ 大包也很快
 *   任一输出指针可为 NULL。计数不含目录条目。 */
void gc_dlc_scan_res(const char *path,
                     int *n_model, int *n_map, int *n_tex, int *n_other);

/* ★ v33: 统一探测结果 + 一次 IO 拿全部信息
 *
 * 性能背景：分别调用 is_switch_format / check_map / scan_res 会重复读同一份数据
 *（改造前每包约 10 次 SD 随机读，列表一多就明显卡）。
 * 本函数每个 RPF 只读 1 次头部 + 1 次名字池，所有判断在内存完成。
 *   ✅ 列表扫描请优先用这个，别分别调用上面三个。 */
typedef struct {
    int fmt;                    /* 1=Switch 格式  0=PC 格式  -1=读不了 */
    int map_st;                 /* GC_MAP_* */
    int n_model, n_map, n_tex;  /* 资源构成 */
    char vehicle[48];           /* ★ v5.2: 车辆刷车名（第一个非 _hi 的 .nft 基名，车辆包才有） */
} GcDlcProbe;

void gc_dlc_probe(const char *path, GcDlcProbe *out);

#endif /* GC_DLC_H */
