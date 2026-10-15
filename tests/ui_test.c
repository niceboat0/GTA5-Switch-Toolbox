#include "../source/ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UiView view;
static int hits,blocked,last_hit;
static void rect(void *p,int x,int y,int w,int h,uint32_t c) {
    (void)p;(void)c;assert(x>=0 && y>=0 && w>=0 && h>=0 && x+w<=1280 && y+h<=720);
}
static int measure(void *p,const char *s,int z) {
    (void)p;int n=0;
    for(const unsigned char *q=(const unsigned char *)s;*q;q++) if((*q&192)!=128) n+=*q<128?(z+1)/2:z;
    return n;
}
static void text(void *p,int x,int y,int z,const char *s,uint32_t c) {
    (void)c;assert(x>=0 && y>=0 && y+z<=720);assert(x+measure(p,s,z)<=1280);
}
static void hit(void *p,int x,int y,int w,int h,UiEvent e) {
    (void)p;assert(x>=0 && y>=0 && x+w<=1280 && y+h<=720);
    hits++;if(e.action==UI_NO_EVENT) blocked=hits;last_hit=e.action;
}
static void fixture(int page,int rows) {
    memset(&view,0,sizeof(view));view.page=page;view.row_count=rows;view.section_count=3;view.button_count=3;
    view.title="Workspace";view.subtitle="Details";view.sections[0]="One";view.sections[1]="Two";view.sections[2]="Three";
    view.primary="A Review";view.empty_title="No files yet";view.empty_body="Add files to the folder, then rescan.";view.empty_path="/switch/gta5save/";
    for(int i=0;i<rows;i++) {snprintf(view.rows[i].title,sizeof(view.rows[i].title),"File %d",i);view.rows[i].event=(UiEvent){1000,i};}
    for(int i=0;i<3;i++) {view.buttons[i].label="Action";view.buttons[i].event=(UiEvent){2000,i};}
}
int main(void) {
    UiState s;UiEvent e;
    fixture(UI_MODS,200);ui_reset(&s,UI_MODS,0);
    for(int i=0;i<199;i++) ui_input(&s,&view,UI_DOWN);
    assert(s.selected==199 && s.scroll==196);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==1000 && e.param==199);
    ui_input(&s,&view,UI_DOWN);assert(s.focus==UI_BUTTONS);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==2000);
    ui_input(&s,&view,UI_UP);assert(s.focus==UI_LIST && s.selected==199);
    s.selected=0;s.scroll=0;ui_input(&s,&view,UI_UP);assert(s.focus==UI_SECTIONS);
    e=ui_input(&s,&view,UI_RIGHT);assert(e.action==UI_GO_SECTION && e.param==1);
    view.rows[0].disabled=1;s.focus=UI_LIST;e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==0);
    view.buttons[0].disabled=1;ui_input(&s,&view,UI_ACTIONS);assert(s.button==1);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.param==1);
    view.dialog=1;e=ui_input(&s,&view,UI_NEXT|UI_RIGHT|UI_ACTIONS);assert(e.action==0);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==UI_DIALOG_OK);
    e=ui_input(&s,&view,UI_BACK);assert(e.action==UI_DIALOG_CANCEL);
    view.dialog=2;e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==UI_DIALOG_CANCEL);
    fixture(UI_DLC,0);ui_reset(&s,UI_DLC,0);ui_input(&s,&view,0);assert(s.focus==UI_BUTTONS);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==2000);
    fixture(UI_HOME,4);ui_reset(&s,UI_HOME,0);ui_input(&s,&view,UI_RIGHT|UI_DOWN);assert(s.selected==3);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.param==3);
    fixture(UI_MODS,5);ui_reset(&s,UI_MODS,0);s.selected=999;s.scroll=999;ui_clamp(&s,&view);assert(s.selected==4 && s.scroll==4);
    view.row_count=0;ui_clamp(&s,&view);assert(s.selected==0 && s.scroll==0);
    UiCanvas c={NULL,rect,text,measure,hit};
    for(int page=0;page<UI_PAGE_COUNT;page++) for(int lang=0;lang<2;lang++) {
        fixture(page,page==UI_HOME?4:9);view.en=lang;ui_reset(&s,page,0);ui_render(&c,&s,&view);
        if(page) {view.row_count=0;ui_render(&c,&s,&view);}
    }
    fixture(UI_MODS,4);ui_reset(&s,UI_MODS,0);view.dialog=1;view.dialog_title="Confirm";view.dialog_body="A specific change to a specific target.";
    hits=blocked=0;ui_render(&c,&s,&view);assert(blocked && hits==blocked+2 && last_hit==UI_DIALOG_OK);
    view.dialog=2;hits=blocked=0;ui_render(&c,&s,&view);assert(hits==blocked+1 && last_hit==UI_DIALOG_CANCEL);
    puts("UI navigation, disabled actions, scrolling, bilingual layout and modal shielding passed.");
    return 0;
}
