.PHONY: build release test sim daemon seed help fmt clean firmware firmware-flash

CARGO ?= cargo

help:
	@echo "Targets:"
	@echo "  build     - debug build of all crates"
	@echo "  release   - release build of all crates"
	@echo "  test      - run all tests"
	@echo "  sim       - run device simulator window"
	@echo "  daemon    - run the daemon (set CLAUDI_DEVICE to a pty path)"
	@echo "  seed      - send a demo set of hook events to a running daemon"

build:
	$(CARGO) build

release:
	$(CARGO) build --release

test:
	$(CARGO) test

sim:
	$(CARGO) run --release -p claudi-simulator

daemon:
	RUST_LOG=info $(CARGO) run --release -p claudid

seed:
	./scripts/seed.sh

fmt:
	$(CARGO) fmt --all

clean:
	$(CARGO) clean

ARM_TC ?= $(HOME)/arm-toolchain/arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi
PICO_SDK_PATH ?= $(HOME)/pico-sdk

firmware:
	@cd firmware && mkdir -p build && cd build && \
	  PICO_TOOLCHAIN_PATH=$(ARM_TC) PICO_SDK_PATH=$(PICO_SDK_PATH) cmake -G Ninja .. >/dev/null && \
	  ninja
	@echo "→ firmware/build/claudi_firmware.uf2"

firmware-flash: firmware
	@echo "Put the device in BOOTSEL mode (hold BOOT while plugging USB)."
	@echo "Then it will mount as /Volumes/RPI-RP2 (or RP2350)."
	@for i in 1 2 3 4 5 6 7 8 9 10; do \
	  if [ -d /Volumes/RP2350 ]; then DEST=/Volumes/RP2350; break; fi; \
	  if [ -d /Volumes/RPI-RP2 ]; then DEST=/Volumes/RPI-RP2; break; fi; \
	  sleep 1; \
	done; \
	if [ -n "$$DEST" ]; then \
	  echo "copying UF2 to $$DEST"; \
	  cp firmware/build/claudi_firmware.uf2 $$DEST/; \
	else \
	  echo "BOOTSEL volume not found. Aborting."; exit 1; \
	fi
