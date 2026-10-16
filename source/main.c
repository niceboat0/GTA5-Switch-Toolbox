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
#include <errno.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <zlib.h>          /* ★ v6: 性能调参页需要 deflate */

#include "save.h"
#include "save_backup.h"
#include "gc_rpf.h"
#include "gc_dlc.h"
#include "gc_vehicle.h"
#include "gc_script.h"   /* ★ v5.0: 脚本 mod（.nsc）安装 */
#include "gc_presets.h"
#include "ui.h"
#include "ui_navigation.h"
#include "lang.h"        /* ★ v6.3: 双语 TR() 宏 */

/* 屏幕分辨率 */
#define FB_WIDTH  1280
#define FB_HEIGHT 720

/* GTA V Switch 版 Title ID */
#define GTAV_TITLE_ID 0x0100B00B51230000ULL

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
#define C_BG UI_BG
#define C_HEADER UI_RGB(19,28,37)
#define C_CARD UI_SURFACE
#define C_CARD_SEL UI_RGB(35,60,61)
#define C_BORDER      RGB_A(0x1C, 0x24, 0x30)   /* #1C2430 = 卡片色（视觉无边框） */
#define C_BORDER_SEL UI_ACCENT
#define C_ACCENT UI_ACCENT
#define C_TEXT UI_TEXT
#define C_TEXT_MUTED UI_MUTED
#define C_GREEN UI_ACCENT
#define C_GOLD UI_WARN
#define C_RED UI_ERROR
#define C_BLUE UI_ACCENT
#define C_ITEM_SEL    RGB_A(0x2E, 0x44, 0x62)   /* = C_CARD_SEL */
#define C_MODAL_BG UI_RGB(19,28,37)


static FT_Library g_ft;
static FT_Face    g_face = NULL;
static PadState   g_pad;

/* ★ v6.3: 双语支持 -- 0=中文（默认） 1=English
 * detect_language() 在 main() 开头按系统语言置位 */
int g_lang_en = 0;
void detect_language(void) {
    /* 🚨 set 服务必须先 setInitialize()，否则所有 set* 调用静默失败
     *   （v6.3 首版漏了这句 ⇒ 英文系统也不生效，实机反馈） */
    if (R_FAILED(setInitialize())) return;

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
    setExit();
}

/* ========================================================================= */
/* ★ v4 新增: 触屏支持 + 命中测试框架                                         */
/* ========================================================================= */
/* 设计:
 *   渲染时把每个「可点区域」登记进 g_hot[] (hotzone);
 *   下一帧处理输入时，用触摸点做命中测试，转换成与按键等价的「动作」。
 *   这样按键与触屏共用同一套动作处理逻辑，不会出现两套行为。 */

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

/* Gestures are handled by the platform-independent UI after HID sampling. */
static void touch_poll(void) {
    memset(&g_touch,0,sizeof(g_touch));hidGetTouchScreenStates(&g_touch,1);
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
/* ★ v29: 全局卡片圆角开关 -- 0 = 直角卡片（当前风格，更利落）
 *   想恢复圆角只需改成 6/8/10，所有调用点自动跟随 */
#define CARD_R 0

/* ★ v28: 卡片（纯色块 + 可选圆角，无描边）
 *   r = 圆角半径；★ v29 起由 CARD_R 统一覆盖，调用点传值不再生效 */
/* ★ v28: 左侧主色条（选中标记，Goldleaf 风格 3px） */
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
    if (!s || !g_face) return 0;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int width=0;
    while (*s) {
        uint32_t cp=utf8_next(&s);
        if (!cp || cp=='\n') break;
        if (FT_Load_Char(g_face,cp,FT_LOAD_DEFAULT)==0) width+=g_face->glyph->advance.x>>6;
    }
    return width;
}

/* 居中绘制文字: 以 cx 为水平中心, 返回实际起点 x */
static int draw_text_c(uint32_t *fb, int cx, int y, int size, const char *str, uint32_t color) {
    int w = text_width(str, size);
    int x = cx - w / 2;
    draw_text(fb, x, y, size, str, color);
    return x;
}

/* 右对齐绘制: 以 rx 为右边界 */
/* ========================================================================= */
/* ★ v5: 自绘符号 -- 系统共享字体缺 U+2212(−) / U+25C0(◀) / U+25B6(▶) 等字形,
 *   直接用矩形拼, 完全不依赖字体, 保证任何环境下都显示正常。
 *   cx,cy = 符号中心; sz = 符号半径/半宽; th = 笔画粗细 */
/* 三角形 (dir: 0=上 1=下 2=左 3=右) */
/* 加号 / 减号 (减号也自绘, 避开字体缺字形) */
/* 实心圆点 (状态指示) */
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
#define NUM_ATTRS 9
static const int CHAR_SP_INDEX[3] = {1, 0, 2};
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
 * GTA V stats_controller.ysc 每 2 秒根据行为统计重算角色技能值并覆盖存档。
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
 * 注意: 游泳/骑车/飞行/直升机/水下 的统计是毫秒(func_66/65 转分钟/秒)，
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
static const PresetSave PRESET_SAVES_EN[] = {
    { "4% prologue, just arrived in LS",   "Franklin early stage, first repo job done", "romfs:/preset_saves/4%/SGTA50000" },
    { "20% Trevor arrives",        "M & T reunited, most of the map open", "romfs:/preset_saves/20%/SGTA50000" },
    { "31.6% pre-Paleto heist", "trio assembled, gear & prep complete", "romfs:/preset_saves/31.6%/SGTA50000" },
    { "61.1% the Big Score eve",   "Union Depository eve, skills & vehicles ready", "romfs:/preset_saves/61.1%/SGTA50000" },
    { "100% perfect completion",     "all gold + strangers 100% save", "romfs:/preset_saves/100%/SGTA50001" },
};
#define PRESET_SAVE(i) (g_lang_en ? PRESET_SAVES_EN[(i)] : PRESET_SAVES[(i)])
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
static const GfxPreset GFX_PRESETS_EN[] = {
    { "*VSync off (fps)",      "only VSync off; removes frame quantization at low fps", "romfs:/preset_gfx/★关VSync（只改这一项）/settings.xml" },
    { "**VSync off + all min",    "VSync off + everything at minimum. ultimate low", "romfs:/preset_gfx/★★关VSync+全压零（终极低画质）/settings.xml" },
    { "Low (stock clocks)",        "min effects, smooth at stock clocks", "romfs:/preset_gfx/低画质（默频使用）/settings.xml" },
    { "Low + shadows",         "low effects with shadows for depth",   "romfs:/preset_gfx/低画质加阴影settings.xml" },
    { "Medium (default)",        "stock default, balanced",         "romfs:/preset_gfx/中画质（默认画质）/settings.xml" },
    { "High (extreme OC)",    "everything maxed, extreme OC only",   "romfs:/preset_gfx/高画质（极限超频专用）/settings.xml" },
};
#define GFX_PRESET(i) (g_lang_en ? GFX_PRESETS_EN[(i)] : GFX_PRESETS[(i)])
#define NUM_GFX_PRESETS (sizeof(GFX_PRESETS)/sizeof(GFX_PRESETS[0]))

/* 画质全局状态 */
static GfxItem g_gfx_items[GFX_MAX_ITEMS];
static int     g_gfx_count = 0;          /* 当前解析出的项数 */
static int     g_gfx_dirty = 0;          /* 画质有未保存修改 */
static char    g_gfx_path[512] = {0};    /* 当前画质文件路径 */
static int     g_gfx_cur_idx = 0;        /* 当前选中画质项 */

/* ★ v4: 页签系统 (0=存档 1=画质 2=工具) -- 取代原 g_gfx_show 的覆盖式页面 */
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

static int g_tab = TAB_DLC;

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

/* 手动调整时被改过的旋钮（记录原值以便显示已改） */
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
            snprintf(g_gc_diag[g_gc_diag_n++], 256, TR("打不开: %s", "cannot open: %s"), p);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        snprintf(g_gc_diag[g_gc_diag_n++], 256, TR("存在 %ld B: %s", "exists %ld B: %s"), sz, p);
    }
    if (g_gc_diag_n == 0) {
        snprintf(g_gc_diag[g_gc_diag_n++], 256, TR("(无候选路径)", "(no candidates)"));
    }
}

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

static int      g_dirty = 0;
static char     g_status_msg[256] = "";  /* ★ v6.3: 初始文案改为运行时按语言填 */
static uint32_t g_status_color = C_TEXT;

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

static unsigned g_write_failure_serial;

/* Keep the actual failing operation, rather than guessing a permissions issue. */
static void save_io_report(const char *operation, const SaveIoError *error, Result commit_result) {
    g_write_failure_serial++;
    const char *stage=error->stage?error->stage:"unknown";
    ensure_dir("sdmc:/switch/gta5save");
    const char *log_path="sdmc:/switch/gta5save/write_fail.log";
    FILE *log=fopen(log_path,"a");
    if(!log) {log_path="sdmc:/gta5save-write-fail.log";log=fopen(log_path,"a");}
    int logged=0;
    if(log) {
        int count=fprintf(log,"time=%" PRIu64 " operation=%s stage=%s errno=%d (%s) commit=0x%08" PRIx32
            "\npath=%s\nsave_dir=%s\nslot=%s\nsettings_source=%s\n\n",
            (uint64_t)time(NULL),operation,stage,error->code,strerror(error->code),(uint32_t)commit_result,
            error->path,g_save_dir,g_save_path,g_gfx_path);
        int closed=fclose(log);logged=count>0 && closed==0;
    }
    if(strcmp(stage,"target")==0 && error->code==ENODEV) {
        snprintf(g_status_msg,sizeof(g_status_msg),TR("%s失败：未找到游戏存档目录。先在游戏中保存一次，退出游戏后点「重新扫描」。",
            "%s failed: no game save target. Save once in game, close it, then Rescan."),operation);
    } else {
        const char *hint=error->code==ENOSPC?TR("空间不足","disk full"):
            (error->code==ENOTDIR?TR("目录被同名文件占用","a folder path is a file"):
            ((error->code==EACCES || error->code==EROFS)?TR("访问被拒绝或只读","access denied or read-only"):strerror(error->code)));
        snprintf(g_status_msg,sizeof(g_status_msg),TR("%s失败 [%s, errno=%d, rc=%08" PRIx32 "]：%s。%s",
            "%s failed [%s, errno=%d, rc=%08" PRIx32 "]: %s. %s"),
            operation,stage,error->code,(uint32_t)commit_result,hint,
            logged?log_path:TR("日志也无法写入","log could not be written"));
    }
    g_status_color=C_RED;
}

/* Back up the actual disk slot before replacing it with a bundled save. */
static int backup_current_save_timestamp(const char *reason) {
    if(!g_save_path[0]) {
        SaveIoError error={"target",ENODEV,""};
        save_io_report(TR("存档备份","Save backup"),&error,0);return 0;
    }
    time_t now=time(NULL);
    struct tm *tm_info=localtime(&now);
    char stamp[64],destination[640];
    if(tm_info) strftime(stamp,sizeof(stamp),"%Y%m%d_%H%M%S",tm_info);
    else snprintf(stamp,sizeof(stamp),"backup_%" PRIu64,(uint64_t)now);
    SaveIoError error;
    if(save_backup_create(g_save_path,"sdmc:/switch/gta5save",stamp,destination,sizeof(destination),&error)!=0) {
        save_io_report(TR("存档备份","Save backup"),&error,0);return 0;
    }
    Result rc=fsdevCommitDevice("sdmc");
    if(R_FAILED(rc)) {
        error=(SaveIoError){"backup-commit",EIO,""};
        snprintf(error.path,sizeof(error.path),"%s",destination);
        save_io_report(TR("存档备份","Save backup"),&error,rc);return 0;
    }
    char info_path[640];snprintf(info_path,sizeof(info_path),"%s",destination);
    char *name=strrchr(info_path,'/');if(name) snprintf(name+1,sizeof(info_path)-(size_t)(name+1-info_path),"info.txt");
    FILE *fp=fopen(info_path,"w");
    if(fp) {fprintf(fp,"%s | %s | %s\n",stamp,reason,g_save_path);fclose(fp);}
    return 1;
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
                                snprintf(g_backups[g_num_backups].note, sizeof(g_backups[g_num_backups].note), TR("自动备份 (%s)", "auto backup (%s)"), se->d_name);
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

/* Write settings only to the game save directory, never to an import source. */
static int gfx_write_target(const char *buf, size_t rd);
static int gfx_load(void);

/* Write the edited settings.xml to the detected game save directory. */
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

    /* SD XML files are import sources; the game reads the save directory. */
    int wrote = gfx_write_target(out_buf, out_len);
    free(out_buf);
    return wrote > 0 ? 0 : -1;
}

/* 保存画质 (供 A 键/退出时调用; 返回0=成功) */
static int gfx_save(void) {
    if (!g_gfx_dirty) return 0;
    unsigned before=g_write_failure_serial;
    if (gfx_save_to_file() != 0) {
        if(before==g_write_failure_serial) {
            snprintf(g_status_msg,sizeof(g_status_msg),TR("画质保存失败：源配置无法读取或解析。请重新载入预设或 XML。",
                "Graphics save failed: cannot read or parse source XML. Reload the preset or XML."));
            g_status_color=C_RED;
        }
        return -1;
    }
    g_gfx_dirty = 0;
    snprintf(g_status_msg, sizeof(g_status_msg), TR("画质设置已保存到游戏存档目录! 重启游戏生效", "Graphics saved to the game save directory! Reboot to take effect"));
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
                    /* ★ v6.3.2: 英文模式直接用字段名（name 本身是英文），中文用 cn */
                    strncpy(it->cn, g_lang_en ? GFX_DEFS[d].name : GFX_DEFS[d].cn,
                            sizeof(it->cn) - 1);
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

/* Read-only diagnostics cannot establish write access: 0=readable, 1=parent exists, 2=unavailable. */
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
                 "%s : %s", p, st == 0 ? TR("可读取", "readable") : (st == 1 ? TR("文件不可读/不存在", "file unreadable/missing") : TR("目录不可用", "dir unavailable")));
    }
    g_check_paths_count = total;
    snprintf(g_status_msg, sizeof(g_status_msg),
             TR("路径检查: 可读 %d / 文件不可读 %d / 目录不可用 %d (共 %d 条)",
             "Paths: readable %d / file unavailable %d / directory unavailable %d (of %d)"), ok, ro, no, total);
    g_status_color = (ok > 0) ? C_GREEN : C_RED;
    show_toast(TR("路径自检完成", "Path self-test done"), ok > 0 ? C_GREEN : C_RED);
}

static int gfx_write_target(const char *buf, size_t rd) {
    SaveIoError error;
    if(save_settings_write(g_save_dir,buf,rd,&error)!=0) {
        save_io_report(TR("画质写入","Graphics write"),&error,0);return 0;
    }
    if(g_is_native_mount) {
        Result rc=fsdevCommitDevice("save");
        if(R_FAILED(rc)) {
            error=(SaveIoError){"commit",EIO,""};
            snprintf(error.path,sizeof(error.path),"%s/settings.xml",g_save_dir);
            save_io_report(TR("画质写入","Graphics write"),&error,rc);return 0;
        }
    }
    snprintf(g_gfx_path,sizeof(g_gfx_path),"%.*s/settings.xml",(int)sizeof(g_gfx_path)-14,g_save_dir);
    return 1;
}

/* Apply a graphics preset to the detected game settings.xml. */
static int gfx_apply_preset(int idx) {
    if (idx < 0 || idx >= (int)NUM_GFX_PRESETS) return -1;
    char *buf = NULL;
    size_t rd = gfx_read_all(GFX_PRESET(idx).path, &buf);
    if (!buf || rd == 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), TR("无法打开内置画质: %s", "cannot open builtin gfx: %s"), GFX_PRESET(idx).name);
        g_status_color = C_RED;
        return -1;
    }

    /* 1. Write and commit the real game target before reporting success. */
    int applied_to_file = 0;
    applied_to_file = gfx_write_target(buf, rd);

    /* 2. 解析预设内容到缓冲供 UI 显示 */
    gfx_parse_xml(buf, rd);
    free(buf);

    if (applied_to_file > 0) {
        g_gfx_dirty = 0;
        snprintf(g_status_msg, sizeof(g_status_msg), TR("已应用【%s】画质并保存到 %d 个位置! 重启游戏生效",
        "Applied [%s] preset to %d locations! Reboot to take effect"), GFX_PRESET(idx).name, applied_to_file);
        g_status_color = C_ACCENT;
        show_toast(TR("画质预设已应用", "GFX preset applied"), C_ACCENT);
    } else {
        g_gfx_dirty = 1;
        snprintf(g_gfx_path,sizeof(g_gfx_path),"%s",GFX_PRESET(idx).path);
        /* Preserve this preset as the pending source for a later retry. */
        return -1;
    }
    return 0;
}

/* 从 /switch/gta5save/ 目录导入指定画质文件作为当前画质配置
 * (复制到游戏存档目录并重新解析; 无目标时返回具体错误)
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
        snprintf(g_status_msg, sizeof(g_status_msg), TR("未找到 %s! 请先放入画质文件", "%s not found! put a gfx file first"), src);
        g_status_color = C_RED;
        return -1;
    }
    fclose(t);

    char *buf = NULL;
    size_t rd = gfx_read_all(src, &buf);
    if (!buf || rd == 0) return -1;

    /* Imported XML is a source, not a fallback destination. */
    int wrote = gfx_write_target(buf, rd);
    if (wrote > 0) {
        g_gfx_dirty = 0;
    } else {
        /* Keep the selected XML for preview/retry; writes still require the game target. */
        snprintf(g_gfx_path,sizeof(g_gfx_path),"%s",src);
        gfx_parse_xml(buf,rd);
        g_gfx_dirty = 1;
        free(buf);
        return -1;
    }
    gfx_parse_xml(buf, rd);
    free(buf);
    snprintf(g_status_msg, sizeof(g_status_msg), TR("已从 /switch/gta5save/ 导入 %s 并写入 %d 个位置! 重启游戏生效",
        "Imported %s from /switch/gta5save/ to %d locations! Reboot to take effect"),
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
        snprintf(g_status_msg, sizeof(g_status_msg), TR("未找到 settings.xml (请放在存档同目录或 /switch/gta5save/)",
        "settings.xml not found (put it next to the save or in /switch/gta5save/)"));
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
        snprintf(g_status_msg, sizeof(g_status_msg), TR("无法识别的存档格式 (非标准 SGTA)", "unrecognized save format (not SGTA)"));
        g_status_color = C_RED;
        return -1;
    }

    if (!already) {
        if (gta5_decrypt_body(g_save_buf, g_save_sz, g_plat, g_start_off) != 0) {
            snprintf(g_status_msg, sizeof(g_status_msg), TR("AES 解密失败 (请确认是PC/Switch移植版)", "AES decrypt failed (is this the PC/Switch port?)"));
            g_status_color = C_RED;
            return -1;
        }
    }
    g_is_plain = 1;

    /* 提取剧情名称 */
    extract_save_title(g_save_buf, g_current_title, sizeof(g_current_title));

    /* 提取三位角色的金钱与技能属性 */
    for (int i = 0; i < 3; i++) {

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

    /* 载入新存档后清除所有技能已修改标记 */
    memset(g_skill_modified, 0, sizeof(g_skill_modified));
    return 0;
}

static int load_file_into_memory(const char *path, const char *source_label) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(g_status_msg, sizeof(g_status_msg), TR("打开文件失败: %s", "failed to open: %s"), path);
        g_status_color = C_RED;
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    size_t sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0x200) {
        fclose(fp);
        snprintf(g_status_msg, sizeof(g_status_msg), TR("存档文件损坏 (大小 < 512字节)", "corrupt save file (< 512 bytes)"));
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
    snprintf(g_status_msg, sizeof(g_status_msg), TR("[已载入] %s (%s)", "[Loaded] %s (%s)"), fn, g_current_title[0] ? g_current_title : source_label);
    g_status_color = C_GREEN;
    return 0;
}

static int load_initial_save(void) {
    if (!locate_and_select_save()) {
        snprintf(g_status_msg, sizeof(g_status_msg), TR("未扫描到存档! 按 ZL 载入内置进度，或按 ZR 从 /switch/gta5save/ 载入历史备份",
        "No save found! ZL = builtin progress, ZR = backup from /switch/gta5save/"));
        g_status_color = C_RED;
        return -1;
    }
    return load_file_into_memory(g_save_path, TR("自动匹配最近修改存档", "auto-matched most recent save"));
}

static int switch_to_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= g_num_slots) return -1;
    g_active_slot_idx = slot_idx;
    strncpy(g_save_path, g_slots[slot_idx].fullpath, sizeof(g_save_path) - 1);
    g_dirty = 0;
    return load_file_into_memory(g_save_path, TR("切换槽位", "Switch Slot"));
}

static int load_backup_save(int backup_idx) {
    if (backup_idx < 0 || backup_idx >= g_num_backups) return -1;
    if (load_file_into_memory(g_backups[backup_idx].fullpath, g_backups[backup_idx].dir_name) == 0) {
        g_dirty = 1;
        snprintf(g_status_msg, sizeof(g_status_msg), TR("[已载入快照 %s] 按 A 键保存写回游戏生效", "[Snapshot %s loaded] press A to write back"), g_backups[backup_idx].dir_name);
        g_status_color = C_ACCENT;
        return 0;
    }
    return -1;
}

static int load_preset_save(int index) {
    if (index < 0 || index >= (int)NUM_PRESETS) return -1;

    FILE *fp = fopen(PRESET_SAVE(index).path, "rb");
    if (!fp) {
        snprintf(g_status_msg, sizeof(g_status_msg), TR("无法打开内置存档: %s", "cannot open builtin save: %s"), PRESET_SAVE(index).path);
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
    snprintf(g_status_msg, sizeof(g_status_msg), TR("[内置进度已载入] %s | 按 A 键写入游戏生效", "[Builtin progress loaded] %s | press A to write"), PRESET_SAVE(index).name);
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
    if (!backup_current_save_timestamp(TR("保存前备份", "backup before save"))) return -1;

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
        snprintf(g_status_msg, sizeof(g_status_msg), TR("保存失败: 校验和重算或加密出错", "Save failed: checksum or encryption error"));
        g_status_color = C_RED;
        return -1;
    }

    SaveIoError error;
    if(save_io_write(g_save_path,enc_buf,g_save_sz,&error)!=0) {
        free(enc_buf);g_dirty=1;
        save_io_report(TR("存档写入（原档备份已保留）","Save write (backup kept)"),&error,0);
        return -1;
    }
    free(enc_buf);
    if(g_is_native_mount) {
        Result rc=fsdevCommitDevice("save");
        if(R_FAILED(rc)) {
            error=(SaveIoError){"commit",EIO,""};
            snprintf(error.path,sizeof(error.path),"%s",g_save_path);g_dirty=1;
            save_io_report(TR("存档提交（原档备份已保留）","Save commit (backup kept)"),&error,rc);
            return -1;
        }
    }

    g_dirty = 0;
    const char *fn = strrchr(g_save_path, '/');
    if (fn) fn++; else fn = g_save_path;
    snprintf(g_status_msg, sizeof(g_status_msg), TR("[已保存成功] 写入 %.96s! 进游戏请通过【暂停->游戏->载入游戏】手动载入该槽位生效",
                                                 "[Saved] Wrote %.96s! In game use Pause > Game > Load Game to activate the slot"), fn);
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
    char source_path[768]; /* Cached discovery path, avoiding repeated root searches. */
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
static int g_script_count = 0, g_script_sel = 0;
static int g_script_rpf_n     = -1;   /* ★ v5.6: RPF 里读到的 .nsc 条数（<0 = 读失败） */
static char g_script_rpf_err[200] = {0};  /* ★ v5.6: 读失败原因 */
/* ★ v5.7: 页内目录信息面板（点「目录信息」/ 按 Y 显示，再按一次关闭）
 *   原来的实现只写状态栏单行 ⇒ 内容太长被裁掉，用户以为没反应 */
static char g_script_dirinfo[512] = {0};
static int  g_script_dirinfo_on = 0;
static int  g_script_dirinfo_ok = 1;

static void script_scan(void) {
    g_script_count = 0;
    g_script_from_alt = 0;
    script_ensure_dir();               /* ★ v5.5: 先确保目录存在 */
    /* ★ v5.6: 必须处理负数返回！旧代码 ninst<0 时循环不执行 ⇒ 所有项都显示未安装，
     *   而且界面无任何提示 ⇒ 用户以为没装上，其实是读 RPF 失败。 */
    /* ★ v5.7: 这里只取总数（max=0）；已装判定改用 gc_script_query（见下） */
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
    if (g_script_sel < 0 || g_script_sel >= g_script_count) { dlc_err(TR("没有选中项", "Nothing selected")); return; }
    ScriptItem *it = &g_script_items[g_script_sel];
    char nm[GC_SCRIPT_NAME_MAX];
    snprintf(nm, sizeof(nm), "%s", it->name);
    if (!it->installed) { dlc_err(TR("%s 本来就没装", "%s is not installed"), nm); return; }
    char err[256];
    err[0] = 0;
    int rc = gc_script_uninstall(nm, err, sizeof(err));
    if (rc != GC_SCRIPT_OK) {
        dlc_err(TR("删除失败: %s", "Delete failed: %s"), err[0] ? err : TR("未知错误", "unknown error"));
        return;
    }
    fsdevCommitDevice("sdmc");
    /* ★ 回读校验：确认真的不在 RPF 里了 */
    script_verify_installed();
    int still = 0;
    for (int k = 0; k < g_script_count; k++)
        if (strcasecmp(g_script_items[k].name, nm) == 0) still = g_script_items[k].installed;
    if (still)
        dlc_err(TR("%s 删除后回读仍在 RPF 里，请检查 update2.rpf", "%s still in RPF after delete, check update2.rpf"), nm);
    else
        dlc_msg(TR("[v] 已删除 %s（回读校验通过，重启游戏生效）", "[v] Deleted %s (verified, reboot to take effect)"), nm);
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
/* 执行二次确认通过的动作 */
static void script_mgr_apply(void) {
    char err[256];
    char what[160];
    int rc;

    if (g_mod_pending_kind == 0) {
        const char *nm = gc_script_stock_name(g_mod_pending_idx);
        if (!nm) { dlc_err(TR("内部错误：还原索引越界", "internal error: restore index out of range")); return; }
        snprintf(what, sizeof(what), "%s", nm);
        err[0] = 0;
        rc = gc_script_restore_stock(nm, err, sizeof(err));
        if (rc == GC_SCRIPT_OK) {
            fsdevCommitDevice("sdmc");
            script_verify_installed();
            dlc_msg(TR("[v] 已还原官方 %s（重启游戏生效）", "[v] Restored stock %s (reboot to take effect)"), what);
        } else {
            dlc_err(TR("还原失败: %s", "Restore failed: %s"), err[0] ? err : TR("未知错误", "unknown error"));
        }
        return;
    }

    const GcBuiltinMod *m = gc_script_builtin(g_mod_pending_idx);
    if (!m) { dlc_err(TR("内部错误：模组索引越界", "internal error: mod index out of range")); return; }
    snprintf(what, sizeof(what), "%s", MOD_TITLE(m) ? MOD_TITLE(m) : "?");
    err[0] = 0;
    rc = gc_script_builtin_install(g_mod_pending_idx, err, sizeof(err));
    if (rc == GC_SCRIPT_OK) {
        fsdevCommitDevice("sdmc");
        script_verify_installed();
        dlc_msg(TR("[v] %s 安装完成（重启游戏生效）", "[v] %s installed (reboot to take effect)"), what);
    } else {
        dlc_err(TR("安装失败: %s", "Install failed: %s"), err[0] ? err : TR("未知错误", "unknown error"));
    }
}

/* 进入二次确认弹窗 */
static void script_install_selected(void) {
    if (g_script_sel < 0 || g_script_sel >= g_script_count) { dlc_err(TR("没有选中项", "Nothing selected")); return; }
    ScriptItem *it = &g_script_items[g_script_sel];
    char nm[GC_SCRIPT_NAME_MAX];
    snprintf(nm, sizeof(nm), "%s", it->name);
    /* ★ v5.7: 提前拦截无效文件（0 字节/过大）——否则会拿它覆盖游戏原版脚本 */
    if (!script_file_usable(it->size)) {
        dlc_err(TR("%s 文件无效（%u 字节）—— 不能安装，请检查源文件", "%s invalid file (%u bytes) - cannot install"),
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
        dlc_err(TR("安装失败: %s", "Install failed: %s"), err[0] ? err : TR("未知错误", "unknown error"));
        return;
    }
    fsdevCommitDevice("sdmc");
    /* ★ v5.6/v5.7: 回读校验 —— 用按名查询（不受 RPF 条目总数限制） */
    script_verify_installed();
    int now = 0;
    for (int k = 0; k < g_script_count; k++)
        if (strcasecmp(g_script_items[k].name, nm) == 0) now = g_script_items[k].installed;
    if (now)
        dlc_msg(TR("[v] 已安装 %s（回读校验通过，重启游戏生效）", "[v] Installed %s (verified, reboot to take effect)"), nm);
    else
        dlc_err(TR("%s 安装后回读没找到，可能写入未生效", "%s not found after install, write may have failed"), nm);
}

/* ★ v5.5: 显示脚本目录信息（路径 + 内容统计），并确保目录已自动创建
 *   ★ v5.6: 同时回读 RPF 实况，把「装没装」的权威数据摆出来
 *   ★ v5.7: 原实现只写状态栏（单行 1280px 放不下 ⇒ 用户反馈点了没反应）。
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
                 TR("目录: %s\n磁盘上 %d 个 .nsc\n读 update2.rpf 失败: %s",
                    "Dir: %s\n%d .nsc on disk\nread update2.rpf failed: %s"),
                 SCRIPT_DIR, n_nsc,
                 g_script_rpf_err[0] ? g_script_rpf_err : TR("未知原因", "unknown"));
        g_script_dirinfo_ok = 0;
        g_script_dirinfo_on = 1;
        dlc_err(TR("读 update2.rpf 失败（详见页内面板）", "read update2.rpf failed (see panel)"));
        return;
    }
    int ninst = 0;
    for (int i = 0; i < g_script_count; i++)
        if (g_script_items[i].installed) ninst++;
    snprintf(g_script_dirinfo, sizeof(g_script_dirinfo),
             TR("脚本目录: %s\n磁盘上 %d 个 .nsc   已装 %d 个\n%s 内共 %d 个 .nsc 条目",
                "Script dir: %s\n%d .nsc on disk   %d installed\n%s contains %d .nsc entries"),
             SCRIPT_DIR, n_nsc, ninst, GC_SCRIPT_REL_RPF, g_script_rpf_n);
    g_script_dirinfo_ok = 1;
    g_script_dirinfo_on = 1;
    dlc_msg(TR("目录信息已显示在列表中（再按一次 Y 关闭）", "Dir info shown in list (press Y again to close)"));
}

/* ★ v32: 前向声明（dlc_scan_src 里要用，而它的定义在后面） */
static int dlc_src_path_of(const char *name, char *out, size_t outsz);
static int     g_dlc_count   = 0;      /* 已装 DLC 数 */
static int     g_dlc_sel     = 0;      /* 当前选中 */
static int     g_dlc_view    = 0;      /* 0=已装  1=待导入 */

/* 待导入（从 /switch/ 等目录扫到的 dlcpack） */
static DlcItem g_dlc_src[DLC_SRC_MAX];
static int     g_dlc_src_count = 0;
static int     g_dlc_src_sel   = 0;
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
        dlc_err(TR("打不开 dlcpacks 目录（SD 卡路径不对？）", "cannot open dlcpacks dir (wrong SD path?)"));
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
        snprintf(it->source_path,sizeof(it->source_path),"%s",sub);
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
            dlc_err(TR("打开 update.rpf 失败: %s", "open update.rpf failed: %s"), gc_rpf_last_error());
            return rc;
        }
        g_gc_opened = 1;
    }
    rc = gc_rpf_find(&g_gc_rpf, "dlclist.xml", &g_dlc_xml_ent);
    if (rc != GC_OK) {
        dlc_err(TR("在 update.rpf 里找不到 dlclist.xml (rc=%d)", "dlclist.xml not found in update.rpf (rc=%d)"), rc);
        return rc;
    }
    u8 *buf = NULL; size_t blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_dlc_xml_ent, &buf, &blen);
    if (rc != GC_OK) {
        dlc_err(TR("读 dlclist.xml 失败 (rc=%d)", "read dlclist.xml failed (rc=%d)"), rc);
        return rc;
    }
    g_dlc_xml = (char *)malloc(blen + 1);
    if (!g_dlc_xml) { free(buf); dlc_err(TR("内存不足", "Out of memory")); return GC_ERR_NOMEM; }
    memcpy(g_dlc_xml, buf, blen);
    g_dlc_xml[blen] = '\0';
    g_dlc_xml_len = blen;
    free(buf);
    g_dlc_xml_loaded = 1;

    /* ---- 第二处：extratitleupdatedata.meta ---- */
    rc = gc_rpf_find(&g_gc_rpf, "extratitleupdatedata.meta", &g_dlc_etud_ent);
    if (rc != GC_OK) {
        /* 找不到不算致命：有些版本没有这个文件，只提示 */
        dlc_msg(TR("提示：update.rpf 里没有 extratitleupdatedata.meta", "note: no extratitleupdatedata.meta in update.rpf"));
        g_dlc_etud_loaded = 0;
        return GC_OK;
    }
    buf = NULL; blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_dlc_etud_ent, &buf, &blen);
    if (rc != GC_OK) {
        dlc_msg(TR("读 extratitleupdatedata.meta 失败 (rc=%d)", "read extratitleupdatedata.meta failed (rc=%d)"), rc);
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
 *   "</Item" 2nd char is '/', vs "Item" 的 'I' 不等 ⇒ 不会把闭合标签当开头。 */
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
        dlc_err(TR("update.rpf 是只读打开的，无法写入", "update.rpf is open read-only, cannot write"));
        return GC_ERR_READONLY;
    }
    int rc = gc_rpf_write_ex(&g_gc_rpf, &g_dlc_xml_ent,
                             (const u8 *)g_dlc_xml, g_dlc_xml_len, 1);
    if (rc != GC_OK) {
        dlc_err(TR("写入 dlclist.xml 失败 (rc=%d): %s", "write dlclist.xml failed (rc=%d): %s"), rc, gc_rpf_last_error());
        return rc;
    }
    return GC_OK;
}

/* 把改过的 etud 写回 update.rpf */
static int dlc_save_etud(void) {
    if (!g_dlc_etud_loaded || !g_dlc_etud) return GC_OK;   /* 没有就不写 */
    if (!gc_rpf_writable(&g_gc_rpf)) {
        dlc_err(TR("update.rpf 是只读打开的，无法写入", "update.rpf is open read-only, cannot write"));
        return GC_ERR_READONLY;
    }
    int rc = gc_rpf_write_ex(&g_gc_rpf, &g_dlc_etud_ent,
                             (const u8 *)g_dlc_etud, g_dlc_etud_len, 1);
    if (rc != GC_OK) {
        dlc_err(TR("写入 extratitleupdatedata.meta 失败 (rc=%d): %s", "write extratitleupdatedata.meta failed (rc=%d): %s"),
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
    dlc_msg(TR("扫描完成：%d 个 DLC，其中 %d 个已完整注册", "Scan done: %d DLCs, %d fully registered"), g_dlc_count, full);
    return 0;
}

/* ★ 切换选中项的注册状态（同时改 dlclist + etud 两处，缺一游戏不认） */
static int dlc_toggle_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err(TR("没有选中项", "Nothing selected")); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    if (!g_dlc_xml_loaded) {
        dlc_err(TR("dlclist.xml 未载入，先按 X 扫描", "dlclist.xml not loaded, press X to scan first")); return -1;
    }

    int want = !it->full;    /* 目标状态：两处齐 = 已注册 */
    int rc;

    if (want) {
        /* ---- 注册：dlclist + etud 都加 ---- */
        if (dlc_xml_add(it->name) < 0) { dlc_err(TR("dlclist 注册失败（空间或格式问题）", "dlclist registration failed (space or format)")); return -1; }
        if (g_dlc_etud_loaded) {
            if (dlc_etud_add(it->name) < 0) {
                /* etud 失败要回滚 dlclist，保持两处一致 */
                dlc_xml_del(it->name);
                dlc_err(TR("etud 挂载失败（找不到 </Mounts>？）", "etud mount failed (no </Mounts>?)")); return -1;
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
        if (a <= 0 && b <= 0) { dlc_err(TR("注销失败：两处都没找到条目", "unregister failed: entry not found in either place")); return -1; }
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
        dlc_msg(TR("%s 已%s（dlclist %s / etud %s，回读校验通过）", "%s %s (dlclist %s / etud %s, verified)"), it->name,
                it->full ? TR("注册", "registered") : TR("注销", "unregistered"),
                it->registered ? TR("有", "yes") : TR("无", "no"),
                it->mounted ? TR("有", "yes") : TR("无", "no"));
        g_dlc_msg_ok = 1;
    } else {
        dlc_msg(TR("%s 已%s，但回读校验失败 (dlclist=%d etud=%d)", "%s %s, but verification failed (dlclist=%d etud=%d)"), it->name,
                it->full ? TR("注册", "registered") : TR("注销", "unregistered"), v1, v2);
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
    if (depth > 8) { snprintf(err, errsz, TR("目录嵌套过深", "dir nesting too deep")); return -1; }
    DIR *d = opendir(src);
    if (!d) { snprintf(err, errsz, TR("打不开源目录", "cannot open source dir")); return -1; }
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
            if (!fi) { snprintf(err, errsz, TR("打不开 %s", "cannot open %s"), ent->d_name); closedir(d); return -1; }
            FILE *fo = fopen(dp, "wb");
            if (!fo) { fclose(fi); snprintf(err, errsz, TR("无法创建 %s", "cannot create %s"), ent->d_name); closedir(d); return -1; }
            static u8 cbuf[65536];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), fi)) > 0) {
                if (fwrite(cbuf, 1, n, fo) != n) {
                    fclose(fi); fclose(fo);
                    snprintf(err, errsz, TR("写入 %s 失败", "write %s failed"), ent->d_name);
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
    if (files == 0) { snprintf(err, errsz, TR("源目录是空的", "source dir is empty")); return -1; }
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
    return dlc_copy_tree_ex(src, dst, &done, total, TR("复制文件", "copying files"), err, errsz, 0);
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
            snprintf(it->source_path,sizeof(it->source_path),"%s",sub);
            /* ★ v5.1: 中文显示名（同一约定） */
            {
                char sdir[768];
                if (snprintf(sdir,sizeof(sdir),"%s",sub)>0)
                    dlc_read_cn_name(sdir, it->cn, sizeof(it->cn));
            }
            /* ★ v33: 资源构成（用统一 probe，一次 IO；PC/Switch 都能统计） */
            {
                char srp[700], rp2[760];
                if (snprintf(srp,sizeof(srp),"%s",sub)>0) {
                    snprintf(rp2, sizeof(rp2), "%s/dlc.rpf", srp);
                    GcDlcProbe pb;
                    gc_dlc_probe(rp2, &pb);
                    it->fmt = pb.fmt;it->map_st = pb.map_st;
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
        dlc_msg(TR("没找到待导入的 dlcpack。请把 dlcpack 目录（含 dlc.rpf）放到 sdmc:/switch/gta5save/dlc/ 下",
                   "No pending dlcpack. Put the dlcpack folder (with dlc.rpf) into sdmc:/switch/gta5save/dlc/"));
        g_dlc_msg_ok = 0;
        return -1;
    }
    dlc_msg(TR("找到 %d 个待导入 dlcpack（来源 %d 个目录）", "Found %d pending dlcpacks (from %d dirs)"), g_dlc_src_count, g_dlc_src_dir_n);
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
        dlc_err(TR("没有选中待导入项", "no pending item selected")); return -1;
    }
    if (!g_dlc_xml_loaded) {
        if (dlc_load_dlclist() != GC_OK) return -1;
    }
    const char *name = g_dlc_src[g_dlc_src_sel].name;

    char src[700];
    if (dlc_src_path_of(name, src, sizeof(src)) != 0) {
        dlc_err(TR("找不到 %s 的源目录", "source dir of %s not found"), name); return -1;
    }
    char dst[700];
    snprintf(dst, sizeof(dst), "%s/%s", DLC_DLCPACKS, name);

    /* ---- (1) 复制（带进度） ---- */
    dlc_prog_begin(TR("正在导入 DLC", "Importing DLC"), name);
    char err[256] = {0};
    int nf = dlc_copy_tree(src, dst, err, sizeof(err));
    if (nf < 0) {
        dlc_prog_end();
        dlc_err(TR("复制失败: %s", "copy failed: %s"), err);
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
        dlc_prog_set(TR("正在转换为 Switch 格式", "Converting to Switch format"), TR("扫描平台目录与资源扩展名...", "Scanning platform dirs & extensions..."), -1);
        dlc_prog_frame();
        char cerr[256] = {0};
        int crc = gc_dlc_convert(rpfpath, &cst, cerr, sizeof(cerr));
        if (crc != GC_DLC_OK) {
            dlc_prog_end();
            dlc_err(TR("★ 格式转换失败（这个包是 PC 格式，不转换会闪退）：%s",
           "* format conversion failed (PC pack crashes without it): %s"), cerr);
            return -1;
        }
        converted = 1;
        fsdevCommitDevice("sdmc");
        /* 转换后再确认一次 */
        int fmt2 = gc_dlc_is_switch_format(rpfpath);
        if (fmt2 != 1) {
            dlc_prog_end();
            dlc_err(TR("★ 转换后仍不是 Switch 格式，已中止（避免闪退）", "* still not Switch format after convert, aborted (avoid crash)"));
            return -1;
        }
    }

    /* ---- (2) 注册 ---- */
    dlc_prog_set(TR("正在注册", "Registering"), TR("写 dlclist.xml + extratitleupdatedata.meta...", "Writing dlclist.xml + extratitleupdatedata.meta..."), -1);
    dlc_prog_frame();
    if (dlc_xml_add(name) < 0) {
        dlc_prog_end(); dlc_err(TR("复制好了，但 dlclist 注册失败", "copied, but dlclist registration failed")); return -1;
    }
    if (g_dlc_etud_loaded && dlc_etud_add(name) < 0) {
        dlc_prog_end(); dlc_err(TR("复制好了，但 etud 挂载失败", "copied, but etud mount failed")); return -1;
    }
    int rc = dlc_save_dlclist();
    if (rc == GC_OK) rc = dlc_save_etud();
    if (rc != GC_OK) {
        dlc_prog_end();
        dlc_err(TR("复制好了，但注册写盘失败 (rc=%d)", "copied, but registration write failed (rc=%d)"), rc);
        return rc;
    }

    /* ---- (3) 回读校验 ---- */
    dlc_prog_set(TR("正在校验", "Verifying"), TR("回读 update.rpf 比对...", "Re-reading update.rpf to compare..."), -1);
    dlc_prog_frame();
    int v1 = gc_rpf_verify(&g_gc_rpf, &g_dlc_xml_ent,
                           (const u8 *)g_dlc_xml, g_dlc_xml_len);
    int v2 = g_dlc_etud_loaded
           ? gc_rpf_verify(&g_gc_rpf, &g_dlc_etud_ent,
                           (const u8 *)g_dlc_etud, g_dlc_etud_len)
           : GC_OK;

    /* ---- (4) 完整性检查（缺 _hi 会闪退） ---- */
    dlc_prog_set(TR("正在检查完整性", "Checking integrity"), TR("车辆资源是否齐全...", "vehicle resources..."), -1);
    dlc_prog_frame();
    char chk[256] = {0};
    dlc_check_integrity(name, chk, sizeof(chk));

    dlc_prog_end();

    if (converted) {
        dlc_msg(TR("[v] %s 导入完成：%d 文件｜★已转 Switch 格式（改名 %d 处：顶层 %d + 内嵌 %d）｜dlclist %s / etud %s%s%s",
           "[v] %s imported: %d files | *converted (%d renames: top %d + embedded %d) | dlclist %s / etud %s%s%s"),
                name, nf, cst.n_top_renamed + cst.n_inner_renamed,
                cst.n_top_renamed, cst.n_inner_renamed,
                v1 == GC_OK ? "OK" : TR("失败", "fail"), v2 == GC_OK ? "OK" : TR("失败", "FAIL"),
                chk[0] ? "  |  " : "", chk);
    } else {
        dlc_msg(TR("[v] %s 导入完成：%d 个文件（已是 Switch 格式）｜dlclist %s / etud %s%s%s",
           "[v] %s imported: %d files (already Switch format) | dlclist %s / etud %s%s%s"),
                name, nf,
                v1 == GC_OK ? "OK" : TR("失败", "FAIL"), v2 == GC_OK ? "OK" : TR("失败", "FAIL"),
                chk[0] ? "  |  " : "", chk);
    }
    g_dlc_msg_ok = (v1 == GC_OK && v2 == GC_OK) ? 1 : 0;
    if (cst.n_skipped_compressed > 0) {
        dlc_err(TR("注意：有 %d 个压缩存储的内嵌 rpf 未处理，可能影响兼容性",
           "note: %d compressed embedded rpfs untouched, may affect compatibility"),
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
/* ★ v31: 地图数据自检的包装（只读 RPF 头部，很快） */
/* ★★ 对「已装但格式不对」的包就地转换（不重新导入）
 *    用途：用户之前用旧版工具导入了 PC 格式的包，现在一键修好 */
static int dlc_convert_selected(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err(TR("没有选中项", "Nothing selected")); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    if (!it->has_rpf) { dlc_err(TR("%s 没有 dlc.rpf", "%s has no dlc.rpf"), it->name); return -1; }
    if (it->fmt == 1)  { dlc_err(TR("%s 已经是 Switch 格式，无需转换", "%s is already Switch format"), it->name); return -1; }

    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, it->name);

    dlc_prog_begin(TR("正在转换为 Switch 格式", "Converting to Switch format"), it->name);
    dlc_prog_frame();
    char err[256] = {0};
    GcDlcStats st; memset(&st, 0, sizeof(st));
    int rc = gc_dlc_convert(rpf, &st, err, sizeof(err));
    dlc_prog_end();

    if (rc != GC_DLC_OK) {
        dlc_err(TR("%s 转换失败：%s", "%s convert failed: %s"), it->name, err);
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
        dlc_err(TR("%s 转换后仍不是 Switch 格式，请检查这个包", "%s still not Switch format, check this pack"), it->name);
        return -1;
    }
    dlc_msg(TR("[v] %s 已转为 Switch 格式（改名 %d 处：顶层 %d + 内嵌 %d，名字池 +%d B）%s",
           "[v] %s converted to Switch format (%d renames: top %d + embedded %d, names +%d B) %s"),
            it->name, st.n_top_renamed + st.n_inner_renamed,
            st.n_top_renamed, st.n_inner_renamed, st.names_grew,
            st.n_hd_removed > 0 ? TR("  [已删高清贴图]", "  [HD stripped]") : "");
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
        dlc_err(TR("没有选中项", "Nothing selected")); return -1;
    }
    DlcItem *it = &g_dlc_items[g_dlc_sel];
    char name[DLC_NAME_MAX];
    snprintf(name, sizeof(name), "%s", it->name);
    if (!it->has_rpf) { dlc_err(TR("%s 没有 dlc.rpf", "%s has no dlc.rpf"), name); return -1; }

    char rpf[400];
    snprintf(rpf, sizeof(rpf), "%s/%s/dlc.rpf", DLC_DLCPACKS, name);

    dlc_prog_begin(TR("正在屏蔽高清贴图(+hi)", "Stripping HD textures (+hi)"), name);
    dlc_prog_frame();
    char err[256] = {0};
    int removed = 0;
    int rc = gc_dlc_hide_hd_textures(rpf, &removed, err, sizeof(err));
    dlc_prog_end();

    if (rc < 0) {
        dlc_err(TR("%s 处理失败：%s", "%s failed: %s"), name, err[0] ? err : TR("未知错误", "unknown error"));
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
        dlc_msg(TR("[v] %s 已屏蔽 %d 个高清贴图(+hi.ytd 系)，重启游戏生效",
           "[v] %s stripped %d HD textures (+hi.ytd), reboot to take effect"), name, removed);
        g_dlc_msg_ok = 1;
    } else {
        dlc_msg(TR("%s 没有高清贴图可屏蔽（已处理过，或本来就没有）", "%s has no HD textures to strip (already done, or none)"), name);
    }
    return removed;
}

/* ★ v5.4: 模型目录信息 —— 列出 SD 卡上 3 个相关目录的路径与内容统计
 *   Switch 无文件管理器，这里把路径直接给出来，方便用户在电脑上找。
 *   统计每个目录下的子目录数（= 装了/待装的 mod 数）。 */
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
        dlc_err(TR("没有选中项", "Nothing selected")); return -1;
    }
    if (!g_dlc_xml_loaded) {
        dlc_err(TR("先点「载入并扫描」再操作", "press Load & Scan first")); return -1;
    }
    g_dlc_confirm = 1;
    return 0;
}

/* ★ 确认后的真正删除：注销 -> 删目录 -> 刷新列表 */
static int dlc_do_delete(void) {
    if (g_dlc_sel < 0 || g_dlc_sel >= g_dlc_count) {
        dlc_err(TR("没有选中项", "Nothing selected")); return -1;
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
        dlc_err(TR("%s: 注销写盘失败 (rc=%d)，已中止删除", "%s: unregister write failed (rc=%d), delete aborted"), name, rc);
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
        dlc_err(TR("[v] %s 已注销（校验 %s/%s），但 %d 个文件/子目录删不掉（SD 卡权限限制），请用电脑删除：%s",
           "[v] %s unregistered (verify %s/%s), but %d files/subdirs undeletable (SD permission limit), remove from PC: %s"),
                name, v1 == GC_OK ? "OK" : TR("失败", "FAIL"), v2 == GC_OK ? "OK" : TR("失败", "FAIL"),
                fail, dir);
        return -1;
    }
    dlc_msg(TR("[v] %s 已彻底删除（注销两处注册 + 清空目录，校验 %s/%s）",
           "[v] %s fully deleted (unregistered + dir emptied, verify %s/%s)"),
            name, v1 == GC_OK ? "OK" : TR("失败", "FAIL"), v2 == GC_OK ? "OK" : TR("失败", "FAIL"));
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
static int dlc_check_integrity(const char *name,char *out,size_t cap) {
    char path[800],err[256]={0};snprintf(path,sizeof(path),"%s/%s/dlc.rpf",DLC_DLCPACKS,name);
    GcVehicleReport *r=malloc(sizeof(*r));
    if(!r || gc_vehicle_inspect(path,r,NULL,NULL,err,sizeof(err))) {
        snprintf(out,cap,TR("无法检查车辆：%s","Vehicle inspection failed: %s"),err[0]?err:TR("内存不足","Out of memory"));free(r);return -1;
    }
    int missing=0;for(int i=0;i<r->count;i++)if(r->vehicles[i].flags!=3)missing++;
    snprintf(out,cap,TR("识别 %d 个车辆/刷车名；%d 项模型配对异常%s。完整名称见「检查资源」。","Found %d vehicle/spawn names; %d pairing issues%s. Open Inspect for the full list."),r->count,missing,r->partial?TR("，部分归档未检查","; some archives uninspected"):"");
    int rc=missing || r->partial?-1:0;free(r);return rc;
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
        gc_msg(TR("打开 update.rpf 失败: %s", "open update.rpf failed: %s"), gc_rpf_last_error());
        g_gc_msg_color = C_RED;
        return rc;
    }

    /* 找 gameconfig.xml（条目名不带路径，直接按文件名找） */
    rc = gc_rpf_find(&g_gc_rpf, "gameconfig.xml", &g_gc_ent);
    if (rc != GC_OK) {
        gc_msg(TR("在 update.rpf 里找不到 gameconfig.xml (共 %u 个条目)", "gameconfig.xml not found in update.rpf (%u entries)"), g_gc_rpf.entry_count);
        g_gc_msg_color = C_RED;
        gc_rpf_close(&g_gc_rpf); g_gc_err = rc; return rc;
    }

    /* 读内容 */
    u8 *buf = NULL; size_t blen = 0;
    rc = gc_rpf_read(&g_gc_rpf, &g_gc_ent, &buf, &blen);
    if (rc != GC_OK) {
        gc_msg(TR("解压 gameconfig.xml 失败 (rc=%d, on_disk=%u size=%u)", "inflate gameconfig.xml failed (rc=%d, on_disk=%u size=%u)"),
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
        gc_msg(TR("gameconfig.xml 里找不到 switch 段 (%d B) 开头: %s", "switch section start not found in gameconfig.xml (%d B): %s"),
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
    gc_msg(TR("已载入 gameconfig.xml (%d B, switch 段 %d 项)", "gameconfig.xml loaded (%d B, switch section %d items)"), (int)g_gc_xml_len, g_gc_n_switch);
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
    if (!g_gc_opened) { gc_msg(TR("请先载入 update.rpf", "Load update.rpf first")); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
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
    gc_msg(TR("已套用【%s】: 改 %d 项 / 新增 %d 项%s", "applied [%s]: %d changed / %d added%s"),
           g_lang_en && p->name_en ? p->name_en : p->name, n_applied, n_inserted, n_fail ? TR(" (部分失败)", " (partial failure)") : "");
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
        if (rc == GC_ERR_TOOBIG) { gc_msg(TR("缓冲区不足", "buffer too small")); g_gc_msg_color = C_RED; }
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
    if (!g_gc_opened) { gc_msg(TR("未打开 update.rpf", "update.rpf not open")); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
    if (!gc_rpf_writable(&g_gc_rpf)) {
        gc_msg(TR("update.rpf 是【只读】打开的 - 检查 SD 卡写保护或文件只读属性",
           "update.rpf is open READ-ONLY - check SD write protection or file attributes"));
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
    const char *how = TR("原样", "as-is");
    size_t zsz = 0;

    size_t bound = compressBound((uLong)blen) + 1024;
    u8 *probe = (u8*)malloc(bound);
    if (!probe) { gc_msg(TR("内存不足", "Out of memory")); g_gc_msg_color = C_RED; return GC_ERR_NOMEM; }

    zsz = gc_try_deflate(body, blen, probe, bound);
    if (zsz == 0 || zsz > cap_safe) {
        /* (2) 真删注释 */
        work = (u8*)malloc(blen + 1);
        if (work) {
            memcpy(work, g_gc_xml, blen);
            size_t nl = gc_drop_comments((char*)work, blen);
            size_t z2 = gc_try_deflate(work, nl, probe, bound);
            if (z2 > 0 && z2 <= cap_safe) {
                body = work; blen = nl; zsz = z2; how = TR("删注释", "strip comments");
            } else {
                /* (3) 再压空白 */
                size_t nl3 = gc_squeeze((char*)work, nl);
                size_t z3 = gc_try_deflate(work, nl3, probe, bound);
                if (z3 > 0 && z3 <= cap_safe) {
                    body = work; blen = nl3; zsz = z3; how = TR("删注释+压空白", "strip+squeeze");
                } else if (z2 > 0 && z2 <= cap) {
                    body = work; blen = nl; zsz = z2; how = TR("删注释(临界)", "strip (edge)");
                } else {
                    body = work; blen = nl; zsz = z2; how = TR("删注释(超限)", "strip (over)");
                }
            }
        }
    }
    free(probe);

    if (zsz == 0 || zsz > cap) {
        if (work) free(work);
        gc_msg(TR("压缩后 %zu B 超出物理可用空间 %u B", "compressed %zu B exceeds physical space %u B"), zsz, cap);
        g_gc_msg_color = C_RED;
        return GC_ERR_TOOBIG;
    }

    int rc = gc_rpf_write(&g_gc_rpf, &g_gc_ent, body, blen);
    if (rc != GC_OK) {
        if (work) free(work);
        gc_msg(rc == GC_ERR_TOOBIG ? TR("写入时容量不足", "out of space while writing")
             : rc == GC_ERR_READONLY ? TR("文件只读打开，无法写入", "file open read-only, cannot write")
             : TR("写入失败 (rc=%d) %s", "write failed (rc=%d) %s"), rc, gc_rpf_last_error());
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
            gc_msg(TR("校验失败, 已自动回滚原内容", "verify failed, auto-rolled back"));
        } else {
            gc_msg(TR("校验失败且无备份!", "verify failed and no backup!"));
        }
        g_gc_msg_color = C_RED;
        if (work) free(work);
        return rc;
    }

    if (work) free(work);
    g_gc_dirty = 0;
    gc_msg(TR("已写入 update.rpf (%zu B -> 压缩 %zu B, 手段=%s) 重启游戏生效",
           "written to update.rpf (%zu B -> compressed %zu B, method=%s) reboot to take effect"),
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
    if (!g_gc_opened) { gc_msg(TR("请先载入 update.rpf", "Load update.rpf first")); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }

    /* 套用原版预设（会把全部 52 项恢复成移植作者的原始值） */
    int rc = gc_apply_preset(GC_IDX_ORIGINAL);
    if (rc != GC_OK) {
        gc_msg(TR("恢复原版失败 (rc=%d)", "restore failed (rc=%d)"), rc);
        g_gc_msg_color = C_RED;
        return rc;
    }
    gc_msg(TR("已载入原版参数 (%d 项)，请点「写入rpf」落盘", "original params loaded (%d), press Write RPF to commit"),
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
    if (!g_gc_opened) { gc_msg(TR("请先载入 update.rpf", "Load update.rpf first")); g_gc_msg_color = C_RED; return GC_ERR_OPEN; }
    if (!g_gc_xml)    { gc_msg(TR("内存里没有 gameconfig 内容", "no gameconfig in memory")); g_gc_msg_color = C_RED; return GC_ERR_FORMAT; }

    GcRpf chk;
    int rc = gc_rpf_open(g_gc_rpf.path, &chk);
    if (rc != GC_OK) {
        gc_msg(TR("回读失败: 打不开 %s (%s)", "verify failed: cannot open %s (%s)"), g_gc_rpf.path, gc_rpf_last_error());
        g_gc_msg_color = C_RED;
        return rc;
    }
    GcEntry e2;
    rc = gc_rpf_find(&chk, "gameconfig.xml", &e2);
    if (rc != GC_OK) {
        gc_msg(TR("回读失败: 磁盘上找不到 gameconfig.xml (rc=%d)", "verify failed: gameconfig.xml not on disk (rc=%d)"), rc);
        gc_rpf_close(&chk); g_gc_msg_color = C_RED; return rc;
    }
    u8 *buf = NULL; size_t bl = 0;
    rc = gc_rpf_read(&chk, &e2, &buf, &bl);
    if (rc != GC_OK) {
        gc_msg(TR("回读失败: 解压失败 rc=%d (%s)", "verify failed: inflate rc=%d (%s)"), rc, gc_rpf_last_error());
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
                            (g_lang_en && GC_PRESETS[g_gc_preset].name_en ? GC_PRESETS[g_gc_preset].name_en : GC_PRESETS[g_gc_preset].name), pn);
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
        gc_msg(TR("回读校验: 已生效 (磁盘内容 == 编辑内容)", "verify: effective (disk == edited)"));
        g_gc_msg_color = C_GREEN;
        return GC_OK;
    } else if (n_diff == 0 && n_missing == 0) {
        gc_msg(TR("回读校验: %d 项值全部一致, 已生效", "verify: all %d values match, effective"), n_same);
        g_gc_msg_color = C_GREEN;
        return GC_OK;
    } else {
        gc_msg(TR("回读校验: 未生效 - %d 项不符, %d 项缺失", "verify: NOT effective - %d differ, %d missing"), n_diff, n_missing);
        g_gc_msg_color = C_RED;
        return GC_ERR_IO;
    }
}

#include "ui_adapter.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    romfsInit();
    detect_language();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    Framebuffer fb;
    if (R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT,
                                  PIXEL_FORMAT_RGBA_8888, 2))) {
        romfsExit(); return 1;
    }
    framebufferMakeLinear(&fb);
    g_dlc_prog_fb=&fb;
    init_shared_font();
    load_initial_save();
    ui_reset(&g_ui, UI_HOME, 0);
    modern_build();
    UiNavigation navigation={0};
    int adjust_dir=0, adjust_ticks=0;
    while(g_ui_running && appletMainLoop()) {
        padUpdate(&g_pad);
        u32 down=padGetButtonsDown(&g_pad), held=padGetButtons(&g_pad);
        unsigned dpad=0;
        if(held&HidNpadButton_Up)dpad|=UI_UP;
        if(held&HidNpadButton_Down)dpad|=UI_DOWN;
        if(held&HidNpadButton_Left)dpad|=UI_LEFT;
        if(held&HidNpadButton_Right)dpad|=UI_RIGHT;
        HidAnalogStickState left=padGetStickPos(&g_pad,0),right=padGetStickPos(&g_pad,1);
        unsigned keys=ui_navigation_step(&navigation,left.x,left.y,right.x,right.y,dpad,svcGetSystemTick()/19200ULL);
        if(down&HidNpadButton_A) keys|=UI_ACCEPT;
        if(down&HidNpadButton_B) keys|=UI_BACK;
        if(down&HidNpadButton_X) keys|=UI_ACTIONS;
        if(down&HidNpadButton_L) keys|=UI_PREV;
        if(down&HidNpadButton_R) keys|=UI_NEXT;
        touch_poll();
        int touched=g_touch.count>0 || g_ui_touch.active;
        UiTouchResult gesture=ui_touch(&g_ui_touch,&g_ui,&g_view,(int)g_touch.count,
            g_touch.count?(int)g_touch.touches[0].finger_id:-1,
            g_touch.count?(int)g_touch.touches[0].x:0,g_touch.count?(int)g_touch.touches[0].y:0);
        int touch_changed=gesture.changed;
        if(keys && g_touch.count) {g_ui_touch.active=-1;gesture.tap=0;}
        if(gesture.report_lines && g_ui_dialog==2) {
            g_ui_report_scroll+=gesture.report_lines;
            if(g_ui_report_scroll<0)g_ui_report_scroll=0;
            if(g_ui_report_scroll>g_ui_report_max)g_ui_report_scroll=g_ui_report_max;
        }
        if(gesture.tap) {
            int hi=hot_hit(gesture.x,gesture.y);
            if(hi>=0) {UiEvent e={g_hot[hi].action,g_hot[hi].param};modern_execute(e,0);touch_changed=1;}
        }
        if(!touched) {
            if(g_ui_dialog==2) {
                if(keys&UI_UP) {if(g_ui_report_scroll>0) g_ui_report_scroll--;}
                if(keys&UI_DOWN) {
                    if(g_ui_report_scroll<g_ui_report_max) g_ui_report_scroll++;
                }
            }
            UiEvent e=ui_input(&g_ui,&g_view,keys);
            modern_sync();modern_execute(e,0);
            if(!g_view.dialog && (down&HidNpadButton_Plus)) modern_execute(modern_event(U_EXIT,0),0);
        }
        int dir=(held&HidNpadButton_ZR)?1:(held&HidNpadButton_ZL)?-1:0;
        int adjust=0;
        if(dir!=adjust_dir) {adjust=dir;adjust_ticks=0;}
        else if(dir && ++adjust_ticks>=20 && adjust_ticks%5==0) adjust=dir;
        adjust_dir=dir;
        if(adjust && !touched && !g_ui_dialog && !g_view.dialog && g_ui.focus==UI_LIST) {
            if(g_ui.page==UI_GRAPHICS && g_ui.section==1) modern_execute(modern_event(U_GFX_STEP,adjust),0);
            else if(g_ui.page==UI_ADVANCED && g_ui.section==0) modern_execute(modern_event(U_PERF_STEP,adjust),0);
        }
        if(!g_ui_running) break;
        if(keys || touch_changed || adjust || down) modern_build();
        u32 stride=0;
        uint32_t *pixels=framebufferBegin(&fb,&stride);
        if(!pixels) continue;
        g_stride_words=stride/sizeof(uint32_t);
        hot_reset();
        modern_transition_update();
        UiCanvas canvas={pixels,modern_rect,modern_text,modern_measure,modern_hit};
        ui_render(&canvas,&g_ui,&g_view);
        framebufferEnd(&fb);
        if(!g_touch.count && modern_dlc_tick()) modern_build();
    }
    modern_dlc_invalidate();
    framebufferClose(&fb);
    if(g_gc_opened) gc_rpf_close(&g_gc_rpf);
    free(g_gc_xml);free(g_dlc_xml);free(g_dlc_etud);
    if(g_save_buf) free(g_save_buf);
    if(g_face) FT_Done_Face(g_face);
    if(g_ft) FT_Done_FreeType(g_ft);
    if(g_is_native_mount) fsdevUnmountDevice("save");
    romfsExit();plExit();
    return 0;
}
