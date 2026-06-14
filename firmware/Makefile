# Multi-board build wrappers. Source ESP-IDF first, e.g.:
#   export PATH="$HOME/.esp_shim/bin:$PATH"   # python3.13 shim for export.sh
#   . ~/esp/esp-idf/export.sh
# Then: `make amoled` or `make watch` (flash with `make flash-amoled` / `flash-watch`).
# Each board keeps its own sdkconfig.<board>; the watch uses a separate build dir
# so the two never cross-contaminate.
PORT ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -n1)
BASE = sdkconfig.defaults
AMOLED_DEFAULTS = $(BASE);sdkconfig.defaults.amoled175
WATCH_DEFAULTS  = $(BASE);sdkconfig.defaults.watch169

amoled:
	idf.py -DSDKCONFIG=sdkconfig.amoled -DSDKCONFIG_DEFAULTS="$(AMOLED_DEFAULTS)" build

watch:
	idf.py -B build.watch -DSDKCONFIG=sdkconfig.watch -DSDKCONFIG_DEFAULTS="$(WATCH_DEFAULTS)" build

flash-amoled:
	idf.py -DSDKCONFIG=sdkconfig.amoled -DSDKCONFIG_DEFAULTS="$(AMOLED_DEFAULTS)" -p $(PORT) flash monitor

flash-watch:
	idf.py -B build.watch -DSDKCONFIG=sdkconfig.watch -DSDKCONFIG_DEFAULTS="$(WATCH_DEFAULTS)" -p $(PORT) flash monitor

.PHONY: amoled watch flash-amoled flash-watch
