# Building Claudi

This document is the practical build guide for the current `claudi` project.
It reflects the repo as it exists now: **ESP-IDF 5.5**, **ESP32-S3**, and the
multi-board firmware under `firmware/`.

## What this repo builds

Claudi is a Claude Code companion device firmware with two current build targets:

- **`amoled175`** — Waveshare ESP32-S3-Touch-AMOLED-1.75 (default, fully supported)
- **`watch169`** — ESPWatch 1.69 bring-up target

All active firmware work lives in:

```sh
firmware/
```

## Prerequisites

### 1. ESP-IDF 5.5 installed at the expected path

The project expects:

```sh
~/esp/esp-idf
```

### 2. Python 3.13 available for `export.sh`

On this machine, `esp-idf/export.sh` requires a Python 3.13-backed environment.
If your default `python3` is not 3.13, create a shim once:

```sh
mkdir -p ~/.esp_shim/bin
ln -sf /opt/homebrew/bin/python3.13 ~/.esp_shim/bin/python3
```

Then prepend the shim before sourcing ESP-IDF:

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
```

If `idf.py` is still missing after sourcing, check the export output. A common
failure is a missing ESP-IDF Python environment.

## Build setup

From a fresh shell:

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
cd /Users/philip/Development/claudi/firmware
```

## Build commands

The repo provides make wrappers so each board keeps its own config/build dir.

### Build the AMOLED target

```sh
make amoled
```

Outputs go to:

```sh
firmware/build/
```

### Build the watch target

```sh
make watch
```

Outputs go to:

```sh
firmware/build.watch/
```

## Direct `idf.py` equivalents

If you prefer the raw ESP-IDF commands:

### AMOLED

```sh
idf.py -DSDKCONFIG=sdkconfig.amoled \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.amoled175" \
  build
```

### Watch

```sh
idf.py -B build.watch \
  -DSDKCONFIG=sdkconfig.watch \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.watch169" \
  build
```

## Flashing to hardware

First check the USB serial device:

```sh
ls /dev/cu.usbmodem*
```

Then use the provided make targets.

### Flash AMOLED target

```sh
make flash-amoled
```

### Flash watch target

```sh
make flash-watch
```

If auto-detection picks the wrong port, override it explicitly:

```sh
make flash-amoled PORT=/dev/cu.usbmodem3101
```

## Host-only verification

These checks do not require the board to be plugged in.

### Core logic test

```sh
cd /Users/philip/Development/claudi/firmware/components/claudi_core/test
make test
```

### Hook self-test

```sh
cd /Users/philip/Development/claudi
python3 .claude/hooks/claudi_hook.py --self-test
```

### Hook verification helper

```sh
cd /Users/philip/Development/claudi
.claude/hooks/verify.sh
```

## Build verification that has been confirmed in this repo

The following commands have been verified successfully in this repo state:

```sh
cd /Users/philip/Development/claudi/firmware
make watch

export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
idf.py build
```

And these host-side checks also pass:

```sh
cd /Users/philip/Development/claudi/firmware/components/claudi_core/test && make test
cd /Users/philip/Development/claudi && python3 .claude/hooks/claudi_hook.py --self-test
```

## Troubleshooting

### `idf.py: command not found`

You likely did not source ESP-IDF successfully.

Run:

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
command -v idf.py
```

### ESP-IDF complains its Python environment is missing

This usually means `export.sh` ran under the wrong Python version. Confirm the
shim is first on `PATH`:

```sh
which python3
python3 --version
```

Then re-run:

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
```

### No serial port appears

Check:

```sh
ls /dev/cu.usbmodem*
```

If nothing appears, the board is not currently connected, not powered, or macOS
has not enumerated the USB device.

### Need a clean rebuild

Remove the board-specific build directory and rebuild:

```sh
rm -rf build
make amoled
```

or

```sh
rm -rf build.watch
make watch
```

## Related files

- `firmware/README.md` — project overview
- `firmware/Makefile` — board-specific build wrappers
- `CLAUDE.md` — repo-specific developer notes
- `.claude/hooks/verify.sh` — no-hardware hook verification helper
