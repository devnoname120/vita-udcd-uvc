import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--shutdown", action="store_true")
args = parser.parse_args()
text = args.source.read_text()
begin = "int module_stop(SceSize argc, const void *args)\n{" if args.shutdown else "static int send_frame(void)\n{"
end = None if args.shutdown else "static int display_vblank_cb_func("
if text.count(begin) != 1 or (end and text.count(end) != 1):
    raise SystemExit("Function boundaries changed; update the test extraction.")
start = text.index(begin)
stop = text.index(end, start) if end else len(text)
args.output.write_text(text[start:stop])
