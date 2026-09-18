#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video.h"

#define MOCK_ERROR (-77)
#define FRAME_SIZE (960u * 544u * 3u / 2u)

static SceUdcdEndpoint endpoint = { .endpointNumber = 1 };
static struct {
	struct {
		void *base;
		unsigned int size;
	} memory[8];
	SceUdcdDeviceRequest *queued;
	unsigned char *queued_copy;
	int event;
	unsigned int event_bits;
	int alloc_calls, base_calls, paddr_calls, live_blocks;
	int waits, sends, cancels, deletes;
	int fail_alloc, fail_base, fail_paddr;
	int fail_event, fail_send, fail_free, fail_delete;
	int auto_complete, cancel_complete, complete_on_send;
	int completion_code;
	void (*on_wait)(void);
	void (*on_send)(void);
} mock;

static int contains(const void *base, unsigned int size, const void *address)
{
	return (uintptr_t)address >= (uintptr_t)base &&
		(uintptr_t)address < (uintptr_t)base + size;
}

static void complete(int status)
{
	SceUdcdDeviceRequest *req = mock.queued;
	assert(req);
	assert(memcmp(req->data, mock.queued_copy, req->size) == 0);
	mock.queued = NULL;
	free(mock.queued_copy);
	mock.queued_copy = NULL;
	req->returnCode = status;
	req->transmitted = 0;
	req->onComplete(req);
}

SceUID ksceKernelCreateEventFlag(const char *name, unsigned int attr,
	unsigned int bits, void *opt)
{
	(void)name;
	(void)attr;
	(void)opt;
	assert(!mock.event);
	if (mock.fail_event)
		return MOCK_ERROR;
	mock.event = 100;
	mock.event_bits = bits;
	return mock.event;
}

int ksceKernelDeleteEventFlag(SceUID id)
{
	assert(id == mock.event);
	assert(!mock.queued);
	if (mock.fail_delete)
		return MOCK_ERROR;
	mock.event = 0;
	mock.deletes++;
	return 0;
}

int ksceKernelSetEventFlag(SceUID id, unsigned int bits)
{
	assert(id == mock.event && mock.event);
	mock.event_bits |= bits;
	return 0;
}

int ksceKernelWaitEventFlagCB(SceUID id, unsigned int bits,
	unsigned int mode, unsigned int *out, SceUInt32 *timeout)
{
	assert(id == mock.event && mock.event);
	assert(mode == (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT));
	assert(timeout && *timeout > 0);
	mock.waits++;
	if (mock.on_wait) {
		void (*hook)(void) = mock.on_wait;
		mock.on_wait = NULL;
		hook();
	}
	if (!(mock.event_bits & bits) && mock.auto_complete && mock.queued)
		complete(mock.completion_code);
	if (!(mock.event_bits & bits))
		return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	if (out)
		*out = mock.event_bits;
	mock.event_bits &= ~bits;
	return 0;
}

SceUID ksceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
	SceSize size, SceKernelAllocMemBlockKernelOpt *opt)
{
	(void)name;
	assert(type == 0x10208006);
	assert(size && !(size & 4095));
	assert(opt && opt->size == sizeof(*opt));
	assert(opt->alignment == 4096);
	assert(opt->attr == (SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT));
	if (++mock.alloc_calls == mock.fail_alloc)
		return MOCK_ERROR;
	for (unsigned int i = 0; i < 8; i++) {
		if (!mock.memory[i].base) {
			mock.memory[i].base = malloc(size);
			assert(mock.memory[i].base);
			mock.memory[i].size = size;
			memset(mock.memory[i].base, 0xCC, size);
			mock.live_blocks++;
			return (SceUID)i + 1;
		}
	}
	abort();
}

int ksceKernelFreeMemBlock(SceUID id)
{
	assert(id >= 1 && id <= 8 && mock.memory[id - 1].base);
	assert(!mock.queued || !contains(mock.memory[id - 1].base,
		mock.memory[id - 1].size, mock.queued->data));
	if (mock.fail_free)
		return MOCK_ERROR;
	free(mock.memory[id - 1].base);
	mock.memory[id - 1].base = NULL;
	mock.live_blocks--;
	return 0;
}

int ksceKernelGetMemBlockBase(SceUID id, void **base)
{
	assert(id >= 1 && id <= 8 && mock.memory[id - 1].base);
	if (++mock.base_calls == mock.fail_base)
		return MOCK_ERROR;
	*base = mock.memory[id - 1].base;
	return 0;
}

int ksceKernelGetPaddr(const void *addr, uintptr_t *paddr)
{
	if (++mock.paddr_calls == mock.fail_paddr)
		return MOCK_ERROR;
	*paddr = (uintptr_t)addr;
	return 0;
}

int ksceUdcdReqSend(SceUdcdDeviceRequest *req)
{
	assert(!mock.queued);
	assert(req->endpoint == &endpoint);
	assert(req->attributes == SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT);
	assert(req->next == NULL && req->physicalAddress == NULL);
	assert(req->onComplete);
	assert(mock.event_bits == 0);
	if (mock.fail_send)
		return MOCK_ERROR;
	mock.queued = req;
	mock.queued_copy = malloc(req->size);
	assert(mock.queued_copy);
	memcpy(mock.queued_copy, req->data, req->size);
	mock.sends++;
	if (mock.on_send) {
		void (*hook)(void) = mock.on_send;
		mock.on_send = NULL;
		hook();
	}
	if (mock.complete_on_send)
		complete(mock.completion_code);
	return 0;
}

int ksceUdcdReqCancelAll(SceUdcdEndpoint *ep)
{
	assert(ep == &endpoint);
	mock.cancels++;
	if (mock.queued && mock.cancel_complete)
		complete(-3);
	return 0;
}

static void reset_mock(void)
{
	assert(!mock.event && !mock.live_blocks && !mock.queued);
	memset(&mock, 0, sizeof(mock));
	mock.auto_complete = mock.cancel_complete = 1;
}

static void setup(void)
{
	reset_mock();
	assert(uvc_video_init(&endpoint) == 0);
	assert(uvc_video_init(&endpoint) < 0);
}

static void teardown(void)
{
	mock.fail_free = mock.fail_delete = 0;
	mock.cancel_complete = 1;
	assert(uvc_video_term() == 0);
	assert(uvc_video_term() == 0);
	assert(!mock.event && !mock.live_blocks && !mock.queued);
}

static void fill(struct uvc_video_buffer *buffer, unsigned int size, int value)
{
	assert(!mock.queued || !contains(buffer->frame,
		size + offsetof(struct uvc_frame, data), mock.queued->data));
	assert(buffer->data_paddr == (uintptr_t)buffer->frame->data);
	memset(buffer->frame->data, value, size);
}

static void test_overlap_and_backpressure(void)
{
	struct uvc_video_buffer first, second, third;
	setup();
	assert(uvc_video_prepare(FRAME_SIZE, &first) == 0);
	fill(&first, FRAME_SIZE, 1);
	assert(uvc_video_submit(0) == 0);
	assert(mock.waits == 0 && mock.queued);
	assert(uvc_video_prepare(FRAME_SIZE, &second) == 0);
#if UVC_FRAMEBUFFER_COUNT == 2
	assert(first.frame != second.frame);
	assert(mock.waits == 0 && mock.queued);
	assert(uvc_video_submit(1) == SCE_UDCD_ERROR_DRIVER_IN_PROGRESS);
#else
	assert(first.frame == second.frame);
	assert(mock.waits == 1 && !mock.queued);
#endif
	fill(&second, FRAME_SIZE, 2);
	assert(uvc_video_wait() == 0);
	assert(uvc_video_submit(1) == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &third) == 0);
	assert(third.frame == first.frame);
	fill(&third, FRAME_SIZE, 3);
	assert(mock.alloc_calls == UVC_FRAMEBUFFER_COUNT);
	assert(mock.paddr_calls == UVC_FRAMEBUFFER_COUNT);
	assert(uvc_video_wait() == 0);
	assert(uvc_video_submit(0) == 0);
	teardown();
}

static void test_stalled_transfer_retains_resources(void)
{
	struct uvc_video_buffer buffer;
	setup();
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
	fill(&buffer, FRAME_SIZE, 4);
	assert(uvc_video_submit(0) == 0);
	mock.auto_complete = mock.cancel_complete = 0;
	assert(uvc_video_wait() == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(uvc_video_prepare(FRAME_SIZE / 2, &buffer) ==
		SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(uvc_video_release() == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(uvc_video_term() == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	assert(mock.live_blocks == UVC_FRAMEBUFFER_COUNT);
	assert(mock.event && mock.deletes == 0 && mock.queued);
	complete(-3);
	teardown();
}

static void test_resolution_changes_and_idle_release(void)
{
	struct uvc_video_buffer buffer;
	setup();
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
	fill(&buffer, FRAME_SIZE, 5);
	assert(uvc_video_submit(0) == 0);
	assert(uvc_video_prepare(480u * 272u * 3u / 2u, &buffer) == 0);
	assert(mock.waits == 1 && !mock.queued);
	assert(mock.alloc_calls == 2 * UVC_FRAMEBUFFER_COUNT);
	fill(&buffer, 480u * 272u * 3u / 2u, 6);
	assert(uvc_video_submit(1) == 0);
	uvc_video_cancel();
	assert(uvc_video_release() == 0);
	assert(!mock.live_blocks && !mock.queued);
	assert(mock.event_bits == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
	fill(&buffer, FRAME_SIZE, 7);
	assert(uvc_video_submit(0) == 0);
	teardown();
}

static void test_allocation_failures(void)
{
	for (int operation = 0; operation < 3; operation++) {
		for (int slot = 1; slot <= UVC_FRAMEBUFFER_COUNT; slot++) {
			struct uvc_video_buffer first, next;
			setup();
			if (operation == 0) mock.fail_alloc = slot;
			if (operation == 1) mock.fail_base = slot;
			if (operation == 2) mock.fail_paddr = slot;
			if (slot == 1) {
				assert(uvc_video_prepare(FRAME_SIZE, &first) == MOCK_ERROR);
				assert(mock.live_blocks == 0);
			} else {
				assert(uvc_video_prepare(FRAME_SIZE, &first) == 0);
				assert(mock.live_blocks == 1);
				fill(&first, FRAME_SIZE, 8);
				assert(uvc_video_submit(0) == 0);
				assert(uvc_video_prepare(FRAME_SIZE, &next) == 0);
				assert(mock.waits == 1 && next.frame == first.frame);
			}
			teardown();
		}
	}
}

static void test_submission_and_completion_errors(void)
{
	struct uvc_video_buffer first, retry;
	setup();
	assert(uvc_video_submit(0) < 0);
	assert(uvc_video_prepare(FRAME_SIZE, &first) == 0);
	fill(&first, FRAME_SIZE, 9);
	mock.fail_send = 1;
	assert(uvc_video_submit(0) == MOCK_ERROR);
	assert(!mock.queued && mock.event_bits == 0);
	assert(uvc_video_prepare(FRAME_SIZE, &retry) == 0);
	assert(retry.frame == first.frame);
	mock.fail_send = 0;
	assert(uvc_video_submit(0) == 0);
	mock.completion_code = MOCK_ERROR;
	assert(uvc_video_wait() == MOCK_ERROR);
	assert(!mock.queued);
	assert(uvc_video_wait() == 0);
	assert(uvc_video_release() == 0);
	teardown();
}

static void test_synchronous_completion_and_header(void)
{
	struct uvc_video_buffer buffer;
	setup();
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
	fill(&buffer, FRAME_SIZE, 10);
	assert(uvc_video_submit(1) == 0);
	assert(mock.queued->data == buffer.frame->header);
	assert(mock.queued->size == (int)(FRAME_SIZE + 12));
	assert(buffer.frame->header[0] == 12 && buffer.frame->header[1] == 0x83);
	for (int i = 2; i < 12; i++) assert(buffer.frame->header[i] == 0);
	complete(0);
	assert(uvc_video_wait() == 0);
	mock.complete_on_send = 1;
	for (int i = 0; i < 6; i++) {
		assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
		fill(&buffer, FRAME_SIZE, i);
		assert(uvc_video_wait() == 0);
		assert(uvc_video_submit(i & 1) == 0);
		assert(!mock.queued);
		assert(uvc_video_wait() == 0);
	}
	teardown();
}

static void test_size_and_alignment(void)
{
	static const unsigned int dimensions[][2] = {
		{960, 544}, {896, 504}, {864, 488}, {480, 272}, {1280, 720}
	};
	struct uvc_video_buffer buffer;
	setup();
	assert(uvc_video_prepare(0, &buffer) < 0);
	assert(uvc_video_prepare(UINT_MAX, &buffer) < 0);
	assert(uvc_video_prepare(FRAME_SIZE, NULL) < 0);
	assert(offsetof(struct uvc_frame, data) == 16);
	for (unsigned int i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
		unsigned int size = dimensions[i][0] * dimensions[i][1] * 3u / 2u;
		assert(uvc_video_prepare(size, &buffer) == 0);
		assert(((uintptr_t)buffer.frame->data & 15) == 0);
		assert(mock.memory[0].size == ((size + 16u + 4095u) & ~4095u));
		fill(&buffer, size, 11);
		assert(uvc_video_submit(i & 1) == 0);
	}
	assert(uvc_video_prepare(4081, &buffer) == 0);
	assert(mock.memory[0].size == 8192);
	fill(&buffer, 4081, 12);
	assert(uvc_video_submit(0) == 0);
	teardown();
}

static void test_resource_cleanup_failures(void)
{
	struct uvc_video_buffer buffer;
	reset_mock();
	mock.fail_event = 1;
	assert(uvc_video_init(&endpoint) == MOCK_ERROR);
	assert(uvc_video_term() == 0);
	setup();
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == 0);
	mock.fail_free = 1;
	assert(uvc_video_release() == MOCK_ERROR);
	assert(mock.live_blocks == UVC_FRAMEBUFFER_COUNT);
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) == MOCK_ERROR);
	mock.fail_free = 0;
	assert(uvc_video_release() == 0);
	mock.fail_delete = 1;
	assert(uvc_video_term() == MOCK_ERROR);
	assert(mock.event && mock.deletes == 0);
	teardown();
}

#define UVC_FRAME_UNCOMPRESSED(n) mock_frame_descriptor
#define FORMAT_INDEX_UNCOMPRESSED_NV12 1
#define VIDEO_FRAME_SIZE_NV12(w, h) ((w) * (h) * 3u / 2u)

struct mock_frame_descriptor {
	unsigned short wWidth, wHeight;
};
static struct {
	struct mock_frame_descriptor frames_uncompressed_nv12[5];
} video_streaming_descriptors = {
	{{960, 544}, {896, 504}, {864, 488}, {480, 272}, {1280, 720}}
};
static struct {
	unsigned char bFrameIndex, bFormatIndex;
} uvc_probe_control_setting;
static atomic_int uvc_thread_run, stream;

typedef struct {
	SceSize size;
	uintptr_t paddr;
	unsigned char unused[32];
} SceDisplayFrameBufInfo;

static struct {
	uintptr_t source;
	int queries, conversions;
	int first_error, first_empty, all_error, all_empty, convert_error;
	int action;
} capture;

static int ksceDisplayGetPrimaryHead(void)
{
	return 2;
}

static int ksceDisplayGetProcFrameBufInternal(SceUID pid, int head, int index,
	SceDisplayFrameBufInfo *info)
{
	assert(pid == -1 && head == 2);
	assert(info->size == sizeof(*info));
	for (unsigned int i = 0; i < sizeof(info->unused); i++)
		assert(info->unused[i] == 0);
	capture.queries++;
	if (capture.all_error || (!index && capture.first_error))
		return MOCK_ERROR;
	info->paddr = capture.all_empty || (!index && capture.first_empty) ? 0 : capture.source;
	return 0;
}

static int frame_convert_to_nv12(uintptr_t dst, const SceDisplayFrameBufInfo *info,
	int width, int height)
{
	struct uvc_video_buffer buffer = {
		.frame = (struct uvc_frame *)(dst - offsetof(struct uvc_frame, data)),
		.data_paddr = dst
	};
	assert(info->paddr == capture.source);
	capture.conversions++;
	if (capture.convert_error)
		return MOCK_ERROR;
	fill(&buffer, VIDEO_FRAME_SIZE_NV12(width, height), 0x44);
	return 0;
}

#include "capture.inc"

static void setup_capture(void)
{
	setup();
	memset(&capture, 0, sizeof(capture));
	capture.source = 0x1000;
	uvc_probe_control_setting.bFrameIndex = 1;
	uvc_probe_control_setting.bFormatIndex = FORMAT_INDEX_UNCOMPRESSED_NV12;
	uvc_thread_run = stream = 1;
}

static void change_capture_state(void)
{
	if (capture.action == 0)
		stream = 0;
	else if (capture.action == 1)
		uvc_thread_run = 0;
	else
		uvc_probe_control_setting.bFrameIndex = 4;
}

static void advance_capture_source(void)
{
	capture.source += 0x1000;
}

static void test_capture_source_lookup(void)
{
	for (int fallback = 0; fallback < 3; fallback++) {
		setup_capture();
		capture.first_error = fallback == 1;
		capture.first_empty = fallback == 2;
		assert(send_frame() == 0);
		assert(capture.queries == (fallback ? 2 : 1));
		assert(capture.conversions == 1 && mock.sends == 1);
		teardown();
	}
	setup_capture();
	capture.all_error = 1;
	assert(send_frame() == MOCK_ERROR);
	assert(capture.queries == 2 && capture.conversions == 0);
	teardown();
	setup_capture();
	capture.all_empty = 1;
	assert(send_frame() < 0);
	assert(capture.queries == 2 && capture.conversions == 0);
	teardown();
}

static void test_capture_invalid_modes(void)
{
	setup_capture();
	uvc_probe_control_setting.bFrameIndex = 0;
	assert(send_frame() < 0);
	uvc_probe_control_setting.bFrameIndex = 6;
	assert(send_frame() < 0);
	uvc_probe_control_setting.bFrameIndex = 1;
	uvc_probe_control_setting.bFormatIndex = 2;
	assert(send_frame() < 0);
	assert(!mock.alloc_calls && !capture.queries && !mock.sends);
	teardown();
}

static void test_capture_state_changes_during_wait(void)
{
	for (int action = 0; action < 3; action++) {
		setup_capture();
		assert(send_frame() == 0);
		capture.action = action;
		mock.on_wait = change_capture_state;
		assert(send_frame() == 0);
		assert(mock.sends == 1 && !mock.queued);
		if (action == 2) {
			assert(send_frame() == 0);
			assert(mock.sends == 2);
			assert(mock.queued->size == (int)(480u * 272u * 3u / 2u + 12u));
		}
		teardown();
	}
}

static void test_capture_state_changes_during_submit(void)
{
	for (int action = 0; action < 3; action++) {
		setup_capture();
		capture.action = action;
		mock.on_send = change_capture_state;
		assert(send_frame() == 0);
		assert(mock.sends == 1 && mock.cancels == 1 && !mock.queued);
		teardown();
	}
}

static void test_capture_samples_source_after_buffer_wait(void)
{
	setup_capture();
	assert(send_frame() == 0);
	uvc_probe_control_setting.bFrameIndex = 4;
	mock.on_wait = advance_capture_source;
	assert(send_frame() == 0);
	assert(capture.queries == 2 && capture.conversions == 2);
	assert(capture.source == 0x2000 && mock.sends == 2);
	teardown();
}

static void test_capture_conversion_and_transfer_errors(void)
{
	setup_capture();
	capture.convert_error = 1;
	assert(send_frame() == MOCK_ERROR);
	assert(mock.sends == 0);
	teardown();
	setup_capture();
	assert(send_frame() == 0);
	mock.completion_code = MOCK_ERROR;
	assert(send_frame() == MOCK_ERROR);
	assert(mock.sends == 1 && !mock.queued);
	mock.completion_code = 0;
	assert(send_frame() == 0);
	assert(mock.sends == 2);
	teardown();
}

int main(void)
{
	struct uvc_video_buffer buffer;
	assert(uvc_video_init(NULL) < 0);
	assert(uvc_video_prepare(FRAME_SIZE, &buffer) < 0);
	test_overlap_and_backpressure();
	test_stalled_transfer_retains_resources();
	test_resolution_changes_and_idle_release();
	test_allocation_failures();
	test_submission_and_completion_errors();
	test_synchronous_completion_and_header();
	test_size_and_alignment();
	test_resource_cleanup_failures();
	test_capture_source_lookup();
	test_capture_invalid_modes();
	test_capture_state_changes_during_wait();
	test_capture_state_changes_during_submit();
	test_capture_samples_source_after_buffer_wait();
	test_capture_conversion_and_transfer_errors();
	printf("video: 14 test groups passed (%d buffer configuration)\n", UVC_FRAMEBUFFER_COUNT);
	return 0;
}
