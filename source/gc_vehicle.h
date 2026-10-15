#ifndef GC_VEHICLE_H
#define GC_VEHICLE_H
#include <stddef.h>
#define GC_VEHICLE_MAX 512
typedef struct {char name[96];unsigned flags;} GcVehicle; /* 1=base, 2=_hi */
typedef struct {int count,partial,archives;GcVehicle vehicles[GC_VEHICLE_MAX];} GcVehicleReport;
typedef void (*GcVehicleProgress)(void *);
/* Reads archive headers and nested containers, never whole uncompressed packs. */
int gc_vehicle_inspect(const char *,GcVehicleReport *,GcVehicleProgress,void *,char *,size_t);
#endif
