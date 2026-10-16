"""Exercise the actual main.c save/settings orchestration with native commit faults."""
import argparse,json,os,shlex,subprocess
from pathlib import Path

def function(source,name):
    start=source.index('static int '+name+'(')
    # Skip a forward declaration, if present.
    while source.index(';',start)<source.index('{',start):
        start=source.index('static int '+name+'(',start+1)
    brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

HEADER=r'''
#include "save_backup.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
typedef uint32_t Result;
#define R_FAILED(rc) ((rc)!=0)
#define TR(zh,en) (en)
#define C_RED 1
#define C_GREEN 2
#define C_ACCENT 3
#define NUM_GFX_PRESETS 1
#define NUM_ATTRS 8
static char g_save_dir[512],g_save_path[512],g_gfx_path[512],g_status_msg[256];
static int g_is_native_mount,g_gfx_dirty,g_dirty,g_status_color,toasts,reports,parsed;
static Result commit_result,last_commit;
static int backups,backup_fail;
static SaveIoError last_error;
static unsigned char *g_save_buf;
static size_t g_save_sz=18;
static int g_is_plain=1,g_plat,g_start_off;
static int g_found[3][8],g_offsets[3][8],g_values[3][8],g_skill_modified[3][8];
static const struct {const char *name,*path;} preset={"Test preset","builtin.xml"};
#define GFX_PRESET(i) (preset)
static Result fsdevCommitDevice(const char *device) {
    assert(strcmp(device,"save")==0);return commit_result;
}
static void save_io_report(const char *operation,const SaveIoError *error,Result rc) {
    reports++;last_error=*error;last_commit=rc;g_status_color=C_RED;
    snprintf(g_status_msg,sizeof(g_status_msg),"%s failed [%s]",operation,error->stage);
}
static void show_toast(const char *text,int color) {(void)text;(void)color;toasts++;}
static void gfx_parse_xml(char *data,size_t size) {assert(size>0 && data);parsed++;}
static size_t gfx_read_all(const char *path,char **out) {
    assert(strcmp(path,"builtin.xml")==0);
    *out=malloc(19);assert(*out);memcpy(*out,"new settings data!",19);return 18;
}
static int backup_current_save_timestamp(const char *reason) {
    assert(reason);backups++;
    if(backup_fail) return 0;
    return save_backup_copy(g_save_path,"slot-backup.tmp")==0;
}
static void gta5_write_value(void *data,size_t size,int off,int width,int sign,int value) {
    (void)data;(void)size;(void)off;(void)width;(void)sign;(void)value;
}
static void write_skill_behaviors(int a,int b) {(void)a;(void)b;}
static int gta5_encrypt_body(void *data,size_t size,int platform,int start) {
    (void)data;(void)size;(void)platform;(void)start;return 0;
}
'''
BODY=r'''
static void fixture(const char *path,const char *data) {
    FILE *file=fopen(path,"wb");assert(file);assert(fwrite(data,1,18,file)==18);assert(fclose(file)==0);
}
static void check_file(const char *path,const char *data) {
    char text[19]={0};FILE *file=fopen(path,"rb");assert(file);assert(fread(text,1,18,file)==18);fclose(file);
    assert(memcmp(text,data,18)==0);
}
int main(void) {
    const char *old="original game save",*next="replacement save!!";
    fixture("source.xml",old);snprintf(g_gfx_path,sizeof(g_gfx_path),"source.xml");
    /* Applying a preset without a game target cannot claim success or alter the source. */
    assert(gfx_apply_preset(0)==-1 && g_gfx_dirty==1 && toasts==0 && reports==1);
    assert(strcmp(last_error.stage,"target")==0 && last_error.code==ENODEV);
    check_file("source.xml",old);
    mkdir("settings-target",0777);snprintf(g_save_dir,sizeof(g_save_dir),"settings-target");
    g_is_native_mount=1;commit_result=0x1234;
    assert(gfx_apply_preset(0)==-1 && g_gfx_dirty==1 && toasts==0);
    assert(strcmp(last_error.stage,"commit")==0 && last_commit==0x1234);
    check_file("source.xml",old);
    commit_result=0;
    assert(gfx_apply_preset(0)==0 && g_gfx_dirty==0 && toasts==1);
    assert(strcmp(g_gfx_path,"settings-target/settings.xml")==0);
    check_file(g_gfx_path,"new settings data!");
    /* Story save replacement keeps the old disk slot in the backup, not the loaded preset. */
    snprintf(g_save_path,sizeof(g_save_path),"game-slot.tmp");g_save_buf=(unsigned char*)next;
    fixture(g_save_path,old);remove("slot-backup.tmp");g_dirty=1;backup_fail=1;int before=toasts;
    assert(save_to_disk()==-1 && g_dirty==1 && toasts==before);
    check_file(g_save_path,old);
    backup_fail=0;commit_result=0x5678;
    assert(save_to_disk()==-1 && g_dirty==1 && toasts==before);
    assert(strcmp(last_error.stage,"commit")==0 && last_commit==0x5678);
    check_file("slot-backup.tmp",old);
    remove("slot-backup.tmp");fixture(g_save_path,old);commit_result=0;
    assert(save_to_disk()==0 && g_dirty==0 && toasts==before+1);
    check_file("slot-backup.tmp",old);check_file(g_save_path,next);
    assert(backups==3);
    remove(g_save_path);remove("slot-backup.tmp");remove("source.xml");remove(g_gfx_path);rmdir("settings-target");
    puts("Production preset/save flows: no-target errors, source protection, backup ordering and failed commits passed.");
    return 0;
}
'''

def test(root,out):
    out.mkdir(parents=True,exist_ok=True)
    source=(root/'source/main.c').read_text(encoding='utf-8')
    harness=out/'save-paths-test.c'
    harness.write_text(HEADER+'\n'+ '\n'.join(function(source,n) for n in ['gfx_write_target','gfx_apply_preset','save_to_disk'])+'\n'+BODY,encoding='utf-8')
    exe=out/('save-paths-test.exe' if os.name=='nt' else 'save-paths-test')
    compiler=shlex.split(os.environ.get('CC','cc'),posix=os.name!='nt')
    args=compiler+['-std=c11','-O1','-g','-UNDEBUG','-Wall','-Wextra','-Werror','-I'+str(root/'source'),str(root/'source/save_backup.c'),str(harness),'-o',str(exe)]
    if os.name!='nt':args+=['-fsanitize=address,undefined']
    subprocess.run(args,check=True)
    subprocess.run([str(exe.resolve())],check=True,cwd=out)
    (out/'save-paths-tests.json').write_text(json.dumps({'passed':True,'console_verified':False}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();test(Path(__file__).resolve().parents[1],a.output.resolve())
