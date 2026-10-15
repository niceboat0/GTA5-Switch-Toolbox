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
    fixture(UI_RADIO,0);view.button_count=6;
    for(int i=0;i<6;i++) {view.buttons[i].label="Radio action";view.buttons[i].event=(UiEvent){3000,i};view.buttons[i].disabled=i<3;}
    ui_reset(&s,UI_RADIO,0);ui_input(&s,&view,0);assert(s.focus==UI_BUTTONS && s.button==3);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==3000 && e.param==3);
    ui_input(&s,&view,UI_RIGHT);e=ui_input(&s,&view,UI_ACCEPT);assert(e.param==4);
    e=ui_input(&s,&view,UI_NEXT);assert(e.action==UI_GO_PAGE && e.param==UI_ADVANCED);
    fixture(UI_MODS,5);ui_reset(&s,UI_MODS,0);s.selected=999;s.scroll=999;ui_clamp(&s,&view);assert(s.selected==4 && s.scroll==1);
    view.row_count=0;ui_clamp(&s,&view);assert(s.selected==0 && s.scroll==0);
    fixture(UI_DLC,40);ui_reset(&s,UI_DLC,0);s.selected=7;s.scroll=4;
    e=ui_input(&s,&view,UI_LEFT);assert(e.action==0 && s.focus==UI_SIDEBAR && s.sidebar==UI_DLC);
    ui_input(&s,&view,UI_DOWN);assert(s.sidebar==UI_GRAPHICS && s.selected==7 && s.scroll==4);
    e=ui_input(&s,&view,UI_RIGHT);assert(e.action==UI_GO_PAGE && e.param==UI_GRAPHICS);
    s.sidebar=0;ui_input(&s,&view,UI_UP);assert(s.sidebar==UI_ADVANCED);
    e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==UI_BROWSE_PAGE && e.param==UI_ADVANCED && s.focus==UI_SIDEBAR);
    s.sidebar=s.page;e=ui_input(&s,&view,UI_ACCEPT);assert(e.action==UI_BROWSE_PAGE && e.param==s.page && s.focus==UI_SIDEBAR);
    e=ui_input(&s,&view,UI_RIGHT);assert(!e.action && s.focus==UI_LIST && s.selected==7);
    s.focus=UI_SECTIONS;s.section=0;ui_input(&s,&view,UI_LEFT);assert(s.focus==UI_SIDEBAR);
    s.focus=UI_BUTTONS;s.button=0;ui_input(&s,&view,UI_LEFT);assert(s.focus==UI_SIDEBAR);
    s.focus=UI_LIST;ui_input(&s,&view,UI_BACK);assert(s.focus==UI_SIDEBAR);
    view.dialog=1;e=ui_input(&s,&view,UI_DOWN|UI_RIGHT);assert(!e.action && s.sidebar==UI_DLC);view.dialog=0;
    UiTouch t={0};UiTouchResult tr;
    s.focus=UI_LIST;s.scroll=0;s.selected=0;
    tr=ui_touch(&t,&s,&view,1,1,400,420);assert(!tr.tap);
    for(int i=0;i<60;i++) {tr=ui_touch(&t,&s,&view,1,1,400,420);assert(!tr.tap && !tr.changed);}
    tr=ui_touch(&t,&s,&view,1,1,400,260);assert(tr.changed && s.scroll==3 && s.selected==3);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);
    ui_touch(&t,&s,&view,1,1,400,260);tr=ui_touch(&t,&s,&view,1,1,400,520);assert(s.scroll==0);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);
    ui_touch(&t,&s,&view,1,1,400,300);ui_touch(&t,&s,&view,1,1,408,304);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(tr.tap && tr.x==400 && tr.y==300);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);
    ui_touch(&t,&s,&view,1,1,400,300);ui_touch(&t,&s,&view,2,1,400,300);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);
    ui_touch(&t,&s,&view,1,1,400,300);ui_touch(&t,&s,&view,1,2,400,300);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);
    ui_touch(&t,&s,&view,1,1,400,300);view.dialog=1;
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);view.dialog=2;
    ui_touch(&t,&s,&view,1,1,400,440);tr=ui_touch(&t,&s,&view,1,1,400,310);assert(tr.report_lines==5);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);view.dialog=1;
    ui_touch(&t,&s,&view,1,1,800,550);ui_touch(&t,&s,&view,1,1,800,350);
    tr=ui_touch(&t,&s,&view,0,0,0,0);assert(!tr.tap);view.dialog=0;
    UiCanvas c={NULL,rect,text,measure,hit};
    assert(ui_report_max_scroll(&c,"1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11")==2);
    assert(ui_report_max_scroll(&c,"Short report")==0);
    for(int page=0;page<UI_PAGE_COUNT;page++) for(int lang=0;lang<2;lang++) {
        fixture(page,page==UI_HOME?4:9);view.en=lang;ui_reset(&s,page,0);ui_render(&c,&s,&view);
        s.focus=UI_SIDEBAR;s.sidebar=(page+1)%UI_PAGE_COUNT;ui_render(&c,&s,&view);
        if(page) {view.row_count=0;ui_render(&c,&s,&view);}
    }
    fixture(UI_MODS,4);ui_reset(&s,UI_MODS,0);view.dialog=1;view.dialog_title="Confirm";view.dialog_body="A specific change to a specific target.";
    hits=blocked=0;ui_render(&c,&s,&view);assert(blocked && hits==blocked+2 && last_hit==UI_DIALOG_OK);
    view.dialog=2;hits=blocked=0;ui_render(&c,&s,&view);assert(hits==blocked+1 && last_hit==UI_DIALOG_CANCEL);
    puts("UI navigation, sidebar A focus retention, disabled actions, scrolling, bilingual layout and modal shielding passed.");
    return 0;
}
