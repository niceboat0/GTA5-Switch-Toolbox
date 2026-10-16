#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

enum { NO_FAULT, SHORT_WRITE, FLUSH_FAIL, SYNC_FAIL, CLOSE_FAIL };
static int fault;
static size_t failing_write(const void *data,size_t size,size_t count,FILE *file) {
    if(fault==SHORT_WRITE) {
        size_t written=fwrite(data,size,count/2,file);errno=ENOSPC;return written;
    }
    return fwrite(data,size,count,file);
}
static int failing_flush(FILE *file) {
    if(fault==FLUSH_FAIL) {errno=ENOSPC;return EOF;}
    return fflush(file);
}
static int failing_close(FILE *file) {
    int result=fclose(file);
    if(fault==CLOSE_FAIL) {errno=EIO;return EOF;}
    return result;
}
static int failing_sync(int fd) {
    if(fault==SYNC_FAIL) {errno=EIO;return -1;}
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

#define fwrite failing_write
#define fflush failing_flush
#define fclose failing_close
#ifdef _WIN32
#define _commit failing_sync
#else
#define fsync failing_sync
#endif
#include "../source/save_backup.c"
#undef fwrite
#undef fflush
#undef fclose
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

int main(void) {
    const char *source="io-fault-source.tmp",*dest="io-fault-dest.tmp";
    FILE *file=fopen(source,"wb");assert(file);fputs("original game save",file);assert(fclose(file)==0);
    const char *stages[]={"","write","flush","sync","close"};
    SaveIoError error;
    for(fault=SHORT_WRITE;fault<=CLOSE_FAIL;fault++) {
        assert(save_io_write(dest,"replacement",11,&error)==-1);
        assert(strcmp(error.stage,stages[fault])==0);
        assert(strcmp(error.path,dest)==0);
        assert(error.code==(fault<=FLUSH_FAIL?ENOSPC:EIO));
        remove(dest);
    }
    for(fault=SHORT_WRITE;fault<=CLOSE_FAIL;fault++) {
        assert(save_backup_copy_ex(source,dest,&error)==-1);
        assert(error.code==(fault<=FLUSH_FAIL?ENOSPC:EIO));
        assert(strcmp(error.stage,fault==CLOSE_FAIL?"read-close":stages[fault])==0);
        assert(fopen(dest,"rb")==NULL);
        FILE *check=fopen(source,"rb");assert(check);char text[32]={0};fread(text,1,sizeof(text)-1,check);fclose(check);
        assert(strcmp(text,"original game save")==0);
    }
    fault=NO_FAULT;
    assert(save_backup_copy_ex(source,dest,&error)==0);
    remove(source);remove(dest);
    puts("Short writes, full disk, flush, sync and close failures are detected; original slots survive failed backups.");
    return 0;
}
