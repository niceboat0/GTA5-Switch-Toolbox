#pragma once
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t Result;
#define R_SUCCEEDED(rc) ((rc)==0)
#define R_FAILED(rc) ((rc)!=0)
extern char gc_host_archive_path[1024];
#define GC_SCRIPT_UPDATE2 gc_host_archive_path
#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif
static inline Result fsdevMountSdmc(void) { return 0; }
static inline Result fsdevCommitDevice(const char *name) { (void)name;return 0; }
