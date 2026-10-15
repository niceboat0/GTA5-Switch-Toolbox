#include "save_backup.h"
#include <stdio.h>

int save_backup_copy(const char *source,const char *destination) {
    FILE *in=fopen(source,"rb");
    if(!in) return -1;
    FILE *out=fopen(destination,"wbx");
    if(!out) {fclose(in);return -1;}
    unsigned char buffer[32768];size_t count,total=0;int ok=1;
    while((count=fread(buffer,1,sizeof(buffer),in))>0) {
        if(fwrite(buffer,1,count,out)!=count) {ok=0;break;}
        total+=count;
    }
    if(ferror(in) || !total) ok=0;
    if(fclose(in)!=0) ok=0;
    if(fclose(out)!=0) ok=0;
    if(!ok) {remove(destination);return -1;}
    return 0;
}
