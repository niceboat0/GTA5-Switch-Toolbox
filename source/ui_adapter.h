/* Included once by main.c: maps the presentation model to existing operations.
 * Archive, save, conflict detection and read-back verification remain in their
 * existing backend functions. All mutating UI actions pass through one dialog. */
#include "ui.h"

enum {
    U_MOD_INSTALL=1000, U_MOD_RESTORE, U_SCRIPT_INSTALL, U_SCRIPT_REMOVE,
    U_SCRIPT_SCAN, U_SCRIPT_INFO, U_DLC_REFRESH, U_DLC_TOGGLE, U_DLC_IMPORT,
    U_DLC_DELETE, U_DLC_CONVERT, U_DLC_HD, U_DLC_CHECK, U_DLC_INFO,
    U_GFX_PRESET, U_GFX_IMPORT, U_GFX_SAVE, U_GFX_CHECK, U_GFX_STEP,
    U_SAVE_PRESET, U_SAVE_BACKUP, U_SAVE_SLOT, U_SAVE_WRITE, U_SAVE_REFRESH,
    U_PERF_LOAD, U_PERF_WRITE, U_PERF_RESTORE, U_PERF_CHECK, U_PERF_VERIFY,
    U_PERF_STEP, U_EXIT,
    U_RADIO_INSTALL, U_RADIO_RESTORE, U_RADIO_CHECK, U_RADIO_SCAN, U_RADIO_RECOVER, U_RADIO_HELP
};
static UiState g_ui;
static UiTouch g_ui_touch;
static u64 g_ui_transition_tick;
static int g_ui_transition_alpha=255,g_ui_transition_x;
static UiView g_view;
static int g_ui_running=1, g_ui_dialog=0, g_ui_report_scroll=0, g_ui_report_max=0;
static UiEvent g_ui_pending;
static char g_ui_dialog_title[160],g_ui_dialog_body[65536],g_ui_summary[256];
static const int UI_GFX_ORDER[]={2,4,3,5,0,1};

static UiEvent modern_event(int action,int param) { UiEvent e={action,param};return e; }
static int modern_measure(void *ctx,const char *str,int size);
static void modern_copy(char *out,size_t len,const char *s) { snprintf(out,len,"%s",s?s:""); }
static UiRow *modern_row(const char *title,const char *sub,const char *badge,
                         const char *note,int action,int param,uint32_t tone,int disabled) {
    if(g_view.row_count>=UI_MAX_ROWS) return NULL;
    UiRow *r=&g_view.rows[g_view.row_count++];
    modern_copy(r->title,sizeof(r->title),title);modern_copy(r->subtitle,sizeof(r->subtitle),sub);
    modern_copy(r->badge,sizeof(r->badge),badge);modern_copy(r->note,sizeof(r->note),note);
    r->event=modern_event(action,param);r->tone=tone;r->disabled=disabled;return r;
}
static void modern_button(const char *label,int action,int param,int disabled,int danger) {
    if(g_view.button_count>=UI_MAX_BUTTONS) return;
    UiButton *b=&g_view.buttons[g_view.button_count++];
    b->label=label;b->event=modern_event(action,param);b->disabled=disabled;b->danger=danger;
}
static void modern_dialog(int type,const char *title,const char *body,UiEvent pending) {
    g_ui_dialog=type;g_ui_pending=pending;g_ui_report_scroll=0;
    modern_copy(g_ui_dialog_title,sizeof(g_ui_dialog_title),title);
    modern_copy(g_ui_dialog_body,sizeof(g_ui_dialog_body),body);
    UiCanvas metrics={NULL,NULL,NULL,modern_measure,NULL};
    g_ui_report_max=ui_report_max_scroll(&metrics,g_ui_dialog_body);
    
}
static void modern_report(const char *title,const char *body) {
    modern_dialog(2,title,body,modern_event(0,0));
}
#include "ui_dlc.h"
#include "ui_radio.h"
static void modern_sync(void) {
    if(g_ui.page==UI_MODS && g_ui.section==1) g_script_sel=g_ui.selected;
    if(g_ui.page==UI_DLC) {
        g_dlc_view=g_ui.section;
        if(g_dlc_view) g_dlc_src_sel=g_ui.selected; else g_dlc_sel=g_ui.selected;
    }
    if(g_ui.page==UI_GRAPHICS && g_ui.section==1) g_gfx_cur_idx=g_ui.selected;
    if(g_ui.page==UI_ADVANCED && g_ui.section==0) g_gc_knob=g_ui.selected;
}
static void modern_scan_saves(void) {
    scan_backup_history();
    if(!g_save_dir[0] && !g_dirty) load_initial_save();
    if(g_save_dir[0]) scan_save_slots(g_save_dir);
    for(int i=0;i<g_num_slots;i++) if(strcmp(g_slots[i].fullpath,g_save_path)==0) {g_active_slot_idx=i;break;}
}
static void modern_enter(int page,int section) {
    if(page<0 || page>=UI_PAGE_COUNT) return;
    if(page!=g_ui.page) g_ui_transition_tick=svcGetSystemTick();
    ui_reset(&g_ui,page,section);
    if(page==UI_MODS) {g_tab=TAB_SCRIPT;g_dlc_msg[0]=0;script_scan();}
    else if(page==UI_DLC) {g_tab=TAB_DLC;modern_dlc_request(section,0);}
    else if(page==UI_GRAPHICS) {g_tab=TAB_GFX;if(!g_gfx_count) gfx_load();scan_switch_gfx_files();}
    else if(page==UI_SAVES) {g_tab=TAB_TOOL;modern_scan_saves();}
    else if(page==UI_RADIO) {g_tab=TAB_TOOL;modern_radio_scan(section);}
    else if(page==UI_ADVANCED) {g_tab=TAB_PERF;if(!g_gc_opened) gc_open_rpf();}
    else g_tab=TAB_TOOL;
    modern_sync();
}
static void modern_section(int section) {
    if(section<0 || section>=g_view.section_count) return;
    g_ui.section=section;g_ui.selected=0;g_ui.scroll=0;g_ui.button=0;g_ui.focus=UI_SECTIONS;
    if(g_ui.page==UI_DLC) modern_dlc_request(section,0);
    if(g_ui.page==UI_GRAPHICS && section==2) scan_switch_gfx_files();
    if(g_ui.page==UI_RADIO && section<2) modern_radio_scan(section);
    modern_sync();
}
static int modern_navigate(UiEvent e) {
    if(e.action!=UI_GO_PAGE && e.action!=UI_BROWSE_PAGE)return 0;
    if(e.param<0 || e.param>=UI_PAGE_COUNT)return 1;
    if(e.param!=g_ui.page || e.action==UI_GO_PAGE)modern_enter(e.param,0);
    if(e.action==UI_BROWSE_PAGE) {g_ui.focus=UI_SIDEBAR;g_ui.sidebar=e.param;}
    return 1;
}
static void modern_build(void) {
    memset(&g_view,0,sizeof(g_view));
    g_view.page=g_ui.page;g_view.section=g_ui.section;g_view.en=g_lang_en;
    g_view.status_tone=UI_MUTED;g_view.status="";
    g_view.empty_title=TR("暂无内容","Nothing here yet");
    g_view.empty_body=TR("添加文件后，选择操作栏中的重新扫描。","Add files, then choose Rescan in the action bar.");
    g_view.empty_path="sdmc:/switch/gta5save/";
    if(g_ui.page==UI_HOME) {
        modern_row(TR("脚本模组","Script mods"),TR("内置模组、外部脚本与官方恢复","Built-in mods, external scripts and stock recovery"),"","",UI_GO_PAGE,UI_MODS,0,0);
        modern_row(TR("DLC 管理","DLC library"),TR("导入扩展内容，查看注册状态","Import add-ons and manage registration"),"","",UI_GO_PAGE,UI_DLC,0,0);
        modern_row(TR("画质设置","Graphics"),TR("选择适合你的预设，或逐项调整","Choose a preset or fine-tune each setting"),"","",UI_GO_PAGE,UI_GRAPHICS,0,0);
        modern_row(TR("存档与备份","Saves & backups"),TR("继续不同进度，找回历史存档","Pick story progress or recover a previous save"),"","",UI_GO_PAGE,UI_SAVES,0,0);
        snprintf(g_ui_summary,sizeof(g_ui_summary),TR("当前玩家：%s    存档槽位：%d    高级参数与诊断位于左侧「高级工具」",
            "Player: %s    Save slots: %d    Parameters and diagnostics: Advanced"),
            g_user_name[0]?g_user_name:TR("未识别","Not detected"),g_num_slots);
        g_view.subtitle=g_ui_summary;
    } else if(g_ui.page==UI_MODS) {
        g_view.title=TR("脚本模组","Script mods");g_view.subtitle=TR("选择模组，查看说明，确认后安装。","Choose a mod, review its details, then confirm installation.");
        g_view.section_count=3;g_view.sections[0]=TR("内置模组","Built-in mods");g_view.sections[1]=TR("外部脚本","External scripts");g_view.sections[2]=TR("恢复官方","Restore stock");
        g_view.primary=g_ui.section==2?TR("A  查看恢复确认","A  Review restoration"):TR("A  查看安装确认","A  Review installation");
        if(g_ui.section==0) {
            for(int i=0;i<gc_script_builtin_count();i++) {
                const GcBuiltinMod *m=gc_script_builtin(i);char sub[192],note[384];
                snprintf(sub,sizeof(sub),TR("作者 %s  /  %d 个脚本文件","By %s  /  %d script files"),m->author,m->n_files);
                snprintf(note,sizeof(note),"%s\n%s",MOD_NOTE(m),TR("安装会覆盖同名脚本。共用槽位的模组会被替换，写入后校验，完成后重启游戏。","Installation replaces same-name scripts. Mods sharing the same slot are replaced; writes are verified. Restart the game afterwards."));
                modern_row(MOD_TITLE(m),sub,TR("已内置","Bundled"),note,U_MOD_INSTALL,i,UI_ACCENT,0);
            }
        } else if(g_ui.section==1) {
            for(int i=0;i<g_script_count;i++) {
                ScriptItem *it=&g_script_items[i];char sub[192],size[32];fmt_size(size,sizeof(size),it->size);
                snprintf(sub,sizeof(sub),"%s  /  %s",size,it->from_alt?"/switch/gta5save/":"/switch/gta5save/script/");
                int invalid=!script_file_usable(it->size);
                modern_row(it->name,sub,invalid?TR("文件无效","Invalid"):g_script_rpf_n<0?TR("状态未知","Unknown"):it->installed?TR("已安装","Installed"):TR("待安装","Ready"),
                    TR("文件名必须与目标脚本名一致。安装将覆盖当前同名条目；需要恢复时，切到「恢复官方」。","Keep the original script filename. Installation replaces the matching archive entry. Use Restore stock to recover bundled originals."),U_SCRIPT_INSTALL,i,invalid?UI_ERROR:it->installed?UI_ACCENT:UI_MUTED,invalid);
            }
        } else {
            for(int i=0;i<gc_script_stock_count();i++) modern_row(gc_script_stock_name(i),TR("用内置官方原版覆盖这一条脚本","Replace this entry with its bundled official original"),"RSC7",
                TR("只恢复所选脚本，不等于恢复整个 update2.rpf。其他改动仍然保留。","Restores only the selected script. This does not restore the entire update2.rpf archive or its other modifications."),U_MOD_RESTORE,i,UI_ACCENT,0);
        }
        g_view.empty_title=TR("还没有外部脚本","No external scripts yet");
        g_view.empty_body=TR("把 .nsc 文件放入下方目录，再选择「重新扫描」。也可以切到「内置模组」直接安装。","Place .nsc files in the folder below, then choose Rescan. Built-in mods are available without copying files.");
        g_view.empty_path="sdmc:/switch/gta5save/script/";
        modern_button(g_ui.section==2?TR("恢复所选","Restore selected"):TR("安装所选","Install selected"),UI_RUN_ROW,0,!g_view.row_count || g_view.rows[g_ui.selected<g_view.row_count?g_ui.selected:0].disabled,0);
        modern_button(TR("重新扫描","Rescan"),U_SCRIPT_SCAN,0,0,0);
        modern_button(TR("目录与状态","Folder & status"),U_SCRIPT_INFO,0,0,0);
        if(g_ui.section==1) modern_button(TR("移除所选","Remove selected"),U_SCRIPT_REMOVE,0,!g_script_count || g_script_rpf_n<0 || !g_script_items[g_ui.selected<g_script_count?g_ui.selected:0].installed,1);
        g_view.status=g_dlc_msg[0]?g_dlc_msg:TR("内置模组无需拷文件；写入完成后重启游戏。","Built-in mods need no file copying. Restart the game after installation.");
        g_view.status_tone=g_dlc_msg[0]?(g_dlc_msg_ok?UI_ACCENT:UI_ERROR):UI_MUTED;
    } else if(g_ui.page==UI_DLC) {
        g_view.title=TR("DLC 管理","DLC library");g_view.subtitle=TR("扩展内容的安装、注册与检查，都在这里。","Install, register and inspect your add-on content.");
        g_view.section_count=2;g_view.sections[0]=TR("已安装","Installed");g_view.sections[1]=TR("待导入","Ready to import");
        g_view.primary=g_ui.section?TR("A  查看导入确认","A  Review import"):TR("A  注册 / 注销","A  Register / unregister");
        int count=g_ui.section?g_dlc_src_count:g_dlc_count;
        for(int i=0;i<count;i++) {
            DlcItem *it=g_ui.section?&g_dlc_src[i]:&g_dlc_items[i];char sub[192],note[384],size[32];fmt_size(size,sizeof(size),it->rpf_size);
            const char *badge=!it->has_rpf?TR("缺少 RPF","Missing RPF"):it->fmt==-2?TR("正在读取","Loading"):it->fmt==0?TR("PC 格式","PC format"):it->fmt<0?TR("无法读取","Unreadable"):it->map_st==GC_MAP_BAD?TR("地图异常","Map issue"):g_ui.section?TR("待导入","Ready"):it->full?TR("已注册","Registered"):TR("未完整注册","Incomplete");
            if(it->fmt==-2)snprintf(sub,sizeof(sub),TR("%s  /  资源信息按需加载","%s  /  Resource details load on demand"),size);
            else snprintf(sub,sizeof(sub),TR("%s  /  模型 %d  地图 %d  贴图 %d","%s  /  Models %d  Maps %d  Textures %d"),size,it->n_model,it->n_map,it->n_tex);
            snprintf(note,sizeof(note),TR("目录：%s\n刷车名：%s\n%s","Folder: %s\nSpawn name: %s\n%s"),it->name,it->spawn[0]?it->spawn:"-",
                it->fmt==0?TR("PC 格式：待导入包按 A 确认后自动转换并导入；已安装包可用「转换格式」。","PC format: pending packs convert automatically after A and confirmation. Installed packs use Convert format."):TR("注册会同时更新两处游戏配置。建议每次少量添加，再重启游戏验证。","Registration updates both required game configurations. Add a few packs at a time and restart to verify."));
            modern_row(it->cn[0]?it->cn:it->name,sub,badge,note,g_ui.section?U_DLC_IMPORT:U_DLC_TOGGLE,i,
                !it->has_rpf||it->fmt!=1||it->map_st==GC_MAP_BAD?UI_WARN:it->full?UI_ACCENT:UI_MUTED,
                modern_dlc_busy() || !g_dlc_xml_loaded || !it->has_rpf || it->fmt<0);
        }
        g_view.empty_title=modern_dlc_busy()?TR("正在读取 DLC 目录…","Loading DLC folders…"):g_ui.section?TR("没有待导入的 DLC","No DLC ready to import"):TR("还没有安装 DLC","No installed DLC");
        g_view.empty_body=TR("将包含 dlc.rpf 的文件夹放到下方目录。重新扫描后，在「待导入」中选择要安装的内容。","Place folders containing dlc.rpf in the directory below. Rescan, then choose a pack in Ready to import.");
        g_view.empty_path="sdmc:/switch/gta5save/dlc/";
        modern_button(g_ui.section?TR("导入所选","Import selected"):TR("注册 / 注销","Toggle registration"),UI_RUN_ROW,0,!count || !g_dlc_xml_loaded || g_view.rows[g_ui.selected<g_view.row_count?g_ui.selected:0].disabled,0);
        modern_button(TR("重新扫描","Rescan"),U_DLC_REFRESH,0,0,0);
        modern_button(TR("检查资源","Inspect"),U_DLC_CHECK,0,!count || modern_dlc_busy(),0);
        if(!g_ui.section) {
            modern_button(TR("转换格式","Convert format"),U_DLC_CONVERT,0,!count || modern_dlc_busy(),0);
            modern_button(TR("屏蔽高清贴图","Hide HD textures"),U_DLC_HD,0,!count || modern_dlc_busy(),0);
            modern_button(TR("删除 DLC","Delete DLC"),U_DLC_DELETE,0,!count || modern_dlc_busy() || !g_dlc_xml_loaded,1);
        } else {modern_button(TR("目录说明","Folders"),U_DLC_INFO,0,0,0);modern_button(TR("安装目录","Installed folder"),UI_GO_SECTION,0,0,0);}
        g_view.status=g_dlc_msg;g_view.status_tone=g_dlc_msg_ok?UI_ACCENT:UI_ERROR;
    } else if(g_ui.page==UI_GRAPHICS) {
        g_view.title=TR("画质设置","Graphics");g_view.subtitle=TR("先选预设；需要时，再逐项微调。","Start with a preset. Fine-tune only when you need to.");
        g_view.section_count=3;g_view.sections[0]=TR("画质预设","Presets");g_view.sections[1]=TR("逐项调整","Fine-tune");g_view.sections[2]=TR("导入配置","Import XML");
        g_view.primary=g_ui.section==1?TR("ZL / ZR  调整数值","ZL / ZR  Adjust value"):TR("A  查看应用确认","A  Review application");g_view.dirty=g_gfx_dirty;
        if(g_ui.section==0) for(int i=0;i<(int)NUM_GFX_PRESETS;i++) {
            int p=UI_GFX_ORDER[i];const GfxPreset *gp=g_lang_en?&GFX_PRESETS_EN[p]:&GFX_PRESETS[p];
            modern_row(gp->name,gp->desc,p==2?TR("默频适用","Stock clocks"):p==5?TR("需超频","Overclock"):TR("预设","Preset"),gp->desc,U_GFX_PRESET,p,p==5?UI_WARN:UI_ACCENT,0);
        } else if(g_ui.section==1) for(int i=0;i<g_gfx_count;i++) {
            GfxItem *it=&g_gfx_items[i];modern_row(g_lang_en?it->name:it->cn,it->name,it->value,
                TR("用 ZL / ZR 或操作栏的减小 / 增大调整。修改保留在内存中，选择「保存调整」后写入。","Use ZL / ZR or Decrease / Increase. Changes stay in memory until you choose Save changes."),U_GFX_STEP,1,UI_ACCENT,0);
        } else for(int i=0;i<g_gfx_switch_count;i++) modern_row(g_gfx_switch_files[i],"/switch/gta5save/","XML",
            TR("导入会应用整个 XML 文件，并写入游戏配置路径。","Import applies the complete XML file to the game configuration paths."),U_GFX_IMPORT,i,UI_ACCENT,0);
        g_view.empty_title=TR("没有找到配置文件","No configuration found");g_view.empty_body=TR("可以先应用内置预设，或把 XML 配置放到下方目录，再重新扫描。","Apply a built-in preset, or put an XML file in the directory below and rescan.");
        modern_button(g_ui.section==1?TR("保存调整","Save changes"):TR("应用所选","Apply selected"),g_ui.section==1?U_GFX_SAVE:UI_RUN_ROW,0,!g_view.row_count || (g_ui.section==1 && !g_gfx_dirty),0);
        if(g_ui.section==1) {modern_button(TR("减小数值","Decrease"),U_GFX_STEP,-1,!g_gfx_count,0);modern_button(TR("增大数值","Increase"),U_GFX_STEP,1,!g_gfx_count,0);}
        modern_button(TR("路径检查","Check paths"),U_GFX_CHECK,0,0,0);
        modern_button(TR("重新扫描","Rescan"),U_SAVE_REFRESH,0,0,0);
        g_view.status=g_status_msg;g_view.status_tone=g_status_color;
    } else if(g_ui.page==UI_SAVES) {
        g_view.title=TR("存档与备份","Saves & backups");g_view.subtitle=TR("选择故事进度，恢复备份，或切换目标槽位。","Choose story progress, restore a backup or change the target slot.");
        g_view.section_count=3;g_view.sections[0]=TR("进度存档","Story saves");g_view.sections[1]=TR("历史备份","Backups");g_view.sections[2]=TR("目标槽位","Target slot");
        g_view.primary=g_ui.section==2?TR("A  切换目标槽位","A  Switch target slot"):TR("A  查看写入确认","A  Review save replacement");g_view.dirty=g_dirty;
        int no_target=!g_save_path[0] || !g_is_plain || !g_save_buf;
        if(g_ui.section==0) for(int i=0;i<(int)NUM_PRESETS;i++) modern_row(PRESET_SAVE(i).name,PRESET_SAVE(i).desc,TR("内置","Bundled"),
            TR("确认后会载入并写入当前目标槽位。保存前使用现有自动备份流程。请先确认目标槽位正确。","Confirmation loads and writes this save to the current target slot using the existing backup-before-save flow. Check the target slot first."),U_SAVE_PRESET,i,UI_ACCENT,no_target);
        else if(g_ui.section==1) for(int i=0;i<g_num_backups;i++) modern_row(g_backups[i].title[0]?g_backups[i].title:g_backups[i].dir_name,g_backups[i].dir_name,TR("备份","Backup"),g_backups[i].note,U_SAVE_BACKUP,i,UI_ACCENT,no_target);
        else for(int i=0;i<g_num_slots;i++) modern_row(g_slots[i].filename,g_slots[i].title,i==g_active_slot_idx?TR("当前目标","Current"):TR("可切换","Available"),
            TR("切换槽位只改变后续存档写入的目标，不会立即覆盖游戏存档。","Switching slots changes the target of future save writes. It does not immediately overwrite a game save."),U_SAVE_SLOT,i,UI_ACCENT,0);
        g_view.empty_title=TR("没有找到存档或备份","No saves or backups found");g_view.empty_body=TR("先在游戏中保存一次，再重新启动工具。历史备份会在写入存档时创建。","Save once in the game, then restart this tool. Save writes create historical backups.");
        g_view.empty_path=g_save_path[0]?g_save_path:"sdmc:/switch/gta5save/";
        modern_button(g_ui.section==2?TR("切换槽位","Switch slot"):TR("写入所选","Write selected"),UI_RUN_ROW,0,!g_view.row_count || (g_ui.section!=2 && no_target),0);
        modern_button(TR("刷新列表","Refresh"),U_SAVE_REFRESH,0,0,0);
        modern_button(TR("保存待写入修改","Save pending"),U_SAVE_WRITE,0,!g_dirty || no_target,0);
        const char *fn=strrchr(g_save_path,'/');fn=fn?fn+1:g_save_path;
        snprintf(g_ui_summary,sizeof(g_ui_summary),TR("目标槽位：%s  /  %s","Target slot: %s  /  %s"),fn[0]?fn:TR("未找到","Not found"),g_status_msg);
        g_view.status=g_ui_summary;g_view.status_tone=no_target?UI_ERROR:g_status_color;
    } else if(g_ui.page==UI_RADIO) {
        modern_radio_build();
    } else {
        g_view.title=TR("高级工具","Advanced tools");g_view.subtitle=TR("gameconfig 参数与诊断。参数调整不会自动提升硬件性能。","Gameconfig parameters and diagnostics. Changes cannot remove hardware limits.");
        g_view.section_count=2;g_view.sections[0]=TR("参数调整","Parameters");g_view.sections[1]=TR("维护诊断","Diagnostics");
        g_view.primary=g_ui.section?TR("A  执行所选操作","A  Run selected action"):TR("ZL / ZR  调整数值","ZL / ZR  Adjust value");g_view.dirty=g_gc_dirty;
        if(g_gc_opened && !g_ui.section) for(int i=0;i<GC_NUM_KNOB_META;i++) {
            const GcKnobMeta *m=&GC_KNOB_META[i];int cur=gc_get_edited(m->key),edited=cur!=INT32_MIN;char sub[192],value[48];
            if(!edited && gc_xml_get_int((char*)g_gc_xml,g_gc_xml_len,g_gc_sw_a,g_gc_sw_b,m->key,&cur)!=GC_OK && gc_xml_get_int((char*)g_gc_xml,g_gc_xml_len,g_gc_any_a,g_gc_any_b,m->key,&cur)!=GC_OK) cur=0;
            snprintf(sub,sizeof(sub),"%s  /  %d ~ %d",m->key,m->lo,m->hi);snprintf(value,sizeof(value),"%d%s",cur,edited?" *":"");
            modern_row(g_lang_en?(m->cn_en?m->cn_en:m->cn):m->cn,sub,value,
                TR("ZL / ZR 调整数值。用 X 进入操作栏保存，或恢复原版参数。写入会先确认，之后可回读校验。","Use ZL / ZR to adjust. X opens the action bar for saving or restoring defaults. Writes require confirmation and can be verified afterwards."),U_PERF_STEP,1,edited?UI_WARN:UI_ACCENT,0);
        }
        g_view.empty_title=TR("无法读取 update.rpf","Could not read update.rpf");g_view.empty_body=TR("检查游戏文件是否位于正确目录。选择「路径自检」查看具体原因。","Check the game file location. Choose Diagnose paths to see the details.");
        g_view.empty_path="/atmosphere/contents/0100b00b51230000/romfs/update/";
        if(!g_ui.section) {
            modern_button(g_gc_opened?TR("写入参数","Write changes"):TR("载入文件","Load file"),g_gc_opened?U_PERF_WRITE:U_PERF_LOAD,0,g_gc_opened && !g_gc_dirty,0);
            modern_button(TR("减小数值","Decrease"),U_PERF_STEP,-1,!g_gc_opened,0);
            modern_button(TR("增大数值","Increase"),U_PERF_STEP,1,!g_gc_opened,0);
            modern_button(TR("恢复原版","Restore stock"),U_PERF_RESTORE,0,!g_gc_opened,0);
            modern_button(TR("路径自检","Diagnose paths"),U_PERF_CHECK,0,0,0);
        } else {
            modern_row(TR("路径自检","Diagnose paths"),TR("检查候选游戏目录与 RPF 文件","Inspect candidate game folders and RPF archives"),TR("只读","Read-only"),"",U_PERF_CHECK,0,UI_ACCENT,0);
            modern_row(TR("回读校验","Verify on disk"),TR("比较当前编辑内容与磁盘实际数据","Compare edited parameters with actual disk contents"),TR("只读","Read-only"),"",U_PERF_VERIFY,0,UI_ACCENT,!g_gc_opened);
            modern_row(TR("重新载入","Reload parameters"),TR("重新从 update.rpf 读取参数","Read parameters from update.rpf again"),TR("读取","Read"),"",U_PERF_LOAD,0,UI_ACCENT,0);
            modern_button(TR("执行所选","Run selected"),UI_RUN_ROW,0,g_view.rows[g_ui.selected<g_view.row_count?g_ui.selected:0].disabled,0);
        }
        g_view.status=g_gc_msg;g_view.status_tone=g_gc_msg_color;
    }
    ui_clamp(&g_ui,&g_view);modern_sync();
    g_view.dialog=g_ui_dialog;g_view.dialog_title=g_ui_dialog_title;g_view.dialog_body=g_ui_dialog_body;g_view.dialog_scroll=g_ui_report_scroll;
}

static void modern_execute(UiEvent e,int confirmed) {
    modern_sync();
    if(g_ui_dialog && e.action!=UI_DIALOG_OK && e.action!=UI_DIALOG_CANCEL) return;
    if(e.action==UI_DIALOG_OK && g_ui_dialog!=1) return;
    if(e.action==UI_NO_EVENT) return;
    if(modern_navigate(e))return;
    if(e.action==UI_GO_SECTION) {modern_section(e.param);return;}
    if(e.action==UI_SELECT_ROW) {
        if(e.param>=0 && e.param<g_view.row_count) {g_ui.selected=e.param;g_ui.focus=UI_LIST;modern_sync();}
        return;
    }
    if(e.action==UI_RUN_BUTTON) {
        if(e.param<0 || e.param>=g_view.button_count || g_view.buttons[e.param].disabled) return;
        g_ui.button=e.param;g_ui.focus=UI_BUTTONS;e=g_view.buttons[e.param].event;
    }
    if(e.action==UI_RUN_ROW) {
        if(!g_view.row_count || g_view.rows[g_ui.selected].disabled) return;
        e=g_view.rows[g_ui.selected].event;
    }
    if(modern_navigate(e))return;
    if(e.action==UI_GO_SECTION) {modern_section(e.param);return;}
    if(e.action==UI_DIALOG_CANCEL) {g_ui_dialog=0;return;}
    if(e.action==UI_DIALOG_OK) {e=g_ui_pending;g_ui_dialog=0;confirmed=1;}
    if(e.action>=U_RADIO_INSTALL && e.action<=U_RADIO_HELP) {modern_radio_execute(e,confirmed);return;}
    int mutates=e.action==U_MOD_INSTALL||e.action==U_MOD_RESTORE||e.action==U_SCRIPT_INSTALL||e.action==U_SCRIPT_REMOVE||
        e.action==U_DLC_TOGGLE||e.action==U_DLC_IMPORT||e.action==U_DLC_DELETE||e.action==U_DLC_CONVERT||e.action==U_DLC_HD||
        e.action==U_GFX_PRESET||e.action==U_GFX_IMPORT||e.action==U_GFX_SAVE||e.action==U_SAVE_PRESET||e.action==U_SAVE_BACKUP||
        e.action==U_SAVE_WRITE||e.action==U_PERF_WRITE||e.action==U_PERF_RESTORE;
    if(!confirmed && (mutates || (e.action==U_EXIT && (g_dirty||g_gfx_dirty||g_gc_dirty)))) {
        char body[2048];const char *name=g_view.row_count?g_view.rows[g_ui.selected].title:"";
        const char *effect=TR("这会覆盖游戏中的对应文件。操作完成前请勿关闭工具。","This replaces the corresponding game files. Keep the tool open until it finishes.");
        const char *target=g_ui.page==UI_MODS?"update2.rpf / script_rel.rpf":g_ui.page==UI_DLC?"update.rpf / dlcpacks":g_ui.page==UI_GRAPHICS?"settings.xml":g_ui.page==UI_SAVES?g_save_path:"update.rpf / gameconfig.xml";
        if(e.action==U_SCRIPT_REMOVE) effect=TR("删除所选脚本条目。该操作不会自动恢复官方原版。","Removes the selected script entry. This does not automatically restore the official original.");
        if(e.action==U_DLC_DELETE) effect=TR("注销并删除所选 DLC 文件夹。删除后需要重新复制文件才能找回。","Unregisters and deletes the selected DLC folder. Copy the original files again to recover it.");
        if(e.action==U_DLC_IMPORT) effect=TR("将所选包复制到安装目录；PC 格式自动转换为 Switch 格式，校验成功后注册。原始包保持不变。",
            "Copies the selected pack to the installed folder, converts PC format to Switch, and registers only after validation. The source pack is unchanged.");
        if(e.action==U_DLC_CONVERT) effect=TR("将已安装的包就地转换为 Switch 格式。请保留一份原始 DLC 备份。","Converts the installed pack to Switch format in place. Keep a copy of the original DLC.");
        if(e.action==U_PERF_RESTORE) effect=TR("恢复原版参数并写入 update.rpf。当前未写入的参数调整会被替换。","Restores original parameters and writes update.rpf, replacing pending parameter edits.");
        if(e.action==U_SAVE_PRESET||e.action==U_SAVE_BACKUP) effect=TR("载入所选存档并写入下方目标槽位，写入前执行现有自动备份流程。","Loads the selected save and writes the target slot below using the existing backup-before-save flow.");
        if(e.action==U_EXIT) effect=TR("仍有未写入的修改。确认退出会放弃这些修改；取消后可以继续保存。","There are unsaved changes. Confirming exits without writing them. Cancel to continue editing and save.");
        snprintf(body,sizeof(body),"%s\n%s: %s\n\n%s\n\n%s",name,TR("目标","Target"),target,effect,
            g_ui.page==UI_MODS||g_ui.page==UI_DLC||g_ui.page==UI_ADVANCED?TR("请先保留完整 RPF 备份。完成后重启游戏生效。","Keep a complete RPF backup. Restart the game afterwards."):TR("请检查所选内容与目标。","Check the selection and target before confirming."));
        modern_dialog(1,e.action==U_EXIT?TR("退出工具？","Exit toolbox?"):TR("确认此次修改？","Confirm this change?"),body,e);return;
    }
    char out[65536]={0};
    switch(e.action) {
    case U_MOD_INSTALL: g_mod_pending_kind=1;g_mod_pending_idx=e.param;script_mgr_apply();break;
    case U_MOD_RESTORE: g_mod_pending_kind=0;g_mod_pending_idx=e.param;script_mgr_apply();break;
    case U_SCRIPT_INSTALL: g_script_sel=e.param;script_install_selected();break;
    case U_SCRIPT_REMOVE: script_uninstall_selected();break;
    case U_SCRIPT_SCAN: script_scan();dlc_msg(TR("扫描完成：%d 个外部脚本","Scan complete: %d external scripts"),g_script_count);break;
    case U_SCRIPT_INFO: script_show_dir();modern_report(TR("脚本目录与安装状态","Script folder & status"),g_script_dirinfo);break;
    case U_DLC_REFRESH: if(g_dlc_dirty)modern_report(TR("请先保存 DLC 修改","Save DLC edits first"),TR("保存未写入修改后再重新扫描。","Save pending edits before rescanning."));else modern_dlc_request(g_ui.section,1);break;
    case U_DLC_TOGGLE: g_dlc_sel=e.param;dlc_toggle_selected();break;
    case U_DLC_IMPORT: g_dlc_src_sel=e.param;if(dlc_import_selected()==0) modern_dlc_request(g_ui.section,1);break;
    case U_DLC_DELETE: if(dlc_delete_selected()==0) {g_dlc_confirm=0;dlc_do_delete();}break;
    case U_DLC_CONVERT: dlc_convert_selected();break;
    case U_DLC_HD: dlc_hide_hd_selected();break;
    case U_DLC_CHECK:
        modern_dlc_inspect(g_ui.section,g_ui.selected,out,sizeof(out));
        modern_report(TR("DLC 车辆与资源检查","DLC vehicles & resources"),out);break;
    case U_DLC_INFO: modern_report(TR("DLC 目录说明","DLC folders"),TR("待导入目录：\nsdmc:/switch/gta5save/dlc/\n每个文件夹需包含 dlc.rpf。选择后按 A 并确认，PC 格式会自动转换再导入。\n\n游戏安装目录：\n/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/\n\n已安装的 PC 格式包可用「转换格式」处理。","Import folder:\nsdmc:/switch/gta5save/dlc/\nEach folder needs dlc.rpf. Press A and confirm; PC packs convert automatically before registration.\n\nInstalled folder:\n/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/\n\nUse Convert format for installed PC-format packs."));break;
    case U_GFX_PRESET: gfx_apply_preset(e.param);break;
    case U_GFX_IMPORT: if(e.param>=0 && e.param<g_gfx_switch_count) gfx_import_from_switch(g_gfx_switch_files[e.param]);break;
    case U_GFX_SAVE: gfx_save();break;
    case U_GFX_STEP: if(g_gfx_count && g_ui.section==1) gfx_step(e.param);break;
    case U_GFX_CHECK: {
        gfx_check_paths();size_t n=0;
        for(int i=0;i<g_check_paths_count && n<sizeof(out)-256;i++) n+=(size_t)snprintf(out+n,sizeof(out)-n,"%s\n",g_check_paths[i]);
        modern_report(TR("画质路径检查","Graphics paths"),out);break;
    }
    case U_SAVE_PRESET: if(load_preset_save(e.param)==0) save_to_disk();break;
    case U_SAVE_BACKUP: if(load_backup_save(e.param)==0) save_to_disk();break;
    case U_SAVE_SLOT: if(g_dirty) {modern_report(TR("请先保存待写入修改","Save pending changes first"),TR("当前存档有未写入修改。先选择「保存待写入修改」，再切换槽位。","The current save has pending changes. Choose Save pending before switching slots."));} else switch_to_slot(e.param);break;
    case U_SAVE_WRITE: save_to_disk();break;
    case U_SAVE_REFRESH: if(g_ui.page==UI_GRAPHICS) {if(!g_save_dir[0] && !g_dirty) load_initial_save();scan_switch_gfx_files();if(!g_gfx_dirty) gfx_load();} else modern_scan_saves();break;
    case U_PERF_LOAD: if(g_gc_dirty && !confirmed) modern_report(TR("请先保存参数","Save parameters first"),TR("当前有参数修改，请先写入后再载入。","Write pending parameter changes before reloading."));else if(g_gc_opened) gc_reload_from_disk();else gc_open_rpf();break;
    case U_PERF_WRITE: gc_write_to_rpf();break;
    case U_PERF_RESTORE: if(gc_restore_original()==GC_OK) gc_write_to_rpf();break;
    case U_PERF_CHECK: gc_rpf_selftest(out,sizeof(out));modern_report(TR("路径自检报告","Path diagnostics"),out);break;
    case U_PERF_VERIFY: gc_verify_on_disk();modern_report(TR("参数回读校验","Parameter verification"),g_gc_selftest);break;
    case U_PERF_STEP: if(g_gc_opened && g_ui.section==0) gc_knob_step(e.param);break;
    case U_EXIT: g_ui_running=0;break;
    default: break;
    }
    if(e.action==U_DLC_DELETE || e.action==U_DLC_CONVERT || e.action==U_DLC_HD) {
        modern_dlc_request(g_ui.section,1);
    }
}
/* A short eased fade/offset adds no full-screen copies or extra buffers. */
static void modern_transition_update(void) {
    unsigned ms=g_ui_transition_tick?(unsigned)((svcGetSystemTick()-g_ui_transition_tick)*1000/19200000ULL):160;
    if(ms>=160 || g_ui_dialog) {g_ui_transition_tick=0;g_ui_transition_alpha=255;g_ui_transition_x=0;return;}
    unsigned remain=160-ms;
    g_ui_transition_alpha=255-(int)((u64)remain*remain*remain*255/(160*160*160));
    g_ui_transition_x=(int)(12*remain*remain/(160*160));
}
static uint32_t modern_color(int x,uint32_t col) {
    if(x<228 || g_ui_dialog || g_ui_transition_alpha==255)return col;
    unsigned a=(unsigned)g_ui_transition_alpha,r=col&255,g=(col>>8)&255,b=(col>>16)&255;
    return UI_RGB((r*a+15*(255-a))/255,(g*a+21*(255-a))/255,(b*a+28*(255-a))/255);
}
static int modern_x(int x) {return x>=228 && !g_ui_dialog?x+g_ui_transition_x:x;}
static void modern_rect(void *ctx,int x,int y,int w,int h,uint32_t col) {gfx_rect(ctx,modern_x(x),y,w,h,modern_color(x,col));}
static void modern_text(void *ctx,int x,int y,int size,const char *str,uint32_t col) {draw_text(ctx,modern_x(x),y,size,str,modern_color(x,col));}
static int modern_measure(void *ctx,const char *str,int size) {(void)ctx;return text_width(str,size);}
static void modern_hit(void *ctx,int x,int y,int w,int h,UiEvent e) {(void)ctx;hot_add(modern_x(x),y,w,h,e.action,e.param);}
