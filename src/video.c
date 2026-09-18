#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <string.h>
#include "uvc.h"
#include "video.h"

#ifndef UVC_FRAMEBUFFER_COUNT
#define UVC_FRAMEBUFFER_COUNT 2
#endif
#if UVC_FRAMEBUFFER_COUNT != 1 && UVC_FRAMEBUFFER_COUNT != 2
#error UVC_FRAMEBUFFER_COUNT must be 1 or 2
#endif

static struct {
	SceUID uid;
	struct uvc_video_buffer buffer;
} slots[UVC_FRAMEBUFFER_COUNT] = {
	{ .uid = -1 },
#if UVC_FRAMEBUFFER_COUNT == 2
	{ .uid = -1 },
#endif
};

static SceUdcdEndpoint *video_endpoint;
static SceUID completion_event = -1;
static SceUdcdDeviceRequest request;
static unsigned int allocated_size;
static unsigned int buffer_count;
static unsigned int next_buffer;
static int pending;
static int prepared;

static void transfer_complete(SceUdcdDeviceRequest *req)
{
	(void)req;
	ksceKernelSetEventFlag(completion_event, 1);
}

int uvc_video_init(SceUdcdEndpoint *endpoint)
{
	if (!endpoint || completion_event >= 0)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	completion_event = ksceKernelCreateEventFlag("uvc_frame_req_evflag", 0, 0, NULL);
	if (completion_event < 0)
		return completion_event;

	video_endpoint = endpoint;
	return 0;
}

int uvc_video_wait(void)
{
	int ret;
	SceUInt32 timeout = 1000000;

	if (!pending)
		return 0;

	ret = ksceKernelWaitEventFlagCB(completion_event, 1,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, NULL, &timeout);
	/* A timeout does not return ownership of the request or its DMA buffer. */
	if (ret < 0)
		return ret;

	pending = 0;
	return request.returnCode;
}

static int free_slot(unsigned int slot)
{
	int ret;

	if (slots[slot].uid < 0)
		return 0;

	ret = ksceKernelFreeMemBlock(slots[slot].uid);
	if (ret < 0)
		return ret;

	slots[slot].uid = -1;
	memset(&slots[slot].buffer, 0, sizeof(slots[slot].buffer));
	return 0;
}

int uvc_video_release(void)
{
	int ret = uvc_video_wait();
	int error = 0;

	if (pending)
		return ret;

	allocated_size = buffer_count = next_buffer = 0;
	prepared = 0;
	for (unsigned int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++) {
		ret = free_slot(i);
		if (ret < 0 && !error)
			error = ret;
	}
	return error;
}

static int allocate_slot(unsigned int slot, unsigned int frame_size)
{
	SceKernelAllocMemBlockKernelOpt opt;
	unsigned int size = (offsetof(struct uvc_frame, data) + frame_size + 4095u) & ~4095u;
	int ret;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4096;
	slots[slot].uid = ksceKernelAllocMemBlock("uvc_frame_buffer", 0x10208006, size, &opt);
	if (slots[slot].uid < 0)
		return slots[slot].uid;

	ret = ksceKernelGetMemBlockBase(slots[slot].uid, (void **)&slots[slot].buffer.frame);
	if (ret >= 0)
		ret = ksceKernelGetPaddr(slots[slot].buffer.frame->data,
			&slots[slot].buffer.data_paddr);
	if (ret < 0)
		free_slot(slot);
	return ret;
}

int uvc_video_prepare(unsigned int frame_size, struct uvc_video_buffer *buffer)
{
	int ret;

	if (completion_event < 0 || !buffer || !frame_size ||
		frame_size > MAX_UVC_VIDEO_FRAME_SIZE)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	if (!buffer_count || allocated_size != frame_size) {
		ret = uvc_video_release();
		if (ret < 0)
			return ret;

		for (unsigned int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++) {
			ret = allocate_slot(i, frame_size);
			if (ret < 0) {
				if (i == 0 || slots[i].uid >= 0) {
					buffer_count = 0;
					return ret;
				}
				/* Preserve serial capture when a second allocation is unavailable. */
				break;
			}
			buffer_count++;
		}
		allocated_size = frame_size;
	}

	if (buffer_count == 1) {
		ret = uvc_video_wait();
		if (ret < 0)
			return ret;
	}

	*buffer = slots[next_buffer].buffer;
	prepared = 1;
	return 0;
}

int uvc_video_submit(int fid)
{
	struct uvc_frame *frame;
	int ret;

	if (pending)
		return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
	if (!prepared || completion_event < 0)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	frame = slots[next_buffer].buffer.frame;
	memset(frame->header, 0, sizeof(frame->header));
	frame->header[0] = UVC_PAYLOAD_HEADER_SIZE;
	frame->header[1] = UVC_STREAM_EOH | UVC_STREAM_EOF | (fid ? UVC_STREAM_FID : 0);
	request = (SceUdcdDeviceRequest){
		.endpoint = video_endpoint,
		.data = frame->header,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = UVC_PAYLOAD_SIZE(allocated_size),
		.onComplete = transfer_complete
	};

	pending = 1;
	ret = ksceUdcdReqSend(&request);
	if (ret < 0) {
		pending = 0;
		return ret;
	}

	prepared = 0;
	next_buffer = (next_buffer + 1) % buffer_count;
	return 0;
}

void uvc_video_cancel(void)
{
	if (video_endpoint)
		ksceUdcdReqCancelAll(video_endpoint);
}

int uvc_video_term(void)
{
	int ret;

	uvc_video_cancel();
	ret = uvc_video_release();
	if (ret < 0)
		return ret;

	if (completion_event >= 0) {
		ret = ksceKernelDeleteEventFlag(completion_event);
		if (ret < 0)
			return ret;
		completion_event = -1;
	}
	video_endpoint = NULL;
	return 0;
}
