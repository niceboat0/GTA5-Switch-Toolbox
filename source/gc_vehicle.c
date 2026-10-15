#include "gc_vehicle.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

typedef struct {FILE *file;const unsigned char *memory;uint64_t size;} Source;
static uint32_t u32le(const unsigned char *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t u64le(const unsigned char *p) {return u32le(p)|((uint64_t)u32le(p+4)<<32);}
static int read_at(Source *s,uint64_t at,void *out,size_t n) {
    if(at>s->size || n>s->size-at)return -1;
    if(s->memory) {memcpy(out,s->memory+(size_t)at,n);return 0;}
    return fseeko(s->file,(long long)at,SEEK_SET) || fread(out,1,n,s->file)!=n?-1:0;
}
static int suffix(const char *s,const char *end) {
    size_t n=strlen(s),m=strlen(end);return n>=m && !strcmp(s+n-m,end);
}
static void add(GcVehicleReport *r,char *name) {
    size_t n=strlen(name);name[n-4]=0;unsigned flags=1;
    if(suffix(name,"_hi")) {name[n-7]=0;flags=2;}
    if(!*name)return;
    for(int i=0;i<r->count;i++) if(!strcmp(r->vehicles[i].name,name)) {r->vehicles[i].flags|=flags;return;}
    if(r->count==GC_VEHICLE_MAX) {r->partial=1;return;}
    GcVehicle *v=&r->vehicles[r->count++];snprintf(v->name,sizeof(v->name),"%s",name);v->flags=flags;
}
static int parse(Source *s,uint64_t base,uint64_t span,int depth,GcVehicleReport *r,GcVehicleProgress progress,void *user) {
    unsigned char hdr[16];
    if(depth>4 || span<16 || read_at(s,base,hdr,16))return -1;
    uint32_t ec=u32le(hdr+4),raw=u32le(hdr+8),nl=raw&0xfffffff,shift=raw>>28;
    if(u32le(hdr)!=0x52504637 || !ec || ec>200000 || !nl || nl>8*1024*1024 || shift>7 || u32le(hdr+12)!=0x4e45504f)return -1;
    size_t toc=(size_t)ec*16,total=toc+nl;
    if(total>span-16)return -1;
    unsigned char *data=malloc(total);if(!data)return -1;
    if(read_at(s,base+16,data,total)) {free(data);return -1;}
    r->archives++;if(progress)progress(user);
    const unsigned char *pool=data+toc;
    for(uint32_t i=0;i<ec;i++) {
        const unsigned char *e=data+i*16;uint64_t q=u64le(e);
        uint32_t off=(uint32_t)((q>>40)&0x7fffff),at=(uint32_t)(q&0xffff)<<shift;
        int resource=(int)(q>>63);
        if(!resource && off==0x7fffff)continue;
        if(at>=nl || !memchr(pool+at,0,nl-at)) {r->partial=1;continue;}
        const char *rawname=(const char *)pool+at;size_t n=strlen(rawname);
        if(n>=96) {r->partial=1;continue;}
        char name[96];for(size_t k=0;k<=n;k++) {unsigned char ch=(unsigned char)rawname[k];name[k]=(char)(ch>='A'&&ch<='Z'?ch+32:ch);}
        if(suffix(name,".nft") || suffix(name,".yft")) {add(r,name);continue;}
        if(resource || !suffix(name,".rpf"))continue;
        uint32_t packed=(uint32_t)((q>>16)&0xffffff),size=u32le(e+8);
        uint64_t child=(uint64_t)off*512,stored=packed?packed:size;
        if(!stored || child>span || stored>span-child) {r->partial=1;continue;}
        if(packed && packed!=size) {
            /* Compressed containers require decoding; cap both allocations. */
            if(size>64*1024*1024 || packed>64*1024*1024) {r->partial=1;continue;}
            unsigned char *in=malloc(packed),*out=malloc(size);int ok=0;
            if(in && out && !read_at(s,base+child,in,packed)) for(int wb=-15;wb<=15 && !ok;wb+=30) {
                z_stream z;memset(&z,0,sizeof(z));if(inflateInit2(&z,wb)!=Z_OK)continue;
                z.next_in=in;z.avail_in=packed;z.next_out=out;z.avail_out=size;
                int rc=inflate(&z,Z_FINISH);ok=rc==Z_STREAM_END && z.total_out==size && z.total_in==packed;inflateEnd(&z);
            }
            if(ok) {Source sub={NULL,out,size};if(parse(&sub,0,size,depth+1,r,progress,user))r->partial=1;}
            else r->partial=1;
            free(in);free(out);
        } else if(parse(s,base+child,stored,depth+1,r,progress,user))r->partial=1;
    }
    free(data);return 0;
}
static int compare(const void *a,const void *b) {return strcmp(((const GcVehicle *)a)->name,((const GcVehicle *)b)->name);}
int gc_vehicle_inspect(const char *path,GcVehicleReport *r,GcVehicleProgress progress,void *user,char *err,size_t cap) {
    if(!r)return -1;
    memset(r,0,sizeof(*r));
    FILE *f=fopen(path,"rb");if(!f) {if(err&&cap)snprintf(err,cap,"Cannot open dlc.rpf");return -1;}
    int rc=-1;
    if(!fseeko(f,0,SEEK_END)) {long long size=ftello(f);if(size>0) {Source s={f,NULL,(uint64_t)size};rc=parse(&s,0,(uint64_t)size,0,r,progress,user);}}
    fclose(f);qsort(r->vehicles,(size_t)r->count,sizeof(r->vehicles[0]),compare);
    if(err&&cap)snprintf(err,cap,"%s",rc?"Unsupported or incomplete RPF archive":r->partial?"Some nested resources could not be inspected":"");
    return rc;
}
