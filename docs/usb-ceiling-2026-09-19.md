# USB transfer ceiling: front versus rear Mac mini port

Date: 2026-09-19, Europe/Zurich. Embedded-sequence follow-up: 2026-09-20.
Production baseline: release 1.9, commit
`1cf3272b957a427394511d44c7aed36f35a89d31`.

## Result and decision

The best validated sustained video-only payload rate on the tested rear-port
connection is approximately **45.0 MB/s**. Raw static USB transfers and ordinary
native-resolution UVC capture reach effectively the same rate. Removing colour
conversion, vblank pacing and the Mac's video-capture stack did not recover
meaningful additional throughput.

Changing from the previously tested front-port path to the rear port raised
ordinary 960x544 capture from approximately **52.15 to 57.45 received FPS** with
the same release-1.9 binary. Steady 1280x720 capture reached approximately
**29.97 FPS**. This is a connection-path improvement, not a new code optimization.

No further production-code change was retained. Extra host buffering, a larger
read size, changed static content and better transfer-buffer alignment failed
to provide an additional material gain. The measured result does not justify
adding video workers, deeper device queues, pixel copies, codecs or speculative
controller-register patches. Release 1.9's existing hardware IFTU conversion,
USB DMA and allocation fallback remain unchanged.

This is the best observed ceiling of the tested configurations, **not a proof
of the maximum attainable by every Vita, host, firmware or controller setting**.
The result must not be generalized to every audio/video scheduling configuration.
A follow-up on 2026-09-20 resolved the tested A/V callback discrepancy: the
extra callbacks repeated frame sequences, while distinct delivery stayed near
57.44 FPS. See the embedded-sequence results below.

## Conditions and topology

The cable/adapter and Vita were unchanged. The user moved the connection to a
rear USB-C port. IORegistry then placed `PSVita@00100000` directly under
`AppleT8132USBXHCI@00000000`, with no intervening hub. The previous front-port
path included an Apple USB2 hub. The other hub-connected devices remained on
a different controller path.

Reference captures used main LiveArea. The runners issued `quit all` when
preparing the reference scene, then `screen on` through VitaCompanion before
each capture, checking its acknowledgment. The live samples had changing pixel
data and correct dimensions; this does not count unique game-rendered frames.

The saved PSVshell profile and taiHEN configuration were checked byte-for-byte
and left unchanged. The saved profile specifies CPU/GPU/bus/crossbar values of
500/222/222/166 MHz; actual hardware clock registers were not independently
sampled. These are not stock-clock game-performance measurements.

## Rear-port measurements

MB/s means decimal megabytes per second. Unless explicitly called image data,
the figures below include the 12-byte UVC-like frame header, but exclude USB
transaction overhead on the physical bus.

| Test | Host reads in flight | Host read size | Payload rate |
|---|---:|---:|---:|
| Raw static native-sized frames, first rear-port run | 8 | 2 MiB | 45.003511 MB/s |
| Raw static native-sized frames, repeated | 8 | 2 MiB | 45.002722 MB/s |
| Raw static native-sized frames | 1 | 2 MiB | 44.168383 MB/s |
| Raw static native-sized frames | 16 | 4 MiB | 44.989910 MB/s |
| Raw static native-sized frames | 8 | 64 KiB | 44.995023 MB/s |
| Static UVC, aligned-buffer variant | UVC driver managed | UVC driver managed | 45.005157 MB/s |
| Static UVC, pseudorandom payload | UVC driver managed | UVC driver managed | 45.002121 MB/s |
| Actual LiveArea UVC capture, 960x544 | UVC driver managed | UVC driver managed | 45.002820 MB/s |
| Actual LiveArea UVC capture after restoring release 1.9 | UVC driver managed | UVC driver managed | 45.003806 MB/s |

The final digits are retained for reproducibility, not as a claim of a precise
or statistically significant ranking. The tiny differences near 45.0 MB/s are
not treated as performance gains. In particular, the aligned-buffer result is
not evidence for a useful production alignment change.

The queue depths in this table are **host libusb reads**, not queued Vita video
requests. The temporary Vita sender retained the existing one-outstanding-request
transport. Increasing the host queue from one to eight removes avoidable host
read starvation in the test harness; normal UVC capture already achieves the
same saturated rate without a plugin change.

### Live capture

| Requested mode | Steady received FPS | Image data rate |
|---|---:|---:|
| 960x544 at 60 FPS, initial rear run | 57.447630 | 45.002176 MB/s |
| 960x544 at 60 FPS, repeated rear run | 57.447572 | 45.002130 MB/s |
| 960x544 at 60 FPS, after final static sweep | 57.448832 | 45.003117 MB/s |
| 1280x720 at 30 FPS, initial rear run | 29.970654 | 41.431432 MB/s |
| 1280x720 at 30 FPS, repeated rear run | 29.969830 | 41.430294 MB/s |
| 480x272 at 60 FPS | 59.941563 | 11.738956 MB/s |

The initial two seconds were excluded from steady-state calculations. For the
repeated 720p run, the maximum steady timestamp gap was 33.667 ms. Its whole-run
maximum was 64.967 ms during startup; that observation is retained rather than
claiming there were no startup gaps. Native steady gaps in the ordinary runs
were generally near 17.4 ms. No host-reported dimension mismatches or dropped
callbacks occurred in those runs. Those callback statistics do not measure all
possible device/USB-level losses.

## What the static/raw tests actually do

The temporary Vita builds allocate the frame buffers and initialize their
pixels once. They skip the per-frame framebuffer query, IFTU conversion and
vblank pacing. They transmit as quickly as completed USB requests permit,
changing only a four-byte sequence number in the image and the UVC header.
They do not copy the full image on the CPU for every transfer.

The raw control uses a temporary vendor-class video interface and a separate
product ID, while retaining the same bulk transfer implementation and packet
size. A libusb reader claims that explicit test interface instead of using
AVFoundation or the UVC host driver. This interface change is confined to the
test binary; it is not a change to the released device format.

The reader uses completed host byte counts and a monotonic clock, with a
two-second warm-up. The first measured completion establishes the timestamp
boundary and is excluded from the byte sum, so numerator and denominator cover
the same interval. Runs measured approximately 10-12 seconds after warm-up.

For reads large enough to contain a complete frame, the reader checked frame
length, header and sequence continuity on every complete frame, including
warm-up. It checked the first full pixel payload byte-for-byte. The first rear
raw run checked 804 frame headers/sequences with zero gaps, bad-frame reports
or transfer errors. **It did not compare every pixel of every frame.**

The 64 KiB case only checked transfer status and received byte counts: it did
not reconstruct whole frames across reads. Its `full_frames_validated: 0` is
preserved. It supports a throughput comparison, not an independent whole-frame
integrity claim.

Earlier front-port raw tests also exposed one old-resolution frame during a
mode transition. The later fresh-boot 720p run was valid. The rear native-size
queue sweep avoided that transition ambiguity by keeping the frame size fixed.

## Bottleneck interpretation

For native video, 960 * 544 * 1.5 = 783,360 image bytes. Including one 12-byte
header per frame, 60 FPS needs **47.002320 MB/s**, approximately 4.44% more than
the best rear raw measurement. The existing live pipeline reaches the measured
raw ceiling to within experimental variation. Its conversion/pacing path and
the host's UVC stack are therefore not the limiting additions in these
video-only runs.

Firmware 3.60 was rechecked in IDA. `fun_prepare_endpoint_dma` at `0x19CE094`
programs the queued request's physical address and whole request size into a DMA
descriptor. `fun_hw_configure_endpoint` at `0x19CE370` configures packet size,
direction and type separately. An entire image request is not a software loop
of 512-byte copies. These functions did not provide evidence for a safe,
obvious register change that would exceed the observed rate.

The rear port improved both ordinary capture and the raw transport path. The
remaining difference from the USB ideal is below the tested conversion/UVC
layers, but the measurements do not uniquely separate host-controller
scheduling, device turnaround, firmware/DMA behaviour or memory stalls. A
software completion timestamp is not a USB bus-analyser trace.

For context, high-speed USB bulk permits up to 13 * 512 payload bytes per
125-microsecond microframe, or an ideal **53.248 MB/s**. The measured 45.0 MB/s
is about **84.5%** of that optimistic payload ceiling. The actual USB wire rate
is a different quantity and was not measured. The observed rate is numerically
close to 11 full 512-byte payloads per microframe, but without a bus trace that
is only a consistency observation, not a count of actual transactions.

Primary protocol reference: [Microchip, USB Frames](https://developerhelp.microchip.com/xwiki/bin/view/applications/usb/how-it-works/frame/).
Host transfer API reference: [libusb asynchronous I/O](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html).

## Initial audio/video check and callback-rate caveat

With normal release 1.9 restored, a simultaneous LiveArea video/audio capture
received correct 960x544 dimensions, changing image data and 721,920 stereo PCM
frames at 48 kHz (15.04 seconds). Both channels were non-silent, with peaks of
4,778 and 7,126 in signed-16 units and no clipped samples. This is a numerical
signal check, not a perceptual audio or synchronization assessment.

The audio/video callback timestamps mixed an initial origin near zero with a
later host-uptime origin. The original aggregate FPS is invalid and retained
as such. Grouping the monotonic timestamps produces approximately 59.92 video
callbacks per second, which conflicts with the independently measured
video-only transport rate if interpreted as distinct full-frame transfers.
**That callback figure is not used as evidence of 60 transmitted FPS.**

Two follow-ups did not resolve the discrepancy. An audio-only FFmpeg client
opened the temporary raw device but received no PCM, so the raw-plus-audio
comparison was rejected. An updated Mac helper intended to read the embedded
frame sequence did not reach capture: it hung in `freopen`/`open` while opening
its log. No sequence results were produced. The cause of that host file-open
stall was not established. Those attempts could not distinguish host
repetition/retiming from a transport change. The successful follow-up below
subsequently resolved that question for this test configuration.

The diagnostic runner restored the original Mac tester on disk and the
release-1.9 Vita plugin. The stalled helper (PID 29533, label
`rear-sequences-182012-video-0`) was subsequently terminated after checking its
exact command line, and a follow-up process check found it gone. The restored
Mac application also passed `codesign --verify --deep --strict`. The failed
sequence probe does not affect the completed video-only ceiling comparisons.

## Embedded-sequence follow-up on 2026-09-20

The previously authorized retry completed after FTP and VitaCompanion became
reachable again. A separate copy of the Mac tester ran from its cache directory;
the original application was not replaced. Its preflight captured changing
release-1.9 LiveArea images at approximately 57.45 FPS before the temporary
plugin was installed. `screen on` was acknowledged before every capture.

The temporary static UVC sender increments a little-endian 32-bit value in the
first four luma bytes for each submitted frame. The tester reads that value
and checks the following 28 luma bytes against the fixed 0x55 pattern. It records
monotonic callback-arrival time separately from presentation timestamps. Thus,
repeated callbacks with the same sequence are not counted as new submissions,
and mixed presentation-timestamp origins cannot inflate the rate.

Three approximately 12-second sessions ran in order: video-only, simultaneous
video/audio, then video-only again. Each result excludes its first two seconds.
No sequence values were missing or out of order in any steady window; all
sampled pattern checks passed.

| Session | Steady seconds | Callbacks | Distinct sequences | Repeats | Distinct FPS | Distinct payload MB/s |
|---|---:|---:|---:|---:|---:|---:|
| Video-only | 9.957752 | 573 | 573 | 0 | 57.442684 | 44.998990 |
| Simultaneous video/audio | 10.010432 | 600 | 576 | 24 | 57.440079 | 44.996949 |
| Video-only after A/V | 10.010517 | 576 | 576 | 0 | 57.439591 | 44.996567 |

Rates use distinct sequence transitions divided by the monotonic time between
the first and last retained callback. Payload rates multiply those transitions
by 783,372 bytes, including the frame header. They are not independent USB
wire-byte counters. Raw logs were independently parsed to reproduce every
count and rate rather than trusting the runner's JSON alone.

**The near-60-FPS A/V callback rate is not additional USB throughput.** In the
steady A/V window, 24 of 600 callbacks repeated an already delivered frame;
there were still only about 57.44 distinct submitted frames per second.
The repeats disappeared in the subsequent video-only session. This locates
the apparent increase in the host-delivered callback stream, not a faster
Vita sender; it does not identify the precise macOS component doing retiming.

The sequence A/V session also received 577,536 stereo PCM frames at 48 kHz
(12.032 seconds) and non-silent samples. After the guarded runner restored
release 1.9 and rebooted, an ordinary LiveArea A/V check received changing
960x544 images and 385,536 stereo PCM frames (8.032 seconds), again non-silent.
All five sessions, including preflight and final live capture, reported correct
dimensions and zero dropped host callbacks. Those callback counters do not
exclude every possible device-level loss, and the final live callback rate is
not presented as a distinct-frame measurement.

The follow-up changes no production source, audio implementation, clocks,
format, or buffering policy. Release 1.9 was independently read back afterward,
matching the original 13,877-byte binary byte-for-byte. The taiHEN configuration
and saved PSVshell profile matched their pre-test copies. The original Mac
tester still passed signature verification. These remain short tests, not a
long-run reliability, perceptual A/V synchronization, or game-overhead study.

The runner, original results, per-callback logs, PNGs, and PCM are under
`tests/.build/usb-firmware-review-20260919/`. The run prefix is
`sequence-retry-20260920-221310`; its main `.json` and
`-independent-verification.json` contain the completed restoration and
independently recomputed results. The successful run supersedes the failed
sequence attempts above without removing their historical evidence.

## Final production state and evidence

The successful restore records verify release 1.9 at
`ur0:tai/udcd_uvc.skprx`, SHA-256
`4fd1145f4ef8705775eb3bd184d1666c43938fb125eb81aa6daa61d722343d87`, 13,877 bytes.
The configuration and saved clock profile were unchanged. The temporary raw
interface, static image generator and sequence-checking host build are not
production changes. No new plugin binary or release is justified by this
video-only experiment.

Generated evidence is local and ignored under
`tests/.build/usb-ceiling-20260919/`. Key records:

- `portcheck-180225-result.json`: paired rear live/raw result and restoration.
- `rear-release19-results.json`: initial rear baseline.
- `rear-sweep-180658.json`: queue/read-size/alignment/content sweep and final live capture.
- `rear-final-av-181058-analysis.json`: PCM statistics and explicit timestamp-origin groups.
- `rear-raw-audio-181425.json` and its FFmpeg log: rejected raw-plus-audio test.
- `rear-sequences-182012.json` and `sequence-launch-stack.txt`: failed sequence probe and restoration record.
- Per-run topology, deployment, raw-reader logs and unmodified capture JSON/PNG files.

These short tests do not establish long-run reliability, unchanged game CPU/GPU
cost, unique game-rendering FPS, end-to-end latency, all host compatibility, or
an absolute hardware maximum. They do establish a repeatable approximately
45 MB/s video-only transport result and a useful rear-port improvement without
adding work to the Vita's video pipeline.
