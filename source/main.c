#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>          /* ★ v25: rmdir/remove（DLC 删除功能） */
#include <time.h>
#include <inttypes.h>
#include <stdarg.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <zlib.h>          /* ★ v6: 性能调参页需要 deflate */

#include "save.h"
#include "gc_rpf.h"
#include "gc_dlc.h"
#include "gc_script.h"   /* ★ v5.0: 脚本 mod（.nsc）安装 */
#include "gc_presets.h"
#include "lang.h"        /* ★ v6.3: 双语 TR() 宏 */

/* 屏幕分辨率 */
#define FB_WIDTH  1280
#define FB_HEIGHT 720

/* GTA V Switch 版 Title ID */
#define GTAV_TITLE_ID 0x0100B00B51230000ULL

/* ========================================================================= */
/* ★ v5: 统一布局常量 -- 全部区块的 y 区间由这里定义, 杜绝魔数散落造成重叠     */
/* ========================================================================= */
#define LY_PAD        40                      /* 左右安全边距 */
#define LY_HEADER_Y   0
#define LY_HEADER_H   62
#define LY_INFO_Y     66
#define LY_INFO_H     42
/* ★ v6.1 (2026-10-01): 顶部「角色标签栏」条带已废弃 —— 它只服务于已移除的
 *   「存档修改」页（渲染块已整块删除）⇒ 112..150 这 44px 永远空白。
 *   现把该条带回收给内容区：LY_BODY_Y 从 156 上提到 112，LY_BODY_H 从 420 增至 464。
 *   （LY_CTAB_* 常量已删除，无残留引用。） */
#define LY_BODY_Y     112                     /* ★ v6.1: 156 -> 112（吸收废弃条带）*/
#define LY_BODY_H     464                     /* ★ v6.1: 420 -> 464（+44）*/
#define LY_BTN_Y      584                     /* 统一按钮栏 584..632 */
#define LY_BTN_H      48
#define LY_HINT_Y     640                     /* 提示行 640..676 */
#define LY_BODY_BOT   (LY_BODY_Y + LY_BODY_H) /* = 576（与按钮栏仍留 8px 间隙）*/
/* 内容区右边界 (给右侧滚动条留 22px) */
#define LY_CONTENT_R  (FB_WIDTH - LY_PAD - 22)   /* = 1218 */

/* 界面主题配色 (深色典雅，无 Emoji 干净排版) */
/* ★★★ v28e 关键修正：本工程 framebuffer = PIXEL_FORMAT_RGBA_8888，
 *   其像素 u32 的内存序是 0xAABBGGRR（R 在【低字节】，与 PC 常见的 BGRA 相反）。
 *   ⇒ 源码里所有颜色【禁止直接写 0xAARRGGBB】！
 *     必须用 RGB_A(r,g,b) 宏（内部按 #RRGGBB 习惯书写，自动转成 ABGR）。
 *   🚨 历史事故：全 UI 一直按 HTML 顺序写色值 ⇒ R/B 互换 ⇒ 主色蓝 #58A6FF
 *      被渲染成橙棕 #FFA658 ⇒ 用户连续 4 版反馈「配色还是棕色」。 */
#define RGB_A(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* ★★ v28 配色体系（借鉴 hbmenu/Goldleaf/Tesla，色值均按 #RRGGBB 书写）：
 *   主色 ACCENT 只用于「选中/焦点」；状态用小色点；金色只留「有修改/警告」；红色只留危险。
 *   层级：BG(最深) < SURFACE(卡片) < SURFACE_HI(选中) < HEADER(顶栏) */
#define C_BG          RGB_A(0x0E, 0x11, 0x16)   /* #0E1116 近黑蓝底 */
#define C_HEADER      RGB_A(0x16, 0x1B, 0x22)   /* #161B22 顶栏/信息栏 */
#define C_CARD        RGB_A(0x1E, 0x26, 0x33)   /* #1E2633 卡片（SURFACE） */
#define C_CARD_SEL    RGB_A(0x2E, 0x44, 0x62)   /* #2E4462 选中卡片（SURFACE_HI） */
#define C_BORDER      RGB_A(0x1C, 0x24, 0x30)   /* #1C2430 = 卡片色（视觉无边框） */
#define C_BORDER_SEL  RGB_A(0x58, 0xA6, 0xFF)   /* #58A6FF GitHub 蓝--选中条/焦点 */
#define C_ACCENT      RGB_A(0x58, 0xA6, 0xFF)   /* #58A6FF 主色 */
#define C_TEXT        RGB_A(0xE8, 0xEC, 0xF2)   /* #E8ECF2 主文字 */
#define C_TEXT_MUTED  RGB_A(0x8B, 0x94, 0x9E)   /* #8B949E 次要文字 */
#define C_GREEN       RGB_A(0x3F, 0xB9, 0x50)   /* #3FB950 状态绿 */
#define C_GOLD        RGB_A(0xE3, 0xB3, 0x41)   /* #E3B341 金（只用于「有修改/警告」） */
#define C_RED         RGB_A(0xF8, 0x51, 0x49)   /* #F85149 危险红 */
#define C_BLUE        RGB_A(0x58, 0xA6, 0xFF)   /* 与主色统一 */
#define C_ITEM_SEL    RGB_A(0x2E, 0x44, 0x62)   /* = C_CARD_SEL */
#define C_MODAL_BG    RGB_A(0x12, 0x16, 0x1D)   /* #12161D 弹窗底 */


static FT_Library g_ft;
static FT_Face    g_face = NULL;
static PadState   g_pad;

/* ★ v6.3: 双语支持 -- 0=中文（默认） 1=English
 * detect_language() 在 main() 开头按系统语言置位 */
int g_lang_en = 0;
void detect_language(void) {
    /* set 服务：读系统语言码（u64，前 8 字节是 "en-US" 这类 ASCII） */
    u64 langcode = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&langcode))) {
        SetLanguage sl = SetLanguage_ZHCN;
        if (R_SUCCEEDED(setMakeLanguage(langcode, &sl))) {
            /* libnx SetLanguage 枚举：1=enUS 12=enGB，其余（含 zh/ja/...）⇒ 中文 */
            if (sl == SetLanguage_ENUS || sl == SetLanguage_ENGB) g_lang_en = 1;
        } else {
            /* setMakeLanguage 失败（老固件）：直接看语言码前 2 字节是不是 "en" */
            const char *lc = (const char *)&langcode;
            if (lc[0] == 'e' && lc[1] == 'n') g_lang_en = 1;
        }
    }
}

/* ========================================================================= */
/* ★ v4 新增: 触屏支持 + 命中测试框架                                         */
/* ========================================================================= */
/* 设计:
 *   渲染时把每个「可点区域」登记进 g_hot[] (hotzone);
 *   下一帧处理输入时，用触摸点做命中测试，转换成与按键等价的「动作」。
 *   这样按键与触屏共用同一套动作处理逻辑，不会出现两套行为。 */

/* 统一动作枚举 */
typedef enum {
    ACT_NONE = 0,
    /* 导航 */
    ACT_NAV_LEFT, ACT_NAV_RIGHT, ACT_NAV_UP, ACT_NAV_DOWN,
    /* 页签 */
    ACT_TAB_SAVE, ACT_TAB_GFX, ACT_TAB_TOOL, ACT_TAB_PERF, ACT_TAB_DLC,
    ACT_TAB_SCRIPT,       /* ★ v5.5: 脚本 mod 独立页签 */
    /* 通用 */
    ACT_CONFIRM, ACT_BACK,
    /* ★ 底部大号 −/+ */
    ACT_BIG_MINUS, ACT_BIG_PLUS,
    /* 功能 */
    ACT_MAX_ALL,          /* 拉满当前项 */
    ACT_MAX_CHAR,         /* 当前角色全满 */
    ACT_CYCLE_STEP,       /* 切换步长 */
    ACT_SAVE,             /* 保存 */
    ACT_QUIT,             /* 保存并退出 */
    ACT_LOAD_PRESET,      /* 载入内置存档 */
    ACT_LOAD_BACKUP,      /* 载入历史备份 */
    ACT_PICK_SLOT,        /* 切换槽位 */
    ACT_CHAR_PREV,        /* 上一个角色 */
    ACT_CHAR_NEXT,        /* 下一个角色 */
    ACT_GFX_PRESET,       /* 画质预设 */
    ACT_GFX_IMPORT,       /* 导入画质 */
    ACT_GFX_CHECK,        /* 路径自检 */
    ACT_PICK_ITEM,        /* 直接选中某项 (param=索引) */
    ACT_SCROLL_UP,        /* 画质列表滚动 */
    ACT_SCROLL_DOWN,
    ACT_SCROLL_TOP,       /* ★ v5: 翻到顶 */
    ACT_SCROLL_BOTTOM,    /* ★ v5: 翻到底 */
    /* ★ v6 性能调参页 */
    ACT_GC_LOAD,          /* 载入 update.rpf */
    ACT_GC_PICK_KNOB,     /* 点选旋钮 (param=索引) */
    ACT_GC_GROUP_PREV,    /* ★ v24: 跳到上一权重组组首 (S/A/B/C) */
    ACT_GC_GROUP_NEXT,    /* ★ v24: 跳到下一权重组组首 */
    ACT_GC_WRITE,         /* 写入 update.rpf */
    ACT_GC_RESTORE,       /* 恢复原版（套用原版预设） */
    ACT_GC_SELFTEST,      /* ★ 路径自检 */
    ACT_GC_VERIFY,        /* ★ 回读校验（确认写入是否生效） */
    /* ★ v21 DLC 管理页 */
    ACT_DLC_LOAD,         /* 扫描 dlcpacks + 读 dlclist */
    ACT_DLC_PICK,         /* 点选某个 DLC (param=索引) */
    ACT_DLC_TOGGLE,       /* 注册/注销选中项 */
    ACT_DLC_SCAN_SRC,     /* 扫描待导入目录 */
    ACT_DLC_IMPORT,       /* 导入选中的待装 DLC（复制+注册） */
    ACT_DLC_VERIFY,       /* 完整性检查（车辆资源是否齐全） */
    ACT_DLC_VIEW,         /* 切换视图：已装 / 待导入 */
    ACT_DLC_CONVERT,      /* ★ v22: 把 PC 格式的包就地转成 Switch 格式 */
    ACT_DLC_DELETE,       /* ★ v25: 删除选中的已装 DLC（注销 + 清空目录） */
    ACT_DLC_HIDEHD,       /* ★ v5.4: 屏蔽高清贴图（+hi.ytd 系，对标官方） */
    ACT_OPEN_MODELDIR,    /* ★ v5.4: 打开/定位模型目录（SD 卡 dlcpacks） */
    /* ★ v5.5 脚本 mod 独立页 */
    ACT_SCRIPT_SCAN,      /* 重新扫描 .nsc */
    ACT_SCRIPT_INSTALL,   /* 安装选中的 .nsc */
    ACT_SCRIPT_UNINSTALL, /* 删除选中的已装脚本 */
    ACT_SCRIPT_DIR,       /* 显示脚本目录路径（并自动建目录） */
    /* ★ v6.2: 脚本管理二级菜单（还原官方 + 内置模组一键安装）
     *   原先是两个「还原官方 error_listener / controller」按钮 —— 文字太长
     *   会溢出格子（实机截图已证实），现合并为一个入口进弹窗。 */
    ACT_SCRIPT_MGR,
    ACT_SCRIPT_CONFIRM_OK,    /* ★ v6.2: 二次确认弹窗的「确认」按钮 */
} UiAction;

/* ★ 手动调整列表可见行数（行高 34 ⇒ 9 行 = 306，留出提示行空间） */
#define GC_KNOB_VIS 12

/* 可点区域 */
#define MAX_HOTZONES 96
typedef struct {
    int x, y, w, h;
    int action;   /* UiAction */
    int param;    /* 附加参数 */
} HotZone;

static HotZone g_hot[MAX_HOTZONES];
static int     g_hot_count = 0;

/* 触屏状态 */
static HidTouchScreenState g_touch;
static int g_touch_x = -1, g_touch_y = -1;   /* 当前触摸点 (-1=无) */
static int g_touch_tapped = 0;               /* 本帧是否有「新触摸」 */
static u64 g_touch_last_tick = 0;            /* 上次触摸时间(防连击) */

/* ★★ v24 触屏点击反馈（用户反馈「选中按钮没有高亮显示」）
 *   点中任意热区后，把该热区的【矩形副本】与时间记下来，
 *   渲染时在所有 UI 之上叠一层醒目高亮框（先白后金，TAP_FX_MS 内渐隐）。
 *   为什么记矩形而不是记坐标：点页签/列表项会导致本帧热区表整体变化，
 *   用坐标重新命中会打到别的控件上；记矩形则一定画在「你刚点的那个东西」上。 */
static int  g_tap_fx[4] = {0, 0, 0, 0};      /* x, y, w, h */
static int  g_tap_fx_on  = 0;
static u64  g_tap_fx_tick = 0;
#define TAP_FX_MS 420

/* ★ v24: 该矩形是否就是「刚被点的那个热区」--给按钮画按下高亮用
 *   （必须在渲染里逐帧判断，不能缓存：按钮栏每帧重算位置） */
static int tap_is_hot(int x, int y, int w, int h) {
    if (!g_tap_fx_on) return 0;
    u64 el = (svcGetSystemTick() - g_tap_fx_tick) * 1000 / 19200000ULL;
    if (el >= TAP_FX_MS) return 0;
    return (g_tap_fx[0] == x && g_tap_fx[1] == y &&
            g_tap_fx[2] == w && g_tap_fx[3] == h);
}

static void hot_reset(void) { g_hot_count = 0; }

static void hot_add(int x, int y, int w, int h, int action, int param) {
    if (g_hot_count >= MAX_HOTZONES) return;
    HotZone *z = &g_hot[g_hot_count++];
    z->x = x; z->y = y; z->w = w; z->h = h;
    z->action = action; z->param = param;
}

/* 命中测试: 返回 hotzone 索引, -1 = 未命中 (后登记的优先 = 上层覆盖下层) */
static int hot_hit(int px, int py) {
    for (int i = g_hot_count - 1; i >= 0; i--) {
        HotZone *z = &g_hot[i];
        if (px >= z->x && px < z->x + z->w && py >= z->y && py < z->y + z->h)
            return i;
    }
    return -1;
}

/* 每帧开头读取触摸屏, 更新 g_touch_x/y 与 g_touch_tapped */
static void touch_poll(void) {
    g_touch_tapped = 0;
    g_touch_x = -1;
    g_touch_y = -1;
    if (hidGetTouchScreenStates(&g_touch, 1) <= 0 || g_touch.count <= 0) {
        g_touch_last_tick = 0;
        return;
    }
    /* 只取第一个触点 (Switch 电容屏最多 16 点, 单指足够) */
    g_touch_x = (int)g_touch.touches[0].x;
    g_touch_y = (int)g_touch.touches[0].y;

    /* 防连击: 距离上次触发 >= 180ms 才算「新点击」 */
    u64 now = svcGetSystemTick();
    u64 ms = (now - g_touch_last_tick) * 1000 / 19200000ULL;
    if (g_touch_last_tick == 0 || ms >= 180) {
        g_touch_tapped = 1;
        g_touch_last_tick = now;
    }
}

/* 摇杆方向 -> 方向键位（超出阈值即模拟方向键）
 * ★ v28: 阈值 8000->4000（更灵敏）
 * ★ v31: 参数化 -- 左摇杆(0) / 右摇杆(1) 共用；用户要求【右摇杆与左摇杆功能完全一致】 */
#define JOY_THRESHOLD 4000
static u32 joy_to_dpad(int idx) {
    HidAnalogStickState st = padGetStickPos(&g_pad, idx);
    u32 bits = 0;
    if (st.x > JOY_THRESHOLD) bits |= HidNpadButton_Right;
    if (st.x < -JOY_THRESHOLD) bits |= HidNpadButton_Left;
    if (st.y > JOY_THRESHOLD) bits |= HidNpadButton_Up;
    if (st.y < -JOY_THRESHOLD) bits |= HidNpadButton_Down;
    return bits;
}
static u32        g_stride_words = 0;

/* 绘制矩形 */
static void gfx_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int j = y; j < y + h; j++) {
        uint32_t *row = fb + j * g_stride_words + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

/* 绘制矩形边框 */
static void gfx_rect_outline(uint32_t *fb, int x, int y, int w, int h, int thickness, uint32_t color) {
    gfx_rect(fb, x, y, w, thickness, color);
    gfx_rect(fb, x, y + h - thickness, w, thickness, color);
    gfx_rect(fb, x, y, thickness, h, color);
    gfx_rect(fb, x + w - thickness, y, thickness, h, color);
}

/* ★ v28: sqrt 近似（避免拉 libm；牛顿迭代 4 轮足够画圆角） */
static double sqrt_approx(double v) {
    if (v <= 0) return 0;
    double x = v > 1 ? v : 1;
    for (int i = 0; i < 4; i++) x = (x + v / x) * 0.5;
    return x;
}

/* ★ v29: 全局卡片圆角开关 -- 0 = 直角卡片（当前风格，更利落）
 *   想恢复圆角只需改成 6/8/10，所有调用点自动跟随 */
#define CARD_R 0

/* ★ v28: 卡片（纯色块 + 可选圆角，无描边）
 *   r = 圆角半径；★ v29 起由 CARD_R 统一覆盖，调用点传值不再生效 */
static void gfx_card(uint32_t *fb, int x, int y, int w, int h, uint32_t color, int r) {
    r = CARD_R;                       /* ★ v29: 统一由开关控制 */
    if (r <= 0) { gfx_rect(fb, x, y, w, h, color); return; }
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    /* 中段（不含上下圆角带） */
    gfx_rect(fb, x, y + r, w, h - 2 * r, color);
    /* 上/下直边段 */
    gfx_rect(fb, x + r, y, w - 2 * r, r, color);
    gfx_rect(fb, x + r, y + h - r, w - 2 * r, r, color);
    /* 四角：逐行阶梯近似圆弧 */
    for (int i = 1; i < r; i++) {
        int inset = (int)(r - sqrt_approx((double)r * r - (double)i * i));
        gfx_rect(fb, x + r - inset, y + r - i, inset, 1, color);            /* 左上 */
        gfx_rect(fb, x + w - r, y + r - i, inset, 1, color);                /* 右上 */
        gfx_rect(fb, x + r - inset, y + h - r + i - 1, inset, 1, color);    /* 左下 */
        gfx_rect(fb, x + w - r, y + h - r + i - 1, inset, 1, color);        /* 右下 */
    }
}

/* ★ v28: 左侧主色条（选中标记，Goldleaf 风格 3px） */
static void gfx_accent_bar(uint32_t *fb, int x, int y, int h, uint32_t color) {
    gfx_rect(fb, x, y, 3, h, color);
}

/* UTF-8 解码 */
static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t*)*p;
    if (!*s) return 0;
    uint32_t cp = 0;
    int len = 0;
    if (*s < 0x80) { cp = *s; len = 1; }
    else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; len = 2; }
    else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; len = 3; }
    else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; len = 4; }
    else { (*p)++; return 0xFFFD; }

    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { (*p) += len; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

/* 绘制文本 */
static int draw_text(uint32_t *fb, int x, int y, int size, const char *str, uint32_t color) {
    if (!g_face || !str) return x;
    FT_Set_Pixel_Sizes(g_face, 0, size);

    uint8_t cr = (color >> 0)  & 0xFF;
    uint8_t cg = (color >> 8)  & 0xFF;
    uint8_t cb = (color >> 16) & 0xFF;
    int pen_x = x;
    int baseline = y + size;

    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == '\n') break;

        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot slot = g_face->glyph;
        FT_Bitmap *bmp = &slot->bitmap;

        int gx = pen_x + slot->bitmap_left;
        int gy = baseline - slot->bitmap_top;

        for (unsigned int r = 0; r < bmp->rows; r++) {
            int py = gy + (int)r;
            if (py < 0 || py >= FB_HEIGHT) continue;
            for (unsigned int c = 0; c < bmp->width; c++) {
                int px = gx + (int)c;
                if (px < 0 || px >= FB_WIDTH) continue;

                uint8_t a = bmp->buffer[r * bmp->pitch + c];
                if (a == 0) continue;

                uint32_t *dst = fb + py * g_stride_words + px;
                if (a == 255) {
                    *dst = (0xFFu << 24) | ((uint32_t)cb << 16) | ((uint32_t)cg << 8) | cr;
                } else {
                    uint32_t cur = *dst;
                    uint8_t br = (cur >> 0)  & 0xFF;
                    uint8_t bg = (cur >> 8)  & 0xFF;
                    uint8_t bb = (cur >> 16) & 0xFF;
                    uint8_t out_r = (cr * a + br * (255 - a)) / 255;
                    uint8_t out_g = (cg * a + bg * (255 - a)) / 255;
                    uint8_t out_b = (cb * a + bb * (255 - a)) / 255;
                    *dst = (0xFFu << 24) | ((uint32_t)out_b << 16) | ((uint32_t)out_g << 8) | out_r;
                }
            }
        }
        pen_x += slot->advance.x >> 6;
    }
    return pen_x;
}

/* ========================================================================= */
/* ★ v5: 精确文字宽度测量 -- 原代码用 strlen()*N 估宽, 而 UTF-8 汉字占 3 字节,
 *   导致估出的宽度是实际的 2~3 倍, 文字被推得严重偏左/偏右。
 *   这里按「码点数」而非「字节数」计算, 汉字/全角按 1 个字宽, ASCII 按 0.5。
 *   返回像素宽度 (近似, 与 FreeType 实测偏差 < 10%) */
static int text_width(const char *s, int size) {
    if (!s) return 0;
    const char *p = s;
    int w = 0;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0 || cp == '\n') break;
        /* CJK / 全角 / 常用符号 = 整宽; ASCII 及窄符号 = 半宽 */
        if (cp >= 0x1100 && !(cp >= 0x2010 && cp <= 0x203B)) w += size;
        else w += (size + 1) / 2;
    }
    return w;
}

/* 居中绘制文字: 以 cx 为水平中心, 返回实际起点 x */
static int draw_text_c(uint32_t *fb, int cx, int y, int size, const char *str, uint32_t color) {
    int w = text_width(str, size);
    int x = cx - w / 2;
    draw_text(fb, x, y, size, str, color);
    return x;
}

/* 右对齐绘制: 以 rx 为右边界 */
static int draw_text_r(uint32_t *fb, int rx, int y, int size, const char *str, uint32_t color) {
    int w = text_width(str, size);
    int x = rx - w;
    draw_text(fb, x, y, size, str, color);
    return x;
}

/* ========================================================================= */
/* ★ v5: 自绘符号 -- 系统共享字体缺 U+2212(−) / U+25C0(◀) / U+25B6(▶) 等字形,
 *   直接用矩形拼, 完全不依赖字体, 保证任何环境下都显示正常。
 *   cx,cy = 符号中心; sz = 符号半径/半宽; th = 笔画粗细 */
/* 三角形 (dir: 0=上 1=下 2=左 3=右) */
static void gfx_tri(uint32_t *fb, int cx, int cy, int sz, int dir, uint32_t color) {
    for (int i = 0; i < sz; i++) {
        int span = sz - i;                       /* 越远离尖端越宽 */
        switch (dir) {
            case 0: gfx_rect(fb, cx - span, cy - sz + i, span * 2, 1, color); break; /* 上 */
            case 1: gfx_rect(fb, cx - span, cy + sz - i, span * 2, 1, color); break; /* 下 */
            case 2: gfx_rect(fb, cx - sz + i, cy - span, 1, span * 2, color); break; /* 左 */
            default:gfx_rect(fb, cx + sz - i, cy - span, 1, span * 2, color); break; /* 右 */
        }
    }
}

/* 加号 / 减号 (减号也自绘, 避开字体缺字形) */
static void gfx_plus_sign(uint32_t *fb, int cx, int cy, int arm, int th, uint32_t color) {
    gfx_rect(fb, cx - arm, cy - th / 2, arm * 2, th, color);   /* 横 */
    gfx_rect(fb, cx - th / 2, cy - arm, th, arm * 2, color);   /* 竖 */
}
static void gfx_minus_sign(uint32_t *fb, int cx, int cy, int arm, int th, uint32_t color) {
    gfx_rect(fb, cx - arm, cy - th / 2, arm * 2, th, color);   /* 只有横 */
}

/* 实心圆点 (状态指示) */
static void gfx_dot(uint32_t *fb, int cx, int cy, int r, uint32_t color) {
    for (int j = -r; j <= r; j++) {
        for (int i = -r; i <= r; i++) {
            if (i * i + j * j <= r * r)
                gfx_rect(fb, cx + i, cy + j, 1, 1, color);
        }
    }
}

/* 屏幕中央提示 (保存成功 / 载入成功等), 2.5 秒后自动消失 */
static char     g_toast_msg[160] = {0};
static uint32_t g_toast_color = C_GREEN;
static uint64_t g_toast_tick  = 0;   /* 触发时记录系统 tick, 用于自动消失 */

static void show_toast(const char *msg, uint32_t color) {
    strncpy(g_toast_msg, msg, sizeof(g_toast_msg) - 1);
    g_toast_msg[sizeof(g_toast_msg) - 1] = '\0';
    g_toast_color = color;
    g_toast_tick = svcGetSystemTick();
}

/* ========================================================================= */
/* ★★ v22: 长任务进度覆盖层                                                   */
/* ========================================================================= */
/* 解决的问题（用户反馈）：
 *   「软件在导入dlc的时候没有加载动画没有提示，会认为卡住了」
 *
 * 设计要点：
 *   · 导入/转换是【阻塞】操作（复制几十 MB + 写 rpf），主循环进不去
 *   · 所以用「子帧渲染」：任务内部周期性调 dlc_prog_frame()，
 *     它自己 framebufferBegin -> 画进度 -> framebufferEnd，与主循环互不干扰
 *   · 必须限制刷新频率（约 20 FPS），否则 889 MB 的 IO 会被渲染拖垮
 *   · 画一个不确定进度的「跑马灯 + 旋转方块」，让用户明确看到「在动」 */
static char     g_dlc_prog_msg[192] = {0};
static char     g_dlc_prog_sub[192] = {0};
static int      g_dlc_prog_pct      = -1;    /* -1 = 不确定进度 */
static int      g_dlc_prog_active   = 0;
static uint64_t g_dlc_prog_last     = 0;
static int      g_dlc_prog_phase    = 0;
static Framebuffer *g_dlc_prog_fb   = NULL;

#define DLC_PROG_INTERVAL_MS 50      /* 约 20 FPS，别太快否则拖慢 IO */

static void dlc_prog_begin(const char *msg, const char *sub) {
    snprintf(g_dlc_prog_msg, sizeof(g_dlc_prog_msg), "%s", msg ? msg : "");
    snprintf(g_dlc_prog_sub, sizeof(g_dlc_prog_sub), "%s", sub ? sub : "");
    g_dlc_prog_pct = -1;
    g_dlc_prog_active = 1;
    g_dlc_prog_phase = 0;
    g_dlc_prog_last = 0;          /* 下一帧立刻画一次 */
}

static void dlc_prog_set(const char *msg, const char *sub, int pct) {
    if (msg) snprintf(g_dlc_prog_msg, sizeof(g_dlc_prog_msg), "%s", msg);
    if (sub) snprintf(g_dlc_prog_sub, sizeof(g_dlc_prog_sub), "%s", sub);
    g_dlc_prog_pct = pct;
}

static void dlc_prog_end(void) { g_dlc_prog_active = 0; }

/* ★ 在阻塞任务里周期性调用：自带限频，自己开子帧渲染 */
static void dlc_prog_frame(void) {
    if (!g_dlc_prog_active || !g_dlc_prog_fb) return;
    uint64_t now = svcGetSystemTick();
    uint64_t ms = (now - g_dlc_prog_last) * 1000ULL / 19200000ULL;
    if (g_dlc_prog_last != 0 && ms < DLC_PROG_INTERVAL_MS) return;
    g_dlc_prog_last = now;
    g_dlc_prog_phase = (g_dlc_prog_phase + 1) % 1000;

    /* ---- 子帧渲染 ---- */
    u32 stride = 0;
    uint32_t *fb = (uint32_t*)framebufferBegin(g_dlc_prog_fb, &stride);
    if (!fb) return;
    u32 sw = stride / sizeof(uint32_t);

    /* 全屏压暗（画一个半透明黑底效果：直接画深色） */
    for (int y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = fb + (size_t)y * sw;
        for (int x = 0; x < FB_WIDTH; x++) row[x] = RGB_A(0x0D, 0x10, 0x14);   /* v28e: ABGR 序 */
    }

    int bx = 240, by = 250, bw = FB_WIDTH - 480, bh = 200;
    gfx_rect(fb, bx, by, bw, bh, C_MODAL_BG);
    gfx_rect_outline(fb, bx, by, bw, bh, 2, C_BORDER_SEL);

    draw_text_c(fb, FB_WIDTH / 2, by + 26, 26, g_dlc_prog_msg, C_ACCENT);
    if (g_dlc_prog_sub[0])
        draw_text_c(fb, FB_WIDTH / 2, by + 68, 16, g_dlc_prog_sub, C_TEXT);

    /* 进度条 */
    int pbx = bx + 40, pby = by + 108, pbw = bw - 80, pbh = 22;
    gfx_rect(fb, pbx, pby, pbw, pbh, C_BG);
    gfx_rect_outline(fb, pbx, pby, pbw, pbh, 1, C_BORDER);
    if (g_dlc_prog_pct >= 0) {
        int p = g_dlc_prog_pct; if (p > 100) p = 100; if (p < 0) p = 0;
        int fw = (pbw - 4) * p / 100;
        if (fw > 0) gfx_rect(fb, pbx + 2, pby + 2, fw, pbh - 4, C_GREEN);
        char pb[32];
        snprintf(pb, sizeof(pb), "%d%%", p);
        draw_text_c(fb, FB_WIDTH / 2, pby + pbh + 10, 17, pb, C_TEXT);
    } else {
        /* 不确定进度：跑马灯块来回跑 */
        int seg = (pbw - 8) / 5;
        int span = pbw - 8 - seg;
        int ph = g_dlc_prog_phase % 200;
        int t = (ph < 100) ? ph : (200 - ph);        /* 0..100 往返 */
        int ox = span * t / 100;
        gfx_rect(fb, pbx + 4 + ox, pby + 4, seg, pbh - 8, C_BORDER_SEL);
    }

    /* 旋转方块（4 个点，动起来就不会被误认死机） */
    {
        static const int dx[4] = { 0, 1, 1, 0 };
        static const int dy[4] = { 0, 0, 1, 1 };
        int on = (g_dlc_prog_phase / 6) % 4;
        int cx = FB_WIDTH / 2 - 30;
        int cy = pby + pbh + 44;
        for (int k = 0; k < 4; k++) {
            gfx_rect(fb, cx + dx[k] * 16, cy + dy[k] * 16, 12, 12,
                     (k == on) ? C_ACCENT : C_BORDER);
        }
        draw_text(fb, cx + 80, cy + 6, 16, TR("处理中，请勿断电", "Working, do not power off"), C_TEXT_MUTED);
    }

    framebufferEnd(g_dlc_prog_fb);
}

/* 渲染屏幕中央提示框 (每帧调用, 到点自动隐藏) */
static void draw_toast(uint32_t *fb) {
    if (g_toast_msg[0] == '\0') return;
    uint64_t elapsed = (svcGetSystemTick() - g_toast_tick) * 1000 / 19200000ULL; /* ns -> ms (192MHz) */
    if (elapsed > 2500) { g_toast_msg[0] = '\0'; return; }

    /* 淡出: 最后 500ms 渐隐 */
    uint32_t alpha = 255;
    if (elapsed > 2000) alpha = (uint32_t)(255 - (elapsed - 2000) * 255 / 500);
    if (alpha == 0) { g_toast_msg[0] = '\0'; return; }

    /* 估算文本宽度 (中文字符按字号宽, 近似) */
    int msg_len = 0; const char *p = g_toast_msg;
    while (*p) { uint32_t cp = utf8_next(&p); if (cp == 0) break; msg_len++; }
    int font_h = 26;
    int box_w = 60 + msg_len * font_h;
    if (box_w > FB_WIDTH - 100) box_w = FB_WIDTH - 100;
    int box_h = 72;
    int bx = (FB_WIDTH - box_w) / 2;
    int by = (FB_HEIGHT - box_h) / 2;

    /* 半透明背景 + 金色边框 (带 alpha) */
    for (int j = by; j < by + box_h; j++) {
        for (int i = bx; i < bx + box_w; i++) {
            if (i < 0 || i >= FB_WIDTH || j < 0 || j >= FB_HEIGHT) continue;
            uint32_t *dst = fb + j * g_stride_words + i;
            uint32_t cur = *dst;
            uint8_t cr = (cur >> 0) & 0xFF, cg = (cur >> 8) & 0xFF, cb = (cur >> 16) & 0xFF;
            uint8_t br = (0x18 >> 0) & 0xFF, bg = (0x1C >> 8) & 0xFF, bb = (0x22 >> 16) & 0xFF;
            uint8_t a = (uint8_t)(200 * alpha / 255);
            uint8_t or_ = (br * a + cr * (255 - a)) / 255;
            uint8_t og = (bg * a + cg * (255 - a)) / 255;
            uint8_t ob = (bb * a + cb * (255 - a)) / 255;
            *dst = (0xFFu << 24) | ((uint32_t)ob << 16) | ((uint32_t)og << 8) | or_;
        }
    }
    gfx_rect_outline(fb, bx, by, box_w, box_h, 2, C_BORDER_SEL);

    /* 提示文本 (居中) */
    int tx = bx + (box_w - msg_len * (font_h - 4)) / 2;
    int ty = by + (box_h - font_h) / 2 - 2;
    draw_text(fb, tx, ty, font_h, g_toast_msg, g_toast_color);
}

/* 从存档头部 0x04 提取 UTF-16LE 剧情任务标题及游戏时间戳 */
static void extract_save_title(const uint8_t *header, char *out_title, size_t max_len) {
    if (!header || !out_title || max_len == 0) return;
    out_title[0] = '\0';
    size_t di = 0;
    for (size_t i = 4; i + 1 < 0x104 && di + 4 < max_len; i += 2) {
        uint16_t u = (uint16_t)header[i] | ((uint16_t)header[i+1] << 8);
        if (u == 0) break;
        if (u < 0x80) {
            out_title[di++] = (char)u;
        } else if (u < 0x800) {
            out_title[di++] = (char)(0xC0 | (u >> 6));
            out_title[di++] = (char)(0x80 | (u & 0x3F));
        } else {
            out_title[di++] = (char)(0xE0 | (u >> 12));
            out_title[di++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out_title[di++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out_title[di] = '\0';
}

/* ========================================================================= */
/* 角色及属性定义                                                            */
/* ========================================================================= */
static const char *CHAR_NAMES_CN[3] = { "富兰克林", "麦克", "崔佛" };
/* ★ v6.1: 英文名/技能行为中文名两个数组随「存档修改」页一并失去调用点。
 *   保留数据（将来若恢复存档编辑可直接复用），用 unused 标注消除编译警告。 */
static const char *CHAR_NAMES_EN[3] __attribute__((unused)) = { "Franklin", "Michael", "Trevor" };
static const int   CHAR_SP_INDEX[3] = { 1, 0, 2 }; /* Franklin=SP1, Michael=SP0, Trevor=SP2 */

#define NUM_ATTRS 9
static const char *ATTR_NAMES[NUM_ATTRS] = {
    "现金 (Cash)",
    "体力 (Stamina)",
    "射击 (Shooting)",
    "力量 (Strength)",
    "潜行 (Stealth)",
    "飞行 (Flying)",
    "驾驶 (Driving)",
    "肺活量 (Lung)",
    "特殊能力 (Special)"
};

/* 8 大技能对应的 4 字节哈希特征 (针对 Franklin=sp1, Michael=sp0, Trevor=sp2) */
static const uint8_t SKILL_HASHES[3][8][4] = {
    /* Franklin (sp1_*) */
    {
        { 0x25, 0x5E, 0xFF, 0xB5 }, /* sp1_stamina */
        { 0xCB, 0x26, 0x14, 0x97 }, /* sp1_shooting_ability */
        { 0xB8, 0x28, 0x74, 0xE3 }, /* sp1_strength */
        { 0xE7, 0x6D, 0x0C, 0x23 }, /* sp1_stealth_ability */
        { 0xE9, 0x8B, 0xEE, 0x3D }, /* sp1_flying_ability */
        { 0x7D, 0xD8, 0x0A, 0xC8 }, /* sp1_wheelie_ability */
        { 0x6C, 0x3B, 0xBB, 0x1A }, /* sp1_lung_capacity */
        { 0x51, 0xCC, 0xFB, 0xA3 }, /* sp1_special_ability_unlocked */
    },
    /* Michael (sp0_*) */
    {
        { 0x22, 0xC8, 0xAA, 0xA2 }, /* sp0_stamina */
        { 0xB4, 0x89, 0x27, 0x09 }, /* sp0_shooting_ability */
        { 0x90, 0x6B, 0x27, 0x99 }, /* sp0_strength */
        { 0x22, 0x68, 0xB7, 0x91 }, /* sp0_stealth_ability */
        { 0x78, 0xAB, 0xE4, 0xE6 }, /* sp0_flying_ability */
        { 0x11, 0xB4, 0x72, 0x70 }, /* sp0_wheelie_ability */
        { 0x73, 0x96, 0x8E, 0xBD }, /* sp0_lung_capacity */
        { 0x4E, 0xCD, 0x9F, 0x81 }, /* sp0_special_ability_unlocked */
    },
    /* Trevor (sp2_*) */
    {
        { 0x7D, 0x82, 0x46, 0xAE }, /* sp2_stamina */
        { 0x2A, 0x3A, 0x74, 0xEA }, /* sp2_shooting_ability */
        { 0x4F, 0x19, 0xE1, 0x59 }, /* sp2_strength */
        { 0xD0, 0x3B, 0x7E, 0xEB }, /* sp2_stealth_ability */
        { 0x77, 0xCF, 0x97, 0x10 }, /* sp2_flying_ability */
        { 0x6B, 0xEF, 0x59, 0x2F }, /* sp2_wheelie_ability */
        { 0x7E, 0x94, 0x87, 0xB3 }, /* sp2_lung_capacity */
        { 0x05, 0xB0, 0x64, 0x42 }, /* sp2_special_ability_unlocked */
    }
};

/* ---------------------------------------------------------------------------
 * GTA V stats_controller.ysc 每 2 秒根据"行为统计"重算角色技能值并覆盖存档。
 * 因此仅修改 spX_*_ability 会被游戏重算覆盖（特殊能力不参与重算，故生效）。
 * 终极方案：保存时按游戏公式【反算】行为统计目标值写入，
 *          使游戏重算公式恒得出我们设定的技能值（0~100 精确生效）。
 *
 * 公式 (来自 stats_controller.c func_60/func_55, 数值为浮点/整数统计):
 *   1 体力  spX_stamina         = base + dist_running/175 + 游泳分钟 + 骑车分钟
 *   2 射击  spX_shooting_ability= base + hits_mission/40 + (hits_peds_vehicles - hits_mission)/80
 *   3 力量  spX_strength        = base + unarmed_hits/20
 *   4 潜行  spX_stealth_ability = base + dist_walk_st/45 + kills_stealth/2*1.5
 *   5 飞行  spX_flying_ability  = base + 飞机分钟/10 + 直升机分钟/10 + plane_landings
 *   6 驾驶  spX_wheelie_ability = base + number_near_miss/50   (base 恒 0)
 *   7 肺活量 spX_lung_capacity  = base + 水下秒数/30
 *   (base: Franklin=SP1[51,24,49,21,19,0,46] Michael=SP0[47,79,21,81,31,0,22]
 *          Trevor=SP2[23,69,79,49,82,0,28], 顺序同技能 1~7)
 *
 * 注意: 游泳/骑车/飞行/直升机/水下 的统计是"毫秒"(func_66/65 转分钟/秒)，
 *       dist_running/dist_walk_st 是 float 米数，int 统计直接反算整数。
 *
 * 写入策略: 主统计按 (目标值 - base) * 除数 反算（>=100 目标时封顶防溢出），
 *           同一技能的次要统计字段清零(否则会叠加产生偏差)，
 *           *_maxed 标记不参与逻辑判断(仅记录满级时间戳)，无需写入。
 * ------------------------------------------------------------------------- */
#define SKILL_BEHAVIOR_CNT 3   /* 每项技能最多 3 个行为统计 */

/* 行为统计字段: hash 为该角色该技能的真实 JOOAT(已用真实存档验证存在),
 *               base 为公式基础值, divisor 为除数, unit 为写入单位:
 *                 0=普通数值(米/次数, 直接写入)
 *                 1=TIME 分钟型 (func_66 取 S/60 分钟, 写入 目标*60 秒)
 *                 2=TIME 秒型   (func_65 取 S 秒, 直接写入秒) */
typedef struct {
    const uint8_t hash[4];
    int   is_primary;          /* 1=主统计(反算目标值), 0=次要统计(清零) */
    int   is_float;            /* 1=float 型, 0=int 型 */
    int   unit;                /* 0/1/2 见上 */
    float divisor;
    int   base;
} BehaviorField;

/* 行为统计中文名与单位 (UI 显示用), 索引同 SKILL_BEHAVIORS[技能1~7][字段0~2]
 * ★ v6.1: 随「存档修改」页失去调用点，保留数据 + unused 标注消除警告 */
static const char *SKILL_BEHAVIOR_CN[8][SKILL_BEHAVIOR_CNT] __attribute__((unused)) = {
    /* 1 体力 */
    { "跑步距离(m)", "游泳(分钟)", "骑车(分钟)" },
    /* 2 射击 */
    { "任务命中(次)", "命中人车(次)", "" },
    /* 3 力量 */
    { "徒手攻击(次)", "", "" },
    /* 4 潜行 */
    { "潜行移动(m)", "潜行击杀(次)", "" },
    /* 5 飞行 */
    { "开飞机(分钟)", "开直升机(分钟)", "飞行降落(次)" },
    /* 6 驾驶 */
    { "惊险超车(次)", "", "" },
    /* 7 肺活量 */
    { "水下时长(秒)", "", "" },
    /* 8 特殊能力占位 */
    { "", "", "" }
};

/* 每项技能的行为统计定义 [技能1~7][角色0~2(对应SP1/SP0/SP2)][最多3字段] */
static const BehaviorField SKILL_BEHAVIORS[8][3][SKILL_BEHAVIOR_CNT] = {
    /* 1 体力 Stamina: dist_running/175 + 游泳分钟 + 骑车分钟 */
    {
        { { {0xA2,0x45,0x39,0x0A}, 1, 1, 0, 175.0f,  51 },   /* sp1_dist_running */
          { {0x85,0xB2,0x0F,0x46}, 0, 1, 1, 1.0f,    0 },   /* sp1_time_swimming */
          { {0xC4,0x21,0xEB,0x13}, 0, 1, 1, 1.0f,    0 } }, /* sp1_time_driving_bicycle */
        { { {0x9B,0x0D,0x6F,0x2B}, 1, 1, 0, 175.0f,  47 },   /* sp0_dist_running */
          { {0x00,0xFC,0x00,0x47}, 0, 1, 1, 1.0f,    0 },   /* sp0_time_swimming */
          { {0x4B,0xEA,0xE1,0xEA}, 0, 1, 1, 1.0f,    0 } }, /* sp0_time_driving_bicycle */
        { { {0x17,0xB8,0x9D,0xAB}, 1, 1, 0, 175.0f,  23 },   /* sp2_dist_running */
          { {0xE3,0xA5,0x1E,0x94}, 0, 1, 1, 1.0f,    0 },   /* sp2_time_swimming */
          { {0x4D,0x21,0x85,0xC6}, 0, 1, 1, 1.0f,    0 } }, /* sp2_time_driving_bicycle */
    },
    /* 2 射击 Shooting: hits_mission/40 + (hits_peds_vehicles-hits_mission)/80 */
    {
        { { {0x7A,0x72,0x8D,0xFF}, 1, 0, 0, 40.0f,  24 },   /* sp1_hits_mission */
          { {0xC4,0x54,0x79,0xD2}, 0, 0, 0, 80.0f,   0 },   /* sp1_hits_peds_vehicles */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0xE3,0xF0,0x6A,0x75}, 1, 0, 0, 40.0f,  79 },   /* sp0_hits_mission */
          { {0xA0,0x8E,0xD8,0xBC}, 0, 0, 0, 80.0f,   0 },   /* sp0_hits_peds_vehicles */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x41,0xDB,0xDD,0xD5}, 1, 0, 0, 40.0f,  69 },   /* sp2_hits_mission */
          { {0xBE,0xFC,0x84,0x5D}, 0, 0, 0, 80.0f,   0 },   /* sp2_hits_peds_vehicles */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    },
    /* 3 力量 Strength: unarmed_hits/20 */
    {
        { { {0x54,0x6D,0xBF,0xC0}, 1, 0, 0, 20.0f,  49 },   /* sp1_unarmed_hits */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0xB3,0x9C,0x10,0xA1}, 1, 0, 0, 20.0f,  21 },   /* sp0_unarmed_hits */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x5C,0xA0,0x18,0x2D}, 1, 0, 0, 20.0f,  79 },   /* sp2_unarmed_hits */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    },
    /* 4 潜行 Stealth: dist_walk_st/45 + kills_stealth/2*1.5 */
    {
        { { {0x26,0x96,0x52,0xAD}, 1, 1, 0, 45.0f,  21 },   /* sp1_dist_walk_st */
          { {0x78,0x22,0xDF,0x05}, 0, 0, 0, 2.0f,    0 },   /* sp1_kills_stealth (除数2, 乘1.5) */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x2C,0xFE,0xBE,0x25}, 1, 1, 0, 45.0f,  81 },   /* sp0_dist_walk_st */
          { {0x50,0x24,0xF4,0x60}, 0, 0, 0, 2.0f,    0 },   /* sp0_kills_stealth */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x01,0xD4,0x0E,0xF6}, 1, 1, 0, 45.0f,  49 },   /* sp2_dist_walk_st */
          { {0x2C,0x9E,0x9F,0xC1}, 0, 0, 0, 2.0f,    0 },   /* sp2_kills_stealth */
          { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    },
    /* 5 飞行 Flying: 飞机分钟/10 + 直升机分钟/10 + plane_landings */
    {
        { { {0x90,0x09,0x83,0xE9}, 1, 1, 1, 10.0f,  19 },   /* sp1_time_driving_plane */
          { {0xE3,0x01,0xA6,0x90}, 0, 1, 1, 10.0f,   0 },   /* sp1_time_driving_heli */
          { {0x61,0x4D,0xB6,0xDF}, 0, 0, 0, 1.0f,    0 } }, /* sp1_plane_landings */
        { { {0x69,0x03,0x91,0x80}, 1, 1, 1, 10.0f,  31 },   /* sp0_time_driving_plane */
          { {0xE2,0x75,0xA1,0xBF}, 0, 1, 1, 10.0f,   0 },   /* sp0_time_driving_heli */
          { {0xA2,0x73,0x42,0xA6}, 0, 0, 0, 1.0f,    0 } }, /* sp0_plane_landings */
        { { {0xC8,0xA3,0xA6,0x87}, 1, 1, 1, 10.0f,  82 },   /* sp2_time_driving_plane */
          { {0xC4,0xDF,0x93,0x2B}, 0, 1, 1, 10.0f,   0 },   /* sp2_time_driving_heli */
          { {0xA6,0xFA,0x09,0xE0}, 0, 0, 0, 1.0f,    0 } }, /* sp2_plane_landings */
    },
    /* 6 驾驶 Driving: number_near_miss/50 (base 恒 0) */
    {
        { { {0x03,0x34,0xD0,0x43}, 1, 0, 0, 50.0f,   0 },   /* sp1_number_near_miss */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0xE8,0x83,0xA6,0x1F}, 1, 0, 0, 50.0f,   0 },   /* sp0_number_near_miss */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x3B,0x9A,0xE2,0xC6}, 1, 0, 0, 50.0f,   0 },   /* sp2_number_near_miss */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    },
    /* 7 肺活量 Lung: 水下秒数/30 */
    {
        { { {0x6B,0x56,0xE5,0x40}, 1, 1, 2, 30.0f,  46 },   /* sp1_time_underwater */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x62,0x13,0x78,0x30}, 1, 1, 2, 30.0f,  22 },   /* sp0_time_underwater */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
        { { {0x70,0x39,0x93,0xD3}, 1, 1, 2, 30.0f,  28 },   /* sp2_time_underwater */
          { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    },
    /* 8 特殊能力 (不参与重算, 占位) */
    { { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    { { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
    { { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 }, { {0,0,0,0}, 0, 0, 0, 0, 0 } },
};

/* 行为统计写入上限 (极大值需压过 stats_controller 的隐藏基线/叠加项,
 * 保证重算恒 >=100 被游戏封顶; 1000万 量级远大于任何正常游戏积累) */
#define BEHAVIOR_MAX_INT   10000000u    /* int 型统计 */
#define BEHAVIOR_MAX_FLOAT 10000000.0f  /* float 型统计 */
#define BEHAVIOR_MAX_SEC   10000000u    /* TIME 秒型统计上限 (约115天) */

/* 金钱步进档位 (1千 ~ 1000万) */
static const uint32_t CASH_STEPS[] = { 1000, 10000, 100000, 1000000, 10000000 };
static const char *CASH_STEP_NAMES[] = { "1千", "1万", "10万", "100万", "1000万" };
#define NUM_CASH_STEPS 5

/* 技能属性步进档位 (1 ~ 20点) */
static const uint32_t SKILL_STEPS[] = { 1, 5, 10, 20 };
static const char *SKILL_STEP_NAMES[] = { "+1", "+5", "+10", "+20" };
#define NUM_SKILL_STEPS 4

/* 5 大内置进度存档 (打包在 romfs 中) */
typedef struct {
    const char *name;
    const char *desc;
    const char *path;
} PresetSave;

static const PresetSave PRESET_SAVES[] = {
    { "4% 序章初入洛圣都",   "富兰克林初始阶段，刚完成偷车差事", "romfs:/preset_saves/4%/SGTA50000" },
    { "20% 崔佛登场",        "麦克崔佛汇合，已开启大地图多数区域", "romfs:/preset_saves/20%/SGTA50000" },
    { "31.6% 佩立托银行前夕", "三人组队作战，军火与准备工作完备", "romfs:/preset_saves/31.6%/SGTA50000" },
    { "61.1% 终局大干一票",   "联合储蓄大劫案前夜，全技能与载具充沛", "romfs:/preset_saves/61.1%/SGTA50000" },
    { "100% 完美全通关",     "全主线金牌+陌生人怪咖100%通关存档", "romfs:/preset_saves/100%/SGTA50001" },
};
#define NUM_PRESETS (sizeof(PRESET_SAVES)/sizeof(PRESET_SAVES[0]))

/* ========================================================================= */
/* 画质设置 (GTA V settings.xml)                                             */
/* ========================================================================= */
/* ★ v4 修正: 原值 40 会导致 settings.xml 里第 41 项之后被静默丢弃
 *   (实测该文件有 60 个含 value= 的字段) ⇒ 提高到 96 留余量 */
#define GFX_MAX_ITEMS 96
typedef struct {
    char name[48];       /* 字段名, 如 ShadowQuality */
    char cn[32];         /* 中文名, 如 阴影质量 */
    char value[16];      /* 原始值字符串, 如 "1" / "0.500000" / "true" */
    int  is_float;       /* 1=浮点步进, 0=整数/枚举 */
    int  is_bool;        /* 1=布尔(true/false) */
    int  lo, hi;         /* 整数/枚举范围 */
    float flo, fhi;      /* 浮点范围 */
    float fstep;         /* 浮点步进 */
    int  is_key;         /* 1=关键画质项(加粗高亮) */
} GfxItem;

typedef struct {
    const char *name;
    const char *desc;
    const char *path;
} GfxPreset;

/* 内置画质预设 (打包在 romfs 中)
 * ★ 前两个是「提帧专项」--针对实测发现的两个浪费点：
 *   (1) VSync 在低帧率下会把帧率量化到刷新率整数分之一
 *      （60Hz + 25FPS 原生 -> 锁到 20FPS，帧时间 33ms->50ms 跳变）
 *   (2) 低画质预设里 ShaderQuality / SSAO / 各向异性 / 抗锯齿 从未压到 0
 */
static const GfxPreset GFX_PRESETS[] = {
    { "★关VSync(提帧)",      "只关垂直同步, 其余不动。低帧率下可解除帧率量化", "romfs:/preset_gfx/★关VSync（只改这一项）/settings.xml" },
    { "★★关VSync+全压零",    "关VSync + 所有项压到最低。终极低画质", "romfs:/preset_gfx/★★关VSync+全压零（终极低画质）/settings.xml" },
    { "低画质(默频)",        "最低特效, 适合默认频率流畅运行", "romfs:/preset_gfx/低画质（默频使用）/settings.xml" },
    { "低画质+阴影",         "低特效保留阴影, 画面更有层次",   "romfs:/preset_gfx/低画质加阴影settings.xml" },
    { "中画质(默认)",        "官方默认画质, 均衡流畅",         "romfs:/preset_gfx/中画质（默认画质）/settings.xml" },
    { "高画质(极限超频)",    "全特效拉满, 仅限极限超频使用",   "romfs:/preset_gfx/高画质（极限超频专用）/settings.xml" },
};
#define NUM_GFX_PRESETS (sizeof(GFX_PRESETS)/sizeof(GFX_PRESETS[0]))

/* 画质全局状态 */
static GfxItem g_gfx_items[GFX_MAX_ITEMS];
static int     g_gfx_count = 0;          /* 当前解析出的项数 */
static int     g_gfx_dirty = 0;          /* 画质有未保存修改 */
static char    g_gfx_path[512] = {0};    /* 当前画质文件路径 */
static char    g_gfx_cur_idx = 0;        /* 当前选中画质项 */

/* ★ v4: 页签系统 (0=存档 1=画质 2=工具) -- 取代原 g_gfx_show 的"覆盖式"页面 */
/* ★ v6 (2026-10-01): 页签逻辑 ID 重排 —— 移除「存档修改」后编号连续。
 *   ❗ 用户要求移除「存档修改」页（金钱/技能编辑），但「内置存档库」
 *      （5 个剧情进度存档，ZL 键 / 工具信息页）**完全保留**。
 *   ❗ TAB_SAVE 不再出现在 TABS[] 里，但为兼容残留的条件判断，
 *      仍保留其宏定义为 -1（任何 g_tab 都不等于它 ⇒ 相关分支自然失效）。 */
#define TAB_SAVE (-1)       /* ★ v6: 已移除（保留宏以免旧引用编译失败） */
#define TAB_DLC  0          /* DLC 管理（首屏） */
#define TAB_SCRIPT 1        /* 脚本 mod（.nsc） */
#define TAB_GFX  2          /* 画质设置 */
#define TAB_PERF 3          /* 性能调参（gameconfig.xml） */
#define TAB_TOOL 4          /* 工具信息 */
#define NUM_TABS 5          /* ★ v6: 6 -> 5 */

/* ★★ 页签唯一定义源：顺序 = 显示顺序。
 * 以前 TAB_TOOL=2 / TAB_PERF=3 与 TAB_NAME[] 的下标不一致，
 * 导致「点工具信息却显示性能调参」（编号与位置错位）。
 * 现在名字/ID/动作三者绑在一起，不可能再错位。
 * ★★ v25: DLC 管理提升为主要功能 ⇒ 放第 1 位，启动默认就在这页。
 * ★★ v6: 移除「存档修改」页（用户要求），编号重排为 0..4。 */
typedef struct { const char *name; int id; int action; } TabDef;
/* ★ v6.3: 双语两份表（static 初始化器里不能用运行时 TR()，编译期常量才行） */
static const TabDef TABS_ZH[NUM_TABS] = {
    { "DLC管理",  TAB_DLC,    ACT_TAB_DLC    },
    { "脚本MOD",  TAB_SCRIPT, ACT_TAB_SCRIPT },
    { "画质设置", TAB_GFX,    ACT_TAB_GFX    },
    { "性能调参", TAB_PERF,   ACT_TAB_PERF   },
    { "工具信息", TAB_TOOL,   ACT_TAB_TOOL   },
};
static const TabDef TABS_EN[NUM_TABS] = {
    { "DLC",      TAB_DLC,    ACT_TAB_DLC    },
    { "Scripts",  TAB_SCRIPT, ACT_TAB_SCRIPT },
    { "Graphics", TAB_GFX,    ACT_TAB_GFX    },
    { "Perf",     TAB_PERF,   ACT_TAB_PERF   },
    { "Tools",    TAB_TOOL,   ACT_TAB_TOOL   },
};
#define TABS (g_lang_en ? TABS_EN : TABS_ZH)

/* ★ v25: 默认页 = DLC 管理（主要功能），进去后自动扫描在 ACT_TAB_DLC 里做 */
static int g_tab = TAB_DLC;

/* ★ v4: 画质列表滚动偏移 (60 项一屏放不下, 需要滚动) */
static int g_gfx_scroll = 0;
/* ★ v4: 底部大号 −/+ 按住连发 */
static int g_big_hold_dir = 0;      /* 1=+, -1=-, 0=无 */
static int g_fast_scroll = 0;       /* ★ v28 右摇杆快速滚动剩余行数；★ v31 右摇杆并入左摇杆导航 ⇒ 恒为 0（保留管道便于恢复） */
static int g_big_hold_tick = 0;

/* ========================================================================= */
/* ★ v6: 性能调参页 (gameconfig.xml)                                          */
/* ========================================================================= */
/* 设计: 直接编辑 update.rpf 里 /common/data/gameconfig.xml 的 switch 段，
 *       用纯字节原位替换写回（不复制 889 MB、不动其它条目）。
 * 流程: 打开 rpf -> inflate -> 改 switch 段 -> deflate -> 写回原偏移 + patch TOC */
static GcRpf  g_gc_rpf;
static int    g_gc_opened   = 0;    /* rpf 是否已打开 */
static int    g_gc_err      = 0;    /* 打开失败原因 */
static u8    *g_gc_xml      = NULL; /* gameconfig.xml 内容（可编辑） */
static size_t g_gc_xml_len  = 0;
static size_t g_gc_xml_cap  = 0;
static size_t g_gc_sw_a = 0, g_gc_sw_b = 0;     /* switch 段区间 */
static size_t g_gc_any_a = 0, g_gc_any_b = 0;   /* Any 段区间（只读对比） */
static int    g_gc_n_switch = 0;
static int    g_gc_n_any    = 0;
static GcEntry g_gc_ent;                        /* 目标条目 */
static int    g_gc_preset   = -1;               /* 选中的预设（保留给「恢复原版」用） */
static int    g_gc_dirty    = 0;                /* 有未应用改动 */
static int    g_gc_knob     = 0;                /* 手动调整：当前旋钮 */
static int    g_gc_knob_scr = 0;                /* 手动调整：滚动偏移 */
static int    g_gc_orig_on_disk = 0;            /* 原始 on_disk（回滚用） */
static u8    *g_gc_orig_blob = NULL;            /* 原始压缩块（备份用） */
static size_t g_gc_orig_len  = 0;
static char   g_gc_msg[192] = {0};
static uint32_t g_gc_msg_color = C_TEXT_MUTED;
static int    g_gc_confirm  = 0;                /* 1=写入确认弹窗 2=恢复原版确认 */

/* 手动调整时被改过的旋钮（记录原值以便显示"已改"） */
#define GC_MAX_EDITED 128
static char g_gc_edited_key[GC_MAX_EDITED][48];
static int  g_gc_edited_val[GC_MAX_EDITED];
static int  g_gc_edited_n = 0;

static void gc_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_gc_msg, sizeof(g_gc_msg), fmt, ap);
    va_end(ap);
}

/* ★ v24: 权重分组跳转（原 L/R 的功能，L/R 收归全局换页后移到按钮栏）
 *   分组 = weight 1(S金) / 2(A蓝) / 3(B灰) / 4(C红勿调) */
static void gc_group_jump(int dir) {
    if (g_gc_knob < 0) g_gc_knob = 0;
    if (g_gc_knob >= GC_NUM_KNOB_META) g_gc_knob = GC_NUM_KNOB_META - 1;
    int w = GC_KNOB_META[g_gc_knob].weight;
    int t = g_gc_knob;
    if (dir < 0) {
        while (t > 0 && GC_KNOB_META[t - 1].weight == w) t--;
        if (t == g_gc_knob) {                       /* 已在组首 -> 跳上一组组首 */
            while (t > 0 && GC_KNOB_META[t - 1].weight != w) t--;
            while (t > 0 && GC_KNOB_META[t - 1].weight == GC_KNOB_META[t].weight) t--;
        }
    } else {
        while (t + 1 < GC_NUM_KNOB_META && GC_KNOB_META[t + 1].weight == w) t++;
        if (t + 1 < GC_NUM_KNOB_META) t++;          /* 跳下一组组首 */
    }
    g_gc_knob = t;
    if (g_gc_knob < g_gc_knob_scr) g_gc_knob_scr = g_gc_knob;
    if (g_gc_knob >= g_gc_knob_scr + GC_KNOB_VIS)
        g_gc_knob_scr = g_gc_knob - GC_KNOB_VIS + 1;
    if (g_gc_knob_scr < 0) g_gc_knob_scr = 0;
}

/* 打开失败的详细诊断（每个候选路径都探测一遍，显示真实原因） */
static char g_gc_diag[3][256];      /* 每个候选路径的诊断行 */
static int  g_gc_diag_n = 0;

/* ★ 自检报告（点「自检」按钮生成） */
static char g_gc_selftest[2048] = {0};
static int  g_gc_selftest_show = 0;

static void gc_build_diag(void) {
    g_gc_diag_n = 0;
    for (int i = 0; i < GC_NUM_CANDIDATES && g_gc_diag_n < 3; i++) {
        const char *p = gc_rpf_candidates[i];
        FILE *f = fopen(p, "rb");
        if (!f) {
            snprintf(g_gc_diag[g_gc_diag_n++], 256, "打不开: %s", p);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        snprintf(g_gc_diag[g_gc_diag_n++], 256, "存在 %ld B: %s", sz, p);
    }
    if (g_gc_diag_n == 0) {
        snprintf(g_gc_diag[g_gc_diag_n++], 256, "(无候选路径)");
    }
}

/* 固定候选画质路径 - 已清空: 实机 MD5 诊断确认游戏真实读取位置 = 存档同目录
 * (原生挂载=save:/settings.xml), 画质与存档写入同一目录即可, 无需其他固定路径。
 * 保留该数组仅为兼容旧代码引用; 写入完全依赖 g_save_dir/settings.xml */
static const char *gfx_all_candidates[] = {
    "",
};
/* 路径自检结果展示 (画质页 X 键), 前 14 条可读写/存在/不存在 */
static char g_check_paths[14][160];
static int  g_check_paths_count = 0;

/* /switch/gta5save/ 下可导入的画质文件列表 (支持多个不同名字的 xml) */
#define MAX_GFX_SWITCH_FILES 16
static char g_gfx_switch_files[MAX_GFX_SWITCH_FILES][256];
static int  g_gfx_switch_count = 0;

/* 扫描 /switch/gta5save/ 目录下所有 *.xml 画质文件 (多个不同名字) */
static void scan_switch_gfx_files(void) {
    g_gfx_switch_count = 0;
    const char *base = "sdmc:/switch/gta5save";
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_gfx_switch_count < MAX_GFX_SWITCH_FILES) {
        if (e->d_name[0] == '.') continue;
        /* 匹配 *.xml (不区分大小写), 排除非画质文件 */
        size_t len = strlen(e->d_name);
        if (len < 5) continue;
        if (strncasecmp(e->d_name + len - 4, ".xml", 4) != 0) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", base, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 100) {
            snprintf(g_gfx_switch_files[g_gfx_switch_count], sizeof(g_gfx_switch_files[0]),
                     "%s", e->d_name);
            g_gfx_switch_count++;
        }
    }
    closedir(d);
}

/* 画质字段定义表 (按字段名匹配, 覆盖游戏主选项) */
typedef struct {
    const char *name;      /* settings.xml 字段名 */
    const char *cn;        /* 中文名 */
    int type;              /* 0=int枚举, 1=float, 2=bool */
    int lo, hi;            /* int 范围 */
    float flo, fhi, fstep; /* float 范围与步进 */
} GfxDef;

static const GfxDef GFX_DEFS[] = {
    { "ShadowQuality",          "阴影质量",   0, 0, 2, 0,0,0 },
    { "ReflectionQuality",      "反射质量",   0, 0, 3, 0,0,0 },
    { "TextureQuality",         "纹理质量",   0, 0, 2, 0,0,0 },
    { "WaterQuality",           "水面质量",   0, 0, 2, 0,0,0 },
    { "ParticleQuality",        "粒子质量",   0, 0, 2, 0,0,0 },
    { "GrassQuality",           "草地质量",   0, 0, 2, 0,0,0 },
    { "ShaderQuality",          "着色器质量", 0, 0, 2, 0,0,0 },
    { "Tessellation",           "曲面细分",   0, 0, 3, 0,0,0 },
    { "PostFX",                 "后期特效",   0, 0, 3, 0,0,0 },
    { "AnisotropicFiltering",   "各向异性过滤",0, 0, 16,0,0,0 },
    { "MSAA",                   "MSAA抗锯齿", 0, 0, 8, 0,0,0 },
    { "CityDensity",            "城市密度",   1, 0, 0, 0,1.0f,0.1f },
    { "PedVarietyMultiplier",   "行人多样度", 1, 0, 0, 0,1.0f,0.1f },
    { "VehicleVarietyMultiplier","载具多样度", 1, 0, 0, 0,1.0f,0.1f },
    { "LodScale",               "视野距离",   1, 0, 0, 0,2.0f,0.1f },
    { "MaxLodScale",            "最大视野距离",1, 0, 0, 0,2.0f,0.1f },
    { "SSAO",                   "环境光遮蔽", 2, 0, 0, 0,0,0 },
    { "DoF",                    "景深效果",   2, 0, 0, 0,0,0 },
    { "FXAA_Enabled",           "FXAA抗锯齿", 2, 0, 0, 0,0,0 },
    { "TXAA_Enabled",           "TXAA抗锯齿", 2, 0, 0, 0,0,0 },
    { "Shadow_SoftShadows",     "柔和阴影",   2, 0, 0, 0,0,0 },
    { "Shadow_LongShadows",     "长阴影",     2, 0, 0, 0,0,0 },
    { "UltraShadows_Enabled",   "极致阴影",   2, 0, 0, 0,0,0 },
    { "Lighting_FogVolumes",    "雾体积光照", 2, 0, 0, 0,0,0 },
    { "HdStreamingInFlight",    "飞行高清流", 2, 0, 0, 0,0,0 },
    { "MotionBlurStrength",     "动态模糊",   1, 0, 0, 0,1.0f,0.05f },
    { "ScreenWidth",            "分辨率宽",   0, 640, 3840, 0,0,0 },
    { "ScreenHeight",           "分辨率高",   0, 360, 2160, 0,0,0 },
    { "RefreshRate",            "刷新率",     0, 30, 120, 0,0,0 },
    { "VSync",                  "垂直同步",   0, 0, 1, 0,0,0 },
    { "Shadow_Distance",        "阴影距离",   1, 0, 0, 0,10.0f,0.1f },
    { "Shadow_SplitZStart",     "阴影分界起", 1, 0, 0, 0,1.0f,0.01f },
    { "Shadow_SplitZEnd",       "阴影分界终", 1, 0, 0, 0,1.0f,0.01f },
    { "Shadow_aircraftExpWeight","飞行阴影权重",1, 0, 0, 0,2.0f,0.01f },
    { "Convergence",            "3D会聚",     1, 0, 0, 0,1.0f,0.01f },
    { "Separation",             "3D分离",     1, 0, 0, 0,2.0f,0.01f },
    { "numBytesPerReplayBlock", "回放块大小", 0, 0, 0x7FFFFFFF, 0,0,0 },
    { "numReplayBlocks",        "回放块数量", 0, 0, 0x7FFFFFFF, 0,0,0 },
    { "maxSizeOfStreamingReplay","流式回放上限",0, 0, 0x7FFFFFFF, 0,0,0 },
    { "maxFileStoreSize",       "文件存储上限",0, 0, 0x7FFFFFFF, 0,0,0 },

    /* ★ v4 新增: 补齐原先缺失的 20 项中文名
     * (原先 GFX_DEFS 只有 40 条, 这些字段会走 gfx_parse_xml 的兜底分支
     *  直接显示英文原名) */
    { "version",                        "配置版本",     0, 0, 999, 0,0,0 },
    { "PedLodBias",                     "行人细节偏置", 1, 0, 0, 0,1.0f,0.05f },
    { "VehicleLodBias",                 "载具细节偏置", 1, 0, 0, 0,1.0f,0.05f },
    { "ReflectionMSAA",                 "反射抗锯齿",   0, 0, 8, 0,0,0 },
    { "MSAAFragments",                  "MSAA采样数",   0, 0, 8, 0,0,0 },
    { "MSAAQuality",                    "MSAA质量",     0, 0, 2, 0,0,0 },
    { "SamplingMode",                   "采样模式",     0, 0, 3, 0,0,0 },
    { "Shadow_ParticleShadows",         "粒子阴影",     2, 0, 0, 0,0,0 },
    { "Shadow_DisableScreenSizeCheck",  "禁用阴影尺寸检查",2, 0, 0, 0,0,0 },
    { "Reflection_MipBlur",             "反射模糊",     2, 0, 0, 0,0,0 },
    { "Shader_SSA",                     "屏幕空间环境光",2, 0, 0, 0,0,0 },
    { "DX_Version",                     "图形API版本",  0, 0, 2, 0,0,0 },
    { "AntiAliasing",                   "抗锯齿模式",   0, 0, 3, 0,0,0 },
    { "Audio3d",                        "3D音效",       2, 0, 0, 0,0,0 },
    { "AdapterIndex",                   "显示适配器",   0, 0, 8, 0,0,0 },
    { "OutputIndex",                    "输出端口",     0, 0, 8, 0,0,0 },
    { "Windowed",                       "窗口模式",     0, 0, 1, 0,0,0 },
    { "Stereo",                         "立体3D",       0, 0, 1, 0,0,0 },
    { "PauseOnFocusLoss",               "失焦暂停",     0, 0, 1, 0,0,0 },
    { "AspectRatio",                    "宽高比",       0, 0, 3, 0,0,0 },
};
#define NUM_GFX_DEFS (sizeof(GFX_DEFS)/sizeof(GFX_DEFS[0]))

/* 存档槽位 */
#define MAX_SLOTS 32
typedef struct {
    char filename[64];
    char fullpath[512];
    char title[128];        /* 从头部 0x04 提取的剧情标题及进度时间 */
    time_t mtime;
    size_t size;
} SaveSlotItem;

static SaveSlotItem g_slots[MAX_SLOTS];
static int          g_num_slots = 0;
static int          g_active_slot_idx = 0;

/* /switch/gta5save/ 历史备份 */
#define MAX_BACKUPS 48
typedef struct {
    char dir_name[64];
    char fullpath[512];
    char title[128];        /* 从备份存档头部提取的剧情标题及进度时间 */
    char note[128];
    time_t mtime;
    size_t size;
} BackupItem;

static BackupItem g_backups[MAX_BACKUPS];
static int        g_num_backups = 0;

static uint8_t *g_save_buf = NULL;
static size_t   g_save_sz = 0;
static size_t   g_start_off = 0;
static gta5_platform g_plat = GTA5_PLATFORM_PC;
static int      g_is_plain = 0;

static char     g_save_dir[512] = {0};
static char     g_save_path[512] = {0};
static char     g_current_title[128] = {0};
static char     g_user_name[64]  = "";   /* 自动识别玩家昵称, 不再硬编码默认值 */
static int      g_is_native_mount = 0;
static AccountUid g_current_uid = {0};

/* 核心数值：3位角色，每人9个属性 (0=金钱, 1~8=对应技能) */
static uint32_t g_values[3][NUM_ATTRS] = {{0}};
static int      g_found[3][NUM_ATTRS]  = {{0}};
static long     g_offsets[3][NUM_ATTRS] = {{-1}};

/* 技能行为统计偏移: [技能1~7][角色0~2(SP1/SP0/SP2)][字段0~2], -1=未找到 */
static long     g_behavior_offsets[8][3][SKILL_BEHAVIOR_CNT];
/* 技能行为统计当前值: [技能1~7][角色0~2][字段0~2] (解析时读取, 供UI显示) */
static double   g_behavior_values[8][3][SKILL_BEHAVIOR_CNT];
/* 技能行为统计是否找到: [技能1~7][角色0~2][字段0~2] */
static int      g_behavior_found[8][3][SKILL_BEHAVIOR_CNT];
/* 技能修改标记: [角色][技能1~7]=1 表示该技能被用户改过, 保存时同步行为统计 */
static int      g_skill_modified[3][8] = {{0}};

static int      g_cur_char = 0;        /* 0: 富兰克林, 1: 麦克, 2: 崔佛 (v24: 点顶部角色标签切换) */
static int      g_cur_attr = 0;        /* 0~8: 当前选择的属性项目 (按←/->切换) */
static int      g_cash_step_idx = 1;   /* 默认金额步长 1万 */
static int      g_skill_step_idx = 2;  /* 默认技能步长 10点 */
static int      g_dirty = 0;
static char     g_status_msg[256] = "";  /* ★ v6.3: 初始文案改为运行时按语言填 */
static uint32_t g_status_color = C_TEXT;

/* 弹窗模态: 0=主界面, 1=内置存档, 2=历史备份库, 3=当前目录槽位
 *           4=画质预设, 5=画质文件, 6=画质自检
 *   ★ v6.2 新增: 7=脚本管理（还原官方 + 内置模组）, 8=操作二次确认 */
static int g_modal_mode = 0;
static int g_modal_sel = 0;
static int g_modal_scr = 0;      /* ★ 弹窗列表滚动偏移（画质预设弹窗用） */

/* ★ v6.2: 脚本管理弹窗状态
 *   选中行沿用 g_modal_sel（0..2 = 还原官方 3 项；3..6 = 内置模组 4 项）
 *   g_mod_pending_* : 待二次确认的操作 */
#define MODE_SCRIPT_MGR     7
#define MODE_SCRIPT_CONFIRM 8
static int g_mod_pending_kind = 0;            /* 0 = 还原官方, 1 = 装内置模组 */
static int g_mod_pending_idx  = 0;            /* 对应的索引 */


/* ★ v6.3: 内置模组的 title/note 按语言选（_en 为 NULL 时回退中文） */
#define MOD_TITLE(m) ((m) && (m)->title_en && g_lang_en ? (m)->title_en : ((m) ? (m)->title : NULL))
#define MOD_NOTE(m)  ((m) && (m)->note_en  && g_lang_en ? (m)->note_en  : ((m) ? (m)->note  : NULL))

/* ★ v6.2: 大小格式化 —— 原来直接 `size / 1024` 做整数除法，
 *   导致 927 B 的 error_listener.nsc 显示成「0 KB」（用户实机反馈的 bug）。
 *   <1KB ⇒ 显示字节数；<1MB ⇒ 一位小数的 KB；否则一位小数的 MB。 */
static void fmt_size(char *out, size_t outsz, unsigned int bytes) {
    if (bytes < 1024u)
        snprintf(out, outsz, "%u B", bytes);
    else if (bytes < 1024u * 1024u)
        snprintf(out, outsz, "%u.%u KB", bytes / 1024u, (bytes % 1024u) * 10u / 1024u);
    else
        snprintf(out, outsz, "%u.%u MB", bytes / 1048576u, (bytes % 1048576u) * 10u / 1048576u);
}

static void ensure_dir(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* 自动时间戳备份 (保存在 /switch/gta5save/<时间戳>/) */
static int backup_current_save_timestamp(const char *reason) {
    if (!g_save_buf || g_save_sz == 0) return 0;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "backup_%" PRIu64, (uint64_t)now);
    }

    char backup_dir[512];
    snprintf(backup_dir, sizeof(backup_dir), "sdmc:/switch/gta5save/%s", time_str);
    ensure_dir(backup_dir);

    const char *fname = strrchr(g_save_path, '/');
    if (fname) fname++;
    else fname = "SGTA50000";

    char dest_file[512];
    snprintf(dest_file, sizeof(dest_file), "%s/%s", backup_dir, fname);

    FILE *fp = fopen(dest_file, "wb");
    if (fp) {
        fwrite(g_save_buf, 1, g_save_sz, fp);
        fclose(fp);

        char info_path[512];
        snprintf(info_path, sizeof(info_path), "%s/info.txt", backup_dir);
        FILE *ifp = fopen(info_path, "w");
        if (ifp) {
            fprintf(ifp, "%s | %s | %s | %s\n", time_str, reason, fname, g_current_title);
            fclose(ifp);
        }
        return 1;
    }
    return 0;
}

/* 扫描指定目录下的所有存档槽位并按修改时间倒序排列 (最新在前) */
static void scan_save_slots(const char *dir) {
    g_num_slots = 0;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_num_slots < MAX_SLOTS) {
        if (e->d_name[0] == '.') continue;
        if (strncasecmp(e->d_name, "SGTA5", 5) == 0 && !strstr(e->d_name, ".bak")) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 1024) {
                strncpy(g_slots[g_num_slots].filename, e->d_name, sizeof(g_slots[g_num_slots].filename) - 1);
                strncpy(g_slots[g_num_slots].fullpath, full, sizeof(g_slots[g_num_slots].fullpath) - 1);
                g_slots[g_num_slots].mtime = st.st_mtime;
                g_slots[g_num_slots].size = st.st_size;
                g_slots[g_num_slots].title[0] = '\0';

                /* 读取头部 0x04 提取剧情名称与游戏内时间戳 */
                FILE *tfp = fopen(full, "rb");
                if (tfp) {
                    uint8_t hdr[0x108];
                    if (fread(hdr, 1, 0x108, tfp) == 0x108) {
                        extract_save_title(hdr, g_slots[g_num_slots].title, sizeof(g_slots[g_num_slots].title));
                    }
                    fclose(tfp);
                }
                g_num_slots++;
            }
        }
    }
    closedir(d);

    /* 倒序冒泡排序：最新修改排最前 */
    for (int i = 0; i < g_num_slots - 1; i++) {
        for (int j = 0; j < g_num_slots - 1 - i; j++) {
            if (g_slots[j].mtime < g_slots[j+1].mtime) {
                SaveSlotItem tmp = g_slots[j];
                g_slots[j] = g_slots[j+1];
                g_slots[j+1] = tmp;
            }
        }
    }
}

/* 扫描 /switch/gta5save/ 下的历史备份 */
static void scan_backup_history(void) {
    g_num_backups = 0;
    const char *base = "sdmc:/switch/gta5save";
    DIR *d = opendir(base);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_num_backups < MAX_BACKUPS) {
        if (e->d_name[0] == '.') continue;
        char sub_dir[512];
        snprintf(sub_dir, sizeof(sub_dir), "%s/%s", base, e->d_name);
        struct stat st;
        if (stat(sub_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *sub_d = opendir(sub_dir);
            if (sub_d) {
                struct dirent *se;
                while ((se = readdir(sub_d)) != NULL) {
                    if (strncasecmp(se->d_name, "SGTA5", 5) == 0 && !strstr(se->d_name, ".bak")) {
                        char save_file[512];
                        snprintf(save_file, sizeof(save_file), "%s/%s", sub_dir, se->d_name);
                        struct stat sst;
                        if (stat(save_file, &sst) == 0 && sst.st_size > 1024) {
                            strncpy(g_backups[g_num_backups].dir_name, e->d_name, sizeof(g_backups[g_num_backups].dir_name) - 1);
                            strncpy(g_backups[g_num_backups].fullpath, save_file, sizeof(g_backups[g_num_backups].fullpath) - 1);
                            g_backups[g_num_backups].mtime = sst.st_mtime;
                            g_backups[g_num_backups].size = sst.st_size;
                            g_backups[g_num_backups].title[0] = '\0';

                            /* 从备份文件中提取原始剧情名称 */
                            FILE *bfp = fopen(save_file, "rb");
                            if (bfp) {
                                uint8_t bhdr[0x108];
                                if (fread(bhdr, 1, 0x108, bfp) == 0x108) {
                                    extract_save_title(bhdr, g_backups[g_num_backups].title, sizeof(g_backups[g_num_backups].title));
                                }
                                fclose(bfp);
                            }

                            char info_path[512];
                            snprintf(info_path, sizeof(info_path), "%s/info.txt", sub_dir);
                            FILE *ifp = fopen(info_path, "r");
                            if (ifp) {
                                if (fgets(g_backups[g_num_backups].note, sizeof(g_backups[g_num_backups].note), ifp)) {
                                    size_t nl = strlen(g_backups[g_num_backups].note);
                                    while (nl > 0 && (g_backups[g_num_backups].note[nl-1] == '\r' || g_backups[g_num_backups].note[nl-1] == '\n')) {
                                        g_backups[g_num_backups].note[--nl] = '\0';
                                    }
                                }
                                fclose(ifp);
                            } else {
                                snprintf(g_backups[g_num_backups].note, sizeof(g_backups[g_num_backups].note), "自动备份 (%s)", se->d_name);
                            }
                            g_num_backups++;
                            break;
                        }
                    }
                }
                closedir(sub_d);
            }
        }
    }
    closedir(d);

    for (int i = 0; i < g_num_backups - 1; i++) {
        for (int j = 0; j < g_num_backups - 1 - i; j++) {
            if (strcmp(g_backups[j].dir_name, g_backups[j+1].dir_name) < 0) {
                BackupItem tmp = g_backups[j];
                g_backups[j] = g_backups[j+1];
                g_backups[j+1] = tmp;
            }
        }
    }
}

static void get_nickname_for_uid(AccountUid uid, char *out_name, size_t max_len) {
    AccountProfile profile;
    if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase base;
        if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
            strncpy(out_name, base.nickname, max_len - 1);
            out_name[max_len - 1] = '\0';
        }
        accountProfileClose(&profile);
    }
}

/* 自动定位存档并默认选定最近存档 */
static int locate_and_select_save(void) {
    accountInitialize(AccountServiceType_Application);
    accountGetPreselectedUser(&g_current_uid);
    if (g_current_uid.uid[0] != 0 || g_current_uid.uid[1] != 0) {
        get_nickname_for_uid(g_current_uid, g_user_name, sizeof(g_user_name));
    }

    /* 1. DBI 原生模式：内部系统存档 */
    FsSaveDataInfoReader reader;
    if (R_SUCCEEDED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) {
        s64 total_entries = 0;
        FsSaveDataInfo info;
        while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &total_entries)) && total_entries > 0) {
            if (info.save_data_type == FsSaveDataType_Account && info.application_id == GTAV_TITLE_ID) {
                g_current_uid = info.uid;
                get_nickname_for_uid(info.uid, g_user_name, sizeof(g_user_name));

                fsdevUnmountDevice("save");
                if (R_SUCCEEDED(fsdevMountSaveData("save", GTAV_TITLE_ID, info.uid))) {
                    scan_save_slots("save:");
                    if (g_num_slots > 0) {
                        g_is_native_mount = 1;
                        strncpy(g_save_dir, "save:", sizeof(g_save_dir) - 1);
                        strncpy(g_save_path, g_slots[0].fullpath, sizeof(g_save_path) - 1);
                        g_active_slot_idx = 0;
                        fsSaveDataInfoReaderClose(&reader);
                        accountExit();
                        return 1;
                    }
                }
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }
    accountExit();

    /* 2. SD 卡目录精确扫描 (DBI 路径支持) */
    const char *BASE_FOLDERS[] = {
        "sdmc:/Installed games/Grand Theft Auto V",
        "/Installed games/Grand Theft Auto V",
        "sdmc:/Installed games/Grand Theft Auto 5",
        "sdmc:/Installed games/GTA V",
        "sdmc:/switch/DBI/saves/Grand Theft Auto V",
        "sdmc:/switch/DBI/saves/0100B00B51230000",
        "sdmc:/switch/Checkpoint/saves/0100B00B51230000",
        "sdmc:/JKSV/Grand Theft Auto V",
        "sdmc:/GTA5Money",
        NULL
    };

    for (int i = 0; BASE_FOLDERS[i]; i++) {
        DIR *d = opendir(BASE_FOLDERS[i]);
        if (!d) continue;

        scan_save_slots(BASE_FOLDERS[i]);
        if (g_num_slots > 0) {
            strncpy(g_save_dir, BASE_FOLDERS[i], sizeof(g_save_dir) - 1);
            strncpy(g_save_path, g_slots[0].fullpath, sizeof(g_save_path) - 1);
            g_active_slot_idx = 0;
            closedir(d);
            return 1;
        }

        struct dirent *sub;
        while ((sub = readdir(d)) != NULL) {
            if (sub->d_name[0] == '.') continue;
            char sub_path[512];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", BASE_FOLDERS[i], sub->d_name);

            struct stat st;
            if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                scan_save_slots(sub_path);
                if (g_num_slots > 0) {
                    strncpy(g_user_name, sub->d_name, sizeof(g_user_name) - 1);
                    strncpy(g_save_dir, sub_path, sizeof(g_save_dir) - 1);
                    strncpy(g_save_path, g_slots[0].fullpath, sizeof(g_save_path) - 1);
                    g_active_slot_idx = 0;
                    closedir(d);
                    return 1;
                }
            }
        }
        closedir(d);
    }

    /* 3. 未能从目录识别到用户名时, 兜底用系统预选用户昵称 (不显示默认占位名) */
    if (g_user_name[0] == '\0') {
        accountInitialize(AccountServiceType_Application);
        AccountUid preselected = {0};
        if (R_SUCCEEDED(accountGetPreselectedUser(&preselected)) &&
            (preselected.uid[0] != 0 || preselected.uid[1] != 0)) {
            get_nickname_for_uid(preselected, g_user_name, sizeof(g_user_name));
        }
        accountExit();
    }
    return 0;
}

/* ========================================================================= */
/* 画质设置功能: 解析/修改 settings.xml (GTA V Switch 移植版画质文件)       */
/* ========================================================================= */
/* 从一行 "<Name value="X" />" 提取 value 内容到 out (不含引号) */
static void xml_get_value(const char *line, char *out, size_t out_sz) {
    const char *v = strstr(line, "value=");
    if (!v) { out[0] = '\0'; return; }
    v += 6;
    if (*v == '"') v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < out_sz) out[i++] = *v++;
    out[i] = '\0';
}

/* 从一行 "<Name ..." 提取字段名到 out */
static void xml_get_name(const char *line, char *out, size_t out_sz) {
    const char *p = line;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p == '<') p++;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '>' && i + 1 < out_sz) out[i++] = *p++;
    out[i] = '\0';
}

/* 逐行遍历文件内容 (不依赖 strtok_r) */
static char *xml_next_line(char *buf, size_t buflen, size_t *pos) {
    if (*pos >= buflen) return NULL;
    char *line = buf + *pos;
    char *nl = memchr(line, '\n', buflen - *pos);
    if (nl) {
        *nl = '\0';
        /* 去掉行尾 \r */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\r')) { line[len-1] = '\0'; len--; }
        *pos = (size_t)(nl - buf) + 1;
    } else {
        *pos = buflen;
    }
    return line;
}

/* 前置声明: 多路径写入辅助函数 (定义在 gfx_save_to_file 之后) */
static int gfx_write_multi_paths(const char *buf, size_t rd);
static int gfx_load(void);

/* 将当前 g_gfx_items 解析结果写回所有候选 settings.xml 并保存 */
static int gfx_save_to_file(void) {
    if (g_gfx_count == 0) return -1;
    /* 源文件可能已被移动/删除(例如换过导入文件), 自动重新定位 */
    FILE *fp = NULL;
    if (g_gfx_path[0]) fp = fopen(g_gfx_path, "r");
    if (!fp) {
        if (gfx_load() != 0) {
            snprintf(g_status_msg, sizeof(g_status_msg), TR("画质保存失败! 未找到 settings.xml", "Save failed! settings.xml not found"));
            g_status_color = C_RED;
            return -1;
        }
        if (g_gfx_path[0]) fp = fopen(g_gfx_path, "r");
    }
    if (!fp) return -1;
    /* 读整个文件 */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(fp); return -1; }
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);

    /* 逐行替换已解析字段的值, 结果写入动态缓冲 */
    size_t out_cap = (size_t)sz * 2 + 4096;
    char *out_buf = (char*)malloc(out_cap);
    if (!out_buf) { free(buf); return -1; }
    size_t out_len = 0;
    size_t pos = 0;
    int replaced = 0;
    char *line;
    while ((line = xml_next_line(buf, rd, &pos)) != NULL) {
        char name[48];
        xml_get_name(line, name, sizeof(name));
        /* 查找该字段是否在解析结果中 */
        int found = -1;
        for (int i = 0; i < g_gfx_count; i++) {
            if (strcmp(name, g_gfx_items[i].name) == 0) { found = i; break; }
        }
        if (found >= 0 && strstr(line, "value=")) {
            /* 替换 value="..." */
            char *vpos = strstr(line, "value=");
            char *vstart = vpos + 6;
            if (*vstart == '"') vstart++;
            char *vend = strchr(vstart, '"');
            if (vend) {
                size_t new_len = strlen(g_gfx_items[found].value);
                size_t old_len = (size_t)(vend - vstart);
                size_t prefix = (size_t)(vstart - line);
                char newline[1024];
                int nl_len;
                if (new_len != old_len) {
                    /* 长度变化需重建该行 */
                    nl_len = snprintf(newline, sizeof(newline), "%.*s%s%s\n",
                                      (int)prefix, line, g_gfx_items[found].value, vend);
                } else {
                    memcpy(vstart, g_gfx_items[found].value, old_len);
                    nl_len = snprintf(newline, sizeof(newline), "%s\n", line);
                }
                if (nl_len > 0 && out_len + (size_t)nl_len < out_cap) {
                    memcpy(out_buf + out_len, newline, (size_t)nl_len);
                    out_len += (size_t)nl_len;
                }
                replaced++;
                continue;
            }
        }
        /* 普通行原样保留 */
        size_t llen = strlen(line);
        if (out_len + llen + 2 < out_cap) {
            memcpy(out_buf + out_len, line, llen);
            out_len += llen;
            out_buf[out_len++] = '\n';
        }
    }
    free(buf);
    if (replaced == 0) { free(out_buf); return -1; }

    /* 多路径写入 (含存档目录 / /switch/gta5save/ / SD根目录) */
    int wrote = gfx_write_multi_paths(out_buf, out_len);
    free(out_buf);
    return wrote > 0 ? 0 : -1;
}

/* 保存画质 (供 A 键/退出时调用; 返回0=成功) */
static int gfx_save(void) {
    if (!g_gfx_dirty) return 0;
    if (gfx_save_to_file() != 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), TR("画质保存失败! 请确认 settings.xml 可写", "Save failed! Make sure settings.xml is writable"));
        g_status_color = C_RED;
        return -1;
    }
    g_gfx_dirty = 0;
    snprintf(g_status_msg, sizeof(g_status_msg), TR("画质设置已保存到所有配置位置! 重启游戏生效", "Graphics saved to all config locations! Reboot to take effect"));
    g_status_color = C_GREEN;
    show_toast(TR("画质已保存", "Graphics saved"), C_GREEN);
    return 0;
}

/* 解析 XML 内容到 g_gfx_items (供 load / apply 共用) */
static int gfx_parse_xml(const char *buf, size_t rd) {
    g_gfx_count = 0;
    size_t pos = 0;
    char *line;
    char *tmp = (char*)malloc(rd + 1);
    if (!tmp) return -1;
    memcpy(tmp, buf, rd);
    tmp[rd] = '\0';
    while ((line = xml_next_line(tmp, rd, &pos)) != NULL && g_gfx_count < GFX_MAX_ITEMS) {
        char name[48];
        xml_get_name(line, name, sizeof(name));
        if (strstr(line, "value=") && name[0] && g_gfx_count < GFX_MAX_ITEMS) {
            GfxItem *it = &g_gfx_items[g_gfx_count];
            strncpy(it->name, name, sizeof(it->name) - 1);
            it->name[sizeof(it->name) - 1] = '\0';
            xml_get_value(line, it->value, sizeof(it->value));
            it->is_float = 0; it->is_bool = 0; it->is_key = 0;
            it->lo = 0; it->hi = 100; it->flo = 0; it->fhi = 1; it->fstep = 0.1f;
            strncpy(it->cn, name, sizeof(it->cn) - 1);
            it->cn[sizeof(it->cn) - 1] = '\0';
            if (strcmp(it->value, "true") == 0 || strcmp(it->value, "false") == 0) {
                it->is_bool = 1;
            } else if (strchr(it->value, '.')) {
                it->is_float = 1;
            }
            for (size_t d = 0; d < NUM_GFX_DEFS; d++) {
                if (strcmp(GFX_DEFS[d].name, name) == 0) {
                    strncpy(it->cn, GFX_DEFS[d].cn, sizeof(it->cn) - 1);
                    it->cn[sizeof(it->cn) - 1] = '\0';
                    it->is_float = (GFX_DEFS[d].type == 1);
                    it->is_bool  = (GFX_DEFS[d].type == 2);
                    it->lo = GFX_DEFS[d].lo; it->hi = GFX_DEFS[d].hi;
                    it->flo = GFX_DEFS[d].flo; it->fhi = GFX_DEFS[d].fhi;
                    it->fstep = GFX_DEFS[d].fstep;
                    it->is_key = 1;
                    break;
                }
            }
            g_gfx_count++;
        }
    }
    free(tmp);
    return 0;
}

/* 读取文件全部内容到 malloc 缓冲, 返回长度 (0=失败) */
static size_t gfx_read_all(const char *path, char **out_buf) {
    *out_buf = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(fp); return 0; }
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    *out_buf = buf;
    return rd;
}

/* 将内容 buf 写入多个候选画质路径 (最大化命中游戏真实读取位置)
 * 返回实际写入成功的路径数 */
/* 取一个路径的父目录 (去掉最后一个 / 之后的部分, 写入 dir_out) */
static void parent_dir(const char *path, char *dir_out, size_t sz) {
    strncpy(dir_out, path, sz - 1);
    dir_out[sz - 1] = '\0';
    char *s = strrchr(dir_out, '/');
    if (s && s != dir_out) *s = '\0';
}

/* 检查一个 settings.xml 候选路径的状态: 0=可读写 1=存在只读 2=不存在 3=不可写 */
static int gfx_path_state(const char *path) {
    FILE *t = fopen(path, "rb");
    if (t) { fclose(t); return 0; }  /* 文件存在且可读 */
    char parent[512];
    parent_dir(path, parent, sizeof(parent));
    DIR *d = opendir(parent);
    if (d) { closedir(d); return 1; } /* 父目录存在但文件不存在(可建) */
    return 2;                          /* 父目录不存在(可自动创建) */
}

/* 路径自检: 检查存档同目录 + 备份目录的 settings.xml 状态 */
static void gfx_check_paths(void) {
    int total = 0, ok = 0, ro = 0, no = 0;
    /* 候选: 存档同目录 (原生=save:/) + /switch/gta5save/ 备份 */
    char cands[3][512];
    int n = 0;
    if (g_save_dir[0]) {
        snprintf(cands[n++], sizeof(cands[0]), "%s/settings.xml", g_save_dir);
    }
    snprintf(cands[n++], sizeof(cands[0]), "sdmc:/switch/gta5save/settings.xml");
    snprintf(cands[n++], sizeof(cands[0]), "sdmc:/settings.xml");
    for (int i = 0; i < n && total < 14; i++) {
        const char *p = cands[i];
        total++;
        int st = gfx_path_state(p);
        if (st == 0) ok++;
        else if (st == 1) ro++;
        else if (st == 2) no++;
        snprintf(g_check_paths[total - 1], sizeof(g_check_paths[0]),
                 "%s : %s", p, st == 0 ? "可读写" : (st == 1 ? "不存在(可建)" : "目录不存在"));
    }
    g_check_paths_count = total;
    snprintf(g_status_msg, sizeof(g_status_msg),
             "路径自检完成: 存在 %d / 可新建 %d / 目录缺失 %d (共 %d 条)", ok, ro, no, total);
    g_status_color = (ok > 0) ? C_GREEN : C_RED;
    show_toast(TR("路径自检完成", "Path self-test done"), ok > 0 ? C_GREEN : C_RED);
}

static int gfx_write_multi_paths(const char *buf, size_t rd) {
    char cands[12][512];
    int n = 0;
    /* 1. 当前定位的画质文件 (存档同目录=save:/settings.xml, 即游戏真实读取位置) */
    if (g_gfx_path[0]) {
        snprintf(cands[n++], sizeof(cands[0]), "%s", g_gfx_path);
    }
    /* 2. 存档目录 (原生挂载=save:/, SD模式=存档目录) - 画质与存档同目录, 游戏必读 */
    if (g_save_dir[0]) {
        snprintf(cands[n++], sizeof(cands[0]), "%s/settings.xml", g_save_dir);
    }
    /* 3. 固定候选 (已精简为空, 见 gfx_all_candidates 注释) */
    for (int k = 0; k < (int)(sizeof(gfx_all_candidates)/sizeof(gfx_all_candidates[0])); k++) {
        if (gfx_all_candidates[k][0]) {
            snprintf(cands[n++], sizeof(cands[0]), "%s", gfx_all_candidates[k]);
        }
    }

    int wrote = 0;
    int mkfail = 0;
    for (int i = 0; i < n; i++) {
        /* 跳过重复 */
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(cands[i], cands[j]) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        /* 父目录不存在时自动创建 (fopen "w" 不会建目录) */
        char parent[512];
        parent_dir(cands[i], parent, sizeof(parent));
        if (parent[0] && strcmp(parent, cands[i]) != 0) {
            ensure_dir(parent);
        }
        FILE *ofp = fopen(cands[i], "w");
        if (ofp) {
            fwrite(buf, 1, rd, ofp);
            fclose(ofp);
            wrote++;
        } else {
            mkfail++;
        }
    }
    /* 原生挂载模式: 写入系统内部存档区后必须 commit 才会落盘 */
    if (g_is_native_mount) {
        fsdevCommitDevice("save");
    }
    if (wrote == 0 && mkfail > 0) {
        /* 全部路径都写失败: 记录详细日志供诊断 */
        FILE *lf = fopen("sdmc:/switch/gta5save/write_fail.log", "w");
        if (lf) {
            fprintf(lf, "gfx_write_multi_paths: 所有 %d 个候选路径写入失败!\n", n);
            for (int i = 0; i < n; i++) fprintf(lf, "  [%d] %s\n", i, cands[i]);
            fclose(lf);
        }
    }
    return wrote;
}

/* 应用一套画质预设 (整体覆盖所有候选 settings.xml) */
static int gfx_apply_preset(int idx) {
    if (idx < 0 || idx >= (int)NUM_GFX_PRESETS) return -1;
    char *buf = NULL;
    size_t rd = gfx_read_all(GFX_PRESETS[idx].path, &buf);
    if (!buf || rd == 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "无法打开内置画质: %s", GFX_PRESETS[idx].name);
        g_status_color = C_RED;
        return -1;
    }

    /* 1. 用预设内容整体覆盖所有候选 settings.xml (多路径写入确保游戏读到) */
    int applied_to_file = 0;
    applied_to_file = gfx_write_multi_paths(buf, rd);

    /* 2. 解析预设内容到缓冲供 UI 显示 */
    gfx_parse_xml(buf, rd);
    free(buf);

    if (applied_to_file > 0) {
        g_gfx_dirty = 0;
        snprintf(g_status_msg, sizeof(g_status_msg), "已应用【%s】画质并保存到 %d 个位置! 重启游戏生效", GFX_PRESETS[idx].name, applied_to_file);
        g_status_color = C_ACCENT;
        show_toast(TR("画质预设已应用", "GFX preset applied"), C_ACCENT);
    } else {
        g_gfx_dirty = 1;
        snprintf(g_status_msg, sizeof(g_status_msg), "预设无法写入磁盘! 检查 /switch/gta5save/ 目录写权限 (详见 write_fail.log)");
        g_status_color = C_RED;
    }
    return 0;
}

/* 从 /switch/gta5save/ 目录导入指定画质文件作为当前画质配置
 * (复制到目标画质文件并重新解析; 无目标时直接以该文件为当前画质)
 * fname: 文件名(如 settings.xml / settings_high.xml), NULL 时用 settings.xml */
static int gfx_import_from_switch(const char *fname) {
    char src[512];
    if (fname && fname[0]) {
        snprintf(src, sizeof(src), "sdmc:/switch/gta5save/%s", fname);
    } else {
        snprintf(src, sizeof(src), "sdmc:/switch/gta5save/settings.xml");
    }
    FILE *t = fopen(src, "rb");
    if (!t) {
        snprintf(g_status_msg, sizeof(g_status_msg), "未找到 %s! 请先放入画质文件", src);
        g_status_color = C_RED;
        return -1;
    }
    fclose(t);

    char *buf = NULL;
    size_t rd = gfx_read_all(src, &buf);
    if (!buf || rd == 0) return -1;

    /* 导入内容同步写入所有候选画质路径 (确保游戏读到) */
    int wrote = gfx_write_multi_paths(buf, rd);
    if (wrote > 0) {
        g_gfx_dirty = 0;
    } else {
        /* 无目标可写时, 把导入文件作为当前画质 */
        strncpy(g_gfx_path, src, sizeof(g_gfx_path) - 1);
        g_gfx_path[sizeof(g_gfx_path) - 1] = '\0';
        g_gfx_dirty = 1;
        snprintf(g_status_msg, sizeof(g_status_msg), "导入失败! 无法写入任何配置位置 (见 /switch/gta5save/write_fail.log)");
        g_status_color = C_RED;
        free(buf);
        return -1;
    }
    gfx_parse_xml(buf, rd);
    free(buf);
    snprintf(g_status_msg, sizeof(g_status_msg), "已从 /switch/gta5save/ 导入 %s 并写入 %d 个位置! 重启游戏生效",
             fname ? fname : "settings.xml", wrote);
    g_status_color = C_ACCENT;
    show_toast(TR("画质已导入", "Graphics imported"), C_ACCENT);
    return 0;
}

/* 解析存档同目录(或 /switch/gta5save/)的 settings.xml */
static int gfx_load(void) {
    g_gfx_path[0] = '\0';
    g_gfx_count = 0;
    g_gfx_dirty = 0;

    /* 候选路径: 优先存档同目录 (原生挂载=save:/), 兜底 /switch/gta5save/ */
    char cands[4][512];
    int n = 0;
    if (g_save_dir[0]) {
        snprintf(cands[n++], sizeof(cands[0]), "%s/settings.xml", g_save_dir);
    }
    snprintf(cands[n++], sizeof(cands[0]), "sdmc:/switch/gta5save/settings.xml");
    snprintf(cands[n++], sizeof(cands[0]), "sdmc:/settings.xml");

    const char *found = NULL;
    for (int i = 0; i < n; i++) {
        FILE *t = fopen(cands[i], "rb");
        if (t) { fclose(t); found = cands[i]; break; }
    }
    if (!found) {
        snprintf(g_status_msg, sizeof(g_status_msg), "未找到 settings.xml (请放在存档同目录或 /switch/gta5save/)");
        g_status_color = C_RED;
        return -1;
    }
    strncpy(g_gfx_path, found, sizeof(g_gfx_path) - 1);
    g_gfx_path[sizeof(g_gfx_path) - 1] = '\0';

    char *buf = NULL;
    size_t rd = gfx_read_all(g_gfx_path, &buf);
    if (!buf || rd == 0) return -1;
    int rc = gfx_parse_xml(buf, rd);
    free(buf);
    return rc;
}

/* 画质项值 +1 步进 */
static void gfx_step(int dir) {
    if (g_gfx_count == 0) return;
    GfxItem *it = &g_gfx_items[g_gfx_cur_idx];
    if (it->is_bool) {
        if (strcmp(it->value, "true") == 0) strcpy(it->value, "false");
        else strcpy(it->value, "true");
    } else if (it->is_float) {
        float v = (float)atof(it->value);
        v += it->fstep * dir;
        if (v < it->flo) v = it->flo;
        if (v > it->fhi) v = it->fhi;
        snprintf(it->value, sizeof(it->value), "%f", v);
    } else {
        int v = atoi(it->value);
        v += dir;
        if (v < it->lo) v = it->lo;
        if (v > it->hi) v = it->hi;
        snprintf(it->value, sizeof(it->value), "%d", v);
    }
    g_gfx_dirty = 1;
}

/* 解密并读取所有角色的 金钱 + 8 大技能属性 */
static int parse_and_decrypt_buffer(void) {
    if (!g_save_buf || g_save_sz < 0x200) return -1;
    int already = 0;
    if (gta5_detect(g_save_buf, g_save_sz, &g_plat, &g_start_off, &already) != 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "无法识别的存档格式 (非标准 SGTA)");
        g_status_color = C_RED;
        return -1;
    }

    if (!already) {
        if (gta5_decrypt_body(g_save_buf, g_save_sz, g_plat, g_start_off) != 0) {
            snprintf(g_status_msg, sizeof(g_status_msg), "AES 解密失败 (请确认是PC/Switch移植版)");
            g_status_color = C_RED;
            return -1;
        }
    }
    g_is_plain = 1;

    /* 提取剧情名称 */
    extract_save_title(g_save_buf, g_current_title, sizeof(g_current_title));

    /* 提取三位角色的金钱与技能属性 */
    for (int i = 0; i < 3; i++) {
        int sp_idx = CHAR_SP_INDEX[i];

        /* 0: 金钱 (gta5_char 枚举序 0=F/1=M/2=T, 与界面角色序 i 一致) */
        uint32_t money_val = 0;
        if (gta5_get_money(g_save_buf, g_save_sz, g_start_off, (gta5_char)i, &money_val) == 0) {
            g_found[i][0] = 1;
            g_values[i][0] = money_val;
            g_offsets[i][0] = gta5_find_money(g_save_buf, g_save_sz, g_start_off, (gta5_char)i);
        } else {
            g_found[i][0] = 0;
            g_values[i][0] = 0;
            g_offsets[i][0] = -1;
        }

        /* 1~8: 8大技能属性 */
        for (int k = 0; k < 8; k++) {
            const uint8_t *h_bytes = SKILL_HASHES[i][k];
            long off = gta5_find_value(g_save_buf, g_save_sz, g_start_off, h_bytes, 4, 4);
            if (off != -1) {
                uint64_t val = 0;
                if (gta5_read_value(g_save_buf, g_save_sz, off, 4, 0, &val) == 0) {
                    g_found[i][k+1] = 1;
                    g_values[i][k+1] = (uint32_t)val;
                    g_offsets[i][k+1] = off;
                } else {
                    g_found[i][k+1] = 0;
                    g_offsets[i][k+1] = -1;
                }
            } else {
                g_found[i][k+1] = 0;
                g_offsets[i][k+1] = -1;
            }
        }
    }

    /* 记录每位角色 7 项技能的行为统计偏移与当前值 (用于UI显示与保存写入) */
    for (int i = 0; i < 3; i++) {
        int sp_idx = CHAR_SP_INDEX[i];
        for (int k = 1; k < 8; k++) {   /* 技能 1~7 (特殊能力 8 不参与重算) */
            for (int f = 0; f < SKILL_BEHAVIOR_CNT; f++) {
                const BehaviorField *bf = &SKILL_BEHAVIORS[k][sp_idx][f];
                if (bf->hash[0] == 0 && bf->hash[1] == 0 &&
                    bf->hash[2] == 0 && bf->hash[3] == 0) {
                    g_behavior_offsets[k][i][f] = -1;   /* 空占位 */
                    g_behavior_found[k][i][f] = 0;
                    g_behavior_values[k][i][f] = 0.0;
                    continue;
                }
                long boff = gta5_find_value(g_save_buf, g_save_sz, g_start_off,
                                            bf->hash, 4, 4);
                g_behavior_offsets[k][i][f] = boff;
                if (boff >= 0) {
                    uint64_t raw = 0;
                    if (gta5_read_value(g_save_buf, g_save_sz, boff, 4, 0, &raw) == 0) {
                        g_behavior_found[k][i][f] = 1;
                        if (bf->is_float) {
                            uint32_t bits = (uint32_t)raw;
                            float fv;
                            memcpy(&fv, &bits, 4);
                            g_behavior_values[k][i][f] = (double)fv;
                        } else {
                            g_behavior_values[k][i][f] = (double)raw;
                        }
                    } else {
                        g_behavior_found[k][i][f] = 0;
                        g_behavior_values[k][i][f] = 0.0;
                    }
                } else {
                    g_behavior_found[k][i][f] = 0;
                    g_behavior_values[k][i][f] = 0.0;
                }
            }
        }
    }

    /* 载入新存档后清除所有技能"已修改"标记 */
    memset(g_skill_modified, 0, sizeof(g_skill_modified));
    return 0;
}

static int load_file_into_memory(const char *path, const char *source_label) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(g_status_msg, sizeof(g_status_msg), "打开文件失败: %s", path);
        g_status_color = C_RED;
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0x200) {
        fclose(fp);
        snprintf(g_status_msg, sizeof(g_status_msg), "存档文件损坏 (大小 < 512字节)");
        g_status_color = C_RED;
        return -1;
    }

    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, sz, fp);
    fclose(fp);

    if (g_save_buf) free(g_save_buf);
    g_save_buf = buf;
    g_save_sz = sz;

    if (parse_and_decrypt_buffer() != 0) return -1;

    const char *fn = strrchr(path, '/');
    if (fn) fn++; else fn = path;
    snprintf(g_status_msg, sizeof(g_status_msg), "[已载入] %s (%s)", fn, g_current_title[0] ? g_current_title : source_label);
    g_status_color = C_GREEN;
    return 0;
}

static int load_initial_save(void) {
    if (!locate_and_select_save()) {
        snprintf(g_status_msg, sizeof(g_status_msg), "未扫描到存档! 按 ZL 载入内置进度，或按 ZR 从 /switch/gta5save/ 载入历史备份");
        g_status_color = C_RED;
        return -1;
    }
    return load_file_into_memory(g_save_path, "自动匹配最近修改存档");
}

static int switch_to_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= g_num_slots) return -1;
    g_active_slot_idx = slot_idx;
    strncpy(g_save_path, g_slots[slot_idx].fullpath, sizeof(g_save_path) - 1);
    g_dirty = 0;
    return load_file_into_memory(g_save_path, "切换槽位");
}

static int load_backup_save(int backup_idx) {
    if (backup_idx < 0 || backup_idx >= g_num_backups) return -1;
    if (load_file_into_memory(g_backups[backup_idx].fullpath, g_backups[backup_idx].dir_name) == 0) {
        g_dirty = 1;
        snprintf(g_status_msg, sizeof(g_status_msg), "[已载入快照 %s] 按 A 键保存写回游戏生效", g_backups[backup_idx].dir_name);
        g_status_color = C_ACCENT;
        return 0;
    }
    return -1;
}

static int load_preset_save(int index) {
    if (index < 0 || index >= (int)NUM_PRESETS) return -1;

    FILE *fp = fopen(PRESET_SAVES[index].path, "rb");
    if (!fp) {
        snprintf(g_status_msg, sizeof(g_status_msg), "无法打开内置存档: %s", PRESET_SAVES[index].path);
        g_status_color = C_RED;
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, sz, fp);
    fclose(fp);

    if (g_save_buf) free(g_save_buf);
    g_save_buf = buf;
    g_save_sz = sz;

    if (parse_and_decrypt_buffer() != 0) return -1;

    g_dirty = 1;
    snprintf(g_status_msg, sizeof(g_status_msg), "[内置进度已载入] %s | 按 A 键写入游戏生效", PRESET_SAVES[index].name);
    g_status_color = C_ACCENT;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 将某个角色某项技能的行为统计按游戏公式反算写入。
 * 目标技能值 g_values[i][k] (k=1~7, 0~100), 使 stats_controller 重算后
 * 恰好得出该值 (不写 *_maxed, 该标记仅记录满级时间戳不参与逻辑判断)。
 * 策略: 所有行为统计一律写极大值(主/次都写满), 因为游戏公式含隐藏
 * 全局基线(Global_114963...)与叠加项, 精算不可靠; 极大值保证重算 >= 100
 * 被游戏封顶为 100, 技能持久满级。次要统计写极大值会叠加, 但被 100 封顶。
 * ------------------------------------------------------------------------- */
static void write_skill_behaviors(int char_idx, int skill_idx) {
    if (char_idx < 0 || char_idx > 2 || skill_idx < 1 || skill_idx > 7) return;
    int sp_idx = CHAR_SP_INDEX[char_idx];
    if (!g_found[char_idx][skill_idx]) return;

    long target = (long)g_values[char_idx][skill_idx];
    for (int f = 0; f < SKILL_BEHAVIOR_CNT; f++) {
        long boff = g_behavior_offsets[skill_idx][char_idx][f];
        if (boff < 0) continue;
        const BehaviorField *bf = &SKILL_BEHAVIORS[skill_idx][sp_idx][f];

        /* 无论主/次统计, 一律写极大值, 保证游戏重算 >= 100 封顶 */
        (void)target; (void)bf->base; (void)bf->divisor;
        if (bf->is_float) {
            float fv = BEHAVIOR_MAX_FLOAT;
            if (bf->unit == 1 || bf->unit == 2) fv = (float)BEHAVIOR_MAX_SEC;
            uint32_t bits;
            memcpy(&bits, &fv, 4);
            gta5_write_value(g_save_buf, g_save_sz, boff, 4, 0, bits);
        } else {
            uint64_t iv = (bf->unit == 2) ? BEHAVIOR_MAX_SEC : BEHAVIOR_MAX_INT;
            gta5_write_value(g_save_buf, g_save_sz, boff, 4, 0, iv);
        }
    }
}

/* 回写所有内存属性并加密存盘 (仅在此处进行一次安全备份) */
static int save_to_disk(void) {
    if (!g_is_plain || !g_save_buf) return -1;

    /* 仅在点击保存前做一次时间戳留底备份 */
    backup_current_save_timestamp("保存前备份");

    /* 将三位角色当前的所有修改回写到解密内存缓冲区 */
    for (int i = 0; i < 3; i++) {
        /* 回写金钱 */
        if (g_found[i][0] && g_offsets[i][0] != -1) {
            gta5_write_value(g_save_buf, g_save_sz, g_offsets[i][0], 4, 0, g_values[i][0]);
        }
        /* 回写 8 大技能属性 */
        for (int k = 1; k < NUM_ATTRS; k++) {
            if (g_found[i][k] && g_offsets[i][k] != -1) {
                gta5_write_value(g_save_buf, g_save_sz, g_offsets[i][k], 4, 0, g_values[i][k]);
            }
        }
        /* 技能被修改过则同步反算行为统计, 防止 stats_controller 覆盖回原值 */
        for (int k = 1; k < 8; k++) {
            if (g_skill_modified[i][k]) {
                write_skill_behaviors(i, k);
            }
        }
    }

    uint8_t *enc_buf = (uint8_t*)malloc(g_save_sz);
    if (!enc_buf) return -1;
    memcpy(enc_buf, g_save_buf, g_save_sz);

    if (gta5_encrypt_body(enc_buf, g_save_sz, g_plat, g_start_off) != 0) {
        free(enc_buf);
        snprintf(g_status_msg, sizeof(g_status_msg), "保存失败: 校验和重算或加密出错");
        g_status_color = C_RED;
        return -1;
    }

    FILE *fp = fopen(g_save_path, "wb");
    if (!fp) {
        free(enc_buf);
        snprintf(g_status_msg, sizeof(g_status_msg), "写入磁盘失败! 请检查写保护");
        g_status_color = C_RED;
        return -1;
    }
    fwrite(enc_buf, 1, g_save_sz, fp);
    fclose(fp);
    free(enc_buf);

    if (g_is_native_mount) {
        fsdevCommitDevice("save");
    }

    g_dirty = 0;
    const char *fn = strrchr(g_save_path, '/');
    if (fn) fn++; else fn = g_save_path;
    snprintf(g_status_msg, sizeof(g_status_msg), TR("[已保存成功] 写入 %s! 进游戏请通过【暂停->游戏->载入游戏】手动载入该槽位生效",
                                                 "[Saved] Wrote %s! In game use Pause > Game > Load Game to activate the slot"), fn);
    g_status_color = C_GREEN;
    show_toast(TR("已保存修改", "Changes saved"), C_GREEN);
    return 0;
}

static void init_shared_font(void) {
    plInitialize(PlServiceType_User);
    PlFontData font_data;
    Result rc = plGetSharedFontByType(&font_data, PlSharedFontType_ChineseSimplified);
    if (R_FAILED(rc)) rc = plGetSharedFontByType(&font_data, PlSharedFontType_Standard);
    if (R_FAILED(rc)) rc = plGetSharedFontByType(&font_data, PlSharedFontType_ChineseTraditional);

    if (R_SUCCEEDED(rc)) {
        if (FT_Init_FreeType(&g_ft) == 0) {
            FT_New_Memory_Face(g_ft, (const FT_Byte*)font_data.address, font_data.size, 0, &g_face);
        }
    }
}

/* ========================================================================= */
/* ★ v21: DLC 管理 -- 扫描 / 注册 / 导入 dlcpack                              */
/* ========================================================================= */
/* 设计要点：
 *   · 不重建 update.rpf，只对 dlclist.xml 做【纯字节原位替换】
 *   · 压缩必须用 raw deflate（wbits=-15），与游戏一致（zlib 头会让游戏读不到）
 *   · 注册判据：dlclist.xml 里有 <Item>dlcpacks:/名字/</Item>
 *   · 完整性检查：车辆资源里每辆车都要有 xxx.nft + xxx.ntd + xxx_hi.nft
 *     （★ 官方 212 辆车 100% 都有 _hi.nft，缺了会在撞击/近距离时崩溃） */

#define DLC_MAX         128
#define DLC_NAME_MAX    64
#define DLC_SRC_MAX     64
#define DLC_SRC_DIR_MAX 6
#define DLC_DLCPACKS    "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks"
#define DLC_UPDATERPF   "sdmc:/atmosphere/contents/0100b00b51230000/romfs/update/update.rpf"
/* ★ 待导入的候选目录：用户把 dlcpack 目录丢进任一个即可被扫到 */
static const char *DLC_SRC_DIRS[DLC_SRC_DIR_MAX] = {
    "sdmc:/switch/gta5save/dlc",          /* 推荐：和编辑器其它文件放一起 */
    "sdmc:/switch/gta5save",              /* 兼容：直接丢在 gta5save 下 */
    "sdmc:/switch/GTA5DLC",
    "sdmc:/dlc",
    "sdmc:/gta5dlc",
    "sdmc:/switch",
};

typedef struct {
    char name[DLC_NAME_MAX];   /* dlcpack 目录名 */
    u32  rpf_size;             /* dlc.rpf 大小（0 = 缺文件） */
    int  has_rpf;              /* 有 dlc.rpf */
    int  registered;           /* 已写进 dlclist.xml */
    int  mounted;              /* 已写进 extratitleupdatedata.meta */
    int  full;                 /* 两处都写了（游戏才认） */
    int  fmt;                  /* ★ 1=Switch 格式  0=PC 格式(会闪退)  -1=读不了 */
    int  map_st;               /* ★ v31: 0=无地图资源 1=地图正常 2=地图数据不在主rpf(不会加载) -1=? */
    int  n_model, n_map, n_tex;/* ★ v32: 资源构成（模型 / 地图数据 / 贴图） */
    char cn[64];               /* ★ v5.1: 中文显示名（读包目录下 name.txt，UTF-8 一行） */
    char spawn[48];            /* ★ v5.2: 车辆刷车名（车辆包才有） */
} DlcItem;

static DlcItem g_dlc_items[DLC_MAX];

/* ========================================================================= */
/* ★ v5.0 脚本 mod（.nsc）-- DLC 页第 3 个视图                                */
/* ========================================================================= */
/* 🚨 前向声明：dlc_msg / dlc_err 的定义在本文件更靠后的位置，
 *    而下面的 script_install_selected() 要用它们（C 里 static 函数也必须先声明） */
static void dlc_msg(const char *fmt, ...);
static void dlc_err(const char *fmt, ...);
static void script_show_dir(void);      /* ★ v5.5: 前向声明（ACT_SCRIPT_DIR 里要用） */
static void script_mgr_open(void);      /* ★ v6.2: 打开脚本管理弹窗 */
static void script_mgr_apply(void);     /* ★ v6.2: 执行已确认的动作 */
static void script_scan(void);          /* ★ v5.5: 前向声明 */
static void script_verify_installed(void);  /* ★ v5.7: script_scan 里要用（定义在后） */

/* 与 dlcpack 不同：脚本装在 update2.rpf 的 script_rel.rpf 里。
 * 用户把 .nsc 丢进下面的目录，扫描后按 A 安装（同名=替换，新名=新增）。
 * ★ v5.5: 目录改为「软件自动创建」，路径在页面上明示。 */
#define SCRIPT_DIR      "sdmc:/switch/gta5save/script"
#define SCRIPT_DIR_ALT  "sdmc:/switch/gta5save"      /* 兼容：直接丢在 gta5save 下也能扫到 */

static int g_script_dir_ready = 0;    /* 目录已确保存在 */
static int g_script_from_alt  = 0;    /* 本次扫描是否用了兼容目录 */

/* ★ v5.5: 确保脚本目录存在（自动创建，用户不用手动建） */
static void script_ensure_dir(void) {
    ensure_dir(SCRIPT_DIR);           /* ensure_dir 已在前文定义（递归建） */
    g_script_dir_ready = 1;
    /* ★ 在目录里放一个英文说明文件，方便用户在电脑上一眼认路
     *   （英文名避免 SD 卡在 Windows 上的编码显示问题） */
    char tip[512];
    snprintf(tip, sizeof(tip), "%s/README_put_nsc_here.txt", SCRIPT_DIR);
    /* 已存在就不重写（省 SD 写次数）；用 fopen 探测，避免依赖 dlc_file_size 的定义顺序 */
    {
        FILE *probe = fopen(tip, "rb");
        if (probe) { fclose(probe); return; }
    }
    FILE *f = fopen(tip, "wb");
    if (!f) return;
    const char *txt =
        "GTA5 Script MOD folder\r\n"
        "======================\r\n"
        "\r\n"
        "Put your .nsc script files in THIS folder.\r\n"
        "\r\n"
        "Then in the NRO:\r\n"
        "  1) go to the [Script MOD] tab\r\n"
        "  2) press X to rescan\r\n"
        "  3) select a script, press A to install\r\n"
        "\r\n"
        "NOTE: the file name IS the script name - do not rename it.\r\n";
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
    fsdevCommitDevice("sdmc");
}

typedef struct {
    char name[GC_SCRIPT_NAME_MAX];
    u32  size;
    int  installed;
    int  from_alt;        /* ★ v5.5: 来自兼容目录 */
} ScriptItem;

/* ★ v5.7: 安装前安全检查 —— 空文件/过小文件不能装。
 *   🚨 若磁盘上的同名文件是 0 字节，安装会拿它【覆盖原版脚本】⇒ 游戏脚本损坏。
 *   （gc_script_install 内部也会拒绝 rlen<=0，这里提前拦并在 UI 上标出来） */
static int script_file_usable(u32 size) {
    return size >= 16 && size <= 8 * 1024 * 1024;
}

static ScriptItem g_script_items[GC_SCRIPT_MAX];
static int g_script_count = 0, g_script_sel = 0, g_script_scr = 0;
static int g_script_rpf_n     = -1;   /* ★ v5.6: RPF 里读到的 .nsc 条数（<0 = 读失败） */
static char g_script_rpf_err[200] = {0};  /* ★ v5.6: 读失败原因 */
/* ★ v5.7: 页内目录信息面板（点「目录信息」/ 按 Y 显示，再按一次关闭）
 *   原来的实现只写状态栏单行 ⇒ 内容太长被裁掉，用户以为"没反应" */
static char g_script_dirinfo[512] = {0};
static int  g_script_dirinfo_on = 0;
static int  g_script_dirinfo_ok = 1;

static void script_scan(void) {
    g_script_count = 0;
    g_script_from_alt = 0;
    script_ensure_dir();               /* ★ v5.5: 先确保目录存在 */
    /* ★ v5.6: 必须处理负数返回！旧代码 ninst<0 时循环不执行 ⇒ 所有项都显示"未安装"，
     *   而且界面无任何提示 ⇒ 用户以为"没装上"，其实是读 RPF 失败。 */
    /* ★ v5.7: 这里只取总数（max=0）；"已装"判定改用 gc_script_query（见下） */
    g_script_rpf_n = gc_script_list(NULL, 0);
    if (g_script_rpf_n < 0) {
        snprintf(g_script_rpf_err, sizeof(g_script_rpf_err), "%s",
                 gc_script_last_error());
    } else {
        g_script_rpf_err[0] = 0;
    }
    /* ★ v5.5: 两个目录都扫（推荐目录 + 兼容目录） */
    const char *dirs[2] = { SCRIPT_DIR, SCRIPT_DIR_ALT };
    for (int di = 0; di < 2; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && g_script_count < GC_SCRIPT_MAX) {
            size_t L = strlen(ent->d_name);
            if (L < 5) continue;
            if (strcasecmp(ent->d_name + L - 4, ".nsc") != 0) continue;
            char p[768];
            snprintf(p, sizeof(p), "%s/%s", dirs[di], ent->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            /* 去重（两目录都有同名时只算一次，优先推荐目录） */
            int dup = 0;
            for (int k = 0; k < g_script_count; k++)
                if (strcasecmp(g_script_items[k].name, ent->d_name) == 0) { dup = 1; break; }
            if (dup) continue;
            ScriptItem *it = &g_script_items[g_script_count];
            snprintf(it->name, sizeof(it->name), "%s", ent->d_name);
            it->size = (u32)st.st_size;
            it->from_alt = (di == 1);
            if (di == 1) g_script_from_alt = 1;
            it->installed = 0;
            g_script_count++;
        }
        closedir(d);
    }
    /* ★ v5.7: 一次性按名查询全部脚本的安装状态（不受 RPF 条目总数限制） */
    if (g_script_rpf_n >= 0 && g_script_count > 0)
        script_verify_installed();
}

/* ★ v5.6: 从 RPF 回读校验某个脚本的安装状态（安装/删除后调用，不用重扫整个目录）
 *   ★ v5.7: 改用 gc_script_query —— 旧实现用 gc_script_list（受 GC_SCRIPT_MAX 截断，
 *   真实 update2 有 1027 个 .nsc ⇒ 排后面的脚本被误判「未安装」，安装后会报
 *   「回读没找到」的假错误） */
static void script_verify_installed(void) {
    if (g_script_count <= 0) {
        static GcScriptItem tmp[1];
        g_script_rpf_n = gc_script_list(tmp, 0);   /* 只要总数 */
        g_script_rpf_err[0] = 0;
        if (g_script_rpf_n < 0)
            snprintf(g_script_rpf_err, sizeof(g_script_rpf_err), "%s",
                     gc_script_last_error());
        return;
    }
    static GcScriptQuery q[GC_SCRIPT_MAX];
    int nq = g_script_count > GC_SCRIPT_MAX ? GC_SCRIPT_MAX : g_script_count;
    for (int i = 0; i < nq; i++) {
        snprintf(q[i].name, sizeof(q[i].name), "%s", g_script_items[i].name);
        q[i].installed = 0;
    }
    char err[200] = {0};
    int total = gc_script_query(q, nq, err, sizeof(err));
    g_script_rpf_n = total;
    if (total < 0) {
        snprintf(g_script_rpf_err, sizeof(g_script_rpf_err), "%s",
                 err[0] ? err : gc_script_last_error());
        return;
    }
    g_script_rpf_err[0] = 0;
    for (int i = 0; i < nq; i++)
        g_script_items[i].installed = q[i].installed;
}

/* ★ v5.2: 删除选中的已装脚本（从 update2.rpf 里移除）
 *   ★ v5.6: 删除后【从 RPF 回读校验】，不靠内存标记猜 —— 用户要能看到真实结果 */
static void script_uninstall_selected(void) {
    if (g_script_sel < 0 || g_script_sel >= g_script_count) { dlc_err("没有选中项"); return; }
    ScriptItem *it = &g_script_items[g_script_sel];
    char nm[GC_SCRIPT_NAME_MAX];
    snprintf(nm, sizeof(nm), "%s", it->name);
    if (!it->installed) { dlc_err("%s 本来就没装", nm); return; }
    char err[256];
    err[0] = 0;
    int rc = gc_script_uninstall(nm, err, sizeof(err));
    if (rc != GC_SCRIPT_OK) {
        dlc_err("删除失败: %s", err[0] ? err : "未知错误");
        return;
    }
    fsdevCommitDevice("sdmc");
    /* ★ 回读校验：确认真的不在 RPF 里了 */
    script_verify_installed();
    int still = 0;
    for (int k = 0; k < g_script_count; k++)
        if (strcasecmp(g_script_items[k].name, nm) == 0) still = g_script_items[k].installed;
    if (still)
        dlc_err("%s 删除后回读仍在 RPF 里，请检查 update2.rpf", nm);
    else
        dlc_msg("[v] 已删除 %s（回读校验通过，重启游戏生效）", nm);
}

/* ★ v6.2: 还原官方脚本的旧单体入口已并入 script_mgr_apply()（二级菜单 + 二次确认）
 *   —— 原来这里是 script_restore_stock()，现删除以免「无二次确认」被误用。 */

/* ========================================================================= */
/* ★ v6.2: 脚本管理二级菜单                                                   */
/* ========================================================================= */
/* 为什么改成弹窗：
 *   原来脚本页有 6 个按钮，其中两个是「还原官方 error_listener」「还原官方
 *   controller」—— 文字太长，实机截图里已经溢出格子还挤在一起。现在合并成
 *   一个「模组管理」按钮，点开进弹窗，里面分两区：
 *
 *   【还原官方脚本】3 项（从 romfs 内置的真·原版写回，全是 RSC7 资源条目）
 *   【内置模组】    4 项（打进 NRO，一键装，免插卡拷文件），署名原作者
 *
 * 每项都走【二次确认】（弹窗 8），因为都会覆盖 RPF 里的既有脚本。 */
static int script_mgr_total(void) {
    return gc_script_stock_count() + gc_script_builtin_count();
}

static void script_mgr_open(void) {
    g_modal_mode = MODE_SCRIPT_MGR;
    g_modal_sel = 0;
    g_modal_scr = 0;
}

/* 执行二次确认通过的动作 */
static void script_mgr_apply(void) {
    char err[256];
    char what[160];
    int rc;

    if (g_mod_pending_kind == 0) {
        const char *nm = gc_script_stock_name(g_mod_pending_idx);
        if (!nm) { dlc_err("内部错误：还原索引越界"); return; }
        snprintf(what, sizeof(what), "%s", nm);
        err[0] = 0;
        rc = gc_script_restore_stock(nm, err, sizeof(err));
        if (rc == GC_SCRIPT_OK) {
            fsdevCommitDevice("sdmc");
            script_verify_installed();
            dlc_msg("[v] 已还原官方 %s（重启游戏生效）", what);
        } else {
            dlc_err("还原失败: %s", err[0] ? err : "未知错误");
        }
        return;
    }

    const GcBuiltinMod *m = gc_script_builtin(g_mod_pending_idx);
    if (!m) { dlc_err("内部错误：模组索引越界"); return; }
    snprintf(what, sizeof(what), "%s", MOD_TITLE(m) ? MOD_TITLE(m) : "?");
    err[0] = 0;
    rc = gc_script_builtin_install(g_mod_pending_idx, err, sizeof(err));
    if (rc == GC_SCRIPT_OK) {
        fsdevCommitDevice("sdmc");
        script_verify_installed();
        dlc_msg("[v] %s 安装完成（重启游戏生效）", what);
    } else {
        dlc_err("安装失败: %s", err[0] ? err : "未知错误");
    }
}

/* 进入二次确认弹窗 */
static void script_mgr_ask(int kind, int idx) {
    g_mod_pending_kind = kind;
    g_mod_pending_idx  = idx;
    g_modal_mode = MODE_SCRIPT_CONFIRM;
}

static void script_install_selected(void) {
    if (g_script_sel < 0 || g_script_sel >= g_script_count) { dlc_err("没有选中项"); return; }
    ScriptItem *it = &g_script_items[g_script_sel];
    char nm[GC_SCRIPT_NAME_MAX];
    snprintf(nm, sizeof(nm), "%s", it->name);
    /* ★ v5.7: 提前拦截无效文件（0 字节/过大）——否则会拿它覆盖游戏原版脚本 */
    if (!script_file_usable(it->size)) {
        dlc_err("%s 文件无效（%u 字节）—— 不能安装，请检查源文件",
                nm, it->size);
        return;
    }
    char p[768], err[256];
    err[0] = 0;
    /* ★ v5.5: 按该项实际所在目录取路径（兼容目录也支持） */
    snprintf(p, sizeof(p), "%s/%s",
             it->from_alt ? SCRIPT_DIR_ALT : SCRIPT_DIR, nm);
    int rc = gc_script_install(p, nm, err, sizeof(err));
    if (rc != GC_SCRIPT_OK) {
        dlc_err("安装失败: %s", err[0] ? err : "未知错误");
        return;
    }
    fsdevCommitDevice("sdmc");
    /* ★ v5.6/v5.7: 回读校验 —— 用按名查询（不受 RPF 条目总数限制） */
    script_verify_installed();
    int now = 0;
    for (int k = 0; k < g_script_count; k++)
        if (strcasecmp(g_script_items[k].name, nm) == 0) now = g_script_items[k].installed;
    if (now)
        dlc_msg("[v] 已安装 %s（回读校验通过，重启游戏生效）", nm);
    else
        dlc_err("%s 安装后回读没找到，可能写入未生效", nm);
}

/* ★ v5.5: 显示脚本目录信息（路径 + 内容统计），并确保目录已自动创建
 *   ★ v5.6: 同时回读 RPF 实况，把「装没装」的权威数据摆出来
 *   ★ v5.7: 原实现只写状态栏（单行 1280px 放不下 ⇒ 用户反馈"点了没反应"）。
 *     改为写【页内信息面板】(g_script_dirinfo)，显示 3 行，停留到下次操作。 */
static void script_show_dir(void) {
    script_ensure_dir();
    /* ★ 先回读 RPF（权威状态） */
    script_verify_installed();
    int n_nsc = 0;
    DIR *d = opendir(SCRIPT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            size_t L = strlen(e->d_name);
            if (L > 4 && strcasecmp(e->d_name + L - 4, ".nsc") == 0) n_nsc++;
        }
        closedir(d);
    }
    if (g_script_rpf_n < 0) {
        snprintf(g_script_dirinfo, sizeof(g_script_dirinfo),
                 "目录: %s\n"
                 "磁盘上 %d 个 .nsc\n"
                 "读 update2.rpf 失败: %s",
                 SCRIPT_DIR, n_nsc,
                 g_script_rpf_err[0] ? g_script_rpf_err : "未知原因");
        g_script_dirinfo_ok = 0;
        g_script_dirinfo_on = 1;
        dlc_err("读 update2.rpf 失败（详见页内面板）");
        return;
    }
    int ninst = 0;
    for (int i = 0; i < g_script_count; i++)
        if (g_script_items[i].installed) ninst++;
    snprintf(g_script_dirinfo, sizeof(g_script_dirinfo),
             "脚本目录: %s\n"
             "磁盘上 %d 个 .nsc   已装 %d 个\n"
             "%s 内共 %d 个 .nsc 条目",
             SCRIPT_DIR, n_nsc, ninst, GC_SCRIPT_REL_RPF, g_script_rpf_n);
    g_script_dirinfo_ok = 1;
    g_script_dirinfo_on = 1;
    dlc_msg("目录信息已显示在列表中（再按一次 Y 关闭）");
}

/* ★ v32: 前向声明（dlc_scan_src 里要用，而它的定义在后面） */
static int dlc_src_path_of(const char *name, char *out, size_t outsz);
static int     g_dlc_count   = 0;      /* 已装 DLC 数 */
static int     g_dlc_sel     = 0;      /* 当前选中 */
static int     g_dlc_scr     = 0;      /* 列表滚动偏移 */
static int     g_dlc_view    = 0;      /* 0=已装  1=待导入 */

/* 待导入（从 /switch/ 等目录扫到的 dlcpack） */
static DlcItem g_dlc_src[DLC_SRC_MAX];
static int     g_dlc_src_count = 0;
static int     g_dlc_src_sel   = 0;
static int     g_dlc_src_scr   = 0;
static char    g_dlc_src_dir[DLC_SRC_DIR_MAX][256];
static int     g_dlc_src_dir_n = 0;

static char    g_dlc_msg[256] = {0};   /* 状态提示 */
static int     g_dlc_msg_ok   = 0;     /* 提示是成功还是失败 */

/* dlclist.xml 的内存副本（读一次，改完写回） */
static char   *g_dlc_xml     = NULL;
static size_t  g_dlc_xml_len = 0;
static GcEntry g_dlc_xml_ent;
static int     g_dlc_xml_loaded = 0;

/* ★ extratitleupdatedata.meta 的内存副本（第二处注册，漏了游戏不加载 dlcpack） */
static char   *g_dlc_etud     = NULL;
static size_t  g_dlc_etud_len = 0;
static GcEntry g_dlc_etud_ent;
static int     g_dlc_etud_loaded = 0;

/* 两个文件都改过（待写入） */
static int     g_dlc_dirty = 0;

/* ★ v25: 删除确认弹窗（0=无 1=等待确认删除） */
static int     g_dlc_confirm = 0;

/* 前置声明（dlc_import_selected 里要用，定义在后面） */
static int dlc_check_integrity(const char *name, char *out, size_t out_sz);
static int gc_open_rpf(void);     /* DLC 模块复用性能页的 rpf 句柄 */

/* ★ v5.4: 模型载入日志 —— 把每次 DLC/脚本 mod 操作追加到 SD 卡日志文件
 *   用途：出问题时把日志拷出来看，比在 Switch 上猜快得多。
 *   路径：sdmc:/switch/gta5save/loader.log （追加写，单文件上限 256 KB 后清空重来）
 *   注意：日志里绝不能出现非 ASCII 之外的高危字符（方框）—— 用纯 ASCII 前缀。 */
#define DLC_LOG_PATH "sdmc:/switch/gta5save/loader.log"
#define DLC_LOG_MAX  (256 * 1024)

static void dlc_log(const char *tag, const char *fmt, ...) {
    va_list ap;
    char body[512];
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* 太大会拖慢 SD 卡 ⇒ 超限就重开（截断） */
    long sz = 0;
    FILE *rf = fopen(DLC_LOG_PATH, "rb");
    if (rf) { fseeko(rf, 0, SEEK_END); sz = (long)ftello(rf); fclose(rf); }

    FILE *f = fopen(DLC_LOG_PATH, sz > DLC_LOG_MAX ? "w" : "a");
    if (!f) return;
    fseeko(f, 0, SEEK_END);
    char line[640];
    /* 时间戳用 tick（Switch 无 RTC 时 tt 不可靠，用秒计数足够定位顺序） */
    u64 tick = armGetSystemTick();
    snprintf(line, sizeof(line), "[%8llu] %-6s %s\n",
             (unsigned long long)(tick / 19200000ULL), tag ? tag : "-", body);
    fwrite(line, 1, strlen(line), f);
    fflush(f);
    fclose(f);
}

static void dlc_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dlc_msg, sizeof(g_dlc_msg), fmt, ap);
    va_end(ap);
    g_dlc_msg_ok = 1;
    dlc_log("OK", "%s", g_dlc_msg);          /* ★ v5.4: 同步落日志 */
}static void dlc_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dlc_msg, sizeof(g_dlc_msg), fmt, ap);
    va_end(ap);
    g_dlc_msg_ok = 0;
    dlc_log("ERR", "%s", g_dlc_msg);         /* ★ v5.4: 同步落日志 */
}

/* 目录是否存在 */
static int dlc_dir_exists(const char *p) {
    DIR *d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

/* 文件大小（-1 = 不存在） */
static long dlc_file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ★ v5.1: 读包目录下的 name.txt 作为中文显示名（UTF-8 一行，可含 BOM）
 *   约定：<dlcpack目录>/name.txt  内容就是显示名，例如「秋名山」
 *   没有该文件 ⇒ cn 为空，界面退回显示目录名。用户可随手改这个文件。 */
static void dlc_read_cn_name(const char *dirpath, char *out, size_t outsz) {
    out[0] = 0;
    char np[768];
    snprintf(np, sizeof(np), "%s/name.txt", dirpath);
    FILE *f = fopen(np, "rb");
    if (!f) return;
    size_t got = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[got] = 0;
    /* 跳过 UTF-8 BOM */
    if (got >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        memmove(out, out + 3, got - 2);
    }
    /* 去掉尾部换行/空白 */
    size_t n = strlen(out);
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' ||
                     out[n-1] == ' '  || out[n-1] == '\t'))
        out[--n] = 0;
}

/* 扫描 dlcpacks 目录，列出所有子目录 */
static int dlc_scan_dlcpacks(void) {
    g_dlc_count = 0;
    DIR *d = opendir(DLC_DLCPACKS);
    if (!d) {
        dlc_err("打不开 dlcpacks 目录（SD 卡路径不对？）");
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_dlc_count < DLC_MAX) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
        char sub[320];
        snprintf(sub, sizeof(sub), "%s/%s", DLC_DLCPACKS, ent->d_name);
        if (!dlc_dir_exists(sub)) continue;

        DlcItem *it = &g_dlc_items[g_dlc_count];
        memset(it, 0, sizeof(*it));
        strncpy(it->name, ent->d_name, DLC_NAME_MAX - 1);
        char rpf[400];
        snprintf(rpf, sizeof(rpf), "%s/dlc.rpf", sub);
        long sz = dlc_file_size(rpf);
        it->has_rpf = (sz > 0);
        it->rpf_size = (sz > 0) ? (u32)sz : 0;
        /* ★ v5.1: 中文显示名（可选） */
        dlc_read_cn_name(sub, it->cn, sizeof(it->cn));
        /* ★ v33: 一次 IO 拿全部信息（格式 + 地图自检 + 资源构成）
         *   🚨 勿改回分别调用三个函数 -- 那会把同一份数据重复读 ~10 次，列表一多就卡 */
        if (it->has_rpf) {
            GcDlcProbe pb;
            gc_dlc_probe(rpf, &pb);
            it->fmt     = pb.fmt;
            it->map_st  = pb.map_st;
            it->n_model = pb.n_model;
            it->n_map   = pb.n_map;
            it->n_tex   = pb.n_tex;
            snprintf(it->spawn, sizeof(it->spawn), "%s", pb.vehicle);
        } else {
            it->fmt = -1;
            it->map_st = -1;
            it->n_model = it->n_map = it->n_tex = 0;
            it->spawn[0] = 0;
        }
        g_dlc_count++;
    }
    closedir(d);

    /* 按名字排序（简单冒泡，数量小） */
    for (int i = 0; i < g_dlc_count; i++)
        for (int j = i + 1; j < g_dlc_count; j++)
            if (strcmp(g_dlc_items[i].name, g_dlc_items[j].name) > 0) {
                DlcItem t = g_dlc_items[i];
                g_dlc_items[i] = g_dlc_items[j];
                g_dlc_items[j] = t;
            }
    return 0;
}

/* 载入 dlclist.xml + extratitleupdatedata.meta（各读一次，缓存在内存）
 * ★ 两处注册缺一不可：
 *     dlclist.xml            = 告诉游戏「有这个 dlcpack」
 *     extratitleupdatedata   = 把 dlcpack 挂载成 dlc_<名字>:/ 设备
 *   PC 教程说「etud 不用改」在 Switch 移植版【不成立】。 */
static int dlc_load_dlclist(void) {
    if (g_dlc_xml) { free(g_dlc_xml); g_dlc_xml = NULL; }
    g_dlc_xml_len = 0;
    g_dlc_xml_loaded = 0;
    if (g_dlc_etud) { free(g_dlc_etud); g_dlc_etud = NULL; }
    g_dlc_etud_len = 0;
    g_dlc_etud_loaded = 0;
    g_dlc_dirty = 0;

    /* 用 gc 模块已打开的 rpf（或自己开一个） */
    int rc = gc_open_rpf();
    if (rc != GC_OK) {
        /* gc_open_rpf 只找 gameconfig；这里换成直接开 update.rpf */
        rc = gc_rpf_open(DLC_UPDATERPF, &g_gc_rpf);
        if (rc != GC_OK) {
            dlc_err("打开 update.rpf 失败: %s", gc_rpf_last_error());
            return rc;
        }
        g_gc_opened = 1;
    }
    rc = gc_rpf_find(&g_gc_rpf, "dlclist.xml", &g_dlc_xml_ent);
    if (rc != GC_OK) {
        dlc_err("在 update.rpf 里找不到 dlclist.xml (rc=%d)", rc);
        return rc;
    }
    u8 *buf = NULL; size_t blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_dlc_xml_ent, &buf, &blen);
    if (rc != GC_OK) {
        dlc_err("读 dlclist.xml 失败 (rc=%d)", rc);
        return rc;
    }
    g_dlc_xml = (char *)malloc(blen + 1);
    if (!g_dlc_xml) { free(buf); dlc_err("内存不足"); return GC_ERR_NOMEM; }
    memcpy(g_dlc_xml, buf, blen);
    g_dlc_xml[blen] = '\0';
    g_dlc_xml_len = blen;
    free(buf);
    g_dlc_xml_loaded = 1;

    /* ---- 第二处：extratitleupdatedata.meta ---- */
    rc = gc_rpf_find(&g_gc_rpf, "extratitleupdatedata.meta", &g_dlc_etud_ent);
    if (rc != GC_OK) {
        /* 找不到不算致命：有些版本没有这个文件，只提示 */
        dlc_msg("提示：update.rpf 里没有 extratitleupdatedata.meta");
        g_dlc_etud_loaded = 0;
        return GC_OK;
    }
    buf = NULL; blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_dlc_etud_ent, &buf, &blen);
    if (rc != GC_OK) {
        dlc_msg("读 extratitleupdatedata.meta 失败 (rc=%d)", rc);
        g_dlc_etud_loaded = 0;
        return GC_OK;
    }
    g_dlc_etud = (char *)malloc(blen + 1);
    if (!g_dlc_etud) { free(buf); return GC_ERR_NOMEM; }
    memcpy(g_dlc_etud, buf, blen);
    g_dlc_etud[blen] = '\0';
    g_dlc_etud_len = blen;
    free(buf);
    g_dlc_etud_loaded = 1;
    return GC_OK;
}

/* 判断某 dlcpack 是否已注册（在 xml 里找 <Item>dlcpacks:/名字/</Item>） */
static int dlc_is_registered(const char *name) {
    if (!g_dlc_xml) return 0;
    char pat[160];
    snprintf(pat, sizeof(pat), "dlcpacks:/%s/", name);
    /* 大小写不敏感匹配（游戏本身大小写混用） */
    size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= g_dlc_xml_len; i++) {
        int ok = 1;
        for (size_t k = 0; k < pl; k++) {
            char a = g_dlc_xml[i + k], b = pat[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* 判断某 dlcpack 是否已写进 extratitleupdatedata.meta（dlc_名字:/） */
static int dlc_is_mounted(const char *name) {
    if (!g_dlc_etud) return 0;
    char pat[160];
    snprintf(pat, sizeof(pat), "dlc_%s:/", name);
    size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= g_dlc_etud_len; i++) {
        int ok = 1;
        for (size_t k = 0; k < pl; k++) {
            char a = g_dlc_etud[i + k], b = pat[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* 刷新每个已装 DLC 的注册状态（dlclist + etud 两处都要） */
static void dlc_refresh_registered(void) {
    for (int i = 0; i < g_dlc_count; i++) {
        int a = dlc_is_registered(g_dlc_items[i].name);
        int b = dlc_is_mounted(g_dlc_items[i].name);
        g_dlc_items[i].registered = a;
        g_dlc_items[i].mounted    = b;
        /* 两处齐了才算「完全注册」（游戏才认） */
        g_dlc_items[i].full = (a && b) ? 1 : 0;
    }
}

/* ★ 从 upto 往前找 <Item 标签的开头（返回 '<' 的下标；找不到返回 (size_t)-1）
 *
 * 为什么不能「往前找第一个 '<'」？
 *   etud 的结构是：
 *     <Item type="...">
 *       <deviceName>dlc_名字:/</deviceName>     ← 名字前面最近的 '<' 是 <deviceName>
 *     </Item>
 *   往前找 '<' 会停在 <deviceName> 上 ⇒ 标签名比对失败 ⇒ 删除静默失效。
 *   踩过：dlclist 能删（名字前就是 <Item>），etud 删不掉（名字前是 <deviceName>）。
 *
 * 为什么用 strncasecmp("</Item", "<Item") 不会误命中？
 *   "</Item" 第 2 个字符是 '/'，与 "Item" 的 'I' 不等 ⇒ 不会把闭合标签当开头。 */
static size_t dlc_tag_before(const char *s, size_t upto, const char *tag) {
    size_t tl = strlen(tag);
    if (upto < tl) return (size_t)-1;
    for (size_t k = upto; k >= tl; k--) {
        if (strncasecmp(s + k - tl, tag, tl) == 0) return k - tl;
    }
    return (size_t)-1;
}

/* 在 xml 里插入一条 <Item>dlcpacks:/名字/</Item>（原地改 g_dlc_xml） */
static int dlc_xml_add(const char *name) {
    if (!g_dlc_xml) return -1;
    if (dlc_is_registered(name)) return 0;   /* 已存在 */

    /* 找最后一个 </Item>，插在它后面 */
    size_t pos = 0;
    int found = 0;
    for (size_t i = g_dlc_xml_len; i >= 7; i--) {
        if (memcmp(g_dlc_xml + i - 7, "</Item>", 7) == 0 ||
            memcmp(g_dlc_xml + i - 7, "</item>", 7) == 0) {
            pos = i; found = 1; break;
        }
    }
    if (!found) return -1;

    char ins[160];
    int ilen = snprintf(ins, sizeof(ins), "\r\n\t\t<Item>dlcpacks:/%s/</Item>", name);
    if (ilen <= 0) return -1;

    size_t newlen = g_dlc_xml_len + (size_t)ilen;
    char *n = (char *)realloc(g_dlc_xml, newlen + 1);
    if (!n) return -1;
    g_dlc_xml = n;
    memmove(g_dlc_xml + pos + ilen, g_dlc_xml + pos, g_dlc_xml_len - pos);
    memcpy(g_dlc_xml + pos, ins, (size_t)ilen);
    g_dlc_xml_len = newlen;
    g_dlc_xml[g_dlc_xml_len] = '\0';
    return 1;
}

/* 从 xml 里删掉 <Item>dlcpacks:/名字/</Item> 整行 */
static int dlc_xml_del(const char *name) {
    if (!g_dlc_xml) return -1;
    char pat[160];
    snprintf(pat, sizeof(pat), "dlcpacks:/%s/", name);
    size_t pl = strlen(pat);

    for (size_t i = 0; i + pl <= g_dlc_xml_len; i++) {
        int ok = 1;
        for (size_t k = 0; k < pl; k++) {
            char a = g_dlc_xml[i + k], b = pat[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (!ok) continue;

        /* 向前找 <Item>，向后找 </Item>，连同前后空白一起删 */
        size_t s = dlc_tag_before(g_dlc_xml, i, "<Item");
        if (s == (size_t)-1) continue;
        if (s + 6 > g_dlc_xml_len) continue;
        if (!((memcmp(g_dlc_xml + s, "<Item>", 6) == 0) ||
              (memcmp(g_dlc_xml + s, "<item>", 6) == 0))) continue;
        size_t e = i + pl;
        while (e < g_dlc_xml_len && g_dlc_xml[e] != '>') e++;
        if (e < g_dlc_xml_len) e++;   /* 跳过 '>' */
        /* 吃前导空白 */
        while (s > 0 && (g_dlc_xml[s - 1] == '\r' || g_dlc_xml[s - 1] == '\n' ||
                         g_dlc_xml[s - 1] == '\t' || g_dlc_xml[s - 1] == ' ')) s--;

        size_t cut = e - s;
        memmove(g_dlc_xml + s, g_dlc_xml + e, g_dlc_xml_len - e);
        g_dlc_xml_len -= cut;
        g_dlc_xml[g_dlc_xml_len] = '\0';
        return 1;
    }
    return 0;   /* 没找到 */
}

/* =========================================================================
 * ★★ 第二处注册：extratitleupdatedata.meta
 *    作用：把 dlcpack 挂载成设备 dlc_<名字>:/，指向 update:/dlc_patch/<名字>/
 *    格式：<Item type="SExtraTitleUpdateMount">
 *            <deviceName>dlc_名字:/</deviceName>
 *            <path>update:/dlc_patch/名字/</path>
 *          </Item>
 * ========================================================================= */

/* 在 etud 里插入一个 Mount（原地改 g_dlc_etud） */
static int dlc_etud_add(const char *name) {
    if (!g_dlc_etud) return -1;
    if (dlc_is_mounted(name)) return 0;      /* 已存在 */

    /* 找最后一个 </Mounts>，插在它前面 */
    size_t pos = 0;
    int found = 0;
    for (size_t i = g_dlc_etud_len; i >= 9; i--) {
        if (memcmp(g_dlc_etud + i - 9, "</Mounts>", 9) == 0) {
            pos = i - 9; found = 1; break;
        }
    }
    if (!found) return -1;

    char ins[400];
    int ilen = snprintf(ins, sizeof(ins),
        "\t\t<Item type=\"SExtraTitleUpdateMount\">\r\n"
        "\t\t\t<deviceName>dlc_%s:/</deviceName>\r\n"
        "\t\t\t<path>update:/dlc_patch/%s/</path>\r\n"
        "\t\t</Item>\r\n", name, name);
    if (ilen <= 0) return -1;

    size_t newlen = g_dlc_etud_len + (size_t)ilen;
    char *n = (char *)realloc(g_dlc_etud, newlen + 1);
    if (!n) return -1;
    g_dlc_etud = n;
    memmove(g_dlc_etud + pos + ilen, g_dlc_etud + pos, g_dlc_etud_len - pos);
    memcpy(g_dlc_etud + pos, ins, (size_t)ilen);
    g_dlc_etud_len = newlen;
    g_dlc_etud[g_dlc_etud_len] = '\0';
    return 1;
}

/* 从 etud 里删掉对应 Mount（连同前后空白） */
static int dlc_etud_del(const char *name) {
    if (!g_dlc_etud) return -1;
    char pat[160];
    snprintf(pat, sizeof(pat), "dlc_%s:/", name);
    size_t pl = strlen(pat);

    for (size_t i = 0; i + pl <= g_dlc_etud_len; i++) {
        int ok = 1;
        for (size_t k = 0; k < pl; k++) {
            char a = g_dlc_etud[i + k], b = pat[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = 0; break; }
        }
        if (!ok) continue;

        /* 向前找本 <Item 的开头（★ 必须用 dlc_tag_before：
         *   名字前面最近的是 <deviceName>，不能用「往前找 '<'」） */
        size_t s = dlc_tag_before(g_dlc_etud, i, "<Item");
        if (s == (size_t)-1) continue;
        if (s + 6 > g_dlc_etud_len) continue;
        if (memcmp(g_dlc_etud + s, "<Item ", 6) != 0 &&
            memcmp(g_dlc_etud + s, "<item ", 6) != 0) continue;
        /* 向后找本 </Item> 的结尾 */
        size_t e = i;
        while (e + 7 <= g_dlc_etud_len &&
               memcmp(g_dlc_etud + e, "</Item>", 7) != 0) e++;
        if (e + 7 > g_dlc_etud_len) continue;
        e += 7;
        /* 吃掉后面的换行/缩进 */
        while (e < g_dlc_etud_len &&
               (g_dlc_etud[e] == '\r' || g_dlc_etud[e] == '\n' ||
                g_dlc_etud[e] == '\t' || g_dlc_etud[e] == ' ')) e++;
        /* 吃掉前面的行首空白 */
        while (s > 0 && (g_dlc_etud[s - 1] == '\t' || g_dlc_etud[s - 1] == ' ')) s--;

        size_t cut = e - s;
        memmove(g_dlc_etud + s, g_dlc_etud + e, g_dlc_etud_len - e);
        g_dlc_etud_len -= cut;
        g_dlc_etud[g_dlc_etud_len] = '\0';
        return 1;
    }
    return 0;
}

/* 把改过的 dlclist.xml 写回 update.rpf
 * ★★ 必须用 gc_rpf_write_ex（允许搬家）：
 *    dlclist.xml 在原位只有 512 B 物理余量（已用 491 B），加 3 个 DLC 就爆。
 *    gc_rpf_write（不允许搬家）会在第 3 个 DLC 上报「超出物理可用空间」。
 * ★ raw deflate 由 gc_rpf_write_ex 内部处理（wbits=-15）。 */
static int dlc_save_dlclist(void) {
    if (!g_dlc_xml_loaded || !g_dlc_xml) return -1;
    if (!gc_rpf_writable(&g_gc_rpf)) {
        dlc_err("update.rpf 是只读打开的，无法写入");
        return GC_ERR_READONLY;
    }
    int rc = gc_rpf_write_ex(&g_gc_rpf, &g_dlc_xml_ent,
                             (const u8 *)g_dlc_xml, g_dlc_xml_len, 1);
    if (rc != GC_OK) {
        dlc_err("写入 dlclist.xml 失败 (rc=%d): %s", rc, gc_rpf_last_error());
        return rc;
    }
    return GC_OK;
}

/* 把改过的 etud 写回 update.rpf */
static int dlc_save_etud(void) {
    if (!g_dlc_etud_loaded || !g_dlc_etud) return GC_OK;   /* 没有就不写 */
    if (!gc_rpf_writable(&g_gc_rpf)) {
        dlc_err("update.rpf 是只读打开的，无法写入");
        return GC_ERR_READONLY;
    }
    int rc = gc_rpf_write_ex(&g_gc_rpf, &g_dlc_etud_ent,
                             (const u8 *)g_dlc_etud, g_dlc_etud_len, 1);
    if (rc != GC_OK) {
        dlc_err("写入 extratitleupdatedata.meta 失败 (rc=%d): %s",
                rc, gc_rpf_last_error());
        return rc;
    }
    return GC_OK;
}

/* 一键刷新：扫描 + 读两个 xml + 更新注册状态 */
static int dlc_reload(void) {
    if (dlc_scan_dlcpacks() != 0) return -1;
    if (dlc_load_dlclist() != GC_OK) return -1;
    dlc_refresh_registered();
    int full = 0;
    for (int i = 0; i < g_dlc_count; i++) if (g_dlc_items[i].full) full++;
    dlc_msg("扫描完成：%d 个 DLC，其中 %d 个已完整注册", g_dlc_count, full);
    return 0;
}

/* ★ 切换选中项的注册状态（同时改 dlclist + etud 两处，缺一游戏不认） */
static int dlc_toggle_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err("没有选中项"); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    if (!g_dlc_xml_loaded) {
        dlc_err("dlclist.xml 未载入，先按 X 扫描"); return -1;
    }

    int want = !it->full;    /* 目标状态：两处齐 = 已注册 */
    int rc;

    if (want) {
        /* ---- 注册：dlclist + etud 都加 ---- */
        if (dlc_xml_add(it->name) < 0) { dlc_err("dlclist 注册失败（空间或格式问题）"); return -1; }
        if (g_dlc_etud_loaded) {
            if (dlc_etud_add(it->name) < 0) {
                /* etud 失败要回滚 dlclist，保持两处一致 */
                dlc_xml_del(it->name);
                dlc_err("etud 挂载失败（找不到 </Mounts>？）"); return -1;
            }
        }
        rc = dlc_save_dlclist();
        if (rc == GC_OK) rc = dlc_save_etud();
        if (rc != GC_OK) {
            /* 写盘失败也要回滚内存 */
            dlc_xml_del(it->name);
            if (g_dlc_etud_loaded) dlc_etud_del(it->name);
            return rc;
        }
    } else {
        /* ---- 注销：两处都删 ---- */
        int a = dlc_xml_del(it->name);
        int b = g_dlc_etud_loaded ? dlc_etud_del(it->name) : 0;
        if (a <= 0 && b <= 0) { dlc_err("注销失败：两处都没找到条目"); return -1; }
        rc = dlc_save_dlclist();
        if (rc == GC_OK) rc = dlc_save_etud();
        if (rc != GC_OK) return rc;
    }

    /* ---- 回读校验（两处都验） ---- */
    int v1 = gc_rpf_verify(&g_gc_rpf, &g_dlc_xml_ent,
                           (const u8 *)g_dlc_xml, g_dlc_xml_len);
    int v2 = g_dlc_etud_loaded
           ? gc_rpf_verify(&g_gc_rpf, &g_dlc_etud_ent,
                           (const u8 *)g_dlc_etud, g_dlc_etud_len)
           : GC_OK;

    it->registered = dlc_is_registered(it->name);
    it->mounted    = dlc_is_mounted(it->name);
    it->full       = (it->registered && it->mounted) ? 1 : 0;

    if (v1 == GC_OK && v2 == GC_OK) {
        dlc_msg("%s 已%s（dlclist %s / etud %s，回读校验通过）", it->name,
                it->full ? "注册" : "注销",
                it->registered ? "有" : "无",
                it->mounted ? "有" : "无");
        g_dlc_msg_ok = 1;
    } else {
        dlc_msg("%s 已%s，但回读校验失败 (dlclist=%d etud=%d)", it->name,
                it->full ? "注册" : "注销", v1, v2);
        g_dlc_msg_ok = 0;
    }
    g_dlc_dirty = 0;
    return 0;
}

/* ★★ 从待导入目录复制一个 dlcpack 到 dlcpacks/，然后注册
 *    目录递归复制（dlc.rpf 可能有几十 MB，用 64 KB 缓冲流式拷贝）
 *    ★ v22：带进度回调（每块都刷，用户能看到「在动」） */
static int dlc_copy_tree_ex(const char *src, const char *dst,
                            long long *done, long long total,
                            const char *what,
                            char *err, size_t errsz, int depth) {
    if (depth > 8) { snprintf(err, errsz, "目录嵌套过深"); return -1; }
    DIR *d = opendir(src);
    if (!d) { snprintf(err, errsz, "打不开源目录"); return -1; }
    mkdir(dst, 0777);
    struct dirent *ent;
    int files = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sp[600], dp[600];
        snprintf(sp, sizeof(sp), "%s/%s", src, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, ent->d_name);

        struct stat st;
        if (stat(sp, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            int r = dlc_copy_tree_ex(sp, dp, done, total, what, err, errsz, depth + 1);
            if (r < 0) { closedir(d); return -1; }
            files += r;
        } else {
            FILE *fi = fopen(sp, "rb");
            if (!fi) { snprintf(err, errsz, "打不开 %s", ent->d_name); closedir(d); return -1; }
            FILE *fo = fopen(dp, "wb");
            if (!fo) { fclose(fi); snprintf(err, errsz, "无法创建 %s", ent->d_name); closedir(d); return -1; }
            static u8 cbuf[65536];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), fi)) > 0) {
                if (fwrite(cbuf, 1, n, fo) != n) {
                    fclose(fi); fclose(fo);
                    snprintf(err, errsz, "写入 %s 失败", ent->d_name);
                    closedir(d); return -1;
                }
                if (done) {
                    *done += (long long)n;
                    dlc_prog_frame();     /* ★ 每块都刷（内部限频 20FPS） */
                }
            }
            fclose(fi);
            fclose(fo);
            files++;
            if (done && what) {
                char sub2[192];
                snprintf(sub2, sizeof(sub2), "%s：%lld / %lld KB",
                         what, *done / 1024, total / 1024);
                dlc_prog_set(NULL, sub2, total > 0 ? (int)(*done * 100 / total) : -1);
            }
        }
    }
    closedir(d);
    if (files == 0) { snprintf(err, errsz, "源目录是空的"); return -1; }
    return files;
}

static int dlc_copy_tree(const char *src, const char *dst, char *err, size_t errsz) {
    long long done = 0, total = 0;
    /* 先算总大小（用于进度百分比） */
    DIR *d = opendir(src);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", src, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) total += st.st_size;
        }
        closedir(d);
    }
    return dlc_copy_tree_ex(src, dst, &done, total, "复制文件", err, errsz, 0);
}

/* 扫描候选目录，列出可导入的 dlcpack（含子目录里的 dlc.rpf 才算） */
static int dlc_scan_src(void) {
    g_dlc_src_count = 0;
    g_dlc_src_dir_n = 0;

    for (int di = 0; di < DLC_SRC_DIR_MAX; di++) {
        const char *base = DLC_SRC_DIRS[di];
        DIR *d = opendir(base);
        if (!d) continue;
        int hit_dir = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && g_dlc_src_count < DLC_SRC_MAX) {
            if (ent->d_name[0] == '.') continue;
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s", base, ent->d_name);
            if (!dlc_dir_exists(sub)) continue;

            /* 必须含 dlc.rpf 才算 dlcpack */
            char rpf[700];
            snprintf(rpf, sizeof(rpf), "%s/dlc.rpf", sub);
            long sz = dlc_file_size(rpf);
            if (sz <= 0) continue;

            /* 已在 dlcpacks 里装了同名目录 ⇒ 跳过（避免重复） */
            int dup = 0;
            for (int k = 0; k < g_dlc_count; k++)
                if (strcasecmp(g_dlc_items[k].name, ent->d_name) == 0) { dup = 1; break; }
            if (dup) continue;

            DlcItem *it = &g_dlc_src[g_dlc_src_count];
            memset(it, 0, sizeof(*it));
            strncpy(it->name, ent->d_name, DLC_NAME_MAX - 1);
            it->has_rpf  = 1;
            it->rpf_size = (u32)sz;
            /* ★ v5.1: 中文显示名（同一约定） */
            {
                char sdir[768];
                if (dlc_src_path_of(it->name, sdir, sizeof(sdir)) == 0)
                    dlc_read_cn_name(sdir, it->cn, sizeof(it->cn));
            }
            /* ★ v33: 资源构成（用统一 probe，一次 IO；PC/Switch 都能统计） */
            {
                char srp[700], rp2[760];
                if (dlc_src_path_of(it->name, srp, sizeof(srp)) == 0) {
                    snprintf(rp2, sizeof(rp2), "%s/dlc.rpf", srp);
                    GcDlcProbe pb;
                    gc_dlc_probe(rp2, &pb);
                    it->n_model = pb.n_model;
                    it->n_map   = pb.n_map;
                    it->n_tex   = pb.n_tex;
                    snprintf(it->spawn, sizeof(it->spawn), "%s", pb.vehicle);
                }
            }
            g_dlc_src_count++;
            hit_dir = 1;
        }
        closedir(d);
        if (hit_dir && g_dlc_src_dir_n < DLC_SRC_DIR_MAX) {
            snprintf(g_dlc_src_dir[g_dlc_src_dir_n], 256, "%s", base);
            g_dlc_src_dir_n++;
        }
    }

    /* 排序 */
    for (int i = 0; i < g_dlc_src_count; i++)
        for (int j = i + 1; j < g_dlc_src_count; j++)
            if (strcmp(g_dlc_src[i].name, g_dlc_src[j].name) > 0) {
                DlcItem t = g_dlc_src[i]; g_dlc_src[i] = g_dlc_src[j]; g_dlc_src[j] = t;
            }
    if (g_dlc_src_count == 0) {
        dlc_msg("没找到待导入的 dlcpack。请把 dlcpack 目录（含 dlc.rpf）"
                "放到 sdmc:/switch/gta5save/dlc/ 下");
        g_dlc_msg_ok = 0;
        return -1;
    }
    dlc_msg("找到 %d 个待导入 dlcpack（来源 %d 个目录）", g_dlc_src_count, g_dlc_src_dir_n);
    g_dlc_msg_ok = 1;
    return 0;
}

/* 记录「这个名字是从哪个源目录来的」，便于导入时找到源路径 */
static int dlc_src_path_of(const char *name, char *out, size_t outsz) {
    for (int di = 0; di < DLC_SRC_DIR_MAX; di++) {
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", DLC_SRC_DIRS[di], name);
        if (dlc_dir_exists(p)) { snprintf(out, outsz, "%s", p); return 0; }
    }
    return -1;
}

/* ★★ 导入选中的待导入 dlcpack：
 *    (1) 递归复制目录到 dlcpacks/
 *    (1).5 ★ 检测平台格式，PC 格式就地转成 Switch 格式（否则游戏必闪退）
 *    (2) 写进 dlclist.xml + extratitleupdatedata.meta（两处注册）
 *    (3) 回读校验 + 完整性检查
 *
 * 🚨 为什么要转格式（用户实测踩坑）：
 *   直接把 PC 版 mod 丢进 dlcpacks/ ⇒ 游戏去 switch/levels/... 找车，
 *   包里却是 x64/levels/... 且资源是 .yft/.ytd ⇒ 找不到 ⇒ 启动闪退。
 *   原包 readme 也写明 "designed for the PC version"，落点是 update\x64\dlcpacks。
 */
static int dlc_import_selected(void) {
    if (g_dlc_src_sel < 0 || g_dlc_src_sel >= g_dlc_src_count) {
        dlc_err("没有选中待导入项"); return -1;
    }
    if (!g_dlc_xml_loaded) {
        if (dlc_load_dlclist() != GC_OK) return -1;
    }
    const char *name = g_dlc_src[g_dlc_src_sel].name;

    char src[700];
    if (dlc_src_path_of(name, src, sizeof(src)) != 0) {
        dlc_err("找不到 %s 的源目录", name); return -1;
    }
    char dst[700];
    snprintf(dst, sizeof(dst), "%s/%s", DLC_DLCPACKS, name);

    /* ---- (1) 复制（带进度） ---- */
    dlc_prog_begin("正在导入 DLC", name);
    char err[256] = {0};
    int nf = dlc_copy_tree(src, dst, err, sizeof(err));
    if (nf < 0) {
        dlc_prog_end();
        dlc_err("复制失败: %s", err);
        return -1;
    }
    fsdevCommitDevice("sdmc");

    /* ---- (1).5 ★ 平台格式检测 + 自动转换 ---- */
    char rpfpath[760];
    snprintf(rpfpath, sizeof(rpfpath), "%s/dlc.rpf", dst);
    int fmt = gc_dlc_is_switch_format(rpfpath);
    GcDlcStats cst; memset(&cst, 0, sizeof(cst));
    int converted = 0;
    if (fmt == 0) {
        /* 是 PC 格式 ⇒ 必须转，否则游戏闪退 */
        dlc_prog_set("正在转换为 Switch 格式", "扫描平台目录与资源扩展名...", -1);
        dlc_prog_frame();
        char cerr[256] = {0};
        int crc = gc_dlc_convert(rpfpath, &cst, cerr, sizeof(cerr));
        if (crc != GC_DLC_OK) {
            dlc_prog_end();
            dlc_err("★ 格式转换失败（这个包是 PC 格式，不转换会闪退）：%s", cerr);
            return -1;
        }
        converted = 1;
        fsdevCommitDevice("sdmc");
        /* 转换后再确认一次 */
        int fmt2 = gc_dlc_is_switch_format(rpfpath);
        if (fmt2 != 1) {
            dlc_prog_end();
            dlc_err("★ 转换后仍不是 Switch 格式，已中止（避免闪退）");
            return -1;
        }
    }

    /* ---- (2) 注册 ---- */
    dlc_prog_set("正在注册", "写 dlclist.xml + extratitleupdatedata.meta...", -1);
    dlc_prog_frame();
    if (dlc_xml_add(name) < 0) {
        dlc_prog_end(); dlc_err("复制好了，但 dlclist 注册失败"); return -1;
    }
    if (g_dlc_etud_loaded && dlc_etud_add(name) < 0) {
        dlc_prog_end(); dlc_err("复制好了，但 etud 挂载失败"); return -1;
    }
    int rc = dlc_save_dlclist();
    if (rc == GC_OK) rc = dlc_save_etud();
    if (rc != GC_OK) {
        dlc_prog_end();
        dlc_err("复制好了，但注册写盘失败 (rc=%d)", rc);
        return rc;
    }

    /* ---- (3) 回读校验 ---- */
    dlc_prog_set("正在校验", "回读 update.rpf 比对...", -1);
    dlc_prog_frame();
    int v1 = gc_rpf_verify(&g_gc_rpf, &g_dlc_xml_ent,
                           (const u8 *)g_dlc_xml, g_dlc_xml_len);
    int v2 = g_dlc_etud_loaded
           ? gc_rpf_verify(&g_gc_rpf, &g_dlc_etud_ent,
                           (const u8 *)g_dlc_etud, g_dlc_etud_len)
           : GC_OK;

    /* ---- (4) 完整性检查（缺 _hi 会闪退） ---- */
    dlc_prog_set("正在检查完整性", "车辆资源是否齐全...", -1);
    dlc_prog_frame();
    char chk[256] = {0};
    dlc_check_integrity(name, chk, sizeof(chk));

    dlc_prog_end();

    if (converted) {
        dlc_msg("[v] %s 导入完成：%d 文件｜★已转 Switch 格式（改名 %d 处：顶层 %d + 内嵌 %d）"
                "｜dlclist %s / etud %s%s%s",
                name, nf, cst.n_top_renamed + cst.n_inner_renamed,
                cst.n_top_renamed, cst.n_inner_renamed,
                v1 == GC_OK ? "OK" : "失败", v2 == GC_OK ? "OK" : "失败",
                chk[0] ? "  |  " : "", chk);
    } else {
        dlc_msg("[v] %s 导入完成：%d 个文件（已是 Switch 格式）"
                "｜dlclist %s / etud %s%s%s",
                name, nf,
                v1 == GC_OK ? "OK" : "失败", v2 == GC_OK ? "OK" : "失败",
                chk[0] ? "  |  " : "", chk);
    }
    g_dlc_msg_ok = (v1 == GC_OK && v2 == GC_OK) ? 1 : 0;
    if (cst.n_skipped_compressed > 0) {
        dlc_err("注意：有 %d 个压缩存储的内嵌 rpf 未处理，可能影响兼容性",
                cst.n_skipped_compressed);
        g_dlc_msg_ok = 0;
    }

    /* 刷新列表 */
    dlc_scan_dlcpacks();
    dlc_scan_src();
    dlc_refresh_registered();
    g_dlc_dirty = 0;
    return 0;
}

/* 判「包是否已经是 Switch 格式」的包装（带缓存友好：只读 TOC+names） */
static int dlc_format_of(const char *name) {
    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, name);
    return gc_dlc_is_switch_format(rpf);
}

/* ★ v31: 地图数据自检的包装（只读 RPF 头部，很快） */
static int dlc_check_map_of(const char *name) {
    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, name);
    return gc_dlc_check_map(rpf);
}

/* ★★ 对「已装但格式不对」的包就地转换（不重新导入）
 *    用途：用户之前用旧版工具导入了 PC 格式的包，现在一键修好 */
static int dlc_convert_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err("没有选中项"); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    if (!it->has_rpf) { dlc_err("%s 没有 dlc.rpf", it->name); return -1; }
    if (it->fmt == 1)  { dlc_err("%s 已经是 Switch 格式，无需转换", it->name); return -1; }

    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, it->name);

    dlc_prog_begin("正在转换为 Switch 格式", it->name);
    dlc_prog_frame();
    char err[256] = {0};
    GcDlcStats st; memset(&st, 0, sizeof(st));
    int rc = gc_dlc_convert(rpf, &st, err, sizeof(err));
    dlc_prog_end();

    if (rc != GC_DLC_OK) {
        dlc_err("%s 转换失败：%s", it->name, err);
        return rc;
    }
    fsdevCommitDevice("sdmc");
    /* ★ v33: 转格式后一次刷新全部信息（格式 / 地图自检 / 资源构成） */
    {
        char rpf2[400];
        snprintf(rpf2, sizeof(rpf2), "%s/%s/dlc.rpf", DLC_DLCPACKS, it->name);
        GcDlcProbe pb;
        gc_dlc_probe(rpf2, &pb);
        it->fmt     = pb.fmt;
        it->map_st  = pb.map_st;
        it->n_model = pb.n_model;
        it->n_map   = pb.n_map;
        it->n_tex   = pb.n_tex;
        snprintf(it->spawn, sizeof(it->spawn), "%s", pb.vehicle);
    }
    int fmt2 = it->fmt;
    if (fmt2 != 1) {
        dlc_err("%s 转换后仍不是 Switch 格式，请检查这个包", it->name);
        return -1;
    }
    dlc_msg("[v] %s 已转为 Switch 格式（改名 %d 处：顶层 %d + 内嵌 %d，名字池 +%d B）"
            "%s  重启游戏生效",
            it->name, st.n_top_renamed + st.n_inner_renamed,
            st.n_top_renamed, st.n_inner_renamed, st.names_grew,
            st.n_hd_removed > 0 ? "  [已删高清贴图]" : "");
    g_dlc_msg_ok = 1;
    return 0;
}

/* ★ v5.4: 屏蔽高清贴图（+hi.ytd 系）—— 对标官方 GTAVPatcher 的
 *   RemoveHighDetailTextures（其 GUI 原文：「Delete high-detail (+hi) resources
 *   - recommended for Switch」）。
 *
 *   注意：删的是【高清贴图】，不是 _hi.nft【模型】——后者是引擎形变必需，
 *         删了必闪退。两者是完全不同的东西，不要混。
 *   实现 = 名字首字符 '+' → '~'（等长改名屏蔽）；可逆，不搬数据。
 *   用于已装列表：对选中 mod 执行；已是 Switch 格式也能用。 */
static int dlc_hide_hd_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err("没有选中项"); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    char name[DLC_NAME_MAX];
    snprintf(name, sizeof(name), "%s", it->name);
    if (!it->has_rpf) { dlc_err("%s 没有 dlc.rpf", name); return -1; }

    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, name);

    dlc_prog_begin("正在屏蔽高清贴图(+hi)", name);
    dlc_prog_frame();
    char err[256] = {0};
    int removed = 0;
    int rc = gc_dlc_hide_hd_textures(rpf, &removed, err, sizeof(err));
    dlc_prog_end();

    if (rc < 0) {
        dlc_err("%s 处理失败：%s", name, err[0] ? err : "未知错误");
        return rc;
    }
    fsdevCommitDevice("sdmc");

    /* 刷新该项的资源构成（贴图数会减少） */
    GcDlcProbe pb;
    gc_dlc_probe(rpf, &pb);
    it->n_model = pb.n_model;
    it->n_map   = pb.n_map;
    it->n_tex   = pb.n_tex;

    if (removed > 0) {
        dlc_msg("[v] %s 已屏蔽 %d 个高清贴图(+hi.ytd 系)，重启游戏生效", name, removed);
        g_dlc_msg_ok = 1;
    } else {
        dlc_msg("%s 没有高清贴图可屏蔽（已处理过，或本来就没有）", name);
    }
    return removed;
}

/* ★ v5.4: 模型目录信息 —— 列出 SD 卡上 3 个相关目录的路径与内容统计
 *   Switch 无文件管理器，这里把路径直接给出来，方便用户在电脑上找。
 *   统计每个目录下的子目录数（= 装了/待装的 mod 数）。 */
static void dlc_show_modeldir(void) {
    static const struct { const char *p; const char *lab; } D[] = {
        { DLC_DLCPACKS,               "已装模型 dlcpacks" },
        { "sdmc:/switch/gta5save/dlc","待导入 dlc" },
        { SCRIPT_DIR,                 "脚本 mod .nsc" },
    };
    int n = (int)(sizeof(D)/sizeof(D[0]));
    char buf[512];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "模型目录（共 %d 个）：", n);
    for (int k = 0; k < n; k++) {
        int cnt = 0;
        DIR *d = opendir(D[k].p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                cnt++;
            }
            closedir(d);
        }
        off += snprintf(buf + off, sizeof(buf) - off,
                        "  %s [%d项] %s |", D[k].lab, cnt, D[k].p);
        if (off >= (int)sizeof(buf) - 8) break;
    }
    dlc_msg("%s", buf);
}

/* =========================================================================
 * ★★ v25: 删除 DLC
 *
 * 「删除」= (1) 注销（dlclist.xml + extratitleupdatedata.meta 两处都删）
 *          (2) 递归清空 dlcpacks/<名字>/ 目录
 *
 * 🚨 SD 卡权限矩阵（实测）：【预存的】文件/目录删除会被系统拒绝。
 *   dlcpacks 下的目录绝大多数是用户用电脑拷进去的 ⇒ 属于「预存」。
 *   ⇒ 删不掉是【预期行为】，不是 bug：统计失败数后明确告诉用户
 *     「请用电脑删除该目录」，并且注册已经注销，游戏不会再加载它。
 *   （本工具导入时新建的目录理论可删，同样走这条路径，能删就删。）
 * ========================================================================= */

/* 递归删除目录：能删就删，删不掉的计入 *failcnt（不中断，继续删下一个） */
static void dlc_remove_tree(const char *path, int *failcnt) {
    DIR *d = opendir(path);
    if (!d) {
        if (failcnt) (*failcnt)++;
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sub[700];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            dlc_remove_tree(sub, failcnt);
        } else {
            if (remove(sub) != 0) {
                if (failcnt) (*failcnt)++;
            }
        }
    }
    closedir(d);
    if (rmdir(path) != 0) {
        if (failcnt) (*failcnt)++;
    }
}

/* ★ 弹出删除确认（真正动手在 dlc_do_delete，由输入分支在 A 键时调用） */
static int dlc_delete_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err("没有选中项"); return -1;
    }
    if (!g_dlc_xml_loaded) {
        dlc_err("先点「载入并扫描」再操作"); return -1;
    }
    g_dlc_confirm = 1;
    return 0;
}

/* ★ 确认后的真正删除：注销 -> 删目录 -> 刷新列表 */
static int dlc_do_delete(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err("没有选中项"); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    /* ★ 名字必须拷出来：dlc_scan_dlcpacks() 会重写 g_dlc_items[]，
     *   后面刷新列表之后还要用它拼提示信息（否则是悬挂指针） */
    char name[DLC_NAME_MAX];
    snprintf(name, sizeof(name), "%s", it->name);

    /* ---- (1) 注销两处注册（不在乎当前是否已注册，删不到就算了） ---- */
    dlc_xml_del(name);
    if (g_dlc_etud_loaded) dlc_etud_del(name);
    int rc = dlc_save_dlclist();
    if (rc == GC_OK) rc = dlc_save_etud();
    if (rc != GC_OK) {
        dlc_err("%s: 注销写盘失败 (rc=%d)，已中止删除", name, rc);
        return rc;
    }

    /* ---- (2) 递归删目录（预存的可能删不掉，统计失败数） ---- */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", DLC_DLCPACKS, name);
    int fail = 0;
    dlc_remove_tree(dir, &fail);
    fsdevCommitDevice("sdmc");

    /* ---- (3) 回读校验（确认两处注册确实没了） ---- */
    int v1 = gc_rpf_verify(&g_gc_rpf, &g_dlc_xml_ent,
                           (const u8 *)g_dlc_xml, g_dlc_xml_len);
    int v2 = g_dlc_etud_loaded
           ? gc_rpf_verify(&g_gc_rpf, &g_dlc_etud_ent,
                           (const u8 *)g_dlc_etud, g_dlc_etud_len)
           : GC_OK;

    /* ---- (4) 刷新列表（选中项后移，别悬空） ---- */
    dlc_scan_dlcpacks();
    dlc_refresh_registered();
    dlc_scan_src();
    if (g_dlc_sel >= g_dlc_count) g_dlc_sel = g_dlc_count > 0 ? g_dlc_count - 1 : 0;

    if (fail > 0) {
        /* 🚨 SD 卡权限限制：预存文件删不掉是正常的，注册已注销 */
        dlc_err("[v] %s 已注销（校验 %s/%s），但 %d 个文件/子目录删不掉"
                "（SD 卡权限限制），请用电脑删除：%s",
                name, v1 == GC_OK ? "OK" : "失败", v2 == GC_OK ? "OK" : "失败",
                fail, dir);
        return -1;
    }
    dlc_msg("[v] %s 已彻底删除（注销两处注册 + 清空目录，校验 %s/%s）",
            name, v1 == GC_OK ? "OK" : "失败", v2 == GC_OK ? "OK" : "失败");
    g_dlc_msg_ok = (v1 == GC_OK && v2 == GC_OK) ? 1 : 0;
    return 0;
}

/* ★ 从内存里的 rpf 数据统计车辆资源配对情况
 *   为什么要下钻：dlcpack 的 dlc.rpf 外层只放 .meta / content.xml，
 *   真正的车辆资源在内嵌的 vehicles.rpf 里（未压缩存储）。
 *   踩过：只扫外层 ⇒ checked==0 ⇒ 误报「该 DLC 里没有车辆资源」。
 *
 * blob  = 完整 rpf 数据（含 16 B header）
 * 返回  1 = 有资源且配对齐全；0 = 无车辆资源；-1 = 缺 _hi */
static int dlc_count_vehicles(const u8 *blob, size_t blen,
                              int *out_model, int *out_hi, int depth) {
    if (depth > 4 || blen < 16) return 0;
    u32 magic = (u32)blob[0] | ((u32)blob[1] << 8) |
                ((u32)blob[2] << 16) | ((u32)blob[3] << 24);
    if (magic != 0x52504637u) return 0;      /* 不是 RPF7 */

    u32 ec   = (u32)blob[4] | ((u32)blob[5] << 8) |
               ((u32)blob[6] << 16) | ((u32)blob[7] << 24);
    u32 nl   = (u32)blob[8] | ((u32)blob[9] << 8) |
               ((u32)blob[10] << 16) | ((u32)blob[11] << 24);
    u32 nlen = nl & 0x0FFFFFFFu;
    u32 sh   = nl >> 28;
    if (ec == 0 || ec > 200000) return 0;

    size_t toc_off = 16;
    size_t names_off = toc_off + (size_t)ec * 16;
    if (names_off + nlen > blen) return 0;

    int model = 0, hi = 0, nested = 0;

    for (u32 i = 0; i < ec; i++) {
        const u8 *p = blob + toc_off + (size_t)i * 16;
        u64 q0 = (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24)
               | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48)
               | ((u64)p[7] << 56);
        u32 name_off = (u32)(q0 & 0xFFFF);
        u32 od       = (u32)((q0 >> 16) & 0xFFFFFF);
        u32 off      = (u32)((q0 >> 40) & 0x7FFFFF);
        int is_res   = (int)((q0 >> 63) & 1);
        u32 d8       = (u32)p[8] | ((u32)p[9] << 8) | ((u32)p[10] << 16) | ((u32)p[11] << 24);
        if (is_res == 0 && off == 0x7FFFFF) continue;      /* 目录 */

        /* 名字（按 NUL 严格切分） */
        u32 no = name_off << sh;
        if (no >= nlen) continue;
        const char *nm = (const char *)(blob + names_off + no);
        size_t maxn = nlen - no;
        size_t ln = 0;
        while (ln < maxn && ln < 127 && nm[ln]) ln++;
        if (ln == 0) continue;

        /* 车辆模型：xxx.nft / xxx.yft */
        if (ln > 4) {
            char tail[5];
            memcpy(tail, nm + ln - 4, 4); tail[4] = 0;
            if (strcasecmp(tail, ".nft") == 0 || strcasecmp(tail, ".yft") == 0) {
                int is_hi = 0;
                if (ln > 7) {
                    char t7[8];
                    memcpy(t7, nm + ln - 7, 7); t7[7] = 0;
                    if (strcasecmp(t7, "_hi.nft") == 0 ||
                        strcasecmp(t7, "_hi.yft") == 0) is_hi = 1;
                }
                if (is_hi) hi++; else model++;
                continue;
            }
        }

        /* 内嵌 rpf：递归下钻（通常是未压缩存储 od==0） */
        if (!is_res && ln > 4) {
            char tail[5];
            memcpy(tail, nm + ln - 4, 4); tail[4] = 0;
            if (strcasecmp(tail, ".rpf") == 0) {
                u32 nbytes = od ? od : d8;
                size_t st = (size_t)off * 512;
                if (nbytes > 0 && st + nbytes <= blen) {
                    const u8 *sub = blob + st;
                    if (od != 0 && od != d8) {
                        /* 压缩的内嵌 rpf：解压到临时缓冲再递归 */
                        u8 *tmp = (u8*)malloc(d8);
                        if (tmp) {
                            z_stream zs; memset(&zs, 0, sizeof(zs));
                            int ok = 0;
                            for (int wb = -15; wb <= 15 && !ok; wb += 30) {
                                memset(&zs, 0, sizeof(zs));
                                if (inflateInit2(&zs, wb) != Z_OK) continue;
                                zs.next_in = (Bytef*)sub;  zs.avail_in  = nbytes;
                                zs.next_out = tmp;         zs.avail_out = d8;
                                int r = inflate(&zs, Z_FINISH);
                                if ((r == Z_STREAM_END || r == Z_OK) && zs.total_out > 0)
                                    ok = (int)zs.total_out;
                                inflateEnd(&zs);
                            }
                            if (ok > 0) {
                                nested += dlc_count_vehicles(tmp, (size_t)ok,
                                                             &model, &hi, depth + 1);
                            }
                            free(tmp);
                        }
                    } else {
                        nested += dlc_count_vehicles(sub, nbytes, &model, &hi, depth + 1);
                    }
                }
            }
        }
    }
    if (out_model) *out_model = model;
    if (out_hi)    *out_hi    = hi;
    return (model + hi + nested) > 0 ? 1 : 0;
}

/* ★ 完整性检查：看 dlc.rpf（含内嵌 rpf）里每个车辆资源是否齐全
 *   规则：每个 xxx.nft 必须有对应的 xxx_hi.nft
 *   （★ 官方 212 辆车 100% 都有 _hi，缺了会在撞击/近距离时闪退） */
static int dlc_check_integrity(const char *name, char *out, size_t out_sz) {
    char p[400];
    snprintf(p, sizeof(p), "%s/%s/dlc.rpf", DLC_DLCPACKS, name);

    /* 直接用 fopen 读整个 dlc.rpf（十几 MB，一次读进内存最省事） */
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(out, out_sz, "打不开 dlc.rpf（路径或文件缺失）");
        return -1;
    }
    fseeko(f, 0, SEEK_END);
    off_t fsz = ftello(f);
    fseeko(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 256 * 1024 * 1024) {
        fclose(f);
        snprintf(out, out_sz, "dlc.rpf 大小异常 (%ld B)", (long)fsz);
        return -1;
    }
    u8 *blob = (u8*)malloc((size_t)fsz);
    if (!blob) { fclose(f); snprintf(out, out_sz, "内存不足"); return -1; }
    size_t got = fread(blob, 1, (size_t)fsz, f);
    fclose(f);
    if (got != (size_t)fsz) {
        free(blob);
        snprintf(out, out_sz, "读取不完整 (%zu/%ld B)", got, (long)fsz);
        return -1;
    }

    int model = 0, hi = 0;
    int has = dlc_count_vehicles(blob, got, &model, &hi, 0);
    free(blob);

    if (!has || (model + hi) == 0) {
        snprintf(out, out_sz, "该 DLC 里没有车辆资源（可能是地图/音频/武器包，正常）");
        return 0;
    }
    if (model != hi) {
        int d = model - hi; if (d < 0) d = -d;
        snprintf(out, out_sz,
                 "注意 缺 _hi 高模！模型 %d / 高模 %d（差 %d）-- 撞击时会闪退",
                 model, hi, d);
        return -1;
    }
    snprintf(out, out_sz, "[v] 车辆资源齐全（%d 辆车，模型/高模配对）", model);
    return 0;
}

/* ========================================================================= */
/* ★ v6: 性能调参 (gameconfig.xml) -- 核心逻辑                                */
/* ========================================================================= */

/* 记录/更新「已手动改过」的旋钮 */
static void gc_mark_edited(const char *key, int val) {
    for (int i = 0; i < g_gc_edited_n; i++) {
        if (strcmp(g_gc_edited_key[i], key) == 0) { g_gc_edited_val[i] = val; return; }
    }
    if (g_gc_edited_n < GC_MAX_EDITED) {
        strncpy(g_gc_edited_key[g_gc_edited_n], key, sizeof(g_gc_edited_key[0]) - 1);
        g_gc_edited_key[g_gc_edited_n][sizeof(g_gc_edited_key[0]) - 1] = '\0';
        g_gc_edited_val[g_gc_edited_n] = val;
        g_gc_edited_n++;
    }
}

static int gc_get_edited(const char *key) {
    for (int i = 0; i < g_gc_edited_n; i++)
        if (strcmp(g_gc_edited_key[i], key) == 0) return g_gc_edited_val[i];
    return INT32_MIN;
}

static void gc_clear_edited(void) { g_gc_edited_n = 0; }

/* 打开 update.rpf 并读取 gameconfig.xml */
static int gc_open_rpf(void) {
    if (g_gc_opened) return GC_OK;
    g_gc_diag_n = 0;   /* 诊断信息作废，重新生成 */

    int rc = GC_ERR_OPEN;
    for (int i = 0; i < GC_NUM_CANDIDATES; i++) {
        rc = gc_rpf_open(gc_rpf_candidates[i], &g_gc_rpf);
        if (rc == GC_OK) break;
    }
    if (rc != GC_OK) {
        g_gc_err = rc;
        gc_build_diag();   /* 立刻探测各路径，供界面显示 */
        gc_msg("打开 update.rpf 失败: %s", gc_rpf_last_error());
        g_gc_msg_color = C_RED;
        return rc;
    }

    /* 找 gameconfig.xml（条目名不带路径，直接按文件名找） */
    rc = gc_rpf_find(&g_gc_rpf, "gameconfig.xml", &g_gc_ent);
    if (rc != GC_OK) {
        gc_msg("在 update.rpf 里找不到 gameconfig.xml (共 %u 个条目)", g_gc_rpf.entry_count);
        g_gc_msg_color = C_RED;
        gc_rpf_close(&g_gc_rpf); g_gc_err = rc; return rc;
    }

    /* 读内容 */
    u8 *buf = NULL; size_t blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_gc_ent, &buf, &blen);
    if (rc != GC_OK) {
        gc_msg("解压 gameconfig.xml 失败 (rc=%d, on_disk=%u size=%u)",
               rc, g_gc_ent.on_disk, g_gc_ent.size);
        g_gc_msg_color = C_RED;
        gc_rpf_close(&g_gc_rpf); g_gc_err = rc; return rc;
    }

    /* 备份原始压缩块（回滚用） */
    g_gc_orig_on_disk = g_gc_ent.on_disk ? g_gc_ent.on_disk : (int)g_gc_ent.size;
    if (g_gc_orig_blob) { free(g_gc_orig_blob); g_gc_orig_blob = NULL; }
    g_gc_orig_blob = (u8*)malloc(g_gc_orig_on_disk);
    if (g_gc_orig_blob) {
        if (fseek(g_gc_rpf.fp, (long)g_gc_ent.offset * 512, SEEK_SET) == 0) {
            g_gc_orig_len = fread(g_gc_orig_blob, 1, g_gc_orig_on_disk, g_gc_rpf.fp);
        }
    }

    /* 编辑缓冲：留足余量（原 103 KB，给 2 倍 + 64 KB） */
    g_gc_xml_cap = blen * 2 + 65536;
    g_gc_xml = (u8*)malloc(g_gc_xml_cap);
    if (!g_gc_xml) { free(buf); gc_rpf_close(&g_gc_rpf); g_gc_err = GC_ERR_NOMEM; return GC_ERR_NOMEM; }
    memcpy(g_gc_xml, buf, blen);
    g_gc_xml[blen] = 0;
    g_gc_xml_len = blen;
    free(buf);

    /* 定位 switch / Any 段 */
    if (gc_xml_find_span((char*)g_gc_xml, g_gc_xml_len, "switch",
                         &g_gc_sw_a, &g_gc_sw_b) != GC_OK) {
        /* 显示开头内容，便于判断读到的是什么 */
        char head[64] = {0};
        for (int i = 0; i < 60 && i < (int)g_gc_xml_len; i++) {
            unsigned char c = g_gc_xml[i];
            head[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        gc_msg("gameconfig.xml 里找不到 switch 段 (%d B) 开头: %s",
               (int)g_gc_xml_len, head);
        g_gc_msg_color = C_RED;
        gc_rpf_close(&g_gc_rpf); free(g_gc_xml); g_gc_xml = NULL;
        g_gc_err = GC_ERR_FORMAT; return GC_ERR_FORMAT;
    }
    if (gc_xml_find_span((char*)g_gc_xml, g_gc_xml_len, "Any",
                         &g_gc_any_a, &g_gc_any_b) != GC_OK) {
        g_gc_any_a = g_gc_any_b = 0;
    }
    g_gc_n_switch = gc_xml_count((char*)g_gc_xml, g_gc_xml_len, g_gc_sw_a, g_gc_sw_b);
    g_gc_n_any = (g_gc_any_b > g_gc_any_a)
               ? gc_xml_count((char*)g_gc_xml, g_gc_xml_len, g_gc_any_a, g_gc_any_b) : 0;

    g_gc_opened = 1;
    g_gc_err = 0;
    gc_msg("已载入 gameconfig.xml (%d B, switch 段 %d 项)", (int)g_gc_xml_len, g_gc_n_switch);
    g_gc_msg_color = C_GREEN;
    return GC_OK;
}

static void gc_reload_from_disk(void) {
    /* 丢弃内存状态，重新读盘（恢复原版时用） */
    if (g_gc_xml) { free(g_gc_xml); g_gc_xml = NULL; }
    if (g_gc_opened) { gc_rpf_close(&g_gc_rpf); g_gc_opened = 0; }
    gc_clear_edited();
    g_gc_dirty = 0;
    gc_open_rpf();
}

/* 应用一套预设（只改 switch 段！） */
static int gc_apply_preset(int idx) {
    if (!g_gc_opened) { gc_msg("请先载入 update.rpf"); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
    if (idx < 0 || idx >= GC_NUM_PRESETS) return GC_ERR_FORMAT;

    const GcPreset *p = &GC_PRESETS[idx];
    int n_applied = 0, n_inserted = 0, n_fail = 0;

    /* 先重新从磁盘读一份干净的（保证多次应用不叠加） */
    gc_reload_from_disk();
    if (!g_gc_opened) return GC_ERR_OPEN;

    for (int i = 0; i < p->n_knobs; i++) {
        int ins = 0;
        /* ★ 用 _ex 版：插入后自动重定位段区间（否则第二次插入必失败） */
        int rc = gc_xml_set_int_ex((char*)g_gc_xml, g_gc_xml_cap, &g_gc_xml_len,
                                   &g_gc_sw_a, &g_gc_sw_b, "switch",
                                   p->knobs[i].key, p->knobs[i].value, &ins);
        if (rc == GC_OK) {
            if (ins) n_inserted++; else n_applied++;
            gc_mark_edited(p->knobs[i].key, p->knobs[i].value);
        } else {
            n_fail++;
        }
    }
    g_gc_n_switch = gc_xml_count((char*)g_gc_xml, g_gc_xml_len, g_gc_sw_a, g_gc_sw_b);

    g_gc_preset = idx;
    g_gc_dirty = 1;
    gc_msg("已套用【%s】: 改 %d 项 / 新增 %d 项%s",
           p->name, n_applied, n_inserted, n_fail ? " (部分失败)" : "");
    g_gc_msg_color = n_fail ? C_RED : C_GREEN;
    return GC_OK;
}

/* 手动调整：对某个旋钮加/减 */
static void gc_knob_step(int dir) {
    if (!g_gc_opened) return;
    if (g_gc_knob < 0 || g_gc_knob >= GC_NUM_KNOB_META) return;
    const GcKnobMeta *m = &GC_KNOB_META[g_gc_knob];

    /* 当前值：优先取已改的，否则从 XML 读 */
    int cur = gc_get_edited(m->key);
    if (cur == INT32_MIN) {
        int v = 0;
        if (gc_xml_get_int((char*)g_gc_xml, g_gc_xml_len, g_gc_sw_a, g_gc_sw_b, m->key, &v) == GC_OK)
            cur = v;
        else if (gc_xml_get_int((char*)g_gc_xml, g_gc_xml_len, g_gc_any_a, g_gc_any_b, m->key, &v) == GC_OK)
            cur = v;   /* 沿用 Any 的值 */
        else
            cur = 0;
    }

    int nv = cur + dir;
    if (nv < m->lo) nv = m->lo;
    if (nv > m->hi) nv = m->hi;
    if (nv == cur) return;

    int ins = 0;
    int rc = gc_xml_set_int_ex((char*)g_gc_xml, g_gc_xml_cap, &g_gc_xml_len,
                               &g_gc_sw_a, &g_gc_sw_b, "switch", m->key, nv, &ins);
    if (rc != GC_OK) {
        if (rc == GC_ERR_TOOBIG) { gc_msg("缓冲区不足"); g_gc_msg_color = C_RED; }
        return;
    }
    gc_mark_edited(m->key, nv);
    g_gc_dirty = 1;
    g_gc_preset = -1;   /* 手动改过 -> 不再是纯预设 */
    gc_msg("%s = %d", m->key, nv);
    g_gc_msg_color = C_BLUE;
}

/* 试压缩：返回压缩后大小（失败返回 0） */
static size_t gc_try_deflate(const u8 *data, size_t len, u8 *out, size_t out_cap) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    zs.next_in = (Bytef*)data;  zs.avail_in  = (uInt)len;
    zs.next_out = out;          zs.avail_out = (uInt)out_cap;
    int rc = deflate(&zs, Z_FINISH);
    uLong got = zs.total_out;
    deflateEnd(&zs);
    return (rc == Z_STREAM_END) ? (size_t)got : 0;
}

/* 写入 update.rpf（含自动瘦身 + 校验 + 失败回滚） */
static int gc_write_to_rpf(void) {
    if (!g_gc_opened) { gc_msg("未打开 update.rpf"); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
    if (!gc_rpf_writable(&g_gc_rpf)) {
        gc_msg("update.rpf 是【只读】打开的 - 检查 SD 卡写保护或文件只读属性");
        g_gc_msg_color = C_RED;
        return GC_ERR_READONLY;
    }

    GcEntry cur;
    gc_rpf_entry(&g_gc_rpf, g_gc_ent.idx, &cur);

    /* 🚨🚨 容量 = 【物理可用空间】，绝不用 g_gc_orig_on_disk！
     *
     *   踩过：原来写 `cap = g_gc_orig_on_disk`（= 打开时磁盘上的 on_disk），
     *   而 on_disk 是【上次写入的结果】⇒ 写一次之后容量就变小，
     *   反复写几次就锁死，换个大点的预设就报「超出容量 6492」。
     *
     *   正确：gameconfig 后面有 93.5 MB 物理空隙，on_disk 24 位上限 16 MB，
     *        压缩后只有 ~7.5 KB ⇒ 余量约 2000 倍，永远塞得下。 */
    int room_i = gc_rpf_room(&g_gc_rpf, &cur);
    u32 cap = (room_i > 0) ? (u32)room_i : (u32)(cur.on_disk ? cur.on_disk : cur.size);

    /* ---- 逐步加重瘦身，直到塞得下 ----
     * (1) 原样  (2) 删注释  (3) 删注释+压空白
     * 实测: 原版 7602 B / 删注释 7268 B / 再压空白 6311 B
     * 注意 保留 16 B 安全余量：临界值容易因浮点/版本差异翻车 */
    u32 cap_safe = (cap > 16) ? (cap - 16) : cap;
    u8 *body = g_gc_xml;
    size_t blen = g_gc_xml_len;
    u8 *work = NULL;
    const char *how = "原样";
    size_t zsz = 0;

    size_t bound = compressBound((uLong)blen) + 1024;
    u8 *probe = (u8*)malloc(bound);
    if (!probe) { gc_msg("内存不足"); g_gc_msg_color = C_RED; return GC_ERR_NOMEM; }

    zsz = gc_try_deflate(body, blen, probe, bound);
    if (zsz == 0 || zsz > cap_safe) {
        /* (2) 真删注释 */
        work = (u8*)malloc(blen + 1);
        if (work) {
            memcpy(work, g_gc_xml, blen);
            size_t nl = gc_drop_comments((char*)work, blen);
            size_t z2 = gc_try_deflate(work, nl, probe, bound);
            if (z2 > 0 && z2 <= cap_safe) {
                body = work; blen = nl; zsz = z2; how = "删注释";
            } else {
                /* (3) 再压空白 */
                size_t nl3 = gc_squeeze((char*)work, nl);
                size_t z3 = gc_try_deflate(work, nl3, probe, bound);
                if (z3 > 0 && z3 <= cap_safe) {
                    body = work; blen = nl3; zsz = z3; how = "删注释+压空白";
                } else if (z2 > 0 && z2 <= cap) {
                    body = work; blen = nl; zsz = z2; how = "删注释(临界)";
                } else {
                    body = work; blen = nl; zsz = z2; how = "删注释(超限)";
                }
            }
        }
    }
    free(probe);

    if (zsz == 0 || zsz > cap) {
        if (work) free(work);
        gc_msg("压缩后 %zu B 超出物理可用空间 %u B", zsz, cap);
        g_gc_msg_color = C_RED;
        return GC_ERR_TOOBIG;
    }

    int rc = gc_rpf_write(&g_gc_rpf, &g_gc_ent, body, blen);
    if (rc != GC_OK) {
        if (work) free(work);
        gc_msg(rc == GC_ERR_TOOBIG ? "写入时容量不足"
             : rc == GC_ERR_READONLY ? "文件只读打开，无法写入"
             : "写入失败 (rc=%d) %s", rc, gc_rpf_last_error());
        g_gc_msg_color = C_RED;
        return rc;
    }

    /* 校验 */
    rc = gc_rpf_verify(&g_gc_rpf, &g_gc_ent, body, blen);
    if (rc != GC_OK) {
        /* 回滚 */
        if (g_gc_orig_blob && g_gc_orig_len > 0) {
            fseek(g_gc_rpf.fp, (long)g_gc_ent.offset * 512, SEEK_SET);
            fwrite(g_gc_orig_blob, 1, g_gc_orig_len, g_gc_rpf.fp);
            long eo = 0x10 + (long)g_gc_ent.idx * 16;
            u8 t[16];
            fseek(g_gc_rpf.fp, eo, SEEK_SET);
            if (fread(t, 1, 16, g_gc_rpf.fp) == 16) {
                u64 q0 = (u64)t[0] | ((u64)t[1]<<8) | ((u64)t[2]<<16) | ((u64)t[3]<<24)
                       | ((u64)t[4]<<32) | ((u64)t[5]<<40) | ((u64)t[6]<<48) | ((u64)t[7]<<56);
                q0 = (q0 & ~(((u64)0xFFFFFF) << 16)) | ((u64)g_gc_orig_on_disk << 16);
                for (int i = 0; i < 8; i++) t[i] = (u8)((q0 >> (8*i)) & 0xFF);
                t[8]=(u8)(cur.size&0xFF); t[9]=(u8)((cur.size>>8)&0xFF);
                t[10]=(u8)((cur.size>>16)&0xFF); t[11]=(u8)((cur.size>>24)&0xFF);
                fseek(g_gc_rpf.fp, eo, SEEK_SET);
                fwrite(t, 1, 16, g_gc_rpf.fp);
            }
            fflush(g_gc_rpf.fp);
            fsdevCommitDevice("sdmc");
            gc_msg("校验失败, 已自动回滚原内容");
        } else {
            gc_msg("校验失败且无备份!");
        }
        g_gc_msg_color = C_RED;
        if (work) free(work);
        return rc;
    }

    if (work) free(work);
    g_gc_dirty = 0;
    gc_msg("已写入 update.rpf (%zu B -> 压缩 %zu B, 手段=%s) 重启游戏生效",
           blen, zsz, how);
    g_gc_msg_color = C_GREEN;
    return GC_OK;
}

/* ★★★ 「恢复原版」= 套用原版预设（用原版值覆盖全部参数）
 *
 * 为什么不用「回滚内存备份」？
 *   (1) 内存备份只在【本次打开 rpf 时】有效 -- 若用户上次已写入过省电，
 *      这次打开时备份的就已经是省电版，回滚等于什么都没做。
 *   (2) 用户真正想要的是「把参数恢复成移植作者的原始值」，
 *      而不是「撤销我这次会话的改动」。
 *   (3) 原版值已经固化在 GC_PRESET_ORIGINAL_KNOBS（从原版 XML 提取），
 *      无论当前是什么状态都能正确还原。
 *
 * 实现：等价于「套用 GC_IDX_ORIGINAL 这套预设」，然后照常写入。
 */
static int gc_restore_original(void) {
    if (!g_gc_opened) { gc_msg("请先载入 update.rpf"); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }

    /* 套用原版预设（会把全部 52 项恢复成移植作者的原始值） */
    int rc = gc_apply_preset(GC_IDX_ORIGINAL);
    if (rc != GC_OK) {
        gc_msg("恢复原版失败 (rc=%d)", rc);
        g_gc_msg_color = C_RED;
        return rc;
    }
    gc_msg("已载入原版参数 (%d 项)，请点「写入rpf」落盘",
           GC_PRESETS[GC_IDX_ORIGINAL].n_knobs);
    g_gc_msg_color = C_ACCENT;
    return GC_OK;
}

/* ★★★ 回读校验：把 rpf 从磁盘【重新打开】读一遍，回答「到底生效没有」
 *
 * 设计要点：
 *   · 用独立的 GcRpf 打开同一路径，绝不复用 g_gc_rpf.fp
 *     （复用会打乱它的位置指针，而且可能命中缓存而非真正的磁盘数据）
 *   · 先做【字节级比对】：磁盘内容 == 内存里准备写的内容 ⇒ 一定生效了
 *   · 再做【逐项比对】：把差异项列出来，便于判断是「没写」还是「写错了」
 *   · 详细报告写进 g_gc_selftest 覆盖层（2048 B），摘要走 gc_msg
 */
static int gc_verify_on_disk(void) {
    if (!g_gc_opened) { gc_msg("请先载入 update.rpf"); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
    if (!g_gc_xml)    { gc_msg("内存里没有 gameconfig 内容"); g_gc_msg_color = C_RED; return GC_ERR_FORMAT; }

    GcRpf chk;
    int rc = gc_rpf_open(g_gc_rpf.path, &chk);
    if (rc != GC_OK) {
        gc_msg("回读失败: 打不开 %s (%s)", g_gc_rpf.path, gc_rpf_last_error());
        g_gc_msg_color = C_RED;
        return rc;
    }
    GcEntry e2;
    rc = gc_rpf_find(&chk, "gameconfig.xml", &e2);
    if (rc != GC_OK) {
        gc_msg("回读失败: 磁盘上找不到 gameconfig.xml (rc=%d)", rc);
        gc_rpf_close(&chk); g_gc_msg_color = C_RED; return rc;
    }
    u8 *buf = NULL; size_t bl = 0;
    rc = gc_rpf_read(&chk, &e2, &buf, &bl);
    if (rc != GC_OK) {
        gc_msg("回读失败: 解压失败 rc=%d (%s)", rc, gc_rpf_last_error());
        gc_rpf_close(&chk); g_gc_msg_color = C_RED; return rc;
    }

    /* 磁盘上的 switch 段 */
    size_t da = 0, db = 0;
    int has_sw = (gc_xml_find_span((char*)buf, bl, "switch", &da, &db) == GC_OK);

    /* ---------- 组织报告 ---------- */
    char *o = g_gc_selftest;
    size_t cap = sizeof(g_gc_selftest), w = 0;
#define VAPP(...) do {                                                      \
        if (w < cap) {                                                      \
            int _n = snprintf(o + w, cap - w, __VA_ARGS__);                 \
            if (_n > 0) {                                                   \
                if ((size_t)_n >= cap - w) { w = cap - 1; }                 \
                else                       { w += (size_t)_n; }             \
                o[w] = 0;                                                   \
            }                                                               \
        }                                                                   \
    } while (0)

    VAPP("★ 回读校验 (重新从磁盘读取)\n");
    VAPP("文件: %s\n", g_gc_rpf.path);
    VAPP("条目: idx=%d on_disk=%u size=%u offset=0x%X\n",
         e2.idx, e2.on_disk, e2.size, e2.offset);
    {
        int room = gc_rpf_room(&chk, &e2);
        VAPP("物理可用空间: %d B   (本次打开时 on_disk=%d)\n",
             room, g_gc_orig_on_disk);
    }
    VAPP("注: on_disk 现在只增不减, 不能当写入判据;\n");
    VAPP("    请以下面的【字节级】与【逐项】比对为准。\n\n");

    int byte_same = (bl == g_gc_xml_len) && (memcmp(buf, g_gc_xml, bl) == 0);

    if (!has_sw) {
        VAPP("★ 磁盘内容里找不到 switch 段 (size=%zu B)\n", bl);
        VAPP("前 60 字节: ");
        for (int i = 0; i < 60 && i < (int)bl; i++) {
            unsigned char c = buf[i];
            VAPP("%c", (c >= 32 && c < 127) ? c : '.');
        }
        VAPP("\n");
    }

    /* 逐项比对：以内存里准备写的值为准 */
    int n_same = 0, n_diff = 0, n_missing = 0;
    int shown = 0;

    /* 优先只比对「本次相关的项」：选了预设就比该预设的项，否则比全部元数据项 */
    const GcKnob *plist = NULL; int pn = 0;
    if (g_gc_preset >= 0 && g_gc_preset < GC_NUM_PRESETS) {
        plist = GC_PRESETS[g_gc_preset].knobs;
        pn    = GC_PRESETS[g_gc_preset].n_knobs;
    }

    {
        /* 拼成一行再输出：多字节分隔线分开 VAPP 会触发 -Wformat-truncation 噪声 */
        char hl[96];
        if (plist) snprintf(hl, sizeof(hl), "-- 逐项比对 (预设「%s」%d 项) --",
                            GC_PRESETS[g_gc_preset].name, pn);
        else       snprintf(hl, sizeof(hl), "-- 逐项比对 (全部 %d 项) --", GC_NUM_KNOB_META);
        VAPP("%s\n", hl);
    }

    for (int i = 0; i < (plist ? pn : GC_NUM_KNOB_META); i++) {
        const char *key = plist ? plist[i].key : GC_KNOB_META[i].key;

        /* 内存里准备写的值 */
        int want = 0;
        if (gc_xml_get_int((char*)g_gc_xml, g_gc_xml_len, g_gc_sw_a, g_gc_sw_b, key, &want) != GC_OK)
            continue;   /* 内存里没有这项，跳过 */

        /* 磁盘上的值 */
        int got = 0;
        int found = 0;
        if (has_sw && gc_xml_get_int((char*)buf, bl, da, db, key, &got) == GC_OK) found = 1;

        if (!found) {
            n_missing++;
            if (shown < 12) { VAPP("  X %-26s 磁盘无此项 (期望 %d)\n", key, want); shown++; }
        } else if (got == want) {
            n_same++;
        } else {
            n_diff++;
            if (shown < 12) { VAPP("  X %-26s 期望 %d, 磁盘 %d\n", key, want, got); shown++; }
        }
    }

    VAPP("\n%s\n", "-- 结果 --");
    VAPP("一致 %d 项 / 值不同 %d 项 / 磁盘缺失 %d 项\n", n_same, n_diff, n_missing);
    VAPP("字节级: 磁盘内容与内存编辑内容 %s\n",
         byte_same ? "【完全一致】" : "【不一致】");

    if (byte_same) {
        VAPP("\n%s\n", "[OK] 已生效：磁盘上就是当前编辑的内容");
        VAPP("   (下次启动游戏即读取此配置)\n");
    } else if (n_diff == 0 && n_missing == 0) {
        VAPP("\n%s\n", "注意 值都对，但字节不一致");
        VAPP("   多半是磁盘上还有别处的差异（注释/空白），值本身已生效\n");
    } else {
        VAPP("\n%s\n", "[X] 未生效：磁盘上仍是旧值");
        VAPP("   -> 请点「写入rpf」并确认\n");
    }
#undef VAPP

    free(buf);
    gc_rpf_close(&chk);

    g_gc_selftest_show = 1;
    if (byte_same) {
        gc_msg("回读校验: 已生效 (磁盘内容 == 编辑内容)");
        g_gc_msg_color = C_GREEN;
        return GC_OK;
    } else if (n_diff == 0 && n_missing == 0) {
        gc_msg("回读校验: %d 项值全部一致, 已生效", n_same);
        g_gc_msg_color = C_GREEN;
        return GC_OK;
    } else {
        gc_msg("回读校验: 未生效 - %d 项不符, %d 项缺失", n_diff, n_missing);
        g_gc_msg_color = C_RED;
        return GC_ERR_IO;
    }
}

int main(int argc, char **argv) {
    romfsInit();
    detect_language();   /* ★ v6.3: 按系统语言切中/英 */

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    /* ★ v4: 初始化触屏 */
    hidInitializeTouchScreen();

    Framebuffer fb;
    framebufferCreate(&fb, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);
    g_dlc_prog_fb = &fb;      /* ★ v22: 进度覆盖层需要自己开子帧渲染 */

    init_shared_font();
    load_initial_save();

    int running = 1;

    while (running && appletMainLoop()) {
        padUpdate(&g_pad);
        u32 kDown = padGetButtonsDown(&g_pad);
        u32 kHeld = padGetButtons(&g_pad);   /* ★ v30: 长按连发用（ZL/ZR 快速调整） */

        /* 摇杆导航注入: 只影响选项框位置, 不调数值; 按住连发
         * - 弹窗 (modal): 全部方向导航 (移动选项框), 摇杆 ←/->↑/↓ 全并入 kDown
         * - 主界面/画质页: 摇杆 ←/-> 并入 kDown 切换选项;
         *                   摇杆 ↑/↓ 走独立 nav_down (列表行切换=移动选项框, 不并入 kDown 以免触发调数值)
         * 连发: 首次按下触发, 持续按住 20 帧后每 6 帧重复触发
         * ★ v31: 左、右摇杆【完全同权】-- 两者输出按位或合并后走同一套导航逻辑，
         *   所以推哪根摇杆都一样（用户要求「右摇杆和左摇杆功能一样就行」）。 */
        static u32 s_joy_held = 0;
        static int s_joy_tick = 0;
        u32 joy_now = joy_to_dpad(0) | joy_to_dpad(1);   /* ★ v31: 左右摇杆合并 */
        /* 弹窗: 全方向导航(移动选项框); 主界面/画质页: 仅 ←/-> 并入 kDown (↑/↓ 走 nav_down) */
        u32 nav_mask = (g_modal_mode != 0) ? 0xFFFFFFFF : (HidNpadButton_Left | HidNpadButton_Right);
        u32 joy_eff = joy_now & nav_mask;
        u32 joy_down = 0;
        if (joy_eff & ~s_joy_held) {
            joy_down = joy_eff & ~s_joy_held; /* 新按下的方向 */
            s_joy_tick = 0;
        } else if (joy_eff & s_joy_held) {
            s_joy_tick++;
            if (s_joy_tick >= 20 && (s_joy_tick % 6) == 0) joy_down = joy_eff; /* 长按连发 */
        }
        s_joy_held = joy_eff;
        kDown |= joy_down;

        /* 摇杆 ↑/↓ 独立注入: 1=上(上一行) 2=下(下一行) 0=无; 仅非弹窗页消费
         * (弹窗已由 kDown 全方向处理, 这里强制清零避免双重触发) */
        static u32 s_joy_nav_held = 0;
        static int  s_joy_nav_tick = 0;
        u32 joy_nav_dir = 0;
        if (joy_now & HidNpadButton_Up)   joy_nav_dir = 1;
        else if (joy_now & HidNpadButton_Down) joy_nav_dir = 2;
        if (g_modal_mode != 0) joy_nav_dir = 0; /* 弹窗已全方向导航 */
        u32 nav_down = 0; /* 本帧有效的摇杆行切换: 1=上一行 2=下一行 */
        if (joy_nav_dir && joy_nav_dir != s_joy_nav_held) {
            nav_down = joy_nav_dir; /* 新按下 */
            s_joy_nav_tick = 0;
        } else if (joy_nav_dir && joy_nav_dir == s_joy_nav_held) {
            s_joy_nav_tick++;
            if (s_joy_nav_tick >= 20 && (s_joy_nav_tick % 6) == 0) nav_down = joy_nav_dir; /* 长按连发 */
        }
        s_joy_nav_held = joy_nav_dir;

        /* ★ v31: 右摇杆与左摇杆已合并到上面同一套逻辑（joy_now 按位或），
         *   此处不再有独立的右摇杆处理分支。 */


        /* =================================================================
         * ★ v4: 触屏处理
         *   命中测试用的是【上一帧】渲染时登记的 g_hot[] (1 帧延迟, 无感)。
         *   命中后把动作转成「等价按键」并入 kDown, 或单独处理新动作
         *   (页签切换 / 底部大号 −/+)。
         * ================================================================= */
        touch_poll();
        int t_act = ACT_NONE, t_param = 0;
        if (g_touch_tapped && g_touch_x >= 0) {
            int hi = hot_hit(g_touch_x, g_touch_y);
            if (hi >= 0) {
                t_act = g_hot[hi].action; t_param = g_hot[hi].param;
                /* ★ v24: 记下被点热区的矩形 + 时刻, 本帧渲染时叠醒目高亮框
                 *   （用户反馈「选中按钮没有高亮显示」--触屏点选必须看得见反馈） */
                g_tap_fx[0] = g_hot[hi].x; g_tap_fx[1] = g_hot[hi].y;
                g_tap_fx[2] = g_hot[hi].w; g_tap_fx[3] = g_hot[hi].h;
                g_tap_fx_on   = 1;
                g_tap_fx_tick = svcGetSystemTick();
            }
        }

        /* ★ 自检报告显示时：点屏幕任意位置即关闭（不走热区，避免被别处拦截） */
        if (g_gc_selftest_show && g_touch_tapped && g_touch_x >= 0) {
            g_gc_selftest_show = 0;
        }

        /* 触屏动作 -> 等价按键 (复用原有全部逻辑, 不写第二套) */
        switch (t_act) {
            case ACT_NAV_LEFT:   kDown |= HidNpadButton_Left;  break;
            case ACT_NAV_RIGHT:  kDown |= HidNpadButton_Right; break;
            case ACT_NAV_UP:
                /* ★ v25: 存档页已列表化, 与画质页统一走十字键 Up/Down */
                if (g_tab == TAB_GFX || g_tab == TAB_SAVE) kDown |= HidNpadButton_Up;
                break;
            case ACT_NAV_DOWN:
                if (g_tab == TAB_GFX || g_tab == TAB_SAVE) kDown |= HidNpadButton_Down;
                break;
            case ACT_CONFIRM:    kDown |= HidNpadButton_A;     break;
            case ACT_BACK:       kDown |= HidNpadButton_B;     break;
            case ACT_CYCLE_STEP: kDown |= HidNpadButton_Y;     break;
            case ACT_MAX_ALL:    kDown |= HidNpadButton_X;     break;
            case ACT_MAX_CHAR:   kDown |= HidNpadButton_Minus; break;
            case ACT_QUIT:       kDown |= HidNpadButton_Plus;  break;
            case ACT_PICK_ITEM:
                /* 直接点选: 卡片 / 画质项 / 角色 / 弹窗条目
                 *   0..8        = 属性索引
                 *   100000+i    = 画质项索引 i
                 *   -1-c        = 角色 c (c=0,1,2) */
                if (g_modal_mode == 0) {
                    if (t_param >= 100000) {
                        int gi = t_param - 100000;
                        if (g_tab == TAB_GFX && gi >= 0 && gi < g_gfx_count)
                            g_gfx_cur_idx = (char)gi;
                    } else if (t_param >= 0 && t_param < NUM_ATTRS) {
                        if (g_tab == TAB_SAVE) g_cur_attr = t_param;
                    } else if (t_param <= -1 && t_param >= -3) {
                        g_cur_char = -1 - t_param;
                    }
                } else if (t_param >= 0) {
                    g_modal_sel = t_param;   /* 弹窗里点选某项 */
                }
                break;
            case ACT_CHAR_PREV:
                if (g_modal_mode == 0) g_cur_char = (g_cur_char + 2) % 3;
                break;
            case ACT_CHAR_NEXT:
                if (g_modal_mode == 0) g_cur_char = (g_cur_char + 1) % 3;
                break;
            case ACT_SAVE:
                if (g_modal_mode == 0) {
                    if (g_tab == TAB_GFX) { if (g_gfx_dirty) gfx_save(); }
                    else save_to_disk();
                }
                break;
            case ACT_LOAD_PRESET:
                if (g_modal_mode == 0) { g_modal_mode = 1; g_modal_sel = 0; }
                break;
            case ACT_LOAD_BACKUP:
                if (g_modal_mode == 0) { scan_backup_history(); g_modal_mode = 2; g_modal_sel = 0; }
                break;
            case ACT_PICK_SLOT:
                if (g_modal_mode == 0) {
                    if (g_save_dir[0] != '\0') scan_save_slots(g_save_dir);
                    g_modal_mode = 3; g_modal_sel = g_active_slot_idx;
                }
                break;
            case ACT_GFX_PRESET:
                if (g_modal_mode == 0) { g_modal_mode = 4; g_modal_sel = 0; g_modal_scr = 0; }
                break;
            case ACT_GFX_IMPORT:
                if (g_modal_mode == 0) {
                    scan_switch_gfx_files();
                    if (g_gfx_switch_count > 0) { g_modal_mode = 5; g_modal_sel = 0; g_modal_scr = 0; }
                    else { gfx_import_from_switch(NULL); }
                }
                break;
            case ACT_GFX_CHECK:
                if (g_modal_mode == 0) { gfx_check_paths(); g_modal_mode = 6; g_modal_sel = 0; }
                break;
            case ACT_SCROLL_UP:
                if (g_tab == TAB_GFX) {
                    if (g_gfx_cur_idx > 0) g_gfx_cur_idx--;
                    if (g_gfx_cur_idx < g_gfx_scroll) g_gfx_scroll = g_gfx_cur_idx;
                }
                break;
            case ACT_SCROLL_DOWN:
                if (g_tab == TAB_GFX) {
                    int vis = (LY_BODY_H + 2) / 40;   /* v6.1: 放开 10 行上限，按实际空间 */
                    if (g_gfx_cur_idx + 1 < g_gfx_count) g_gfx_cur_idx++;
                    if (g_gfx_cur_idx >= g_gfx_scroll + vis) g_gfx_scroll = g_gfx_cur_idx - vis + 1;
                }
                break;
            case ACT_SCROLL_TOP:
                if (g_tab == TAB_GFX && g_gfx_count > 0) { g_gfx_cur_idx = 0; g_gfx_scroll = 0; }
                break;
            case ACT_SCROLL_BOTTOM:
                if (g_tab == TAB_GFX && g_gfx_count > 0) {
                    int vis = (LY_BODY_H + 2) / 40;   /* v6.1: 放开 10 行上限，按实际空间 */
                    g_gfx_cur_idx = (char)(g_gfx_count - 1);
                    g_gfx_scroll = (g_gfx_count > vis) ? g_gfx_count - vis : 0;
                }
                break;
            case ACT_TAB_SAVE:
                if (g_modal_mode == 0) g_tab = TAB_SAVE;
                break;
            case ACT_TAB_GFX:
                if (g_modal_mode == 0) {
                    if (g_gfx_count == 0) gfx_load();
                    g_tab = TAB_GFX;
                }
                break;
            case ACT_TAB_TOOL:
                if (g_modal_mode == 0) g_tab = TAB_TOOL;
                break;
            case ACT_TAB_PERF:
                if (g_modal_mode == 0) {
                    g_tab = TAB_PERF;
                    if (!g_gc_opened) gc_open_rpf();   /* 切到该页自动尝试载入 */
                }
                break;
            case ACT_TAB_DLC:
                if (g_modal_mode == 0) {
                    g_tab = TAB_DLC;
                    if (!g_dlc_xml_loaded) dlc_reload();   /* 切到该页自动扫描 */
                }
                break;
            /* ---- ★ v5.5 脚本 mod 独立页 ---- */
            case ACT_TAB_SCRIPT:
                if (g_modal_mode == 0) {
                    g_tab = TAB_SCRIPT;
                    script_scan();               /* 切到该页自动扫描（含自动建目录） */
                    g_script_sel = 0; g_script_scr = 0;
                }
                break;
            case ACT_SCRIPT_SCAN:
                if (g_modal_mode == 0) {
                    script_scan();
                    dlc_msg("已重新扫描：%d 个 .nsc 脚本", g_script_count);
                }
                break;
            case ACT_SCRIPT_INSTALL:
                /* ★ v5.5: 点列表项(param>=1000) -> 只选中；点按钮(param=0) -> 装选中项 */
                if (g_modal_mode == 0) {
                    if (t_param >= 1000 && t_param - 1000 < g_script_count) {
                        g_script_sel = t_param - 1000;    /* 点行 = 选中 */
                    } else {
                        script_install_selected();        /* 点按钮 = 安装 */
                    }
                }
                break;
            case ACT_SCRIPT_UNINSTALL:
                if (g_modal_mode == 0) script_uninstall_selected();
                break;
            case ACT_SCRIPT_DIR:
                /* ★ v5.7: 开关式 —— 已显示则关闭 */
                if (g_modal_mode == 0) {
                    if (g_script_dirinfo_on) {
                        g_script_dirinfo_on = 0;
                        dlc_msg("已关闭目录信息");
                    } else {
                        script_show_dir();
                    }
                }
                break;
            /* ★ v6.2: 脚本管理二级菜单（还原官方 + 内置模组） */
            case ACT_SCRIPT_MGR:
                if (g_modal_mode == 0) {
                    script_mgr_open();
                } else if (g_modal_mode == MODE_SCRIPT_MGR) {
                    /* 触屏点某一行：param 就是行号（0..2 还原官方，3..6 内置模组） */
                    int ns = gc_script_stock_count();
                    if (t_param >= 0 && t_param < script_mgr_total()) {
                        g_modal_sel = t_param;
                        if (t_param < ns) script_mgr_ask(0, t_param);
                        else              script_mgr_ask(1, t_param - ns);
                    }
                }
                break;
            case ACT_SCRIPT_CONFIRM_OK:
                if (g_modal_mode == MODE_SCRIPT_CONFIRM) {
                    g_modal_mode = MODE_SCRIPT_MGR;
                    script_mgr_apply();
                }
                break;
            /* ---- ★ v21 DLC 管理页 ---- */
            case ACT_DLC_LOAD:
                if (g_modal_mode == 0) {
                    dlc_reload();
                    dlc_scan_src();
                    g_dlc_view = 0;
                }
                break;
            case ACT_DLC_SCAN_SRC:
                if (g_modal_mode == 0) {
                    if (!g_dlc_xml_loaded) dlc_reload();
                    dlc_scan_dlcpacks();
                    dlc_refresh_registered();
                    dlc_scan_src();
                    g_dlc_view = 1;
                    g_dlc_src_sel = 0; g_dlc_src_scr = 0;
                }
                break;
            case ACT_DLC_VIEW:
                if (g_modal_mode == 0) {
                    /* ★ v5.5: 脚本 mod 已独立成 TAB_SCRIPT 页 ⇒ 这里回到 2 值循环 */
                    g_dlc_view = (g_dlc_view + 1) % 2;
                    if (g_dlc_view == 1 && g_dlc_src_count == 0) {
                        if (!g_dlc_xml_loaded) dlc_reload();
                        dlc_scan_src();
                    }
                }
                break;
            case ACT_DLC_CONVERT:
                if (g_modal_mode == 0) {
                    if (g_dlc_view == 0) {
                        dlc_convert_selected();
                    } else {
                        dlc_msg("「转格式」只对已装列表有效；导入时已自动转换");
                    }
                }
                break;
            case ACT_DLC_DELETE:
                /* ★ v25 删除只在已装列表有效（待导入的本来就没装，直接删源目录无意义） */
                if (g_modal_mode == 0 && g_dlc_view == 0) dlc_delete_selected();
                break;
            case ACT_DLC_HIDEHD:
                /* ★ v5.4: 屏蔽高清贴图（+hi.ytd 系），仅已装列表有效 */
                if (g_modal_mode == 0 && g_dlc_view == 0) dlc_hide_hd_selected();
                break;
            case ACT_OPEN_MODELDIR:
                /* ★ v5.4: 打开模型目录 —— Switch 没有文件管理器，
                 *   这里给出目录路径与内容统计（用户在电脑上操作） */
                if (g_modal_mode == 0) dlc_show_modeldir();
                break;
            case ACT_DLC_PICK:
                if (g_modal_mode == 0) {
                    if (t_param >= 1000) {
                        int i = t_param - 1000;
                        if (i >= 0 && i < g_dlc_src_count) { g_dlc_view = 1; g_dlc_src_sel = i; }
                    } else if (t_param >= 0 && t_param < g_dlc_count) {
                        g_dlc_view = 0; g_dlc_sel = t_param;
                    }
                }
                break;
            case ACT_DLC_TOGGLE:
                if (g_modal_mode == 0 && g_dlc_view == 0) dlc_toggle_selected();
                break;
            case ACT_DLC_IMPORT:
                if (g_modal_mode == 0) {
                    if (g_dlc_view == 1) dlc_import_selected();
                    else dlc_msg("先切到「待导入列表」再导入（按 Y）");
                }
                break;
            case ACT_DLC_VERIFY:
                if (g_modal_mode == 0) {
                    int idx = (g_dlc_view == 0) ? g_dlc_sel : g_dlc_src_sel;
                    const char *nm = (g_dlc_view == 0)
                                   ? (idx < g_dlc_count ? g_dlc_items[idx].name : NULL)
                                   : (idx < g_dlc_src_count ? g_dlc_src[idx].name : NULL);
                    if (!nm) { dlc_err("没有选中项"); break; }
                    char out[256] = {0};
                    int rc = dlc_check_integrity(nm, out, sizeof(out));
                    if (rc < 0) { dlc_err("%s: %s", nm, out); }
                    else        { dlc_msg("%s: %s", nm, out); g_dlc_msg_ok = 1; }
                }
                break;
            /* ---- ★ v6 性能调参页 ---- */
            case ACT_GC_LOAD:
                if (g_modal_mode == 0) gc_open_rpf();   /* 已打开则重新载入（按钮叫「重新载入」） */
                break;
            case ACT_GC_PICK_KNOB:
                if (g_modal_mode == 0 && g_gc_opened) g_gc_knob = t_param;
                break;
            case ACT_GC_GROUP_PREV:
                if (g_modal_mode == 0 && g_gc_opened) gc_group_jump(-1);
                break;
            case ACT_GC_GROUP_NEXT:
                if (g_modal_mode == 0 && g_gc_opened) gc_group_jump(+1);
                break;
            case ACT_GC_WRITE:
                if (g_modal_mode == 0 && g_gc_opened) {
                    if (g_gc_dirty) g_gc_confirm = 1;
                    else gc_msg("还没有改动");
                }
                break;
            case ACT_GC_RESTORE:
                if (g_modal_mode == 0 && g_gc_opened) g_gc_confirm = 2;
                break;
            case ACT_GC_SELFTEST:
                if (g_modal_mode == 0) {
                    int n = gc_rpf_selftest(g_gc_selftest, sizeof(g_gc_selftest));
                    g_gc_selftest_show = 1;
                    gc_msg("自检完成: %d/%d 个候选路径可访问", n, GC_NUM_CANDIDATES);
                    g_gc_msg_color = n > 0 ? C_GREEN : C_RED;
                }
                break;
            case ACT_GC_VERIFY:
                if (g_modal_mode == 0) gc_verify_on_disk();
                break;
            default: break;
        }

        /* ★ 底部大号 −/+ : 独立处理 (带按住连发), 不并入 kDown */
        int big_act = ACT_NONE;
        if (t_act == ACT_BIG_MINUS || t_act == ACT_BIG_PLUS) big_act = t_act;
        if (g_modal_mode == 0 && (g_tab == TAB_SAVE || g_tab == TAB_GFX)) {
            int dir = (big_act == ACT_BIG_PLUS) ? 1 : (big_act == ACT_BIG_MINUS ? -1 : 0);
            /* 键盘也能用: ZL=减 ZR=加 (但弹窗里 ZL/ZR 另有用途, 故仅非弹窗)
             * ★ v30: 用 kHeld -- 按住不放走下方连发逻辑（首帧立即，20 帧后每 5 帧一次） */
            if (kHeld & HidNpadButton_ZL) dir = -1;
            if (kHeld & HidNpadButton_ZR) dir =  1;

            if (dir != 0) {
                if (dir == g_big_hold_dir) {
                    g_big_hold_tick++;
                    /* 首次立即生效, 之后 20 帧后每 5 帧重复 */
                    if (g_big_hold_tick < 20 || (g_big_hold_tick % 5) != 0) dir = 0;
                } else {
                    g_big_hold_dir = dir;
                    g_big_hold_tick = 0;
                }
            } else {
                g_big_hold_dir = 0;
                g_big_hold_tick = 0;
            }

            if (dir != 0) {
                if (g_tab == TAB_SAVE) {
                    uint32_t stp = (g_cur_attr == 0) ? CASH_STEPS[g_cash_step_idx]
                                                     : SKILL_STEPS[g_skill_step_idx];
                    uint32_t mx  = (g_cur_attr == 0) ? GTA5_MONEY_LIMIT : 100;
                    uint32_t *v  = &g_values[g_cur_char][g_cur_attr];
                    if (dir > 0) {
                        *v = (*v + stp > mx) ? mx : (*v + stp);
                    } else {
                        *v = (*v < stp) ? 0 : (*v - stp);
                    }
                    g_dirty = 1;
                    if (g_cur_attr >= 1 && g_cur_attr <= 7)
                        g_skill_modified[g_cur_char][g_cur_attr] = 1;
                } else if (g_tab == TAB_GFX) {
                    gfx_step(dir > 0 ? 1 : -1);
                }
            }
        } else {
            g_big_hold_dir = 0;
            g_big_hold_tick = 0;
        }

        if (kDown & HidNpadButton_Plus) {
            if (g_dirty) save_to_disk();
            if (g_gfx_dirty) gfx_save();
            break;
        }

        /* =====================================================================
         * ★★ v24 全局 L / R = 切换页签
         *   用户原话：「存档修改里面 L R 按键多余了，触屏就行，LR 换成切换页面」
         *   ⇒ 从各页里收回 L/R 的旧语义（切角色 / 翻顶底 / 跳分组 / 跳首尾），
         *     统一改成「L = 上一页, R = 下一页」，按 TABS[] 显示顺序循环：
         *     DLC管理 -> 存档修改 -> 画质设置 -> 性能调参 -> 工具信息 -> DLC管理
         *   只在没有弹窗/确认框/自检报告时生效（那些场合 L/R 另有用途）。
         *   切页后清空本帧按键，避免同帧又在新页触发别的动作。
         * ===================================================================== */
        if (g_modal_mode == 0 && !g_gc_confirm && !g_gc_selftest_show && !g_dlc_confirm &&
            (kDown & (HidNpadButton_L | HidNpadButton_R))) {
            int cur = 0;
            for (int t = 0; t < NUM_TABS; t++) {
                if (TABS[t].id == g_tab) { cur = t; break; }
            }
            int step = (kDown & HidNpadButton_R) ? 1 : (NUM_TABS - 1);
            int nt   = (cur + step) % NUM_TABS;
            /* 与「点页签」完全一致的副作用（否则切过去是空白页） */
            if (TABS[nt].id == TAB_GFX  && g_gfx_count == 0) gfx_load();
            if (TABS[nt].id == TAB_PERF && !g_gc_opened)     gc_open_rpf();
            if (TABS[nt].id == TAB_DLC  && !g_dlc_xml_loaded) dlc_reload();
            /* ★ v5.5: 脚本页懒加载（scan 内部会自动建目录） */
            if (TABS[nt].id == TAB_SCRIPT && g_script_count == 0) script_scan();
            g_tab = TABS[nt].id;
            kDown = 0;   /* 本帧按键已消费完 */
        }

        /* ---------------- 模态弹窗处理 (1=内置存档,2=备份库,3=槽位,4=画质预设,5=画质文件选择) ---------------- */
        if (g_modal_mode != 0) {
            int total_items = 0;
            if (g_modal_mode == 1) total_items = NUM_PRESETS;
            else if (g_modal_mode == 2) total_items = g_num_backups;
            else if (g_modal_mode == 3) total_items = g_num_slots;
            else if (g_modal_mode == 4) total_items = NUM_GFX_PRESETS + 1; /* +1 = 导入选项 */
            else if (g_modal_mode == 5) total_items = g_gfx_switch_count;
            else if (g_modal_mode == 6) total_items = 0; /* 自检结果: 仅查看 */
            else if (g_modal_mode == MODE_SCRIPT_MGR) total_items = script_mgr_total();
            else if (g_modal_mode == MODE_SCRIPT_CONFIRM) total_items = 0;

            /* ★ v6.2: 列表条目数变化时夹紧选中项（脚本管理弹窗） */
            if (g_modal_mode == MODE_SCRIPT_MGR && total_items > 0) {
                if (g_modal_sel >= total_items) g_modal_sel = total_items - 1;
                if (g_modal_sel < 0) g_modal_sel = 0;
            }

            if (kDown & HidNpadButton_Up) {
                if (total_items > 0) {
                    g_modal_sel = (g_modal_sel + total_items - 1) % total_items;
                }
            }
            if (kDown & HidNpadButton_Down) {
                if (total_items > 0) {
                    g_modal_sel = (g_modal_sel + 1) % total_items;
                }
            }
            if (kDown & HidNpadButton_A) {
                if (g_modal_mode == 1) {
                    if (load_preset_save(g_modal_sel) == 0) g_modal_mode = 0; /* 成功自动关闭 */
                } else if (g_modal_mode == 2) {
                    if (load_backup_save(g_modal_sel) == 0) g_modal_mode = 0; /* 成功自动关闭 */
                } else if (g_modal_mode == 3) {
                    if (switch_to_slot(g_modal_sel) == 0) g_modal_mode = 0; /* 成功自动关闭 */
                } else if (g_modal_mode == 4) {
                    if (g_modal_sel == 0) {
                        /* 导入项: 扫描 /switch/gta5save/ 下所有画质文件 */
                        scan_switch_gfx_files();
                        if (g_gfx_switch_count > 0) {
                            g_modal_mode = 5;
                            g_modal_sel = 0;
                            g_modal_scr = 0;
                        } else {
                            gfx_import_from_switch(NULL);
                            g_modal_mode = 0;
                        }
                    } else {
                        gfx_apply_preset(g_modal_sel - 1);
                        g_modal_mode = 0;
                    }
                } else if (g_modal_mode == 5) {
                    /* 选定具体画质文件导入 */
                    if (g_gfx_switch_count > 0) {
                        gfx_import_from_switch(g_gfx_switch_files[g_modal_sel]);
                    }
                    g_modal_mode = 0;
                } else if (g_modal_mode == MODE_SCRIPT_MGR) {
                    /* ★ v6.2: A 键 = 对选中项发起二次确认 */
                    int ns = gc_script_stock_count();
                    if (g_modal_sel < ns)      script_mgr_ask(0, g_modal_sel);
                    else                       script_mgr_ask(1, g_modal_sel - ns);
                } else if (g_modal_mode == MODE_SCRIPT_CONFIRM) {
                    /* ★ v6.2: 二次确认 —— 先回列表再执行，避免重入弹窗 */
                    g_modal_mode = MODE_SCRIPT_MGR;
                    script_mgr_apply();
                }
            }
            if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_X) || (kDown & HidNpadButton_Y)) {
                /* ★ v6.2: 确认弹窗里按 B = 只退回列表（不直接关整个弹窗） */
                if (g_modal_mode == MODE_SCRIPT_CONFIRM)
                    g_modal_mode = MODE_SCRIPT_MGR;
                else
                    g_modal_mode = 0;
            }
        }
        else if (g_tab == TAB_GFX) {
            /* ---------------- ★ v4 画质页按键交互 (由页签驱动, 取代原 g_gfx_show) ---------------- */

            /* 一屏行数 (与渲染保持一致) */
            int gfx_vis = (LY_BODY_H + 2) / 40;
            if (gfx_vis > 10) gfx_vis = 10;
            if (gfx_vis < 1)  gfx_vis = 1;

            /* ★ v5: 选中项变化后自动把视图滚到可见范围 (抽出成小函数式写法) */
            #define GFX_ENSURE_VISIBLE()                                            \
                do {                                                                \
                    if (g_gfx_cur_idx < g_gfx_scroll)                               \
                        g_gfx_scroll = g_gfx_cur_idx;                               \
                    else if (g_gfx_cur_idx >= g_gfx_scroll + gfx_vis)               \
                        g_gfx_scroll = g_gfx_cur_idx - gfx_vis + 1;                 \
                    if (g_gfx_scroll < 0) g_gfx_scroll = 0;                         \
                } while (0)

            /* [←/->] 与 [↑/↓] 都做「切换画质项」(v4 起改数值统一交给底部 −/+) */
            if (kDown & (HidNpadButton_Left | HidNpadButton_Up)) {
                if (g_gfx_count > 0) {
                    g_gfx_cur_idx = (char)((g_gfx_cur_idx + g_gfx_count - 1) % g_gfx_count);
                    GFX_ENSURE_VISIBLE();
                }
            }
            if (kDown & (HidNpadButton_Right | HidNpadButton_Down)) {
                if (g_gfx_count > 0) {
                    g_gfx_cur_idx = (char)((g_gfx_cur_idx + 1) % g_gfx_count);
                    GFX_ENSURE_VISIBLE();
                }
            }

            /* ★★ v5 核心修复: 摇杆 ↑/↓ 在画质页也要能滚动列表
             *   (v4 的 nav_down 只在存档页被消费, 画质页完全没接 -> 用户反馈"摇杆无法上下滚动")
             *   ★ v28: nav_down 带 g_fast_scroll（右摇杆一次 3 行） */
            if (nav_down == 1) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                if (g_gfx_count > 0) {
                    for (int s = 0; s < steps; s++) {
                        if (g_gfx_cur_idx > 0) g_gfx_cur_idx--;
                        else                   g_gfx_cur_idx = (char)(g_gfx_count - 1);
                    }
                    GFX_ENSURE_VISIBLE();
                }
            }
            if (nav_down == 2) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                if (g_gfx_count > 0) {
                    for (int s = 0; s < steps; s++) {
                        if (g_gfx_cur_idx + 1 < g_gfx_count) g_gfx_cur_idx++;
                        else                                 g_gfx_cur_idx = 0;
                    }
                    GFX_ENSURE_VISIBLE();
                }
            }

            /* ★ v24: L/R 已收归「全局切页签」, 这里不再接管
             *   （原「翻到顶/翻到底」保留为按钮栏的「翻到顶」「翻到底」） */

            #undef GFX_ENSURE_VISIBLE

            /* [A 键]: 保存画质 */
            if (kDown & HidNpadButton_A) {
                gfx_save();
            }

            /* [B 键]: 返回 DLC 主页（★ v25：DLC 管理已是主页面） */
            if (kDown & HidNpadButton_B) {
                if (g_gfx_dirty) {
                    snprintf(g_status_msg, sizeof(g_status_msg), TR("画质有未保存修改! 点「保存写回」或按 A", "Unsaved graphics changes! Tap Save or press A"));
                    g_status_color = C_ACCENT;
                }
                g_tab = TAB_DLC;
            }

            /* [Y 键]: 呼出内置画质预设列表 */
            if (kDown & HidNpadButton_Y) {
                g_modal_mode = 4;
                g_modal_sel = 0;
                g_modal_scr = 0;
            }
        }
        else if (g_tab == TAB_SAVE) {
            /* =================================================================
             * ★ v27 存档页按键（列表化后）
             *   ↑/↓/←/-> 选属性项（列表循环）   A=保存写回  B=返回DLC主页
             *   X=拉满当前项  Y=切步长  -=当前角色全满  +=保存退出
             *   ZL/ZR / 底部大号 −/+ = 调数值（上方统一处理）
             *   槽位切换 = 按钮栏「切换槽位」；角色切换 = 顶部角色标签栏
             * ================================================================= */

            /* [A 键]: 保存修改 */
            if (kDown & HidNpadButton_A) {
                save_to_disk();
            }

            /* [X 键]: 拉满当前项 (统一语义, 不再兼职"路径自检") */
            if (kDown & HidNpadButton_X) {
                uint32_t mx = (g_cur_attr == 0) ? GTA5_MONEY_LIMIT : 100;
                g_values[g_cur_char][g_cur_attr] = mx;
                g_dirty = 1;
                if (g_cur_attr >= 1 && g_cur_attr <= 7) g_skill_modified[g_cur_char][g_cur_attr] = 1;
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "已将 %s 的【%s】拉满! 按 A 键即可保存写回",
                         CHAR_NAMES_CN[g_cur_char], ATTR_NAMES[g_cur_attr]);
                g_status_color = C_ACCENT;
            }

            /* [Y 键]: 切换步长 (统一语义, 不再兼职"载入内置存档") */
            if (kDown & HidNpadButton_Y) {
                if (g_cur_attr == 0) {
                    g_cash_step_idx = (g_cash_step_idx + 1) % NUM_CASH_STEPS;
                    snprintf(g_status_msg, sizeof(g_status_msg), "金钱步长: %s", CASH_STEP_NAMES[g_cash_step_idx]);
                } else {
                    g_skill_step_idx = (g_skill_step_idx + 1) % NUM_SKILL_STEPS;
                    snprintf(g_status_msg, sizeof(g_status_msg), "技能步长: %s 点", SKILL_STEP_NAMES[g_skill_step_idx]);
                }
                g_status_color = C_BLUE;
            }

            /* ★ v24: L/R 已收归「全局切页签」--用户反馈切角色多余（触屏点顶部标签即可），
             *   原「L=上一个角色 / R=下一个角色」已删除。 */

            /* ★ v25: 界面已从 3×3 网格改成【列表】，导航统一为「上一项 / 下一项」
             *   ↑/↓/←/-> + 摇杆 ↑/↓ 都是 ±1 项（循环），调数值交给底部大号 −/+ */
            if (kDown & HidNpadButton_Up) {
                g_cur_attr = (g_cur_attr + NUM_ATTRS - 1) % NUM_ATTRS;
            }
            if (kDown & HidNpadButton_Down) {
                g_cur_attr = (g_cur_attr + 1) % NUM_ATTRS;
            }
            if (kDown & HidNpadButton_Left) {
                g_cur_attr = (g_cur_attr + NUM_ATTRS - 1) % NUM_ATTRS;
            }
            if (kDown & HidNpadButton_Right) {
                g_cur_attr = (g_cur_attr + 1) % NUM_ATTRS;
            }

            /* 【摇杆 ↑/↓】列表行切换（原 3×3 网格的 ±3 已作废）
             * ★ v28: 右摇杆快速滚动（一次 3 项） */
            if (nav_down == 1) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                for (int s = 0; s < steps; s++)
                    g_cur_attr = (g_cur_attr + NUM_ATTRS - 1) % NUM_ATTRS;
            }
            if (nav_down == 2) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                for (int s = 0; s < steps; s++)
                    g_cur_attr = (g_cur_attr + 1) % NUM_ATTRS;
            }

            /* [- 键]: 当前角色全满 (金钱 21.47 亿 + 8 大技能 100) */
            if (kDown & HidNpadButton_Minus) {
                g_values[g_cur_char][0] = GTA5_MONEY_LIMIT;
                for (int k = 1; k < NUM_ATTRS; k++) {
                    g_values[g_cur_char][k] = 100;
                }
                for (int k = 1; k < 8; k++) {
                    g_skill_modified[g_cur_char][k] = 1;
                }
                g_dirty = 1;
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "已将 %s 金钱21.47亿与全套技能全部拉满 100! 按 A 键保存",
                         CHAR_NAMES_CN[g_cur_char]);
                g_status_color = C_ACCENT;
            }

            /* 【B 键】★ v27: 存档页 B = 返回 DLC 主页（统一「返回上级」语义；
             *   槽位切换保留在按钮栏「切换槽位」按钮，不再占物理键） */
            if (kDown & HidNpadButton_B) {
                g_tab = TAB_DLC;
            }
        }
        else if (g_tab == TAB_PERF) {
            /* =================================================================
             * ★ v24 性能调参页按键（预设列表已删除）
             *   ↑/↓ 选参数（列表按 S/A/B/C 重要性排序）
             *   ←/-> 增减数值        ZL/ZR 大步增减
             *   A   载入 update.rpf
             *   X   写入 update.rpf（带确认）
             *   Y   恢复原版（唯一保留的预设功能）
             *   B   回 DLC 主页
             *   L/R 全局换页（见上方统一处理）
             * ================================================================= */
            /* ★★ 自检报告必须【最优先】处理！
             *   原来它排在 `else if (!g_gc_opened)` 之后 ⇒ 未载入状态下
             *   （正是最需要看自检的时候）永远进不到这个分支 ⇒ 关不掉。
             *   现在提到最前，任何状态下都能关。 */
            if (g_gc_selftest_show) {
                if (kDown & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
                             HidNpadButton_Y | HidNpadButton_Plus | HidNpadButton_Minus |
                             HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL |
                             HidNpadButton_ZR | HidNpadButton_Up | HidNpadButton_Down |
                             HidNpadButton_Left | HidNpadButton_Right)) {
                    g_gc_selftest_show = 0;
                }
            } else if (!g_gc_opened) {
                if (kDown & HidNpadButton_A) gc_open_rpf();
                if (kDown & HidNpadButton_B) g_tab = TAB_DLC;   /* ★ v25 返回主页 */
            } else if (g_gc_confirm) {
                /* 确认弹窗 */
                if (kDown & HidNpadButton_A) {
                    int which = g_gc_confirm;
                    g_gc_confirm = 0;
                    if (which == 1) {
                        gc_write_to_rpf();
                    } else if (which == 2) {
                        /* ★ 恢复原版 = 载入原版参数，然后直接进写入确认 */
                        if (gc_restore_original() == GC_OK) {
                            g_gc_confirm = 1;      /* 顺势弹「确认写入」 */
                        }
                    }
                }
                if (kDown & HidNpadButton_B) { g_gc_confirm = 0; gc_msg("已取消"); }
            } else {
                /* =================================================================
                 * ★★ v24 手动调整（用户要求「预设列表删除，只保留恢复原版的」）
                 *   · 原「预设列表」视图（16 套档位）已整体删除
                 *   · 参数列表直接展开，列表按重要性排序（S 金 -> A 蓝 -> B 灰 -> C 红）
                 *   · 「恢复原版」保留：按钮栏与这里都能触发
                 *   · L/R 已收归全局切页签，这里不再接管
                 * ================================================================= */
                int vis = GC_KNOB_VIS;
                /* ★ v28: 右摇杆快速滚动 */
                if (nav_down == 1) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_gc_knob > 0) g_gc_knob--;
                    if (g_gc_knob < g_gc_knob_scr) g_gc_knob_scr = g_gc_knob;
                }
                if (nav_down == 2) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_gc_knob + 1 < GC_NUM_KNOB_META) g_gc_knob++;
                    if (g_gc_knob >= g_gc_knob_scr + vis) g_gc_knob_scr = g_gc_knob - vis + 1;
                }
                if (kDown & HidNpadButton_Up) {
                    if (g_gc_knob > 0) g_gc_knob--;
                    if (g_gc_knob < g_gc_knob_scr) g_gc_knob_scr = g_gc_knob;
                }
                if (kDown & HidNpadButton_Down) {
                    if (g_gc_knob + 1 < GC_NUM_KNOB_META) g_gc_knob++;
                    if (g_gc_knob >= g_gc_knob_scr + vis) g_gc_knob_scr = g_gc_knob - vis + 1;
                }
                if (kDown & HidNpadButton_Left)  gc_knob_step(-1);
                if (kDown & HidNpadButton_Right) gc_knob_step(+1);
                /* ★ v30: ZL/ZR 大步进（一次 10 步）+ 长按连发
                 *   首帧立即 10 步；按住 20 帧后每 4 帧再 10 步（约 150 步/秒） */
                {
                    static int s_zl_t = 0, s_zr_t = 0;
                    int dzl = 0, dzr = 0;
                    if (kDown & HidNpadButton_ZL) { dzl = 1; s_zl_t = 0; }
                    else if (kHeld & HidNpadButton_ZL) {
                        s_zl_t++;
                        if (s_zl_t >= 20 && (s_zl_t % 4) == 0) dzl = 1;
                    } else s_zl_t = 0;
                    if (kDown & HidNpadButton_ZR) { dzr = 1; s_zr_t = 0; }
                    else if (kHeld & HidNpadButton_ZR) {
                        s_zr_t++;
                        if (s_zr_t >= 20 && (s_zr_t % 4) == 0) dzr = 1;
                    } else s_zr_t = 0;
                    if (dzl) { for (int i = 0; i < 10; i++) gc_knob_step(-1); }
                    if (dzr) { for (int i = 0; i < 10; i++) gc_knob_step(+1); }
                }
                /* Y = 恢复原版（原「切回预设列表」的位置让给它，这是用户唯一要留的预设功能） */
                if (kDown & HidNpadButton_Y) { g_gc_confirm = 2; }
                if (kDown & HidNpadButton_X) {
                    if (g_gc_dirty) g_gc_confirm = 1;
                    else gc_msg("还没有改动");
                }
                if (kDown & HidNpadButton_B) g_tab = TAB_DLC;   /* ★ v25 返回主页 */
            }
        }
        else if (g_tab == TAB_DLC) {
            /* =================================================================
             * ★ v25 DLC 管理页（主要功能，启动默认页）
             *   ↑/↓ 选条目   A 注册/注销（或导入）   Y 切换列表   X 重新扫描
             *   ZR 转格式     ZL 删除选中（先注销两处注册，再清空目录）
             *   B 去存档页    L/R 全局换页
             *   B 回存档页
             * ================================================================= */
            /* ★ v5.5: 脚本 mod 已独立成 TAB_SCRIPT 页，这里不再接管 g_dlc_view==2 */
            /* ★ v25: 删除确认弹窗优先处理（弹窗显示时只有 A/B 有效） */
            if (g_dlc_confirm) {
                if (kDown & HidNpadButton_A) { g_dlc_confirm = 0; dlc_do_delete(); }
                if (kDown & HidNpadButton_B) { g_dlc_confirm = 0; dlc_msg("已取消删除"); }
            }
            else if (!g_dlc_xml_loaded) {
                if (kDown & HidNpadButton_A) {
                    dlc_reload();
                    dlc_scan_src();
                }
                /* ★ v27b: 主页是最顶层，B 无处可退 -- 什么都不做（杜绝误触跳页） */
            } else if (g_dlc_view == 0) {
                /* ---- 已装列表 ---- */
                /* ★ v27b: 行高 44->38（354/38=9 行，44 只装得下 8 行） */
                int vis = (LY_BODY_H + 4) / 50;   /* v6.1: 与渲染端 row_h=50 对齐 + 放开上限 */
                /* ★ v28: 右摇杆 nav_down 转 kDown Up/Down（快速滚动 3 行） */
                if (nav_down == 1) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_dlc_sel > 0) g_dlc_sel--;
                    if (g_dlc_sel < g_dlc_scr) g_dlc_scr = g_dlc_sel;
                }
                if (nav_down == 2) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_dlc_sel + 1 < g_dlc_count) g_dlc_sel++;
                    if (g_dlc_sel >= g_dlc_scr + vis) g_dlc_scr = g_dlc_sel - vis + 1;
                }
                if (kDown & HidNpadButton_Up) {
                    if (g_dlc_sel > 0) g_dlc_sel--;
                    if (g_dlc_sel < g_dlc_scr) g_dlc_scr = g_dlc_sel;
                }
                if (kDown & HidNpadButton_Down) {
                    if (g_dlc_sel + 1 < g_dlc_count) g_dlc_sel++;
                    if (g_dlc_sel >= g_dlc_scr + vis) g_dlc_scr = g_dlc_sel - vis + 1;
                }
                /* ★ v24: L/R 已收归全局换页签，这里不再接管「跳首/跳尾」 */
                if (kDown & HidNpadButton_A) dlc_toggle_selected();
                if (kDown & HidNpadButton_X) {
                    dlc_reload();
                    dlc_scan_src();
                }
                if (kDown & HidNpadButton_ZR) dlc_convert_selected();   /* ★ 转格式 */
                if (kDown & HidNpadButton_ZL) dlc_delete_selected();    /* ★ v25 删除 */
                /* ★ v5.4: − 键 = 屏蔽高清贴图（+hi.ytd 系，对标官方 RemoveHighDetailTextures） */
                if (kDown & HidNpadButton_Minus) dlc_hide_hd_selected();
                if (kDown & HidNpadButton_Y) {
                    dlc_scan_src();
                    g_dlc_view = 1;
                    g_dlc_src_sel = 0; g_dlc_src_scr = 0;
                }
                /* ★ v27: B = 返回上级（已装列表 -> DLC 主页说明视图） */
                if (kDown & HidNpadButton_B) {
                    g_dlc_xml_loaded = 0;   /* 卸载视图，回到「载入并扫描」主页 */
                    g_dlc_view = 0;
                    dlc_msg("已返回 DLC 主页（A 重新载入）");
                }
            } else {
                /* ---- 待导入列表 ---- */
                /* ★ v27b: 行高同步 38 */
                int vis = (LY_BODY_H + 4) / 50;   /* v6.1: 与渲染端 row_h=50 对齐 + 放开上限 */
                /* ★ v28: 右摇杆快速滚动 */
                if (nav_down == 1) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_dlc_src_sel > 0) g_dlc_src_sel--;
                    if (g_dlc_src_sel < g_dlc_src_scr) g_dlc_src_scr = g_dlc_src_sel;
                }
                if (nav_down == 2) {
                    int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                    g_fast_scroll = 0;
                    for (int s = 0; s < steps; s++)
                        if (g_dlc_src_sel + 1 < g_dlc_src_count) g_dlc_src_sel++;
                    if (g_dlc_src_sel >= g_dlc_src_scr + vis)
                        g_dlc_src_scr = g_dlc_src_sel - vis + 1;
                }
                if (kDown & HidNpadButton_Up) {
                    if (g_dlc_src_sel > 0) g_dlc_src_sel--;
                    if (g_dlc_src_sel < g_dlc_src_scr) g_dlc_src_scr = g_dlc_src_sel;
                }
                if (kDown & HidNpadButton_Down) {
                    if (g_dlc_src_sel + 1 < g_dlc_src_count) g_dlc_src_sel++;
                    if (g_dlc_src_sel >= g_dlc_src_scr + vis)
                        g_dlc_src_scr = g_dlc_src_sel - vis + 1;
                }
                if (kDown & HidNpadButton_A) dlc_import_selected();
                if (kDown & HidNpadButton_X) {
                    dlc_scan_dlcpacks();
                    dlc_refresh_registered();
                    dlc_scan_src();
                }
                if (kDown & HidNpadButton_Y) g_dlc_view = 0;
                /* ★ v27: B = 返回上级（待导入 -> 已装列表） */
                if (kDown & HidNpadButton_B) {
                    g_dlc_view = 0;
                    dlc_scan_dlcpacks();
                    dlc_refresh_registered();
                }
            }
        }
        else if (g_tab == TAB_SCRIPT) {
            /* ================= ★ v5.5 脚本 MOD 独立页 =================
             * ↑↓ 选择   A 安装/替换   ZL 删除   X 重扫   Y 看目录   B 回 DLC 页
             * ★ v5.6: 列表上方有 44px 的 RPF 实况条 ⇒ 可视行数相应减少 */
            int vis = (LY_BODY_H - 44) / 50;
            if (vis < 1) vis = 1;
            if (nav_down == 1) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                for (int s = 0; s < steps; s++)
                    if (g_script_sel > 0) g_script_sel--;
                if (g_script_sel < g_script_scr) g_script_scr = g_script_sel;
            }
            if (nav_down == 2) {
                int steps = g_fast_scroll > 0 ? g_fast_scroll : 1;
                g_fast_scroll = 0;
                for (int s = 0; s < steps; s++)
                    if (g_script_sel + 1 < g_script_count) g_script_sel++;
                if (g_script_sel >= g_script_scr + vis) g_script_scr = g_script_sel - vis + 1;
            }
            if (kDown & HidNpadButton_Up) {
                if (g_script_sel > 0) g_script_sel--;
                if (g_script_sel < g_script_scr) g_script_scr = g_script_sel;
            }
            if (kDown & HidNpadButton_Down) {
                if (g_script_sel + 1 < g_script_count) g_script_sel++;
                if (g_script_sel >= g_script_scr + vis) g_script_scr = g_script_sel - vis + 1;
            }
            if (kDown & HidNpadButton_A) script_install_selected();
            if (kDown & HidNpadButton_ZL) script_uninstall_selected();   /* ★ 删除 */
            if (kDown & HidNpadButton_X) {
                script_scan();
                dlc_msg("已重新扫描：%d 个 .nsc 脚本", g_script_count);
            }
            if (kDown & HidNpadButton_Y) {
                /* ★ v5.7: 开关式 */
                if (g_script_dirinfo_on) {
                    g_script_dirinfo_on = 0;
                    dlc_msg("已关闭目录信息");
                } else {
                    script_show_dir();
                }
            }
            if (kDown & HidNpadButton_B) g_tab = TAB_DLC;
        }
        else {
            /* =================================================================
             * ★ v5 工具页: 只保留「返回」与「保存」, 其余全部交给可点按钮,
             *   避免误触发存档页的调数值逻辑 (v4 里工具页会落到存档页分支)
             *   ★ v25: B 返回 DLC 主页
             * ================================================================= */
            if (kDown & HidNpadButton_B) {
                g_tab = TAB_DLC;
            }
            if (kDown & HidNpadButton_A) {
                if (g_dirty) save_to_disk();
                if (g_gfx_dirty) gfx_save();
            }
        }

        /* ---------------- 界面渲染 ---------------- */
        u32 stride;
        uint32_t *framebuf = (uint32_t*)framebufferBegin(&fb, &stride);
        g_stride_words = stride / sizeof(uint32_t);

        /* =====================================================================
         * ★ v5 渲染: 页签化布局 (全部 y 由 LY_* 常量统一控制, 不再散落魔数)
         *   0..62     顶部栏（标题 + 3 个页签）
         *   66..108   信息栏（★ 按页签切换内容: 画质页不再显示槽位信息）
         *   112..150  角色标签栏（仅存档页）
         *   156..510  内容区（存档列表 / 画质滚动列表 / DLC 列表 / 工具页）
         *   516..584  底部 −/+ 调节区（★ 缩小到 150×68, 自绘符号）
         *   588..636  统一按钮栏（★ 按页签切换按钮组, 消除重叠）
         *   642..678  提示行
         * ===================================================================== */
        hot_reset();   /* 每帧重建热区（命中测试用上一帧的，1 帧延迟无感） */

        /* 1. 背景底色 */
        gfx_rect(framebuf, 0, 0, FB_WIDTH, FB_HEIGHT, C_BG);

        /* 2. 顶部栏 -- ★ v28: 无底部分隔线（去边框化） */
        gfx_rect(framebuf, 0, LY_HEADER_Y, FB_WIDTH, LY_HEADER_H, C_HEADER);

        draw_text(framebuf, LY_PAD, 12, 24, TR("GTA V 工具箱", "GTA V Toolbox"), C_TEXT);
        draw_text(framebuf, LY_PAD, 42, 12, "v6.3", C_TEXT_MUTED);   /* ★ v6.2: 原为 "v5.7" 一直没跟着版本走 */

        /* 3. 页签（可点）-- ★ v28: hbmenu 式底边高亮条，无描边 */
        {
            /* ★ 页签：直接用 TABS[] 单一数据源（名字/ID/动作绑在一起）
             * ★ v5.5: 页签从 5 个增加到 6 个 ⇒ 缩窄到 138，避免溢出屏宽
             *   6 × (138+6) = 864，起点 372 ⇒ 终点 1236 < 1280 ✓
             *   标题"GTA V 工具箱"(24px≈200宽) + 版本号 在 0..372 内，不重叠 */
            int tw = 138, th = 42, ty = 10, tx0 = 372;
            for (int t = 0; t < NUM_TABS; t++) {
                int tx = tx0 + t * (tw + 6);
                int sel = (g_tab == TABS[t].id);
                gfx_card(framebuf, tx, ty, tw, th, sel ? C_CARD_SEL : C_HEADER, 6);
                /* 选中 = 底部 2px 主色条（hbmenu 风格） */
                if (sel) gfx_rect(framebuf, tx + 6, ty + th - 3, tw - 12, 2, C_ACCENT);
                draw_text_c(framebuf, tx + tw / 2, ty + 11, 18,
                            TABS[t].name, sel ? C_ACCENT : C_TEXT_MUTED);
                hot_add(tx, ty, tw, th, TABS[t].action, 0);
            }
        }

        /* 4. 信息栏 -- ★ v28: 无描边圆角卡片 */
        gfx_card(framebuf, LY_PAD, LY_INFO_Y, FB_WIDTH - LY_PAD * 2, LY_INFO_H, C_CARD, 8);
        {
            char info[320];
            if (g_tab == TAB_PERF) {
                if (g_gc_opened) {
                    snprintf(info, sizeof(info),
                             TR("gameconfig.xml  %d B   switch 段 %d 项   Any 基准 %d 项   on_disk %d",
                                "gameconfig.xml  %d B   switch section %d   Any baseline %d   on_disk %d"),
                             (int)g_gc_xml_len, g_gc_n_switch, g_gc_n_any, g_gc_orig_on_disk);
                } else {
                    snprintf(info, sizeof(info),
                             TR("未载入 update.rpf%s", "update.rpf not loaded%s"),
                             g_gc_err ? TR(" (打开失败)", " (open failed)") : "");
                    if (g_gc_err) {
                        const char *le = gc_rpf_last_error();
                        if (le && le[0]) {
                            snprintf(info, sizeof(info), TR("打开失败: %s", "Open failed: %s"), le);
                        }
                    }
                }
            } else if (g_tab == TAB_GFX) {
                const char *fn = g_gfx_path[0] ? strrchr(g_gfx_path, '/') : NULL;
                fn = fn ? fn + 1 : (g_gfx_path[0] ? g_gfx_path : TR("未找到 settings.xml", "settings.xml not found"));
                snprintf(info, sizeof(info), TR("画质文件: %s   共 %d 项   当前第 %d 项",
                                                "GFX file: %s   %d items   current #%d"),
                         fn, g_gfx_count,
                         g_gfx_count > 0 ? (int)g_gfx_cur_idx + 1 : 0);
            } else if (g_tab == TAB_DLC) {
                if (g_dlc_view == 0) {
                    int full = 0, part = 0;
                    for (int i = 0; i < g_dlc_count; i++) {
                        if (g_dlc_items[i].full) full++;
                        else if (g_dlc_items[i].registered || g_dlc_items[i].mounted) part++;
                    }
                    snprintf(info, sizeof(info),
                             TR("已装 dlcpack %d 个   完整注册 %d 个   半注册 %d 个%s",
                                "Installed dlcpacks: %d   fully registered %d   partial %d%s"),
                             g_dlc_count, full, part,
                             g_dlc_xml_loaded ? "" : TR("   [dlclist 未载入]", "   [dlclist not loaded]"));
                } else {
                    snprintf(info, sizeof(info),
                             TR("待导入 dlcpack %d 个   来源目录 %d 个   放到 sdmc:/switch/gta5save/dlc/ 下",
                                "Pending dlcpacks: %d   source dirs: %d   put them in sdmc:/switch/gta5save/dlc/"),
                             g_dlc_src_count, g_dlc_src_dir_n);
                }
            } else if (g_tab == TAB_SCRIPT) {
                /* ★ v5.6: 脚本页信息栏 —— 明示目录 + RPF 实况（读失败要说出来） */
                if (g_script_rpf_n < 0) {
                    snprintf(info, sizeof(info),
                             TR("[!] 读 update2.rpf 失败: %s", "[!] Failed to read update2.rpf: %s"),
                             g_script_rpf_err[0] ? g_script_rpf_err : TR("未知原因", "unknown"));
                } else {
                    int ninst = 0;
                    for (int i = 0; i < g_script_count; i++)
                        if (g_script_items[i].installed) ninst++;
                    snprintf(info, sizeof(info),
                             TR("目录 %s | .nsc %d 个（已装 %d） | RPF 内 %d 条",
                                "Dir %s | .nsc %d (installed %d) | in RPF %d"),
                             SCRIPT_DIR, g_script_count, ninst, g_script_rpf_n);
                }
            } else if (g_tab == TAB_TOOL) {
                const char *fn = strrchr(g_save_path, '/');
                fn = fn ? fn + 1 : g_save_path;
                snprintf(info, sizeof(info), TR("存档: %s   %zu KB   槽位 %d 个   备份 %d 个",
                                                "Save: %s   %zu KB   slots %d   backups %d"),
                         fn, g_save_sz / 1024, g_num_slots, g_num_backups);
            } else {
                const char *cur_fn = (g_num_slots > 0) ? g_slots[g_active_slot_idx].filename
                                                       : "SGTA50000";
                const char *disp_user = g_user_name[0] ? g_user_name : TR("未知玩家", "Player");
                snprintf(info, sizeof(info), TR("玩家: %s   槽位: %s   剧情: %s",
                                                "Player: %s   Slot: %s   Story: %s"),
                         disp_user, cur_fn,
                         g_current_title[0] ? g_current_title : TR("自定义存档", "Custom save"));
            }
            draw_text(framebuf, LY_PAD + 18, LY_INFO_Y + 11, 16, info, C_ACCENT);
        }
        /* 未保存标记: 自绘圆点, 不依赖字体 */
        {
            int dirty = (g_tab == TAB_GFX) ? g_gfx_dirty : g_dirty;
            if (dirty) {
                gfx_dot(framebuf, FB_WIDTH - LY_PAD - 132, LY_INFO_Y + 21, 6, C_RED);
                draw_text(framebuf, FB_WIDTH - LY_PAD - 118, LY_INFO_Y + 11, 16,
                          TR("有未保存修改", "Unsaved changes"), C_RED);
            }
        }

        /* 5. ★ v6.1: 原「角色标签栏」已随「存档修改」页一并移除。
         *   该条带（112..150）现被内容区吸收 —— 见 LY_BODY_Y/LY_BODY_H 定义。 */

        /* =====================================================================
         * 6. 内容区（按页签切换）
         * ===================================================================== */
        if (g_tab == TAB_SAVE) {
            /* ---------- ★ v25 存档页：属性列表（可点）
             *   原 3×3 网格改成单列列表：左名字、右数值、技能项底部带进度条
             *   ★ v29: 行高 44，9 项一屏装下（LY_BODY_H=420），底部留出页脚行 ---------- */
            int lx = LY_PAD, ly = LY_BODY_Y, lw = LY_CONTENT_R - lx;
            /* ★ v29 修复：行高 46->44 -- 原来 9×46=414 ⇒ 列表到 y=570，
             *   页脚文字画在 578..591，被按钮栏（LY_BTN_Y=584 起）压住。
             *   44 时列表到 552，页脚 560..573，安全留白 11px。 */
            int row_h = 44;
            int vis = LY_BODY_H / row_h;      /* = 9，刚好 NUM_ATTRS */
            if (vis > NUM_ATTRS) vis = NUM_ATTRS;
            if (g_cur_attr < 0) g_cur_attr = 0;
            if (g_cur_attr >= NUM_ATTRS) g_cur_attr = NUM_ATTRS - 1;

            for (int r = 0; r < vis; r++) {
                int a = r;                    /* 一屏放得下，不用滚动 */
                int iy = ly + r * row_h;
                int sel = (a == g_cur_attr);
                uint32_t val = g_values[g_cur_char][a];

                /* ★ v28: 圆角卡片 + 选中主色条；类型色条保留（金钱绿/技能蓝） */
                gfx_card(framebuf, lx, iy, lw, row_h - 3, sel ? C_ITEM_SEL : C_CARD, 6);
                gfx_rect(framebuf, lx + 2, iy + 2, 5, row_h - 8,
                         (a == 0) ? C_GREEN : C_BLUE);
                if (sel) gfx_accent_bar(framebuf, lx + lw - 12, iy + 4, row_h - 11, C_ACCENT);

                draw_text(framebuf, lx + 18, iy + 5, 18, ATTR_NAMES[a], C_TEXT);

                if (!g_found[g_cur_char][a]) {
                    draw_text_r(framebuf, lx + lw - 24, iy + 12, 15,
                                "(存档中未找到)", C_RED);
                } else if (a == 0) {
                    char vs[48];
                    snprintf(vs, sizeof(vs), "$ %'" PRIu32, val);
                    draw_text_r(framebuf, lx + lw - 24, iy + 9, 24, vs, C_GREEN);
                } else {
                    char vs[32];
                    snprintf(vs, sizeof(vs), "%u / 100", val);
                    draw_text_r(framebuf, lx + lw - 24, iy + 8, 20, vs,
                                (val >= 100) ? C_GREEN : C_TEXT);
                    /* 进度条（名字下方，靠左，不压数值）-- v28: 无边框纯色条 */
                    int bw = 300, bh = 6, bx = lx + 18, by = iy + 32;
                    gfx_rect(framebuf, bx, by, bw, bh, C_BG);
                    int fw = (int)((val > 100 ? 100 : val) * bw / 100);
                    if (fw > 0)
                        gfx_rect(framebuf, bx, by, fw, bh, C_BLUE);
                }
                hot_add(lx, iy, lw, row_h - 3, ACT_PICK_ITEM, a);
            }
            {
                char pg[128];
                snprintf(pg, sizeof(pg),
                         "%s : 共 %d 项   ↑↓<-->选择  ZL/ZR 调值(可长按快调)  X 拉满  Y 切步长",
                         CHAR_NAMES_CN[g_cur_char], NUM_ATTRS);
                draw_text(framebuf, lx, ly + vis * row_h + 8, 13, pg, C_TEXT_MUTED);
            }
        }
        else if (g_tab == TAB_GFX) {
            /* ---------- 画质页：可滚动列表（可点） ----------
             * ★ v5 修正:
             *   (1) 列表右边留出 22px 给滚动条, 数值不再被滚动按钮压住
             *   (2) 滚动按钮移到列表【左侧外】的独立竖条区, 彻底避开数值列
             *   (3) 数值用 text_width 精确右对齐 (原来是 strlen*10 错估) */
            int lx = LY_PAD, ly = LY_BODY_Y;
            int row_h = 40, vis = (LY_BODY_H + 2) / row_h;   /* v6.1: 464/40=11 行 */
            int lw = LY_CONTENT_R - lx;                      /* 列表宽度 (到 1218) */

            /* 裁剪：只画可见范围 (★ 防负数) */
            int max_scroll = g_gfx_count - vis;
            if (max_scroll < 0) max_scroll = 0;
            if (g_gfx_scroll > max_scroll) g_gfx_scroll = max_scroll;
            if (g_gfx_scroll < 0) g_gfx_scroll = 0;

            for (int r = 0; r < vis; r++) {
                int i = g_gfx_scroll + r;
                if (i >= g_gfx_count) break;
                GfxItem *it = &g_gfx_items[i];
                int iy = ly + r * row_h;
                int sel = (i == g_gfx_cur_idx);

                /* ★ v28: 圆角卡片 + 选中主色条 */
                gfx_card(framebuf, lx, iy, lw, row_h - 3, sel ? C_ITEM_SEL : C_CARD, 6);
                if (sel) gfx_accent_bar(framebuf, lx + 4, iy + 4, row_h - 11, C_ACCENT);

                draw_text(framebuf, lx + 16, iy + 9, 17, it->cn,
                          sel ? C_TEXT : (it->is_key ? C_TEXT : C_TEXT_MUTED));

                /* 值靠右显示 (精确测量, 右边界 = 列表右边 - 16) */
                draw_text_r(framebuf, lx + lw - 16, iy + 9, 17, it->value, C_GREEN);

                hot_add(lx, iy, lw, row_h - 3, ACT_PICK_ITEM, 100000 + i);
            }

            /* 滚动条 (紧贴列表右侧 22px 通道内, 不压任何文字) -- v28: 无边框纯色 */
            if (g_gfx_count > vis && max_scroll > 0) {
                int sb_x = LY_CONTENT_R + 6, sb_y = ly, sb_h = vis * row_h - 3;
                gfx_rect(framebuf, sb_x, sb_y, 8, sb_h, C_CARD);
                int th = sb_h * vis / g_gfx_count;
                if (th < 16) th = 16;
                if (th > sb_h) th = sb_h;
                int ty2 = sb_y + (sb_h - th) * g_gfx_scroll / max_scroll;
                gfx_rect(framebuf, sb_x + 1, ty2 + 1, 6, th - 2, C_ACCENT);
            }
        }
        else if (g_tab == TAB_PERF) {
            /* =================================================================
             * ★ v6 性能调参页
             * ================================================================= */
            int lx = LY_PAD, ly = LY_BODY_Y, lw = FB_WIDTH - LY_PAD * 2;

            if (!g_gc_opened) {
                /* ---- 未载入：显示提示 + 载入按钮 ---- */
                gfx_card(framebuf, lx, ly, lw, 268, C_CARD, 8);
                draw_text(framebuf, lx + 20, ly + 12, 19, TR("性能调参 (gameconfig.xml)", "Performance tuning (gameconfig.xml)"), C_ACCENT);
                /* ★ 实测结论：这些参数不影响帧率（瓶颈在硬件），必须说明，避免误导 */
                draw_text(framebuf, lx + 20, ly + 40, 14,
                          TR("实测: 本页参数只改变【人口/载具密度】，不影响帧率",
                             "Tested: these only change ped/vehicle density, NOT framerate"), C_ACCENT);
                draw_text(framebuf, lx + 20, ly + 62, 13,
                          TR("瓶颈在 Tegra X1 的 CPU/GPU/内存带宽，需超频才能提帧",
                             "The bottleneck is the Tegra X1 CPU/GPU/RAM; overclocking is the only fix"), C_TEXT_MUTED);
                if (g_gc_err) {
                    char eb[300];
                    snprintf(eb, sizeof(eb), TR("打开失败: %s", "Open failed: %s"), gc_rpf_last_error());
                    draw_text(framebuf, lx + 20, ly + 90, 15, eb, C_RED);
                    /* 逐条显示候选路径的探测结果 */
                    if (g_gc_diag_n == 0) gc_build_diag();
                    for (int i = 0; i < g_gc_diag_n && i < 4; i++) {
                        draw_text(framebuf, lx + 20, ly + 114 + i * 20, 13,
                                  g_gc_diag[i],
                                  strncmp(g_gc_diag[i], "存在", 4) == 0 ? C_GREEN : C_TEXT_MUTED);
                    }
                } else {
                    draw_text(framebuf, lx + 20, ly + 90, 16,
                              TR("点「载入」读取 update.rpf 里的 gameconfig.xml",
                                 "Tap Load to read gameconfig.xml from update.rpf"), C_TEXT);
                    draw_text(framebuf, lx + 20, ly + 118, 14,
                              "路径: sdmc:/atmosphere/contents/0100b00b51230000/romfs/update/update.rpf",
                              C_TEXT_MUTED);
                }
                int bx = lx + 20, by = ly + 200, bw = 180, bh = 46;
                gfx_card(framebuf, bx, by, bw, bh, C_HEADER, 6);
                draw_text_c(framebuf, bx + bw / 2, by + 12, 19, TR("载入", "Load"), C_ACCENT);
                hot_add(bx, by, bw, bh, ACT_GC_LOAD, 0);

                /* ★ 自检按钮：把路径探测结果直接显示在屏幕上 */
                int bx2 = bx + bw + 16, bw2 = 200;
                gfx_card(framebuf, bx2, by, bw2, bh, C_CARD, 6);
                draw_text_c(framebuf, bx2 + bw2 / 2, by + 12, 19, TR("路径自检", "Self-test"), C_BLUE);
                hot_add(bx2, by, bw2, bh, ACT_GC_SELFTEST, 0);
            } else {
                /* =================================================================
                 * ★★ v24 手动调整（用户要求「预设列表删除，只保留恢复原版的」）
                 *   原「16 套预设档位列表」视图已删除；参数列表直接展开。
                 *   列表已按重要性排序：S 关键(金) -> A 重要(蓝) -> B 次要(灰) -> C 勿调(红)
                 * ================================================================= */
                int row_h = 34, vis = GC_KNOB_VIS;
                for (int r = 0; r < vis; r++) {
                    int i = g_gc_knob_scr + r;
                    if (i >= GC_NUM_KNOB_META) break;
                    const GcKnobMeta *m = &GC_KNOB_META[i];
                    int iy = ly + r * row_h;
                    int sel = (i == g_gc_knob);

                    /* 权重配色：S 金 / A 蓝 / B 灰 / C 红（警告别调） */
                    uint32_t wcol = (m->weight == 1) ? C_GOLD
                                  : (m->weight == 2) ? C_BLUE
                                  : (m->weight == 3) ? C_TEXT_MUTED
                                                     : C_RED;

                    gfx_card(framebuf, lx, iy, lw, row_h - 3, sel ? C_ITEM_SEL : C_CARD, 6);
                    /* 左侧权重色条 */
                    gfx_rect(framebuf, lx + 2, iy + 2, 4, row_h - 7, wcol);
                    if (sel) gfx_accent_bar(framebuf, lx + lw - 12, iy + 4, row_h - 11, C_ACCENT);

                    /* 当前值：已改的用金色，否则读 XML（switch 段优先，其次 Any） */
                    int cur = gc_get_edited(m->key);
                    int is_edited = (cur != INT32_MIN);
                    if (!is_edited) {
                        int v = 0;
                        if (gc_xml_get_int((char*)g_gc_xml, g_gc_xml_len,
                                           g_gc_sw_a, g_gc_sw_b, m->key, &v) == GC_OK) cur = v;
                        else if (gc_xml_get_int((char*)g_gc_xml, g_gc_xml_len,
                                                g_gc_any_a, g_gc_any_b, m->key, &v) == GC_OK) cur = v;
                        else cur = 0;
                    }

                    /* 权重标签 + 中文名 */
                    char nb[96];
                    snprintf(nb, sizeof(nb), "[%s] %s",
                             m->weight == 1 ? "S" : m->weight == 2 ? "A"
                           : m->weight == 3 ? "B" : "C", m->cn);
                    draw_text(framebuf, lx + 14, iy + 6, 15, nb, sel ? C_ACCENT : wcol);

                    char vb[64];
                    snprintf(vb, sizeof(vb), "%d", cur);
                    draw_text_r(framebuf, lx + lw - 110, iy + 6, 16, vb,
                                is_edited ? C_GOLD : C_GREEN);
                    char rb[32];
                    snprintf(rb, sizeof(rb), "%d~%d", m->lo, m->hi);
                    draw_text_r(framebuf, lx + lw - 16, iy + 7, 13, rb, C_TEXT_MUTED);
                    hot_add(lx, iy, lw, row_h - 3, ACT_GC_PICK_KNOB, i);
                }
                char pg[128];
                snprintf(pg, sizeof(pg),
                         TR("旋钮 %d/%d   已改 %d 项   [S]关键 [A]重要 [B]次要 [C]容量池勿调",
                            "Knob %d/%d   %d edited   [S]key [A]major [B]minor [C]pool - do not touch"),
                         g_gc_knob + 1, GC_NUM_KNOB_META, g_gc_edited_n);
                draw_text(framebuf, lx, ly + vis * row_h + 4, 13, pg, C_TEXT_MUTED);
            }
        }
        else if (g_tab == TAB_DLC) {
            /* ================= DLC 管理页 =================
             * 两个视图：0=已装 dlcpack（可注册/注销）  1=待导入（可导入）
             * ★ v5.5: 脚本 mod（.nsc）已独立成「脚本MOD」页 */
            int lx = LY_PAD, ly = LY_BODY_Y, lw = LY_CONTENT_R - lx;

            if (!g_dlc_xml_loaded) {
                /* ---- 未载入：显示说明 + 载入按钮 ---- */
                gfx_card(framebuf, lx, ly, lw, 190, C_CARD, 8);
                draw_text(framebuf, lx + 20, ly + 14, 19, TR("DLC 管理（PC 车辆 mod 移植）", "DLC Manager (PC vehicle mod porting)"), C_ACCENT);
                draw_text(framebuf, lx + 20, ly + 48, 15,
                          TR("先把 PC 的 dlcpack 目录（含 dlc.rpf）放进 sdmc:/switch/gta5save/dlc/",
                             "First put the PC dlcpack folder (with dlc.rpf) into sdmc:/switch/gta5save/dlc/"), C_TEXT);
                draw_text(framebuf, lx + 20, ly + 72, 15,
                          TR("本工具会：(1) 复制到 dlcpacks/  (2) 写 dlclist.xml  (3) 写 extratitleupdatedata.meta",
                             "This tool will: (1) copy to dlcpacks/  (2) write dlclist.xml  (3) write extratitleupdatedata.meta"), C_TEXT);
                draw_text(framebuf, lx + 20, ly + 96, 15,
                          TR("(4) 回读校验  (5) 检查车辆资源是否缺 _hi 高模（缺了撞击会闪退）",
                             "(4) verify by re-read  (5) check vehicle _hi models (missing = crash on impact)"), C_TEXT);
                draw_text(framebuf, lx + 20, ly + 126, 14,
                          TR("落点: romfs/update/switch/dlcpacks/<名字>/dlc.rpf",
                             "Target: romfs/update/switch/dlcpacks/<name>/dlc.rpf"), C_TEXT_MUTED);
                draw_text(framebuf, lx + 20, ly + 148, 14,
                          TR("★ 一次只加一个 DLC，启动确认能进游戏再加下一个",
                             "* Add one DLC at a time; confirm the game boots before adding the next"), C_GOLD);

                int bx = lx + 20, by = ly + 200, bw = 220, bh = 48;
                gfx_card(framebuf, bx, by, bw, bh, C_HEADER, 6);
                draw_text_c(framebuf, bx + bw / 2, by + 13, 19, TR("载入并扫描", "Load & Scan"), C_ACCENT);
                hot_add(bx, by, bw, bh, ACT_DLC_LOAD, 0);
            } else if (g_dlc_view == 0) {
                /* ---- 已装列表 ---- */
                int row_h = 50, vis = (LY_BODY_H + 4) / row_h;   /* v6.1: 放开上限 */
                if (g_dlc_sel < 0) g_dlc_sel = 0;
                if (g_dlc_sel >= g_dlc_count) g_dlc_sel = g_dlc_count > 0 ? g_dlc_count - 1 : 0;
                if (g_dlc_sel < g_dlc_scr) g_dlc_scr = g_dlc_sel;
                if (g_dlc_sel >= g_dlc_scr + vis) g_dlc_scr = g_dlc_sel - vis + 1;
                if (g_dlc_scr < 0) g_dlc_scr = 0;

                if (g_dlc_count == 0) {
                    draw_text(framebuf, lx + 20, ly + 20, 18,
                              "dlcpacks 目录里没有子目录（路径不对？）", C_RED);
                    draw_text(framebuf, lx + 20, ly + 50, 14, DLC_DLCPACKS, C_TEXT_MUTED);
                }
                for (int r = 0; r < vis; r++) {
                    int i = g_dlc_scr + r;
                    if (i >= g_dlc_count) break;
                    DlcItem *it = &g_dlc_items[i];
                    int iy = ly + r * row_h;
                    int sel = (i == g_dlc_sel);

                    /* ★ v28: 圆角卡片无描边；选中 = 色块变亮 + 左侧 3px 主色条 */
                    gfx_card(framebuf, lx, iy, lw, row_h - 4, sel ? C_ITEM_SEL : C_CARD, 6);
                    if (sel) gfx_accent_bar(framebuf, lx + 4, iy + 4, row_h - 12, C_ACCENT);

                    char n1[200];
                    /* ★ v5.1: 有中文名就显示中文名（目录名挪到第 2 行） */
                    snprintf(n1, sizeof(n1), "%s", it->cn[0] ? it->cn : it->name);
                    draw_text(framebuf, lx + 18, iy + 5, 17, n1, C_TEXT);

                    /* ★ v28: 状态点（绿=完整注册 / 金=半注册 / 灰=未注册）+ 文字 */
                    {
                        uint32_t dotc = it->full ? C_GREEN
                                      : ((it->registered || it->mounted) ? C_GOLD : C_TEXT_MUTED);
                        const char *stt = it->full ? "已注册"
                                        : ((it->registered || it->mounted) ? "半注册" : "未注册");
                        gfx_dot(framebuf, lx + 200, iy + 12, 4, dotc);
                        draw_text(framebuf, lx + 212, iy + 7, 12, stt, C_TEXT_MUTED);
                    }

                    char n2[260];
                    if (it->cn[0])   /* 有中文名 ⇒ 第 2 行开头补上目录名，便于对照路径 */
                        snprintf(n2, sizeof(n2), "%s  dlc.rpf %u KB  dlclist:%s etud:%s",
                                 it->name, it->rpf_size / 1024,
                                 it->registered ? "有" : "无",
                                 it->mounted ? "有" : "无");
                    else
                        snprintf(n2, sizeof(n2), "dlc.rpf %u KB   dlclist:%s   etud:%s",
                                 it->rpf_size / 1024,
                                 it->registered ? "有" : "无",
                                 it->mounted ? "有" : "无");
                    draw_text(framebuf, lx + 18, iy + 22, 12, n2, C_TEXT_MUTED);

                    /* ★ v32: 第 3 行 -- 资源构成（模型 / 地图数据 / 贴图） */
                    {
                        char n3[220];
                        if (it->spawn[0])
                            snprintf(n3, sizeof(n3), TR("模型 %d   地图 %d   贴图 %d   刷车: %s",
                                      "models %d   map %d   tex %d   spawn: %s"),
                                     it->n_model, it->n_map, it->n_tex, it->spawn);
                        else
                            snprintf(n3, sizeof(n3), TR("模型 %d   地图 %d   贴图 %d",
                                      "models %d   map %d   tex %d"),
                                     it->n_model, it->n_map, it->n_tex);
                        draw_text(framebuf, lx + 18, iy + 35, 11, n3,
                                  it->spawn[0] ? C_ACCENT : C_TEXT_MUTED);
                    }

                    /* ★ v31: 地图数据自检提醒（只在异常时显示，避免杂乱）
                     *   map_st==2 ⇒ manifest 不在主 rpf ⇒ 实测：贴图能显示、
                     *   模型完全不渲染、无碰撞（AkinaV 秋名山就是这个症状） */
                    if (it->map_st == 2) {
                        draw_text_r(framebuf, lx + lw - 400, iy + 22, 12,
                                    "地图数据不在主包(不会加载)", C_RED);
                    } else if (it->map_st == 1) {
                        draw_text_r(framebuf, lx + lw - 400, iy + 22, 12,
                                    "地图正常", C_GREEN);
                    }

                    /* ★ 平台格式徽标：PC 格式 = 会闪退，必须显眼 */
                    {
                        const char *fb_txt;
                        uint32_t fb_col;
                        if (it->fmt == 1)      { fb_txt = "Switch 格式"; fb_col = C_GREEN; }
                        else if (it->fmt == 0) { fb_txt = "★ PC 格式（会闪退！）"; fb_col = C_RED; }
                        else                   { fb_txt = "读不了"; fb_col = C_TEXT_MUTED; }
                        draw_text_r(framebuf, lx + lw - 200, iy + 22, 12, fb_txt, fb_col);
                    }

                    if (sel) {
                        draw_text_r(framebuf, lx + lw - 16, iy + 5, 14,
                                    it->full ? TR("A 注销", "A Unreg") : TR("A 注册", "A Reg"), C_ACCENT);
                        /* ★ v25: 删除提示（危险操作，放第二行靠右） */
                        draw_text_r(framebuf, lx + lw - 16, iy + 22, 12,
                                    TR("ZL 删除", "ZL Del"), C_RED);
                    }
                    hot_add(lx, iy, lw, row_h - 4, ACT_DLC_PICK, i);
                }
                {
                    char pg[200];
                    snprintf(pg, sizeof(pg),
                             TR("共 %d 个   第 %d 项   ↑↓选择  A注册  ZL删除  ZR转格式  [-]删高清贴图",
                                "Total %d   #%d   Up/Down select  A reg  ZL del  ZR convert  [-] strip HD"),
                             g_dlc_count, g_dlc_sel + 1);
                    draw_text(framebuf, lx, ly + vis * row_h + 2, 13, pg, C_TEXT_MUTED);
                }
            } else {
                /* ---- 待导入列表 ---- */
                int row_h = 50, vis = (LY_BODY_H + 4) / row_h;   /* v6.1: 放开上限 */
                if (g_dlc_src_sel < 0) g_dlc_src_sel = 0;
                if (g_dlc_src_sel >= g_dlc_src_count)
                    g_dlc_src_sel = g_dlc_src_count > 0 ? g_dlc_src_count - 1 : 0;
                if (g_dlc_src_sel < g_dlc_src_scr) g_dlc_src_scr = g_dlc_src_sel;
                if (g_dlc_src_sel >= g_dlc_src_scr + vis) g_dlc_src_scr = g_dlc_src_sel - vis + 1;
                if (g_dlc_src_scr < 0) g_dlc_src_scr = 0;

                if (g_dlc_src_count == 0) {
                    draw_text(framebuf, lx + 20, ly + 20, 18,
                              TR("没找到待导入的 dlcpack", "No pending dlcpacks found"), C_ACCENT);
                    draw_text(framebuf, lx + 20, ly + 52, 15,
                              TR("把 dlcpack 目录（里面要有 dlc.rpf）放进下面任一目录：",
                              "Put the dlcpack folder (must contain dlc.rpf) into any of:"), C_TEXT);
                    const char *hint[] = {
                        TR("sdmc:/switch/gta5save/dlc/     <- 推荐", "sdmc:/switch/gta5save/dlc/     <- recommended"),
                        "sdmc:/switch/gta5save/",
                        "sdmc:/switch/GTA5DLC/",
                        "sdmc:/dlc/",
                    };
                    for (int k = 0; k < 4; k++)
                        draw_text(framebuf, lx + 40, ly + 82 + k * 24, 14, hint[k], C_TEXT_MUTED);
                    draw_text(framebuf, lx + 20, ly + 190, 14,
                              TR("放好后按 Y 键或点下面「重新扫描」", "Then press Y or tap Rescan below"), C_ACCENT);
                }
                for (int r = 0; r < vis; r++) {
                    int i = g_dlc_src_scr + r;
                    if (i >= g_dlc_src_count) break;
                    DlcItem *it = &g_dlc_src[i];
                    int iy = ly + r * row_h;
                    int sel = (i == g_dlc_src_sel);

                    /* ★ v28: 圆角卡片 + 选中主色条 */
                    gfx_card(framebuf, lx, iy, lw, row_h - 4, sel ? C_ITEM_SEL : C_CARD, 6);
                    if (sel) gfx_accent_bar(framebuf, lx + 4, iy + 4, row_h - 12, C_ACCENT);

                    draw_text(framebuf, lx + 18, iy + 5, 17,
                              it->cn[0] ? it->cn : it->name, C_TEXT);
                    char n2[240];
                    /* ★ 源包也检测格式：PC 格式导入时会自动转换（告知用户） */
                    char srp[700];
                    int fmt = -1;
                    if (dlc_src_path_of(it->name, srp, sizeof(srp)) == 0) {
                        char rp2[760];
                        snprintf(rp2, sizeof(rp2), "%s/dlc.rpf", srp);
                        fmt = gc_dlc_is_switch_format(rp2);
                    }
                    if (fmt == 0) {
                        snprintf(n2, sizeof(n2),
                                 TR("dlc.rpf %u KB   ★ PC 格式 -> 导入时自动转换",
                                    "dlc.rpf %u KB   * PC format -> auto-converted on import"),
                                 it->rpf_size / 1024);
                        draw_text(framebuf, lx + 18, iy + 22, 12, n2, C_ACCENT);
                    } else if (fmt == 1) {
                        snprintf(n2, sizeof(n2),
                                 TR("dlc.rpf %u KB   已是 Switch 格式，可直接导入",
                                    "dlc.rpf %u KB   already Switch format, ready to import"),
                                 it->rpf_size / 1024);
                        draw_text(framebuf, lx + 18, iy + 22, 12, n2, C_GREEN);
                    } else {
                        snprintf(n2, sizeof(n2), TR("dlc.rpf %u KB   待导入", "dlc.rpf %u KB   pending"),
                                 it->rpf_size / 1024);
                        draw_text(framebuf, lx + 18, iy + 22, 12, n2, C_TEXT_MUTED);
                    }
                    /* ★ v32: 第 3 行 -- 资源构成（待导入的 PC 包也能统计） */
                    {
                        char n3[220];
                        if (it->spawn[0])
                            snprintf(n3, sizeof(n3), TR("模型 %d   地图 %d   贴图 %d   刷车: %s",
                                      "models %d   map %d   tex %d   spawn: %s"),
                                     it->n_model, it->n_map, it->n_tex, it->spawn);
                        else
                            snprintf(n3, sizeof(n3), TR("模型 %d   地图 %d   贴图 %d",
                                      "models %d   map %d   tex %d"),
                                     it->n_model, it->n_map, it->n_tex);
                        draw_text(framebuf, lx + 18, iy + 35, 11, n3,
                                  it->spawn[0] ? C_ACCENT : C_TEXT_MUTED);
                    }
                    if (sel) {
                        draw_text_r(framebuf, lx + lw - 16, iy + 5, 14,
                                    TR("A 导入", "A Import"), C_ACCENT);
                    }
                    hot_add(lx, iy, lw, row_h - 4, ACT_DLC_PICK, 1000 + i);
                }
                char pg[180];
                snprintf(pg, sizeof(pg),
                         TR("待导入 %d 个   第 %d 项   ↑↓选择  A导入  Y已装列表",
                            "Pending %d   #%d   Up/Down select  A import  Y installed"),
                         g_dlc_src_count, g_dlc_src_count > 0 ? g_dlc_src_sel + 1 : 0);
                draw_text(framebuf, lx, ly + vis * row_h + 2, 13, pg, C_TEXT_MUTED);
            }

            /* ★ v27b: 内容区底部按钮栏已删除（与统一按钮栏 B_DLC 重复），
             *   行高 44->38，列表 9 行。 */

            /* ★ v25: 删除确认弹窗（画在 DLC 页所有内容之上） */
            if (g_dlc_confirm) {
                const char *dname = (g_dlc_sel >= 0 && g_dlc_sel < g_dlc_count)
                                  ? g_dlc_items[g_dlc_sel].name : "?";
                int mw = 720, mh = 300;
                int mx = (FB_WIDTH - mw) / 2, my = (FB_HEIGHT - mh) / 2;
                /* 遮罩：吃掉弹窗外的所有点击（ACT_NONE 什么都不做） */
                hot_add(0, 0, FB_WIDTH, my, ACT_NONE, 0);
                hot_add(0, my + mh, FB_WIDTH, FB_HEIGHT - my - mh, ACT_NONE, 0);
                hot_add(0, my, mx, mh, ACT_NONE, 0);
                hot_add(mx + mw, my, FB_WIDTH - mx - mw, mh, ACT_NONE, 0);

                gfx_card(framebuf, mx, my, mw, mh, C_MODAL_BG, 10);
                draw_text_c(framebuf, FB_WIDTH / 2, my + 22, 24, TR("★ 删除 DLC ?", "* Delete DLC?"), C_RED);
                char l1[256];
                snprintf(l1, sizeof(l1), TR("将彻底移除「%s」", "Will completely remove \"%s\""), dname);
                draw_text_c(framebuf, FB_WIDTH / 2, my + 76, 19, l1, C_TEXT);
                draw_text_c(framebuf, FB_WIDTH / 2, my + 116, 16,
                            TR("1. 从 dlclist.xml + extratitleupdatedata.meta 注销（游戏不再加载）",
                               "1. Unregister from dlclist.xml + extratitleupdatedata.meta (game stops loading it)"),
                            C_TEXT_MUTED);
                draw_text_c(framebuf, FB_WIDTH / 2, my + 144, 16,
                            TR("2. 清空 dlcpacks/<名字>/ 目录", "2. Empty the dlcpacks/<name>/ folder"), C_TEXT_MUTED);
                draw_text_c(framebuf, FB_WIDTH / 2, my + 184, 15,
                            TR("★ SD 卡里预先存在的文件可能删不掉，届时请用电脑删除该目录",
                               "* Pre-existing files on the SD card may not be deletable; remove the folder from a PC if so"),
                            C_GOLD);

                /* 两个按钮 -- v28: 圆角 */
                int bw3 = 200, bh3 = 50, gp3 = 40;
                int bx3 = (FB_WIDTH - (bw3 * 2 + gp3)) / 2, by3 = my + 224;
                int pa = tap_is_hot(bx3, by3, bw3, bh3);
                gfx_card(framebuf, bx3, by3, bw3, bh3, pa ? C_CARD_SEL : C_HEADER, 6);
                draw_text_c(framebuf, bx3 + bw3 / 2, by3 + 13, 20, TR("A 确认删除", "A Delete"),
                            pa ? 0xFFFFFFFFu : C_RED);
                hot_add(bx3, by3, bw3, bh3, ACT_CONFIRM, 0);

                int bx4 = bx3 + bw3 + gp3;
                int pb = tap_is_hot(bx4, by3, bw3, bh3);
                gfx_card(framebuf, bx4, by3, bw3, bh3, pb ? C_CARD_SEL : C_CARD, 6);
                draw_text_c(framebuf, bx4 + bw3 / 2, by3 + 13, 20, TR("B 取消", "B Cancel"),
                            pb ? 0xFFFFFFFFu : C_TEXT);
                hot_add(bx4, by3, bw3, bh3, ACT_BACK, 0);
            }
        }
        else if (g_tab == TAB_SCRIPT) {
            /* ================= ★ v5.5 脚本 MOD 独立页 =================
             * 从 DLC 页第 3 视图提升为顶级页签，并：
             *   · 自动创建存放目录（用户不用手动建）
             *   · 页面上明示路径（信息栏 + 空列表时的提示块）
             *   · 删除功能保留（ZL 键） */
            int lx = LY_PAD, ly = LY_BODY_Y, lw = LY_CONTENT_R - lx;
            int row_h = 50;
            if (g_script_sel < 0) g_script_sel = 0;
            if (g_script_sel >= g_script_count)
                g_script_sel = g_script_count > 0 ? g_script_count - 1 : 0;
            if (g_script_sel < g_script_scr) g_script_scr = g_script_sel;
            if (g_script_scr < 0) g_script_scr = 0;

            if (g_script_count == 0) {
                /* ---- 空列表：把「目录在哪、怎么放」讲清楚 ---- */
                gfx_card(framebuf, lx, ly, lw, 250, C_CARD, 8);
                draw_text(framebuf, lx + 20, ly + 14, 20,
                          TR("还没有 .nsc 脚本", "No .nsc scripts yet"), C_ACCENT);
                draw_text(framebuf, lx + 20, ly + 52, 15,
                          TR("1. 把 .nsc 脚本文件拷到下面这个目录（本工具已自动创建好）：",
                          "1. Copy .nsc script files into this folder (auto-created):"), C_TEXT);
                draw_text(framebuf, lx + 40, ly + 78, 15, SCRIPT_DIR, C_GOLD);
                draw_text(framebuf, lx + 20, ly + 110, 14,
                          TR("（也可直接放在 sdmc:/switch/gta5save/ 下，同样能扫到）",
                          "(You can also drop them in sdmc:/switch/gta5save/ directly)"), C_TEXT_MUTED);
                draw_text(framebuf, lx + 20, ly + 142, 15,
                          TR("2. 回到本页按 X 键（或「重新扫描」按钮）", "2. Press X here (or the Rescan button)"), C_TEXT);
                draw_text(framebuf, lx + 20, ly + 172, 15,
                          TR("3. ↑↓ 选中脚本 -> 按 A 安装（重启游戏生效）", "3. Up/Down to select -> A to install (reboot to take effect)"), C_TEXT);
                draw_text(framebuf, lx + 20, ly + 208, 14,
                          TR("注意：文件名即脚本名，安装时不要改名。", "Note: the file name is the script name; do not rename it."), C_GOLD);

                int bx = lx + 20, by = ly + 262, bw = 200, bh = 48;
                int pr = tap_is_hot(bx, by, bw, bh);
                gfx_card(framebuf, bx, by, bw, bh, pr ? C_CARD_SEL : C_HEADER, 6);
                draw_text_c(framebuf, bx + bw / 2, by + 13, 19, TR("重新扫描", "Rescan"), C_ACCENT);
                hot_add(bx, by, bw, bh, ACT_SCRIPT_SCAN, 0);

                int bx2 = bx + bw + 16;
                int pr2 = tap_is_hot(bx2, by, bw, bh);
                gfx_card(framebuf, bx2, by, bw, bh, pr2 ? C_CARD_SEL : C_CARD, 6);
                draw_text_c(framebuf, bx2 + bw / 2, by + 13, 19, TR("显示目录信息", "Dir Info"), pr2 ? 0xFFFFFFFFu : C_TEXT);
                hot_add(bx2, by, bw, bh, ACT_SCRIPT_DIR, 0);
            } else {
                /* ★ v5.6: 顶部一条 RPF 实况栏 —— 让「装没装」一眼可见 */
                int y = ly;
                if (g_script_rpf_n < 0) {
                    gfx_card(framebuf, lx, y, lw, 40, C_CARD, 6);
                    gfx_rect(framebuf, lx + 2, y + 2, 5, 36, C_RED);
                    draw_text(framebuf, lx + 18, y + 11, 15,
                              TR("[!] 读 update2.rpf 失败 -> 下面的「已装/未装」不可信",
                              "[!] Failed to read update2.rpf -> installed status below is unreliable"), C_RED);
                    y += 46;
                } else {
                    int ninst_r = 0;
                    for (int i = 0; i < g_script_count; i++)
                        if (g_script_items[i].installed) ninst_r++;
                    gfx_card(framebuf, lx, y, lw, 38, C_CARD, 6);
                    gfx_rect(framebuf, lx + 2, y + 2, 5, 34,
                             ninst_r > 0 ? C_GREEN : C_TEXT_MUTED);
                    char st[260];
                    snprintf(st, sizeof(st),
                             TR("update2.rpf 内的 script_rel.rpf 读到 %d 个 .nsc 条目   本列表 %d 个，已装 %d 个",
                                "script_rel.rpf in update2.rpf: %d .nsc entries   local list %d, installed %d"),
                             g_script_rpf_n, g_script_count, ninst_r);
                    draw_text(framebuf, lx + 18, y + 10, 14, st, C_TEXT);
                    y += 44;
                }
                /* ★ v5.7: 页内目录信息面板（按 Y 或点「目录信息」开关） */
                if (g_script_dirinfo_on) {
                    const char *p = g_script_dirinfo;
                    int lines = 1;
                    for (const char *q = p; *q; q++) if (*q == '\n') lines++;
                    int ph = 8 + lines * 24;
                    gfx_card(framebuf, lx, y, lw, ph,
                             g_script_dirinfo_ok ? C_CARD : C_MODAL_BG, 6);
                    gfx_rect(framebuf, lx + 2, y + 2, 5, ph - 4,
                             g_script_dirinfo_ok ? C_ACCENT : C_RED);
                    int ly3 = y + 6;
                    char tmp[512];
                    snprintf(tmp, sizeof(tmp), "%s", p);
                    char *save = tmp;
                    for (char *ln = tmp; ; ln++) {
                        if (*ln == '\n' || *ln == 0) {
                            char keep = *ln;
                            *ln = 0;
                            draw_text(framebuf, lx + 18, ly3, 15, save,
                                      g_script_dirinfo_ok ? C_TEXT : C_RED);
                            ly3 += 24;
                            if (keep == 0) break;
                            save = ln + 1;
                        }
                    }
                    y += ph + 6;
                }
                int ly2 = y;                        /* 列表起点下移 */
                int vis2 = (LY_BODY_H - (ly2 - ly)) / row_h;
                if (vis2 < 1) vis2 = 1;
                if (g_script_sel >= g_script_scr + vis2)
                    g_script_scr = g_script_sel - vis2 + 1;
                if (g_script_scr < 0) g_script_scr = 0;

                for (int r = 0; r < vis2; r++) {
                    int i = g_script_scr + r;
                    if (i >= g_script_count) break;
                    ScriptItem *it = &g_script_items[i];
                    int iy = ly2 + r * row_h;
                    int sel = (i == g_script_sel);
                    gfx_card(framebuf, lx, iy, lw, row_h - 4, sel ? C_ITEM_SEL : C_CARD, 6);
                    if (sel) gfx_accent_bar(framebuf, lx + 4, iy + 4, row_h - 12, C_ACCENT);

                    /* 状态点：绿=RPF 里已装，灰=未装，红=读取失败 */
                    uint32_t dotc = (g_script_rpf_n < 0) ? C_RED
                                  : (it->installed ? C_GREEN : C_TEXT_MUTED);
                    gfx_dot(framebuf, lx + 18, iy + 14, 5, dotc);
                    draw_text(framebuf, lx + 34, iy + 5, 17, it->name,
                              it->installed ? C_GREEN : C_TEXT);

                    char b[240];
                    /* ★ v6.2: 统一用 fmt_size —— 原来 `size / 1024` 整数除法会把
                     *   927 B 的 error_listener.nsc 显示成「0 KB」（用户实机反馈）。 */
                    char szs[24];
                    fmt_size(szs, sizeof(szs), it->size);
                    if (!script_file_usable(it->size))
                        snprintf(b, sizeof(b),
                                 TR("%u B   [!] 文件无效（0 字节或过大），不能安装%s",
                                    "%u B   [!] invalid file (zero or too large), cannot install%s"),
                                 it->size,
                                 it->from_alt ? TR("  [兼容目录]", "  [compat dir]") : "");
                    else if (g_script_rpf_n < 0)
                        snprintf(b, sizeof(b), TR("%s   [状态未知：RPF 读取失败]%s", "%s   [status unknown: RPF read failed]%s"),
                                 szs,
                                 it->from_alt ? TR("  [兼容目录]", "  [compat dir]") : "");
                    else
                        snprintf(b, sizeof(b), TR("%s   %s%s", "%s   %s%s"), szs,
                                 it->installed ? TR("RPF 里已有 -> 再装将替换", "in RPF -> reinstall replaces")
                                               : TR("RPF 里没有 -> 按 A 装", "not in RPF -> press A to install"),
                                 it->from_alt ? TR("   [来自 gta5save 兼容目录]", "   [from gta5save compat dir]") : "");
                    draw_text(framebuf, lx + 34, iy + 27, 12, b,
                              (!script_file_usable(it->size)) ? C_RED
                            : (it->installed ? C_TEXT_MUTED : C_TEXT));

                    /* 右侧：状态徽标 */
                    if (!script_file_usable(it->size)) {
                        draw_text_r(framebuf, lx + lw - 16, iy + 5, 14, TR("文件无效", "Invalid"), C_RED);
                    } else if (g_script_rpf_n < 0) {
                        draw_text_r(framebuf, lx + lw - 16, iy + 5, 14, TR("状态未知", "Unknown"), C_RED);
                    } else if (it->installed) {
                        draw_text_r(framebuf, lx + lw - 130, iy + 5, 15, TR("[已装]", "[Installed]"), C_GREEN);
                        if (sel)
                            draw_text_r(framebuf, lx + lw - 16, iy + 5, 13, TR("ZL 删除", "ZL Del"), C_RED);
                    } else {
                        draw_text_r(framebuf, lx + lw - 130, iy + 5, 15, TR("[未装]", "[Not installed]"), C_TEXT_MUTED);
                        if (sel)
                            draw_text_r(framebuf, lx + lw - 16, iy + 5, 14, TR("A 安装", "A Install"), C_ACCENT);
                    }
                    hot_add(lx, iy, lw, row_h - 4, ACT_SCRIPT_INSTALL, 1000 + i);
                }
                {
                    char pg[300];
                    /* ★ v5.7: 拆两行 —— 原来一行塞了路径+按键，末尾会被截断 */
                    snprintf(pg, sizeof(pg),
                             TR("共 %d 个脚本   第 %d 项   ↑↓选择   A安装/替换   ZL删除   X重扫   Y目录",
                                "%d scripts   #%d   Up/Down select   A install   ZL del   X rescan   Y dir"),
                             g_script_count, g_script_sel + 1);
                    draw_text(framebuf, lx, ly + LY_BODY_H - 30, 12, pg, C_TEXT_MUTED);
                    draw_text(framebuf, lx, ly + LY_BODY_H - 15, 11,
                              TR("底部「★模组管理」= 还原官方脚本 / 内置模组一键安装（含原作者署名）",
                                 "Bottom \"*Mod Mgr\" = restore stock scripts / one-tap builtin mods (authors credited)"),
                              C_ACCENT);
                }
            }
        }
        else {
            int lx = LY_PAD, ly = LY_BODY_Y, lw = FB_WIDTH - LY_PAD * 2;
            gfx_card(framebuf, lx, ly, lw, 150, C_CARD, 8);
            draw_text(framebuf, lx + 20, ly + 14, 18, "存档信息", C_ACCENT);
            {
                char b1[512], b2[512];
                snprintf(b1, sizeof(b1), "路径: %s", g_save_path);
                snprintf(b2, sizeof(b2), "大小: %zu KB   槽位: %d 个   备份: %d 个   来源: %s",
                         g_save_sz / 1024, g_num_slots, g_num_backups,
                         g_is_native_mount ? "系统内部直读" : "SD 卡目录");
                draw_text(framebuf, lx + 20, ly + 46, 15, b1, C_TEXT);
                draw_text(framebuf, lx + 20, ly + 74, 15, b2, C_TEXT_MUTED);
                draw_text(framebuf, lx + 20, ly + 102, 14,
                          "提示：画质文件与存档同目录，游戏读取的就是 save:/settings.xml", C_TEXT_MUTED);
            }

            /* 功能按钮 2 行 */
            {
                struct { const char *t; int act; } BTN[8] = {
                    { "内置存档库", ACT_LOAD_PRESET },
                    { "历史备份库", ACT_LOAD_BACKUP },
                    { "切换槽位",   ACT_PICK_SLOT },
                    { "画质预设",   ACT_GFX_PRESET },
                    { "导入画质",   ACT_GFX_IMPORT },
                    { TR("路径自检", "Self-test"), ACT_GFX_CHECK },
                    { "保存写回",   ACT_SAVE },
                    { "保存并退出", ACT_QUIT },
                };
                int bw = 276, bh = 50, bgx = LY_PAD, bgy = 330, bgp = 20;
                for (int b = 0; b < 8; b++) {
                    int bx = bgx + (b % 4) * (bw + bgp);
                    int by = bgy + (b / 4) * (bh + 14);
                    gfx_card(framebuf, bx, by, bw, bh, C_CARD, 6);
                    draw_text_c(framebuf, bx + bw / 2, by + 14, 19, BTN[b].t, C_TEXT);
                    hot_add(bx, by, bw, bh, BTN[b].act, 0);
                }
            }
        }

        /* =====================================================================
         * 7. ★ v28b: 底部 −/+ 大按钮区【整体删除】（用户反馈占空间）
         *    调数值改用：ZL/ZR 物理键（原有）+ 列表行内小 ±（渲染在选中行右侧）
         *    腾出的空间让内容区列表更舒展
         * ===================================================================== */

        /* =====================================================================
         * 8. 统一按钮栏（可点）-- ★ v5: 按页签切换按钮组, 不再出现"点了没反应"
         *    且左右两组各自留够宽度, 彻底消除原「保存写回 / 保存并退出」完全重叠
         * ===================================================================== */
        {
            int by = LY_BTN_Y, bh = LY_BTN_H;

            /* 按页签取不同按钮组 (每组 4 个)
             * ★ v5: 角色切换已由顶部可点标签栏承担, 这里换成更实用的 4 个动作 */
            struct BtnDef { const char *t; int act; };
            /* ★ v6.3: static 初始化器不能用 TR()（运行时值）⇒ 首帧填一次 */
            static struct BtnDef B_SAVE[4], B_GFX[4], B_TOOL[4],
                                 B_PERF[5], B_DLC[5],  B_SCRIPT[5];
            static int btn_inited = 0;
            if (!btn_inited) {
                const struct BtnDef zh_save[4] = {
                    { "拉满当前", ACT_MAX_ALL }, { "角色全满", ACT_MAX_CHAR },
                    { "切换步长", ACT_CYCLE_STEP }, { "切换槽位", ACT_PICK_SLOT },
                };
                const struct BtnDef en_save[4] = {
                    { "Max All", ACT_MAX_ALL }, { "Max Char", ACT_MAX_CHAR },
                    { "Step Size", ACT_CYCLE_STEP }, { "Slot", ACT_PICK_SLOT },
                };
                const struct BtnDef zh_gfx[4] = {
                    { "上一项", ACT_NAV_UP }, { "下一项", ACT_NAV_DOWN },
                    { "翻到顶", ACT_SCROLL_TOP }, { "翻到底", ACT_SCROLL_BOTTOM },
                };
                const struct BtnDef en_gfx[4] = {
                    { "Prev", ACT_NAV_UP }, { "Next", ACT_NAV_DOWN },
                    { "Top", ACT_SCROLL_TOP }, { "Bottom", ACT_SCROLL_BOTTOM },
                };
                const struct BtnDef zh_tool[4] = {
                    { "内置存档库", ACT_LOAD_PRESET }, { "历史备份库", ACT_LOAD_BACKUP },
                    { "画质预设", ACT_GFX_PRESET }, { "导入画质", ACT_GFX_IMPORT },
                };
                const struct BtnDef en_tool[4] = {
                    { "Save Presets", ACT_LOAD_PRESET }, { "Backups", ACT_LOAD_BACKUP },
                    { "GFX Preset", ACT_GFX_PRESET }, { "Import GFX", ACT_GFX_IMPORT },
                };
                const struct BtnDef zh_perf[5] = {
                    { "重新载入", ACT_GC_LOAD }, { "上一组", ACT_GC_GROUP_PREV },
                    { "下一组", ACT_GC_GROUP_NEXT }, { "恢复原版", ACT_GC_RESTORE },
                    { "回读校验", ACT_GC_VERIFY },
                };
                const struct BtnDef en_perf[5] = {
                    { "Reload", ACT_GC_LOAD }, { "Prev Group", ACT_GC_GROUP_PREV },
                    { "Next Group", ACT_GC_GROUP_NEXT }, { "Restore", ACT_GC_RESTORE },
                    { "Verify", ACT_GC_VERIFY },
                };
                const struct BtnDef zh_dlc[5] = {
                    { "重新扫描", ACT_DLC_SCAN_SRC }, { "注册/注销", ACT_DLC_TOGGLE },
                    { "模型目录", ACT_OPEN_MODELDIR }, { "★转格式", ACT_DLC_CONVERT },
                    { "切换列表", ACT_DLC_VIEW },
                };
                const struct BtnDef en_dlc[5] = {
                    { "Rescan", ACT_DLC_SCAN_SRC }, { "Register", ACT_DLC_TOGGLE },
                    { "Model Dir", ACT_OPEN_MODELDIR }, { "*Convert", ACT_DLC_CONVERT },
                    { "Switch View", ACT_DLC_VIEW },
                };
                const struct BtnDef zh_script[5] = {
                    { "重新扫描", ACT_SCRIPT_SCAN }, { "★安装", ACT_SCRIPT_INSTALL },
                    { "★删除", ACT_SCRIPT_UNINSTALL }, { "目录信息", ACT_SCRIPT_DIR },
                    { "★模组管理", ACT_SCRIPT_MGR },
                };
                const struct BtnDef en_script[5] = {
                    { "Rescan", ACT_SCRIPT_SCAN }, { "*Install", ACT_SCRIPT_INSTALL },
                    { "*Delete", ACT_SCRIPT_UNINSTALL }, { "Dir Info", ACT_SCRIPT_DIR },
                    { "*Mod Mgr", ACT_SCRIPT_MGR },
                };
                memcpy(B_SAVE,   g_lang_en ? en_save   : zh_save,   sizeof(B_SAVE));
                memcpy(B_GFX,    g_lang_en ? en_gfx    : zh_gfx,    sizeof(B_GFX));
                memcpy(B_TOOL,   g_lang_en ? en_tool   : zh_tool,   sizeof(B_TOOL));
                memcpy(B_PERF,   g_lang_en ? en_perf   : zh_perf,   sizeof(B_PERF));
                memcpy(B_DLC,    g_lang_en ? en_dlc    : zh_dlc,    sizeof(B_DLC));
                memcpy(B_SCRIPT, g_lang_en ? en_script : zh_script, sizeof(B_SCRIPT));
                btn_inited = 1;
            }
            const struct BtnDef *B = (g_tab == TAB_GFX) ? B_GFX
                                    : (g_tab == TAB_TOOL) ? B_TOOL
                                    : (g_tab == TAB_PERF) ? B_PERF
                                    : (g_tab == TAB_DLC)  ? B_DLC
                                    : (g_tab == TAB_SCRIPT) ? B_SCRIPT : B_SAVE;

            /* 左区按钮：按页签取组
             * ★ v6.2: 脚本页回到 5 个（模组管理合并了原来的两个还原入口）。 */
            int nbtn = (g_tab == TAB_PERF || g_tab == TAB_DLC) ? 5
                     : (g_tab == TAB_SCRIPT) ? 5 : 4;
            int right_x0 = FB_WIDTH - LY_PAD - 290;      /* 右区起点 */
            int avail = right_x0 - LY_PAD - 12;          /* 左区可用宽（留 12 间隔）*/
            int gp = 12;
            int bw = (avail - (nbtn - 1) * gp) / nbtn;
            if (bw > 200) bw = 200;
            int x0 = LY_PAD;
            for (int b = 0; b < nbtn; b++) {
                int bx = x0 + b * (bw + gp);
                int pressed = tap_is_hot(bx, by, bw, bh);   /* ★ v24 点击反馈 */
                /* ★ v28: 圆角按钮无描边，按下 = 主色块 */
                gfx_card(framebuf, bx, by, bw, bh, pressed ? C_CARD_SEL : C_CARD, 6);
                /* ★ v6: 脚本页 6 个按钮宽度变窄 ⇒ 字号自适应，长文案才不会溢出 */
                int fs = (bw >= 170) ? 19 : (bw >= 120 ? 16 : 14);
                draw_text_c(framebuf, bx + bw / 2, by + 13, fs, B[b].t,
                            pressed ? C_ACCENT : C_TEXT);
                hot_add(bx, by, bw, bh, B[b].act, 0);
            }
            /* 右区: 写入 + 退出 (两枚, 各 140 宽, 不重叠) */
            {
                int bw2 = 140, gp2 = 10;
                int bx1 = FB_WIDTH - LY_PAD - (bw2 * 2 + gp2);
                int bx2 = FB_WIDTH - LY_PAD - bw2;

                const char *t1 = (g_tab == TAB_PERF) ? TR("写入rpf", "Write RPF")
                               : (g_tab == TAB_DLC)  ? TR("导入选中", "Import")
                               : (g_tab == TAB_SCRIPT) ? TR("装/替换", "Install") : TR("保存写回", "Save");
                const char *t2 = (g_tab == TAB_PERF) ? TR("返回", "Back")
                               : (g_tab == TAB_DLC)  ? TR("返回", "Back")
                               : (g_tab == TAB_SCRIPT) ? TR("返回", "Back")    : TR("保存退出", "Save+Exit");
                int a1 = (g_tab == TAB_PERF) ? ACT_GC_WRITE
                       : (g_tab == TAB_DLC)  ? ACT_DLC_IMPORT
                       : (g_tab == TAB_SCRIPT) ? ACT_SCRIPT_INSTALL : ACT_SAVE;
                int a2 = (g_tab == TAB_PERF) ? ACT_BACK
                       : (g_tab == TAB_DLC)  ? ACT_BACK
                       : (g_tab == TAB_SCRIPT) ? ACT_BACK        : ACT_QUIT;

                int hot1 = ((g_tab == TAB_PERF && !g_gc_opened) ||
                            (g_tab == TAB_DLC && (g_dlc_view == 0 || !g_dlc_xml_loaded)) ||
                            (g_tab == TAB_SCRIPT && g_script_count == 0)) ? 1 : 0;
                int pr1 = tap_is_hot(bx1, by, bw2, bh);      /* ★ v24 点击反馈 */
                int pr2 = tap_is_hot(bx2, by, bw2, bh);
                if (pr1) hot1 = 1;
                gfx_card(framebuf, bx1, by, bw2, bh, hot1 ? C_HEADER : C_CARD, 6);
                draw_text_c(framebuf, bx1 + bw2 / 2, by + 13, 19, t1,
                            hot1 ? C_ACCENT : C_TEXT);
                hot_add(bx1, by, bw2, bh, a1, 0);

                gfx_card(framebuf, bx2, by, bw2, bh, pr2 ? C_CARD_SEL : C_HEADER, 6);
                draw_text_c(framebuf, bx2 + bw2 / 2, by + 13, 19, t2,
                            pr2 ? 0xFFFFFFFFu : C_ACCENT);
                hot_add(bx2, by, bw2, bh, a2, 0);
            }
        }

        /* 9. 操作结果提示栏 */
        {
            int ry = LY_HINT_Y;
            const char *msg = g_status_msg;
            uint32_t msgc = g_status_color;
            int dirty = (g_tab == TAB_GFX) ? g_gfx_dirty : g_dirty;

            if (g_tab == TAB_PERF) {
                /* 性能页用自己的状态消息 */
                msg = g_gc_msg;
                msgc = g_gc_msg_color;
                dirty = g_gc_dirty;
            } else if (g_tab == TAB_DLC) {
                msg = g_dlc_msg[0] ? g_dlc_msg : TR("点「载入并扫描」开始", "Press Load & Scan to start");
                /* ★ v5.7: 失败必须是红色 —— 原来用 C_ACCENT(蓝)，错误信息看起来像普通提示 */
                msgc = g_dlc_msg_ok ? C_GREEN : C_RED;
                dirty = g_dlc_dirty;
            } else if (g_tab == TAB_SCRIPT) {
                msg = g_dlc_msg[0] ? g_dlc_msg
                                   : TR("把 .nsc 放进下面的目录 -> 按 X 扫描 -> 选 A 安装",
                                        "Put .nsc files in the folder below -> X to scan -> A to install");
                msgc = g_dlc_msg_ok ? C_GREEN : C_RED;   /* ★ v5.7: 同上 */
                dirty = 0;
            }
            if (dirty) {
                int x = LY_PAD;
                x = draw_text(framebuf, x, ry, 16, TR("[有未保存修改] ", "[Unsaved] "), C_GOLD);
                draw_text(framebuf, x, ry, 16, msg, msgc);
            } else {
                draw_text(framebuf, LY_PAD, ry, 16, msg, msgc);
            }
            {
                const char *hint;
                if (g_tab == TAB_PERF) {
                    if (!g_gc_opened)
                        hint = TR("A 载入 update.rpf   B 返回DLC页   L/R 换页",
                         "A load update.rpf   B back   L/R switch tab");
                    else
                        hint = TR("↑/↓ 选参数   <-/-> 增减   ZL/ZR 大步(可长按)   Y 恢复原版   X 写入rpf   B 返回DLC页   L/R 换页",
                         "Up/Down param   Left/Right value   ZL/ZR big step (hold)   Y restore   X write RPF   B back   L/R tab");
                }
                else if (g_tab == TAB_GFX)
                    hint = TR("↑/↓ 选项   ZL/ZR 调值(可长按)   A 保存   Y 预设   B 返回DLC页",
                         "Up/Down item   ZL/ZR adjust (hold)   A save   Y preset   B back");
                else if (g_tab == TAB_DLC) {
                    if (!g_dlc_xml_loaded)
                        hint = TR("A 载入并扫描   L/R 换页", "A load & scan   L/R switch tab");
                    else if (g_dlc_view == 0)
                        hint = TR("↑/↓ 选包   A 注册/注销   ZR 转格式   ZL 删除   Y 待导入   B 返回主页   L/R 换页",
                         "Up/Down pack   A register   ZR convert   ZL delete   Y pending   B back   L/R tab");
                    else
                        hint = TR("↑/↓ 选待导入项   A 导入   Y 已装列表   B 返回已装   L/R 换页",
                         "Up/Down item   A import   Y installed   B back   L/R tab");
                }
                else if (g_tab == TAB_SCRIPT)
                    hint = TR("↑/↓ 选脚本   A 安装/替换   ZL 删除   X 重扫   Y 目录信息   B 返回DLC页   L/R 换页",
                         "Up/Down script   A install   ZL delete   X rescan   Y dir info   B back   L/R tab");
                else if (g_tab == TAB_TOOL)
                    hint = TR("点按钮执行功能   B 返回DLC页   L/R 换页", "Tap buttons to run   B back   L/R switch tab");
                else
                    hint = TR("A 保存   B 返回DLC页   X 拉满   Y 步长   ZL/ZR 调值(可长按)   - 角色全满",
                         "A save   B back   X max   Y step   ZL/ZR adjust (hold)   - max char");
                draw_text(framebuf, LY_PAD, ry + 24, 14, hint, C_TEXT_MUTED);
            }
        }

        /* 11. ★ v8 路径自检报告（全屏覆盖，方便上机排查）
         *   🚨 关键：不能就地改 g_gc_selftest（把 \n 换成 \0）！
         *   渲染每帧都跑，第一帧就把换行全毁掉 ⇒ 之后只画一行就 break。
         *   （踩过：报告只显示了「sdmc 挂载」一行）
         *   改为把每行拷进临时缓冲再画。 */
        if (g_gc_selftest_show && g_gc_selftest[0]) {
            gfx_card(framebuf, 50, 30, FB_WIDTH - 100, FB_HEIGHT - 60, C_MODAL_BG, 14);
            draw_text(framebuf, 76, 44, 22, TR("路径自检报告 (update.rpf)", "Path self-test (update.rpf)"), C_ACCENT);

            int y = 80;
            const char *p = g_gc_selftest;
            char line[256];
            while (*p && y < FB_HEIGHT - 62) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p) : strlen(p);
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, p, n);
                line[n] = '\0';

                uint32_t c = C_TEXT;
                if (strstr(line, "[有]"))          c = C_GREEN;
                else if (strstr(line, "[无]"))     c = C_TEXT_MUTED;
                else if (strstr(line, "[打不开]")) c = C_RED;
                else if (strstr(line, "B]"))       c = C_GREEN;
                else if (strstr(line, "rc="))      c = C_BLUE;
                else if (strstr(line, "★"))        c = C_RED;
                else if (strstr(line, "(OK)") || strstr(line, "(成功)")) c = C_GREEN;
                else if (line[0] == 0)             c = C_TEXT_MUTED;

                if (line[0]) draw_text(framebuf, 76, y, 13, line, c);
                y += 17;
                if (!nl) break;
                p = nl + 1;
            }
            draw_text(framebuf, 76, FB_HEIGHT - 52, 16, "按任意键 / 点屏幕关闭", C_BORDER_SEL);
        }

        if (g_gc_confirm) {
            gfx_card(framebuf, 240, 200, FB_WIDTH - 480, 320, C_MODAL_BG, 14);
            if (g_gc_confirm == 1) {
                draw_text_c(framebuf, FB_WIDTH / 2, 232, 24, "确认写入 update.rpf ?", C_ACCENT);
                draw_text_c(framebuf, FB_WIDTH / 2, 286, 17,
                            "将把改动写进 889 MB 的 update.rpf", C_TEXT);
                char l2[160];
                snprintf(l2, sizeof(l2), "switch 段 %d 项, 已改 %d 项",
                         g_gc_n_switch, g_gc_edited_n);
                draw_text_c(framebuf, FB_WIDTH / 2, 316, 16, l2, C_TEXT_MUTED);
                draw_text_c(framebuf, FB_WIDTH / 2, 350, 16,
                            "采用纯字节原位替换：文件大小不变、其它条目零改动", C_GREEN);
                draw_text_c(framebuf, FB_WIDTH / 2, 380, 16,
                            "写入后会立即校验，失败自动回滚", C_GREEN);
                draw_text_c(framebuf, FB_WIDTH / 2, 424, 18,
                            "A 确认写入      B 取消", C_BORDER_SEL);
            } else {
                draw_text_c(framebuf, FB_WIDTH / 2, 232, 24, "恢复原版参数 ?", C_ACCENT);
                draw_text_c(framebuf, FB_WIDTH / 2, 286, 17,
                            "把全部 52 项参数恢复成移植作者的原始值", C_TEXT);
                draw_text_c(framebuf, FB_WIDTH / 2, 316, 16,
                            "（无论当前是哪一档，都能正确还原；之后需点「写入rpf」落盘）",
                            C_TEXT_MUTED);
                draw_text_c(framebuf, FB_WIDTH / 2, 424, 18,
                            "A 确认恢复      B 取消", C_BORDER_SEL);
            }
        }

        /* ---------------- 模态弹窗 1：内置通关进度库 (ZL) ---------------- */
        if (g_modal_mode == 1) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, TR("[内置] 游戏剧情通关进度存档库 (A 键载入内存，再按 A 写入游戏)",
                                     "[Builtin] Story progress saves (A to load into memory, A again to write)"), C_ACCENT);
            draw_text(framebuf, 120, 110, 16, "上下键 选择剧情节点 | A键 载入 | B键 取消返回", C_TEXT_MUTED);

            int item_y = 145;
            for (int p = 0; p < (int)NUM_PRESETS; p++) {
                int is_p_sel = (p == g_modal_sel);
                gfx_card(framebuf, 120, item_y, FB_WIDTH - 240, 75, is_p_sel ? C_CARD_SEL : C_CARD, 8);
                if (is_p_sel) gfx_accent_bar(framebuf, 124, item_y + 6, 63, C_ACCENT);

                draw_text(framebuf, 140, item_y + 14, 22, PRESET_SAVES[p].name, is_p_sel ? C_ACCENT : C_TEXT);
                draw_text(framebuf, 140, item_y + 44, 16, PRESET_SAVES[p].desc, C_TEXT_MUTED);

                if (is_p_sel) {
                    draw_text(framebuf, FB_WIDTH - 250, item_y + 26, 18, "> 按 A 键载入", C_ACCENT);
                }
                item_y += 85;
            }
        }

        /* ---------------- 模态弹窗 2：历史备份库 (/switch/gta5save/) (ZR) ---------------- */
        if (g_modal_mode == 2) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, TR("[备份库] 历史备份存档管理器 (直接读取备份存档的剧情名与时间戳)",
                                     "[Backups] Backup save manager (reads story name & timestamp)"), C_BLUE);
            draw_text(framebuf, 120, 110, 16, "上下键 选择历史快照 | A键 恢复此备份进内存 | B键 取消返回", C_TEXT_MUTED);

            if (g_num_backups == 0) {
                draw_text(framebuf, 140, 200, 22, "暂无历史备份文件! 每次在主界面按 A 键保存时系统会自动创建安全备份。", C_TEXT_MUTED);
            } else {
                int item_y = 145;
                int start_i = 0;
                if (g_modal_sel >= 5) start_i = g_modal_sel - 4;
                int end_i = start_i + 5;
                if (end_i > g_num_backups) end_i = g_num_backups;

                for (int b = start_i; b < end_i; b++) {
                    int is_b_sel = (b == g_modal_sel);
                    gfx_card(framebuf, 120, item_y, FB_WIDTH - 240, 75, is_b_sel ? C_CARD_SEL : C_CARD, 8);
                    if (is_b_sel) gfx_accent_bar(framebuf, 124, item_y + 6, 63, C_BLUE);

                    char title_line[256];
                    const char *b_title = g_backups[b].title[0] ? g_backups[b].title : "未知剧情标题";
                    snprintf(title_line, sizeof(title_line), "快照: %s  [%s]", g_backups[b].dir_name, b_title);
                    draw_text(framebuf, 140, item_y + 14, 20, title_line, is_b_sel ? C_ACCENT : C_TEXT);

                    char sub_line[256];
                    snprintf(sub_line, sizeof(sub_line), "备份原因: %s  (大小: %zu KB)", g_backups[b].note, g_backups[b].size / 1024);
                    draw_text(framebuf, 140, item_y + 44, 15, sub_line, C_TEXT_MUTED);

                    if (is_b_sel) {
                        draw_text(framebuf, FB_WIDTH - 250, item_y + 26, 18, "> 按 A 恢复", C_BLUE);
                    }
                    hot_add(120, item_y, FB_WIDTH - 240, 85, ACT_PICK_ITEM, b);   /* ★ v4 触屏点选备份 */
                    item_y += 85;
                }

                char page_info[64];
                snprintf(page_info, sizeof(page_info), "共 %d 个备份，当前第 %d 项", g_num_backups, g_modal_sel + 1);
                draw_text(framebuf, 120, FB_HEIGHT - 80, 16, page_info, C_TEXT_MUTED);
            }
        }

        /* ---------------- 模态弹窗 3：选择当前目录存档槽位 (B) ---------------- */
        if (g_modal_mode == 3) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, TR("[槽位选择] 当前游戏目录中的所有存档 (已显示剧情任务与进度时间)",
                                     "[Slots] All saves in the game folder (story mission & time shown)"), C_GREEN);
            draw_text(framebuf, 120, 110, 16, "默认已选中最近修改的存档 | 上下键 挑选槽位 | A键 确认切换 | B键 返回", C_TEXT_MUTED);

            if (g_num_slots == 0) {
                draw_text(framebuf, 140, 200, 22, "当前目录下未检测到任何 SGTA5* 存档文件!", C_RED);
            } else {
                int item_y = 145;
                int start_i = 0;
                if (g_modal_sel >= 5) start_i = g_modal_sel - 4;
                int end_i = start_i + 5;
                if (end_i > g_num_slots) end_i = g_num_slots;

                for (int s = start_i; s < end_i; s++) {
                    int is_s_sel = (s == g_modal_sel);
                    int is_active = (s == g_active_slot_idx);

                    gfx_card(framebuf, 120, item_y, FB_WIDTH - 240, 75, is_s_sel ? C_CARD_SEL : C_CARD, 8);
                    if (is_s_sel || is_active)
                        gfx_accent_bar(framebuf, 124, item_y + 6, 63, is_active ? C_GREEN : C_ACCENT);

                    char line1[256];
                    const char *slot_title = g_slots[s].title[0] ? g_slots[s].title : "未知剧情标题";
                    snprintf(line1, sizeof(line1), "%s:  %s  %s", g_slots[s].filename, slot_title, is_active ? "[当前正在编辑]" : "");
                    draw_text(framebuf, 140, item_y + 14, 20, line1, is_active ? C_GREEN : (is_s_sel ? C_ACCENT : C_TEXT));

                    char line2[256];
                    snprintf(line2, sizeof(line2), "大小: %zu KB", g_slots[s].size / 1024);
                    draw_text(framebuf, 140, item_y + 44, 15, line2, C_TEXT_MUTED);

                    if (is_s_sel) {
                        draw_text(framebuf, FB_WIDTH - 250, item_y + 26, 18, is_active ? "已是当前" : "> 按 A 切换", C_GREEN);
                    }
                    hot_add(120, item_y, FB_WIDTH - 240, 75, ACT_PICK_ITEM, s);   /* ★ v4 触屏点选槽位 */
                    item_y += 85;
                }

                char page_info[64];
                snprintf(page_info, sizeof(page_info), "共 %d 个存档槽位 (第 1 项为最近修改)", g_num_slots);
                draw_text(framebuf, 120, FB_HEIGHT - 80, 16, page_info, C_TEXT_MUTED);
            }
        }

        /* (v5 已删除原「覆盖式画质页」死代码块 if (0 && g_gfx_show) {...} 共 56 行) */

        /* ---------------- 模态弹窗 4：内置画质预设 (Y 键) ---------------- */
        if (g_modal_mode == 4) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, TR("[画质设置] 导入外部配置 或 一键应用内置预设",
                                     "[Graphics] Import external config or apply a builtin preset"), C_ACCENT);
            draw_text(framebuf, 120, 110, 16, "上下键 选择 | A键 应用并保存 | B键 取消", C_TEXT_MUTED);

            /* ★ 预设变多（6 套 + 导入 = 7 项），需要滚动：
             *   弹窗可用高度 = FB_HEIGHT-100-145-40 ≈ 435；行高 62 ⇒ 可见 7 行
             *   g_modal_scr = 滚动偏移，【专用于本弹窗与模态5】（模态3 不用它） */
            int total = (int)NUM_GFX_PRESETS + 1;
            int vis = 7;
            int row_h = 62;
            if (g_modal_sel < g_modal_scr) g_modal_scr = g_modal_sel;
            if (g_modal_sel >= g_modal_scr + vis) g_modal_scr = g_modal_sel - vis + 1;
            if (g_modal_scr > total - vis) g_modal_scr = total - vis;
            if (g_modal_scr < 0) g_modal_scr = 0;

            int item_y = 145;
            for (int r = 0; r < vis; r++) {
                int p = g_modal_scr + r;
                if (p >= total) break;
                int is_p_sel = (p == g_modal_sel);
                const char *pname;
                const char *pdesc;
                if (p == 0) {
                    pname = "从 /switch/gta5save/ 导入画质文件";
                    pdesc = "扫描该目录所有 xml 画质文件供选择";
                } else {
                    pname = GFX_PRESETS[p - 1].name;
                    pdesc = GFX_PRESETS[p - 1].desc;
                }
                gfx_card(framebuf, 120, item_y, FB_WIDTH - 240, row_h - 6,
                         is_p_sel ? C_CARD_SEL : C_CARD, 8);
                if (is_p_sel) gfx_accent_bar(framebuf, 124, item_y + 5, row_h - 16, C_ACCENT);

                draw_text(framebuf, 140, item_y + 8, 21, pname, is_p_sel ? C_ACCENT : C_TEXT);
                draw_text(framebuf, 140, item_y + 33, 15, pdesc, C_TEXT_MUTED);

                if (is_p_sel) {
                    draw_text(framebuf, FB_WIDTH - 250, item_y + 18, 18, "> 按 A 应用", C_BORDER_SEL);
                }
                item_y += row_h;
            }
            if (total > vis) {
                char pg[64];
                snprintf(pg, sizeof(pg), "第 %d/%d 项 (上下滚动)", g_modal_sel + 1, total);
                draw_text(framebuf, 120, item_y + 2, 14, pg, C_TEXT_MUTED);
            }
        }

        /* ---------------- 模态弹窗 5：选择 /switch/gta5save/ 下的画质文件 ---------------- */
        if (g_modal_mode == 5) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, "[导入画质文件] 选择要导入的文件", C_ACCENT);
            draw_text(framebuf, 120, 110, 16, "上下键 选择 | A键 导入 | B键 返回", C_TEXT_MUTED);

            if (g_gfx_switch_count == 0) {
                draw_text(framebuf, 140, 200, 20, "未在 /switch/gta5save/ 找到 xml 画质文件!", C_RED);
            } else {
                /* ★ 文件可能超过 6 个，必须滚动（行高 66，可见 6 行） */
                int total5 = g_gfx_switch_count;
                int vis5   = 6;
                int row_h5 = 66;
                if (g_modal_sel < g_modal_scr) g_modal_scr = g_modal_sel;
                if (g_modal_sel >= g_modal_scr + vis5) g_modal_scr = g_modal_sel - vis5 + 1;
                if (g_modal_scr > total5 - vis5) g_modal_scr = total5 - vis5;
                if (g_modal_scr < 0) g_modal_scr = 0;

                int item_y = 145;
                for (int r = 0; r < vis5; r++) {
                    int p = g_modal_scr + r;
                    if (p >= total5) break;
                    int is_p_sel = (p == g_modal_sel);
                    gfx_card(framebuf, 120, item_y, FB_WIDTH - 240, 56, is_p_sel ? C_CARD_SEL : C_CARD, 8);
                    if (is_p_sel) gfx_accent_bar(framebuf, 124, item_y + 5, 46, C_ACCENT);
                    draw_text(framebuf, 140, item_y + 15, 22, g_gfx_switch_files[p], is_p_sel ? C_ACCENT : C_TEXT);
                    if (is_p_sel) {
                        draw_text(framebuf, FB_WIDTH - 250, item_y + 17, 18, "> 按 A 导入", C_BORDER_SEL);
                    }
                    item_y += row_h5;
                }
                if (total5 > vis5) {
                    char pg5[64];
                    snprintf(pg5, sizeof(pg5), "第 %d/%d 个文件 (上下滚动)", g_modal_sel + 1, total5);
                    draw_text(framebuf, 120, item_y + 2, 14, pg5, C_TEXT_MUTED);
                }
            }
        }

        /* ---------------- 模态弹窗 6：画质路径自检结果 (画质页 X 键) ---------------- */
        if (g_modal_mode == 6) {
            gfx_card(framebuf, 80, 50, FB_WIDTH - 160, FB_HEIGHT - 100, C_MODAL_BG, 14);
            
            draw_text(framebuf, 120, 75, 24, "[画质路径自检] 各候选位置读写状态", C_ACCENT);
            draw_text(framebuf, 120, 110, 16, "可读写=游戏可能读取 | 存在只读/不存在=不会读到 | B键 返回", C_TEXT_MUTED);

            int iy = 145;
            for (int p = 0; p < g_check_paths_count && p < 14; p++) {
                int is_sel = (p == g_modal_sel);
                gfx_card(framebuf, 120, iy, FB_WIDTH - 240, 44, is_sel ? C_CARD_SEL : C_CARD, 6);
                if (is_sel) gfx_accent_bar(framebuf, 124, iy + 5, 34, C_ACCENT);
                draw_text(framebuf, 140, iy + 11, 17,
                          g_check_paths[p][0] ? g_check_paths[p] : "(空)",
                          strstr(g_check_paths[p], "可读写") ? C_GREEN : C_TEXT_MUTED);
                iy += 52;
            }
        }

        /* ---------------- 模态弹窗 7：脚本管理（还原官方 + 内置模组） ----------------
         * ★ v6.2：取代原来两个超长按钮「还原官方 error_listener / 还原官方 controller」
         *   分区展示：上=还原官方（3 项），下=内置模组（4 项，署名原作者）。
         *   每项按 A 都进弹窗 8 做二次确认。 */
        if (g_modal_mode == MODE_SCRIPT_MGR || g_modal_mode == MODE_SCRIPT_CONFIRM) {
            int ns = gc_script_stock_count();
            int nm = gc_script_builtin_count();
            int total = ns + nm;

            gfx_card(framebuf, 56, 36, FB_WIDTH - 112, FB_HEIGHT - 72, C_MODAL_BG, 14);

            draw_text(framebuf, 88, 56, 23,
                      TR("[脚本管理] 还原官方脚本 / 内置模组一键安装",
                         "[Script Manager] Restore stock / Install builtin mods"),
                      C_ACCENT);
            draw_text(framebuf, 88, 88, 14,
                      TR("↑↓ 选择    A 执行（会二次确认）    B 返回",
                         "Up/Down select    A apply (confirm twice)    B back"),
                      C_TEXT_MUTED);

            int card_x = 88, card_w = FB_WIDTH - 176;
            int iy = 116;

            /* --- 区一：还原官方 --- */
            draw_text(framebuf, card_x, iy, 15,
                      TR("[还原官方脚本] 用本工具内置的真·原版覆盖回去",
                         "[Restore Stock] Overwrite with the bundled true originals"),
                      C_BLUE);
            iy += 22;
            for (int i = 0; i < ns; i++) {
                int sel = (g_modal_sel == i);
                gfx_card(framebuf, card_x, iy, card_w, 48, sel ? C_CARD_SEL : C_CARD, 6);
                if (sel) gfx_accent_bar(framebuf, card_x + 4, iy + 4, 40, C_ACCENT);
                const char *sn = gc_script_stock_name(i);
                draw_text(framebuf, card_x + 18, iy + 6, 17, sn ? sn : "(空)",
                          sel ? C_ACCENT : C_TEXT);
                /* ★ v6.2: 修改器其实只借 achievement_controller 一个壳
                 *   （ragemenu/achievement 的字节码里都不引用 shop_controller）
                 *   ⇒ shop_controller 只有被旧版「三文件」装法改过的卡才需要用 */
                draw_text(framebuf, card_x + 18, iy + 27, 11,
                          (sn && strcmp(sn, "shop_controller.nsc") == 0)
                              ? TR("官方原版（一般无需还原；仅被旧版「三文件」装法改过才用）",
                                   "Stock original (usually no need; only for cards touched by the old 3-file install)")
                              : TR("官方原版（RSC7 资源条目，非 mod 的裸条目）",
                                   "Stock original (RSC7 resource entry, not a raw mod entry)"),
                          C_TEXT_MUTED);
                if (sel) draw_text_r(framebuf, card_x + card_w - 16, iy + 6, 14,
                                     TR("A 还原", "A Restore"), C_ACCENT);
                hot_add(card_x, iy, card_w, 48, ACT_SCRIPT_MGR, i);   /* param = 绝对行号 */
                iy += 52;
            }

            /* --- 区二：内置模组 --- */
            iy += 6;
            draw_text(framebuf, card_x, iy, 15,
                      TR("[内置模组] 已打进本工具，不用往卡里拷文件（原作者署名见下）",
                         "[Builtin Mods] Bundled in this tool, no file copy needed (authors credited below)"),
                      C_GREEN);
            iy += 22;
            for (int k = 0; k < nm; k++) {
                int idx = ns + k;
                int sel = (g_modal_sel == idx);
                const GcBuiltinMod *m = gc_script_builtin(k);
                gfx_card(framebuf, card_x, iy, card_w, 70, sel ? C_CARD_SEL : C_CARD, 6);
                if (sel) gfx_accent_bar(framebuf, card_x + 4, iy + 4, 62, C_ACCENT);
                draw_text(framebuf, card_x + 18, iy + 6, 17,
                          MOD_TITLE(m) ? MOD_TITLE(m) : TR("(空)", "(empty)"), sel ? C_ACCENT : C_TEXT);
                {
                    char ab[200];
                    snprintf(ab, sizeof(ab), TR("作者 %s   %d 个文件", "By %s   %d files"),
                             (m && m->author) ? m->author : TR("未知", "?"), m ? m->n_files : 0);
                    draw_text(framebuf, card_x + 18, iy + 28, 12, ab, C_TEXT_MUTED);
                }
                if (MOD_NOTE(m))
                    draw_text(framebuf, card_x + 18, iy + 48, 11, MOD_NOTE(m), C_TEXT_MUTED);
                if (sel) draw_text_r(framebuf, card_x + card_w - 16, iy + 6, 14,
                                     TR("A 安装", "A Install"), C_ACCENT);
                hot_add(card_x, iy, card_w, 70, ACT_SCRIPT_MGR, idx); /* param = 绝对行号 */
                iy += 74;
            }

            draw_text(framebuf, card_x, FB_HEIGHT - 76, 12,
                      TR("装模组会覆盖同名的官方脚本；想恢复点上面「还原官方脚本」。装完重启游戏生效。",
                         "Installing a mod overwrites the stock script with the same name; use Restore Stock above to recover. Reboot the game after install."),
                      C_TEXT_MUTED);
            (void)total;
        }

        /* ---------------- 模态弹窗 8：操作二次确认 ----------------
         * ★ v6.2：所有会覆盖 RPF 脚本的操作都要过这一关（用户要求）。 */
        if (g_modal_mode == MODE_SCRIPT_CONFIRM) {
            /* 上一步的脚本管理列表仍在下面渲染（条件里带了 MODE_SCRIPT_CONFIRM）
             * ⇒ 确认框直接叠在列表中央，不用压暗整屏（RGB_A 无 alpha 混合）。 */
            gfx_card(framebuf, FB_WIDTH / 2 - 360, 170, 720, 300, C_MODAL_BG, 14);
            gfx_rect_outline(framebuf, FB_WIDTH / 2 - 360, 170, 720, 300, 3, C_BORDER_SEL);

            if (g_mod_pending_kind == 0) {
                const char *sn = gc_script_stock_name(g_mod_pending_idx);
                draw_text_c(framebuf, FB_WIDTH / 2, 200, 22,
                            TR("确认还原官方脚本 ?", "Restore stock script?"), C_ACCENT);
                {
                    char l1[200];
                    snprintf(l1, sizeof(l1), "%s", sn ? sn : TR("(未知)", "(unknown)"));
                    draw_text_c(framebuf, FB_WIDTH / 2, 250, 19, l1, C_TEXT);
                }
                draw_text_c(framebuf, FB_WIDTH / 2, 290, 15,
                            TR("会用本工具内置的官方原版覆盖当前这一条",
                               "This will overwrite the entry with the bundled stock original"),
                            C_TEXT);
                draw_text_c(framebuf, FB_WIDTH / 2, 318, 14,
                            TR("（mod 借壳改过的就是这一条；发到卡上重启游戏后才生效）",
                               "(The entry mods hijack; takes effect after rebooting the game)"),
                            C_TEXT_MUTED);
            } else {
                const GcBuiltinMod *m = gc_script_builtin(g_mod_pending_idx);
                draw_text_c(framebuf, FB_WIDTH / 2, 200, 22,
                            TR("确认安装内置模组 ?", "Install builtin mod?"), C_GREEN);
                draw_text_c(framebuf, FB_WIDTH / 2, 246, 19,
                            MOD_TITLE(m) ? MOD_TITLE(m) : TR("(未知)", "(unknown)"), C_TEXT);
                {
                    char l2[300];
                    snprintf(l2, sizeof(l2), TR("作者 %s", "By %s"),
                             (m && m->author) ? m->author : TR("未知", "?"));
                    draw_text_c(framebuf, FB_WIDTH / 2, 278, 14, l2, C_TEXT_MUTED);
                    if (MOD_NOTE(m))
                        draw_text_c(framebuf, FB_WIDTH / 2, 300, 12, MOD_NOTE(m), C_TEXT_MUTED);
                }
                draw_text_c(framebuf, FB_WIDTH / 2, 324, 15,
                            TR("将写入下列 RPF 条目（同名会被覆盖）：",
                               "Will write these RPF entries (same-name overwritten):"),
                            C_TEXT);
                if (m) {
                    char fl[260]; int w = 0;
                    for (int k = 0; k < m->n_files; k++) {
                        const char *en = gc_script_builtin_entry(g_mod_pending_idx, k);
                        w += snprintf(fl + w, sizeof(fl) - w, "%s%s",
                                      k ? " / " : "", en ? en : "?");
                        if (w >= (int)sizeof(fl) - 8) break;
                    }
                    draw_text_c(framebuf, FB_WIDTH / 2, 350, 12, fl, C_TEXT_MUTED);
                }
            }

            /* 两个按钮（键盘 + 触屏都可用） */
            {
                int by2 = 390, bw2 = 190, bh2 = 46, gap = 30;
                int bx1 = FB_WIDTH / 2 - bw2 - gap / 2;
                int bx2 = FB_WIDTH / 2 + gap / 2;
                int okp = tap_is_hot(bx1, by2, bw2, bh2);
                int cnp = tap_is_hot(bx2, by2, bw2, bh2);
                gfx_card(framebuf, bx1, by2, bw2, bh2, okp ? C_ACCENT : C_CARD_SEL, 6);
                draw_text_c(framebuf, bx1 + bw2 / 2, by2 + 12, 18, TR("A 确认", "A Confirm"),
                            okp ? 0xFFFFFFFFu : C_ACCENT);
                hot_add(bx1, by2, bw2, bh2, ACT_SCRIPT_CONFIRM_OK, 0);
                gfx_card(framebuf, bx2, by2, bw2, bh2, cnp ? C_CARD_SEL : C_HEADER, 6);
                draw_text_c(framebuf, bx2 + bw2 / 2, by2 + 12, 18, TR("B 取消", "B Cancel"),
                            cnp ? C_ACCENT : C_TEXT);
                hot_add(bx2, by2, bw2, bh2, ACT_BACK, 0);
            }
        }

        /* =====================================================================
         * ★★ v24 触屏点击反馈（用户反馈「选中按钮没有高亮显示」）
         *   点中热区后，在【刚被点的那个矩形】上叠一层醒目高亮：
         *   前 40% 时间 = 4px 纯白外框 + 左侧白粗条（最强调，绝不会看漏）
         *   之后      = 2px 金框，420ms 后自动消失。
         *   画在所有页面内容之上、toast 之下。
         * ===================================================================== */
        if (g_tap_fx_on) {
            u64 el = (svcGetSystemTick() - g_tap_fx_tick) * 1000 / 19200000ULL;
            if (el >= TAP_FX_MS) {
                g_tap_fx_on = 0;
            } else {
                int early = (el < TAP_FX_MS * 2 / 5);
                uint32_t tc = early ? 0xFFFFFFFFu : C_BORDER_SEL;
                int th = early ? 4 : 2;
                int fx = g_tap_fx[0] - 2, fy = g_tap_fx[1] - 2;
                int fw = g_tap_fx[2] + 4, fh = g_tap_fx[3] + 4;
                gfx_rect_outline(framebuf, fx, fy, fw, fh, th, tc);
                if (early) gfx_rect(framebuf, fx, fy, 7, fh, tc);   /* 左侧粗条 */
            }
        }

        /* 屏幕中央提示框 (保存成功等, 绘制在最上层) */
        draw_toast(framebuf);

        framebufferEnd(&fb);
    }

    framebufferClose(&fb);
    if (g_save_buf) free(g_save_buf);
    if (g_face) FT_Done_Face(g_face);
    if (g_ft) FT_Done_FreeType(g_ft);
    if (g_is_native_mount) fsdevUnmountDevice("save");
    romfsExit();
    plExit();
    return 0;
}
