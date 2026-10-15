/* Exercise the real cooperative catalog loader with disposable directories. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../source/ui.h"
#include "../source/gc_dlc.h"
#include "../source/gc_vehicle.h"
#define TR(zh,en) (en)
#define DLC_MAX 128
#define DLC_SRC_MAX 128
#define DLC_SRC_DIR_MAX 1
#define DLC_DLCPACKS installed_root
static char installed_root[768],import_root[768];
static const char *DLC_SRC_DIRS[]={import_root};
typedef struct {
    char name[96],cn[96],spawn[48],source_path[768];
    int has_rpf,fmt,map_st,n_model,n_map,n_tex,registered,mounted,full;
    u32 rpf_size;
} DlcItem;
static DlcItem g_dlc_items[DLC_MAX],g_dlc_src[DLC_SRC_MAX];
static int g_dlc_count,g_dlc_src_count,g_dlc_src_dir_n,g_dlc_dirty,g_dlc_xml_loaded;
static int g_dlc_msg_ok,g_ui_dialog,probe_reads,config_reads,configuration_ok=1;
static char g_dlc_msg[256];
static UiState g_ui;
static u64 ticks;
static u64 armGetSystemTick(void) {ticks+=20000;return ticks;}
static int dlc_load_dlclist(void) {config_reads++;g_dlc_xml_loaded=configuration_ok;return configuration_ok?0:-1;}
static int dlc_is_registered(const char *name) {(void)name;return 1;}
static int dlc_is_mounted(const char *name) {(void)name;return 1;}
static void dlc_read_cn_name(const char *folder,char *out,size_t cap) {(void)folder;(void)cap;out[0]=0;}
void gc_dlc_probe(const char *path,GcDlcProbe *p) {(void)path;probe_reads++;memset(p,0,sizeof(*p));p->fmt=1;}
static int dlc_src_path_of(const char *name,char *out,size_t cap) {snprintf(out,cap,"%s/%s",import_root,name);return 0;}
static void dlc_prog_frame(void) {}
static void dlc_prog_begin(const char *a,const char *b) {(void)a;(void)b;}
static void dlc_prog_end(void) {}
#include "../source/ui_dlc.h"
static void finish(void) {int limit=100;while(modern_dlc_busy() && limit--)modern_dlc_tick();assert(limit>0);}
int main(int argc,char **argv) {
    assert(argc==3);snprintf(installed_root,sizeof(installed_root),"%s",argv[1]);snprintf(import_root,sizeof(import_root),"%s",argv[2]);
    g_ui.page=UI_DLC;modern_dlc_request(0,0);
    assert(modern_dlc_busy() && !probe_reads && !config_reads && !g_dlc_count);
    modern_dlc_tick();assert(config_reads==1 && !g_dlc_count);
    modern_dlc_tick();assert(g_dlc_count<12 && !probe_reads);
    int old_count=g_dlc_count;g_ui.page=UI_HOME;assert(!modern_dlc_tick() && g_dlc_count==old_count);g_ui.page=UI_DLC;
    finish();assert(g_dlc_count==12 && !probe_reads && !g_dlc_src_count);
    for(int i=0;i<8;i++)modern_dlc_tick();
    assert(probe_reads==4);
    modern_dlc_request(0,0);assert(!modern_dlc_busy() && config_reads==1);
    assert(!modern_dlc_tick() && probe_reads==4);
    g_ui.scroll=4;g_ui.selected=4;for(int i=0;i<8;i++)modern_dlc_tick();
    assert(probe_reads==8);
    g_ui.section=1;g_ui.scroll=g_ui.selected=0;modern_dlc_request(1,0);finish();assert(g_dlc_src_count==2);
    for(int i=0;i<4;i++)modern_dlc_tick();
    assert(probe_reads==10);
    modern_dlc_request(1,0);assert(!modern_dlc_busy() && !modern_dlc_tick());
    modern_dlc_invalidate();g_dlc_xml_loaded=0;configuration_ok=0;modern_dlc_request(0,0);finish();
    assert(config_reads==2 && g_dlc_count==12 && !g_dlc_msg_ok);
    modern_dlc_request(0,1);g_ui_dialog=2;assert(!modern_dlc_tick());g_ui_dialog=0;finish();
    modern_dlc_invalidate();
    puts("DLC catalog: deferred entry, bounded batches, visible probes, cached reentry, source deduplication and failed-config progress passed.");
    return 0;
}
