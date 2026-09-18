#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "vita_test_kernel.h"

#define SCE_KERNEL_STOP_FAIL (-1)
#define SCE_KERNEL_STOP_SUCCESS 0
#define UVC_DRIVER_NAME "VITAUVC00"

static atomic_int uvc_thread_run;
static int uvc_driver_registered;
static SceUID uvc_thread_id = 20, uvc_event_flag_id = 21;
static SceUID SceUdcd_sub_01E1128C_hook_uid = 22;
static int SceUdcd_sub_01E1128C_ref, uvc_udcd_driver;
enum { AUDIO_STOP, JOIN, VIDEO_RELEASE, UNREGISTER, VIDEO_TERM, AUDIO_TERM };
static struct {
	int fail_at, registered, unregisters, event_deleted, thread_deleted, hook_released;
	int joined, released, video_dead, audio_dead;
} state;

static int result(int stage) { return state.fail_at == stage ? -77 : 0; }
static void uac_audio_begin_shutdown(void) {}
static int uac_audio_stop_sync(void) { return result(AUDIO_STOP); }
static void uvc_handle_video_abort(void) {}
int ksceKernelSetEventFlag(SceUID id, unsigned int bits)
{
	assert(id == uvc_event_flag_id && !state.event_deleted && bits == 1);
	return 0;
}
static int ksceKernelWaitThreadEnd(SceUID id, void *status, void *timeout)
{
	assert(id == uvc_thread_id && !state.thread_deleted && !uvc_thread_run);
	if (result(JOIN) < 0) return -77;
	state.joined = 1;
	return 0;
}
static int uvc_video_release(void)
{
	assert(state.joined);
	if (result(VIDEO_RELEASE) < 0) return -77;
	state.released = 1;
	return 0;
}
static int ksceUdcdDeactivate(void) { assert(state.released); return 0; }
static int ksceUdcdStop(const char *name, int size, void *args) { return 0; }
static int ksceUdcdUnregister(void *driver)
{
	assert(driver == &uvc_udcd_driver);
	state.unregisters++;
	if (!state.registered || result(UNREGISTER) < 0) return -77;
	state.registered = 0;
	return 0;
}
static int uvc_video_term(void)
{
	assert(!state.registered && state.released);
	if (result(VIDEO_TERM) < 0) return -77;
	state.video_dead = 1;
	return 0;
}
static int uac_audio_term(void)
{
	assert(!state.registered);
	if (result(AUDIO_TERM) < 0) return -77;
	state.audio_dead = 1;
	return 0;
}
int ksceKernelDeleteEventFlag(SceUID id)
{
	assert(id == uvc_event_flag_id && state.audio_dead && state.video_dead);
	state.event_deleted++;
	return 0;
}
static int ksceKernelDeleteThread(SceUID id)
{
	assert(id == uvc_thread_id && state.audio_dead && state.video_dead);
	state.thread_deleted++;
	return 0;
}
static int taiHookReleaseForKernel(SceUID id, int ref)
{
	assert(id == SceUdcd_sub_01E1128C_hook_uid && !state.registered);
	state.hook_released++;
	return 0;
}

#include "shutdown.inc"

int main(void)
{
	for (int stage = AUDIO_TERM; stage >= -1; stage--) {
		memset(&state, 0, sizeof(state));
		uvc_thread_run = uvc_driver_registered = state.registered = 1;
		state.fail_at = stage;
		if (stage >= 0) {
			assert(module_stop(0, NULL) == SCE_KERNEL_STOP_FAIL);
			assert(!state.event_deleted && !state.thread_deleted && !state.hook_released);
		}
		state.fail_at = -1;
		assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
		assert(!uvc_driver_registered && !state.registered);
		assert(state.unregisters == (stage == UNREGISTER ? 2 : 1));
		assert(state.event_deleted == 1 && state.thread_deleted == 1 && state.hook_released == 1);
	}
	puts("shutdown: 7 failure/retry scenarios passed");
	return 0;
}
