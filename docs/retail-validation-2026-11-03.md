# Retail capture verification — 2026-11-03

This bounded hardware check used a retail Vita on the official 3.60 profile.
It does not establish compatibility or audio coverage on other hardware,
firmware, applications or port types.

## Attribution and initial failure

USB inventories taken before and after the user's connection identified the
retail video UID as `0x3122300054c1337`. All capture probes used that exact UID
or its audio UID, `AppleUSBAudioEngine:SONY:vitastream:vitastream:3`. No command
was sent to the devkit.

OBS initially selected another Vita for both video and audio. A dedicated
`Retail PS Vita` scene was configured with the retail inputs. Audio monitoring
was initially off and was changed to Monitor and Output for the retail audio
source only.

The retail plugin's full readback matched the published 2.0 artifact, SHA-256
`1ad98823c0f8b527c0797d64a62caa886f3c0101acbef3f04df10d5bdd90ca87`.
Direct USB audio captures contained only zero samples, including a five-second
capture with 480,256 float samples. This reproduced silent USB output while
the user could hear local playback; it was not solely an OBS monitoring issue.

## Applied candidate and results

The initial installed candidate was the diagnostic build from the 2026-11-01 issue test,
using `VIDEO_KERNEL_POOL=1 DIAGNOSTIC=1`. Its source files and Makefile match
the source being committed. It restores the firmware-owned MAIN monitor path
and uses one bounded video buffer outside the application PhyCont pool.

Installed SHA-256, verified by full readback before and after reboot:

```text
640dd3da819883eb25eb5c9a3ec7067094657bd65591be14a107f13ff6165081
```

The previous module remains backed up on the retail device as
`ur0:/tai/vitastream.retail-20261103-103840-backup.skprx`. The taiHEN config was
not changed. Startup diagnostics selected SceUdcd `0x8E26B2A3`, SceAudio
`0x0DA7714A` and SceKernelDmacMgr `0xF926C804`, with successful compatibility,
hook, worker, USB activation and initial monitor-read results.

Simultaneous direct video/audio capture after activation produced:

- 225 video frames at 960×544 over approximately 5.14 wall-clock seconds;
  224 successive sampled image hashes changed.
- 240,128 stereo audio frames at 48 kHz, containing 479,393 nonzero samples
  out of 480,256 float samples. Peak amplitude was approximately 0.2585 and
  RMS approximately 0.02489. The host probe reported no callback gaps.
- The user confirmed that sound worked in OBS after the change.

These results establish updating video, nonzero USB audio and the user's
audible confirmation for the observed source. They do not prove a sustained
frame rate, long-term synchronization, every audio route, Camera or Madden
behavior. That candidate's firmware monitor muted local speakers/headphones
while USB audio capture was active. The later candidate below adds forwarding
to the local output.

`make test` passed under AddressSanitizer and UndefinedBehaviorSanitizer before
the commit. Existing offline firmware/import evidence remains separate from
this single-device result. Local raw receipts and captures are retained under
`.git/retail-investigation-2026-11-03/` and are not included in the source tree.

## Simultaneous local playback and streaming controls

The later candidate, tested before the version 2.1 release, adds `screen=on/off` and `speakers=on/off`
in `ur0:/tai/vitastream.cfg`, read at startup. Without a file, defaults remain
screen on and local forwarding off. The supplied configuration enables both.

The native MAIN monitor still captures the complete mix. A separately allocated
DMA operation forwards the captured PCM from a private 4 KiB buffer to the
selected local I2S sink. Each 512-frame block adds one 2 KiB copy, approximately
192 kB/s at 48 kHz stereo. It performs no mixing or resampling. The worker
fences its own operation before restoring the native playback route.

Retail 3.60 tests established the following:

- With `screen=on` and `speakers=on`, a five-second recording contained 240,128
  stereo frames, 477,776 nonzero float samples out of 480,256, and no host
  callback gaps. The user confirmed audible sound from the Vita itself while
  OBS monitoring was disabled. The local DMA completed 25,268 blocks before
  a later controlled stop, without a reported DMA failure.
- With both settings off, USB video produced 180 updating 960×544 frames and
  USB audio contained 192,000 stereo frames with nonzero PCM and no host gaps.
  The user confirmed the Vita's screen was dark and its local output silent.
- With `screen=off` and `speakers=on`, the final OLED fix produced 175 updating
  960×544 video frames over approximately 4.12 seconds, and 192,000 stereo
  audio frames. Of 384,000 float samples, 383,698 were nonzero; peak amplitude
  was approximately 0.24783, RMS 0.02174, with no host callback gaps. Diagnostics
  confirmed the local playback DMA completed successfully.

The first OLED implementation called DisplayOn before restoring brightness.
Firmware analysis showed that SetBrightness already queues OLED power-on when
restoring a nonzero value, so the extra DisplayOn left the command queue busy.
The fix restores brightness directly and retries a pending command on the
worker. Stopping capture with the fixed build logged a successful local display
restore. The user also confirmed that the Vita's screen came back on after
capture was paused. A transient busy result also occurred when video activity briefly
resumed during host input teardown; the persistent restore failure was absent.

The first dual-output prototype also failed USB audio startup. Moving relay
preparation onto the capture worker and retaining the working CPU affinity
resolved startup in the tested revision; these changes were tested together,
so the evidence does not isolate either change as the sole cause.

An immediate stop/reopen attempt returned `0x80027101` once. A subsequent host
retry succeeded with nonzero audio. Immediate restart without a host retry is
therefore not established. LCD panels, headphones, long-term audio/video sync,
other applications and other firmware remain untested on physical hardware.
Offline validation of 31 external firmware combinations and 10 tool images
checks required imports and profiles, not those runtime behaviors.

New coredump checks after the controlled experiments found none. Local receipts,
captures and copied analysis databases are retained under
`.git/audio-dual-output-2026-11-03/`. Builds are under
`builds/dual-output-2026-11-03/`. These experiments preceded the version 2.1
release; the release rebuild updates the module version to 2.1.
