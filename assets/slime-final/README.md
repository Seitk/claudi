# Blue Slime Final Assets

Canonical blue jelly slime asset pack extracted from the final A/B/C sprite sheets.

## Output sizes
- `172x172` per-frame sprite PNGs on black background
- `320x172` per-state screen preview PNGs (sprite centered on target display size)

## State map

### Sheet A
1. `idle`
2. `blink`
3. `happy`
4. `idea`

### Sheet B
1. `sleepy`
2. `curious`
3. `alert`
4. `bored`

### Sheet C
1. `thinking`
2. `working`
3. `attention`
4. `excited`

## Important files
- `manifest.json` — machine-readable asset manifest
- `sheetA-contact-strip.png`
- `sheetB-contact-strip.png`
- `sheetC-contact-strip.png`

## Notes
- The checked-in PNG frames and `manifest.json` are the canonical repo-local source for the current pet art.
- The original image-generation source sheets were one-off local drafts and are intentionally not versioned here.
- Each frame was auto-cropped to the visible non-black sprite, scaled with nearest-neighbor, and centered on a `172x172` canvas.
- Screen preview files show how each state would sit on the `320x172` LCD.
