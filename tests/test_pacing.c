#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "uvc.h"

#define LOG(...) ((void)0)

typedef struct {
	uint16_t wValue;
	uint8_t bRequest;
} SceUdcdEP0DeviceRequest;

static struct uvc_streaming_control uvc_probe_control_setting;
static struct { unsigned char buffer[64]; } pending_recv;
static atomic_int stream;
atomic_uint uvc_capture_interval;
atomic_uint uvc_pacing_epoch;
static int uvc_event_flag_id = 42;
static unsigned int signals;

static int ksceKernelSetEventFlag(int id, unsigned int bits)
{
	assert(id == uvc_event_flag_id && bits == 1);
	signals++;
	return 0;
}

#include "controls.inc"
#include "pacing.inc"

static void negotiate(unsigned int selector, unsigned int interval)
{
	const struct uvc_streaming_control control = {
		.bFormatIndex = 1,
		.bFrameIndex = 1,
		.dwFrameInterval = interval
	};
	const SceUdcdEP0DeviceRequest request = {
		.wValue = selector << 8,
		.bRequest = UVC_SET_CUR
	};
	memcpy(pending_recv.buffer, &control, sizeof(control));
	uvc_handle_video_streaming_req_recv(&request);
}

static void start(unsigned int interval)
{
	stream = 0;
	assert(display_vblank_cb_func(0, 1, 0, NULL) == 0);
	negotiate(UVC_VS_COMMIT_CONTROL, interval);
	assert(stream);
	signals = 0;
}

static unsigned int notify(int count)
{
	unsigned int before = signals;
	assert(display_vblank_cb_func(0, count, 0, NULL) == 0);
	assert(signals - before <= 1);
	return signals - before;
}

static unsigned int run(unsigned int vblanks)
{
	unsigned int before = signals;
	for (unsigned int i = 0; i < vblanks; i++)
		notify(1);
	return signals - before;
}

static void test_rounded_supported_intervals(void)
{
	const struct { unsigned int interval, expected; } cases[] = {
		{166666, 600}, {166667, 600},
		{333333, 300}, {333334, 300},
		{500000, 200}
	};
	for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		start(cases[i].interval);
		unsigned int count = run(600);
		printf("interval %u: %u captures / 600 vblanks (expected %u)\n",
			cases[i].interval, count, cases[i].expected);
		fflush(stdout);
		assert(count == cases[i].expected);
	}
}

static void test_fractional_phase(void)
{
	start(208333);
	assert(run(600) == 480);
	start(416666);
	assert(run(600) == 240);
	start(416667);
	unsigned int count = run(600);
	assert(count >= 239 && count <= 240);
	start(166833);
	assert(run(600) == 599);
}

static void test_coalesced_notifications(void)
{
	start(333333);
	assert(notify(5) == 1);
	assert(notify(1) == 1);
	assert(notify(1) == 0);
	assert(notify(1) == 1);
	start(166667);
	assert(notify(120) == 1);
	assert(notify(1) == 1);
}

static void test_invalid_counts_and_long_delays(void)
{
	start(333333);
	assert(notify(0) == 0);
	assert(notify(-1) == 0);
	assert(notify(INT_MIN) == 0);
	assert(notify(1) == 0);
	assert(notify(1) == 1);
	start(333333);
	assert(notify(INT_MAX) == 1);
	assert(notify(1) == 1);
	start(UINT_MAX);
	assert(run(600) == 0);
	assert(notify(INT_MAX) == 1);
	start(0);
	assert(run(600) == 600);
	start(1);
	assert(run(600) == 600);
}

static void test_stop_and_restart(void)
{
	start(333333);
	assert(notify(1) == 0);
	stream = 0;
	assert(notify(10) == 0);
	stream = 1;
	assert(notify(1) == 0);
	assert(notify(1) == 1);

	assert(notify(1) == 0);
	stream = 0;
	negotiate(UVC_VS_COMMIT_CONTROL, 333333);
	assert(notify(1) == 0);
	assert(notify(1) == 1);
}

static void test_committed_timing_only(void)
{
	start(333333);
	negotiate(UVC_VS_PROBE_CONTROL, 500000);
	assert(notify(1) == 0);
	assert(notify(1) == 1);
	assert(notify(1) == 0);
	negotiate(UVC_VS_COMMIT_CONTROL, 500000);
	assert(notify(1) == 0);
	assert(notify(1) == 0);
	assert(notify(1) == 1);
}

int main(void)
{
	test_rounded_supported_intervals();
	test_fractional_phase();
	test_coalesced_notifications();
	test_invalid_counts_and_long_delays();
	test_stop_and_restart();
	test_committed_timing_only();
	puts("pacing: 6 test groups passed");
	return 0;
}
