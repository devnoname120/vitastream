import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--shutdown", action="store_true")
mode.add_argument("--pacing", action="store_true")
mode.add_argument("--controls", action="store_true")
mode.add_argument("--startup", action="store_true")
mode.add_argument("--conversion", action="store_true")
mode.add_argument("--usb-recovery", action="store_true")
mode.add_argument("--audio-backend", action="store_true")
mode.add_argument("--usb-start", action="store_true")
mode.add_argument("--suspend", action="store_true")
args = parser.parse_args()
text = args.source.read_text()
prefix = ""
if args.suspend:
    regions = (
        ("enum { UVC_EVENT_WORK", "static int usb_ep0_req_send("),
        ("static int uvc_suspend_quiesce_device(void)", "static int uvc_vitashell_usb_request(void)"),
        ("static int uvc_system_event_bridge(", "void _start()"),
    )
    parts = []
    for begin, end in regions:
        if text.count(begin) != 1 or text.count(end) != 1:
            raise SystemExit("Suspend boundaries changed; update the test extraction.")
        start = text.index(begin)
        parts.append(text[start:text.index(end, start)])
    args.output.write_text("\n".join(parts))
    raise SystemExit(0)
if args.audio_backend:
    # Compile the production backend with simulated firmware exports, worker
    # acknowledgements and DMA completion. Keep constants and types in sync.
    regions = (
        ("#define ALIGN(value, alignment)", "int module_get_export_func("),
        ("static AudioMonitorSetPathModeFn", "typedef int16_t AudioPcmFrame"),
        ("static int audio_resolve_monitor_functions(void)", "static void audio_ring_reset(void)"),
        ("static int audio_capture_run(unsigned int generation)", "static int audio_capture_thread("),
        ("int uac_audio_start(void)", "void uac_audio_request_stop(void)"),
    )
    parts = []
    for begin, end in regions:
        if text.count(begin) != 1 or text.count(end) != 1:
            raise SystemExit("Audio boundaries changed; update the test extraction.")
        start = text.index(begin)
        parts.append(text[start:text.index(end, start)])
    args.output.write_text("\n".join(parts))
    raise SystemExit(0)
elif args.usb_start:
    begin = "int uvc_start(void)\n{"
    end = "int uvc_stop(void)\n{"
elif args.usb_recovery:
    begin = "static int uvc_mtp_ready(void)\n{"
    end = "static inline unsigned int display_to_iftu_pixelformat("
elif args.conversion:
    begin = "static inline unsigned int display_to_iftu_pixelformat("
    end = "#define SCE_DISPLAY_FOR_DRIVER_NID"
elif args.startup:
    begin = "int module_start(SceSize argc, const void *args)\n{"
    end = "int module_stop(SceSize argc, const void *args)\n{"
elif args.shutdown:
    begin = "int module_stop(SceSize argc, const void *args)\n{"
    end = None
elif args.pacing:
    begin = "static int display_vblank_cb_func("
    end = "static int uvc_thread("
    descriptors = args.source.parent.parent / "include" / "usb_descriptors.h"
    prefix = next(line for line in descriptors.read_text().splitlines()
                  if line.startswith("#define FPS_TO_INTERVAL(")) + "\n"
elif args.controls:
    begin = "static void uvc_handle_video_streaming_req_recv("
    end = "void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req)\n{"
else:
    begin = "#define SCE_DISPLAY_FOR_DRIVER_NID"
    end = "static int display_vblank_cb_func("
if text.count(begin) != 1 or (end and text.count(end) != 1):
    raise SystemExit("Function boundaries changed; update the test extraction.")
start = text.index(begin)
stop = text.index(end, start) if end else len(text)
args.output.write_text(prefix + text[start:stop])
