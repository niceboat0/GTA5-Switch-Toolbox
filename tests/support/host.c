#include <stdio.h>
#include <errno.h>
int g_lang_en=1;
char gc_host_archive_path[1024];
void gc_host_set_archive(const char *p) {
    snprintf(gc_host_archive_path,sizeof(gc_host_archive_path),"%s",p);
}
static int fail_rename,rename_count;
void gc_host_fail_rename(int call) {fail_rename=call;rename_count=0;}
int gc_host_rename(const char *from,const char *to) {
    if(++rename_count==fail_rename){fail_rename=0;errno=EIO;return -1;}
    return rename(from,to);
}
