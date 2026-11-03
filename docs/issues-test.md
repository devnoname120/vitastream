# Unreleased issue test build

This candidate addresses the four open issues reviewed on 2026-10-14. It is
based on vitastream 2.0 (`d7836db7a57e606ed7008b8e0c848be8be5da0e5`), includes
the existing memory fix, and is intended for user testing before a release.
The module version remains 2.0; identify this candidate by its SHA-256.

## Changes and evidence

| Report | Candidate change | Evidence and remaining limits |
| --- | --- | --- |
| [#1: launch errors and USB storage recovery](https://github.com/devnoname120/vitastream/issues/1) | Keep large video buffers out of the ordinary kernel-driver pool; retain source memory during conversion; restart UVC after external USB takeover ends and MTP returns. | Host tests cover finite memory pools, source lifetime and USB recovery gates. Madden NFL 13's C2-12828-1 has not been reproduced or diagnosed from a crash dump. It still needs a focused test. |
| [#2: camera crash](https://github.com/devnoname120/vitastream/issues/2) | Validate framebuffer format, pitch, dimensions and arithmetic; retain the process and source memory through IFTU; reject a stale/replaced physical mapping. | Firmware inspection establishes that the framebuffer getter returns a snapshot after unlocking display state. The former converter used the physical address without retaining its backing memory and treated unknown formats as RGB. Host tests cover rejection and reference cleanup. This establishes concrete risks, not the cause of the reported camera crash. The attached clips could not be inspected. |
| [#3: application transitions and reboots](https://github.com/devnoname120/vitastream/issues/3) | Preserve the existing physically contiguous pool allocation fix and single-buffer fallback; add source lifetime protection for process transitions. | Reporters confirmed that the earlier memory candidate improved application switching and reboots. That feedback predates the combined candidate. Memory remains finite, and Madden was still failing for one reporter. |
| [#4: missing sounds](https://github.com/devnoname120/vitastream/issues/4) | Restore the 1.9 firmware-owned SourceMixer1 MAIN monitor backend; keep per-millisecond USB packets and transport stop fences. Check firmware identity and reject another active monitor/Bluetooth owner before changing the route. | Reporters distinguish working 1.9 audio from missing LiveArea, PSP, VitaTube, radio/IPTV and game audio in 2.0. Firmware inspection verifies the path and completion fence. The combined candidate still requires source-by-source listening tests. |

The memory allocator investigation is in [issue-3-memory.md](issue-3-memory.md).
Video buffers never fall back to the ordinary kernel-driver pool. The second
buffer may fall back to serial capture; a failed first allocation skips the
frame. No extra source copy or RGB capture buffer is introduced.

**Speaker/headphone output is muted while the computer captures USB audio.**
Stopping USB audio should restore it. This is the explicit tradeoff of using
the complete, firmware-owned mix again. It is not a fix for local playback
while streaming. The removed secondary hardware tap could not be relied on
to capture every source in the reported 2.0 configurations.

USB storage recovery follows VitaShell's
[USB lifecycle](https://github.com/TheOfficialFloW/VitaShell/blob/master/usb.c):
storage stops MTP, and exiting storage starts MTP again. Recovery checks both
`USB_MTP_Driver` and `USBStor_Driver`, requires an activated controller, drains
audio/video, and checks the driver states again before restarting UVC. It
does not take the controller while storage is active. A failed restart restores
MTP and leaves recovery pending for a later retry.

## Test procedure

Use `vitastream.skprx` first. Replace the existing vitastream plugin using the
same installation entry; load only one streaming plugin. Keep a known working
binary available for rollback. The normal build has no diagnostic file writes.

1. Boot once with USB connected and once disconnected. Open several native
   games, including Madden NFL 13 if available, then test launch while capture
   is already active and launch before opening capture on the computer.
2. With video and audio capture active, switch repeatedly between native games,
   Adrenaline/PSP, LiveArea and homebrew. Check for launch errors, reboots and
   unresponsive controls.
3. Open the camera while capturing in OBS. Test preview, front/back camera
   switching, leaving the camera and opening another application. Note whether
   capture briefly skips a frame or remains frozen after the transition.
4. Listen to LiveArea background music, a PSP title, affected native games,
   VitaTube and web radio/IPTV where available. Record whether each source
   reaches USB audio and whether stopping host audio restores local sound.
5. Start VitaShell USB mass storage, complete a transfer, then stop storage.
   Reopen the host's video/audio capture device after re-enumeration. Confirm
   streaming returns without rebooting. Repeat once and test cable reconnection.

If a failure persists, replace the plugin with `vitastream_diagnostic.skprx`
using the same config entry and repeat the smallest failing sequence. The
diagnostic build writes `ux0:/data/vitastream_diag.txt`; it truncates that file
on plugin startup. Save the log before another reboot overwrites it. Report:

- Exact candidate filename and SHA-256, Vita model, real firmware and spoofed
  version if configured, plus other kernel plugins.
- Application, exact steps, USB connection at boot, host OS and capture app.
- Error code and any crash dump, with whether video, USB audio and local sound
  continued, stopped, or returned afterward.
- Diagnostic log when available; a sudden reboot may lose its last messages.

## Build and verification

`make test` compiles production functions with AddressSanitizer and
UndefinedBehaviorSanitizer. Its simulated dependencies exercise memory-pool
policy, cancellation/drain ownership, pacing, firmware selection, startup and
shutdown cleanup, source references, USB recovery and audio failure paths.
These checks do not run IFTU or the Vita's USB/audio hardware.

The candidate binaries, source snapshot, build logs, `SHA256SUMS`, build
provenance and firmware import/export evidence are kept together under
`builds/issues-test-2026-10-14/`. VitaSDK is pinned to:

```text
vitasdk/vitasdk@sha256:fd82d88ff456d118eef16533ebf9a3a6ae0e292684fb8a3a35f15f0707b4692d
```

Offline firmware checks verify the actual ELF imports, the four runtime
SceAudio monitor exports, and the existing module profiles against available
decrypted firmware. Unavailable kit images remain unverified; the detailed
candidate matrix is `firmware-compatibility.json` alongside the binaries.

No release, tag, issue comment or issue closure is part of this test build.
No device installation, reboot or hardware test was performed for it.
