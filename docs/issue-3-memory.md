# Issue #3: preserve kernel-driver memory during capture

Issue: <https://github.com/devnoname120/vitastream/issues/3>

Baseline: `d7836db7a57e606ed7008b8e0c848be8be5da0e5` (vitastream 2.0).

## Finding and confidence

The reports describe application-launch failures and reboots while streaming,
including a report that launching an application before connecting USB avoids
its launch failure. Memory exhaustion is the reporters' hypothesis, not a
confirmed diagnosis from a crash dump.

The video allocator nevertheless has a concrete resource-policy problem: it
allocates its large framebuffers from the shared kernel-driver physical
partition first, and only tries another partition after that allocation fails.
A successful allocation can leave too little kernel-driver memory for subsequent
system work. Detecting failure of the streaming allocation itself is too late
to preserve that headroom.

The normal capture path reuses its buffers; this investigation did not identify
an accumulating per-frame allocation leak. Double buffering increases resident
memory, but a single buffer also consumes the same kernel-driver partition.

Firmware inspection verifies the partition distinction. A finite-pool host
regression demonstrates the old policy consuming kernel-driver capacity and the
new policy leaving that capacity untouched. Neither constitutes reproduction
of the reported application-launch failure on a Vita. The meaning of the
reported C1-2569-2 error and the cause of the individual reboots were not
established here.

## Change

`src/video.c:allocate_slot` now allocates video buffers directly with
`SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW` (`0x30808006`). It never
falls back to `0x10208006` for video framebuffers.

The existing physical-contiguity attribute, 4096-byte alignment, non-cacheable
mapping, frame layout, conversion path, USB request ownership, and cleanup
rules are unchanged. The new policy uses the same allocation type that was
already present as the fallback in version 2.0; no new import or firmware hook
is introduced.

If only one buffer fits, the existing serial-capture fallback remains active.
If the first allocation fails, that frame is skipped and subsequent capture
attempts can retry. A buffer in an outstanding USB request remains allocated
until completion, including after cancellation.

| Mode | One allocated video buffer | Two buffers |
| --- | ---: | ---: |
| 960x544 NV12 | 768 KiB | 1536 KiB |
| 1280x720 NV12 | 1352 KiB | 2704 KiB |

These sizes include the 16-byte prefix and page rounding. They are not the
plugin's total memory footprint. Audio buffers, thread stacks, and other
kernel resources are unchanged.

Using a single-buffer default instead would reduce consumption but would not
remove the video allocator's use of kernel-driver memory. Both buffering modes
failed the old-policy regression. The retained fix preserves overlap whenever
the physical-contiguous partition can accommodate both buffers.

The physical-contiguous partition is still a finite shared resource, not
additional RAM. This change does not guarantee that every application or plugin
combination has sufficient memory, and does not reserve headroom for every
other consumer of that partition.

## Firmware evidence

The following addresses refer to the supplied enriched retail 3.60 databases,
not portable live-kernel addresses:

- `/Volumes/data/Downloads/vitadump 3.60 KERNEL/SceSysmem.enriched.elf.i64`
- `/Volumes/data/Downloads/vitadump 3.60 KERNEL/SceProcessmgr.enriched.elf.i64`

The local Henkaku wiki dump supplied the partition/type terminology; its
relevant claims were checked against these firmware implementations in IDA.
The attempted debug-rich 4.00 sysmem database failed to open, so the independent
firmware checks recorded here are for 3.60, not all supported revisions.

| Location | Observation |
| --- | --- |
| Sysmem type table, `0xAE9574` | `0x10208006` resolves to default attributes `0x30608006`. |
| Sysmem type table, `0xAE96B4` | `0x30808006` retains default attributes `0x30808006`. |
| Sysmem `sub_AC9C50`, `0xAC9C50` | These types resolve the kernel process's address space. |
| Sysmem `sub_ACCB5C`, `0xACCB5C` | `(defaultAttr >> 20) & 0xF` selects physical resource 6 versus 8. |
| Processmgr `fun_SetProcessAddressSpacePhyMemParts`, `0xAF8078` | Resource 6 is populated with the shared kernel physical partition handle. |
| Processmgr `fun_InitProcessMemoryPartitionsAndBudgets`, `0xAF818C` | Creates `ScePhyMemPartPhyCont` with 27262976 bytes (26 MiB). |
| Processmgr `fun_SetType1000000ExtraPhyMemParts`, `0xAF7634` | Installs that partition at resource 8, including for the kernel address space during initialization. |

Thus this changes the physical allocation source, not merely a cacheability
flag or the virtual address of the buffer. The 26 MiB figure is the partition
size in the inspected initialization path, not a measurement of available
memory on a running device.

## Verification

Before the production edit, the new `test_capture_preserves_kernel_memory`
regression failed in both buffering modes at the assertion that kernel capacity
remain unchanged. The test models separate finite pools; its 1 MiB and 4 MiB
capacity values are synthetic, not measured device free-memory values.

After the edit, a fresh build and run passed:

```sh
make -C tests BUILD_DIR=.build/issue3-final-host test
```

This ran 17 video test groups in each buffering mode with AddressSanitizer and
UndefinedBehaviorSanitizer, plus the existing shutdown, pacing, firmware-profile,
and startup tests. New coverage checks native and 720p allocation, exhaustion
without kernel-pool fallback, one-buffer serialization, allocation errors,
cleanup accounting, and recovery when capacity becomes available again.

All twelve GCC 15.2 target configurations built successfully: normal, OLED-off,
LCD-off, diagnostic, debug, and debug plus diagnostic, each with default double
buffering and `PARALLEL=0`. Their logs contain no compiler warning or error
messages. Build sources were copied into isolated directories; no device was
modified. Local evidence is under:

`tests/.build/issue3-crossbuild-tow7mihh/`

The normal double-buffer candidate is `normal-double/vitastream.skprx` beneath
that directory, with SHA-256:

`16718b4c0866aba30c4ff6f2ac6595f1fb04d51e091626daa51beb84918d0e26`

## Remaining device validation

The candidate has not been deployed or tested for application switching on
hardware. A useful comparison retains the same plugin set and streaming mode,
then repeats native-game, PSP-game, and homebrew launch/exit/switch sequences
with version 2.0 and the candidate. Native resolution and 720p should both be
included, as well as reconnect and stop/start capture checks. Any continuing
failure needs its error and available crash/log evidence preserved rather than
being assumed to have the same cause.

No new performance measurements were taken. The historical LiveArea
measurements in `performance-2026-09-19.md` do not validate application launching
or the current allocation policy. The separate system/PSP audio regression
mentioned in the issue comments is not addressed by this patch.
