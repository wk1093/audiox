CC = aarch64-linux-gnu-gcc
CFLAGS = -static -O2 -Wall -Wextra -Werror -Wno-error=format-truncation -pthread -std=gnu11
CXX = aarch64-linux-gnu-g++
CXXFLAGS = -static -O3 -Iinclude -Wall -Wextra -Werror -Wno-error=format-truncation -Wno-error=unused-parameter -pthread -fno-exceptions -fno-rtti -std=c++17
LIBS = -lm -pthread

PKG_FETCH ?= curl -fsSL

# Recursive source discovery for upcoming C++ port.
CPP_SRCS := $(shell find src -type f -name '*.cpp' 2>/dev/null)
BOOTLOADER_SRC := src/init/bootloader.cpp
RUNTIME_CPP_SRCS := $(filter-out $(BOOTLOADER_SRC),$(CPP_SRCS))
RUNTIME_SRCS := $(RUNTIME_CPP_SRCS)
RUNTIME_COMPILER := $(CXX)
RUNTIME_FLAGS = $(CXXFLAGS)
RUNTIME_LIBS = $(LIBS)

BOOTLOADER_SRCS := $(BOOTLOADER_SRC)
BOOTLOADER_COMPILER := $(CXX)
BOOTLOADER_FLAGS := $(CXXFLAGS)

# ALSA dependency strategy (build-only; runtime audio code remains unchanged).
# Source build is the only supported mode.
ENABLE_ALSA ?= 1
ALSA_SYSROOT ?= $(OUT_DIR)/alsa-sysroot
ALSA_PREFIX ?= /usr
ALSA_LIB_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/lib
ALSA_INCLUDE_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/include
ALSA_STATIC_LIB ?= $(ALSA_LIB_DIR)/libasound.a
ALSA_CONF_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/share/alsa

ALSA_VERSION ?= 1.2.14
ALSA_SOURCE_URL ?= https://www.alsa-project.org/files/pub/lib/alsa-lib-$(ALSA_VERSION).tar.bz2
ALSA_SRC_TARBALL ?= $(OUT_DIR)/downloads/alsa-lib-$(ALSA_VERSION).tar.bz2
ALSA_SRC_DIR ?= $(OUT_DIR)/build/alsa-lib-$(ALSA_VERSION)
ALSA_BUILD_DIR ?= $(OUT_DIR)/build/alsa-lib-$(ALSA_VERSION)-build

ifeq ($(ENABLE_ALSA),1)
RUNTIME_FLAGS += -I$(ALSA_INCLUDE_DIR)
RUNTIME_LIBS += -L$(ALSA_LIB_DIR) -lasound -ldl
endif

# Version information
AUDIOX_VERSION_MAJOR = 1
AUDIOX_VERSION_MINOR = 1
AUDIOX_VERSION_PATCH = 6

# Auto-detected from firmware after fetch_deps runs.
KV = $(shell $(SCRIPTS_DIR)/detect_kernel_version.sh "$(OUT_DIR)" "6.18.37-v8+")
VC4_OVERLAY ?= vc4-fkms-v3d-pi4
DSI_TOUCH_OVERLAY ?=
DEBUG_SHELL ?= 0

# Paths
ARCH ?= aarch64
OUT_DIR = $(CURDIR)/out
BOOTLOADER_ROOTFS_DIR = $(OUT_DIR)/bootloader_rootfs
ROOTFS_DIR = $(OUT_DIR)/program_rootfs
PROGRAM_STAGE_DIR = $(OUT_DIR)/program_stage
INITRAMFS = $(OUT_DIR)/initramfs.cpio.gz
PROGRAM_INITRAMFS = $(OUT_DIR)/program.cpio.gz
COMBINED_INITRAMFS = $(OUT_DIR)/combined-initramfs.cpio.gz
MODULE_LOAD_LIST = $(OUT_DIR)/module-load.list
MODULE_LOAD_BASE_LIST = $(OUT_DIR)/module-load.base.list
MODULE_LOAD_NORMAL_LIST = $(OUT_DIR)/module-load.normal.list
BOOT_DEP_FILE = $(CURDIR)/bootmod.txt
BOOT_MODULE_LOAD_LIST = $(OUT_DIR)/bootmodule-load.list
BOOT_MODULE_LOAD_BASE_LIST = $(OUT_DIR)/bootmodule-load.base.list
BOOT_MODULE_LOAD_NORMAL_LIST = $(OUT_DIR)/bootmodule-load.normal.list
SCRIPTS_DIR = $(CURDIR)/scripts
DEP_FILE = $(CURDIR)/depmod.txt
BOOT_LOGO_PPM ?= $(CURDIR)/logo_boot.ppm
WEB_LOGO_SVG ?= $(CURDIR)/logo.svg
WEB_LOGO_PNG ?= $(CURDIR)/logo.png
WEB_INDEX_HTML ?= $(CURDIR)/web/index.html
FIRMWARE_GIT_URL ?= https://github.com/raspberrypi/firmware.git
PI_HOST ?= 169.254.1.2
PI_PORT ?= 80

SD_MOUNT_BOOT ?= /run/media/$(USER)/bootfs

IMG_FILE = $(OUT_DIR)/audiox.img
IMG_SIZE_MB = 128

.PHONY: all clean rootfs bootloader_rootfs program_initramfs bootloader_initramfs initramfs fetch_deps fetch_modules fetch_boot_modules show_modules show_kernel qemu fancyexport export image dev FORCE alsa alsa_source show_alsa

all: initramfs

fetch_deps:
	@mkdir -p $(OUT_DIR)
	@$(SCRIPTS_DIR)/sync_firmware.sh "$(FIRMWARE_GIT_URL)" "$(OUT_DIR)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

fetch_modules: fetch_deps
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(DEP_FILE)"

fetch_boot_modules: fetch_deps
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(BOOT_DEP_FILE)" "bootmodule-load" "bootmodules_staging"

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

$(BOOT_MODULE_LOAD_LIST) $(BOOT_MODULE_LOAD_BASE_LIST) $(BOOT_MODULE_LOAD_NORMAL_LIST): fetch_boot_modules
	@:

ifeq ($(ENABLE_ALSA),1)
RUNTIME_DEPS := $(ALSA_STATIC_LIB)
else
RUNTIME_DEPS :=
endif

show_alsa:
	@echo "ENABLE_ALSA=$(ENABLE_ALSA)"
	@echo "ALSA_MODE=source"
	@echo "ALSA_SYSROOT=$(ALSA_SYSROOT)"
	@echo "ALSA_STATIC_LIB=$(ALSA_STATIC_LIB)"
	@echo "RUNTIME_FLAGS=$(RUNTIME_FLAGS)"
	@echo "RUNTIME_LIBS=$(RUNTIME_LIBS)"

alsa: $(ALSA_STATIC_LIB)

$(ALSA_STATIC_LIB):
	@mkdir -p "$(OUT_DIR)"
	@$(MAKE) alsa_source
	@test -f "$(ALSA_STATIC_LIB)" || { echo "Missing $(ALSA_STATIC_LIB) after ALSA build step"; exit 1; }

alsa_source:
	@echo "Building static alsa-lib $(ALSA_VERSION) with cross toolchain..."
	@mkdir -p "$(OUT_DIR)/downloads" "$(OUT_DIR)/build" "$(ALSA_SYSROOT)"
	@[ -f "$(ALSA_SRC_TARBALL)" ] || $(PKG_FETCH) "$(ALSA_SOURCE_URL)" -o "$(ALSA_SRC_TARBALL)"
	rm -rf "$(ALSA_SRC_DIR)" "$(ALSA_BUILD_DIR)"
	mkdir -p "$(ALSA_SRC_DIR)" "$(ALSA_BUILD_DIR)"
	tar -xjf "$(ALSA_SRC_TARBALL)" -C "$(OUT_DIR)/build"
	cd "$(ALSA_BUILD_DIR)" && \
		"$(ALSA_SRC_DIR)/configure" \
			--host=aarch64-linux-gnu \
			--prefix=$(ALSA_PREFIX) \
			--libdir=$(ALSA_PREFIX)/lib \
			--disable-shared \
			--enable-static \
			CC="$(CC)" CXX="$(CXX)"
	$(MAKE) -C "$(ALSA_BUILD_DIR)" -j$$(nproc)
	$(MAKE) -C "$(ALSA_BUILD_DIR)" DESTDIR="$(ALSA_SYSROOT)" install
	@echo "ALSA static lib ready: $(ALSA_STATIC_LIB)"


$(BOOTLOADER_ROOTFS_DIR)/init: FORCE $(BOOTLOADER_SRCS)
	@echo "Compiling bootloader init..."
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)
	$(BOOTLOADER_COMPILER) $(BOOTLOADER_FLAGS) \
		-DKERNEL_VERSION='"$(KV)"' \
		-o $(BOOTLOADER_ROOTFS_DIR)/init $(BOOTLOADER_SRCS) $(LIBS)

$(ROOTFS_DIR)/init: fetch_deps FORCE $(RUNTIME_DEPS) $(RUNTIME_SRCS)
	@echo "Compiling runtime init..."
	mkdir -p $(ROOTFS_DIR)
	$(RUNTIME_COMPILER) $(RUNTIME_FLAGS) \
		-DKERNEL_VERSION='"$(KV)"' \
		-DAUDIOX_VERSION_MAJOR=$(AUDIOX_VERSION_MAJOR) \
		-DAUDIOX_VERSION_MINOR=$(AUDIOX_VERSION_MINOR) \
		-DAUDIOX_VERSION_PATCH=$(AUDIOX_VERSION_PATCH) \
		-DDEBUG_SHELL=$(DEBUG_SHELL) \
		-o $(ROOTFS_DIR)/init $(RUNTIME_SRCS) $(RUNTIME_LIBS)

bootloader_rootfs: $(BOOTLOADER_ROOTFS_DIR)/init $(BOOT_MODULE_LOAD_LIST) $(BOOT_MODULE_LOAD_BASE_LIST)
	@echo "Creating bootloader rootfs structure..."
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/etc $(BOOTLOADER_ROOTFS_DIR)/lib/modules
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/proc $(BOOTLOADER_ROOTFS_DIR)/sys $(BOOTLOADER_ROOTFS_DIR)/dev
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/mnt/boot $(BOOTLOADER_ROOTFS_DIR)/mnt/rootfs
	cp -r $(OUT_DIR)/bootmodules_staging/* $(BOOTLOADER_ROOTFS_DIR)/lib/modules/
	cp $(BOOT_MODULE_LOAD_BASE_LIST) $(BOOTLOADER_ROOTFS_DIR)/etc/bootmodule-load.list

rootfs: $(ROOTFS_DIR)/init $(MODULE_LOAD_LIST) $(MODULE_LOAD_BASE_LIST) $(MODULE_LOAD_NORMAL_LIST)
	@echo "Creating runtime rootfs structure..."
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
	@if [ -d "$(CURDIR)/web" ]; then \
		echo "Staging web assets from $(CURDIR)/web/..."; \
		cp -a "$(CURDIR)/web/." "$(ROOTFS_DIR)/etc/www/"; \
	else \
		echo "Warning: web assets directory not found at $(CURDIR)/web"; \
	fi
	@if [ -f "$(WEB_LOGO_SVG)" ]; then \
		echo "Staging web logo svg from $(WEB_LOGO_SVG)..."; \
		cp "$(WEB_LOGO_SVG)" "$(ROOTFS_DIR)/etc/www/logo.svg"; \
	fi
	@if [ -f "$(WEB_LOGO_PNG)" ]; then \
		echo "Staging web logo png from $(WEB_LOGO_PNG)..."; \
		cp "$(WEB_LOGO_PNG)" "$(ROOTFS_DIR)/etc/www/logo.png"; \
	fi
	@if [ "$(ENABLE_ALSA)" = "1" ]; then \
		if [ -f "$(ALSA_CONF_DIR)/alsa.conf" ]; then \
			echo "Staging ALSA config from $(ALSA_CONF_DIR)..."; \
			mkdir -p "$(ROOTFS_DIR)$(ALSA_PREFIX)/share"; \
			cp -a "$(ALSA_CONF_DIR)" "$(ROOTFS_DIR)$(ALSA_PREFIX)/share/"; \
		else \
			echo "Warning: ALSA config not found at $(ALSA_CONF_DIR)"; \
		fi; \
	fi

bootloader_initramfs: bootloader_rootfs
	@echo "Packaging bootloader initramfs..."
	cd $(BOOTLOADER_ROOTFS_DIR) && \
	find . -print0 | cpio --null --create --format=newc | gzip -9 > $(INITRAMFS)

program_initramfs: rootfs
	@echo "Packaging runtime initramfs..."
	rm -rf $(PROGRAM_STAGE_DIR)
	mkdir -p $(PROGRAM_STAGE_DIR)/program
	cp -a $(ROOTFS_DIR)/. $(PROGRAM_STAGE_DIR)/program/
	cd $(PROGRAM_STAGE_DIR) && \
	find . -print0 | cpio --null --create --format=newc | gzip -9 > $(PROGRAM_INITRAMFS)

initramfs: bootloader_initramfs program_initramfs
	@echo "Success! audiox bootloader built at: $(INITRAMFS)"
	@echo "Success! audiox runtime built at: $(PROGRAM_INITRAMFS)"

$(COMBINED_INITRAMFS): initramfs
	cat $(INITRAMFS) $(PROGRAM_INITRAMFS) > $(COMBINED_INITRAMFS)

qemu: $(COMBINED_INITRAMFS)
	@echo "Launching QEMU in Raspberry Pi 4 Mode..."
	qemu-system-aarch64 \
		-M raspi4b \
		-m 2048 \
		-kernel $(OUT_DIR)/bootfiles/kernel8.img \
		-dtb $(OUT_DIR)/bootfiles/bcm2711-rpi-4-b.dtb \
		-initrd $(COMBINED_INITRAMFS) \
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
	cp $(PROGRAM_INITRAMFS) $(SD_MOUNT_BOOT)/program.cpio.gz
	@echo "initramfs initramfs.cpio.gz,program.cpio.gz followkernel" > $(SD_MOUNT_BOOT)/config.txt
	# if we enable cd start it might be a bit faster
	@echo "start_cd=1" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "boot_delay=0" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "disable_splash=1" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "dtoverlay=$(VC4_OVERLAY)" >> $(SD_MOUNT_BOOT)/config.txt
	@if [ -n "$(DSI_TOUCH_OVERLAY)" ]; then \
		echo "dtoverlay=$(DSI_TOUCH_OVERLAY)" >> $(SD_MOUNT_BOOT)/config.txt; \
	fi
	@echo "dtoverlay=dwc2,dr_mode=peripheral" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "console=fbcon console=tty1 quiet loglevel=3 rdinit=/init fbcon=map:0" > $(SD_MOUNT_BOOT)/cmdline.txt
	sync
	@echo "SD Card Flashed and ready for hardware execution!"

dev: program_initramfs
	@echo "Uploading initramfs to http://$(PI_HOST):$(PI_PORT)/api/initram ..."
	@status=$$(curl --silent --show-error --output $(OUT_DIR)/dev-upload.response --write-out "%{http_code}" -X PUT --data-binary @$(PROGRAM_INITRAMFS) http://$(PI_HOST):$(PI_PORT)/api/initram); \
	cat $(OUT_DIR)/dev-upload.response; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "HTTP $$status"; \
		exit 1; \
	fi
	@echo "Upload complete. Device will reboot once to install the staged runtime, then reboot into the new image."

image: initramfs
	@$(SCRIPTS_DIR)/build_image.sh "$(IMG_FILE)" "$(IMG_SIZE_MB)" "$(OUT_DIR)" "$(INITRAMFS)" "$(PROGRAM_INITRAMFS)" "$(VC4_OVERLAY)" "$(DSI_TOUCH_OVERLAY)"

clean:
	rm -rf $(OUT_DIR)