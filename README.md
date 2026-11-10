# Issue #9 diagnostic candidate — 2026-11-10

[Download the issue #9 diagnostic ZIP](vitastream-issue-9-diagnostic-2026-11-10.zip).
This is instrumentation, not a claimed fix. It retains the failing `70c77f5`
capture baseline, adds precise negotiation/error-stage diagnostics and retains
the previous boot log. It excludes the issue #10 timing change and experimental
issue #11 suspend handler. Complete source, patch, checksums and validation
receipts are included. [ZIP checksum](SHA256SUMS-issue-9-2026-11-10).

---

# Current test candidates — 2026-11-09

These are test builds, not releases. Install only one SKPRX and keep your existing configuration.

| Package | Source | Purpose |
| --- | --- | --- |
| [Current main](vitastream-main-test-2026-11-09.zip) | `70c77f5` | Corrected VitaShell-only handoff and audio startup settling; baseline for issues #8, #9 and #11. No experimental suspend handler. |
| [Issue #10 Adrenaline tearing](vitastream-issue-10-test-2026-11-09.zip) | `51d66bf` | Current main plus the separate compatibility-framebuffer capture change. Hardware confirmation pending. |

Both packages include normal/diagnostic binaries, complete corresponding source, instructions, licence, checksums and build/firmware receipts. See [SHA256SUMS-2026-11-09](SHA256SUMS-2026-11-09).

The historical issue #4 package is retained below.

---

# vitastream test builds

This branch stores test artifacts only. Development remains on `main`.
No release or release tag is created for these builds.

Download: [vitastream-issue-4-test-2026-11-01.zip](vitastream-issue-4-test-2026-11-01.zip)

ZIP SHA-256: `f22444e5420a19159a59cab3f6b0713481e86bca85a7c102cf388c159890a011`.

# Issue #4: WiliWili crash and missing audio test

Report: <https://github.com/devnoname120/vitastream/issues/4#issuecomment-6560322110>.
Investigation performed statically on 2026-11-01. No Vita was installed,
rebooted or exercised. The reporter explicitly used release 2.0; this is a new
test candidate, not a released version or a confirmed hardware fix.

## What the supplied dump establishes

The attached WiliWili dump has SHA-256
`13755373d28f9c78e9a692a57f3f3d4efe5918ff051a925f727b5acc858d8d39`.
It reports firmware 3.65, WiliWili 1.60, module NID `0xB95DE7C4`, and these
matching executable segment sizes: text `0xDE20C0`, data memory `0x1D9E9C`.
The ELF extracted from the official WiliWili v1.6.0 PSV VPK matches that module
identity and geometry. Its code was inspected with IDA, together with the
locally supplied firmware 3.65 SceLibKernel. Analysis copies use ordinary ELF
headers for loading into IDA; executable segment bytes are not patched.

The crashed thread has PC `0xE00079C6`, LR `0x8128965B`, r0 `0`, r1 `1`,
r2 `0xC10`, DFAR `0x1BC`, and DFSR `0xC7`. PC resolves to SceLibKernel text
offset `0x49B6`, inside `sceClibMspaceCalloc` (export NID `0x678374AD`).
That instruction reads the heap flags at mspace + `0x1BC`. The heap argument
is null. This is a CPU null-pointer read in the application's allocator.

WiliWili's wrapper at text offset `0x28962C` initializes a global heap lazily,
then calls `sceClibMspaceCalloc` without checking whether initialization
succeeded. Its initializer at offset `0x289594` allocates `curl_mempool` using
type `0x0C80D060` (`USER_MAIN_PHYCONT_RW`), size `0x01A00000` (26 MiB).
Failure cleans up and returns 1; the wrapper ignores that result. The next
stack candidate is consistent with libcurl's first easy-handle allocation of
one 3088-byte object. Stack candidates alone are not an unwound backtrace.

The captured TTY independently records the same 26 MiB request failing from
`ScePhyMemPartPhyCont`, with `0x01600000` (22 MiB) available. It also records
an earlier failed 4 MiB allocation for the Madden title `PCSE00084`, when that
pool reports zero available. There is no Madden dump here, so its crash PC,
caller and handling of that failure remain unknown. No dump establishes the
cause of the later unresponsive left stick.

This is evidence of a specific failed allocation and an unchecked application
heap, not evidence that a particular plugin owns the unavailable 4 MiB. The
config screenshot and dump boot log show other kernel plugins, but their
allocation sizes and ownership cannot be established from this user-process
dump. The earlier suggestion to blame the plugin count was too broad.

## Why a separate allocation test is needed

The prior memory fix protects ordinary kernel-driver memory by moving video
buffers into the shared 26 MiB PhyCont pool. An application requesting the
entire pool cannot coexist with any capture allocation in it. Keeping that
policy alone cannot resolve this WiliWili failure.

The opt-in `VIDEO_KERNEL_POOL=1` build allocates exactly one physically
contiguous, non-cacheable video buffer from `KERNEL_ROOT_NC_RW` (`0x10208006`).
It does not fall back to PhyCont when kernel allocation fails. Its maximum
resident video allocation is 1352 KiB at 1280x720, rather than two buffers.
A failed allocation skips capture instead of taking application PhyCont pages.
The existing request completion/cancellation ownership rules remain in effect.

This comparison trades some kernel-driver headroom for leaving PhyCont wholly
available to applications. It is not the default allocation policy. It cannot
remove allocations made by other components, and single buffering may reduce
capture performance. The intended test is whether launch behavior changes
when vitastream contributes no PhyCont allocation.

Both binaries in this test bundle use that policy. They also include the
existing source-lifetime and USB recovery fixes and the restored 1.9-style
firmware-owned audio monitor backend. The latter targets the missing sounds
in Netstream, VitaTube, VitaIPTV and VitaMediaDeck; it mutes local
speakers/headphones while USB audio is captured. This test does not attempt to
restore simultaneous local playback.

## Build and validation

```sh
make clean DEBUG=1 DIAGNOSTIC=1
make VIDEO_KERNEL_POOL=1
make clean DEBUG=1 DIAGNOSTIC=1
make VIDEO_KERNEL_POOL=1 DIAGNOSTIC=1
make test
```

VitaSDK is pinned to
`vitasdk/vitasdk@sha256:fd82d88ff456d118eef16533ebf9a3a6ae0e292684fb8a3a35f15f0707b4692d`.
The source snapshot, logs, firmware import/export matrix, checksums and build
provenance are in `builds/issue-4-2026-11-01/`.

The sanitizer-backed transport suite runs both default buffer counts and the
single-buffer kernel-pool option. A finite-pool regression demonstrates that
a whole 26 MiB application allocation conflicts with the default capture pool
and remains available under the opt-in policy. Other tests check the 1352 KiB
bound, allocation failure without pool fallback, capture ownership and cleanup.
Host models do not establish actual device free memory or crash resolution.

## Tester procedure

1. Back up the current plugin. Replace the existing `vitastream.skprx` at the
   same configured path and reboot; do not add another kernel-plugin entry.
2. Keep the current plugin configuration and 1280x720 at 30 fps for the first
   comparison. Start USB capture before launching WiliWili and Madden NFL 13.
3. Record the two launch outcomes separately, including any error code, reboot
   or unresponsive controls. Check the left stick in VitaShell after an error.
4. Test USB audio in each affected application separately: Netstream, VitaTube,
   VitaIPTV and VitaMediaDeck. Also check LiveArea music. Stopping host audio
   capture should restore local sound.
5. If WiliWili still fails, test once without opening host capture. If it still
   fails then, the same pool can be exhausted independently of active capture.
   A comparison after temporarily disabling only optional plugins can then
   isolate other consumers; keep storage/mount plugins required for boot.
6. If needed, use `vitastream_diagnostic.skprx` in place of the normal binary
   at the same config path and repeat only the failing sequence. Save
   `ux0:/data/vitastream_diag.txt` before rebooting again; the file is truncated
   on plugin startup. Provide a new crash dump and say which test binary was
   installed. The diagnostic build has the same memory and audio policy.

Rollback: restore the backed-up plugin and reboot. Report the binary SHA-256
from the bundle, exact steps, firmware/model, capture resolution and host app.
