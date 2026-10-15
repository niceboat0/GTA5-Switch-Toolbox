/* Cooperative DLC catalog loading: metadata first, visible probes on demand. */
static int g_dlc_ready[2],g_dlc_scan_section=-1,g_dlc_scan_root,g_dlc_scan_next=-1;
static DIR *g_dlc_scan_dir;
static int g_dlc_config_attempted;
static void modern_dlc_invalidate(void) {
    if(g_dlc_scan_dir) {closedir(g_dlc_scan_dir);g_dlc_scan_dir=NULL;}
    g_dlc_scan_section=-1;g_dlc_scan_next=-1;g_dlc_ready[0]=g_dlc_ready[1]=0;g_dlc_config_attempted=0;
}
static int modern_dlc_busy(void) {return g_dlc_scan_section>=0;}
static void modern_dlc_start(int section) {
    if(g_dlc_scan_dir) {closedir(g_dlc_scan_dir);g_dlc_scan_dir=NULL;}
    g_dlc_scan_section=section;g_dlc_scan_root=0;g_dlc_ready[section]=0;
    if(section) {g_dlc_src_count=0;g_dlc_src_dir_n=0;}else g_dlc_count=0;
    snprintf(g_dlc_msg,sizeof(g_dlc_msg),"%s",TR("正在分批读取 DLC 目录…","Reading DLC folders in batches…"));g_dlc_msg_ok=1;
}
static void modern_dlc_request(int section,int force) {
    if(force) {if(g_dlc_dirty)return;modern_dlc_invalidate();g_dlc_xml_loaded=0;}
    if(g_dlc_ready[section]) {
        snprintf(g_dlc_msg,sizeof(g_dlc_msg),TR("使用缓存：%d 个 DLC；新增文件后可重新扫描。","Cached: %d DLC folders; rescan after adding files."),section?g_dlc_src_count:g_dlc_count);
        g_dlc_msg_ok=g_dlc_xml_loaded;return;
    }
    if(modern_dlc_busy() && (g_dlc_scan_section==section || g_dlc_scan_next==section))return;
    g_dlc_scan_next=section && !g_dlc_ready[0]?1:-1;
    modern_dlc_start(section && !g_dlc_ready[0]?0:section);
}
static int modern_dlc_compare(const void *a,const void *b) {return strcmp(((const DlcItem*)a)->name,((const DlcItem*)b)->name);}
static void modern_dlc_finish(void) {
    int section=g_dlc_scan_section,n=section?g_dlc_src_count:g_dlc_count;
    qsort(section?g_dlc_src:g_dlc_items,(size_t)n,sizeof(DlcItem),modern_dlc_compare);
    g_dlc_ready[section]=1;g_dlc_scan_section=-1;
    snprintf(g_dlc_msg,sizeof(g_dlc_msg),TR("目录已载入：%d 个 DLC；资源信息按需读取。新增文件后可重新扫描。","Loaded %d DLC folders; resource details load on demand. Rescan after adding files."),n);g_dlc_msg_ok=g_dlc_xml_loaded;
    if(!g_dlc_xml_loaded)snprintf(g_dlc_msg,sizeof(g_dlc_msg),"%s",TR("目录已载入；游戏配置读取失败，请检查路径后重新扫描。","Folders loaded; game configuration could not be read. Check paths and rescan."));
    if(g_dlc_scan_next>=0) {int next=g_dlc_scan_next;g_dlc_scan_next=-1;modern_dlc_start(next);}
}
/* Called after presenting a frame, so entering this workspace draws first. */
static int modern_dlc_tick(void) {
    if(g_ui.page!=UI_DLC || g_ui_dialog)return 0;
    if(!g_dlc_xml_loaded && !g_dlc_config_attempted && modern_dlc_busy()) {g_dlc_config_attempted=1;dlc_load_dlclist();return 1;}
    if(modern_dlc_busy()) {
        u64 started=armGetSystemTick();int section=g_dlc_scan_section;
        for(int budget=0;budget<8;budget++) {
            const char *base=section?DLC_SRC_DIRS[g_dlc_scan_root]:DLC_DLCPACKS;
            if(!g_dlc_scan_dir)g_dlc_scan_dir=opendir(base);
            struct dirent *entry=g_dlc_scan_dir?readdir(g_dlc_scan_dir):NULL;
            if(!entry) {
                if(g_dlc_scan_dir) {closedir(g_dlc_scan_dir);g_dlc_scan_dir=NULL;}
                if(!section || ++g_dlc_scan_root>=DLC_SRC_DIR_MAX) {modern_dlc_finish();break;}
                continue;
            }
            if(entry->d_name[0]=='.')continue;
            int *count=section?&g_dlc_src_count:&g_dlc_count,limit=section?DLC_SRC_MAX:DLC_MAX;
            if(*count>=limit) {closedir(g_dlc_scan_dir);g_dlc_scan_dir=NULL;modern_dlc_finish();break;}
            int duplicate=0;
            if(section) {
                for(int i=0;i<g_dlc_count;i++)if(!strcasecmp(g_dlc_items[i].name,entry->d_name))duplicate=1;
                for(int i=0;i<g_dlc_src_count;i++)if(!strcasecmp(g_dlc_src[i].name,entry->d_name))duplicate=1;
            }
            if(duplicate)continue;
            char folder[768],path[800];struct stat info;
            snprintf(folder,sizeof(folder),"%s/%s",base,entry->d_name);
            if(stat(folder,&info) || !S_ISDIR(info.st_mode))continue;
            snprintf(path,sizeof(path),"%s/dlc.rpf",folder);
            int present=!stat(path,&info) && S_ISREG(info.st_mode) && info.st_size>0;
            if(section && !present)continue;
            DlcItem *it=section?&g_dlc_src[*count]:&g_dlc_items[*count];memset(it,0,sizeof(*it));
            size_t name_len=strlen(entry->d_name);if(name_len>=sizeof(it->name))continue;
            memcpy(it->name,entry->d_name,name_len+1);snprintf(it->source_path,sizeof(it->source_path),"%s",folder);
            it->has_rpf=present;it->rpf_size=present?(u32)info.st_size:0;it->fmt=present?-2:-1;it->map_st=-1;
            dlc_read_cn_name(folder,it->cn,sizeof(it->cn));
            if(!section) {it->registered=dlc_is_registered(it->name);it->mounted=dlc_is_mounted(it->name);it->full=it->registered && it->mounted;}
            (*count)++;
            if(armGetSystemTick()-started>19200ULL*4)break;
        }
        return 1;
    }
    int count=g_ui.section?g_dlc_src_count:g_dlc_count;
    for(int i=g_ui.scroll;i<count && i<g_ui.scroll+4;i++) {
        DlcItem *it=g_ui.section?&g_dlc_src[i]:&g_dlc_items[i];
        if(it->fmt!=-2)continue;
        char path[800];snprintf(path,sizeof(path),"%s/dlc.rpf",it->source_path);GcDlcProbe p;gc_dlc_probe(path,&p);
        it->fmt=p.fmt;it->map_st=p.map_st;it->n_model=p.n_model;it->n_map=p.n_map;it->n_tex=p.n_tex;
        snprintf(it->spawn,sizeof(it->spawn),"%s",p.vehicle);return 1;
    }
    return 0;
}
static void modern_dlc_progress(void *ctx) {(void)ctx;dlc_prog_frame();}
static void modern_dlc_inspect(int section,int index,char *out,size_t cap) {
    int count=section?g_dlc_src_count:g_dlc_count;if(index<0 || index>=count)return;
    DlcItem *it=section?&g_dlc_src[index]:&g_dlc_items[index];char folder[768],path[800];
    if(it->source_path[0])snprintf(folder,sizeof(folder),"%s",it->source_path);
    else if(section) {if(dlc_src_path_of(it->name,folder,sizeof(folder)))return;}
    else snprintf(folder,sizeof(folder),"%s/%s",DLC_DLCPACKS,it->name);
    snprintf(path,sizeof(path),"%s/dlc.rpf",folder);
    dlc_prog_begin(TR("正在检查 DLC 车辆资源","Inspecting DLC vehicle resources"),it->name);dlc_prog_frame();
    GcVehicleReport *report=malloc(sizeof(*report));char err[256]={0};
    int rc=report?gc_vehicle_inspect(path,report,modern_dlc_progress,NULL,err,sizeof(err)):-1;
    dlc_prog_end();
    if(rc) {snprintf(out,cap,TR("无法检查：%s","Inspection failed: %s"),err[0]?err:TR("内存不足","Out of memory"));free(report);return;}
    int missing=0;for(int i=0;i<report->count;i++)if(report->vehicles[i].flags!=3)missing++;
    size_t used=(size_t)snprintf(out,cap,TR("车辆 / 刷车名：%d 项\n模型配对异常：%d 项%s\n按 .nft / .yft 模型名称识别，非游戏内翻译名称。名称齐全不代表已验证游戏内兼容性。\n\n","Vehicle / spawn names: %d\nModel pairing issues: %d%s\nIdentified from .nft / .yft model filenames, not localized display names. Matching names do not prove in-game compatibility.\n\n"),report->count,missing,report->partial?TR("；部分归档未完成检查","; some archives could not be fully inspected"):"");
    for(int i=0;i<report->count && used<cap;i++) {
        GcVehicle *v=&report->vehicles[i];int n=snprintf(out+used,cap-used,"%d. %s%s\n",i+1,v->name,v->flags==3?"":v->flags==1?TR("  [缺 _hi 模型]","  [missing _hi]"):TR("  [缺普通模型]","  [missing base model]"));
        if(n<0 || (size_t)n>=cap-used)break;
        used+=(size_t)n;
    }
    if(!report->count && used<cap)snprintf(out+used,cap-used,"%s",TR("没有识别到车辆模型，可能是地图、音频或武器包。","No vehicle models found; this may be a map, audio or weapon pack."));
    free(report);
}
