#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H
#include <stdint.h>
#include "ui.h"
typedef struct {
    unsigned sticks[2],direction;
    int source;
    uint64_t repeat_at;
} UiNavigation;
/* Switch stick range is approximately -32768..32767. Emits UI direction bits. */
unsigned ui_navigation_step(UiNavigation *,int,int,int,int,unsigned,uint64_t);
#endif
