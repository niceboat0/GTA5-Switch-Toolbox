#include "gc_package.h"
#include "gc_rpf.h"
#include "lang.h"
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#else
#define MAKE_DIR(path) mkdir(path,0777)
#endif

#define CHUNK 65536
#define MAX_TARGETS 24
#ifndef GC_PACKAGE_RENAME
#define GC_PACKAGE_RENAME rename
#endif
static GcPackageProgress active_progress;
static void *active_progress_user;
typedef struct { char relative[160], path[GC_PACKAGE_PATH], stage[GC_PACKAGE_PATH], backup[GC_PACKAGE_PATH];int existed;char hash[65]; } Target;
static int error(char *err,size_t cap,const char *message) {if(err && cap)snprintf(err,cap,"%s",message);return -1;}
static int exists(const char *path) {struct stat s;return stat(path,&s)==0;}
static int safe(const char *path) {
    if(!path || !*path || *path=='/' || strchr(path,'\\') || strchr(path,':') || strchr(path,'|'))return 0;
    const char *s=path;
    while(*s) {
        const char *end=strchr(s,'/');size_t n=end?(size_t)(end-s):strlen(s);
        if(!n || (n==1 && *s=='.') || (n==2 && !memcmp(s,"..",2)))return 0;
        for(size_t i=0;i<n;++i)if((unsigned char)s[i]<32 || s[i]=='\x7f')return 0;
        if(!end)break;
        s=end+1;
        if(!*s)return 0;
    }
    return 1;
}
static int join(char *out,size_t cap,const char *base,const char *relative) {
    return safe(relative) && snprintf(out,cap,"%s/%s",base,relative)>0 && strlen(base)+strlen(relative)+2<=cap;
}
static int parents(const char *path) {
    char p[GC_PACKAGE_PATH];if(strlen(path)>=sizeof(p))return -1;strcpy(p,path);
    for(char *s=p+1;*s;++s)if(*s=='/') {
        if(s[-1]==':')continue;
        *s=0;int rc=MAKE_DIR(p);*s='/';if(rc && errno!=EEXIST)return -1;
    }
    return 0;
}
static int hash_file(const char *path,char out[65]) {
    FILE *f=fopen(path,"rb");if(!f)return -1;
    struct stat info;unsigned long long done=0,total=!stat(path,&info)?(unsigned long long)info.st_size:0;
    unsigned char bytes[CHUNK],digest[32];mbedtls_sha256_context c;mbedtls_sha256_init(&c);
    int bad=mbedtls_sha256_starts_ret(&c,0);size_t n;
    while(!bad && (n=fread(bytes,1,sizeof(bytes),f))) {
        bad=mbedtls_sha256_update_ret(&c,bytes,n);done+=n;
        if(active_progress)active_progress(done,total,active_progress_user);
    }
    if(ferror(f))bad=1;
    if(!bad)bad=mbedtls_sha256_finish_ret(&c,digest);
    mbedtls_sha256_free(&c);if(fclose(f))bad=1;if(bad)return -1;
    for(int i=0;i<32;++i)snprintf(out+i*2,3,"%02x",digest[i]);
    return 0;
}
static int hash_bytes(const unsigned char *data,size_t n,char out[65]) {
    unsigned char digest[32];if(mbedtls_sha256_ret(data,n,digest,0))return -1;
    for(int i=0;i<32;++i)snprintf(out+i*2,3,"%02x",digest[i]);
    return 0;
}
static int copy(const char *from,const char *to,GcPackageProgress callback,void *user) {
    if(parents(to))return -1;
    FILE *a=fopen(from,"rb"),*b=NULL;int bad=0;
    if(!a)return -1;
    b=fopen(to,"wb");if(!b){fclose(a);return -1;}
    if(fseeko(a,0,SEEK_END) || ftello(a)<0){fclose(a);fclose(b);return -1;}
    unsigned long long total=(unsigned long long)ftello(a),done=0;
    if(fseeko(a,0,SEEK_SET)){fclose(a);fclose(b);return -1;}
    unsigned char chunk[CHUNK];size_t n;
    while((n=fread(chunk,1,sizeof(chunk),a))) {
        if(fwrite(chunk,1,n,b)!=n){bad=1;break;}done+=n;
        if(callback)callback(done,total,user);
    }
    if(ferror(a) || fflush(b))bad=1;
    if(fclose(a))bad=1;
    if(fclose(b))bad=1;
    return bad?-1:0;
}
static int valid_hash(const char *h) {
    if(strlen(h)!=64)return 0;
    for(int i=0;i<64;++i)if(!((h[i]>='0' && h[i]<='9') || (h[i]>='a' && h[i]<='f')))return 0;
    return 1;
}
static int allowed(const GcPackage *p,const GcPackageFile *f) {
    if(!safe(f->target) || !safe(f->source) || !valid_hash(f->hash) ||
       (strcmp(f->before,"-") && !valid_hash(f->before)))return 0;
    if(!strcmp(f->mode,"RPF")) {
        if(strcmp(f->target,"romfs/update/update.rpf") || !safe(f->entry))return 0;
        if(!strcmp(p->kind,"selfradio"))
            return !strcmp(f->entry,"common/data/ui/pausemenu.xml") || !strcmp(f->entry,"switch/audio/config/game.dat151.rel") ||
                   !strcmp(f->entry,"switch/data/cdimages/scaleform_frontend.rpf") ||
                   (!strncmp(f->entry,"switch/patch/data/lang/",22) && strstr(f->entry,"_rel.rpf"));
        size_t n=strlen(f->entry);return !strcmp(p->kind,"radio") && n>10 && !strcmp(f->entry+n-10,".dat54.rel");
    }
    if(strcmp(f->mode,"FILE") || *f->entry)return 0;
    if(!strcmp(p->kind,"selfradio"))return !strcmp(f->target,"exefs/subsdk4") || !strcmp(f->target,"exefs/main.npdm");
    size_t n=strlen(f->target);
    return !strcmp(p->kind,"radio") && !strncmp(f->target,"romfs/switch/audio/",19) && n>4 && !strcmp(f->target+n-4,".rpf");
}
int gc_package_read(const char *folder,GcPackage *p,char *err,size_t cap) {
    char path[GC_PACKAGE_PATH];if(!join(path,sizeof(path),folder,"package.txt"))return error(err,cap,TR("安装包路径太长","Package path is too long"));
    FILE *f=fopen(path,"rb");if(!f)return error(err,cap,TR("找不到 package.txt","package.txt was not found"));
    memset(p,0,sizeof(*p));snprintf(p->folder,sizeof(p->folder),"%s",folder);
    char line[1024];int first=1,bad=0;
    while(fgets(line,sizeof(line),f)) {
        size_t n=strlen(line);if(n==sizeof(line)-1 && line[n-1]!='\n'){bad=1;break;}
        line[strcspn(line,"\r\n")]=0;if(!*line || *line=='#')continue;
        if(first){first=0;if(strcmp(line,"GTA5-TOOLBOX-PACK|1")){bad=1;break;}continue;}
        char *v[7]={line};int count=1;
        for(char *s=line;*s;++s)if(*s=='|'){*s=0;if(count>=7){bad=1;break;}v[count++]=s+1;}
        if(bad)break;
        if(count==2) {
            char *dest=NULL;size_t room=0;
            if(!strcmp(v[0],"id")){dest=p->id;room=sizeof(p->id);}
            else if(!strcmp(v[0],"title")){dest=p->title;room=sizeof(p->title);}
            else if(!strcmp(v[0],"title_zh")){dest=p->title_zh;room=sizeof(p->title_zh);}
            else if(!strcmp(v[0],"kind")){dest=p->kind;room=sizeof(p->kind);}
            else {bad=1;break;}
            if(*dest || strlen(v[1])>=room){bad=1;break;}strcpy(dest,v[1]);
        } else if(count==6 && p->count<GC_PACKAGE_MAX_FILES) {
            GcPackageFile *a=&p->files[p->count++];char *fields[]={a->mode,a->target,a->entry,a->source,a->hash,a->before};
            size_t rooms[]={sizeof(a->mode),sizeof(a->target),sizeof(a->entry),sizeof(a->source),sizeof(a->hash),sizeof(a->before)};
            for(int i=0;i<6;++i){if(strlen(v[i])>=rooms[i]){bad=1;break;}strcpy(fields[i],v[i]);}
        } else {bad=1;break;}
    }
    if(ferror(f))bad=1;
    if(fclose(f))bad=1;
    if(!*p->id || !safe(p->id) || strchr(p->id,'/') || !*p->title || !p->count)bad=1;
    int audio=0,timing=0;
    for(int i=0;i<p->count;++i) {
        if(!allowed(p,&p->files[i]))bad=1;
        for(int j=0;j<i;++j)if(!strcmp(p->files[i].target,p->files[j].target) && !strcmp(p->files[i].entry,p->files[j].entry))bad=1;
        if(strstr(p->files[i].target,"/sfx/"))audio=1;
        if(strstr(p->files[i].target,"audio_rel.rpf") || strstr(p->files[i].entry,".dat54.rel"))timing=1;
    }
    if(!strcmp(p->kind,"radio") && (!audio || !timing))bad=1;
    return bad?error(err,cap,TR("安装包清单无效（音频包必须包含时间元数据）","Invalid package manifest (radio imports must include timing metadata)")):0;
}
int gc_package_scan(GcPackage *out,int max,const char *kind) {
    parents(GC_PACKAGE_HOME "/.folder");DIR *d=opendir(GC_PACKAGE_HOME);if(!d)return 0;
    int n=0;struct dirent *item;
    while(n<max && (item=readdir(d))) {
        if(item->d_name[0]=='.' || !safe(item->d_name))continue;
        char p[GC_PACKAGE_PATH];GcPackage candidate;
        if(join(p,sizeof(p),GC_PACKAGE_HOME,item->d_name) && !gc_package_read(p,&candidate,NULL,0) && !strcmp(candidate.kind,kind))out[n++]=candidate;
    }
    closedir(d);return n;
}
/* Resolve the complete RPF path via directory ownership, never substring-match. */
static int find_entry(GcRpf *r,const char *path,GcEntry *out) {
    unsigned index=0;const char *s=path;
    for(unsigned depth=0;depth<16;++depth) {
        GcEntry dir;if(gc_rpf_entry(r,(int)index,&dir) || !dir.is_dir || dir.first>r->entry_count || dir.count>r->entry_count-dir.first)return -1;
        const char *slash=strchr(s,'/');size_t len=slash?(size_t)(slash-s):strlen(s);int found=-1;
        for(unsigned i=dir.first;i<dir.first+dir.count;++i) {
            GcEntry e;if(gc_rpf_entry(r,(int)i,&e))return -1;
            if(strlen(e.name)==len && !strncmp(e.name,s,len)){if(found!=-1)return -1;found=(int)i;*out=e;}
        }
        if(found<0)return -1;
        if(!slash)return out->is_dir || out->is_res || r->toc[out->idx*16+12] ||
            r->toc[out->idx*16+13] || r->toc[out->idx*16+14] || r->toc[out->idx*16+15]?-1:0;
        index=(unsigned)found;s=slash+1;
    }
    return -1;
}
static int entry_hash(const char *archive,const char *entry,char hash[65]) {
    GcRpf r;GcEntry e;u8 *data=NULL;size_t n=0;
    if(gc_rpf_open(archive,&r))return -1;
    int rc=find_entry(&r,entry,&e);
    if(!rc)rc=gc_rpf_read(&r,&e,&data,&n);
    if(!rc)rc=hash_bytes(data,n,hash);
    free(data);gc_rpf_close(&r);return rc;
}
static int replace_entry(const char *archive,const char *entry,const char *source) {
    GcRpf r;GcEntry e;if(gc_rpf_open(archive,&r))return -1;
    if(r.encryption!=0x4e45504f || find_entry(&r,entry,&e)){gc_rpf_close(&r);return -1;}
    FILE *src=fopen(source,"rb");if(!src){gc_rpf_close(&r);return -1;}
    int bad=fseeko(src,0,SEEK_END);long long bytes=ftello(src);
    if(bytes<=0 || bytes>0xffffffffLL)bad=1;
    if(fseeko(src,0,SEEK_SET) || fseeko(r.fp,0,SEEK_END))bad=1;
    long long start=(ftello(r.fp)+511)&~511LL;
    if(start<0 || start/512>0x7fffff || fseeko(r.fp,start,SEEK_SET))bad=1;
    unsigned char buf[CHUNK];size_t n;
    while(!bad && (n=fread(buf,1,sizeof(buf),src)))if(fwrite(buf,1,n,r.fp)!=n)bad=1;
    if(!bad) {
        unsigned pad=(unsigned)((512-(bytes&511))&511);unsigned char zero[512]={0};
        if(pad && fwrite(zero,1,pad,r.fp)!=pad)bad=1;
    }
    if(ferror(src))bad=1;
    if(fclose(src))bad=1;
    if(!bad) {
        u8 *record=r.toc+e.idx*16;uint64_t q=(uint64_t)(e.name_off&0xffff)|((uint64_t)(start/512)<<40);
        for(int i=0;i<8;++i)record[i]=(u8)(q>>(i*8));
        for(int i=0;i<4;++i)record[8+i]=(u8)((uint32_t)bytes>>(i*8));
        memset(record+12,0,4);
        if(fseeko(r.fp,16+(long long)e.idx*16,SEEK_SET) || fwrite(record,1,16,r.fp)!=16 || fflush(r.fp))bad=1;
    }
    gc_rpf_close(&r);return bad?-1:0;
}
static int targets(const GcPackage *p,Target *out) {
    if(!p || p->count<1 || p->count>GC_PACKAGE_MAX_FILES || !safe(p->id) || strchr(p->id,'/') ||
       (strcmp(p->kind,"selfradio") && strcmp(p->kind,"radio")))return -1;
    int n=0;
    for(int i=0;i<p->count;++i) {
        if(!allowed(p,&p->files[i]))return -1;
        int found=0;for(int j=0;j<n;++j)if(!strcmp(out[j].relative,p->files[i].target))found=1;
        if(found)continue;
        if(n==MAX_TARGETS)return -1;
        Target *t=&out[n++];memset(t,0,sizeof(*t));strcpy(t->relative,p->files[i].target);
        if(!join(t->path,sizeof(t->path),GC_PACKAGE_GAME,t->relative))return -1;
        if(snprintf(t->stage,sizeof(t->stage),"%s.toolbox-new",t->path)>=(int)sizeof(t->stage))return -1;
        char relative[256];snprintf(relative,sizeof(relative),".backups/%s/%d.bin",p->id,n-1);
        if(!join(t->backup,sizeof(t->backup),GC_PACKAGE_HOME,relative))return -1;
        t->existed=exists(t->path);
    }
    return n;
}
static int journal(Target *targets,int n,const char *path) {
    char temp[GC_PACKAGE_PATH];
    if(parents(path) || snprintf(temp,sizeof(temp),"%s.new",path)>=(int)sizeof(temp))return -1;
    FILE *f=fopen(temp,"wb");if(!f)return -1;
    int bad=0;
    for(int i=0;i<n;++i)if(fprintf(f,"%s|%s|%s|%d|%s\n",targets[i].path,targets[i].stage,targets[i].backup,targets[i].existed,targets[i].hash)<0){bad=1;break;}
    if(fflush(f))bad=1;
    if(fclose(f))bad=1;
    if(R_FAILED(fsdevCommitDevice("sdmc")))bad=1;
    if(bad){remove(temp);return -1;}
    if(GC_PACKAGE_RENAME(temp,path) || R_FAILED(fsdevCommitDevice("sdmc")))return -1;
    return 0;
}
int gc_package_recover(char *err,size_t cap) {
    FILE *f=fopen(GC_PACKAGE_HOME "/.pending","rb");
    if(!f)return exists(GC_PACKAGE_HOME "/.pending")?error(err,cap,TR("无法读取恢复记录","Cannot read recovery record")):0;
    Target rows[MAX_TARGETS];int n=0;char line[1600];int bad=0;
    while(fgets(line,sizeof(line),f)) {
        if(n==MAX_TARGETS){bad=1;break;}
        Target *t=&rows[n++];char *parts[5]={line};int k=1;
        for(char *p=line;*p;++p)if(*p=='|'){*p=0;if(k>=5){bad=1;break;}parts[k++]=p+1;}
        if(bad || k!=5 || strlen(parts[0])>=sizeof(t->path) || strlen(parts[1])>=sizeof(t->stage) || strlen(parts[2])>=sizeof(t->backup)){bad=1;break;}
        parts[4][strcspn(parts[4],"\r\n")]=0;
        if(!valid_hash(parts[4]) || (strcmp(parts[3],"0") && strcmp(parts[3],"1"))){bad=1;break;}
        strcpy(t->path,parts[0]);strcpy(t->stage,parts[1]);strcpy(t->backup,parts[2]);t->existed=atoi(parts[3]);strcpy(t->hash,parts[4]);
        /* Journal is tool-owned; still constrain paths before any recovery. */
        size_t root=strlen(GC_PACKAGE_GAME),home=strlen(GC_PACKAGE_HOME);
        if(strncmp(t->path,GC_PACKAGE_GAME,root) || t->path[root]!='/' || !safe(t->path+root+1) ||
           strncmp(t->backup,GC_PACKAGE_HOME,home) || t->backup[home]!='/' || !safe(t->backup+home+1) ||
           strncmp(t->stage,t->path,strlen(t->path)) || strcmp(t->stage+strlen(t->path),".toolbox-new")){bad=1;break;}
    }
    if(ferror(f) || !n)bad=1;
    if(fclose(f))bad=1;
    if(bad)return error(err,cap,TR("恢复记录损坏；请手动恢复备份","Recovery record is invalid; restore the backup manually"));
    for(int i=n-1;i>=0;--i) {
        Target *t=&rows[i];
        char current[65];
        if((exists(t->backup) || !t->existed) && exists(t->path) &&
           (hash_file(t->path,current) || strcmp(current,t->hash)))
            return error(err,cap,TR("恢复过程中发现其他修改；已保留文件","Recovery found other edits; files were preserved"));
        if(exists(t->backup)) {if(exists(t->path) && remove(t->path))return -1;if(GC_PACKAGE_RENAME(t->backup,t->path))return -1;}
        else if(!t->existed && exists(t->path) && remove(t->path))return -1;
        if(exists(t->stage) && remove(t->stage))return -1;
    }
    if(remove(GC_PACKAGE_HOME "/.pending") || R_FAILED(fsdevCommitDevice("sdmc")))return -1;
    return 0;
}
int gc_package_state(const GcPackage *p) {
    if(!p || p->count<1 || p->count>GC_PACKAGE_MAX_FILES)return 0;
    char path[GC_PACKAGE_PATH],hash[65];
    for(int i=0;i<p->count;++i) {
        const GcPackageFile *f=&p->files[i];if(!join(path,sizeof(path),GC_PACKAGE_GAME,f->target))return 0;
        if((!strcmp(f->mode,"RPF")?entry_hash(path,f->entry,hash):hash_file(path,hash)) || strcmp(hash,f->hash))return 0;
    }
    return 1;
}
static int install(const GcPackage *p,GcPackageProgress callback,void *user,char *err,size_t cap) {
    if(gc_package_recover(err,cap))return -1;
    Target plan[MAX_TARGETS];int n=targets(p,plan);if(n<1)return -1;
    char source[GC_PACKAGE_PATH],path[GC_PACKAGE_PATH],hash[65];int bad=0;
    for(int i=0;i<p->count;++i) {
        const GcPackageFile *f=&p->files[i];
        if(!allowed(p,f) || !join(source,sizeof(source),p->folder,f->source) || hash_file(source,hash) || strcmp(hash,f->hash))
            return error(err,cap,TR("安装包文件缺失或校验失败","Package file is missing or failed verification"));
        if(!join(path,sizeof(path),GC_PACKAGE_GAME,f->target))return -1;
        if(exists(path) && ((!strcmp(f->mode,"RPF")?entry_hash(path,f->entry,hash):hash_file(path,hash)) ||
           (strcmp(hash,f->hash) && strcmp(hash,f->before))))
            return error(err,cap,TR("目标已被其他模组修改；为避免覆盖已停止","Target was modified by another mod; stopped to preserve it"));
        if(!exists(path) && !strcmp(f->mode,"RPF"))return error(err,cap,TR("缺少兼容的 update.rpf","Compatible update.rpf is missing"));
    }
    if(gc_package_state(p))return 0;
    for(int i=0;i<n;++i) {
        if(exists(plan[i].backup))return error(err,cap,TR("已有安装备份；请先还原","An installation backup exists; restore it first"));
        if(parents(plan[i].stage) || parents(plan[i].backup))return -1;
        if(plan[i].existed && copy(plan[i].path,plan[i].stage,callback,user)){bad=1;break;}
    }
    for(int i=0;!bad && i<p->count;++i) {
        const GcPackageFile *f=&p->files[i];int at=0;while(at<n && strcmp(plan[at].relative,f->target))++at;
        if(at==n || !join(source,sizeof(source),p->folder,f->source)){bad=1;break;}
        bad=!strcmp(f->mode,"RPF")?replace_entry(plan[at].stage,f->entry,source):copy(source,plan[at].stage,callback,user);
        if(!bad)bad=(!strcmp(f->mode,"RPF")?entry_hash(plan[at].stage,f->entry,hash):hash_file(plan[at].stage,hash)) || strcmp(hash,f->hash);
    }
    for(int i=0;!bad && i<n;++i)bad=hash_file(plan[i].stage,plan[i].hash);
    if(bad){for(int i=0;i<n;++i)if(exists(plan[i].stage))remove(plan[i].stage);return error(err,cap,TR("准备安装失败（检查 SD 卡空间）；游戏文件未更改","Staging failed (check SD space); game files were not changed"));}
    char record[GC_PACKAGE_PATH],relative[160];snprintf(relative,sizeof(relative),".backups/%s/installed.txt",p->id);
    if(!join(record,sizeof(record),GC_PACKAGE_HOME,relative) || journal(plan,n,GC_PACKAGE_HOME "/.pending"))return -1;
    for(int i=0;i<n;++i) {
        if((plan[i].existed && GC_PACKAGE_RENAME(plan[i].path,plan[i].backup)) || GC_PACKAGE_RENAME(plan[i].stage,plan[i].path)) {
            gc_package_recover(NULL,0);return error(err,cap,TR("提交失败，已尝试恢复原文件","Commit failed; original-file recovery was attempted"));
        }
    }
    if(journal(plan,n,record) || remove(GC_PACKAGE_HOME "/.pending")) {gc_package_recover(NULL,0);return -1;}
    if(R_FAILED(fsdevCommitDevice("sdmc")))return error(err,cap,TR("SD 提交失败，请检查文件","SD commit failed; check the files"));
    if(!strcmp(p->kind,"selfradio"))parents(GC_PACKAGE_GAME "/romfs/User Music/.folder");
    return 0;
}
int gc_package_install(const GcPackage *p,GcPackageProgress callback,void *user,char *err,size_t cap) {
    active_progress=callback;active_progress_user=user;
    int rc=install(p,callback,user,err,cap);
    active_progress=NULL;active_progress_user=NULL;return rc;
}
static int restore(const GcPackage *p,char *err,size_t cap) {
    if(gc_package_recover(err,cap))return -1;
    Target plan[MAX_TARGETS];int n=targets(p,plan);if(n<1)return -1;
    char record[GC_PACKAGE_PATH],relative[160];snprintf(relative,sizeof(relative),".backups/%s/installed.txt",p->id);
    if(!join(record,sizeof(record),GC_PACKAGE_HOME,relative))return -1;
    FILE *f=fopen(record,"rb");if(!f)return error(err,cap,TR("此安装包没有备份","No backup exists for this package"));
    char line[1600],hash[65];int i=0,bad=0;
    while(fgets(line,sizeof(line),f)) {
        char *last=strrchr(line,'|');if(!last || i>=n){bad=1;break;}last[strcspn(last,"\r\n")]=0;
        if(hash_file(plan[i].path,hash) || strcmp(hash,last+1)){bad=1;break;}
        ++i;
    }
    fclose(f);if(bad || i!=n)return error(err,cap,TR("安装后的文件又被修改；已停止还原以保留修改","Installed files changed again; restore stopped to preserve edits"));
    /* Reuse the recovery journal and its constrained paths. */
    if(copy(record,GC_PACKAGE_HOME "/.pending.new",NULL,NULL) || R_FAILED(fsdevCommitDevice("sdmc")) ||
       GC_PACKAGE_RENAME(GC_PACKAGE_HOME "/.pending.new",GC_PACKAGE_HOME "/.pending") || R_FAILED(fsdevCommitDevice("sdmc")))return -1;
    if(gc_package_recover(err,cap))return -1;
    return remove(record)?-1:0;
}
int gc_package_restore_progress(const GcPackage *p,GcPackageProgress callback,void *user,char *err,size_t cap) {
    active_progress=callback;active_progress_user=user;
    int rc=restore(p,err,cap);
    active_progress=NULL;active_progress_user=NULL;return rc;
}
int gc_package_restore(const GcPackage *p,char *err,size_t cap) {
    return gc_package_restore_progress(p,NULL,NULL,err,cap);
}
int gc_package_recover_progress(GcPackageProgress callback,void *user,char *err,size_t cap) {
    active_progress=callback;active_progress_user=user;
    int rc=gc_package_recover(err,cap);
    active_progress=NULL;active_progress_user=NULL;return rc;
}
