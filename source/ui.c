#include "ui.h"
#include <stdio.h>
#include <string.h>

#define PANEL UI_RGB(19,28,37)
#define LINE UI_RGB(41,55,67)
#define SELECTED UI_RGB(35,60,61)
#define INK UI_RGB(18,41,35)
#define T(v,chinese,english) ((v)->en ? (english) : (chinese))

static UiEvent event(int a,int p) { UiEvent e={a,p}; return e; }
static void rect(const UiCanvas *c,int x,int y,int w,int h,uint32_t col) {
    if (w>0 && h>0) c->rect(c->ctx,x,y,w,h,col);
}
static void roundrect(const UiCanvas *c,int x,int y,int w,int h,int r,uint32_t col) {
    if (r>h/2) r=h/2;
    if (r>w/2) r=w/2;
    rect(c,x,y+r,w,h-2*r,col);
    for (int j=0;j<r;j++) {
        int d=r-j-1, inset=r;
        /* Find the horizontal circle extent using integer arithmetic. */
        int extent=0;
        while ((extent+1)*(extent+1)+d*d<r*r) extent++;
        inset=r-extent;
        rect(c,x+inset,y+j,w-2*inset,1,col);
        rect(c,x+inset,y+h-j-1,w-2*inset,1,col);
    }
}
static void text(const UiCanvas *c,int x,int y,int size,const char *s,uint32_t col) {
    if (s && *s) c->text(c->ctx,x,y,size,s,col);
}
static int next_char(const char *s) {
    unsigned char a=(unsigned char)*s;
    if (!a) return 0;
    int n=a<128?1:(a&224)==192?2:(a&240)==224?3:(a&248)==240?4:1;
    for (int k=1;k<n;k++) if (!s[k] || ((unsigned char)s[k]&192)!=128) return 1;
    return n;
}
/* Every external filename, translation and error message is width constrained. */
static void fit(const UiCanvas *c,int x,int y,int size,int width,const char *s,uint32_t col) {
    if (!s) return;
    if(!strchr(s,'\n') && c->measure(c->ctx,s,size)<=width) {text(c,x,y,size,s,col);return;}
    char b[768]; size_t n=0;int width_used=0;
    while (*s && *s!='\n' && n+8<sizeof(b)) {
        int k=next_char(s);char ch[5]={0};memcpy(ch,s,(size_t)k);
        int advance=c->measure(c->ctx,ch,size);
        if (width_used+advance>width-((s[k] && s[k]!='\n')?c->measure(c->ctx,"...",size):0)) break;
        memcpy(b+n,s,(size_t)k);n+=(size_t)k; s+=k;width_used+=advance;
    }
    if (*s && *s!='\n') { memcpy(b+n,"...",3); n+=3; }
    b[n]=0; text(c,x,y,size,b,col);
}
static const char *next_line(const UiCanvas *c,const char *s,char b[768],int size,int width) {
        size_t n=0,last_space=0;int width_used=0;const char *resume=NULL;
        while (*s && *s!='\n' && n+5<768) {
            int k=next_char(s);char ch[5]={0};memcpy(ch,s,(size_t)k);
            int advance=c->measure(c->ctx,ch,size);
            if (width_used+advance>width && n) {
                if(resume && last_space>=n/2) {n=last_space;s=resume;}
                break;
            }
            if(*s==' ') {last_space=n;resume=s+1;}
            memcpy(b+n,s,(size_t)k);n+=(size_t)k; s+=k;width_used+=advance;
        }
        b[n]=0;
        if (*s=='\n') s++;
        return s;
}
static void paragraph(const UiCanvas *c,int x,int y,int size,int width,int max_lines,
                      const char *s,uint32_t col,int skip) {
    if (!s) return;
    for (int line=0;*s && line<max_lines+skip;line++) {
        char b[768];s=next_line(c,s,b,size,width);
        if (line>=skip) text(c,x,y+(line-skip)*(size+9),size,b,col);
    }
}
int ui_report_max_scroll(const UiCanvas *c,const char *s) {
    if(!s) return 0;
    int lines=0;char b[768];
    while(*s) {s=next_line(c,s,b,17,842);lines++;}
    return lines>9?lines-9:0;
}
static void hit(const UiCanvas *c,int x,int y,int w,int h,int a,int p) {
    if (c->hit) c->hit(c->ctx,x,y,w,h,event(a,p));
}
static void button(const UiCanvas *c,int x,int y,int w,const char *label,int focused,
                   int primary,int disabled,int danger) {
    uint32_t bg=disabled?PANEL:primary?UI_ACCENT:focused?SELECTED:UI_SURFACE;
    if (focused && !disabled) roundrect(c,x-2,y-2,w+4,50,12,UI_ACCENT);
    roundrect(c,x,y,w,46,10,bg);
    int width=c->measure(c->ctx,label,17);
    fit(c,x+(width<w-24?(w-width)/2:12),y+12,17,w-24,label,
        disabled?UI_MUTED:primary?INK:danger?UI_ERROR:UI_TEXT);
}
/* Simple line icons are rendered natively, without extra fonts or assets. */
static void icon(const UiCanvas *c,int x,int y,int type,uint32_t col) {
    if (type==UI_HOME) {
        for(int i=0;i<2;i++) for(int j=0;j<2;j++) roundrect(c,x+i*12,y+j*12,9,9,2,col);
    } else if (type==UI_MODS) {
        rect(c,x+2,y+3,18,3,col); rect(c,x+2,y+10,13,3,col); rect(c,x+2,y+17,18,3,col);
        rect(c,x+17,y+8,3,7,col);
    } else if (type==UI_DLC) {
        rect(c,x+2,y+5,18,3,col); rect(c,x+2,y+17,18,3,col);
        rect(c,x+2,y+5,3,15,col); rect(c,x+17,y+5,3,15,col); rect(c,x+8,y+2,6,3,col);
    } else if (type==UI_GRAPHICS) {
        rect(c,x,y+2,22,3,col); rect(c,x,y+2,3,16,col); rect(c,x+19,y+2,3,16,col);
        rect(c,x,y+16,22,3,col); rect(c,x+10,y+19,3,4,col); rect(c,x+5,y+23,12,2,col);
    } else if (type==UI_SAVES) {
        rect(c,x+2,y+2,18,3,col); rect(c,x+2,y+2,3,20,col); rect(c,x+17,y+2,3,20,col);
        rect(c,x+2,y+19,18,3,col); rect(c,x+8,y+3,6,6,col); rect(c,x+7,y+14,8,3,col);
    } else if (type==UI_RADIO) {
        rect(c,x+2,y+7,20,3,col); rect(c,x+2,y+7,3,15,col);
        rect(c,x+19,y+7,3,15,col); rect(c,x+2,y+19,20,3,col);
        rect(c,x+6,y+2,13,2,col); rect(c,x+7,y+12,5,4,col);
        rect(c,x+15,y+12,3,3,col);
    } else {
        for(int i=0;i<3;i++) { rect(c,x,y+4+i*7,22,2,col); rect(c,x+4+i*5,y+1+i*7,3,8,col); }
    }
}
void ui_reset(UiState *s,int page,int section) {
    memset(s,0,sizeof(*s)); s->page=page; s->section=section;s->sidebar=page;
}
void ui_clamp(UiState *s,const UiView *v) {
    if (s->selected>=v->row_count) s->selected=v->row_count>0?v->row_count-1:0;
    if (s->selected<0) s->selected=0;
    if (s->button>=v->button_count) s->button=0;
    if (s->scroll>s->selected) s->scroll=s->selected;
    if (s->selected>=s->scroll+4) s->scroll=s->selected-3;
    int max_scroll=v->row_count>4?v->row_count-4:0;
    if(s->scroll>max_scroll) s->scroll=max_scroll;
    if(s->scroll<0) s->scroll=0;
    if (!v->row_count && s->focus==UI_LIST) s->focus=v->button_count?UI_BUTTONS:UI_SECTIONS;
    if(s->focus==UI_BUTTONS && v->button_count && v->buttons[s->button].disabled) {
        for(int i=1;i<v->button_count;i++) {
            int next=(s->button+i)%v->button_count;
            if(!v->buttons[next].disabled) {s->button=next;break;}
        }
    }
}
static int next_button(const UiView *v,int start,int dir) {
    for(int n=0;n<v->button_count;n++) {
        start=(start+v->button_count+dir)%v->button_count;
        if (!v->buttons[start].disabled) return start;
    }
    return 0;
}
static void sidebar_focus(UiState *s) {s->focus=UI_SIDEBAR;s->sidebar=s->page;}
UiEvent ui_input(UiState *s,const UiView *v,unsigned keys) {
    ui_clamp(s,v);
    if(v->dialog) {
        if(keys&UI_BACK) return event(UI_DIALOG_CANCEL,0);
        if(keys&UI_ACCEPT) return event(v->dialog==1?UI_DIALOG_OK:UI_DIALOG_CANCEL,0);
        return event(0,0);
    }
    if(keys&UI_BACK) {
        if(s->focus==UI_SIDEBAR) return event(UI_GO_PAGE,UI_HOME);
        sidebar_focus(s);return event(0,0);
    }
    if(keys&UI_PREV) return event(UI_GO_PAGE,(s->page+UI_PAGE_COUNT-1)%UI_PAGE_COUNT);
    if(keys&UI_NEXT) return event(UI_GO_PAGE,(s->page+1)%UI_PAGE_COUNT);
    if(s->focus==UI_SIDEBAR) {
        if(keys&UI_UP) s->sidebar=(s->sidebar+UI_PAGE_COUNT-1)%UI_PAGE_COUNT;
        else if(keys&UI_DOWN) s->sidebar=(s->sidebar+1)%UI_PAGE_COUNT;
        if(keys&(UI_RIGHT|UI_ACCEPT)) return event(UI_GO_PAGE,s->sidebar);
        return event(0,0);
    }
    if(keys&UI_ACTIONS) {
        s->focus=s->focus==UI_BUTTONS?(v->row_count?UI_LIST:UI_SECTIONS):UI_BUTTONS;
        if(v->button_count && v->buttons[s->button].disabled) s->button=next_button(v,s->button,1);
        return event(0,0);
    }
    if(s->page==UI_HOME) {
        if((keys&UI_LEFT) && !(s->selected&1)) {sidebar_focus(s);return event(0,0);}
        if(keys&UI_LEFT) s->selected&=~1;
        if(keys&UI_RIGHT) s->selected|=1;
        if(keys&UI_UP) s->selected=s->selected>=2?s->selected-2:s->selected;
        if(keys&UI_DOWN) s->selected=s->selected<2?s->selected+2:s->selected;
        if(keys&UI_ACCEPT) return v->rows[s->selected].event;
        return event(0,0);
    }
    if(s->focus==UI_SECTIONS) {
        if((keys&UI_LEFT) && s->section==0) {sidebar_focus(s);return event(0,0);}
        int dir=(keys&UI_LEFT)?-1:(keys&UI_RIGHT)?1:0;
        if(dir && v->section_count>1) return event(UI_GO_SECTION,(s->section+dir+v->section_count)%v->section_count);
        if(keys&(UI_DOWN|UI_ACCEPT)) s->focus=v->row_count?UI_LIST:UI_BUTTONS;
    } else if(s->focus==UI_BUTTONS) {
        if(keys&UI_LEFT) {
            int previous=0;for(int i=0;i<s->button;i++) if(!v->buttons[i].disabled) previous=1;
            if(!previous) {sidebar_focus(s);return event(0,0);}
        }
        if(keys&UI_LEFT) s->button=next_button(v,s->button,-1);
        if(keys&UI_RIGHT) s->button=next_button(v,s->button,1);
        if(keys&UI_UP) s->focus=v->row_count?UI_LIST:UI_SECTIONS;
        if(keys&UI_ACCEPT && v->button_count && !v->buttons[s->button].disabled) return v->buttons[s->button].event;
    } else {
        if(keys&UI_LEFT) {sidebar_focus(s);return event(0,0);}
        if(keys&UI_UP) { if(s->selected>0) s->selected--; else if(v->section_count>1) s->focus=UI_SECTIONS; }
        if(keys&UI_DOWN) { if(s->selected+1<v->row_count) s->selected++; else if(v->button_count) s->focus=UI_BUTTONS; }
        if(keys&UI_ACCEPT && v->row_count && !v->rows[s->selected].disabled) return v->rows[s->selected].event;
    }
    ui_clamp(s,v); return event(0,0);
}

/* One tap on release. A drag, long hold or extra finger never repeats actions. */
UiTouchResult ui_touch(UiTouch *t,UiState *s,const UiView *v,int count,int id,int x,int y) {
    UiTouchResult r={0};
    if(count==0) {
        if(t->active==1 && !t->dragged && t->page==v->page && t->section==v->section && t->dialog==v->dialog) {r.tap=1;r.x=t->start_x;r.y=t->start_y;}
        memset(t,0,sizeof(*t));return r;
    }
    if(count!=1 || t->active==-1) {t->active=-1;return r;}
    if(!t->active) {
        t->active=1;t->id=id;t->start_x=x;t->start_y=y;t->last_y=y;
        t->page=v->page;t->section=v->section;t->dialog=v->dialog;
        if(v->dialog==2 && x>=329 && x<1171 && y>=243 && y<520) t->area=2;
        else if(!v->dialog && v->page!=UI_HOME && x>=264 && x<915 && y>=234 && y<538) t->area=1;
        return r;
    }
    if(id!=t->id || t->page!=v->page || t->section!=v->section || t->dialog!=v->dialog) {t->active=-1;return r;}
    int dx=x-t->start_x,dy=y-t->start_y;
    if(dx>12 || dx<-12 || dy>12 || dy<-12) t->dragged=1;
    if(t->dragged && t->area && (dy>12 || dy<-12 || t->carry)) {
        t->carry+=t->last_y-y;
        int step=t->area==2?26:52,lines=t->carry/step;t->carry-=lines*step;
        if(t->area==2) {r.report_lines=lines;r.changed=lines!=0;}
        else if(lines && v->row_count>4) {
            int old=s->scroll,next=old+lines,max=v->row_count-4;
            if(next<0) next=0;
            if(next>max) next=max;
            s->scroll=next;s->selected+=next-old;s->focus=UI_LIST;
            ui_clamp(s,v);r.changed=old!=next;
        }
    }
    t->last_y=y;return r;
}

void ui_render(const UiCanvas *c,UiState *s,const UiView *v) {
    static const char *zh[]={"总览","脚本模组","DLC 管理","画质设置","存档与备份","电台","高级工具"};
    static const char *en[]={"Overview","Script mods","DLC library","Graphics","Saves & backups","Radio","Advanced"};
    ui_clamp(s,v);
    rect(c,0,0,1280,720,UI_BG); rect(c,0,0,228,720,PANEL); rect(c,227,0,1,720,LINE);
    roundrect(c,28,30,44,44,12,UI_ACCENT); text(c,41,35,27,"V",INK);
    text(c,86,31,19,"TOOLBOX",UI_TEXT); text(c,87,57,11,"GTA V / SWITCH",UI_MUTED);
    text(c,30,115,12,T(v,"工作空间","WORKSPACE"),UI_MUTED);
    for(int i=0;i<UI_PAGE_COUNT;i++) {
        int y=149+i*62+(i==UI_ADVANCED?26:0),active=i==s->page;
        if(i==UI_ADVANCED) rect(c,28,y-18,172,1,LINE);
        int focused=s->focus==UI_SIDEBAR && s->sidebar==i;
        if(focused) roundrect(c,14,y-2,200,53,12,UI_ACCENT);
        if(active || focused) roundrect(c,16,y,196,49,10,SELECTED);
        icon(c,32,y+13,i,active?UI_ACCENT:UI_MUTED);
        fit(c,68,y+14,v->en?15:17,138,v->en?en[i]:zh[i],active?UI_ACCENT:UI_MUTED);
        hit(c,16,y,196,49,UI_GO_PAGE,i);
    }
    text(c,30,617,13,"GTA5 SWITCH TOOLBOX",UI_MUTED);
    text(c,30,642,12,"v7.2.0",UI_MUTED);
    text(c,30,680,12,T(v,"L / R  切换页面","L / R  Switch page"),UI_MUTED);

    if(s->page==UI_HOME) {
        text(c,264,31,12,"YOUR LOS SANTOS, YOUR WAY",UI_ACCENT);
        text(c,264,68,36,T(v,"让洛圣都，按你的方式运行。","Make Los Santos yours."),UI_TEXT);
        text(c,264,125,18,T(v,"管理模组、调整画质、继续你的故事。","Manage mods, tune graphics, continue your story."),UI_MUTED);
        roundrect(c,264,174,980,69,12,SELECTED);
        text(c,286,187,17,T(v,"从下面选择一项，开始管理","Choose a workspace to get started"),UI_ACCENT);
        text(c,286,215,14,T(v,"写入 RPF 前，请先备份 update.rpf 与 update2.rpf。","Back up update.rpf and update2.rpf before editing archives."),UI_MUTED);
        for(int i=0;i<v->row_count && i<4;i++) {
            int x=264+(i%2)*502,y=269+(i/2)*149,sel=i==s->selected;
            if(sel && s->focus==UI_LIST) roundrect(c,x-2,y-2,482,135,16,UI_ACCENT);
            roundrect(c,x,y,478,131,14,sel?SELECTED:UI_SURFACE);
            icon(c,x+24,y+24,i+1,UI_ACCENT);
            fit(c,x+65,y+22,24,380,v->rows[i].title,UI_TEXT);
            fit(c,x+24,y+68,16,430,v->rows[i].subtitle,UI_MUTED);
            text(c,x+24,y+102,12,sel?T(v,"A  打开工作空间","A  Open workspace"):"GTA V / SWITCH",sel?UI_ACCENT:UI_MUTED);
            hit(c,x,y,478,131,UI_GO_PAGE,v->rows[i].event.param);
        }
        fit(c,264,594,15,980,v->subtitle,UI_MUTED);
    } else {
        text(c,264,31,12,"GTA V / WORKSPACE",UI_ACCENT);
        text(c,264,64,34,v->title,UI_TEXT);
        fit(c,264,116,17,980,v->subtitle,UI_MUTED);
        if(v->dirty) {
            roundrect(c,1094,70,150,32,8,SELECTED);
            fit(c,1107,77,14,128,T(v,"有待保存的修改","Unsaved changes"),UI_WARN);
        }
        for(int i=0;i<v->section_count;i++) {
            int x=264+i*218,sel=i==s->section;
            if(sel && s->focus==UI_SECTIONS) roundrect(c,x-2,167,214,46,10,UI_ACCENT);
            roundrect(c,x,169,210,42,9,sel?SELECTED:PANEL);
            fit(c,x+17,180,16,180,v->sections[i],sel?UI_ACCENT:UI_MUTED);
            hit(c,x,169,210,42,UI_GO_SECTION,i);
        }
        if(v->row_count) {
            for(int r=0;r<4;r++) {
                int i=s->scroll+r;if(i>=v->row_count) break;
                const UiRow *row=&v->rows[i];
                int y=234+r*78,sel=i==s->selected;
                if(sel && s->focus==UI_LIST) roundrect(c,262,y-2,642,74,12,UI_ACCENT);
                roundrect(c,264,y,638,70,10,sel?SELECTED:UI_SURFACE);
                fit(c,284,y+11,19,455,row->title,row->disabled?UI_MUTED:UI_TEXT);
                fit(c,284,y+41,14,594,row->subtitle,UI_MUTED);
                int bw=c->measure(c->ctx,row->badge,13)+20;if(bw>143) bw=143;
                roundrect(c,882-bw,y+10,bw,26,6,PANEL);
                fit(c,892-bw,y+16,13,bw-20,row->badge,row->tone?row->tone:UI_MUTED);
                hit(c,264,y,638,70,UI_SELECT_ROW,i);
            }
            /* Selection position is always visible, even for very long lists. */
            char pos[48];snprintf(pos,sizeof(pos),"%d / %d",s->selected+1,v->row_count);
            text(c,264,550,12,pos,UI_MUTED);
            if(v->row_count>4) {
                rect(c,910,234,3,304,LINE);
                int hh=304*4/v->row_count;if(hh<12) hh=12;
                int sy=234+(304-hh)*s->scroll/(v->row_count-4);
                rect(c,910,sy,3,hh,UI_ACCENT);
            }
            const UiRow *row=&v->rows[s->selected];
            roundrect(c,930,234,314,304,12,PANEL);
            text(c,950,252,12,T(v,"当前选择","SELECTION"),UI_ACCENT);
            paragraph(c,950,282,21,274,2,row->title,UI_TEXT,0);
            paragraph(c,950,350,15,274,5,row->note[0]?row->note:row->subtitle,UI_MUTED,0);
            fit(c,950,503,14,274,v->primary,UI_ACCENT);
        } else {
            roundrect(c,264,234,980,304,14,PANEL);
            icon(c,295,264,s->page,UI_ACCENT);
            text(c,338,263,24,v->empty_title,UI_TEXT);
            paragraph(c,295,323,18,890,3,v->empty_body,UI_MUTED,0);
            roundrect(c,291,435,922,57,9,UI_SURFACE);
            fit(c,309,455,15,882,v->empty_path,UI_ACCENT);
        }
        int gap=12,n=v->button_count;
        int bw=n?(980-gap*(n-1))/n:0;
        for(int i=0;i<n;i++) {
            const UiButton *b=&v->buttons[i];int x=264+i*(bw+gap);
            button(c,x,578,bw,b->label,s->focus==UI_BUTTONS && s->button==i,i==0,b->disabled,b->danger);
            if(!b->disabled) hit(c,x,578,bw,46,UI_RUN_BUTTON,i);
        }
    }
    rect(c,264,665,980,1,LINE);
    fit(c,264,637,14,980,v->status,v->status_tone?v->status_tone:UI_MUTED);
    text(c,264,682,14,T(v,"左 / B  侧栏    上下  选择    右 / A  打开    X  操作栏    触屏滑动列表","Left / B  Sidebar    Up / Down  Select    Right / A  Open    X  Actions    Swipe lists"),UI_MUTED);
    if(v->dialog) {
        /* A solid scrim also prevents visually reading the background as active. */
        rect(c,228,0,1052,720,UI_BG);
        roundrect(c,296,123,912,485,16,LINE);
        roundrect(c,297,124,910,483,15,PANEL);
        text(c,329,151,12,v->dialog==1?T(v,"请确认此次操作","REVIEW THIS CHANGE"):T(v,"详细信息","DETAILS"),UI_ACCENT);
        fit(c,329,185,28,842,v->dialog_title,UI_TEXT);
        paragraph(c,329,243,17,842,9,v->dialog_body,UI_MUTED,v->dialog_scroll);
        /* Modal input shields the entire screen, including the sidebar. */
        hit(c,0,0,1280,720,UI_NO_EVENT,0);
        if(v->dialog==1) {
            button(c,329,532,410,T(v,"B  取消","B  Cancel"),0,0,0,0);
            button(c,759,532,410,v->dialog_confirm?v->dialog_confirm:T(v,"A  确认执行","A  Confirm"),1,1,0,0);
            hit(c,329,532,410,46,UI_DIALOG_CANCEL,0);hit(c,759,532,410,46,UI_DIALOG_OK,0);
        } else {
            button(c,329,532,840,T(v,"A / B  关闭    上 / 下  滚动报告","A / B  Close    Up / Down  Scroll report"),0,1,0,0);
            hit(c,329,532,840,46,UI_DIALOG_CANCEL,0);
        }
    }
}
