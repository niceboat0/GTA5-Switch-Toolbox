#include "ui_navigation.h"

#define STICK_PRESS 12000
#define STICK_RELEASE 8000
static unsigned stick_direction(unsigned previous,int x,int y) {
    int64_t ax=x,ay=y;
    if(ax<0)ax=-ax;
    if(ay<0)ay=-ay;
    int horizontal;
    /* Keep the current axis near diagonals instead of alternating each frame. */
    if((previous&(UI_UP|UI_DOWN)) && ay>=STICK_RELEASE && ax*4<ay*5)horizontal=0;
    else if((previous&(UI_LEFT|UI_RIGHT)) && ax>=STICK_RELEASE && ay*4<ax*5)horizontal=1;
    else horizontal=ax>ay;
    unsigned direction=horizontal?(x>0?UI_RIGHT:UI_LEFT):(y>0?UI_UP:UI_DOWN);
    int64_t magnitude=horizontal?ax:ay;
    return magnitude>=(direction==previous?STICK_RELEASE:STICK_PRESS)?direction:0;
}
static unsigned first_direction(unsigned dpad) {
    if(dpad&UI_UP)return UI_UP;
    if(dpad&UI_DOWN)return UI_DOWN;
    if(dpad&UI_LEFT)return UI_LEFT;
    return dpad&UI_RIGHT;
}
unsigned ui_navigation_step(UiNavigation *s,int lx,int ly,int rx,int ry,unsigned dpad,uint64_t now) {
    s->sticks[0]=stick_direction(s->sticks[0],lx,ly);
    s->sticks[1]=stick_direction(s->sticks[1],rx,ry);
    unsigned direction=first_direction(dpad);
    int source=direction?1:2;
    if(!direction)direction=s->sticks[0]?s->sticks[0]:s->sticks[1];
    if(!direction) {s->direction=0;s->source=0;s->repeat_at=0;return 0;}
    if(direction!=s->direction || source!=s->source) {
        s->direction=direction;s->source=source;s->repeat_at=now+(source==1?400:450);return direction;
    }
    if(now<s->repeat_at)return 0;
    /* No catch-up burst after a slow frame or archive operation. */
    s->repeat_at=now+(source==1?100:180);return direction;
}
