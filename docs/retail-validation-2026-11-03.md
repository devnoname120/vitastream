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

The installed candidate is the diagnostic build from the 2026-11-01 issue test,
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
behavior. The firmware monitor still mutes local speakers/headphones while
USB audio capture is active; simultaneous local playback is not implemented.

`make test` passed under AddressSanitizer and UndefinedBehaviorSanitizer before
the commit. Existing offline firmware/import evidence remains separate from
this single-device result. Local raw receipts and captures are retained under
`.git/retail-investigation-2026-11-03/` and are not included in the source tree.
