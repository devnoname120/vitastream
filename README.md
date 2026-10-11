# vitastream

USB video and audio streaming for PS Vita, based on
[vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc) by
[Xerpi (Sergi Granell)](https://github.com/xerpi), who created the original
USB video streaming plugin.

## What's this?

This is a kernel plugin that lets you stream your PSVita screen and its final
software audio mix to your computer via USB.

## How does it work?

The plugin uses the [SceUdcd](https://wiki.henkaku.xyz/vita/SceUdcd) module of the PSVita OS to setup
the necessary USB descriptors to simulate and behave as an [USB Video Class](https://en.wikipedia.org/wiki/USB_video_device_class) device (like a webcam or an USB video capture card).

The [hardware color space converter](https://wiki.henkaku.xyz/vita/IFTU_Registers) of the PSVita's SoC is used to perform the conversion to the destination pixel format; then the USB
controller directly performs a DMA transfer from the physical address of the resulting converted framebuffer, and therefore, saving CPU usage and power consumption.

Audio is exposed to the computer as a USB Audio Class 1.0 capture/input device
using 48 kHz, stereo, signed 16-bit PCM. On firmware 3.60 it captures the normal
handheld mix from SourceMixer1's non-destructive secondary output while the
normal main output continues feeding the I2S7 speaker/headphone path. The tap is
after SceAudio and SceAudioSource have been mixed and before the codec applies
analog volume or mute. It does not capture the microphone.

## Supported formats and resolutions

* 960x544 @ 30 FPS and (less than) 60 FPS
* 896x504 @ 30 FPS and (almost) 60 FPS
* 864x488 @ 30 FPS and 60 FPS
* 480x272 @ 30 FPS and 60 FPS
* 1280x720 @ 30 FPS

## Download and installation

**Download**:

* [vitastream.skprx](https://github.com/devnoname120/vitastream/releases)

Releases published before the rename contain `udcd_uvc.skprx`. Rename that
downloaded file to `vitastream.skprx` to use the installation paths below.

**Compilation**

* [vitasdk](https://vitasdk.org/) is needed.

**Installation**:

1. Copy `vitastream.skprx` to your PSVita
2. Add `vitastream.skprx` to taiHEN's config (`ur0:/tai/config.txt` or `ux0:/tai/config.txt`):
```
*KERNEL
ur0:tai/vitastream.skprx
```
3. Reboot your PSVita.

When upgrading from vita-udcd-uvc, replace its existing kernel plugin entry with
`vitastream.skprx` so only one streaming plugin is loaded. New builds identify
the USB video and audio device as `vitastream` on your computer.

## Listening to audio on macOS

On macOS, FFmpeg versions older than 8.1.3 have an audio bug that prevents
sound from working correctly. Upgrade to FFmpeg 8.1.3 or later.

## Troubleshooting

If the video looks glitched, try to change the video player configuration to use the *NV12* format or switch to another player (like PotPlayer or OBS). If the colors look wrong, set color range to full and color space to BT.601 (Rec. 601).

If you use Windows 10 you might have to change the Camera access permissions on the Privacy Settings.

On Linux I recommend using *mplayer* (`mplayer tv:// -tv driver=v4l2:device=/dev/videoX:width=960:height=544`).

USB audio captures firmware 3.60's normal SourceMixer1 handheld mix immediately
before its main output is sent to I2S7. Audio routed exclusively to Bluetooth,
HDMI, or other hardware outputs is not guaranteed to be present in the USB
capture.

Note: Remember that if anything goes wrong (like PSVita not booting) you can always press L at boot to skip plugin loading.
