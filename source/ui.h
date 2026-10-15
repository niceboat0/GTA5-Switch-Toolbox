#ifndef TOOLBOX_UI_H
#define TOOLBOX_UI_H

#include <stdint.h>

/* Platform independent presentation and navigation; no filesystem writes here. */
#define UI_MAX_ROWS 256
#define UI_MAX_BUTTONS 6
#define UI_PAGE_COUNT 7
#define UI_RGB(r,g,b) (0xff000000u | ((uint32_t)(b)<<16) | ((uint32_t)(g)<<8) | (r))
#define UI_BG UI_RGB(15,21,28)
#define UI_SURFACE UI_RGB(24,34,44)
#define UI_ACCENT UI_RGB(141,233,196)
#define UI_TEXT UI_RGB(238,244,245)
#define UI_MUTED UI_RGB(155,173,185)
#define UI_WARN UI_RGB(244,197,115)
#define UI_ERROR UI_RGB(255,147,147)

enum { UI_HOME, UI_MODS, UI_DLC, UI_GRAPHICS, UI_SAVES, UI_RADIO, UI_ADVANCED };
enum { UI_LIST, UI_SECTIONS, UI_BUTTONS, UI_SIDEBAR };
enum { UI_UP=1, UI_DOWN=2, UI_LEFT=4, UI_RIGHT=8, UI_ACCEPT=16,
       UI_ACTIONS=32, UI_PREV=64, UI_NEXT=128, UI_BACK=256 };
/* Reserved presentation events. Backend commands use positive values. */
enum { UI_NO_EVENT=0, UI_GO_PAGE=-1, UI_GO_SECTION=-2, UI_SELECT_ROW=-3,
       UI_RUN_ROW=-4, UI_RUN_BUTTON=-5, UI_DIALOG_OK=-6, UI_DIALOG_CANCEL=-7 };

typedef struct { int action, param; } UiEvent;
typedef struct {
    char title[128], subtitle[192], badge[48], note[384];
    uint32_t tone;
    UiEvent event;
    int disabled;
} UiRow;
typedef struct {
    const char *label;
    UiEvent event;
    int disabled, danger;
} UiButton;
typedef struct {
    int page, section, row_count, button_count, section_count, en, dirty;
    const char *title, *subtitle, *sections[3], *primary;
    const char *empty_title, *empty_body, *empty_path;
    const char *status;
    uint32_t status_tone;
    UiRow rows[UI_MAX_ROWS];
    UiButton buttons[UI_MAX_BUTTONS];
    int dialog; /* 1=confirmation, 2=read-only report */
    const char *dialog_title, *dialog_body, *dialog_confirm;
    int dialog_scroll;
} UiView;
typedef struct {
    int page, section, selected, scroll, focus, button, sidebar;
} UiState;

typedef struct {
    int active, id, start_x, start_y, last_y, dragged, area, carry;
    int page, section, dialog;
} UiTouch;
typedef struct { int tap, x, y, report_lines, changed; } UiTouchResult;
typedef struct {
    void *ctx;
    void (*rect)(void *,int,int,int,int,uint32_t);
    void (*text)(void *,int,int,int,const char *,uint32_t);
    int (*measure)(void *,const char *,int);
    void (*hit)(void *,int,int,int,int,UiEvent);
} UiCanvas;

void ui_reset(UiState *, int page, int section);
void ui_clamp(UiState *, const UiView *);
UiEvent ui_input(UiState *, const UiView *, unsigned keys);
UiTouchResult ui_touch(UiTouch *, UiState *, const UiView *, int count, int id, int x, int y);
void ui_render(const UiCanvas *, UiState *, const UiView *);
int ui_report_max_scroll(const UiCanvas *, const char *);

#endif
