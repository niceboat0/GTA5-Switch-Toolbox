/* ========================================================================= */
/* lang.h -- ★ v6.3: 双语支持（中文 / English）
 * ---------------------------------------------------------------------------
 * 用法：把原来的 "中文文案" 改成 TR("中文文案", "English text")
 *   draw_text(fb, x, y, s, TR("DLC管理", "DLC"), color);
 *
 * 语言检测：main() 里 detect_language() 读 Switch 系统语言，
 *   英语系（en-US/en-GB/...）⇒ g_lang_en=1，其余（含日语等）⇒ 中文。
 *   字体无需切换：简中共享字体自带完整 Latin 字形。
 *
 * 🚨 注意：
 *   1. 两个参数的 printf 格式串必须带【相同数量/顺序】的 % 占位符
 *   2. TR() 是宏，展开为三元表达式，可放在任何 const char* 出现的位置
 *   3. 库层（gc_rpf/gc_dlc/gc_script）的 err 诊断串暂不翻译（技术诊断，
 *      中英混排可接受；后续版本再补）
 * ========================================================================= */
#ifndef LANG_H
#define LANG_H

extern int g_lang_en;   /* 0=中文（默认） 1=English */

#define TR(zh, en) (g_lang_en ? (en) : (zh))

/* 读 Switch 系统语言置位 g_lang_en（set 服务，语言码 "en-*" ⇒ 英语） */
void detect_language(void);

#endif /* LANG_H */
