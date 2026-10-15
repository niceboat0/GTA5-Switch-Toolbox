#pragma once
#ifdef _WIN32
/* Host SHA fixture uses Windows SHA256; production uses devkitPro mbedTLS. */
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
typedef struct {BCRYPT_ALG_HANDLE algorithm;BCRYPT_HASH_HANDLE hash;} mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c){c->algorithm=NULL;c->hash=NULL;}
static inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context *c,int is224){
    if(is224 || BCryptOpenAlgorithmProvider(&c->algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)return -1;
    return BCryptCreateHash(c->algorithm,&c->hash,NULL,0,NULL,0,0)<0?-1:0;
}
static inline int mbedtls_sha256_update_ret(mbedtls_sha256_context *c,const unsigned char *p,size_t n){return n>0xffffffffu || BCryptHashData(c->hash,(PUCHAR)p,(ULONG)n,0)<0?-1:0;}
static inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context *c,unsigned char out[32]){return BCryptFinishHash(c->hash,out,32,0)<0?-1:0;}
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c){if(c->hash)BCryptDestroyHash(c->hash);if(c->algorithm)BCryptCloseAlgorithmProvider(c->algorithm,0);}
static inline int mbedtls_sha256_ret(const unsigned char *p,size_t n,unsigned char out[32],int is224){
    mbedtls_sha256_context c;mbedtls_sha256_init(&c);int rc=mbedtls_sha256_starts_ret(&c,is224);
    if(!rc)rc=mbedtls_sha256_update_ret(&c,p,n);if(!rc)rc=mbedtls_sha256_finish_ret(&c,out);mbedtls_sha256_free(&c);return rc;
}

#else
#include <openssl/sha.h>
typedef SHA256_CTX mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c){(void)c;}
static inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context *c,int is224){return is224 || !SHA256_Init(c)?-1:0;}
static inline int mbedtls_sha256_update_ret(mbedtls_sha256_context *c,const unsigned char *p,size_t n){return SHA256_Update(c,p,n)?0:-1;}
static inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context *c,unsigned char out[32]){return SHA256_Final(out,c)?0:-1;}
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c){(void)c;}
static inline int mbedtls_sha256_ret(const unsigned char *p,size_t n,unsigned char out[32],int is224){return is224 || !SHA256(p,n,out)?-1:0;}
#endif
