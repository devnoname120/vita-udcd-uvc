# Video allocation and performance checks

Date: 2026-09-19, Europe/Zurich. Baseline: `13ee8b7`, based on the
`devnoname120/vita-udcd-uvc` main branch, not the experimental audio branch.

## Change retained

`src/video.c` first tries the original `0x10208006` kernel allocation. Only
`SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE` triggers a retry with
`SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW` (`0x30808006`). The same
size, physical-contiguity attribute, alignment, ownership rules, and release
path apply. If allocation still fails, the existing single-buffer fallback
remains available.

There is no new per-frame operation, video thread, codec, image copy, USB
request queue, clock change, or audio change. The retry is confined to buffer
allocation. Successful primary allocations do not use the alternative pool.
Both pools are finite shared resources: this is not additional free memory or
a guarantee of unchanged memory availability for every other kernel client.

## Why this change, rather than more queued requests

A temporary diagnostic build recorded allocation results and timestamps in
memory, then wrote a finite trace. It found:

| Baseline mode | Allocated buffers | Mean CSC | Mean USB submission-to-callback | Mean completion-to-next-submit |
|---|---:|---:|---:|---:|
| 960x544 | 2 | 2.220 ms | 19.261 ms | 0.019 ms |
| 1280x720 | 1 | 3.156 ms | 33.943 ms | 3.186 ms |

The second 720p allocation failed with `0x80024302`, so the pipeline was
correctly falling back to serial conversion/transmission. The fallback test
allocated both 720p buffers successfully.

A second queued request cannot recover the roughly 3 ms conversion cost when
only one image buffer exists. At native resolution, eliminating the entire
measured 19 microsecond re-arm gap would recover only about 0.1% of the
19.28 ms submission period. The proposed deeper queue was therefore not added.

The diagnostic's one-time file write disturbed capture timing outside its
recorded sample. Its whole-run FPS and maximum gap are not used as the
performance comparison below.

## Firmware verification

Inspection of the supplied firmware 3.60 SceSysmem dump in IDA confirmed the
allocation-type table and dispatch path. `ksceKernelAllocMemBlockWithInfo`
resolves the type; `fun_DispatchMemBlockAllocation` and `sub_AC9C50` route these
types through the kernel address space. The type table maps the original type
to `0x30608006`, while the retry uses `0x30808006`; the physical-resource
selector used by `sub_ACCB5C` differs. This is a genuine alternative allocator
path, not removal of a physical-contiguity requirement or selection of a user
process's framebuffer.

The named error matches the installed vitasdk error definition. The retry's
actual usefulness and DMA compatibility were subsequently tested on the Vita;
source inspection alone was not treated as sufficient evidence.

SceUdcd inspection also confirmed that an already queued software request can
be started before the previous client's callback. This does not imply a
hardware DMA chain across whole video frames, nor does it make the measured
19 microsecond software gap a significant bottleneck. Ordinary IN completion
can assign the requested length to `transmitted`; that field is not used as an
independent wire-byte counter.

## Release-like on-device comparison

The comparison used normal builds without timing instrumentation. Both runs
used the same main LiveArea page, Mac capture application, USB connection, and
saved PSVshell profile. The device rebooted when changing the binary. The
screen was explicitly turned on before every capture. The saved profile was
500/222/222/166 MHz for CPU/GPU/bus/crossbar; hardware clock registers were not
independently sampled. USB audio was inactive for the comparison pair.

| Requested mode | Before fallback | After fallback | Result |
|---|---:|---:|---|
| 960x544 at 60 FPS | 51.8854 FPS | 51.8802 FPS | Unchanged |
| 1280x720 at 30 FPS | 26.9048 FPS | 29.3461 FPS | Approximately +9.1% |

Each comparison capture lasted approximately ten seconds. Rates use
`(timestamped_frames - 1) / timestamp_span`, not requested FPS. This is one
short pair per mode, not a randomized repeated-trial benchmark. The normal
720p run did not sustain an uninterrupted 30 FPS: its largest timestamp gap
was 69.37 ms. A separate diagnostic run reached about 30 FPS but is not a
substitute for the uninstrumented comparison.

Native and 720p images were inspected and show the main LiveArea screen.
All tested resolutions had the requested dimensions and changing image data.
Changed image data establishes a live stream, not a count of unique game
renders. No improvement in game FPS, CPU cost, or end-to-end latency is claimed.

## Functional checks

The normal candidate also passed capture at 896x504, 864x488, and 480x272,
with short-window received rates around 60 FPS. After simultaneous video/audio,
video-only capture restarted at native 30 FPS and then native 60 FPS.

Simultaneous 720p video and audio produced 298 video frames and 481,280 stereo
PCM frames at 48 kHz, or 10.027 seconds of sample data. Both channels were
non-silent: 478,698 nonzero left samples and 478,743 nonzero right samples.
Peaks were 7,890 and 7,998 respectively, with no clipped samples. This is a
numerical signal check, not a perceptual audio-quality or long-term drift test.

The AV session's first timestamps were 0, 0.033333333, then host uptime. Its
reported aggregate FPS therefore mixed time origins and is excluded from FPS
claims. The raw result is retained, not silently corrected. An earlier
experimental runner also passed `av` instead of the tester's required `audio`
argument; that run did not enable audio and was rejected as an audio test.
The corrected runner verifies the requested audio mode and non-silent PCM.

Host callbacks reported zero dimension mismatches and zero dropped callbacks.
The latter does not establish that no device/USB-level frames were missed.

## Software verification and installed state

The new regression test failed before the production change, then passed.
Fifteen transport/capture groups passed in each buffer configuration with
AddressSanitizer and UndefinedBehaviorSanitizer, alongside seven shutdown
scenarios and six pacing groups. New cases cover primary allocation failure,
successful retry, failure of both pools, address/base acquisition failures,
cleanup, and absence of retry for unrelated errors.

All twelve cross-build configurations passed without compiler warnings or
unresolved symbols: normal, OLED-off, LCD-off, diagnostic, debug, and
debug-plus-diagnostic, each with default buffering and `PARALLEL=0`.
The normal ELF text size increased by 16 bytes versus `13ee8b7`.

The normal candidate was read back from `ur0:tai/udcd_uvc.skprx` and matched
SHA-256 `4fd1145f4ef8705775eb3bd184d1666c43938fb125eb81aa6daa61d722343d87`.
The taiHEN configuration and saved clock profile remained unchanged.
Release 1.8 and `13ee8b7` rollback copies remain available on the Vita.
Audio sources and USB descriptor sources are byte-identical to the baseline.

Long-run allocation pressure, game workloads, unplug/suspend/unload testing,
perceived latency, and audio/video synchronization remain outside these short
checks. No speculative controller-register hook or compressed format was added.

## Local evidence

The generated evidence remains under `tests/.build/queue-experiment/`:
`POOL_RELEASE_RESULT.json`, `POOL_AUDIO_RESULT.json`, the `timing-*.bin` and
`timing-*.timing.json` files, deployment records, and the isolated
`pool-release/.build/matrix/` logs and binaries. Raw captures and images are in
`tests/.build/device-test/retry-20260919-115139/` under the corresponding
`allocation-before-*` and `allocation-after-*` names. These ignored directories
are machine-local test evidence, not files published with a release.
