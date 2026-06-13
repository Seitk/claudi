#!/usr/bin/env python3
"""Convert the slime pet art (assets/slime-final) into embedded LVGL images.

For each state in the slime manifest, converts its 2 animation frames to LVGL
C-array images (RGB565A8, alpha preserved) named slime_<state>_<frame> into the
claudi app component's assets/ dir. The app declares these and plays them with
lv_animimg, swapping the source per pet state.

Run from anywhere:  python3 firmware/tools/gen_pet_assets.py
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)                      # firmware/
REPO = os.path.dirname(FW)                      # repo root
MANIFEST = os.path.join(REPO, "assets", "slime-final", "manifest.json")
CONVERTER = os.path.join(FW, "managed_components", "lvgl__lvgl", "scripts", "LVGLImage.py")
OUTDIR = os.path.join(FW, "components", "brookesia_app_claudi", "assets")

# claudi_core's state vocabulary (must match claudi_state_name()).
STATES = ["idle", "blink", "happy", "sleepy", "curious", "alert",
          "bored", "working", "thinking", "attention", "idea", "excited"]


def main():
    with open(MANIFEST) as fh:
        manifest = json.load(fh)
    states = manifest["states"]
    os.makedirs(OUTDIR, exist_ok=True)

    generated = []
    for st in STATES:
        if st not in states:
            print(f"  WARN: state '{st}' missing from manifest; skipping")
            continue
        frames = states[st].get("frames") or [states[st]["frame_png"]]
        # Always emit exactly 2 frames (duplicate if only one) for a stable table.
        if len(frames) == 1:
            frames = [frames[0], frames[0]]
        for i, frame in enumerate(frames[:2]):
            if not os.path.exists(frame):
                print(f"  ERROR: missing frame {frame}")
                return 1
            name = f"slime_{st}_{i}"
            cmd = [sys.executable, CONVERTER, "--ofmt", "C", "--cf", "RGB565A8",
                   "--name", name, "-o", OUTDIR, frame]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            generated.append(name)
        print(f"  ok  {st}: slime_{st}_0, slime_{st}_1")

    print(f"\nGenerated {len(generated)} images into {OUTDIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
