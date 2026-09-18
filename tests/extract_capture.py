import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--shutdown", action="store_true")
mode.add_argument("--pacing", action="store_true")
mode.add_argument("--controls", action="store_true")
args = parser.parse_args()
text = args.source.read_text()
prefix = ""
if args.shutdown:
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
    begin = "static int send_frame(void)\n{"
    end = "static int display_vblank_cb_func("
if text.count(begin) != 1 or (end and text.count(end) != 1):
    raise SystemExit("Function boundaries changed; update the test extraction.")
start = text.index(begin)
stop = text.index(end, start) if end else len(text)
args.output.write_text(prefix + text[start:stop])
