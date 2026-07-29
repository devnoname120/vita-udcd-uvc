#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/fast_mutex.h>
#include <psp2kern/udcd.h>
#include <taihen.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "diagnostic.h"
#include "uac.h"

#define ALIGN(value, alignment) \
	(((value) + ((alignment) - 1)) & ~((alignment) - 1))

#define AUDIO_MONITOR_PATH			0
#define AUDIO_MONITOR_MODE_DISABLED		0
#define AUDIO_MONITOR_MODE_READBACK		2
#define AUDIO_MONITOR_STEREO			2
#define AUDIO_CAPTURE_FRAMES			512

#define AUDIO_RING_FRAMES			8192
#define AUDIO_RING_MASK				(AUDIO_RING_FRAMES - 1)
#define AUDIO_RING_TARGET_FRAMES		1024
#define AUDIO_RING_DRIFT_THRESHOLD		128

#define AUDIO_USB_QUEUE_DEPTH			32
#define AUDIO_USB_PACKET_STRIDE			224

#define AUDIO_THREAD_WAKE			0x01
#define AUDIO_THREAD_DONE			0x02
#define AUDIO_THREAD_EXIT			0x04

#define AUDIO_CAPTURE_STOPPED			0x01
#define AUDIO_USB_STOPPED			0x02
#define AUDIO_ALL_STOPPED			\
	(AUDIO_CAPTURE_STOPPED | AUDIO_USB_STOPPED)

#define AUDIO_STOP_TIMEOUT_US			1000000

#define SCE_AUDIO_FOR_DRIVER_NID			0x15D711C1
#define SCE_AUDIO_MONITOR_SET_PATH_MODE_NID	0xAC9383D0
#define SCE_AUDIO_MONITOR_SET_RATE_NID		0x134C96C1
#define SCE_AUDIO_MONITOR_SET_CHANNEL_MODE_NID	0xFC375BAC
#define SCE_AUDIO_MONITOR_READ_NID		0x81EB0AE5

/*
 * These exports are private SceAudioForDriver functions on firmware 3.60.
 * monitor_path 0 taps the final SceAudio/SourceMixer output before the
 * handheld codec applies its speaker/headphone volume and mute settings.
 *
 * Resolve them at runtime rather than importing them in the SKPRX. SceAudio
 * is not guaranteed to be loaded yet when taiHEN processes early *KERNEL
 * plugins, and a direct import would prevent this module from loading at all.
 */
typedef int (*AudioMonitorSetPathModeFn)(int monitor_path, int mode);
typedef int (*AudioMonitorSetSampleRateFn)(int sample_rate);
typedef int (*AudioMonitorSetChannelModeFn)(int channel_mode);
typedef int (*AudioMonitorReadFn)(int monitor_path, void *pcm,
				  int frame_count);

int module_get_export_func(SceUID pid, const char *module_name,
			   uint32_t library_nid, uint32_t function_nid,
			   uintptr_t *function);

static AudioMonitorSetPathModeFn g_audio_monitor_set_path_mode;
static AudioMonitorSetSampleRateFn g_audio_monitor_set_sample_rate;
static AudioMonitorSetChannelModeFn g_audio_monitor_set_channel_mode;
static AudioMonitorReadFn g_audio_monitor_read;

typedef int16_t AudioPcmFrame[UAC_CHANNEL_COUNT];

struct AudioMemory {
	AudioPcmFrame capture[2][AUDIO_CAPTURE_FRAMES]
		__attribute__((aligned(64)));
	AudioPcmFrame ring[AUDIO_RING_FRAMES]
		__attribute__((aligned(64)));
	unsigned char usb_packet[AUDIO_USB_QUEUE_DEPTH][AUDIO_USB_PACKET_STRIDE]
		__attribute__((aligned(64)));
} __attribute__((aligned(4096)));

struct AudioUsbSlot {
	SceUdcdDeviceRequest request;
	unsigned int generation;
};

_Static_assert((AUDIO_RING_FRAMES & AUDIO_RING_MASK) == 0,
	"audio ring size must be a power of two");
_Static_assert(AUDIO_USB_QUEUE_DEPTH <= 32,
	"audio USB completion mask supports at most 32 slots");
_Static_assert(AUDIO_USB_PACKET_STRIDE >= UAC_MAX_PACKET_SIZE,
	"audio USB packet stride is too small");

static SceUdcdEndpoint *g_audio_endpoint;
static SceUID g_audio_memory_uid = -1;
static struct AudioMemory *g_audio_memory;
static SceKernelFastMutex g_audio_ring_mutex;
static SceKernelFastMutex g_audio_state_mutex;

static SceUID g_capture_event_id = -1;
static SceUID g_usb_event_id = -1;
static SceUID g_state_event_id = -1;
static SceUID g_capture_thread_id = -1;
static SceUID g_usb_thread_id = -1;

static struct AudioUsbSlot g_usb_slots[AUDIO_USB_QUEUE_DEPTH];

static SceInt32 g_audio_initialized;
static SceInt32 g_audio_exit;
static SceInt32 g_audio_shutdown;
static SceInt32 g_audio_desired;
static SceInt32 g_audio_generation;
static SceInt32 g_capture_unsafe;
static SceInt32 g_capture_stopped_generation;
static SceInt32 g_usb_stopped_generation;
static SceInt32 g_usb_in_flight;
static SceInt32 g_usb_done_mask;

static uint32_t g_ring_read;
static uint32_t g_ring_write;
static unsigned int g_usb_packet_sequence;
static int g_ring_rebuffering;

static int audio_resolve_monitor_functions(void)
{
	int ret;

	if (g_audio_monitor_set_path_mode &&
	    g_audio_monitor_set_sample_rate &&
	    g_audio_monitor_set_channel_mode &&
	    g_audio_monitor_read)
		return 0;

	ret = module_get_export_func(KERNEL_PID, "SceAudio",
		SCE_AUDIO_FOR_DRIVER_NID,
		SCE_AUDIO_MONITOR_SET_PATH_MODE_NID,
		(uintptr_t *)&g_audio_monitor_set_path_mode);
	diagnostic_record("resolve SceAudio monitor path mode", ret);
	if (ret < 0)
		goto fail;

	ret = module_get_export_func(KERNEL_PID, "SceAudio",
		SCE_AUDIO_FOR_DRIVER_NID, SCE_AUDIO_MONITOR_SET_RATE_NID,
		(uintptr_t *)&g_audio_monitor_set_sample_rate);
	diagnostic_record("resolve SceAudio monitor rate", ret);
	if (ret < 0)
		goto fail;

	ret = module_get_export_func(KERNEL_PID, "SceAudio",
		SCE_AUDIO_FOR_DRIVER_NID,
		SCE_AUDIO_MONITOR_SET_CHANNEL_MODE_NID,
		(uintptr_t *)&g_audio_monitor_set_channel_mode);
	diagnostic_record("resolve SceAudio monitor channel mode", ret);
	if (ret < 0)
		goto fail;

	ret = module_get_export_func(KERNEL_PID, "SceAudio",
		SCE_AUDIO_FOR_DRIVER_NID, SCE_AUDIO_MONITOR_READ_NID,
		(uintptr_t *)&g_audio_monitor_read);
	diagnostic_record("resolve SceAudio monitor read", ret);
	if (ret < 0)
		goto fail;

	return 0;

fail:
	g_audio_monitor_set_path_mode = NULL;
	g_audio_monitor_set_sample_rate = NULL;
	g_audio_monitor_set_channel_mode = NULL;
	g_audio_monitor_read = NULL;
	return ret;
}

static void audio_ring_reset(void)
{
	ksceKernelLockFastMutex(&g_audio_ring_mutex);
	g_ring_read = 0;
	g_ring_write = 0;
	g_ring_rebuffering = 1;
	ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
}

static void audio_ring_push(const AudioPcmFrame *frames, unsigned int count)
{
	unsigned int first;
	unsigned int used;

	ksceKernelLockFastMutex(&g_audio_ring_mutex);

	used = g_ring_write - g_ring_read;
	if (used > AUDIO_RING_FRAMES) {
		g_ring_read = g_ring_write;
		used = 0;
	}

	if (count > AUDIO_RING_FRAMES - used)
		g_ring_read += count - (AUDIO_RING_FRAMES - used);

	first = AUDIO_RING_FRAMES - (g_ring_write & AUDIO_RING_MASK);
	if (first > count)
		first = count;

	memcpy(&g_audio_memory->ring[g_ring_write & AUDIO_RING_MASK],
	       frames, first * sizeof(AudioPcmFrame));
	memcpy(&g_audio_memory->ring[0], &frames[first],
	       (count - first) * sizeof(AudioPcmFrame));
	g_ring_write += count;

	ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
}

static unsigned int audio_ring_pop_packet(unsigned char *packet)
{
	AudioPcmFrame *dst = (AudioPcmFrame *)packet;
	unsigned int count = UAC_NOMINAL_PACKET_FRAMES;
	unsigned int first;
	unsigned int used;

	memset(packet, 0,
	       UAC_NOMINAL_PACKET_FRAMES * sizeof(AudioPcmFrame));

	ksceKernelLockFastMutex(&g_audio_ring_mutex);
	used = g_ring_write - g_ring_read;

	if (used > AUDIO_RING_FRAMES) {
		g_ring_read = g_ring_write;
		used = 0;
		g_ring_rebuffering = 1;
	}

	if (g_ring_rebuffering) {
		if (used < AUDIO_RING_TARGET_FRAMES) {
			ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
			return UAC_NOMINAL_PACKET_FRAMES;
		}
		g_ring_rebuffering = 0;
	}

	/*
	 * The UAC endpoint is asynchronous and has no feedback endpoint. Apply a
	 * sparse 47/49-frame correction around the ring target to absorb the
	 * small difference between the Vita and host USB clocks.
	 */
	if ((g_usb_packet_sequence & 1) == 0) {
		if (used > AUDIO_RING_TARGET_FRAMES +
			   AUDIO_RING_DRIFT_THRESHOLD)
			count++;
		else if (used < AUDIO_RING_TARGET_FRAMES -
				AUDIO_RING_DRIFT_THRESHOLD)
			count--;
	}
	g_usb_packet_sequence++;

	if (used < count) {
		g_ring_rebuffering = 1;
		ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
		return UAC_NOMINAL_PACKET_FRAMES;
	}

	first = AUDIO_RING_FRAMES - (g_ring_read & AUDIO_RING_MASK);
	if (first > count)
		first = count;

	memcpy(dst, &g_audio_memory->ring[g_ring_read & AUDIO_RING_MASK],
	       first * sizeof(AudioPcmFrame));
	memcpy(&dst[first], &g_audio_memory->ring[0],
	       (count - first) * sizeof(AudioPcmFrame));
	g_ring_read += count;

	ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
	return count;
}

static void audio_signal_failure(void)
{
	/*
	 * Only the active-to-stopped transition owns the stop notification.
	 * Duplicate failures must not leave a wake behind after a worker has
	 * already acknowledged the stopped generation.
	 */
	if (!ksceKernelAtomicGetAndSet32(&g_audio_desired, 0))
		return;

	ksceKernelAtomicAddAndGet32(&g_audio_generation, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_WAKE);
	ksceKernelSetEventFlag(g_usb_event_id, AUDIO_THREAD_WAKE);
}

static int audio_usb_submit(unsigned int slot, unsigned int generation,
			    int silence);

static void audio_usb_complete(SceUdcdDeviceRequest *request)
{
	unsigned int slot;

	for (slot = 0; slot < AUDIO_USB_QUEUE_DEPTH; slot++) {
		if (request == &g_usb_slots[slot].request) {
			ksceKernelAtomicOrAndGet32(&g_usb_done_mask,
						  1u << slot);
			/*
			 * Publish the completed slot before dropping the in-flight
			 * count. A stop that observes zero may then safely consume
			 * every completion belonging to the old generation.
			 */
			ksceKernelAtomicSubAndGet32(&g_usb_in_flight, 1);
			ksceKernelSetEventFlag(g_usb_event_id, AUDIO_THREAD_DONE);
			break;
		}
	}
}

static int audio_usb_submit(unsigned int slot, unsigned int generation,
			    int silence)
{
	struct AudioUsbSlot *usb_slot = &g_usb_slots[slot];
	unsigned char *packet = g_audio_memory->usb_packet[slot];
	unsigned int frames;
	int ret;

	if (silence) {
		frames = UAC_NOMINAL_PACKET_FRAMES;
		memset(packet, 0, frames * sizeof(AudioPcmFrame));
	} else {
		frames = audio_ring_pop_packet(packet);
	}

	ksceKernelDcacheCleanRange(packet, frames * sizeof(AudioPcmFrame));

	usb_slot->generation = generation;
	usb_slot->request = (SceUdcdDeviceRequest){
		.endpoint = g_audio_endpoint,
		.data = packet,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = frames * sizeof(AudioPcmFrame),
		.isControlRequest = 0,
		.onComplete = audio_usb_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelAtomicAddAndGet32(&g_usb_in_flight, 1);
	ret = ksceUdcdReqSend(&usb_slot->request);
	if (ret < 0)
		ksceKernelAtomicSubAndGet32(&g_usb_in_flight, 1);

	return ret;
}

static void audio_usb_mark_stopped(void)
{
	unsigned int ignored;
	int generation;

	/*
	 * Cancel callbacks can leave DONE set after in_flight reaches zero.
	 * Consume all notifications for the finished generation before exposing
	 * its stopped acknowledgement.
	 */
	(void)ksceKernelAtomicGetAndSet32(&g_usb_done_mask, 0);
	(void)ksceKernelPollEventFlag(g_usb_event_id,
		AUDIO_THREAD_WAKE | AUDIO_THREAD_DONE,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);

	generation = ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
	if (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0))
		return;

	ksceKernelAtomicSet32(&g_usb_stopped_generation, generation);
	ksceKernelSetEventFlag(g_state_event_id, AUDIO_USB_STOPPED);
}

static int audio_usb_thread(SceSize args, void *argp)
{
	unsigned int active_generation = 0;
	int streaming = 0;

	(void)args;
	(void)argp;

	for (;;) {
		unsigned int out_bits;
		unsigned int done_mask;
		unsigned int slot;

		ksceKernelWaitEventFlag(g_usb_event_id,
			AUDIO_THREAD_WAKE | AUDIO_THREAD_DONE | AUDIO_THREAD_EXIT,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, NULL);

		if (ksceKernelAtomicGetAndAdd32(&g_audio_exit, 0))
			break;

		if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0)) {
			if (streaming ||
			    ksceKernelAtomicGetAndAdd32(&g_usb_in_flight, 0) > 0)
				ksceUdcdReqCancelAll(g_audio_endpoint);

			(void)ksceKernelAtomicGetAndSet32(&g_usb_done_mask, 0);

			if (ksceKernelAtomicGetAndAdd32(&g_usb_in_flight, 0) == 0) {
				if (streaming)
					ksceUdcdClearFIFO(g_audio_endpoint);
				streaming = 0;
				audio_usb_mark_stopped();
			}
			continue;
		}

		if (!streaming) {
			active_generation =
				ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
			ksceKernelAtomicSet32(&g_usb_done_mask, 0);
			ksceKernelAtomicSet32(&g_usb_in_flight, 0);
			g_usb_packet_sequence = 0;
			streaming = 1;

			for (slot = 0; slot < AUDIO_USB_QUEUE_DEPTH; slot++) {
				if (audio_usb_submit(slot, active_generation, 1) < 0) {
					audio_signal_failure();
					break;
				}
			}
			continue;
		}

		done_mask = ksceKernelAtomicGetAndSet32(&g_usb_done_mask, 0);
		for (slot = 0; slot < AUDIO_USB_QUEUE_DEPTH; slot++) {
			if (!(done_mask & (1u << slot)))
				continue;
			if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0))
				break;
			if (active_generation !=
			    (unsigned int)ksceKernelAtomicGetAndAdd32(
				    &g_audio_generation, 0))
				break;
			if (g_usb_slots[slot].request.returnCode < 0) {
				audio_signal_failure();
				break;
			}
			if (audio_usb_submit(slot, active_generation, 0) < 0) {
				audio_signal_failure();
				break;
			}
		}
	}

	if (streaming ||
	    ksceKernelAtomicGetAndAdd32(&g_usb_in_flight, 0) > 0) {
		ksceUdcdReqCancelAll(g_audio_endpoint);
		while (ksceKernelAtomicGetAndAdd32(&g_usb_in_flight, 0) > 0) {
			unsigned int ignored;
			ksceKernelWaitEventFlag(g_usb_event_id, AUDIO_THREAD_DONE,
				SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
				&ignored, NULL);
		}
		ksceUdcdClearFIFO(g_audio_endpoint);
	}

	audio_usb_mark_stopped();
	return 0;
}

static void audio_capture_mark_stopped(void)
{
	unsigned int ignored;
	int generation;

	/*
	 * The stop wake may still be pending after monitor_read(NULL) drains the
	 * last destination. Clear it before publishing stopped so it cannot
	 * produce a second, stale acknowledgement in the next generation.
	 */
	(void)ksceKernelPollEventFlag(g_capture_event_id, AUDIO_THREAD_WAKE,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);

	generation = ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
	if (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0))
		return;

	ksceKernelAtomicSet32(&g_capture_stopped_generation, generation);
	ksceKernelSetEventFlag(g_state_event_id, AUDIO_CAPTURE_STOPPED);
}

static int audio_capture_run(unsigned int generation)
{
	unsigned int current = 0;
	int ret;

	ksceKernelDcacheInvalidateRange(
		g_audio_memory->capture[current],
		sizeof(g_audio_memory->capture[current]));
	ret = g_audio_monitor_read(AUDIO_MONITOR_PATH,
		g_audio_memory->capture[current], AUDIO_CAPTURE_FRAMES);
	diagnostic_record("initial audio monitor read", ret);
	if (ret < 0) {
		int mode_ret = g_audio_monitor_set_path_mode(
			AUDIO_MONITOR_PATH, AUDIO_MONITOR_MODE_DISABLED);
		diagnostic_record("disable audio monitor path", mode_ret);
		audio_signal_failure();
		/*
		 * A failed initial queue did not transfer ownership of a capture
		 * buffer, so it is safe for the worker to acknowledge the stop.
		 */
		return 0;
	}

	while (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) &&
	       generation == (unsigned int)ksceKernelAtomicGetAndAdd32(
		       &g_audio_generation, 0)) {
		unsigned int next = current ^ 1;

		ksceKernelDcacheInvalidateRange(
			g_audio_memory->capture[next],
			sizeof(g_audio_memory->capture[next]));
		ret = g_audio_monitor_read(AUDIO_MONITOR_PATH,
			g_audio_memory->capture[next], AUDIO_CAPTURE_FRAMES);
		if (ret < 0) {
			audio_signal_failure();
			break;
		}

		ksceKernelDcacheInvalidateRange(
			g_audio_memory->capture[current],
			sizeof(g_audio_memory->capture[current]));

		if (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) &&
		    generation == (unsigned int)ksceKernelAtomicGetAndAdd32(
			    &g_audio_generation, 0))
			audio_ring_push(g_audio_memory->capture[current],
					AUDIO_CAPTURE_FRAMES);
		current = next;
	}

	/*
	 * Wait for the last queued monitor destination to be released, without
	 * queueing another destination. This is required before reuse or unload.
	 */
	ret = g_audio_monitor_read(AUDIO_MONITOR_PATH, NULL,
				   AUDIO_CAPTURE_FRAMES);
	if (ret < 0) {
		int mode_ret = g_audio_monitor_set_path_mode(
			AUDIO_MONITOR_PATH, AUDIO_MONITOR_MODE_DISABLED);
		diagnostic_record("disable unsafe audio monitor path",
				  mode_ret);
		/*
		 * Do not advertise the capture memory as safe when the final
		 * ownership fence fails. The stop timeout will prevent unload.
		 */
		ksceKernelAtomicSet32(&g_capture_unsafe, 1);
		audio_signal_failure();
		return ret;
	}

	ret = g_audio_monitor_set_path_mode(
		AUDIO_MONITOR_PATH, AUDIO_MONITOR_MODE_DISABLED);
	diagnostic_record("disable audio monitor path", ret);
	if (ret < 0)
		return ret;

	ksceKernelAtomicSet32(&g_capture_unsafe, 0);
	return 0;
}

static int audio_capture_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	for (;;) {
		unsigned int ignored;
		unsigned int generation;

		ksceKernelWaitEventFlag(g_capture_event_id,
			AUDIO_THREAD_WAKE | AUDIO_THREAD_EXIT,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&ignored, NULL);

		if (ksceKernelAtomicGetAndAdd32(&g_audio_exit, 0))
			break;
		if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0)) {
			if (!ksceKernelAtomicGetAndAdd32(&g_capture_unsafe, 0))
				audio_capture_mark_stopped();
			continue;
		}

		generation =
			ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
		if (audio_capture_run(generation) >= 0)
			audio_capture_mark_stopped();
	}

	if (!ksceKernelAtomicGetAndAdd32(&g_capture_unsafe, 0))
		audio_capture_mark_stopped();
	return 0;
}

static int audio_wait_stopped(int generation)
{
	unsigned int ignored;
	SceUInt timeout = AUDIO_STOP_TIMEOUT_US;
	int ret;

	for (;;) {
		int current_generation =
			ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);

		/*
		 * A worker failure can race a synchronous stop between publishing
		 * desired=false and incrementing the generation. While the state
		 * mutex excludes starts, follow that transition to its newer target.
		 */
		if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) &&
		    current_generation != generation)
			generation = current_generation;

		if (ksceKernelAtomicGetAndAdd32(
			    &g_capture_stopped_generation, 0) == generation &&
		    ksceKernelAtomicGetAndAdd32(
			    &g_usb_stopped_generation, 0) == generation)
			return 0;

		ret = ksceKernelWaitEventFlag(g_state_event_id,
			AUDIO_ALL_STOPPED,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&ignored, &timeout);
		if (ret < 0)
			return ret;
	}
}

static void audio_request_stop_locked(void)
{
	/*
	 * A stopped worker must not receive a redundant wake just before the
	 * next generation clears its stopped acknowledgement. Event bits are
	 * persistent, so the one wake emitted by the active-to-stopped
	 * transition is sufficient.
	 */
	if (!ksceKernelAtomicGetAndSet32(&g_audio_desired, 0))
		return;

	ksceKernelAtomicAddAndGet32(&g_audio_generation, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_WAKE);
	ksceKernelSetEventFlag(g_usb_event_id, AUDIO_THREAD_WAKE);
}

static void audio_discard_stale_worker_events(void)
{
	unsigned int ignored;

	/*
	 * A worker-side failure wakes both workers, including the caller. The
	 * caller can acknowledge stopped before consuming that self-wake. Drain
	 * such completed-generation notifications before publishing a new one.
	 */
	(void)ksceKernelPollEventFlag(g_capture_event_id, AUDIO_THREAD_WAKE,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);
	(void)ksceKernelPollEventFlag(g_usb_event_id,
		AUDIO_THREAD_WAKE | AUDIO_THREAD_DONE,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);
}

int uac_audio_init(SceUdcdEndpoint *endpoint)
{
	SceKernelAllocMemBlockKernelOpt opt;
	unsigned int memory_size = ALIGN(sizeof(struct AudioMemory), 4096);
	int ret;

	if (!endpoint)
		return SCE_UDCD_ERROR_INVALID_POINTER;
	if (ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return 0;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		   SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4096;

	g_audio_memory_uid = ksceKernelAllocMemBlock("uac_audio_memory",
		0x10208006, memory_size, &opt);
	if (g_audio_memory_uid < 0)
		return g_audio_memory_uid;

	ret = ksceKernelGetMemBlockBase(g_audio_memory_uid,
				       (void **)&g_audio_memory);
	if (ret < 0)
		goto fail_memory;
	memset(g_audio_memory, 0, sizeof(*g_audio_memory));

	ret = ksceKernelInitializeFastMutex(&g_audio_ring_mutex,
					    "uac_audio_ring", 0, NULL);
	if (ret < 0)
		goto fail_memory;

	ret = ksceKernelInitializeFastMutex(&g_audio_state_mutex,
					    "uac_audio_state", 0, NULL);
	if (ret < 0)
		goto fail_ring_mutex;

	g_capture_event_id =
		ksceKernelCreateEventFlag("uac_capture_event", 0, 0, NULL);
	if (g_capture_event_id < 0) {
		ret = g_capture_event_id;
		goto fail_state_mutex;
	}

	g_usb_event_id =
		ksceKernelCreateEventFlag("uac_usb_event", 0, 0, NULL);
	if (g_usb_event_id < 0) {
		ret = g_usb_event_id;
		goto fail_capture_event;
	}

	g_state_event_id = ksceKernelCreateEventFlag("uac_state_event", 0,
						    AUDIO_ALL_STOPPED, NULL);
	if (g_state_event_id < 0) {
		ret = g_state_event_id;
		goto fail_usb_event;
	}

	g_capture_thread_id = ksceKernelCreateThread("uac_capture_thread",
		audio_capture_thread, 0x3A, 0x2000, 0, 0x10000, NULL);
	if (g_capture_thread_id < 0) {
		ret = g_capture_thread_id;
		goto fail_state_event;
	}

	g_usb_thread_id = ksceKernelCreateThread("uac_usb_thread",
		audio_usb_thread, 0x38, 0x2000, 0, 0x10000, NULL);
	if (g_usb_thread_id < 0) {
		ret = g_usb_thread_id;
		goto fail_capture_thread;
	}

	g_audio_endpoint = endpoint;
	ksceKernelAtomicSet32(&g_audio_exit, 0);
	ksceKernelAtomicSet32(&g_audio_shutdown, 0);
	ksceKernelAtomicSet32(&g_audio_desired, 0);
	ksceKernelAtomicSet32(&g_audio_generation, 0);
	ksceKernelAtomicSet32(&g_capture_unsafe, 0);
	ksceKernelAtomicSet32(&g_capture_stopped_generation, 0);
	ksceKernelAtomicSet32(&g_usb_stopped_generation, 0);
	ksceKernelAtomicSet32(&g_usb_in_flight, 0);
	ksceKernelAtomicSet32(&g_usb_done_mask, 0);
	audio_ring_reset();

	ret = ksceKernelStartThread(g_capture_thread_id, 0, NULL);
	if (ret < 0)
		goto fail_usb_thread;

	ret = ksceKernelStartThread(g_usb_thread_id, 0, NULL);
	if (ret < 0)
		goto fail_started_capture;

	ksceKernelAtomicSet32(&g_audio_initialized, 1);
	return 0;

fail_started_capture:
	ksceKernelAtomicSet32(&g_audio_exit, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_EXIT);
	ksceKernelWaitThreadEnd(g_capture_thread_id, NULL, NULL);
fail_usb_thread:
	ksceKernelDeleteThread(g_usb_thread_id);
	g_usb_thread_id = -1;
fail_capture_thread:
	ksceKernelDeleteThread(g_capture_thread_id);
	g_capture_thread_id = -1;
fail_state_event:
	ksceKernelDeleteEventFlag(g_state_event_id);
	g_state_event_id = -1;
fail_usb_event:
	ksceKernelDeleteEventFlag(g_usb_event_id);
	g_usb_event_id = -1;
fail_capture_event:
	ksceKernelDeleteEventFlag(g_capture_event_id);
	g_capture_event_id = -1;
fail_state_mutex:
	ksceKernelFinalizeFastMutex(&g_audio_state_mutex);
fail_ring_mutex:
	ksceKernelFinalizeFastMutex(&g_audio_ring_mutex);
fail_memory:
	ksceKernelFreeMemBlock(g_audio_memory_uid);
	g_audio_memory_uid = -1;
	g_audio_memory = NULL;
	return ret;
}

int uac_audio_start(void)
{
	unsigned int ignored;
	int ret;

	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	ksceKernelLockFastMutex(&g_audio_state_mutex);

	if (ksceKernelAtomicGetAndAdd32(&g_audio_shutdown, 0)) {
		ret = SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
		goto out;
	}
	if (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0)) {
		ret = 0;
		goto out;
	}

	ret = audio_resolve_monitor_functions();
	if (ret < 0)
		goto out;

	/*
	 * The rate/channel setters only select the monitor format. SceAudio's
	 * path must separately be placed in readback mode; otherwise the first
	 * monitor read returns -2 and no capture DMA is started.
	 */
	ret = g_audio_monitor_set_path_mode(
		AUDIO_MONITOR_PATH, AUDIO_MONITOR_MODE_READBACK);
	diagnostic_record("enable audio monitor path", ret);
	if (ret < 0)
		goto out;

	audio_request_stop_locked();
	ret = audio_wait_stopped(
		ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0));
	if (ret < 0)
		goto disable_monitor_path;

	ret = g_audio_monitor_set_sample_rate(UAC_SAMPLE_RATE);
	diagnostic_record("set audio monitor rate", ret);
	if (ret < 0)
		goto disable_monitor_path;
	ret = g_audio_monitor_set_channel_mode(AUDIO_MONITOR_STEREO);
	diagnostic_record("set audio monitor channel mode", ret);
	if (ret < 0)
		goto disable_monitor_path;

	/* Discard any already-consumed generation's notification bits. */
	audio_discard_stale_worker_events();
	(void)ksceKernelPollEventFlag(g_state_event_id, AUDIO_ALL_STOPPED,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);

	audio_ring_reset();
	ksceKernelAtomicAddAndGet32(&g_audio_generation, 1);
	ksceKernelAtomicSet32(&g_audio_desired, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_WAKE);
	ksceKernelSetEventFlag(g_usb_event_id, AUDIO_THREAD_WAKE);
	ret = 0;
	goto out;

disable_monitor_path:
	(void)g_audio_monitor_set_path_mode(
		AUDIO_MONITOR_PATH, AUDIO_MONITOR_MODE_DISABLED);
out:
	ksceKernelUnlockFastMutex(&g_audio_state_mutex);
	return ret;
}

void uac_audio_request_stop(void)
{
	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return;

	ksceKernelLockFastMutex(&g_audio_state_mutex);
	audio_request_stop_locked();
	ksceKernelUnlockFastMutex(&g_audio_state_mutex);
}

int uac_audio_stop_sync(void)
{
	int ret;

	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return 0;

	ksceKernelLockFastMutex(&g_audio_state_mutex);
	audio_request_stop_locked();
	ret = audio_wait_stopped(
		ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0));
	ksceKernelUnlockFastMutex(&g_audio_state_mutex);
	return ret;
}

void uac_audio_begin_shutdown(void)
{
	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return;

	/*
	 * Keep this gate set even if a stop timeout aborts module unload. A host
	 * must not restart capture while the plugin is partially shutting down.
	 */
	ksceKernelLockFastMutex(&g_audio_state_mutex);
	ksceKernelAtomicSet32(&g_audio_shutdown, 1);
	audio_request_stop_locked();
	ksceKernelUnlockFastMutex(&g_audio_state_mutex);
}

void uac_audio_on_attach(void)
{
	int generation;

	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return;

	ksceKernelLockFastMutex(&g_audio_state_mutex);
	audio_request_stop_locked();
	generation = ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
	if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) &&
	    ksceKernelAtomicGetAndAdd32(
		    &g_usb_stopped_generation, 0) == generation)
		ksceUdcdClearFIFO(g_audio_endpoint);
	ksceKernelUnlockFastMutex(&g_audio_state_mutex);
}

int uac_audio_term(void)
{
	int ret;

	if (!ksceKernelAtomicGetAndAdd32(&g_audio_initialized, 0))
		return 0;

	uac_audio_begin_shutdown();
	ret = uac_audio_stop_sync();
	if (ret < 0)
		return ret;

	ksceKernelAtomicSet32(&g_audio_exit, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_EXIT);
	ksceKernelSetEventFlag(g_usb_event_id, AUDIO_THREAD_EXIT);
	ksceKernelWaitThreadEnd(g_capture_thread_id, NULL, NULL);
	ksceKernelWaitThreadEnd(g_usb_thread_id, NULL, NULL);

	ksceKernelDeleteThread(g_capture_thread_id);
	ksceKernelDeleteThread(g_usb_thread_id);
	ksceKernelDeleteEventFlag(g_capture_event_id);
	ksceKernelDeleteEventFlag(g_usb_event_id);
	ksceKernelDeleteEventFlag(g_state_event_id);
	ksceKernelAtomicSet32(&g_audio_initialized, 0);
	ksceKernelFinalizeFastMutex(&g_audio_state_mutex);
	ksceKernelFinalizeFastMutex(&g_audio_ring_mutex);
	ksceKernelFreeMemBlock(g_audio_memory_uid);

	g_capture_thread_id = -1;
	g_usb_thread_id = -1;
	g_capture_event_id = -1;
	g_usb_event_id = -1;
	g_state_event_id = -1;
	g_audio_memory_uid = -1;
	g_audio_memory = NULL;
	g_audio_endpoint = NULL;
	return 0;
}
