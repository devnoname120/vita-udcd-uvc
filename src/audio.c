#include <psp2/kernel/error.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/cpu/spinlock.h>
#include <psp2kern/kernel/dmac.h>
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

#define AUDIO_CAPTURE_FRAMES			512
#define AUDIO_CAPTURE_BUFFER_COUNT		8
#define AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH	2
#define AUDIO_CAPTURE_BUFFER_MASK		\
	((1u << AUDIO_CAPTURE_BUFFER_COUNT) - 1)

/*
 * SceAudio's normal SrcMix1 MAIN output writes the complete local mix to the
 * selected I2S sink. Firmware 3.60 initializes the handheld route to I2S7,
 * which is a final sink rather than one of the I2S0..2 capture/loopback ports.
 * Attempting to read I2S7's TX FIFO therefore arms DMA successfully but never
 * receives a peripheral request.
 *
 * SourceMixer hardware also exposes a secondary output FIFO. Sony uses
 * SrcMix0's secondary FIFO and control register 39 as a non-destructive monitor
 * while its MAIN output continues feeding SrcMix1. Mirror that exact design one
 * stage later: enable SrcMix1 register 39 only after arming a FIFO-to-RAM DMAC4
 * chain, then drain SrcMix1 +0x240 using the adjacent request selector 0x60000.
 * The normal MAIN-to-I2S7 route remains untouched.
 *
 * SrcMix0 secondary readback uses DMAC4 channel 4, the next mixer's input-1
 * channel. The corresponding SrcMix1 secondary path is channel 7 (SrcMix2
 * input-1). SceAudio allocates that operation but leaves it idle unless the
 * private I2S0 capture API is called, so runtime guards reject an active owner.
 * The audio buffers use a non-cacheable mapping and must not set COHERENT_DST.
 */
#define SRCMIX1_SECONDARY_DMAC_COMPLETION	0x00002000
#define SRCMIX1_SECONDARY_DMAC_DIRECTION	0x05000000
#define SRCMIX1_SECONDARY_DMAC_TAIL_BYTES	256
#define SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE	16
#define SRCMIX1_SECONDARY_DMAC_ERROR_MASK	\
	(SCE_KERNEL_DMAC_STAT_ABORTED |		\
	 SCE_KERNEL_DMAC_STAT_ERROR_READ |	\
	 SCE_KERNEL_DMAC_STAT_ERROR_WRITE |	\
	 SCE_KERNEL_DMAC_STAT_ERROR_ILLEGAL_CONFIG | \
	 SCE_KERNEL_DMAC_STAT_ERROR_TAG |	\
	 SCE_KERNEL_DMAC_STAT_ERROR_ZERO_BYTE)

/*
 * Runtime layout guards for the 3.60 SceAudio data segment. This project is
 * already firmware-specific, but checking the neighboring initialized tags
 * prevents an unknown SceAudio build from arming DMA against the wrong
 * peripheral.
 */
#define SCE_AUDIO_SRCMIX1_MAIN_SOURCE_OFFSET	0xBAB4
#define SCE_AUDIO_SRCMIX1_MAIN_DEST_OFFSET	0xBAB8
#define SCE_AUDIO_SRCMIX1_MAIN_COMMAND_OFFSET	0xBB1C
#define SCE_AUDIO_SRCMIX1_REGS_OFFSET		0xBB24
#define SCE_AUDIO_SRCMIX1_OUTPUT_MODE_OFFSET	0xBB28
#define SCE_AUDIO_SRCMIX1_SPINLOCK_OFFSET	0xBB44
#define SCE_AUDIO_SRCMIX2_INPUT1_STATE_OFFSET	0xBC14
#define SCE_AUDIO_I2S_ENABLED_MASK_OFFSET	0xBD21
#define SCE_AUDIO_SELECTED_I2S_INDEX_OFFSET	0xBD22
#define SCE_AUDIO_SRCMIX2_INPUT_I2S_OFFSET	0xBD23

#define SCE_AUDIO_SRCMIX1_MAIN_FIFO_PHYSICAL	0xE04B0200
#define SCE_AUDIO_SRCMIX1_MAIN_DMAC_COMMAND	0x0005C000
#define SCE_AUDIO_HANDHELD_I2S_INDEX		7
#define SCE_AUDIO_HANDHELD_I2S_FIFO_PHYSICAL	0xE0490100
#define SRCMIX1_SECONDARY_FIFO_PHYSICAL		((const void *)0xE04B0240)
#define SRCMIX1_SECONDARY_DMAC_COMMAND		0x00060000
#define SRCMIX1_SECONDARY_DMAC_CHANNEL		7
#define SRCMIX_SECONDARY_CONTROL_INDEX		39
#define SRCMIX_SECONDARY_CONTROL_DISABLED	0x00000000
#define SRCMIX_SECONDARY_CONTROL_ENABLED		0x00010001

#define AUDIO_RING_FRAMES			8192
#define AUDIO_RING_MASK				(AUDIO_RING_FRAMES - 1)
#define AUDIO_RING_TARGET_FRAMES		1024

/*
 * Keep one SceUdcd request per one-millisecond UAC transaction. SceUdcd
 * accepts larger requests, but batching multiple isochronous transactions
 * makes the macOS host stream discontinuous even when the request reports
 * that every byte was transmitted.
 */
#define AUDIO_USB_QUEUE_DEPTH			32
#define AUDIO_USB_REQUEST_INTERVALS		1
#define AUDIO_USB_REQUEST_FRAMES		\
	(UAC_NOMINAL_PACKET_FRAMES * AUDIO_USB_REQUEST_INTERVALS)
#define AUDIO_USB_REQUEST_STRIDE		\
	ALIGN(AUDIO_USB_REQUEST_FRAMES * sizeof(AudioPcmFrame), 64)

#define AUDIO_THREAD_WAKE			0x01
#define AUDIO_THREAD_DONE			0x02
#define AUDIO_THREAD_EXIT			0x04
#define AUDIO_THREAD_DMA			0x08

#define AUDIO_CAPTURE_STOPPED			0x01
#define AUDIO_USB_STOPPED			0x02
#define AUDIO_ALL_STOPPED			\
	(AUDIO_CAPTURE_STOPPED | AUDIO_USB_STOPPED)

#define AUDIO_STOP_TIMEOUT_US			1000000
#define AUDIO_DMA_TIMEOUT_US			500000

/*
 * SceKernelDmacMgr 3.60 services the DMAC4 group interrupt on CPU 3. Keeping
 * the capture worker on that CPU makes stop/unload serialize with the client
 * callback, which DmaOpQuit and DmaOpFree do not otherwise wait for.
 */
#define AUDIO_CAPTURE_CPU_AFFINITY		0x00080000
#define AUDIO_USB_CPU_AFFINITY			0x00010000

int module_get_offset(SceUID pid, SceUID modid, int segment_index,
		      size_t offset, uintptr_t *address);

typedef int16_t AudioPcmFrame[UAC_CHANNEL_COUNT];

struct AudioMemory {
	AudioPcmFrame capture[AUDIO_CAPTURE_BUFFER_COUNT][AUDIO_CAPTURE_FRAMES]
		__attribute__((aligned(64)));
	AudioPcmFrame ring[AUDIO_RING_FRAMES]
		__attribute__((aligned(64)));
	unsigned char usb_packet[AUDIO_USB_QUEUE_DEPTH][AUDIO_USB_REQUEST_STRIDE]
		__attribute__((aligned(64)));
} __attribute__((aligned(4096)));

struct AudioUsbSlot {
	SceUdcdDeviceRequest request;
	unsigned int generation;
};

struct AudioDmaPeriod {
	SceKernelDmaOpTag tag[2];
} __attribute__((aligned(64)));

/*
 * VitaSDK's public declaration omits the fifth argument and declares a void
 * return, while SceKernelDmacMgr 3.60 actually calls this ABI. Convert through
 * a union when registering it so the implementation can validate the real
 * byte count without an incompatible-function-pointer cast.
 */
typedef int (*AudioDmaOpCallbackAbi)(
	SceKernelDmaOpId op_id,
	SceUInt32 hardware_status,
	void *user_data,
	const SceKernelDmaOpTag *fault_tag,
	SceUInt32 bytes_processed);

union AudioDmaOpCallbackPointer {
	AudioDmaOpCallbackAbi abi;
	SceKernelDmaOpCallback sdk;
};

_Static_assert((AUDIO_RING_FRAMES & AUDIO_RING_MASK) == 0,
	"audio ring size must be a power of two");
_Static_assert(AUDIO_USB_QUEUE_DEPTH <= 32,
	"audio USB completion mask supports at most 32 slots");
_Static_assert(AUDIO_USB_REQUEST_STRIDE >=
	       AUDIO_USB_REQUEST_FRAMES * sizeof(AudioPcmFrame),
	"audio USB request stride is too small");
_Static_assert(UAC_NOMINAL_PACKET_FRAMES == UAC_MAX_PACKET_FRAMES,
	"USB requests require fixed-size one-millisecond packets");
_Static_assert(AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH >= 2 &&
	       AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH < AUDIO_CAPTURE_BUFFER_COUNT,
	"audio DMA needs queued headroom and at least one spare buffer");
_Static_assert(AUDIO_CAPTURE_BUFFER_COUNT < 32,
	"audio DMA buffer masks must fit in a signed 32-bit atomic");
_Static_assert(sizeof(AudioPcmFrame) * AUDIO_CAPTURE_FRAMES >
	       SRCMIX1_SECONDARY_DMAC_TAIL_BYTES,
	"audio DMA period must contain a body and a 256-byte tail");
_Static_assert((sizeof(AudioPcmFrame) * AUDIO_CAPTURE_FRAMES) %
	       SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE == 0,
	"audio DMA period must be a multiple of the SrcMix transfer block size");
_Static_assert(SRCMIX1_SECONDARY_DMAC_COMMAND ==
	       SCE_AUDIO_SRCMIX1_MAIN_DMAC_COMMAND + 0x4000,
	"SrcMix1 secondary request must immediately follow its main request");

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
static struct AudioDmaPeriod
	g_capture_dma_period[AUDIO_CAPTURE_BUFFER_COUNT];
static const SceKernelDmaOpChainParam g_capture_dma_chain_param = {
	.size = sizeof(SceKernelDmaOpChainParam),
	.coherencyMask = 0x0003FFFF,
	.setValue = 0
};

static SceKernelDmaOpId g_capture_dma_op_id = -1;
static volatile uint32_t *g_srcmix1_regs;
static SceKernelSpinlock *g_srcmix1_spinlock;
static volatile unsigned char *g_srcmix1_output_mode;
static volatile uint32_t *g_srcmix2_input1_state;

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
static SceInt32 g_capture_dma_running;
static SceInt32 g_capture_dma_done_mask;
static SceInt32 g_capture_dma_free_mask;
static SceInt32 g_capture_dma_error;
static SceInt32 g_capture_dma_error_stage;
static SceInt32 g_capture_dma_error_status;
static SceInt32 g_capture_dma_error_bytes;
static SceInt32 g_capture_dma_callback_period;

#ifdef DIAGNOSTIC
#define AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES	32
static unsigned char
	g_capture_dma_previous[AUDIO_CAPTURE_BUFFER_COUNT]
			      [AUDIO_CAPTURE_FRAMES * sizeof(AudioPcmFrame)]
	__attribute__((aligned(64)));
static unsigned int g_capture_dma_previous_valid_mask;
static unsigned int g_capture_dma_lines_compared;
static unsigned int g_capture_dma_lines_unchanged;
static unsigned char
	g_capture_dma_sentinel[AUDIO_CAPTURE_BUFFER_COUNT];
static unsigned int g_capture_dma_sentinel_valid_mask;
static unsigned int g_capture_dma_sentinel_sequence;
static unsigned int g_capture_dma_sentinel_lines_tested;
static unsigned int g_capture_dma_sentinel_lines_remaining;
static unsigned int g_capture_dma_tag_mutation_count;
static unsigned int g_capture_dma_callback_count;
static unsigned int g_capture_dma_callback_busy_count;
static unsigned int g_capture_dma_callback_sync_count;
static unsigned int g_capture_dma_callback_status_or;
#endif

static uint32_t g_ring_read;
static uint32_t g_ring_write;
static int g_ring_rebuffering;

static void audio_capture_dma_prepare_period(unsigned int period);
#ifdef DIAGNOSTIC
static int audio_capture_dma_period_tags_changed(unsigned int period);
#endif

static inline void audio_data_sync_barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static int audio_resolve_srcmix1_secondary(void)
{
	tai_module_info_t module_info;
	volatile unsigned char *data_bytes;
	volatile uint32_t *data;
	volatile uint32_t *srcmix1_regs;
	SceKernelSpinlock *srcmix1_spinlock;
	SceKernelIntrStatus interrupt_state;
	uintptr_t data_address;
	uint32_t secondary_control;
	int ret;

	memset(&module_info, 0, sizeof(module_info));
	module_info.size = sizeof(module_info);
	ret = taiGetModuleInfoForKernel(KERNEL_PID, "SceAudio", &module_info);
	diagnostic_record("resolve SceAudio module", ret);
	if (ret < 0)
		return ret;

	ret = module_get_offset(KERNEL_PID, module_info.modid, 1, 0,
				&data_address);
	diagnostic_record("resolve SceAudio data segment", ret);
	if (ret < 0)
		return ret;
	data = (volatile uint32_t *)data_address;
	data_bytes = (volatile unsigned char *)data_address;

	diagnostic_record("SceAudio SrcMix1 main source",
		data[SCE_AUDIO_SRCMIX1_MAIN_SOURCE_OFFSET / sizeof(uint32_t)]);
	diagnostic_record("SceAudio SrcMix1 main destination",
		data[SCE_AUDIO_SRCMIX1_MAIN_DEST_OFFSET / sizeof(uint32_t)]);
	diagnostic_record("SceAudio SrcMix1 main base command",
		data[SCE_AUDIO_SRCMIX1_MAIN_COMMAND_OFFSET / sizeof(uint32_t)]);
	diagnostic_record("SceAudio SrcMix1 output mode",
		data_bytes[SCE_AUDIO_SRCMIX1_OUTPUT_MODE_OFFSET]);
	diagnostic_record("SceAudio SrcMix2 input1 DMA state",
		data[SCE_AUDIO_SRCMIX2_INPUT1_STATE_OFFSET / sizeof(uint32_t)]);
	diagnostic_record("SceAudio enabled I2S mask",
		data_bytes[SCE_AUDIO_I2S_ENABLED_MASK_OFFSET]);
	diagnostic_record("SceAudio selected I2S index",
		data_bytes[SCE_AUDIO_SELECTED_I2S_INDEX_OFFSET]);
	diagnostic_record("SceAudio SrcMix2 capture I2S index",
		data_bytes[SCE_AUDIO_SRCMIX2_INPUT_I2S_OFFSET]);

	srcmix1_regs = (volatile uint32_t *)(uintptr_t)
		data[SCE_AUDIO_SRCMIX1_REGS_OFFSET / sizeof(uint32_t)];
	srcmix1_spinlock = (SceKernelSpinlock *)
		(data_bytes + SCE_AUDIO_SRCMIX1_SPINLOCK_OFFSET);
	if (!srcmix1_regs || ((uintptr_t)srcmix1_regs & 3u))
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	interrupt_state =
		ksceKernelSpinlockLowLockCpuSuspendIntr(srcmix1_spinlock);
	secondary_control =
		srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX];
	ksceKernelSpinlockLowUnlockCpuResumeIntr(srcmix1_spinlock,
		interrupt_state);
	diagnostic_record("SceAudio SrcMix1 secondary control",
		(int)secondary_control);

	/*
	 * The normal final route must still be SrcMix1 MAIN to I2S7. Refuse to
	 * collide with either firmware monitor mode, an already-enabled secondary
	 * output, or SceAudio's private channel-7 operation.
	 */
	if (data[SCE_AUDIO_SRCMIX1_MAIN_SOURCE_OFFSET / sizeof(uint32_t)] !=
		    SCE_AUDIO_SRCMIX1_MAIN_FIFO_PHYSICAL ||
	    data[SCE_AUDIO_SRCMIX1_MAIN_DEST_OFFSET / sizeof(uint32_t)] !=
		    SCE_AUDIO_HANDHELD_I2S_FIFO_PHYSICAL ||
	    data[SCE_AUDIO_SRCMIX1_MAIN_COMMAND_OFFSET / sizeof(uint32_t)] !=
		    SCE_AUDIO_SRCMIX1_MAIN_DMAC_COMMAND ||
	    data_bytes[SCE_AUDIO_SRCMIX1_OUTPUT_MODE_OFFSET] != 0 ||
	    data[SCE_AUDIO_SRCMIX2_INPUT1_STATE_OFFSET / sizeof(uint32_t)] != 0 ||
	    secondary_control != SRCMIX_SECONDARY_CONTROL_DISABLED ||
	    !(data_bytes[SCE_AUDIO_I2S_ENABLED_MASK_OFFSET] &
	      (1u << SCE_AUDIO_HANDHELD_I2S_INDEX)) ||
	    data_bytes[SCE_AUDIO_SELECTED_I2S_INDEX_OFFSET] !=
		    SCE_AUDIO_HANDHELD_I2S_INDEX)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	g_srcmix1_regs = srcmix1_regs;
	g_srcmix1_spinlock = srcmix1_spinlock;
	g_srcmix1_output_mode =
		data_bytes + SCE_AUDIO_SRCMIX1_OUTPUT_MODE_OFFSET;
	g_srcmix2_input1_state =
		&data[SCE_AUDIO_SRCMIX2_INPUT1_STATE_OFFSET /
		      sizeof(uint32_t)];
	return 0;
}

static void audio_capture_dma_fail(int stage, int failure,
				   SceUInt32 bytes_processed)
{
	ksceKernelAtomicSet32(&g_capture_dma_error_stage, stage);
	ksceKernelAtomicSet32(&g_capture_dma_error_status, failure);
	ksceKernelAtomicSet32(&g_capture_dma_error_bytes,
			     (SceInt32)bytes_processed);
	ksceKernelAtomicSet32(&g_capture_dma_error, 1);
	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_DMA);
}

static int audio_capture_dma_complete(SceKernelDmaOpId op_id,
				      SceUInt32 hardware_status,
				      void *user_data,
				      const SceKernelDmaOpTag *fault_tag,
				      SceUInt32 bytes_processed)
{
	SceKernelIntrStatus interrupt_state;
	unsigned int period;
	unsigned int next_period;
	unsigned int append_period;
	unsigned int append_mask;
	int ret;

	(void)user_data;
	(void)fault_tag;

	if (!ksceKernelAtomicGetAndAdd32(&g_capture_dma_running, 0))
		return 0;

#ifdef DIAGNOSTIC
	g_capture_dma_callback_count++;
	g_capture_dma_callback_status_or |= hardware_status;
	if (hardware_status & SCE_KERNEL_DMAC_STAT_BUSY)
		g_capture_dma_callback_busy_count++;
#endif

	if ((hardware_status & SRCMIX1_SECONDARY_DMAC_ERROR_MASK) ||
	    bytes_processed !=
		    sizeof(g_audio_memory->capture[0])) {
		audio_capture_dma_fail(
			1, (SceInt32)hardware_status, bytes_processed);
		return 0;
	}

	/*
	 * DmacMgr guarantees callbacks in channel order, but its normal
	 * completion interrupt can pass a null/stale fault_tag. Hardware
	 * status bit 0 is the callback-time DMAC BUSY snapshot. SceAudio accepts
	 * it as sufficient liveness evidence and polls only when it is clear.
	 * Track the bounded queue position explicitly instead of relying on the
	 * callback's fault_tag.
	 */
	period = (unsigned int)ksceKernelAtomicGetAndAdd32(
		&g_capture_dma_callback_period, 0);
	next_period = period + 1;
	if (next_period == AUDIO_CAPTURE_BUFFER_COUNT)
		next_period = 0;
	ksceKernelAtomicSet32(&g_capture_dma_callback_period,
			     (SceInt32)next_period);

	/*
	 * Keep the peripheral chain alive from callback context, as SceAudio
	 * does. Deferring Concatenate to a worker allows the operation to drain;
	 * a later successful append then does not restart the hardware stream.
	 *
	 * Do not immediately requeue the completed destination. Two periods stay
	 * in the hardware queue while six remain in a worker-owned reserve. The
	 * callback appends the next period in sequence only after the worker has
	 * copied and released it. Exhausting that reserve stops the producer
	 * before DMAC4 can overwrite unread PCM.
	 */
	append_period = period + AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH;
	if (append_period >= AUDIO_CAPTURE_BUFFER_COUNT)
		append_period -= AUDIO_CAPTURE_BUFFER_COUNT;
	append_mask = 1u << append_period;
	ret = ksceKernelAtomicGetAndClear32(
		&g_capture_dma_free_mask, (SceInt32)append_mask);
	if (!(ret & append_mask)) {
		audio_capture_dma_fail(4, ret, bytes_processed);
		return 0;
	}

#ifdef DIAGNOSTIC
	if (audio_capture_dma_period_tags_changed(append_period))
		g_capture_dma_tag_mutation_count++;
#endif
	audio_capture_dma_prepare_period(append_period);
	interrupt_state = ksceKernelCpuSuspendIntr();
	if (!(hardware_status & SCE_KERNEL_DMAC_STAT_BUSY)) {
#ifdef DIAGNOSTIC
		g_capture_dma_callback_sync_count++;
#endif
		ret = ksceKernelDmaOpSync(
			op_id, SCE_KERNEL_DMA_OP_SYNC_POLL, NULL, NULL);
		if (ret <= 0) {
			(void)ksceKernelCpuResumeIntr(interrupt_state);
			audio_capture_dma_fail(2, ret, bytes_processed);
			return 0;
		}
	}
	ret = ksceKernelDmaOpConcatenate(
		op_id,
		g_capture_dma_period[append_period].tag,
		SCE_KERNEL_DMA_OP_VIRTUAL_DST_ADDR);
	(void)ksceKernelCpuResumeIntr(interrupt_state);
	if (ret < 0) {
		audio_capture_dma_fail(3, ret, bytes_processed);
		return 0;
	}

	ret = ksceKernelAtomicGetAndOr32(&g_capture_dma_done_mask,
					1u << period);
	if (ret & (1u << period)) {
		audio_capture_dma_fail(5, ret, bytes_processed);
		return 0;
	}

	ksceKernelSetEventFlag(g_capture_event_id, AUDIO_THREAD_DMA);
	return 0;
}

static void audio_capture_dma_prepare_period(unsigned int period)
{
	const unsigned int period_bytes =
		sizeof(g_audio_memory->capture[period]);
	/*
	 * AudioMemory uses KERNEL_ROOT_NC_RW. SceAudio adds COHERENT_DST only
	 * for cached monitor destinations; applying it to this non-cacheable
	 * alias produces incomplete destination writes on DMAC4.
	 */
	const unsigned int command = SRCMIX1_SECONDARY_DMAC_COMMAND;
	unsigned char *destination =
		(unsigned char *)g_audio_memory->capture[period];
	SceKernelDmaOpTag *tag = g_capture_dma_period[period].tag;

	tag[0] = (SceKernelDmaOpTag){
		.src = SRCMIX1_SECONDARY_FIFO_PHYSICAL,
		.dst = destination,
		.len = SRCMIX1_SECONDARY_DMAC_DIRECTION |
		       (period_bytes - SRCMIX1_SECONDARY_DMAC_TAIL_BYTES),
		.cmd = command,
		.keyring = 0,
		.iv = NULL,
		.blockSize = SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE,
		.pNext = &tag[1]
	};
	tag[1] = (SceKernelDmaOpTag){
		.src = SRCMIX1_SECONDARY_FIFO_PHYSICAL,
		.dst = destination + period_bytes -
		       SRCMIX1_SECONDARY_DMAC_TAIL_BYTES,
		.len = SRCMIX1_SECONDARY_DMAC_DIRECTION |
		       SRCMIX1_SECONDARY_DMAC_TAIL_BYTES,
		.cmd = command | SRCMIX1_SECONDARY_DMAC_COMPLETION,
		.keyring = 0,
		.iv = NULL,
		.blockSize = SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE,
		.pNext = SCE_KERNEL_DMAC_CHAIN_END
	};
}

#ifdef DIAGNOSTIC
static int audio_capture_dma_period_tags_changed(unsigned int period)
{
	const unsigned int period_bytes =
		sizeof(g_audio_memory->capture[period]);
	const unsigned int command = SRCMIX1_SECONDARY_DMAC_COMMAND;
	const unsigned char *destination =
		(const unsigned char *)g_audio_memory->capture[period];
	const SceKernelDmaOpTag *tag =
		g_capture_dma_period[period].tag;

	return tag[0].src != SRCMIX1_SECONDARY_FIFO_PHYSICAL ||
	       tag[0].dst != destination ||
	       tag[0].len !=
		       (SRCMIX1_SECONDARY_DMAC_DIRECTION |
			(period_bytes - SRCMIX1_SECONDARY_DMAC_TAIL_BYTES)) ||
	       tag[0].cmd != command ||
	       tag[0].keyring != 0 ||
	       tag[0].iv != NULL ||
	       tag[0].blockSize != SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE ||
	       tag[0].pNext != &g_capture_dma_period[period].tag[1] ||
	       tag[1].src != SRCMIX1_SECONDARY_FIFO_PHYSICAL ||
	       tag[1].dst !=
		       destination + period_bytes -
			       SRCMIX1_SECONDARY_DMAC_TAIL_BYTES ||
	       tag[1].len !=
		       (SRCMIX1_SECONDARY_DMAC_DIRECTION |
			SRCMIX1_SECONDARY_DMAC_TAIL_BYTES) ||
	       tag[1].cmd !=
		       (command | SRCMIX1_SECONDARY_DMAC_COMPLETION) ||
	       tag[1].keyring != 0 ||
	       tag[1].iv != NULL ||
	       tag[1].blockSize != SRCMIX1_SECONDARY_DMAC_BLOCK_SIZE ||
	       tag[1].pNext != SCE_KERNEL_DMAC_CHAIN_END;
}

static void audio_capture_dma_measure_visibility(unsigned int period)
{
	const unsigned char *current =
		(const unsigned char *)g_audio_memory->capture[period];
	unsigned int offset;

	if (g_capture_dma_previous_valid_mask & (1u << period)) {
		for (offset = 0;
		     offset < sizeof(g_audio_memory->capture[period]);
		     offset += AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES) {
			g_capture_dma_lines_compared++;
			if (memcmp(&current[offset],
				   &g_capture_dma_previous[period][offset],
				   AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES) == 0)
				g_capture_dma_lines_unchanged++;
		}
	} else {
		g_capture_dma_previous_valid_mask |= 1u << period;
	}

	memcpy(g_capture_dma_previous[period], current,
	       sizeof(g_audio_memory->capture[period]));
}

static unsigned int
audio_capture_dma_measure_sentinel(unsigned int period)
{
	const unsigned char *current =
		(const unsigned char *)g_audio_memory->capture[period];
	const unsigned char sentinel = g_capture_dma_sentinel[period];
	unsigned int lines_remaining = 0;
	unsigned int offset;

	if (!(g_capture_dma_sentinel_valid_mask & (1u << period)))
		return 0;

	for (offset = 0;
	     offset < sizeof(g_audio_memory->capture[period]);
	     offset += AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES) {
		unsigned int byte;

		g_capture_dma_sentinel_lines_tested++;
		for (byte = 0; byte < AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES; byte++) {
			if (current[offset + byte] != sentinel)
				break;
		}
		if (byte == AUDIO_DIAGNOSTIC_CACHE_LINE_BYTES) {
			lines_remaining++;
			g_capture_dma_sentinel_lines_remaining++;
		}
	}

	return lines_remaining;
}

static void audio_capture_dma_prime_sentinel(unsigned int period)
{
	unsigned char sentinel =
		(unsigned char)(0x80 | (g_capture_dma_sentinel_sequence & 0x3F));

	g_capture_dma_sentinel_sequence++;
	g_capture_dma_sentinel[period] = sentinel;
	g_capture_dma_sentinel_valid_mask |= 1u << period;
	memset(g_audio_memory->capture[period], sentinel,
	       sizeof(g_audio_memory->capture[period]));
}
#endif

static int audio_capture_dma_init(void)
{
	union AudioDmaOpCallbackPointer callback = {
		.abi = audio_capture_dma_complete
	};
	int ret;

	g_capture_dma_op_id = ksceKernelDmaOpAlloc("uac_srcmix1_aux");
	diagnostic_record("allocate SrcMix1 secondary DMA",
		g_capture_dma_op_id);
	if (g_capture_dma_op_id < 0)
		return g_capture_dma_op_id;

	ret = ksceKernelDmaOpSetCallback(g_capture_dma_op_id,
					callback.sdk, NULL);
	diagnostic_record("set SrcMix1 secondary DMA callback", ret);
	if (ret < 0)
		goto fail;

	ret = ksceKernelDmaOpAssign(g_capture_dma_op_id,
				   SCE_KERNEL_DMAC_ID_DMAC4,
				   SRCMIX1_SECONDARY_DMAC_CHANNEL);
	diagnostic_record("assign SrcMix1 secondary DMA channel", ret);
	if (ret < 0)
		goto fail;

	ksceKernelAtomicSet32(&g_capture_dma_running, 0);
	ksceKernelAtomicSet32(&g_capture_dma_done_mask, 0);
	ksceKernelAtomicSet32(&g_capture_dma_free_mask, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_stage, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_status, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_bytes, 0);
	ksceKernelAtomicSet32(&g_capture_dma_callback_period, 0);
	return 0;

fail:
	(void)ksceKernelDmaOpFree(g_capture_dma_op_id);
	g_capture_dma_op_id = -1;
	return ret;
}

static int audio_capture_dma_term(void)
{
	int ret;

	if (g_capture_dma_op_id < 0)
		return 0;
	if (ksceKernelAtomicGetAndAdd32(&g_capture_dma_running, 0))
		return SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;

	ret = ksceKernelDmaOpFree(g_capture_dma_op_id);
	diagnostic_record("free SrcMix1 secondary DMA", ret);
	if (ret < 0)
		return ret;

	g_capture_dma_op_id = -1;
	return 0;
}

static int audio_capture_dma_start(void)
{
	SceKernelIntrStatus interrupt_state;
	unsigned int period;
	unsigned int ignored;
	int append_attempted = 0;
	int append_ret = 0;
	int enqueue_attempted = 0;
	int enqueue_ret = 0;
	int setup_done = 0;
	int setup_ret;
	int ret;

	if (g_capture_dma_op_id < 0 || !g_srcmix1_regs ||
	    !g_srcmix1_spinlock || !g_srcmix1_output_mode ||
	    !g_srcmix2_input1_state)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	ksceKernelAtomicSet32(&g_capture_dma_done_mask, 0);
	ksceKernelAtomicSet32(
		&g_capture_dma_free_mask,
		(SceInt32)(AUDIO_CAPTURE_BUFFER_MASK &
			~((1u << AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH) - 1)));
	ksceKernelAtomicSet32(&g_capture_dma_error, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_stage, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_status, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_bytes, 0);
	ksceKernelAtomicSet32(&g_capture_dma_callback_period, 0);
#ifdef DIAGNOSTIC
	g_capture_dma_previous_valid_mask = 0;
	g_capture_dma_lines_compared = 0;
	g_capture_dma_lines_unchanged = 0;
	g_capture_dma_sentinel_valid_mask = 0;
	g_capture_dma_sentinel_sequence = 0;
	g_capture_dma_sentinel_lines_tested = 0;
	g_capture_dma_sentinel_lines_remaining = 0;
	g_capture_dma_tag_mutation_count = 0;
	g_capture_dma_callback_count = 0;
	g_capture_dma_callback_busy_count = 0;
	g_capture_dma_callback_sync_count = 0;
	g_capture_dma_callback_status_or = 0;
#endif
	(void)ksceKernelPollEventFlag(g_capture_event_id, AUDIO_THREAD_DMA,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);

	for (period = 0; period < AUDIO_CAPTURE_BUFFER_COUNT; period++) {
		audio_capture_dma_prepare_period(period);
#ifdef DIAGNOSTIC
		audio_capture_dma_prime_sentinel(period);
#endif
	}

	/*
	 * Match Sony's secondary-monitor start sequence while holding the
	 * SrcMix1 context lock: disabled output, DSB, armed DMA, then 0x10001.
	 * No diagnostic file I/O is performed with interrupts suspended.
	 */
	interrupt_state = ksceKernelSpinlockLowLockCpuSuspendIntr(
		g_srcmix1_spinlock);
	if (*g_srcmix1_output_mode != 0 ||
	    *g_srcmix2_input1_state != 0 ||
	    g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX] !=
		    SRCMIX_SECONDARY_CONTROL_DISABLED) {
		ret = SCE_UDCD_ERROR_DRIVER_IN_PROGRESS;
		setup_ret = ret;
		goto unlock;
	}

	g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX] =
		SRCMIX_SECONDARY_CONTROL_DISABLED;
	audio_data_sync_barrier();

	setup_ret = ksceKernelDmaOpSetupChain(g_capture_dma_op_id,
		g_capture_dma_period[0].tag,
		(SceKernelDmaOpChainParam *)&g_capture_dma_chain_param,
		SCE_KERNEL_DMA_OP_VIRTUAL_DST_ADDR);
	ret = setup_ret;
	if (ret < 0)
		goto unlock;
	setup_done = 1;

	for (period = 1;
	     period < AUDIO_CAPTURE_INITIAL_QUEUE_DEPTH;
	     period++) {
		append_attempted = 1;
		append_ret = ksceKernelDmaOpConcatenate(g_capture_dma_op_id,
			g_capture_dma_period[period].tag,
			SCE_KERNEL_DMA_OP_VIRTUAL_DST_ADDR);
		ret = append_ret;
		if (ret < 0)
			goto unlock;
	}

	ksceKernelAtomicSet32(&g_capture_dma_running, 1);
	enqueue_attempted = 1;
	enqueue_ret = ksceKernelDmaOpEnQueue(g_capture_dma_op_id);
	ret = enqueue_ret;
	if (ret < 0) {
		ksceKernelAtomicSet32(&g_capture_dma_running, 0);
		goto unlock;
	}

	g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX] =
		SRCMIX_SECONDARY_CONTROL_ENABLED;
	audio_data_sync_barrier();
	ret = 0;

unlock:
	ksceKernelSpinlockLowUnlockCpuResumeIntr(g_srcmix1_spinlock,
		interrupt_state);
	diagnostic_record("setup SrcMix1 secondary DMA", setup_ret);
	if (append_attempted)
		diagnostic_record("append SrcMix1 secondary DMA period",
			append_ret);
	if (enqueue_attempted)
		diagnostic_record("enqueue SrcMix1 secondary DMA", enqueue_ret);
	if (ret >= 0) {
		diagnostic_record("enable SrcMix1 secondary output",
			(int)g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX]);
		return 0;
	}

	if (setup_done) {
		int quit_ret = ksceKernelDmaOpQuit(g_capture_dma_op_id);
		diagnostic_record("quit failed SrcMix1 secondary start",
			quit_ret);
		if (quit_ret < 0)
			return quit_ret;
	}
	return ret;
}

static int audio_capture_dma_stop(void)
{
	SceKernelIntrStatus interrupt_state;
	unsigned int ignored;
	uint32_t secondary_control = SRCMIX_SECONDARY_CONTROL_DISABLED;
	int dequeue_ret;
	int quit_ret;
	int was_running;

	was_running = ksceKernelAtomicGetAndSet32(&g_capture_dma_running, 0);
	if (!was_running)
		return 0;

	/* Stop the FIFO producer before dismantling its DMA consumer. */
	if (g_srcmix1_regs && g_srcmix1_spinlock) {
		interrupt_state = ksceKernelSpinlockLowLockCpuSuspendIntr(
			g_srcmix1_spinlock);
		g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX] =
			SRCMIX_SECONDARY_CONTROL_DISABLED;
		audio_data_sync_barrier();
		secondary_control =
			g_srcmix1_regs[SRCMIX_SECONDARY_CONTROL_INDEX];
		ksceKernelSpinlockLowUnlockCpuResumeIntr(g_srcmix1_spinlock,
			interrupt_state);
	}
	diagnostic_record("disable SrcMix1 secondary output",
		(int)secondary_control);

	/*
	 * Quit releases the operation's tags but does not unlink an operation
	 * which is queued behind another client on the same physical channel.
	 * Remove that queue node first; NOT_QUEUED and ON_TRANSFERRING are the
	 * two expected races with normal DMAC4 progress.
	 */
	dequeue_ret = ksceKernelDmaOpDeQueue(g_capture_dma_op_id);
	diagnostic_record("dequeue SrcMix1 secondary DMA", dequeue_ret);

	quit_ret = ksceKernelDmaOpQuit(g_capture_dma_op_id);
	diagnostic_record("quit SrcMix1 secondary DMA", quit_ret);
	if (quit_ret < 0)
		return quit_ret;
	if (dequeue_ret < 0 &&
	    dequeue_ret != (int)SCE_KERNEL_ERROR_NOT_QUEUED &&
	    dequeue_ret != (int)SCE_KERNEL_ERROR_ON_TRANSFERRING)
		return dequeue_ret;

	ksceKernelAtomicSet32(&g_capture_dma_done_mask, 0);
	ksceKernelAtomicSet32(&g_capture_dma_free_mask, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_stage, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_status, 0);
	ksceKernelAtomicSet32(&g_capture_dma_error_bytes, 0);
	(void)ksceKernelPollEventFlag(g_capture_event_id, AUDIO_THREAD_DMA,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);
	return 0;
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

static unsigned int audio_ring_pop_request(unsigned char *request_data)
{
	AudioPcmFrame *dst = (AudioPcmFrame *)request_data;
	unsigned int count = AUDIO_USB_REQUEST_FRAMES;
	unsigned int first;
	unsigned int used;

	memset(request_data, 0,
	       AUDIO_USB_REQUEST_FRAMES * sizeof(AudioPcmFrame));

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
			return AUDIO_USB_REQUEST_FRAMES;
		}
		g_ring_rebuffering = 0;
	}

	if (used < count) {
		g_ring_rebuffering = 1;
		ksceKernelUnlockFastMutex(&g_audio_ring_mutex);
		return AUDIO_USB_REQUEST_FRAMES;
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
	unsigned char *request_data = g_audio_memory->usb_packet[slot];
	unsigned int frames;
	int ret;

	if (silence) {
		frames = AUDIO_USB_REQUEST_FRAMES;
		memset(request_data, 0, frames * sizeof(AudioPcmFrame));
	} else {
		frames = audio_ring_pop_request(request_data);
	}

	ksceKernelDcacheCleanRange(request_data,
				  frames * sizeof(AudioPcmFrame));

	usb_slot->generation = generation;
	usb_slot->request = (SceUdcdDeviceRequest){
		.endpoint = g_audio_endpoint,
		.data = request_data,
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
	 * A final DMA completion may race the stop wake. Clear both notifications
	 * before publishing stopped so neither can leak into the next generation.
	 */
	(void)ksceKernelPollEventFlag(g_capture_event_id,
		AUDIO_THREAD_WAKE | AUDIO_THREAD_DMA,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &ignored);

	generation = ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0);
	if (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0))
		return;

	ksceKernelAtomicSet32(&g_capture_stopped_generation, generation);
	ksceKernelSetEventFlag(g_state_event_id, AUDIO_CAPTURE_STOPPED);
}

static int audio_capture_run(unsigned int generation)
{
	unsigned int expected_period = 0;
	int ret;

	ret = audio_capture_dma_start();
	diagnostic_record("start SrcMix1 secondary capture", ret);
	if (ret < 0) {
		audio_signal_failure();
		return 0;
	}

	while (ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) &&
	       generation == (unsigned int)ksceKernelAtomicGetAndAdd32(
		       &g_audio_generation, 0)) {
		unsigned int done_mask;
		unsigned int out_bits;
		SceUInt timeout = AUDIO_DMA_TIMEOUT_US;
		int dma_error;

		ret = ksceKernelWaitEventFlag(g_capture_event_id,
			AUDIO_THREAD_WAKE | AUDIO_THREAD_DMA,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, &timeout);
		if (ret < 0) {
			diagnostic_record("SrcMix1 secondary DMA wait timeout",
				ret);
			audio_signal_failure();
			break;
		}

		if (!ksceKernelAtomicGetAndAdd32(&g_audio_desired, 0) ||
		    generation != (unsigned int)ksceKernelAtomicGetAndAdd32(
			    &g_audio_generation, 0))
			break;

		dma_error =
			ksceKernelAtomicGetAndSet32(&g_capture_dma_error, 0);
		if (dma_error) {
			diagnostic_record("SrcMix1 secondary DMA error stage",
				ksceKernelAtomicGetAndAdd32(
					&g_capture_dma_error_stage, 0));
			diagnostic_record("SrcMix1 secondary DMA error status",
				ksceKernelAtomicGetAndAdd32(
					&g_capture_dma_error_status, 0));
			diagnostic_record("SrcMix1 secondary DMA error bytes",
				ksceKernelAtomicGetAndAdd32(
					&g_capture_dma_error_bytes, 0));
			audio_signal_failure();
			break;
		}

		done_mask = (unsigned int)ksceKernelAtomicGetAndSet32(
			&g_capture_dma_done_mask, 0);
		while (done_mask) {
			unsigned int expected_mask = 1u << expected_period;
#ifdef DIAGNOSTIC
			unsigned int sentinel_lines_remaining;
#endif

			/*
			 * DMAC4 completes the bounded period queue in order.
			 * Rejecting any other order avoids silently shuffling PCM
			 * if a future DmacMgr changes callback-tag semantics.
			 */
			if (!(done_mask & expected_mask)) {
				diagnostic_record(
					"SrcMix1 secondary DMA period order",
					(int)done_mask);
				audio_signal_failure();
				break;
			}
			done_mask &= ~expected_mask;

#ifdef DIAGNOSTIC
			sentinel_lines_remaining =
				audio_capture_dma_measure_sentinel(expected_period);
			audio_capture_dma_measure_visibility(expected_period);
#endif
#ifdef DIAGNOSTIC
			if (!sentinel_lines_remaining)
#endif
			audio_ring_push(
				g_audio_memory->capture[expected_period],
				AUDIO_CAPTURE_FRAMES);

			ret = ksceKernelAtomicGetAndOr32(
				&g_capture_dma_free_mask,
				(SceInt32)expected_mask);
			if (ret & expected_mask) {
				audio_capture_dma_fail(
					6, ret,
					sizeof(g_audio_memory
						       ->capture[expected_period]));
				audio_signal_failure();
				break;
			}

			if (!ksceKernelAtomicGetAndAdd32(
				    &g_audio_desired, 0) ||
			    generation !=
				    (unsigned int)ksceKernelAtomicGetAndAdd32(
					    &g_audio_generation, 0))
				break;

			expected_period++;
			if (expected_period == AUDIO_CAPTURE_BUFFER_COUNT)
				expected_period = 0;
		}
	}

	ret = audio_capture_dma_stop();
	if (ret < 0) {
		ksceKernelAtomicSet32(&g_capture_unsafe, 1);
		audio_signal_failure();
		return ret;
	}

#ifdef DIAGNOSTIC
	diagnostic_record("SrcMix1 DMA cache lines compared",
			  (int)g_capture_dma_lines_compared);
	diagnostic_record("SrcMix1 DMA cache lines unchanged",
			  (int)g_capture_dma_lines_unchanged);
	diagnostic_record("SrcMix1 DMA sentinel lines tested",
			  (int)g_capture_dma_sentinel_lines_tested);
	diagnostic_record("SrcMix1 DMA sentinel lines remaining",
			  (int)g_capture_dma_sentinel_lines_remaining);
	diagnostic_record("SrcMix1 DMA tag mutations",
			  (int)g_capture_dma_tag_mutation_count);
	diagnostic_record("SrcMix1 DMA callback count",
			  (int)g_capture_dma_callback_count);
	diagnostic_record("SrcMix1 DMA busy callbacks",
			  (int)g_capture_dma_callback_busy_count);
	diagnostic_record("SrcMix1 DMA sync callbacks",
			  (int)g_capture_dma_callback_sync_count);
	diagnostic_record("SrcMix1 DMA callback status OR",
			  (int)g_capture_dma_callback_status_or);
#endif
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
	(void)ksceKernelPollEventFlag(g_capture_event_id,
		AUDIO_THREAD_WAKE | AUDIO_THREAD_DMA,
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
		SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW,
		memory_size, &opt);
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

	ret = audio_capture_dma_init();
	if (ret < 0)
		goto fail_state_event;

	g_capture_thread_id = ksceKernelCreateThread("uac_capture_thread",
		audio_capture_thread, 0x3A, 0x2000, 0,
		AUDIO_CAPTURE_CPU_AFFINITY, NULL);
	if (g_capture_thread_id < 0) {
		ret = g_capture_thread_id;
		goto fail_capture_dma;
	}

	g_usb_thread_id = ksceKernelCreateThread("uac_usb_thread",
		audio_usb_thread, 0x38, 0x2000, 0,
		AUDIO_USB_CPU_AFFINITY, NULL);
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
fail_capture_dma:
	(void)audio_capture_dma_term();
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

	audio_request_stop_locked();
	ret = audio_wait_stopped(
		ksceKernelAtomicGetAndAdd32(&g_audio_generation, 0));
	if (ret < 0)
		goto out;

	ret = audio_resolve_srcmix1_secondary();
	if (ret < 0)
		goto out;

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
	ret = audio_capture_dma_term();
	if (ret < 0)
		return ret;
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
