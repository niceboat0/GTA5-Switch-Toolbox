"""Exercise the production DLC rows, A action and importer with real RPF conversion.

Game configuration writes are stubs; conversion and format checks use gc_dlc.c.
"""
import argparse,json,os,shlex,struct,subprocess
from pathlib import Path
from test_save_paths import function
from test_scripts import flat_rpf,read_flat

HEADER=r'''
#include "ui.h"
#include "gc_dlc.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#endif
#define TR(zh,en) (en)
#define GC_OK 0
#define DLC_DLCPACKS "installed"
typedef struct {
    char name[64],cn[64],spawn[48],source_path[768];
    int has_rpf,fmt,map_st,n_model,n_map,n_tex,registered,mounted,full;
    u32 rpf_size;
} DlcItem;
static DlcItem g_dlc_src[2],g_dlc_items[2];
static int g_dlc_src_sel,g_dlc_src_count=1,g_dlc_count,g_dlc_xml_loaded=1,g_dlc_etud_loaded=1,g_dlc_dirty;
static int g_dlc_msg_ok,busy,copies,registrations,game_writes,progress_open;
static int g_gc_rpf,g_dlc_xml_ent,g_dlc_etud_ent;
static const char *g_dlc_xml="xml",*g_dlc_etud="etud";
static size_t g_dlc_xml_len=3,g_dlc_etud_len=4;
static char g_dlc_msg[256];
static UiState g_ui;
static UiView g_view;
static void dlc_msg(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(g_dlc_msg,sizeof(g_dlc_msg),format,args);va_end(args);g_dlc_msg_ok=1;
}
static void dlc_err(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(g_dlc_msg,sizeof(g_dlc_msg),format,args);va_end(args);g_dlc_msg_ok=0;
}
static int modern_dlc_busy(void) {return busy;}
static void fmt_size(char *out,size_t cap,unsigned bytes) {snprintf(out,cap,"%u B",bytes);}
static int dlc_load_dlclist(void) {g_dlc_xml_loaded=1;return 0;}
static int dlc_src_path_of(const char *name,char *path,size_t cap) {
    /* Discovery must win over a different same-name folder in another root. */
    (void)name;(void)path;(void)cap;assert(0 && "import ignored its discovered source");return -1;
}
static void dlc_prog_begin(const char *a,const char *b) {(void)a;(void)b;progress_open++;}
static void dlc_prog_end(void) {progress_open--;assert(progress_open>=0);}
static void dlc_prog_frame(void) {}
static void dlc_prog_set(const char *a,const char *b,int n) {(void)a;(void)b;(void)n;}
static int dlc_copy_tree(const char *source,const char *dest,char *err,size_t cap) {
    (void)err;(void)cap;char from[800],to[800];copies++;mkdir(dest,0777);
    snprintf(from,sizeof(from),"%s/dlc.rpf",source);snprintf(to,sizeof(to),"%s/dlc.rpf",dest);
    FILE *in=fopen(from,"rb"),*out=fopen(to,"wb");assert(in && out);
    unsigned char data[1024];size_t n;while((n=fread(data,1,sizeof(data),in)))assert(fwrite(data,1,n,out)==n);
    assert(!ferror(in));assert(fclose(in)==0);assert(fclose(out)==0);return 1;
}
static int dlc_xml_add(const char *name) {(void)name;registrations++;return 0;}
static int dlc_etud_add(const char *name) {(void)name;registrations++;return 0;}
static int dlc_save_dlclist(void) {game_writes++;return 0;}
static int dlc_save_etud(void) {game_writes++;return 0;}
static int gc_rpf_verify(const int *archive,const int *entry,const u8 *data,size_t n) {
    (void)archive;(void)entry;assert(data && n);return 0;
}
static void dlc_check_integrity(const char *name,char *out,size_t cap) {(void)name;(void)cap;out[0]=0;}
static void dlc_scan_dlcpacks(void) {}
static void dlc_scan_src(void) {}
static void dlc_refresh_registered(void) {}
static UiEvent modern_event(int action,int param) {return (UiEvent){action,param};}
static void modern_copy(char *out,size_t cap,const char *text) {snprintf(out,cap,"%s",text?text:"");}
'''
BODY=r'''
static void select_pack(const char *name) {
    memset(g_dlc_src,0,sizeof(g_dlc_src));DlcItem *item=&g_dlc_src[0];
    snprintf(item->name,sizeof(item->name),"%s",name);
    snprintf(item->source_path,sizeof(item->source_path),"imports/%s",name);
    item->has_rpf=1;item->fmt=0;g_dlc_src_sel=0;
}
static void check_action(int format,int disabled) {
    g_dlc_src[0].fmt=format;memset(&g_view,0,sizeof(g_view));build_dlc();
    assert(g_view.row_count==1 && g_view.rows[0].disabled==disabled);
    assert(g_view.buttons[0].disabled==disabled);
    g_ui.focus=UI_LIST;UiEvent action=ui_input(&g_ui,&g_view,UI_ACCEPT);
    assert(action.action==(disabled?0:U_DLC_IMPORT));
}
int main(void) {
    ui_reset(&g_ui,UI_DLC,1);select_pack("pc");
    check_action(0,0);check_action(1,0);check_action(-1,1);check_action(-2,1);
    busy=1;check_action(0,1);busy=0;g_dlc_xml_loaded=0;check_action(0,1);g_dlc_xml_loaded=1;
    select_pack("pc");assert(dlc_import_selected()==0 && g_dlc_msg_ok);
    assert(registrations==2 && game_writes==2 && copies==1 && !progress_open);
    assert(gc_dlc_is_switch_format("installed/pc/dlc.rpf")==1);
    assert(gc_dlc_is_switch_format("imports/pc/dlc.rpf")==0);
    select_pack("switch");assert(dlc_import_selected()==0);
    assert(registrations==4 && game_writes==4 && copies==2);
    select_pack("invalid");assert(dlc_import_selected()<0 && !g_dlc_msg_ok);
    assert(registrations==4 && game_writes==4 && copies==2 && !progress_open);
    select_pack("no-platform");assert(dlc_import_selected()<0 && !g_dlc_msg_ok);
    assert(registrations==4 && game_writes==4 && copies==3 && !progress_open);
    select_pack("compressed");assert(dlc_import_selected()<0 && !g_dlc_msg_ok);
    assert(registrations==4 && game_writes==4 && copies==4 && !progress_open);
    puts("DLC PC/Switch A actions, source preservation, real conversion and validation-before-registration passed.");
    return 0;
}
'''

def platform_rpf(platform,inner,compressed=False):
    raw=flat_rpf([('vehicles.rpf',inner,compressed)])
    # A valid root -> platform directory -> nested archive chain.
    names=b'\0'+platform.encode()+b'\0vehicles.rpf\0'
    old_name_size=struct.unpack_from('<I',raw,8)[0]
    old_name_pool=raw[48:48+old_name_size]
    old_record=raw[32:48];q,size,flags=struct.unpack('<QII',old_record)
    q=(q&~65535)|(len(platform)+2)
    head=struct.pack('<4I',0x52504637,3,len(names),0x4e45504f)+struct.pack('<4I',0,0x7fffff00,1,1)+struct.pack('<4I',1,0x7fffff00,2,1)+struct.pack('<QII',q,size,flags)+names
    result=bytearray(raw);result[:512]=bytes(512);result[:len(head)]=head
    assert old_name_pool.startswith(b'\0vehicles.rpf\0')
    return bytes(result)

def test(root,out,zlib_source):
    out.mkdir(parents=True,exist_ok=True)
    inner=flat_rpf([('tr22.yft',b'base model',False),('tr22_hi.yft',b'high model',False),('tr22.ytd',b'texture',False)])
    fixtures={'pc':platform_rpf('x64',inner),'switch':platform_rpf('switch',flat_rpf([('tr22.nft',b'base model',False)])),
              'invalid':b'not a valid RPF','no-platform':inner,'compressed':platform_rpf('x64',inner,True)}
    for name,data in fixtures.items():
        folder=out/'imports'/name;folder.mkdir(parents=True,exist_ok=True);(folder/'dlc.rpf').write_bytes(data)
    (out/'installed').mkdir(exist_ok=True)
    adapter=(root/'source/ui_adapter.h').read_text(encoding='utf-8');main=(root/'source/main.c').read_text(encoding='utf-8')
    fragment=adapter.split('} else if(g_ui.page==UI_DLC) {',1)[1].split('} else if(g_ui.page==UI_GRAPHICS) {',1)[0]
    start=adapter.index('static UiRow *modern_row(');end=adapter.index('static void modern_dialog(',start)
    enum_start=adapter.index('enum {');enum_end=adapter.index('};',enum_start)+2
    code=HEADER+'\n'+adapter[enum_start:enum_end]+'\n'+adapter[start:end]+'\nstatic void build_dlc(void) {\n'+fragment+'}\n'+function(main,'dlc_import_selected')+'\n'+BODY
    harness=out/'dlc-import-test.c';harness.write_text(code,encoding='utf-8')
    compiler=shlex.split(os.environ.get('CC','cc'),posix=os.name!='nt')
    exe=out/('dlc-import-test.exe' if os.name=='nt' else 'dlc-import-test')
    sources=[root/'source/gc_dlc.c',root/'source/ui.c',harness]
    args=compiler+['-std=c11','-D_DEFAULT_SOURCE','-O1','-g','-UNDEBUG','-I'+str(root/'source'),'-I'+str(root/'tests/support')]
    if os.name=='nt':
        if zlib_source is None:raise RuntimeError('Windows tests require --zlib-source')
        sources += [zlib_source/n for n in ['adler32.c','crc32.c','compress.c','deflate.c','inflate.c','inffast.c','inftrees.c','trees.c','zutil.c']]
        args+=['-I'+str(zlib_source)]
    else:args+=['-fsanitize=address,undefined','-include','strings.h']
    args += [str(p) for p in sources]+['-o',str(exe)]+(['-lz'] if os.name!='nt' else [])
    subprocess.run(args,check=True);subprocess.run([str(exe.resolve())],cwd=out,check=True)
    for name,data in fixtures.items():assert (out/'imports'/name/'dlc.rpf').read_bytes()==data,'source changed: '+name
    converted=(out/'installed/pc/dlc.rpf').read_bytes();ec,nl=struct.unpack_from('<II',converted,4)
    names=converted[16+ec*16:16+ec*16+nl];assert b'switch\0' in names and b'x64\0' not in names
    q=struct.unpack_from('<Q',converted,48)[0];nested=read_flat(converted[((q>>40)&0x7fffff)*512:])
    assert nested=={'tr22.nft':b'base model','tr22_hi.nft':b'high model','tr22.ntd':b'texture'}
    record={'passed':True,'pc_A_enabled':True,'switch_A_enabled':True,'invalid_A_disabled':True,'source_bytes_unchanged':True,
            'real_rpf_conversion_verified':True,'failed_conversion_not_registered':True,'console_verified':False}
    (out/'dlc-import-tests.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(record))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--zlib-source',type=Path);a=p.parse_args()
    test(Path(__file__).resolve().parents[1],a.output.resolve(),a.zlib_source.resolve() if a.zlib_source else None)
