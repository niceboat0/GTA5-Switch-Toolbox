#ifndef GC_PACKAGE_H
#define GC_PACKAGE_H
#include <stddef.h>
#define GC_PACKAGE_MAX_FILES 24
#define GC_PACKAGE_MAX 16
#define GC_PACKAGE_PATH 512
#ifndef GC_PACKAGE_GAME
#define GC_PACKAGE_GAME "sdmc:/atmosphere/contents/0100b00b51230000"
#endif
#ifndef GC_PACKAGE_HOME
#define GC_PACKAGE_HOME "sdmc:/switch/gta5save/packages"
#endif
#define GC_PACKAGE_OK 0
typedef struct {
    char mode[8], target[160], entry[200], source[160], hash[65], before[65];
} GcPackageFile;
typedef struct {
    char folder[GC_PACKAGE_PATH], id[64], title[96], title_zh[96], kind[16];
    int count;
    GcPackageFile files[GC_PACKAGE_MAX_FILES];
} GcPackage;
typedef void (*GcPackageProgress)(unsigned long long done,unsigned long long total,void *user);
int gc_package_read(const char *folder,GcPackage *out,char *err,size_t cap);
int gc_package_scan(GcPackage *out,int max,const char *kind);
int gc_package_install(const GcPackage *pack,GcPackageProgress progress,void *user,char *err,size_t cap);
int gc_package_restore(const GcPackage *pack,char *err,size_t cap);
int gc_package_restore_progress(const GcPackage *pack,GcPackageProgress progress,void *user,char *err,size_t cap);
int gc_package_state(const GcPackage *pack); /* 1 installed, 0 absent/mixed */
int gc_package_recover(char *err,size_t cap); /* recover interrupted commit, before new writes */
int gc_package_recover_progress(GcPackageProgress progress,void *user,char *err,size_t cap);
#endif
