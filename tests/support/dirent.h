#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct dirent { char d_name[256]; };
typedef struct { HANDLE handle;WIN32_FIND_DATAA data;struct dirent entry;int first; } DIR;
static inline DIR *opendir(const char *path) {
    char pattern[1024];snprintf(pattern,sizeof(pattern),"%s/*",path);
    DIR *d=(DIR *)calloc(1,sizeof(DIR));if (!d) return NULL;
    d->handle=FindFirstFileA(pattern,&d->data);d->first=1;
    if (d->handle==INVALID_HANDLE_VALUE) {free(d);return NULL;}
    return d;
}
static inline struct dirent *readdir(DIR *d) {
    if (!d || (!d->first && !FindNextFileA(d->handle,&d->data))) return NULL;
    d->first=0;snprintf(d->entry.d_name,sizeof(d->entry.d_name),"%s",d->data.cFileName);
    return &d->entry;
}
static inline int closedir(DIR *d) {if (d) {FindClose(d->handle);free(d);}return 0;}

#else
#include_next <dirent.h>
#endif
