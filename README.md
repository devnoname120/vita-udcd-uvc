# PSVita UDCD USB Video Class plugin + audio streaming support

## What's this?

This is a kernel plugin that lets you stream your PSVita screen and its final
software audio mix to your computer via USB.

## How does it work?

The plugin uses the [SceUdcd](https://wiki.henkaku.xyz/vita/SceUdcd) module of the PSVita OS to setup
the necessary USB descriptors to simulate and behave as an [USB Video Class](https://en.wikipedia.org/wiki/USB_video_device_class) device (like a webcam or an USB video capture card).

The [hardware color space converter](https://wiki.henkaku.xyz/vita/IFTU_Registers) of the PSVita's SoC is used to perform the conversion to the destination pixel format; then the USB
controller directly performs a DMA transfer from the physical address of the resulting converted framebuffer, and therefore, saving CPU usage and power consumption.

Audio is exposed to the computer as a USB Audio Class 1.0 capture/input device
using 48 kHz, stereo, signed 16-bit PCM. It captures the final mix produced by
SceAudio and SceAudioSource before the handheld codec applies
speaker/headphone volume or mute. It does not capture the microphone.

Unlike audio-capture plugins that hook individual audio ports as they are
opened and closed (BGM, sound effects, and similar sources), this plugin
captures the already-combined final digital mix immediately before it is
routed to the speaker/headphone codec. The USB stream therefore contains the
mix actually produced by the Vita instead of a best-effort reconstruction
performed by the plugin. Reconstructing the mix from separate ports can miss
audio, lose synchronization, or produce clicks, gaps, and other glitches.

Due to a PS Vita audio-routing limitation, sound is not played through the
Vita speakers while the computer is actively capturing the USB audio stream.
Speaker playback resumes immediately when the computer stops capturing audio
or the USB cable is disconnected.

## Supported formats and resolutions

### Audio

* 48 kHz stereo signed 16-bit PCM (1,536 kb/s)

### Video

* 960x544 @ 30 FPS and (less than) 60 FPS
* 896x504 @ 30 FPS and (almost) 60 FPS
* 864x488 @ 30 FPS and 60 FPS
* 480x272 @ 30 FPS and 60 FPS
* 1280x720 @ 30 FPS

## Download and installation

**Download**:

* [udcd\_uvc.skprx](https://github.com/devnoname120/vita-udcd-uvc/releases)

**Compilation**

* [vitasdk](https://vitasdk.org/) is needed.

### Video pipeline

The default build uses two dynamically sized conversion buffers. The existing
video worker can convert the next frame while the USB controller transmits the
previous one; only one USB video request is queued at a time. A buffer is not
reused or freed until its request completes, including after cancellation.
All five video modes and the USB audio path are retained.

Allocation first uses the original kernel memory pool. If that pool has no
free contiguous physical pages, it retries the kernel's dedicated physically
contiguous, non-cacheable pool. Both pools remain finite shared resources;
if the second buffer still cannot be allocated, capture falls back to one buffer.
For a deliberately single-buffer build, use:

```sh
make clean
make PARALLEL=0
```

Run `make clean` before switching build options. The second buffer uses an
additional 768 KiB at 960x544, or 1352 KiB at 1280x720, while allocated.
Idle capture releases its buffers. Overlap does not increase USB bandwidth;
its effect depends on the selected mode and host. See the
[hardware test report](docs/performance-2026-09-19.md) for measured capture rates
and the remaining testing limits. The overlap is adapted from
[trap15's change](https://github.com/trap15/vita-udcd-uvc/commit/2ad09ffa8453d8b0ea5bc283566bd25155832236),
with explicit buffer ownership and without removing 720p. Video remains
uncompressed NV12, using IFTU and USB DMA; the audio implementation is unchanged.

Capture pacing uses integer arithmetic at the nominal 60 Hz vblank cadence.
It tolerates either rounding of the supported frame intervals, retains the
fractional remainder, and coalesces delayed notifications instead of queuing
stale captures. Timing changes take effect on COMMIT, not while the host is
still probing, and a new COMMIT resets the pacing phase.

### Host tests

```sh
make test
```

The tests require Python 3 and a native C compiler supporting AddressSanitizer
and UndefinedBehaviorSanitizer. They exercise the production video transport,
capture scheduling, and shutdown path with simulated kernel/USB dependencies.
They do not replace device tests of IFTU, USB timing, or simultaneous audio.

**Installation**:

1. Copy `udcd_uvc.skprx` to your PSVita
2. Add `udcd_uvc.skprx` to taiHEN's config (`ur0:/tai/config.txt` or `ux0:/tai/config.txt`):
```
*KERNEL
ur0:tai/udcd_uvc.skprx
```
3. Reboot your PSVita.

## Listening to audio on macOS

On macOS, FFmpeg versions older than 8.1.3 have an audio bug that prevents
sound from working correctly. Upgrade to FFmpeg 8.1.3 or later.

## Troubleshooting

If the video looks glitched, try to change the video player configuration to use the *NV12* format or switch to another player (like PotPlayer or OBS). If the colors look wrong, set color range to full and color space to BT.601 (Rec. 601).

If you use Windows 10 you might have to change the Camera access permissions on the Privacy Settings.

On Linux I recommend using *mplayer* (`mplayer tv:// -tv driver=v4l2:device=/dev/videoX:width=960:height=544`).

The Vita's Bluetooth audio path uses the same single firmware monitor slot as
USB audio capture. Do not use Bluetooth audio while USB audio streaming is
active; simultaneous use is not supported.

Note: Remember that if anything goes wrong (like PSVita not booting) you can always press L at boot to skip plugin loading.
