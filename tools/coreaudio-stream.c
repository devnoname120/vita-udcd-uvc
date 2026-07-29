#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STREAM_RING_CAPACITY (1u << 22)
#define STREAM_RING_MASK (STREAM_RING_CAPACITY - 1)

struct StreamState {
	AudioStreamBasicDescription format;
	unsigned char *ring;
	_Atomic uint64_t read_position;
	_Atomic uint64_t write_position;
	_Atomic uint64_t callbacks;
	_Atomic uint64_t frames;
	_Atomic uint64_t overruns;
};

static volatile sig_atomic_t g_running = 1;

static void stop_streaming(int signal_number)
{
	(void)signal_number;
	g_running = 0;
}

static OSStatus input_callback(AudioObjectID device,
			       const AudioTimeStamp *now,
			       const AudioBufferList *input,
			       const AudioTimeStamp *input_time,
			       AudioBufferList *output,
			       const AudioTimeStamp *output_time,
			       void *context)
{
	struct StreamState *state = context;
	const AudioBuffer *buffer;
	uint64_t read_position;
	uint64_t write_position;
	size_t data_size;
	size_t first;

	(void)device;
	(void)now;
	(void)input_time;
	(void)output;
	(void)output_time;

	if (input->mNumberBuffers != 1)
		return noErr;

	buffer = &input->mBuffers[0];
	data_size = buffer->mDataByteSize;
	write_position =
		atomic_load_explicit(&state->write_position,
				     memory_order_relaxed);
	read_position =
		atomic_load_explicit(&state->read_position,
				     memory_order_acquire);

	if (data_size > STREAM_RING_CAPACITY -
				(size_t)(write_position - read_position)) {
		atomic_fetch_add_explicit(&state->overruns, 1,
					  memory_order_relaxed);
		return noErr;
	}

	first = STREAM_RING_CAPACITY -
		(size_t)(write_position & STREAM_RING_MASK);
	if (first > data_size)
		first = data_size;
	memcpy(state->ring + (write_position & STREAM_RING_MASK),
	       buffer->mData, first);
	memcpy(state->ring, (const unsigned char *)buffer->mData + first,
	       data_size - first);

	atomic_fetch_add_explicit(&state->callbacks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(
		&state->frames,
		data_size / state->format.mBytesPerFrame,
		memory_order_relaxed);
	atomic_store_explicit(&state->write_position,
			      write_position + data_size,
			      memory_order_release);
	return noErr;
}

static int get_cfstring(AudioObjectID object,
			AudioObjectPropertySelector selector,
			char *buffer,
			size_t buffer_size)
{
	AudioObjectPropertyAddress address = {
		selector,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	CFStringRef value = NULL;
	UInt32 size = sizeof(value);

	if (AudioObjectGetPropertyData(object, &address, 0, NULL,
				       &size, &value) != noErr ||
	    !value)
		return -1;
	if (!CFStringGetCString(value, buffer, buffer_size,
				kCFStringEncodingUTF8)) {
		CFRelease(value);
		return -1;
	}
	CFRelease(value);
	return 0;
}

static AudioDeviceID find_input_device(const char *target_name)
{
	AudioObjectPropertyAddress devices_address = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	AudioDeviceID *devices;
	UInt32 size;
	UInt32 count;
	UInt32 index;
	AudioDeviceID result = kAudioObjectUnknown;

	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
					   &devices_address, 0, NULL,
					   &size) != noErr)
		return result;
	devices = malloc(size);
	if (!devices)
		return result;
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
				       &devices_address, 0, NULL,
				       &size, devices) != noErr) {
		free(devices);
		return result;
	}

	count = size / sizeof(*devices);
	for (index = 0; index < count; index++) {
		AudioObjectPropertyAddress streams_address = {
			kAudioDevicePropertyStreams,
			kAudioDevicePropertyScopeInput,
			kAudioObjectPropertyElementMain
		};
		char name[256];
		UInt32 stream_size = 0;

		if (AudioObjectGetPropertyDataSize(
			    devices[index], &streams_address, 0, NULL,
			    &stream_size) != noErr ||
		    !stream_size)
			continue;
		if (get_cfstring(devices[index], kAudioObjectPropertyName,
				 name, sizeof(name)) < 0)
			continue;
		if (!strcmp(name, target_name)) {
			result = devices[index];
			break;
		}
	}

	free(devices);
	return result;
}

static int write_ring_to_stdout(struct StreamState *state)
{
	uint64_t read_position = atomic_load_explicit(
		&state->read_position, memory_order_relaxed);
	uint64_t write_position = atomic_load_explicit(
		&state->write_position, memory_order_acquire);
	size_t available = (size_t)(write_position - read_position);
	size_t contiguous;
	ssize_t written;

	if (!available)
		return 0;

	contiguous = STREAM_RING_CAPACITY -
		(size_t)(read_position & STREAM_RING_MASK);
	if (contiguous > available)
		contiguous = available;
	if (contiguous > 65536)
		contiguous = 65536;

	written = write(STDOUT_FILENO,
			state->ring + (read_position & STREAM_RING_MASK),
			contiguous);
	if (written < 0) {
		if (errno == EINTR)
			return 0;
		if (errno == EPIPE)
			return 1;
		return -1;
	}
	atomic_store_explicit(&state->read_position,
			      read_position + (size_t)written,
			      memory_order_release);
	return 0;
}

int main(int argc, char **argv)
{
	const char *device_name = argc > 1 ? argv[1] : "PSVita";
	AudioObjectPropertyAddress format_address = {
		kAudioDevicePropertyStreamFormat,
		kAudioDevicePropertyScopeInput,
		kAudioObjectPropertyElementMain
	};
	struct StreamState state = {0};
	AudioDeviceIOProcID io_proc = NULL;
	AudioDeviceID device;
	UInt32 size;
	OSStatus error;
	int result = 0;

	device = find_input_device(device_name);
	if (device == kAudioObjectUnknown) {
		fprintf(stderr, "input device not found: %s\n", device_name);
		return 2;
	}

	size = sizeof(state.format);
	error = AudioObjectGetPropertyData(device, &format_address, 0, NULL,
					   &size, &state.format);
	if (error != noErr) {
		fprintf(stderr, "could not read input format: %d\n", error);
		return 3;
	}
	if (state.format.mFormatID != kAudioFormatLinearPCM ||
	    !(state.format.mFormatFlags & kAudioFormatFlagIsFloat) ||
	    (state.format.mFormatFlags &
	     kAudioFormatFlagIsNonInterleaved) ||
	    state.format.mSampleRate != 48000.0 ||
	    state.format.mChannelsPerFrame != 2 ||
	    state.format.mBitsPerChannel != 32 ||
	    state.format.mBytesPerFrame != 8) {
		fprintf(stderr,
			"unsupported format: rate=%.3f id=%08x flags=%08x "
			"channels=%u bits=%u bytesPerFrame=%u\n",
			state.format.mSampleRate,
			(unsigned int)state.format.mFormatID,
			(unsigned int)state.format.mFormatFlags,
			state.format.mChannelsPerFrame,
			state.format.mBitsPerChannel,
			state.format.mBytesPerFrame);
		return 4;
	}

	state.ring = malloc(STREAM_RING_CAPACITY);
	if (!state.ring) {
		fprintf(stderr, "could not allocate stream ring\n");
		return 5;
	}

	signal(SIGINT, stop_streaming);
	signal(SIGTERM, stop_streaming);
	signal(SIGPIPE, stop_streaming);

	error = AudioDeviceCreateIOProcID(device, input_callback, &state,
					  &io_proc);
	if (error != noErr) {
		fprintf(stderr, "AudioDeviceCreateIOProcID failed: %d\n", error);
		free(state.ring);
		return 6;
	}
	error = AudioDeviceStart(device, io_proc);
	if (error != noErr) {
		fprintf(stderr, "AudioDeviceStart failed: %d\n", error);
		AudioDeviceDestroyIOProcID(device, io_proc);
		free(state.ring);
		return 7;
	}

	fprintf(stderr,
		"streaming %s as f32le, 48000 Hz, stereo; press Ctrl-C to stop\n",
		device_name);
	while (g_running) {
		int write_result = write_ring_to_stdout(&state);

		if (write_result > 0)
			break;
		if (write_result < 0) {
			result = 8;
			break;
		}
		if (atomic_load_explicit(&state.read_position,
					 memory_order_relaxed) ==
		    atomic_load_explicit(&state.write_position,
					 memory_order_acquire))
			usleep(1000);
	}

	AudioDeviceStop(device, io_proc);
	AudioDeviceDestroyIOProcID(device, io_proc);
	fprintf(stderr, "callbacks=%llu frames=%llu overruns=%llu\n",
		(unsigned long long)atomic_load(&state.callbacks),
		(unsigned long long)atomic_load(&state.frames),
		(unsigned long long)atomic_load(&state.overruns));
	free(state.ring);
	return result;
}
