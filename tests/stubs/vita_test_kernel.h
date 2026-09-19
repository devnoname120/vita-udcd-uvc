#ifndef VITA_TEST_KERNEL_H
#define VITA_TEST_KERNEL_H

#include <stdint.h>

typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt32;
typedef int32_t SceKernelMemBlockType;

typedef struct {
	SceSize size;
	SceUInt32 field_4;
	SceUInt32 attr;
	SceUInt32 field_C;
	SceUInt32 paddr;
	SceSize alignment;
} SceKernelAllocMemBlockKernelOpt;

typedef struct SceUdcdEndpoint {
	int direction;
	int driverEndpointNumber;
	int endpointNumber;
	int transmittedBytes;
} SceUdcdEndpoint;

typedef struct SceUdcdDeviceRequest {
	SceUdcdEndpoint *endpoint;
	void *data;
	unsigned int attributes;
	int size;
	int isControlRequest;
	void (*onComplete)(struct SceUdcdDeviceRequest *req);
	int transmitted;
	int returnCode;
	struct SceUdcdDeviceRequest *next;
	void *unused;
	void *physicalAddress;
} SceUdcdDeviceRequest;

#define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT 1u
#define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT 2u
#define SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT 1u
#define SCE_EVENT_WAITOR 1u
#define SCE_EVENT_WAITCLEAR_PAT 2u
#define SCE_UDCD_ERROR_INVALID_ARGUMENT ((int)0x80243001)
#define SCE_UDCD_ERROR_DRIVER_IN_PROGRESS ((int)0x80243006)
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT ((int)0x80028005)
#define SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE ((int)0x80024302)
#define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW 0x30808006

SceUID ksceKernelCreateEventFlag(const char *name, unsigned int attr,
	unsigned int bits, void *opt);
int ksceKernelDeleteEventFlag(SceUID id);
int ksceKernelSetEventFlag(SceUID id, unsigned int bits);
int ksceKernelWaitEventFlagCB(SceUID id, unsigned int bits,
	unsigned int mode, unsigned int *out, SceUInt32 *timeout);
SceUID ksceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
	SceSize size, SceKernelAllocMemBlockKernelOpt *opt);
int ksceKernelFreeMemBlock(SceUID id);
int ksceKernelGetMemBlockBase(SceUID id, void **base);
int ksceKernelGetPaddr(const void *addr, uintptr_t *paddr);
int ksceUdcdReqSend(SceUdcdDeviceRequest *req);
int ksceUdcdReqCancelAll(SceUdcdEndpoint *endpoint);

#endif
