CC = aarch64-linux-gnu-gcc
CFLAGS = -static -O3 -Iinclude -Wall -Wextra -Werror -pthread
LIBS = -lm -pthread

KV ?= 6.18.36-v8+
VC4_OVERLAY ?= vc4-fkms-v3d-pi4
DSI_TOUCH_OVERLAY ?=

# Paths
ARCH ?= aarch64
OUT_DIR = $(CURDIR)/out
ROOTFS_DIR = $(OUT_DIR)/rootfs
INITRAMFS = $(OUT_DIR)/initramfs.cpio.gz
MODULE_LOAD_LIST = $(OUT_DIR)/module-load.list
SCRIPTS_DIR = $(CURDIR)/scripts
DEP_FILE = $(CURDIR)/depmod.txt
WAV_DIR ?= $(CURDIR)/wavs
FIRMWARE_GIT_URL ?= https://github.com/raspberrypi/firmware.git

SD_MOUNT_BOOT ?= /run/media/$(USER)/bootfs

IMG_FILE = $(OUT_DIR)/custom_audio_console.img
IMG_SIZE_MB = 128

# Debug build variables (native x86_64, runs in qemu-system-x86_64)
DEBUG_CC     = gcc
DEBUG_CFLAGS = -static -O0 -g -Iinclude -Wall -Wextra -Werror -pthread
DEBUG_OUT    = $(OUT_DIR)/debug
DEBUG_ROOTFS = $(DEBUG_OUT)/rootfs
DEBUG_INITRAMFS = $(DEBUG_OUT)/initramfs.cpio.gz
DEBUG_KERNEL ?= $(firstword $(wildcard /boot/vmlinuz-linux /boot/vmlinuz /boot/vmlinuz-generic))

.PHONY: all clean rootfs initramfs fetch_deps fetch_modules show_modules qemu fancyexport export image debug debug_rootfs debug_initramfs FORCE

all: initramfs

# 1. Clone/Sync Firmware Tree and Stage Boot Files
fetch_deps:
	@$(SCRIPTS_DIR)/sync_firmware.sh "$(FIRMWARE_GIT_URL)" "$(OUT_DIR)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

# 2. Resolve Full Module Dependency Closure from depmod.txt
fetch_modules: fetch_deps
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(DEP_FILE)"

show_modules: fetch_modules
	@cat $(OUT_DIR)/module-load.summary.txt

$(MODULE_LOAD_LIST): fetch_modules
	@:

$(ROOTFS_DIR)/init: FORCE src/init.c
	@echo "Compiling system architecture init binary..."
	mkdir -p $(ROOTFS_DIR)
	$(CC) $(CFLAGS) -DKERNEL_VERSION='"$(KV)"' -o $(ROOTFS_DIR)/init src/init.c $(LIBS)

# 3. Build the File System Structure and Stage Kernel Modules
rootfs: $(ROOTFS_DIR)/init $(MODULE_LOAD_LIST)
	@echo "Creating rootfs structure..."
	mkdir -p $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/sbin $(ROOTFS_DIR)/etc
	mkdir -p $(ROOTFS_DIR)/proc $(ROOTFS_DIR)/sys $(ROOTFS_DIR)/dev
	mkdir -p $(ROOTFS_DIR)/etc/wavs
	mkdir -p $(ROOTFS_DIR)/lib/modules
	
	@echo "Staging kernel objects into target lib tree..."
	cp -r $(OUT_DIR)/modules_staging/* $(ROOTFS_DIR)/lib/modules/
	cp $(MODULE_LOAD_LIST) $(ROOTFS_DIR)/etc/module-load.list
	@if [ -f "$(WAV_DIR)/voice0.wav" ] && [ -f "$(WAV_DIR)/voice1.wav" ] && [ -f "$(WAV_DIR)/voice2.wav" ] && [ -f "$(WAV_DIR)/voice3.wav" ]; then \
		echo "Staging WAV voices from $(WAV_DIR)..."; \
		cp "$(WAV_DIR)/voice0.wav" "$(ROOTFS_DIR)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice1.wav" "$(ROOTFS_DIR)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice2.wav" "$(ROOTFS_DIR)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice3.wav" "$(ROOTFS_DIR)/etc/wavs/"; \
	else \
		echo "Warning: missing one or more required wav files in $(WAV_DIR) (voice0.wav..voice3.wav)."; \
		echo "Warning: runtime audio init will fail until all 4 files are present."; \
	fi

# 4. Pack the Directory into an Initramfs Archive
initramfs: rootfs
	@echo "Packaging initramfs..."
	cd $(ROOTFS_DIR) && \
	find . -print0 | cpio --null --create --format=newc | gzip -9 > $(INITRAMFS)
	@echo "Success! Audio Appliance Initramfs built at: $(INITRAMFS)"

# 5. Emulate the Architecture using QEMU
qemu: initramfs
	@echo "Launching QEMU in Raspberry Pi 4 Mode..."
	qemu-system-aarch64 \
		-M raspi4b \
		-m 2048 \
		-kernel $(OUT_DIR)/bootfiles/kernel8.img \
		-dtb $(OUT_DIR)/bootfiles/bcm2711-rpi-4-b.dtb \
		-initrd $(INITRAMFS) \
		-append "console=fbcon console=tty1 quiet loglevel=3 rdinit=/init" \
		-display sdl,gl=on

# Automated Device Detection and Flashing Flow
fancyexport: initramfs
	@$(SCRIPTS_DIR)/wait_and_export.sh "$(SD_MOUNT_BOOT)" "$(CURDIR)"

export: initramfs
	@if [ ! -d "$(SD_MOUNT_BOOT)" ]; then \
		echo "Error: SD mount point $(SD_MOUNT_BOOT) not found."; \
		exit 1; \
	fi
	@echo "Exporting build files to SD card..."
	cp $(OUT_DIR)/bootfiles/* $(SD_MOUNT_BOOT)/ -r
	cp $(INITRAMFS) $(SD_MOUNT_BOOT)/initramfs.cpio.gz
	@echo "initramfs initramfs.cpio.gz followkernel" > $(SD_MOUNT_BOOT)/config.txt
	@echo "dtoverlay=$(VC4_OVERLAY)" >> $(SD_MOUNT_BOOT)/config.txt
	@if [ -n "$(DSI_TOUCH_OVERLAY)" ]; then \
		echo "dtoverlay=$(DSI_TOUCH_OVERLAY)" >> $(SD_MOUNT_BOOT)/config.txt; \
	fi
	@echo "dtoverlay=dwc2,dr_mode=peripheral" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "console=fbcon console=tty1 quiet loglevel=3 rdinit=/init fbcon=map:0" > $(SD_MOUNT_BOOT)/cmdline.txt
	sync
	@echo "SD Card Flashed and ready for hardware execution!"

# Generate Standalone Flashable Disk Image File
image: initramfs
	@$(SCRIPTS_DIR)/build_image.sh "$(IMG_FILE)" "$(IMG_SIZE_MB)" "$(OUT_DIR)" "$(INITRAMFS)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

# Native x86_64 Debug Configurations
$(DEBUG_ROOTFS)/init: FORCE src/init.c
	@echo "Compiling debug init binary (x86_64)..."
	mkdir -p $(DEBUG_ROOTFS)
	$(DEBUG_CC) $(DEBUG_CFLAGS) -o $(DEBUG_ROOTFS)/init src/init.c $(LIBS)

debug_rootfs: $(DEBUG_ROOTFS)/init
	@echo "Creating debug rootfs structure..."
	mkdir -p $(DEBUG_ROOTFS)/bin $(DEBUG_ROOTFS)/sbin $(DEBUG_ROOTFS)/etc
	mkdir -p $(DEBUG_ROOTFS)/proc $(DEBUG_ROOTFS)/sys $(DEBUG_ROOTFS)/dev
	mkdir -p $(DEBUG_ROOTFS)/etc/wavs
	@if [ -f "$(WAV_DIR)/voice0.wav" ] && [ -f "$(WAV_DIR)/voice1.wav" ] && [ -f "$(WAV_DIR)/voice2.wav" ] && [ -f "$(WAV_DIR)/voice3.wav" ]; then \
		echo "Staging WAV voices from $(WAV_DIR) into debug rootfs..."; \
		cp "$(WAV_DIR)/voice0.wav" "$(DEBUG_ROOTFS)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice1.wav" "$(DEBUG_ROOTFS)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice2.wav" "$(DEBUG_ROOTFS)/etc/wavs/"; \
		cp "$(WAV_DIR)/voice3.wav" "$(DEBUG_ROOTFS)/etc/wavs/"; \
	else \
		echo "Warning: missing one or more required wav files in $(WAV_DIR) (voice0.wav..voice3.wav)."; \
		echo "Warning: debug runtime audio init will fail until all 4 files are present."; \
	fi

debug_initramfs: debug_rootfs
	@echo "Packaging debug initramfs..."
	cd $(DEBUG_ROOTFS) && \
	find . -print0 | cpio --null --create --format=newc | gzip -9 > $(DEBUG_INITRAMFS)
	@echo "Debug initramfs built at: $(DEBUG_INITRAMFS)"

debug: debug_initramfs
	@if [ -z "$(DEBUG_KERNEL)" ]; then \
		echo "Error: no kernel found. Set DEBUG_KERNEL=/path/to/bzImage"; \
		exit 1; \
	fi
	@echo "Launching QEMU x86_64 debug session..."
	qemu-system-x86_64 \
		-M pc \
		-m 512 \
		-kernel $(DEBUG_KERNEL) \
		-initrd $(DEBUG_INITRAMFS) \
		-append "console=tty1 vga=0x343 rdinit=/init quiet loglevel=0 fbcon=map:0" \
		-display sdl

clean:
	rm -rf $(OUT_DIR)