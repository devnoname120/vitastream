# Everybody's Golf: audio route reset — 2026-11-04

Report: [issue #3, comment 6625594208](https://github.com/devnoname120/vitastream/issues/3#issuecomment-6625594208).
The reporter uses a PCH-1004 on firmware 3.65. The local reproduction and
candidate verification below used the identified retail OLED Vita on 3.60,
running `PCSF00006` (Everybody's Golf). The devkit was not used.

## Observed failure

Before launching the game, USB capture contained nonzero PCM. Launching the
game changed the native SrcMix1 output mode from 2 (RAM readback) to 0 (local
playback). Monitor reads then returned `-2`. The previous error path stopped
the USB audio session and tried a NULL monitor read to drain capture. That
call also returned `-2` because the path was no longer in readback mode. The
worker marked capture unsafe and did not restart while the host kept audio
open. Returning to LiveArea therefore did not restore capture.

The controlled pre-fix captures contained:

- LiveArea baseline: 288,622 nonzero float samples out of 288,768.
- After game launch: zero nonzero samples out of 385,024.
- After closing the game and returning to LiveArea: zero out of 288,768.

The native monitor format remained 48 kHz stereo. The observed failure was a
route change, rather than a monitor sample-rate/channel conversion change.

## Why the route changes

The SceAudioIn active-port query changed from 0 before launch to 1 after launch,
and returned to 0 after closing the game. SceAVConfig's policy worker polls
that predicate. A change schedules a routing update through event `0x1000`.
The worker reapplies monitor modes for paths 0 and 1 from its own desired-route
state: HDMI selects mode 1, its readback route selects mode 2, and local routes
select mode 0. VitaStream's direct SceAudio readback setting does not update
that separate AVConfig policy state.

Thus a normal input-routing refresh can cancel the private readback route.
The game-side call site and the particular game feature opening the input
port were not traced. No anti-recording behavior is inferred.

The private analysis copies retain function names, prototypes and comments
for the AVConfig routing worker, its input-activity notification, and the
AudioIn active-port query. IDA MCP could not start its worker because its
`ida_nexus` dependency was missing; analysis used the installed IDA runtime
on private copies instead. Original firmware files were not modified.

## Candidate behavior

The primary fix holds the capture routes through SceAVConfig's ordinary local
route refresh. A hook on SceAVConfig's import of the monitor-mode setter returns
success for path 0 or 1 / mode 0 only while VitaStream owns capture and both
native mixer modes are still 2. The SrcMix0 forwarding mode must match SrcMix1
to avoid losing PS1 VOICE-port audio; see the separate PS1 investigation.
Nonlocal mode requests, input configuration, volume updates and the rest of
the policy worker continue normally. An explicit nonlocal mode disarms the
hold, and explicit claimants are recorded independently for both paths,
including a new claimant requesting mode 2. Cleanup preserves those owners.
Each held mode-0 request updates that path's desired restoration mode. If no
newer request was observed, cleanup uses the pre-capture mode instead. The
native mode write and ownership decision share a routing mutex with the
hook, so a newer request cannot be overwritten after its success return.
Primary reads and the NULL drain are serialized by the same mutex and skipped
after primary ownership changes. Capture stops when an observed takeover occurs;
unfenced capture memory remains allocated rather than being reused.
The hook is installed on the capture worker, outside USB callbacks; failed
installation leaves recovery available.

The held-reset counter is recorded from the video worker rather than from
the hook. The initial primary-only guard held two route resets while audio
start count and generation remained 1 on the retail 3.60 unit. No monitor
read failure or route recovery was logged during that test. Final verification
also covers the aligned-mode guard after integrating the PS1 fix.

The capture worker handles `-2` as a potentially recoverable route change.
It fences its local playback DMA, revalidates firmware and monitor ownership,
and reclaims the route only when it has returned to ordinary local playback.
It restores readback/format, resets the USB PCM ring, and discards two
transition blocks before forwarding audio again. Capture buffers remain
allocated throughout recovery.

The USB alternate setting and session generation are preserved. Repeated
route fighting is bounded; healthy capture rearms the retry budget. A foreign
monitor/HDMI/Bluetooth owner is not taken over, and unsafe DMA stop failures
retain memory rather than allowing unload.

Recovery remains a fallback for resets outside the guarded import. It does
not suppress Sony's entire routing update. Bluetooth coexistence remains
deferred; this change is not a dynamic Bluetooth handoff implementation.

Once the import hook has been published, live module unload is refused until
reboot. Releasing a taiHEN hook alone does not establish that another CPU has
left its handler. The unload gate also prevents publication during shutdown.
Updates therefore use the existing reboot activation path.

## Verification and limits

ASan/UBSan host tests cover initial and ongoing route resets, speakers on/off,
foreign owners, DMA stop failures, repeated resets, retry-budget rearming and
host stop cancellation. Separate route-guard tests cover the held call,
forwarded paths and modes, owner release, installation failures and the unload
publication gate. Tests also cover latest-request restoration on both paths,
same-mode takeovers, failed requests, concurrent cleanup and concurrent drain.
Normal and diagnostic target builds pass with the
pinned SDK. Offline checks cover 31 external combinations and 10 additional
tool images; five external kit images remain unavailable.

With the diagnostic candidate installed and the same host audio session open:

- After advancing the game's silent autosave notice to the title sequence,
  384,975 of 385,024 float samples were nonzero over a four-second capture.
- After returning to LiveArea with PS while the game remained suspended,
  383,086 of 384,000 samples were nonzero over four seconds.
- Both host probes reported zero callback gaps. Diagnostics retained audio
  start count 1 and session generation 1 through recovery.
- The last readback check found no new coredumps; the taiHEN configuration and
  VitaStream configuration were unchanged.

The initial candidate capture taken at the autosave notice was silent; that
screen's silence is not evidence that in-game capture still failed. The title
sequence provides the nonzero-audio check above.

The transition gap was not measured. The local DMA resumed successfully, but
the absence of speaker distortion after recovery still needs physical
listening confirmation. The reporter's 3.65 setup has not been device-tested.
The general LiveArea crackling and controller-specific PSP menu reports are
not claimed fixed by this change.

Candidate artifacts and source snapshot:
`builds/everybodys-golf-2026-11-04/recovery/`.
Raw recordings, logs, installed-byte receipts and IDA copies:
`.git/everybodys-golf-2026-11-04/`.
The routing guard, recovery and latest-request restoration are included in
[VitaStream v2.3](https://github.com/devnoname120/vitastream/releases/tag/v2.3).
The earlier recovery-only artifacts above are retained as investigation history.
