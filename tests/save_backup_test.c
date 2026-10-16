#include "../source/save_backup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

int main(void) {
    const char *source="backup-test-source.tmp",*dest="backup-test-dest.tmp";
    remove(source);remove(dest);
    FILE *f=fopen(source,"wb");assert(f);
    for(int i=0;i<70000;i++) assert(fputc(i%251,f)!=EOF);
    assert(fclose(f)==0);
    assert(save_backup_copy(source,dest)==0);
    f=fopen(dest,"rb");assert(f);
    for(int i=0;i<70000;i++) assert(fgetc(f)==i%251);
    assert(fgetc(f)==EOF);fclose(f);
    /* A second save must never replace an existing historical snapshot. */
    f=fopen(source,"wb");assert(f);fputs("replacement save",f);fclose(f);
    assert(save_backup_copy(source,dest)==-1);
    f=fopen(dest,"rb");assert(f && fgetc(f)==0);fclose(f);
    assert(save_backup_copy("missing-source.tmp",dest)==-1);
    assert(save_backup_copy(source,"missing-directory/backup.tmp")==-1);
    remove(dest);f=fopen(source,"wb");assert(f);fclose(f);
    assert(save_backup_copy(source,dest)==-1);
    f=fopen(dest,"rb");assert(!f);
    remove(source);remove(dest);

    SaveIoError error;char first[640],second[640];
    f=fopen(source,"wb");assert(f);fputs("original game save",f);assert(fclose(f)==0);
    assert(save_backup_create(source,"backup-test-root/nested","stamp",first,sizeof(first),&error)==0);
    assert(save_backup_create(source,"backup-test-root/nested","stamp",second,sizeof(second),&error)==0);
    assert(strcmp(first,second)!=0);
    f=fopen(first,"rb");assert(f);char content[64]={0};assert(fread(content,1,sizeof(content)-1,f)>0);fclose(f);
    assert(strcmp(content,"original game save")==0);
    remove(first);remove(second);
    assert(rmdir("backup-test-root/nested/stamp_000")==0);
    assert(rmdir("backup-test-root/nested/stamp_001")==0);
    assert(rmdir("backup-test-root/nested")==0);
    assert(rmdir("backup-test-root")==0);
    /* An obstructing file is a directory error, not an assumed permission error. */
    assert(save_backup_create(source,source,"stamp",first,sizeof(first),&error)==-1);
    assert(error.code==ENOTDIR && strcmp(error.stage,"mkdir")==0 && strcmp(error.path,source)==0);
    assert(save_backup_copy_ex("missing-source.tmp",dest,&error)==-1);
    assert(error.code==ENOENT && strcmp(error.stage,"read-open")==0);
    /* No detected game target must never write back to an SD import source. */
    assert(save_settings_write("","new settings",12,&error)==-1);
    assert(error.code==ENODEV && strcmp(error.stage,"target")==0);
    f=fopen(source,"rb");assert(f);memset(content,0,sizeof(content));fread(content,1,sizeof(content)-1,f);fclose(f);
    assert(strcmp(content,"original game save")==0);
    assert(save_settings_write("missing-game-directory","new settings",12,&error)==-1);
    assert(strcmp(error.stage,"write-open")==0);
    assert(save_io_write(dest,"new settings",12,&error)==0);
    f=fopen(dest,"rb");assert(f);memset(content,0,sizeof(content));assert(fread(content,1,sizeof(content)-1,f)==12);fclose(f);
    assert(strcmp(content,"new settings")==0);
    assert(save_io_write(dest,"",0,&error)==-1 && error.code==EINVAL);
    remove(source);remove(dest);
    puts("Backup content, exclusive snapshots, missing directories, target selection and checked writes passed.");
    return 0;
}
