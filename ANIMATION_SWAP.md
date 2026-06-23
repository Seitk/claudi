# Swapping in New Claudi Animations

This file explains the current, safest way to replace Claudi's pet animation art.
It is based on the current repo layout, where the pet frames come from:

```sh
assets/slime-final/
```

and are converted into embedded LVGL image assets by:

```sh
firmware/tools/gen_pet_assets.py
```

## How the animation pipeline works

1. The source art lives under `assets/slime-final/`.
2. `assets/slime-final/manifest.json` declares which PNG frames belong to each pet state.
3. `firmware/tools/gen_pet_assets.py` reads that manifest.
4. The script converts each frame into LVGL C image assets in:

   ```sh
   firmware/components/brookesia_app_claudi/assets/
   ```

5. The Claudi app uses those generated images for each pet state.

The generator expects the Claudi state vocabulary to stay aligned with `claudi_core`:

- `idle`
- `blink`
- `happy`
- `sleepy`
- `curious`
- `alert`
- `bored`
- `working`
- `thinking`
- `attention`
- `idea`
- `excited`

## Best-practice workflow

### Option A: Replace the art but keep the same state names

This is the easiest and safest route.

1. Prepare new PNG frames for each existing state.
2. Put them somewhere under `assets/slime-final/`.
3. Update `assets/slime-final/manifest.json` so each state points to the new frame files.
4. Regenerate the embedded assets.
5. Rebuild the firmware.
6. Flash and verify on hardware.

## Manifest expectations

The generator supports either:

- a `frames` array per state, or
- a single `frame_png` entry

Examples:

### Two-frame state

```json
{
  "states": {
    "idle": {
      "frames": [
        "assets/slime-final/idle_0.png",
        "assets/slime-final/idle_1.png"
      ]
    }
  }
}
```

### Single-frame state

```json
{
  "states": {
    "idea": {
      "frame_png": "assets/slime-final/idea.png"
    }
  }
}
```

If only one frame is provided, the generator duplicates it automatically so the
runtime still gets a stable two-frame table.

## Regenerate the Claudi assets

The generator depends on the Python packages `pypng` and `lz4`. On this machine,
running it without those packages failed inside `LVGLImage.py` with errors like:

```text
ImportError: Need pypng package, do `pip3 install pypng`
ImportError: Need lz4 package, do `pip3 install lz4`
```

So install those first in the Python environment you plan to use for generation.
For example:

```sh
python3 -m pip install pypng lz4
```

Then from the repo root:

```sh
cd /Users/philip/Development/claudi
python3 firmware/tools/gen_pet_assets.py
```

If successful, the script writes generated LVGL C assets into:

```sh
firmware/components/brookesia_app_claudi/assets/
```

## Rebuild after changing animation art

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
cd /Users/philip/Development/claudi/firmware
make amoled
```

Or for the watch target:

```sh
make watch
```

## Flash and verify

If the target device is connected:

```sh
make flash-amoled PORT=/dev/cu.usbmodem3101
```

Then verify:

- the device still boots
- each expected pet state still renders
- transparent edges look correct
- no state is missing or mapped to the wrong frame
- animations are obvious enough on-device

## Important constraints

### 1. Do not rename state keys casually

The code expects the existing Claudi state names. If you invent new state names
in the manifest without also updating the firmware state mapping, those new
states will not be used.

### 2. Keep image dimensions visually consistent

If the replacement art has very different framing, Claudi may look misaligned or
cropped even if the build succeeds. Prefer keeping subject scale and canvas size
consistent across states.

### 3. Preserve transparency

The generator emits `RGB565A8`, so alpha is expected and supported. Use PNGs
with correct transparency instead of flattening onto a solid background.

### 4. Generated files should be regenerated, not hand-edited

Treat the files under:

```sh
firmware/components/brookesia_app_claudi/assets/
```

as generated output. Update the source PNGs and manifest, then rerun the script.
Do not manually tweak the generated C files.

## If you want a totally new animation set

If you want to move away from `slime-final` entirely, the cleanest approach is:

1. create a new source-art folder under `assets/`
2. update `firmware/tools/gen_pet_assets.py` to point `MANIFEST` at the new folder
3. keep the same state names
4. regenerate assets
5. rebuild and flash

That lets you swap the visual style without changing the Claudi state machine.

## Recommended minimal change sequence

For a normal refresh of Claudi's look, do exactly this:

```sh
cd /Users/philip/Development/claudi
# 1. replace PNGs under assets/slime-final/
# 2. update assets/slime-final/manifest.json
python3 firmware/tools/gen_pet_assets.py
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
cd firmware
make amoled
```

Then flash the board and visually confirm the new animations on-device.
