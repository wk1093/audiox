CC = aarch64-linux-gnu-gcc
CFLAGS = -static -O2 -Iold/include -Wall -Wextra -Werror -Wno-error=format-truncation -pthread -std=gnu11
CXX = aarch64-linux-gnu-g++
CXXFLAGS = -static -O3 -Iinclude -Iold/include -Wall -Wextra -Werror -Wno-error=format-truncation -Wno-error=unused-parameter -pthread -fno-exceptions -fno-rtti -std=c++17
LIBS = -lm -pthread

# Recursive source discovery for upcoming C++ port.
CPP_SRCS := $(shell find src -type f -name '*.cpp' 2>/dev/null)

ifeq ($(strip $(CPP_SRCS)),)
INIT_SRCS := old/src/init.c
INIT_COMPILER := $(CC)
INIT_FLAGS := $(CFLAGS)
else
INIT_SRCS := $(CPP_SRCS)
INIT_COMPILER := $(CXX)
INIT_FLAGS := $(CXXFLAGS)
endif

# Version information
AUDIOX_VERSION_MAJOR = 0
AUDIOX_VERSION_MINOR = 1
AUDIOX_VERSION_PATCH = 1

# Auto-detected from firmware after fetch_deps runs.
KV = $(shell $(SCRIPTS_DIR)/detect_kernel_version.sh "$(OUT_DIR)" "6.18.37-v8+")
VC4_OVERLAY ?= vc4-fkms-v3d-pi4
DSI_TOUCH_OVERLAY ?=
DEBUG_SHELL ?= 0

# Paths
ARCH ?= aarch64
OUT_DIR = $(CURDIR)/out
ROOTFS_DIR = $(OUT_DIR)/rootfs
INITRAMFS = $(OUT_DIR)/initramfs.cpio.gz
MODULE_LOAD_LIST = $(OUT_DIR)/module-load.list
MODULE_LOAD_BASE_LIST = $(OUT_DIR)/module-load.base.list
MODULE_LOAD_NORMAL_LIST = $(OUT_DIR)/module-load.normal.list
SCRIPTS_DIR = $(CURDIR)/scripts
DEP_FILE = $(CURDIR)/depmod.txt
BOOT_LOGO_PPM ?= $(CURDIR)/logo_boot.ppm
WEB_LOGO_SVG ?= $(CURDIR)/logo.svg
WEB_LOGO_PNG ?= $(CURDIR)/logo.png
WEB_INDEX_HTML ?= $(CURDIR)/web/index.html
FIRMWARE_GIT_URL ?= https://github.com/raspberrypi/firmware.git

SD_MOUNT_BOOT ?= /run/media/$(USER)/bootfs

IMG_FILE = $(OUT_DIR)/audiox.img
IMG_SIZE_MB = 128

.PHONY: all clean rootfs initramfs fetch_deps fetch_modules show_modules show_kernel qemu fancyexport export image FORCE

all: initramfs

fetch_deps:
	@mkdir -p $(OUT_DIR)
	@$(SCRIPTS_DIR)/sync_firmware.sh "$(FIRMWARE_GIT_URL)" "$(OUT_DIR)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

fetch_modules: fetch_deps
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(DEP_FILE)"

show_modules: fetch_modules
	@cat $(OUT_DIR)/module-load.summary.txt

show_kernel:
	@echo "Detected kernel version: $(KV)"
	@if [ -f "$(OUT_DIR)/firmware/extra/uname_string8" ]; then \
		echo "Source: $(OUT_DIR)/firmware/extra/uname_string8"; \
		echo "Full uname string:"; \
		cat "$(OUT_DIR)/firmware/extra/uname_string8"; \
	else \
		echo "(using default; fetch_deps not yet run or file not found)"; \
	fi

$(MODULE_LOAD_LIST) $(MODULE_LOAD_BASE_LIST) $(MODULE_LOAD_NORMAL_LIST): fetch_modules
	@:

$(ROOTFS_DIR)/init: fetch_deps FORCE $(INIT_SRCS)
	@echo "Compiling single-process init runtime..."
	mkdir -p $(ROOTFS_DIR)
	$(INIT_COMPILER) $(INIT_FLAGS) \
		-DKERNEL_VERSION='"$(KV)"' \
		-DAUDIOX_VERSION_MAJOR=$(AUDIOX_VERSION_MAJOR) \
		-DAUDIOX_VERSION_MINOR=$(AUDIOX_VERSION_MINOR) \
		-DAUDIOX_VERSION_PATCH=$(AUDIOX_VERSION_PATCH) \
		-DDEBUG_SHELL=$(DEBUG_SHELL) \
		-o $(ROOTFS_DIR)/init $(INIT_SRCS) $(LIBS)

rootfs: $(ROOTFS_DIR)/init $(MODULE_LOAD_LIST) $(MODULE_LOAD_BASE_LIST) $(MODULE_LOAD_NORMAL_LIST)
	@echo "Creating rootfs structure..."
	mkdir -p $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/sbin $(ROOTFS_DIR)/etc
	mkdir -p $(ROOTFS_DIR)/proc $(ROOTFS_DIR)/sys $(ROOTFS_DIR)/dev
	mkdir -p $(ROOTFS_DIR)/etc/www
	mkdir -p $(ROOTFS_DIR)/lib/modules
	
	@echo "Staging kernel objects into target lib tree..."
	cp -r $(OUT_DIR)/modules_staging/* $(ROOTFS_DIR)/lib/modules/
	cp $(MODULE_LOAD_LIST) $(ROOTFS_DIR)/etc/module-load.list
	cp $(MODULE_LOAD_BASE_LIST) $(ROOTFS_DIR)/etc/module-load.base.list
	cp $(MODULE_LOAD_NORMAL_LIST) $(ROOTFS_DIR)/etc/module-load.normal.list
	@if [ -f "$(BOOT_LOGO_PPM)" ]; then \
		echo "Staging boot logo from $(BOOT_LOGO_PPM)..."; \
		cp "$(BOOT_LOGO_PPM)" "$(ROOTFS_DIR)/etc/logo_boot.ppm"; \
	else \
		echo "Warning: boot logo file not found at $(BOOT_LOGO_PPM)"; \
	fi
	@if [ -f "$(WEB_INDEX_HTML)" ]; then \
		echo "Staging web index from $(WEB_INDEX_HTML)..."; \
		cp "$(WEB_INDEX_HTML)" "$(ROOTFS_DIR)/etc/www/index.html"; \
	else \
		echo "Warning: web index file not found at $(WEB_INDEX_HTML)"; \
	fi
	@if [ -f "$(WEB_LOGO_SVG)" ]; then \
		echo "Staging web logo svg from $(WEB_LOGO_SVG)..."; \
		cp "$(WEB_LOGO_SVG)" "$(ROOTFS_DIR)/etc/www/logo.svg"; \
	fi
	@if [ -f "$(WEB_LOGO_PNG)" ]; then \
		echo "Staging web logo png from $(WEB_LOGO_PNG)..."; \
		cp "$(WEB_LOGO_PNG)" "$(ROOTFS_DIR)/etc/www/logo.png"; \
	fi

initramfs: rootfs
	@echo "Packaging initramfs..."
	cd $(ROOTFS_DIR) && \
	find . -print0 | cpio --null --create --format=newc | gzip -9 > $(INITRAMFS)
	@echo "Success! audiox initramfs built at: $(INITRAMFS)"

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

image: initramfs
	@$(SCRIPTS_DIR)/build_image.sh "$(IMG_FILE)" "$(IMG_SIZE_MB)" "$(OUT_DIR)" "$(INITRAMFS)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

clean:
	rm -rf $(OUT_DIR)