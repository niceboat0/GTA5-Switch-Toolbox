#include "gc_nsc.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define NSC_LIMIT (16u * 1024u * 1024u)
static uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t u64le(const uint8_t *p) { return u32le(p)|((uint64_t)u32le(p+4)<<32); }
static unsigned lower(unsigned ch) { return ch>='A' && ch<='Z'?ch+32:ch; }
static uint32_t joaat(const char *name) {
    uint32_t h=0;
    for (;*name;++name) {h+=lower((unsigned char)*name);h+=h<<10;h^=h>>6;}
    h+=h<<3;h^=h>>11;h+=h<<15;return h;
}
static int range(size_t size,uint64_t at,uint64_t length) {
    return at<=size && length<=size-at;
}
static int pointer(uint64_t p) { return (p>>24)==0x50; }
static int fail(char *err,size_t n,const char *message) {
    if (err && n) snprintf(err,n,"%s",message);
    return -1;
}
int gc_nsc_validate(const uint8_t *p,size_t n,const char *entry,char *err,size_t cap) {
    if (!p || n<0x78 || n>NSC_LIMIT || !entry)
        return fail(err,cap,TR("脚本头部不完整","Incomplete script header"));
    size_t length=strlen(entry);
    if (length<5 || length>=96 || strchr(entry,'/') || strchr(entry,'\\') ||
        strchr(entry,':') || strchr(entry,'\t') || strchr(entry,'\n') ||
        lower(entry[length-4])!='.' || lower(entry[length-3])!='n' ||
        lower(entry[length-2])!='s' || lower(entry[length-1])!='c')
        return fail(err,cap,TR("无效的 .nsc 条目名","Invalid .nsc entry name"));
    const uint64_t base=u64le(p);
    if (base<UINT64_C(0x0000700000000000) || base>=UINT64_C(0x0000800000000000))
        return fail(err,cap,TR("脚本不是支持的 payload 格式","Unsupported script payload layout"));
    const uint64_t name_ptr=u64le(p+0x60),names=name_ptr&0xffffff;
    if (!pointer(name_ptr) || names>=n)
        return fail(err,cap,TR("脚本名称指针越界","Script name pointer is out of bounds"));
    size_t end=(size_t)names;
    while (end<n && end-names<96 && p[end]) ++end;
    if (end==n || end==names || end-names>=96)
        return fail(err,cap,TR("脚本名称损坏","Invalid embedded script name"));
    if (end-names!=length-4)
        return fail(err,cap,TR("条目名必须与编译的脚本名称相同；不要重命名脚本","Entry name must match the compiled script name; do not rename scripts"));
    for (size_t i=0;i<length-4;++i)
        if (lower(p[names+i])!=lower((unsigned char)entry[i]))
            return fail(err,cap,TR("条目名与脚本名称不匹配","Entry name does not match the embedded script name"));
    if (u32le(p+0x58)!=joaat((const char *)p+names))
        return fail(err,cap,TR("脚本名称哈希损坏","Script name hash is invalid"));
    const uint32_t code=u32le(p+0x1c),native_count=u32le(p+0x2c),strings=u32le(p+0x70);
    const uint64_t code_table=u64le(p+0x10),native_table=u64le(p+0x40),string_table=u64le(p+0x68);
    const uint32_t blocks=code/0x4000+(code%0x4000!=0);
    if (!code || code>NSC_LIMIT || !pointer(code_table) ||
        !range(n,code_table&0xffffff,(uint64_t)blocks*8))
        return fail(err,cap,TR("脚本代码表越界","Script code table is out of bounds"));
    for (uint32_t i=0;i<blocks;++i) {
        uint64_t at=u64le(p+(code_table&0xffffff)+i*8);
        uint32_t remain=code-i*0x4000,bytes=remain<0x4000?remain:0x4000;
        if (!pointer(at) || !range(n,at&0xffffff,bytes))
            return fail(err,cap,TR("脚本代码块越界","Script code block is out of bounds"));
    }
    if (native_count && (!pointer(native_table) || !range(n,native_table&0xffffff,(uint64_t)native_count*8)))
        return fail(err,cap,TR("脚本 native 表越界","Script native table is out of bounds"));
    if (strings && (!pointer(string_table) ||
        !range(n,string_table&0xffffff,(uint64_t)(strings/0x4000+(strings%0x4000!=0))*8)))
        return fail(err,cap,TR("脚本文本表越界","Script string table is out of bounds"));
    for (uint32_t i=0;i<strings/0x4000+(strings%0x4000!=0);++i) {
        uint64_t at=u64le(p+(string_table&0xffffff)+i*8);
        uint32_t remain=strings-i*0x4000,bytes=remain<0x4000?remain:0x4000;
        if (!pointer(at) || !range(n,at&0xffffff,bytes))
            return fail(err,cap,TR("脚本文本块越界","Script string block is out of bounds"));
    }
    if (err && cap) err[0]=0;
    return 0;
}
int gc_nsc_decode_validate(const uint8_t *file,size_t n,const char *entry,char *err,size_t cap) {
    if (!file || n<4) return fail(err,cap,TR("脚本文件不完整","Incomplete script file"));
    if (memcmp(file,"RSC7",4)) return gc_nsc_validate(file,n,entry,err,cap);
    if (n<=16) return fail(err,cap,TR("RSC7 文件不完整","Incomplete RSC7 file"));
    /* Trial decode, never guess compression from resource flags. */
    if (!gc_nsc_validate(file+16,n-16,entry,NULL,0))
        return gc_nsc_validate(file+16,n-16,entry,err,cap);
    uint8_t *decoded=(uint8_t *)malloc(NSC_LIMIT);
    if (!decoded) return fail(err,cap,TR("脚本校验内存不足","Not enough memory to validate the script"));
    z_stream stream;memset(&stream,0,sizeof(stream));
    int result=-1;
    if (inflateInit2(&stream,-15)==Z_OK) {
        stream.next_in=(Bytef *)(file+16);stream.avail_in=(uInt)(n-16);
        stream.next_out=decoded;stream.avail_out=NSC_LIMIT;
        int z=inflate(&stream,Z_FINISH);
        if (z==Z_STREAM_END && stream.avail_in==0)
            result=gc_nsc_validate(decoded,stream.total_out,entry,err,cap);
        else fail(err,cap,TR("RSC7 压缩流损坏或过大","RSC7 stream is corrupt or exceeds the size limit"));
        inflateEnd(&stream);
    } else fail(err,cap,TR("无法初始化脚本解压","Cannot initialize script decompression"));
    free(decoded);return result;
}
