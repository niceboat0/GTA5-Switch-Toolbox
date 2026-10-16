#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "save_backup.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static int io_error(SaveIoError *error, const char *stage, const char *path, int code) {
    if(error) {
        error->stage=stage;
        error->code=code?code:EIO;
        snprintf(error->path,sizeof(error->path),"%s",path?path:"");
    }
    errno=code?code:EIO;
    return -1;
}

static int io_sync(FILE *file) {
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

static int io_finish(FILE *file, const char *path, SaveIoError *error) {
    int result=0;
    if(fflush(file)!=0) result=io_error(error,"flush",path,errno);
    else if(io_sync(file)!=0) result=io_error(error,"sync",path,errno);
    int saved_errno=errno;
    if(fclose(file)!=0 && result==0) return io_error(error,"close",path,errno);
    if(result!=0) errno=saved_errno;
    return result;
}

int save_backup_copy_ex(const char *source, const char *destination, SaveIoError *error) {
    SaveIoError local_error;
    if(!error) error=&local_error;
    memset(error,0,sizeof(*error));
    FILE *in=fopen(source,"rb");
    if(!in) return io_error(error,"read-open",source,errno);
    FILE *out=fopen(destination,"wbx");
    if(!out) {
        int code=errno;fclose(in);
        return io_error(error,"backup-create",destination,code);
    }
    unsigned char buffer[32768];size_t count,total=0;int result=0;
    while((count=fread(buffer,1,sizeof(buffer),in))>0) {
        errno=0;
        if(fwrite(buffer,1,count,out)!=count) {
            result=io_error(error,"write",destination,errno);break;
        }
        total+=count;
    }
    if(result==0 && (ferror(in) || !total)) result=io_error(error,"read",source,total?errno:EIO);
    int saved_errno=errno;
    if(fclose(in)!=0 && result==0) result=io_error(error,"read-close",source,errno);
    if(result==0) result=io_finish(out,destination,error);
    else {saved_errno=error->code;fclose(out);errno=saved_errno;}
    if(result!=0) {saved_errno=errno;remove(destination);errno=saved_errno;}
    return result;
}

int save_backup_copy(const char *source, const char *destination) {
    return save_backup_copy_ex(source,destination,NULL);
}

static int io_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path,0777);
#endif
}

static int io_ensure_directory(const char *path, SaveIoError *error) {
    struct stat st;
    if(stat(path,&st)==0) {
        if(S_ISDIR(st.st_mode)) return 0;
        return io_error(error,"mkdir",path,ENOTDIR);
    }
    if(errno!=ENOENT) return io_error(error,"mkdir",path,errno);
    if(io_mkdir(path)==0) return 0;
    int code=errno;
    if(code==EEXIST && stat(path,&st)==0 && S_ISDIR(st.st_mode)) return 0;
    return io_error(error,"mkdir",path,code);
}

int save_backup_create(const char *source, const char *root, const char *stamp,
                       char *destination, size_t capacity, SaveIoError *error) {
    if(error) memset(error,0,sizeof(*error));
    if(capacity) destination[0]='\0';
    char path[640];
    if(snprintf(path,sizeof(path),"%s",root)>=(int)sizeof(path))
        return io_error(error,"mkdir",root,ENAMETOOLONG);
    /* Skip the device prefix (sdmc:) and absolute root when creating parents. */
    char *start=strchr(path,':');start=start?start+1:path;
    for(char *p=start;*p;p++) if(*p=='/' && p>start) {
        *p='\0';int result=io_ensure_directory(path,error);*p='/';
        if(result!=0) return -1;
    }
    if(io_ensure_directory(path,error)!=0) return -1;
    const char *name=strrchr(source,'/');name=name?name+1:source;
    for(int i=0;i<1000;i++) {
        if(snprintf(path,sizeof(path),"%s/%s_%03d",root,stamp,i)>=(int)sizeof(path))
            return io_error(error,"backup-create",root,ENAMETOOLONG);
        /* mkdir, rather than a stat-then-mkdir race, reserves each snapshot. */
        if(io_mkdir(path)==0) {
            if(snprintf(destination,capacity,"%s/%s",path,name)>=(int)capacity) {
                rmdir(path);
                return io_error(error,"backup-create",path,ENAMETOOLONG);
            }
            if(save_backup_copy_ex(source,destination,error)==0) return 0;
            int code=errno;rmdir(path);errno=code;return -1;
        }
        if(errno!=EEXIST) return io_error(error,"mkdir",path,errno);
    }
    return io_error(error,"backup-create",root,EEXIST);
}

int save_io_write(const char *path, const void *data, size_t size, SaveIoError *error) {
    if(error) memset(error,0,sizeof(*error));
    if(!size) return io_error(error,"write",path,EINVAL);
    FILE *file=fopen(path,"wb");
    if(!file) return io_error(error,"write-open",path,errno);
    errno=0;
    if(fwrite(data,1,size,file)!=size) {
        int code=errno;fclose(file);
        return io_error(error,"write",path,code);
    }
    return io_finish(file,path,error);
}

int save_settings_write(const char *directory, const void *data, size_t size, SaveIoError *error) {
    if(error) memset(error,0,sizeof(*error));
    if(!directory || !directory[0]) return io_error(error,"target","",ENODEV);
    char path[640];
    if(snprintf(path,sizeof(path),"%s/settings.xml",directory)>=(int)sizeof(path))
        return io_error(error,"target",directory,ENAMETOOLONG);
    return save_io_write(path,data,size,error);
}
