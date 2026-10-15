#ifndef GC_NSC_H
#define GC_NSC_H
#include <stddef.h>
#include <stdint.h>

/* Payload checks only: successful validation is not game-build/native proof. */
int gc_nsc_validate(const uint8_t *payload, size_t size, const char *entry_name,
                    char *err, size_t errsz);
int gc_nsc_decode_validate(const uint8_t *file, size_t size, const char *entry_name,
                           char *err, size_t errsz);
#endif
