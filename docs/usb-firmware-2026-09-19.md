# USB firmware investigation after the rear-port ceiling test

Date: 2026-09-19 (Europe/Paris).

Production baseline: release 1.9, code commit `1cf3272`. The documentation-only
commit `8579a37` records the preceding ceiling experiments. This investigation
adds no production driver patch, codec, video thread, clock change, or audio
implementation change.

## Result

The additional **Vita-side DMA request-size sweep** completed. Its best result
was **45.030638 MB/s**, using 1,382,412-byte requests. Native-sized requests
measured **45.001656 MB/s** and **44.982899 MB/s** in the bracketing runs.
The larger request improves on the first native run by only about 0.06%.
These are decimal MB/s of bytes delivered to the host, including the 12-byte
payload header, not raw signalling bandwidth.

Together with the earlier static-image, alignment, host-queue, and raw-USB
controls, this does not support a per-frame software optimization capable of
recovering the roughly 4.4% needed for native 60 FPS on this connection. It
also does **not** prove that Sony's controller setup is optimal or that the
Vita has an immutable 45 MB/s hardware limit.

The remaining unresolved performance mechanisms are inside a request:
controller FIFO refill/transaction turnaround, memory-bus arbitration, and
host microframe scheduling. No register change with established semantics
and a demonstrated benefit was found, so none was installed.

## 1. Device-side request-size experiment

The earlier host-read-size sweep changed the size of reads submitted by the
Mac, not the length of each DMA request submitted by the Vita. This follow-up
varied the latter using the already built temporary raw benchmark.

Conditions: same rear root-controller path; static 0x55 pixels; no IFTU
conversion or vblank pacing; one outstanding Vita request; eight 2 MiB host
reads; two seconds of warm-up followed by approximately eight measured
seconds. Each size started after a fresh reboot. The sequence was native,
small, 720p, then native again. The taiHEN configuration and saved clock
profile were checked, not changed. `screen on` was sent through VitaCompanion.

| Order | Vita request bytes | Received MB/s | Seconds per completed request | Frame/sequence/transfer errors |
|---|---:|---:|---:|---:|
| Native, first | 783,372 | 45.001656 | 0.017407626 | 0 |
| Small | 195,852 | 44.794430 | 0.004372240 | 0 |
| 720p-sized | 1,382,412 | 45.030638 | 0.030699365 | 0 |
| Native, repeat | 783,372 | 44.982899 | 0.017414885 | 0 |

The host checked the length, header, and embedded sequence of complete
frames. It fully checked the first payload against the fixed pattern; it did
not byte-compare every later image payload. The numbers are not based on the
firmware's `transmitted` field.

Evidence:
`tests/.build/usb-firmware-review-20260919/device-request-sweep-20260919-194353.json`
and the four correspondingly named raw-reader logs.

### Fixed-boundary-cost estimate

An ordinary least-squares fit of the four observations to

```
time_per_request = fixed_cost + request_bytes / asymptotic_rate
```

gives approximately **28.37 microseconds** of fixed cost and
**45.0699 MB/s** for the fitted asymptotic rate. This is an empirical model,
not a separate hardware measurement or a rigorous upper bound. Request
boundaries, short packets, host scheduling, and measurement noise all
contribute to the observations.

Nevertheless, the scale is informative: the fitted fixed cost is about
0.16% of a native request period. Removing it entirely would not account for
the roughly 0.74 ms reduction needed to fit a native transfer into a nominal
16.667 ms period. Enlarging or chaining whole-frame requests is therefore a
poor candidate for the missing 4.4% on the evidence available.

## 2. What the firmware actually does

The supplied firmware 3.60 modules were inspected through IDA MCP. Addresses
below refer to those enriched ELF databases, not fixed live kernel addresses
suitable for blindly applying a patch.

### Request submission and DMA ownership

`SceUdcd` keeps separate software request queues per physical endpoint.
`fun_validate_request_chain` at `0x19C8CB4` checks the request chain and obtains
physical addresses. The plugin already uses physically contiguous buffers;
it does not require a CPU copy of each USB packet.

`fun_initialize_endpoint_dma_state` at `0x19CEA94` allocates a 4 KiB
`SceUdcdDmaTag` block and reserves tag-storage areas for endpoints. The
512-byte stride in that tag allocation is descriptor storage: it is not a
loop transmitting only 512 bytes of an image at a time.

`fun_prepare_endpoint_dma` at `0x19CE094` prepares a descriptor containing a
command, physical address, the **whole request length**, and a terminating
link value. Ordinary bulk IN uses command value 44. The path can append a
zero-length descriptor for the particular request option that requests
termination after an exact packet multiple. The plugin's ordinary video
requests do not use that option and already finish with a short packet.

`fun_hw_kick_send_request` at `0x19CFEBC` programs the selected endpoint's DMA
state and starts the request. The data path is distinct from the explicit
CPU-driven EP0 control-transfer loop in the same function. Treating that EP0
loop as video pixel-copy overhead would be a decompilation-reading error.

### Completion ordering

`fun_complete_endpoint_request` at `0x19CA16C` removes the completed software
queue head and can start the next queued request **before** invoking the
previous request's client callback. The relevant start-next call is near
`0x19CA240`, and the callback is near `0x19CA1D8`; control-flow order matters
more than numerical address order.

An existing comment in the enriched database described the opposite order.
The conclusion here follows the function's control flow, not that comment.

Thus, a second software request is possible without inventing a new USB
protocol. It is not equivalent to a hardware descriptor chain spanning all
frames, and it does not eliminate transaction timing within one active
request. The earlier approximately 19-microsecond re-arm measurement and the
new size sweep both argue against expecting a material gain from deeper
whole-frame queues.

Ordinary IN completion can report the requested length in `transmitted`.
It must not be treated as an independent bus-byte counter. Host-received
lengths were used for these experiments instead.

### Endpoint/FIFO configuration

`fun_hw_configure_endpoint` at `0x19CE370` derives direction, endpoint type,
and packet size from the endpoint descriptor. It programs endpoint-specific
configuration and DMA registers. In the analyzed layout, endpoint packet and
configuration words are at offsets `0x230 + 0x10 * ep` and
`0x234 + 0x10 * ep` from the controller mapping; the endpoint DMA register
banks have a separate stride.

The firmware selects high configuration bits `0xC000` or `0x4000` based on
endpoint number and packet-size thresholds. A normal 512-byte bulk endpoint
already follows the `0xC000` branch. The observed conditional selection is
verified. The exact physical FIFO-depth/banking interpretation of those bits
is **not** established by that fact alone.

Changing them merely because they resemble another vendor's double-buffer
bits would be speculative. In particular, nothing here establishes that
Sony accidentally left a known higher-throughput FIFO mode disabled.

### Clock/reset helpers are not a discovered frequency control

Controller initialization calls
`ScePervasiveForDriver_A2EFD7AF(bus, 10)` and
`ScePervasiveForDriver_13CC07C9(bus, 10)`.

In `SceLowio`, `ScePervasiveForDriver_A2EFD7AF` calls
`fun_clock_gate_reg_set_bits` at `0xC08A6C`; that helper ORs a **bit mask** into
the selected register under the Pervasive spinlock. The other wrapper calls
`sub_C084AC`, which forwards to `fun_reset_reg_clear_bits`. The argument 10
is not a MHz value or an identified divider.

`fun_ActivateUdcdBus2` at `0x19CE554`,
`fun_initialize_endpoint_dma_state`, and `sub_19CE71C` use these helpers during
controller activation. These inspected paths do not establish a hidden USB
frequency knob. This is a statement about the traced calls, not proof that
no other clock or bus-arbitration setting exists anywhere in the SoC.

## 3. Comparison with public controller implementations

Local comparison references include Sony CXD56/NuttX, Socionext F_USB20HDC,
Renesas USBF, and the Goku, GR, and Net2280 device-controller layouts. Their
register offsets, endpoint organization, or DMA formats did not establish a
matching Vita controller programming model.

For example, the CXD56 reference has direction-separated endpoint blocks
with 0x20 strides and global device registers around 0x400. That is not enough
to assign its named burst/threshold fields to the Vita's differently arranged
controller registers. Shared Sony branding is not evidence of identical IP.

The compared files are retained in
`tests/.build/usb-firmware-review-20260919/reference/`. No values were copied
from those drivers into a live Vita controller.

## 4. A useful microframe hypothesis, not a packet-trace result

High-speed USB uses 125-microsecond microframes, and bulk packets contain at
most 512 payload bytes. The usual ideal budget is up to thirteen full-size
bulk transactions per microframe. See Microchip's primary technical overview:
<https://developerhelp.microchip.com/xwiki/bin/view/applications/usb/speeds-specs/high-speed/>.

Calculated payload rates at constant full-size transaction counts are:

| Full-size transactions per microframe | Payload MB/s |
|---|---:|
| 10 | 40.960 |
| 11 | 45.056 |
| 12 | 49.152 |
| 13 | 53.248 |

The previous front-port plateau near 40.9 MB/s and the rear-port plateau near
45.0 MB/s are close to the ten- and eleven-transaction values. Packet
termination and occasional boundary gaps would lower the observed average.
This is **consistent with** a transaction-packing or turnaround limit, but no
wire capture counted ten or eleven transactions in individual microframes.
The numerical agreement is a hypothesis, not a decoded controller setting.

Native 960x544 NV12 at 60 FPS needs 47.0016 MB/s of image data, or
47.00232 MB/s including one 12-byte header per image. That is roughly 11.48
full-packet equivalents per microframe. A successful low-level optimization
would therefore need enough additional bus service to exceed the apparent
eleven-transaction plateau, rather than just saving a few CPU instructions
at a frame boundary.

The header itself is about 0.0015% of the native image payload. Removing most
of it cannot supply 4.4%. Making the final transfer exactly packet-aligned
also requires valid UVC end-of-payload handling; silently appending padding is
not a free optimization. Larger-than-512-byte bulk packets would violate the
ordinary USB 2.0 high-speed bulk endpoint constraints, regardless of whether
the Vita's descriptor validator were hooked.

## 5. Remaining discriminators and rejected changes

The useful next evidence would distinguish FIFO/NAK/turnaround stalls while
an active request is in progress from unused time imposed by the host.
A verified controller counter or a USB packet trace could provide that
separation. Aggregate request-completion timing alone cannot.

A controller experiment should preserve the standard 512-byte bulk endpoint,
change only a field with established semantics, record actual received data,
and retain the current rollback. A register dump should avoid undocumented
read-sensitive FIFO/data registers. No such verified throughput-control
field was identified in this investigation, so no speculative register sweep
was performed.

Rejected as fixes for the present shortfall: larger host read buffers, deeper
host queues beyond an adequate queue, better payload alignment, eliminating
IFTU conversion, oversized USB packets, a larger whole-frame DMA request,
additional capture threads, and blind clock/reset changes. These are either
already covered by negative controls, too small by measurement, incompatible
with the intended USB protocol, or unsupported by decoded semantics.

There remains an A/V-specific measurement question: a Mac capture session
with audio can report nearly 60 video callbacks/s. The earlier evidence does
not establish that those are distinct submitted USB frames. The sequence
checker is the appropriate discriminator; callback timestamps alone are not.

## 6. Final state and explicit limits

The request-sweep runner restored release 1.9 and verified the reboot before
attempting its final live-capture check. That Mac helper then timed out; this
is not recorded as a fresh successful live capture. The raw request-size
measurements had already completed and been saved.

A further independent FTP readback in this continuation confirmed the
13,877-byte release-1.9 binary at `ur0:tai/udcd_uvc.skprx`, SHA-256
`4fd1145f4ef8705775eb3bd184d1666c43938fb125eb81aa6daa61d722343d87`.
The taiHEN configuration and saved PSVshell profile matched their saved
copies. The timed-out capture helper (PID 72869, with the exact request-sweep
label) was terminated; no unrelated application was signalled.

A sequence-check retry was prepared to use a separate Mac test-app copy and
cache-directory output, leaving the original tester untouched. Its launch
was blocked by the tool layer before execution. It did not install a plugin
or produce another measurement. The script is retained as
`tests/.build/usb-firmware-review-20260919/resume_sequence_probe.py`.
No attempt was made to bypass the blocked launch or alter macOS permission
databases.

The observed ceiling is approximately **45 MB/s under the tested video-only
conditions**. Real native capture previously reached approximately 57.45 FPS
on the same rear-port path, and steady 720p capture reached approximately
29.97 FPS. No new production optimization or 60-distinct-FPS claim follows
from this investigation. Unknown FIFO/turnaround semantics, the A/V callback
ambiguity, different host controllers, and long-run/game-impact testing are
not silently treated as solved.
