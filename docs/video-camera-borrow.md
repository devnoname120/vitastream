# Camera DRAM video buffering

Normal builds include this backend; `make DIAGNOSTIC=1` adds diagnostics. It keeps
one owned kernel video buffer and borrows SceCamera's preallocated uncached
DRAM for the optional second buffer. It does not allocate a second framebuffer
from the game's or shared PhyCont budgets and performs no full-frame copy.

The `lowmemory=off` configuration default enables the second buffer.
`lowmemory=on` retains only the first buffer and skips camera borrowing,
recovery attempts and hook installation. Existing configuration files without
this key use the default. Reboot after changing it. The published v2.3 binaries
predate this backend.

Native layout access is pinned to retail 3.60 SceCamera NID `EC26CAF9` and SceSysmem
NID `3380B323`, with segment bounds and instruction checks. Unknown layouts or
unavailable backing retain ordinary single-buffer capture.

The borrowing thread holds the camera's existing control mutex while IFTU or
USB can access its memory. Both camera clocks, both active-channel fields,
suspend fields and worker read activity must be idle. The backing UID, mapped
size, cache type and every physical page are validated before first use; a
cached validation is reused only for that same UID and backing generation.

SceCamera's control-mutex import revokes borrowing before a native camera
request proceeds. The camera free import fences backing deletion, and its
system-event callback fences suspend/resume notifications. Revocation cancels
pending UVC transfers and wakes the capture thread, which returns the camera
mutex after DMA completion. Hooks do not unlock a mutex owned by another
thread, and a timeout does not return ownership of an in-flight DMA buffer.
The post-enqueue check closes cancellation versus submission races.

After revocation, capture uses the retained first buffer and retries borrowing
once per second. This covers any app or game using SceCamera, rather than just
the system Camera app. Pending native requests take priority over retries, and
the native control mutex plus the complete idle-state checks must allow a new
loan. A changed UID, camera free, or system event invalidates the backing
validation cache, including when a recreated allocation reuses its UID. A
revocation generation prevents a request arriving during validation from being
lost when a new lease is published. Recovery keeps the original owned buffer
and requires no USB restart or reboot.

Resident camera hooks prevent live plugin unload; activate updates by reboot,
as with the existing audio route guard. A permanently stalled cancellation cannot
safely permit freeing memory still owned by DMA; this remains a failure case
to investigate on hardware, not a reason to release active backing.

`make test` includes camera-buffer overlap, stalled-transfer retention,
pre-submit and enqueue-race revocation, conversion failure, resolution change,
firmware/layout rejection, partial hook installation and concurrent handoff
coverage with ASan/UBSan. Recovery tests cover initial unavailability, repeated
camera use, waiting native callers, recreated backing with the same or a new
UID, and free racing first validation. The native backend fixture also runs
200 cross-thread handoffs and supports a focused ThreadSanitizer run. Host
tests do not establish physical camera/USB behavior.

Test receipts and immutable build provenance are kept in the local private
`builds/video-camera-dram-2026-11-05/` artifact directory and corresponding
`.git/video-camera-dram-2026-11-05/` investigation directory.

## Retail verification, 5 November 2026

These receipts describe the original handoff-only candidate. The automatic
recovery update and its separate receipts are in
`builds/video-camera-recovery-2026-11-06/`.

The final private candidate was installed on the retail 3.60 Vita and read back
byte-for-byte. Its SHA-256 is
`662ab0ec4004f3b977f036eacb4a95b7e84171d859a1e57ed782eafde8150e42`.

- The device log confirms validation of the 3,686,400-byte camera allocation,
  two video buffers, and submission of the borrowed buffer to USB.
- Opening the real Camera tile while capture ran returned the borrowed DRAM
  and reduced capture to one buffer. Camera preview remained live; a short
  preview capture contained changing sensor frames. The 20-second capture
  spanning takeover delivered 1,003 frames.
- A final reboot reset the conservative guard. A native 960x544 capture then
  delivered 222 frames over 3.9376 seconds. These are functional samples, not
  controlled performance benchmarks.
- Final LiveArea audio contained non-silent 48 kHz stereo PCM with no host
  callback gaps or full-scale samples. The device configuration and taiHEN
  configuration remained byte-identical; no new Vita coredump appeared.
- Retail OBS inputs were restored using the freshly verified retail USB
  identity. The devkit was not controlled or opened by the capture tools.

Use **960x544** for this candidate. A 1280x720 selection still needs 2 MiB of
physical backing for the first owned kernel buffer, which did not fit on this
setup. Borrowing the second buffer does not remove that first-buffer limit.
The native 960x544 NV12 image is 783,360 bytes; with metadata and page alignment
the mapped allocation is 786,432 bytes, with 1 MiB physical backing under the
current PHYCONT rounding policy. At 1280x720, the image is 1,382,400 bytes and
the mapped allocation is 1,384,448 bytes, rounded to 2 MiB physical backing.
This is the host-requested USB image size; it does not change the Vita panel's
960x544 resolution or add source detail when scaling a native framebuffer.

Photos' `C2-12570-5` in this test came from the title-ID launch route. Its
firmware metadata advertises `photo:` and `camera:`; opening the **Camera tile
in Photos' LiveArea** launched it successfully. No Photos crash dump was
produced by that unsupported launch attempt.

## Automatic recovery verification, 6 November 2026

The recovery candidate SHA-256 is
`19a09f448c20e5d51a52a78eeab56ac8176b9d0a29216e9cfaad7914dc948ecb`.
It was built from an immutable source archive in a clean SDK container,
installed on the retail 3.60 Vita with a backup, and read back byte-for-byte.

Two real Camera open/close cycles were tested with the existing OBS video and
audio inputs active. In each cycle, the camera preview worked, capture used
one buffer while the camera was active, and the device log confirmed recovery
to two buffers after `quit NPXS10004`. Neither a reboot nor a USB/source
restart was used between these phases. A separate direct AVFoundation capture
helper could not obtain the device configuration lock while OBS was active;
there is no new frame-rate benchmark from this pass.

The full ASan/UBSan host suite, focused native backend TSan run with 200
cross-thread handoffs, clean experimental target build, and clean default
single-buffer target build passed. The tested source manifest still matched
the working tree at final verification. Both configuration files were
unchanged, no new files appeared in the Vita's coredump directory, and OBS
settings and monitoring remained as before the test. Physical takeover was
tested with Sony Camera; coverage of other apps and games follows from the
shared driver hook, rather than separate per-title hardware tests.
