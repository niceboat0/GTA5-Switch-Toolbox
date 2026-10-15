#include "../source/save_backup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    puts("On-disk backup content, collision protection and failure handling passed.");
    return 0;
}
