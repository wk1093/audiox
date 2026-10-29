CC = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
CXXFLAGS = -static -O3 -Iinclude -Wall -Wextra -Werror -Wno-error=format-truncation -Wno-error=unused-parameter -pthread -fno-exceptions -fno-rtti -std=c++17
LIBS = -lm -pthread

PKG_FETCH ?= curl -fsSL

AUDIOX_USE_SHARED_GRAPH_PROCESSOR ?= 0

# AUDIOX_QEMU=1 builds the QEMU debug variant (make debug): virtio disk
# (/dev/vda) instead of the SD card, usbip-vudc instead of the dwc2 UDC, and a
# generic arm64 kernel. Its build output goes to out/qemu; toolchain deps are
# shared with the normal build in out/.
AUDIOX_QEMU ?= 0
ifeq ($(filter 0 1,$(AUDIOX_QEMU)),)
$(error AUDIOX_QEMU must be 0 or 1, got '$(AUDIOX_QEMU)')
endif

# Recursive source discovery for upcoming C++ port.
# Sorted so build order does not depend on directory order of the checkout.
CPP_SRCS := $(sort $(shell find src -type f -name '*.cpp' 2>/dev/null))
BOOTLOADER_SRC := src/init/bootloader.cpp
RUNTIME_MAIN_SRC := src/init/init.cpp
WATCHDOG_SRC := src/init/watchdog.cpp
RUNTIME_CPP_SRCS := $(filter-out $(BOOTLOADER_SRC) $(RUNTIME_MAIN_SRC) $(WATCHDOG_SRC) src/audio/engine.cpp,$(CPP_SRCS))
ifeq ($(AUDIOX_USE_SHARED_GRAPH_PROCESSOR),0)
RUNTIME_CPP_SRCS := $(filter-out src/audio/graph_processor.cpp,$(RUNTIME_CPP_SRCS))
endif
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
ALSA_SYSROOT ?= $(SHARED_OUT_DIR)/alsa-sysroot
ALSA_PREFIX ?= /usr
ALSA_LIB_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/lib
ALSA_INCLUDE_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/include
ALSA_STATIC_LIB ?= $(ALSA_LIB_DIR)/libasound.a
ALSA_CONF_DIR ?= $(ALSA_SYSROOT)$(ALSA_PREFIX)/share/alsa

ALSA_VERSION ?= 1.2.14
ALSA_SOURCE_URL ?= https://www.alsa-project.org/files/pub/lib/alsa-lib-$(ALSA_VERSION).tar.bz2
ALSA_SRC_TARBALL ?= $(SHARED_OUT_DIR)/downloads/alsa-lib-$(ALSA_VERSION).tar.bz2
ALSA_SRC_DIR ?= $(SHARED_OUT_DIR)/build/alsa-lib-$(ALSA_VERSION)
ALSA_BUILD_DIR ?= $(SHARED_OUT_DIR)/build/alsa-lib-$(ALSA_VERSION)-build

SBC_VERSION ?= 2.2
SBC_COMMIT ?= b3deb8a5dcfb42d8c10ba1f2f1ac9bd7bf7271cc
SBC_SOURCE_URL ?= https://git.kernel.org/pub/scm/bluetooth/sbc.git
SBC_SRC_DIR ?= $(SHARED_OUT_DIR)/build/sbc-$(SBC_VERSION)-src
SBC_BUILD_DIR ?= $(SHARED_OUT_DIR)/build/sbc-$(SBC_VERSION)-build
SBC_SYSROOT ?= $(SHARED_OUT_DIR)/sbc-sysroot
SBC_STATIC_LIB ?= $(SBC_SYSROOT)/usr/lib/libsbc.a
SBC_INCLUDE_DIR ?= $(SBC_SYSROOT)/usr/include
SBC_HEADER = $(SBC_INCLUDE_DIR)/sbc/sbc.h

# Static DHCP/DNS server for the USB network (see root/etc/dnsmasq.conf).
DNSMASQ_VERSION ?= 2.93
DNSMASQ_SHA256 ?= 0c00d4e5c97c8306e5fb932b348b34269c9c29a0e7df0e8e82958b407092bc19
DNSMASQ_BIN ?= $(SHARED_OUT_DIR)/dnsmasq-$(DNSMASQ_VERSION)/dnsmasq
DNSMASQ_CONF ?= $(CURDIR)/root/etc/dnsmasq.conf
DNSMASQ_TEST_RUNNER ?= $(shell command -v qemu-aarch64 2>/dev/null)

ifeq ($(ENABLE_ALSA),1)
RUNTIME_FLAGS += -I$(ALSA_INCLUDE_DIR)
RUNTIME_LIBS += -L$(ALSA_LIB_DIR) -lasound -ldl
endif
RUNTIME_FLAGS += -I$(SBC_INCLUDE_DIR)
RUNTIME_LIBS += $(SBC_STATIC_LIB)

# SemVer version information.
# Override on the command line with e.g.:
#   make AUDIOX_VERSION_MAJOR=1 AUDIOX_VERSION_MINOR=8 AUDIOX_VERSION_PATCH=0 AUDIOX_VERSION_PRERELEASE=beta.2
# Nightly builds set AUDIOX_VERSION_NIGHTLY=1, which appends a "nightly" identifier to the
# prerelease and the commit as build metadata:
#   PRERELEASE=beta.3 -> 1.10.0-beta.3.nightly+<commit>
#   PRERELEASE=       -> 1.10.1-nightly+<commit>
# AUDIOX_VERSION_BUILD defaults to the 12-character git commit for nightly builds.
AUDIOX_VERSION_MAJOR ?= 1
AUDIOX_VERSION_MINOR ?= 10
AUDIOX_VERSION_PATCH ?= 1
AUDIOX_VERSION_PRERELEASE ?= 
AUDIOX_VERSION_NIGHTLY ?= 0
ifeq ($(filter 0 1,$(AUDIOX_VERSION_NIGHTLY)),)
$(error AUDIOX_VERSION_NIGHTLY must be 0 or 1, got '$(AUDIOX_VERSION_NIGHTLY)')
endif
ifeq ($(origin AUDIOX_VERSION_BUILD)$(AUDIOX_VERSION_NIGHTLY),undefined1)
AUDIOX_VERSION_BUILD := $(shell git -C "$(CURDIR)" rev-parse --short=12 HEAD 2>/dev/null)
endif
AUDIOX_VERSION_BUILD ?=
AUDIOX_VERSION_BASE = $(AUDIOX_VERSION_MAJOR).$(AUDIOX_VERSION_MINOR).$(AUDIOX_VERSION_PATCH)
AUDIOX_VERSION_FULL_PRERELEASE = $(if $(filter 1,$(AUDIOX_VERSION_NIGHTLY)),$(if $(AUDIOX_VERSION_PRERELEASE),$(AUDIOX_VERSION_PRERELEASE).nightly,nightly),$(AUDIOX_VERSION_PRERELEASE))
AUDIOX_VERSION_STRING = $(AUDIOX_VERSION_BASE)$(if $(AUDIOX_VERSION_FULL_PRERELEASE),-$(AUDIOX_VERSION_FULL_PRERELEASE))$(if $(AUDIOX_VERSION_BUILD),+$(AUDIOX_VERSION_BUILD))

# Kernel-style configuration.
AUDIOX_CONFIG_FILE ?= $(CURDIR)/.config
HOSTCXX ?= g++
HOST_CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
HOST_NCURSES_CFLAGS ?= $(shell pkg-config --cflags ncursesw 2>/dev/null)
HOST_NCURSES_LIBS ?= $(shell pkg-config --libs ncursesw 2>/dev/null)
HOST_AUDIO_CFLAGS ?= $(shell pkg-config --cflags alsa sndfile 2>/dev/null)
HOST_AUDIO_LIBS ?= $(shell pkg-config --libs alsa sndfile 2>/dev/null)

-include $(AUDIOX_CONFIG_FILE)

AUDIOX_SAMPLE_RATE ?= $(CONFIG_SAMPLE_RATE)
AUDIOX_BUFFER_FRAMES ?= $(CONFIG_BUFFER_FRAMES)
AUDIOX_ENABLE_ALSA_LOGS ?= $(if $(filter y,$(CONFIG_ALSA_LOGS)),1,0)
DEBUG_SHELL ?= $(if $(filter y,$(CONFIG_DEBUG_SHELL)),1,0)

# Kconfig string values are quoted in .config.
config_str = $(subst ",,$(CONFIG_$(1)))

# Board selection (see Kconfig; make pi4_defconfig / pi5_defconfig).
AUDIOX_BOARD = $(if $(filter y,$(CONFIG_BOARD_PI5)),pi5,pi4)
KERNEL_IMAGE ?= $(call config_str,KERNEL_IMAGE)
BOARD_DTBS ?= $(call config_str,BOARD_DTBS)
BOARD_FIRMWARE_FILES ?= $(call config_str,BOARD_FIRMWARE_FILES)
AUDIOX_USB_GADGET_UDC ?= $(call config_str,USB_GADGET_UDC)
AUDIOX_DEVICE_IP ?= $(call config_str,DEVICE_IP)
KERNEL_CMDLINE ?= $(call config_str,KERNEL_CMDLINE)
VC4_OVERLAY ?= $(call config_str,VC4_OVERLAY)
DSI_TOUCH_OVERLAY ?= $(call config_str,DSI_TOUCH_OVERLAY)
BOOT_OVERLAYS = $(VC4_OVERLAY) $(DSI_TOUCH_OVERLAY) dwc2 $(if $(filter y,$(CONFIG_BT_MINIUART)),miniuart-bt)

# Auto-detected from firmware after fetch_deps runs, using the uname file that
# matches the kernel image (kernel_2712.img -> uname_string_2712).
KERNEL_UNAME_FILE = uname_string$(patsubst kernel%.img,%,$(KERNEL_IMAGE))
KV = $(shell $(SCRIPTS_DIR)/detect_kernel_version.sh "$(OUT_DIR)" "6.18.37-v8+" "$(KERNEL_UNAME_FILE)")

# Paths
ARCH ?= aarch64
OUT_DIR = $(CURDIR)/out
QEMU_OUT_DIR ?= $(CURDIR)/out/qemu
ifeq ($(AUDIOX_QEMU),1)
SHARED_OUT_DIR := $(OUT_DIR)
OUT_DIR = $(QEMU_OUT_DIR)
else
SHARED_OUT_DIR = $(OUT_DIR)
endif
BOOTLOADER_ROOTFS_DIR = $(OUT_DIR)/bootloader_rootfs
ROOTFS_DIR = $(OUT_DIR)/program_rootfs
PROGRAM_STAGE_DIR = $(OUT_DIR)/program_stage
INITRAMFS = $(OUT_DIR)/initramfs.cpio.gz
PROGRAM_INITRAMFS = $(OUT_DIR)/program.cpio.gz
COMBINED_INITRAMFS = $(OUT_DIR)/combined-initramfs.cpio.gz
OBJ_DIR = $(OUT_DIR)/obj
RUNTIME_OBJ_DIR = $(OBJ_DIR)/runtime
BOOTLOADER_OBJ_DIR = $(OBJ_DIR)/bootloader
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
WEB_FAVICON_SVG ?= $(CURDIR)/logo_small.svg
WEB_FAVICON_PNG ?= $(CURDIR)/logo_small.png
WEB_INDEX_HTML ?= $(CURDIR)/web/index.html
FFMPEG_BIN ?= $(CURDIR)/third_party/ffmpeg/ffmpeg
FFMPEG_URL ?= https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/ffmpeg-n8.1.2-34-g9b6c8969e0-linuxarm64-gpl-8.1.tar.xz
FFMPEG_ARCHIVE_SHA256 ?= 177e40c91564dec3840096f3bf1ffe696b94330585972462cfc739fa29fe0e1a
FFMPEG_ARCHIVE ?= $(SHARED_OUT_DIR)/downloads/ffmpeg.pkg
FFMPEG_CROSS_LIB_DIR ?= /usr/aarch64-linux-gnu/lib
FFMPEG_RUNTIME_LIBS ?= ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libdl.so.2 librt.so.1 libpthread.so.0 libgcc_s.so.1
FFMPEG_STAGE_DIR ?= $(OUT_DIR)/ffmpeg_stage
CONFIG_MENU_BIN = $(SHARED_OUT_DIR)/host/audiox-menuconfig
CONFIG_MENU_SRC = $(SCRIPTS_DIR)/audiox-menuconfig.cpp
UPDATE_PACK_BIN = $(SHARED_OUT_DIR)/host/audiox-update-pack
UPDATE_PACK_SRC = $(SCRIPTS_DIR)/audiox-update-pack.cpp
UPDATE_ARCHIVE ?= $(OUT_DIR)/audiox-update.axu
AUDIO_BENCH_BIN = $(OUT_DIR)/host/audiox-audio-bench
AUDIO_ENGINE_TEST_BIN = $(OUT_DIR)/host/audiox-audio-engine-test
AUDIO_GRAPH_MODE_STAMP = $(OUT_DIR)/.audiox_graph_mode_$(AUDIOX_USE_SHARED_GRAPH_PROCESSOR)
AUDIO_BENCH_SRCS = \
	tools/audiox-audio-bench.cpp \
	src/audio/graph_processor.cpp \
	src/audio/engine.cpp \
	src/audio/effects/slot.cpp \
	src/audio/effects/gain.cpp \
	src/audio/effects/distortion.cpp \
	src/audio/effects/gate.cpp \
	src/audio/effects/pitch.cpp \
	src/audio/effects/reverb.cpp
BOOT_CONFIG_FILE ?= $(OUT_DIR)/dev-config.txt
BOOT_KERNEL_FILE = $(OUT_DIR)/bootfiles/$(KERNEL_IMAGE)
BOOT_CMDLINE_FILE ?= $(OUT_DIR)/dev-cmdline.txt
# Generated boot partition config.txt; kernel= points at the board's kernel file name.
BOOT_CONFIG_LINES = \
	'initramfs initramfs.cpio.gz,program.cpio.gz followkernel' \
	$(if $(filter y,$(CONFIG_BOOT_CD_FIRMWARE)),'start_cd=1') \
	'boot_delay=0' \
	'disable_splash=1' \
	'dtoverlay=$(VC4_OVERLAY)' \
	$(if $(filter y,$(CONFIG_BT_MINIUART)),'dtoverlay=miniuart-bt') \
	'enable_uart=1' \
	$(if $(filter y,$(CONFIG_BT_MINIUART)),'core_freq=250' 'core_freq_min=250') \
	$(if $(filter y,$(CONFIG_ARM_BOOST)),'arm_boost=1') \
	'krnbt=on' \
	'dtoverlay=dwc2,dr_mode=peripheral' \
	$(if $(DSI_TOUCH_OVERLAY),'dtoverlay=$(DSI_TOUCH_OVERLAY)') \
	'kernel=$(KERNEL_IMAGE)' \
	'arm_64bit=1'
BT_FIRMWARE ?= $(call config_str,BT_FIRMWARE)
BT_FIRMWARE_URL ?= https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/pios/trixie/debian/firmware/broadcom/$(BT_FIRMWARE).hcd
BT_FIRMWARE_SHA256 ?= $(call config_str,BT_FIRMWARE_SHA256)
BT_FIRMWARE_FILE ?= $(SHARED_OUT_DIR)/downloads/$(BT_FIRMWARE).hcd
FIRMWARE_GIT_URL ?= https://github.com/raspberrypi/firmware.git
FIRMWARE_GIT_REF ?= $(call config_str,FIRMWARE_GIT_REF)
PI_HOST ?= $(AUDIOX_DEVICE_IP)
PI_PORT ?= 80

SD_MOUNT_BOOT ?= /run/media/$(USER)/bootfs

IMG_FILE = $(OUT_DIR)/audiox.img
IMG_SIZE_MB = 256

# Kernel and module tree that resolve_modules.sh stages from, and the target
# that provides them.
MODULES_ROOT = $(OUT_DIR)/firmware/modules
MODULES_SOURCE_TARGET = fetch_deps

# QEMU debug VM (make debug). The Raspberry Pi kernel has no virtio drivers, so
# the VM runs the generic Debian arm64 kernel, pinned here.
QEMU_KERNEL_VERSION ?= 6.12.111+deb13-arm64
QEMU_KERNEL_PKG_VERSION ?= 6.12.111-1
QEMU_KERNEL_DEB ?= linux-image-$(QEMU_KERNEL_VERSION)-unsigned_$(QEMU_KERNEL_PKG_VERSION)_arm64.deb
QEMU_KERNEL_SHA256 ?= 3ec4284e8705fd0049d053ddead61ceeeab3530140c5599aeb2d47ac0b4b7d0f
QEMU_KERNEL_URLS ?= \
	https://deb.debian.org/debian/pool/main/l/linux/$(QEMU_KERNEL_DEB) \
	https://snapshot.debian.org/archive/debian/20261027T000000Z/pool/main/l/linux/$(QEMU_KERNEL_DEB)
QEMU_KERNEL_DIR ?= $(SHARED_OUT_DIR)/qemu-kernel/$(QEMU_KERNEL_VERSION)
QEMU_KERNEL_IMAGE = $(QEMU_KERNEL_DIR)/vmlinuz
QEMU_DISK ?= $(QEMU_OUT_DIR)/disk.img
QEMU_DISK_SIZE_MB ?= 512
QEMU_MEMORY ?= 1024
QEMU_SMP ?= 4
QEMU_HTTP_PORT ?= 8080
QEMU_USBIP_PORT ?= 3240
QEMU_USBIP_ATTACH ?= 1
QEMU_EXTRA_ARGS ?=
# QEMU display backend for make debug_ui (gtk or sdl).
QEMU_UI_DISPLAY ?= gtk
QEMU_CMDLINE ?= console=ttyAMA0 rdinit=/init loglevel=4

ifeq ($(AUDIOX_QEMU),1)
AUDIOX_BOARD = qemu
KV = $(QEMU_KERNEL_VERSION)
MODULES_ROOT = $(QEMU_KERNEL_DIR)/modules
MODULES_SOURCE_TARGET = qemu_kernel
override AUDIOX_USB_GADGET_UDC = usbip-vudc.0
endif

DEPFLAGS = -MMD -MP

RUNTIME_DEFINES = \
	-DKERNEL_VERSION='"$(KV)"' \
	-DAUDIOX_USE_SHARED_GRAPH_PROCESSOR=$(AUDIOX_USE_SHARED_GRAPH_PROCESSOR) \
	-DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) \
	-DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) \
	-DAUDIOX_ENABLE_ALSA_LOGS=$(AUDIOX_ENABLE_ALSA_LOGS) \
	-DAUDIOX_VERSION_MAJOR=$(AUDIOX_VERSION_MAJOR) \
	-DAUDIOX_VERSION_MINOR=$(AUDIOX_VERSION_MINOR) \
	-DAUDIOX_VERSION_PATCH=$(AUDIOX_VERSION_PATCH) \
	-DAUDIOX_VERSION_PRERELEASE='"$(AUDIOX_VERSION_FULL_PRERELEASE)"' \
	-DAUDIOX_VERSION_BUILD='"$(AUDIOX_VERSION_BUILD)"' \
	-DAUDIOX_VERSION_STRING='"$(AUDIOX_VERSION_STRING)"' \
	-DAUDIOX_USB_GADGET_UDC='"$(AUDIOX_USB_GADGET_UDC)"' \
	-DAUDIOX_USB_NET_SERVER_IP='"$(AUDIOX_DEVICE_IP)"' \
	-DAUDIOX_BOARD='"$(AUDIOX_BOARD)"' \
	-DAUDIOX_QEMU=$(AUDIOX_QEMU) \
	-DDEBUG_SHELL=$(DEBUG_SHELL)

BOOTLOADER_DEFINES = -DKERNEL_VERSION='"$(KV)"' -DAUDIOX_QEMU=$(AUDIOX_QEMU)

RUNTIME_OBJS := $(patsubst src/%.cpp,$(RUNTIME_OBJ_DIR)/%.o,$(RUNTIME_CPP_SRCS))
RUNTIME_MAIN_OBJ := $(patsubst src/%.cpp,$(RUNTIME_OBJ_DIR)/%.o,$(RUNTIME_MAIN_SRC))
WATCHDOG_OBJ := $(patsubst src/%.cpp,$(RUNTIME_OBJ_DIR)/%.o,$(WATCHDOG_SRC))
WATCHDOG_HOST_BIN := $(OUT_DIR)/host/audiox-watchdog-test
WATCHDOG_HUNG_CHILD_BIN := $(OUT_DIR)/host/audiox-watchdog-hung-child
BOOTLOADER_OBJS := $(patsubst src/%.cpp,$(BOOTLOADER_OBJ_DIR)/%.o,$(BOOTLOADER_SRCS))
RUNTIME_DEPS_FILES := $(RUNTIME_OBJS:.o=.d)
BOOTLOADER_DEPS_FILES := $(BOOTLOADER_OBJS:.o=.d)


.DEFAULT_GOAL := all

.PHONY: all clean rootfs bootloader_rootfs program_initramfs bootloader_initramfs initramfs fetch_deps fetch_modules fetch_boot_modules show_modules show_kernel qemu fancyexport export image dev alsa alsa_source show_alsa sbc ffmpeg upload_ffmpeg upload_bootloader upload_kernel upload_cmdline update_archive upload_update boot_config stage_ffmpeg bluetooth_firmware dnsmasq check_dnsmasq_conf test-dnsmasq-config config menuconfig defconfig pi4_defconfig pi5_defconfig olddefconfig print-config print-version audio-bench run-audio-bench test-audio-engine test-watchdog test-resolve-modules debug debug_ui debug-attach debug-reset-disk debug-clean qemu_kernel qemu_disk qemu_debug qemu_require_debug_build

all: initramfs

config: menuconfig

archive: update_archive

menuconfig: $(CONFIG_MENU_BIN)
	@$(CONFIG_MENU_BIN) "$(AUDIOX_CONFIG_FILE)" "$(CURDIR)/Kconfig"

defconfig: pi4_defconfig

pi4_defconfig pi5_defconfig: $(CONFIG_MENU_BIN)
	@$(CONFIG_MENU_BIN) --defconfig "$(CURDIR)/Kconfig" "$(AUDIOX_CONFIG_FILE)" BOARD_$(shell echo "$(@:_defconfig=)" | tr a-z A-Z)
	@echo "Default $(@:_defconfig=) audiox configuration written to $(AUDIOX_CONFIG_FILE)"

olddefconfig: $(CONFIG_MENU_BIN)
	@$(CONFIG_MENU_BIN) --olddefconfig "$(CURDIR)/Kconfig" "$(AUDIOX_CONFIG_FILE)"

print-config:
	@cat "$(AUDIOX_CONFIG_FILE)"

# Keeps existing choices and fills in options added to Kconfig since the last run.
$(AUDIOX_CONFIG_FILE): $(CONFIG_MENU_BIN) $(CURDIR)/Kconfig
	@$(CONFIG_MENU_BIN) --olddefconfig "$(CURDIR)/Kconfig" "$(AUDIOX_CONFIG_FILE)"

$(RUNTIME_OBJS) $(RUNTIME_MAIN_OBJ) $(WATCHDOG_OBJ) $(BOOTLOADER_OBJS): $(AUDIOX_CONFIG_FILE)

$(AUDIO_GRAPH_MODE_STAMP):
	@mkdir -p "$(OUT_DIR)"
	@rm -f "$(OUT_DIR)"/.audiox_graph_mode_*
	@touch "$@"

$(RUNTIME_OBJ_DIR)/audio/processing.o: $(AUDIO_GRAPH_MODE_STAMP)

$(CONFIG_MENU_BIN): $(CONFIG_MENU_SRC)
	@mkdir -p "$(dir $@)"
	@command -v "$(HOSTCXX)" >/dev/null || { echo "Host C++ compiler not found: $(HOSTCXX)"; exit 1; }
	@pkg-config --exists ncursesw || { echo "ncurses development files are required (pkg-config ncursesw)"; exit 1; }
	$(HOSTCXX) $(HOST_CXXFLAGS) $(HOST_NCURSES_CFLAGS) -o "$@" "$<" $(HOST_NCURSES_LIBS)

audio-bench: $(AUDIO_BENCH_BIN)

run-audio-bench: $(AUDIO_BENCH_BIN)
	@"$(AUDIO_BENCH_BIN)" "$(if $(BENCH_INPUT),$(BENCH_INPUT),-)" "$(BENCH_ALSA_DEVICE)" "$(BENCH_PORT)"

test-audio-engine: $(AUDIO_ENGINE_TEST_BIN)
	@$(AUDIO_ENGINE_TEST_BIN)

.PHONY: test-soundboard-config
test-soundboard-config: $(OUT_DIR)/host/audiox-soundboard-config-test
	@"$<"

SOUNDBOARD_CONFIG_TEST_SRCS = tests/soundboard_config_test.cpp src/context.cpp \
	src/config/basic.cpp src/config/midi_map.cpp src/config/backup.cpp \
	src/midi/midi.cpp src/audio/setup.cpp src/audio/gadget_volume.cpp src/http/server.cpp src/init/recovery.cpp src/touch/touch.cpp \
	$(filter-out tools/audiox-audio-bench.cpp src/audio/engine.cpp src/audio/graph_processor.cpp,$(AUDIO_BENCH_SRCS))

$(OUT_DIR)/host/audiox-soundboard-config-test: $(SOUNDBOARD_CONFIG_TEST_SRCS) src/http/api.cpp $(wildcard include/config/*.hpp include/midi/*.hpp include/audio/*.hpp include/audio/effects/*.hpp include/http/*.hpp include/touch/*.hpp) include/defs.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Wno-unused-parameter -Iinclude \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o "$@" $(SOUNDBOARD_CONFIG_TEST_SRCS) \
		$(shell pkg-config --cflags --libs alsa) -pthread -lm

.PHONY: test-boot-recovery
test-boot-recovery: $(OUT_DIR)/host/audiox-boot-recovery-test
	@"$<"

$(OUT_DIR)/host/audiox-boot-recovery-test: tests/boot_recovery_test.cpp include/boot_recovery.hpp include/defs.hpp include/update_archive.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<"

.PHONY: test-config-backup
test-config-backup: $(OUT_DIR)/host/audiox-config-backup-test $(OUT_DIR)/host/audiox-config-backup-http-test
	@"$(OUT_DIR)/host/audiox-config-backup-test"
	@"$(OUT_DIR)/host/audiox-config-backup-http-test"

$(OUT_DIR)/host/audiox-config-backup-test: tests/config_backup_test.cpp src/config/backup.cpp include/config/backup.hpp include/update_archive.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/config_backup_test.cpp src/config/backup.cpp

$(OUT_DIR)/host/audiox-config-backup-http-test: tests/config_backup_http_test.cpp src/config/backup.cpp src/http/server.cpp include/config/backup.hpp include/http/context.hpp include/update_archive.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Wno-unused-parameter -Iinclude \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/config_backup_http_test.cpp src/config/backup.cpp -pthread

.PHONY: test-audio-pcm-recover
test-audio-pcm-recover: $(OUT_DIR)/host/audiox-audio-pcm-recover-test
	@"$<"

.PHONY: test-gadget-volume test-diagnostics test-mdns
.PHONY: test-system
test-system: $(OUT_DIR)/host/audiox-system-test
	@"$<"

$(OUT_DIR)/host/audiox-system-test: tests/system_test.cpp include/system.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<"

.PHONY: test-gadget-setup
test-gadget-setup: $(OUT_DIR)/host/audiox-gadget-setup-test
	@"$<"

$(OUT_DIR)/host/audiox-gadget-setup-test: tests/gadget_setup_test.cpp src/init/gadget.cpp $(wildcard include/audio/*.hpp include/config/*.hpp) include/context.hpp include/init.hpp include/system.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-parameter -Iinclude \
		-DGADGET_ROOT='"gadget"' -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" "$<" $(shell pkg-config --cflags alsa)

test-mdns: $(OUT_DIR)/host/audiox-mdns-test
	@"$<"

$(OUT_DIR)/host/audiox-mdns-test: tests/mdns_test.cpp src/http/mdns.cpp include/http/mdns.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -fsanitize=address,undefined -Iinclude -o "$@" tests/mdns_test.cpp src/http/mdns.cpp

test-gadget-volume: $(OUT_DIR)/host/audiox-gadget-volume-test-0 $(OUT_DIR)/host/audiox-gadget-volume-test-1
	@"$(OUT_DIR)/host/audiox-gadget-volume-test-0"
	@"$(OUT_DIR)/host/audiox-gadget-volume-test-1"

$(OUT_DIR)/host/audiox-gadget-volume-test-%: tests/gadget_volume_test.cpp src/audio/processing.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp $(wildcard include/audio/*.hpp include/audio/*.h) $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter \
		-Iinclude -DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) -DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) -DAUDIOX_USE_SHARED_GRAPH_PROCESSOR=$* \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o "$@" \
		tests/gadget_volume_test.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp $(shell pkg-config --cflags --libs alsa) -pthread -lm

test-diagnostics: $(OUT_DIR)/host/audiox-diagnostics-test
	@"$<"

$(OUT_DIR)/host/audiox-diagnostics-test: tests/diagnostics_test.cpp src/init/diagnostics.cpp include/diagnostics.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/diagnostics_test.cpp

$(OUT_DIR)/host/audiox-audio-pcm-recover-test: tests/audio_pcm_recover_test.cpp include/audio/alsa_pcm.h include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-parameter -Iinclude $(shell pkg-config --cflags alsa) -o "$@" "$<" $(shell pkg-config --libs alsa)

test-watchdog: $(WATCHDOG_HOST_BIN)
test-watchdog: $(WATCHDOG_HOST_BIN) $(WATCHDOG_HUNG_CHILD_BIN)
	@"$(WATCHDOG_HOST_BIN)" /bin/false 2 "" > "$(OUT_DIR)/watchdog-crash-test.log" 2>&1
	@grep -q "main process exited" "$(OUT_DIR)/watchdog-crash-test.log"
	@"$(WATCHDOG_HOST_BIN)" "$(WATCHDOG_HUNG_CHILD_BIN)" 1 "" > "$(OUT_DIR)/watchdog-hang-test.log" 2>&1
	@grep -q "main-loop heartbeat timed out" "$(OUT_DIR)/watchdog-hang-test.log"
	@echo "watchdog exit and heartbeat-timeout tests passed"
	@bash tests/watchdog_service_test.sh "$(WATCHDOG_HOST_BIN)" "$(WATCHDOG_HUNG_CHILD_BIN)" "$(OUT_DIR)/watchdog-service-test"

test-resolve-modules:
	@bash tests/resolve_modules_test.sh

BENCH_INPUT ?=
BENCH_ALSA_DEVICE ?= default
BENCH_PORT ?= 8765

$(AUDIO_BENCH_BIN): $(AUDIO_BENCH_SRCS) $(AUDIOX_CONFIG_FILE) include/audio/pcm_convert.hpp include/audio/engine.hpp include/audio/graph_processor.hpp include/audio/effects/slot.hpp
	@mkdir -p "$(dir $@)"
	@command -v "$(HOSTCXX)" >/dev/null || { echo "Host C++ compiler not found: $(HOSTCXX)"; exit 1; }
	@pkg-config --exists alsa sndfile || { echo "Host ALSA and libsndfile development files are required (pkg-config alsa sndfile)"; exit 1; }
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude $(HOST_AUDIO_CFLAGS) \
		-DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) \
		-DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) \
		-DAUDIOX_BENCH_HTML='"$(CURDIR)/web/audio-bench.html"' \
		-o "$@" $(AUDIO_BENCH_SRCS) $(HOST_AUDIO_LIBS) -pthread -lm

$(AUDIO_ENGINE_TEST_BIN): tests/audio_engine_test.cpp $(filter-out tools/audiox-audio-bench.cpp,$(AUDIO_BENCH_SRCS)) include/audio/pcm_convert.hpp include/audio/engine.hpp include/audio/graph_processor.hpp $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) \
		-DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) -o "$@" tests/audio_engine_test.cpp \
		$(filter-out tools/audiox-audio-bench.cpp,$(AUDIO_BENCH_SRCS)) -pthread -lm

$(WATCHDOG_HOST_BIN): $(WATCHDOG_SRC) include/boot_recovery.hpp include/update_archive.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -DAUDIOX_WATCHDOG_STARTUP_TIMEOUT_MS=3000 \
		-DAUDIOX_WATCHDOG_HEARTBEAT_TIMEOUT_MS=1200 -o "$@" "$<"

$(WATCHDOG_HUNG_CHILD_BIN): tests/watchdog_hung_child.cpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -o "$@" "$<"

fetch_deps:
	@mkdir -p $(OUT_DIR)
	@$(SCRIPTS_DIR)/sync_firmware.sh "$(FIRMWARE_GIT_URL)" "$(FIRMWARE_GIT_REF)" "$(OUT_DIR)" "$(KERNEL_IMAGE)" "$(BOARD_DTBS)" "$(BOARD_FIRMWARE_FILES)" "$(BOOT_OVERLAYS)"

fetch_modules: $(MODULES_SOURCE_TARGET)
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(DEP_FILE)" "module-load" "modules_staging" "$(AUDIOX_BOARD)" "$(MODULES_ROOT)"

fetch_boot_modules: $(MODULES_SOURCE_TARGET)
	@$(SCRIPTS_DIR)/resolve_modules.sh "$(KV)" "$(OUT_DIR)" "$(BOOT_DEP_FILE)" "bootmodule-load" "bootmodules_staging" "$(AUDIOX_BOARD)" "$(MODULES_ROOT)"

qemu_kernel:
	@$(SCRIPTS_DIR)/fetch_qemu_kernel.sh "$(QEMU_KERNEL_DIR)" "$(QEMU_KERNEL_VERSION)" \
		"$(SHARED_OUT_DIR)/downloads/$(QEMU_KERNEL_DEB)" "$(QEMU_KERNEL_SHA256)" $(QEMU_KERNEL_URLS)

show_modules: fetch_modules
	@cat $(OUT_DIR)/module-load.summary.txt

print-version:
	@printf '%s\n' "$(AUDIOX_VERSION_STRING)"

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
	@mkdir -p "$(dir $(ALSA_SRC_TARBALL))" "$(dir $(ALSA_SRC_DIR))" "$(ALSA_SYSROOT)"
	@[ -f "$(ALSA_SRC_TARBALL)" ] || $(PKG_FETCH) "$(ALSA_SOURCE_URL)" -o "$(ALSA_SRC_TARBALL)"
	rm -rf "$(ALSA_SRC_DIR)" "$(ALSA_BUILD_DIR)"
	mkdir -p "$(ALSA_SRC_DIR)" "$(ALSA_BUILD_DIR)"
	tar -xjf "$(ALSA_SRC_TARBALL)" -C "$(dir $(ALSA_SRC_DIR))"
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

sbc: $(SBC_STATIC_LIB) $(SBC_HEADER)

# `make install` writes the library before the headers, so track both; an
# interrupted install then rebuilds instead of leaving sbc.h missing.
$(SBC_STATIC_LIB) $(SBC_HEADER) &:
	@mkdir -p "$(dir $(SBC_SRC_DIR))" "$(SBC_SYSROOT)"
	@if [ ! -d "$(SBC_SRC_DIR)/.git" ]; then \
		git clone --no-checkout "$(SBC_SOURCE_URL)" "$(SBC_SRC_DIR)"; \
	fi
	@git -C "$(SBC_SRC_DIR)" fetch --depth 1 origin "$(SBC_COMMIT)"
	@git -C "$(SBC_SRC_DIR)" checkout --detach "$(SBC_COMMIT)"
	@# Upstream ./bootstrap symlinks build-aux into the host automake, which
	@# breaks after automake upgrades; copy the files and redo it if dangling.
	@if [ ! -x "$(SBC_SRC_DIR)/configure" ] || [ ! -e "$(SBC_SRC_DIR)/build-aux/install-sh" ]; then \
		cd "$(SBC_SRC_DIR)" && rm -rf build-aux autom4te.cache && mkdir build-aux && autoreconf --force --install; \
	fi
	@mkdir -p "$(SBC_BUILD_DIR)"
	cd "$(SBC_BUILD_DIR)" && "$(SBC_SRC_DIR)/configure" \
		--host=aarch64-linux-gnu \
		--prefix=/usr \
		--libdir=/usr/lib \
		--disable-shared \
		--enable-static \
		--disable-tools \
		--disable-tester \
		CC="$(CC)"
	$(MAKE) -C "$(SBC_BUILD_DIR)" -j$$(nproc)
	$(MAKE) -C "$(SBC_BUILD_DIR)" DESTDIR="$(SBC_SYSROOT)" install
	@test -f "$(SBC_STATIC_LIB)" && test -f "$(SBC_HEADER)" || { echo "SBC install is missing $(SBC_STATIC_LIB) or $(SBC_HEADER)"; exit 1; }
	@touch "$(SBC_STATIC_LIB)" "$(SBC_HEADER)"
	@echo "Static SBC decoder ready: $(SBC_STATIC_LIB)"

dnsmasq: $(DNSMASQ_BIN)

$(DNSMASQ_BIN): scripts/build_dnsmasq.sh
	@"$(CURDIR)/scripts/build_dnsmasq.sh" "$(DNSMASQ_VERSION)" "$(DNSMASQ_SHA256)" "$(SHARED_OUT_DIR)" "$@" "$(CC)" "$(PKG_FETCH)"

# The static dnsmasq.conf must agree with CONFIG_DEVICE_IP (a /24 on usb0).
check_dnsmasq_conf:
	@ip="$(AUDIOX_DEVICE_IP)"; net="$${ip%.*}"; conf="$(DNSMASQ_CONF)"; \
	test -f "$$conf" || { echo "Error: missing $$conf"; exit 1; }; \
	grep -qx "host-record=audiox,audiox.lan,audiox.local,audiox.wklab.net,$$ip" "$$conf" && \
	grep -qx "dhcp-option=option:dns-server,$$ip" "$$conf" && \
	grep -q "^dhcp-range=$$net\.[0-9]*,$$net\.[0-9]*,255\.255\.255\.0," "$$conf" || { \
		echo "Error: $$conf does not match CONFIG_DEVICE_IP=$$ip"; \
		echo "Update host-record, dns-server, and the /24 dhcp-range in $$conf."; \
		exit 1; \
	}

test-dnsmasq-config: $(DNSMASQ_BIN) check_dnsmasq_conf
	@if [ -n "$(DNSMASQ_TEST_RUNNER)" ]; then \
		"$(DNSMASQ_TEST_RUNNER)" "$(DNSMASQ_BIN)" --test -C "$(DNSMASQ_CONF)"; \
	elif command -v dnsmasq >/dev/null 2>&1; then \
		echo "qemu-aarch64 not found; checking syntax with host dnsmasq"; \
		dnsmasq --test -C "$(DNSMASQ_CONF)"; \
	else \
		echo "Error: need qemu-aarch64 or host dnsmasq to check $(DNSMASQ_CONF)"; \
		exit 1; \
	fi

ffmpeg:
	@if [ -x "$(FFMPEG_BIN)" ]; then \
		echo "Using existing ffmpeg at $(FFMPEG_BIN)"; \
	elif [ -n "$(FFMPEG_URL)" ]; then \
		echo "Fetching ffmpeg from $(FFMPEG_URL)..."; \
		mkdir -p "$(dir $(FFMPEG_BIN))" "$(dir $(FFMPEG_ARCHIVE))"; \
		tmpdir=$$(mktemp -d); \
		if ! $(PKG_FETCH) "$(FFMPEG_URL)" -o "$(FFMPEG_ARCHIVE)"; then \
			rm -rf "$$tmpdir"; \
			echo "Error: failed to download ffmpeg from $(FFMPEG_URL)"; \
			exit 1; \
		fi; \
		if [ -n "$(FFMPEG_ARCHIVE_SHA256)" ]; then \
			actual_sha256=$$(sha256sum "$(FFMPEG_ARCHIVE)" | awk '{print $$1}'); \
			if [ "$$actual_sha256" != "$(FFMPEG_ARCHIVE_SHA256)" ]; then \
				rm -rf "$$tmpdir"; \
				rm -f "$(FFMPEG_ARCHIVE)"; \
				echo "Error: ffmpeg archive checksum mismatch"; \
				echo "Expected: $(FFMPEG_ARCHIVE_SHA256)"; \
				echo "Actual:   $$actual_sha256"; \
				exit 1; \
			fi; \
			echo "Verified ffmpeg archive checksum: $$actual_sha256"; \
		fi; \
		if tar -tf "$(FFMPEG_ARCHIVE)" >/dev/null 2>&1; then \
			if ! tar -xf "$(FFMPEG_ARCHIVE)" -C "$$tmpdir"; then \
				rm -rf "$$tmpdir"; \
				echo "Error: failed to extract ffmpeg archive $(FFMPEG_ARCHIVE)"; \
				exit 1; \
			fi; \
			found=$$(find "$$tmpdir" -type f -name ffmpeg | head -n 1); \
			if [ -z "$$found" ]; then \
				rm -rf "$$tmpdir"; \
				echo "Error: archive does not contain an ffmpeg binary"; \
				exit 1; \
			fi; \
			cp "$$found" "$(FFMPEG_BIN)"; \
		else \
			cp "$(FFMPEG_ARCHIVE)" "$(FFMPEG_BIN)"; \
		fi; \
		rm -rf "$$tmpdir"; \
		chmod 0755 "$(FFMPEG_BIN)"; \
		echo "Fetched ffmpeg to $(FFMPEG_BIN)"; \
	else \
		echo "Warning: ffmpeg missing at $(FFMPEG_BIN). Set FFMPEG_URL to auto-fetch or place binary manually."; \
	fi

bluetooth_firmware: $(BT_FIRMWARE_FILE)

$(BT_FIRMWARE_FILE):
	@mkdir -p "$(dir $@)"
	@echo "Fetching Raspberry Pi Bluetooth firmware..."
	@$(PKG_FETCH) "$(BT_FIRMWARE_URL)" -o "$@.tmp"
	@if [ -n "$(BT_FIRMWARE_SHA256)" ]; then \
		test "$$(sha256sum "$@.tmp" | awk '{print $$1}')" = "$(BT_FIRMWARE_SHA256)" || { echo "Bluetooth firmware checksum mismatch"; rm -f "$@.tmp"; exit 1; }; \
	fi
	@mv "$@.tmp" "$@"


$(BOOTLOADER_OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(BOOTLOADER_COMPILER) $(BOOTLOADER_FLAGS) $(BOOTLOADER_DEFINES) $(DEPFLAGS) -c -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(RUNTIME_COMPILER) $(RUNTIME_FLAGS) $(RUNTIME_DEFINES) $(DEPFLAGS) -c -o $@ $<

# Runtime objects include ALSA and SBC headers (bluetooth/context.hpp pulls in
# sbc/sbc.h), so the sysroots must exist before any of them compile.
$(RUNTIME_OBJS) $(RUNTIME_MAIN_OBJ): | $(RUNTIME_DEPS) $(SBC_STATIC_LIB) $(SBC_HEADER)

$(BOOTLOADER_ROOTFS_DIR)/init: $(BOOTLOADER_OBJS)
	@echo "Compiling bootloader init..."
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)
	$(BOOTLOADER_COMPILER) $(BOOTLOADER_FLAGS) \
		-o $(BOOTLOADER_ROOTFS_DIR)/init $(BOOTLOADER_OBJS) $(LIBS)


$(ROOTFS_DIR)/init: $(WATCHDOG_OBJ)
	@echo "Compiling PID 1 watchdog..."
	mkdir -p $(ROOTFS_DIR)
	$(RUNTIME_COMPILER) $(RUNTIME_FLAGS) \
		-o $(ROOTFS_DIR)/init $(WATCHDOG_OBJ) $(LIBS)

$(ROOTFS_DIR)/sbin/audiox-main: $(RUNTIME_DEPS) $(RUNTIME_OBJS) $(RUNTIME_MAIN_OBJ) $(SBC_STATIC_LIB)
	@echo "Compiling audiox main process..."
	mkdir -p $(dir $@)
	$(RUNTIME_COMPILER) $(RUNTIME_FLAGS) \
		-o $@ $(RUNTIME_OBJS) $(RUNTIME_MAIN_OBJ) $(RUNTIME_LIBS)

bootloader_rootfs: $(BOOTLOADER_ROOTFS_DIR)/init $(BOOT_MODULE_LOAD_LIST) $(BOOT_MODULE_LOAD_BASE_LIST)
	@echo "Creating bootloader rootfs structure..."
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/etc $(BOOTLOADER_ROOTFS_DIR)/lib/modules
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/proc $(BOOTLOADER_ROOTFS_DIR)/sys $(BOOTLOADER_ROOTFS_DIR)/dev
	mkdir -p $(BOOTLOADER_ROOTFS_DIR)/mnt/boot $(BOOTLOADER_ROOTFS_DIR)/mnt/rootfs
	rm -rf $(BOOTLOADER_ROOTFS_DIR)/lib/modules/*
	cp -r "$(OUT_DIR)/bootmodules_staging/$(KV)" $(BOOTLOADER_ROOTFS_DIR)/lib/modules/
	cp $(BOOT_MODULE_LOAD_BASE_LIST) $(BOOTLOADER_ROOTFS_DIR)/etc/bootmodule-load.list

rootfs: $(ROOTFS_DIR)/init $(ROOTFS_DIR)/sbin/audiox-main $(MODULE_LOAD_LIST) $(MODULE_LOAD_BASE_LIST) $(MODULE_LOAD_NORMAL_LIST) bluetooth_firmware $(SBC_STATIC_LIB) $(DNSMASQ_BIN) check_dnsmasq_conf
	@echo "Creating runtime rootfs structure..."
	mkdir -p $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/sbin $(ROOTFS_DIR)/etc
	mkdir -p $(ROOTFS_DIR)/proc $(ROOTFS_DIR)/sys $(ROOTFS_DIR)/dev
	mkdir -p $(ROOTFS_DIR)/etc/www
	mkdir -p $(ROOTFS_DIR)/lib/modules
	mkdir -p $(ROOTFS_DIR)/lib/firmware/brcm
	rm -f $(ROOTFS_DIR)/lib/firmware/brcm/*.hcd
	cp "$(BT_FIRMWARE_FILE)" "$(ROOTFS_DIR)/lib/firmware/brcm/$(BT_FIRMWARE).hcd"
	@echo "Staging glibc runtime for /audiox/ffmpeg from $(FFMPEG_CROSS_LIB_DIR)..."
	@for lib in $(FFMPEG_RUNTIME_LIBS); do \
		if [ ! -e "$(FFMPEG_CROSS_LIB_DIR)/$$lib" ]; then \
			echo "Error: missing $$lib in $(FFMPEG_CROSS_LIB_DIR)"; \
			exit 1; \
		fi; \
		cp -Lf "$(FFMPEG_CROSS_LIB_DIR)/$$lib" "$(ROOTFS_DIR)/lib/$$lib"; \
	done
	
	@echo "Staging root folder contents..."
	@if [ -d "$(CURDIR)/root" ]; then \
		cp -a $(CURDIR)/root/* $(ROOTFS_DIR)/; \
	fi
	
	@echo "Staging kernel objects into target lib tree..."
	@mkdir -p "$(ROOTFS_DIR)/usr/share/licenses/audiox/libsbc"
	cp "$(SBC_SRC_DIR)/COPYING.LIB" "$(ROOTFS_DIR)/usr/share/licenses/audiox/libsbc/COPYING.LIB"
	@echo "Staging static dnsmasq $(DNSMASQ_VERSION)..."
	install -m 0755 "$(DNSMASQ_BIN)" "$(ROOTFS_DIR)/sbin/dnsmasq"
	@mkdir -p "$(ROOTFS_DIR)/usr/share/licenses/audiox/dnsmasq"
	cp "$(dir $(DNSMASQ_BIN))COPYING" "$(dir $(DNSMASQ_BIN))COPYING-v3" \
		"$(ROOTFS_DIR)/usr/share/licenses/audiox/dnsmasq/"
	rm -rf $(ROOTFS_DIR)/lib/modules/*
	cp -r "$(OUT_DIR)/modules_staging/$(KV)" $(ROOTFS_DIR)/lib/modules/
	cp $(MODULE_LOAD_LIST) $(ROOTFS_DIR)/etc/module-load.list
	cp $(MODULE_LOAD_BASE_LIST) $(ROOTFS_DIR)/etc/module-load.base.list
	cp $(MODULE_LOAD_NORMAL_LIST) $(ROOTFS_DIR)/etc/module-load.normal.list
	@echo "$(AUDIOX_VERSION_STRING) (built $$(date -u +%Y-%m-%dT%H:%M:%SZ))" > "$(ROOTFS_DIR)/etc/audiox-version"
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
	@if [ -f "$(WEB_FAVICON_SVG)" ]; then \
		echo "Staging web favicon svg from $(WEB_FAVICON_SVG)..."; \
		cp "$(WEB_FAVICON_SVG)" "$(ROOTFS_DIR)/etc/www/logo_small.svg"; \
	else \
		echo "Warning: web favicon svg not found at $(WEB_FAVICON_SVG)"; \
	fi
	@if [ -f "$(WEB_FAVICON_PNG)" ]; then \
		echo "Staging web favicon png from $(WEB_FAVICON_PNG)..."; \
		cp "$(WEB_FAVICON_PNG)" "$(ROOTFS_DIR)/etc/www/logo_small.png"; \
	else \
		echo "Warning: web favicon png not found at $(WEB_FAVICON_PNG)"; \
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

stage_ffmpeg: ffmpeg
	@echo "Staging ffmpeg for rootfs deployment..."
	@if [ -f "$(FFMPEG_BIN)" ]; then \
		mkdir -p "$(FFMPEG_STAGE_DIR)"; \
		cp "$(FFMPEG_BIN)" "$(FFMPEG_STAGE_DIR)/ffmpeg"; \
		chmod 0755 "$(FFMPEG_STAGE_DIR)/ffmpeg"; \
		echo "ffmpeg runtime libraries are staged by the rootfs target."; \
	else \
		echo "Warning: ffmpeg binary not found at $(FFMPEG_BIN) (ffmpeg deployment disabled)"; \
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
	@if [ "$(CONFIG_BOARD_PI4)" != "y" ]; then echo "qemu only emulates the Raspberry Pi 4; run make pi4_defconfig first."; exit 1; fi
	@echo "Launching QEMU in Raspberry Pi 4 Mode..."
	qemu-system-aarch64 \
		-M raspi4b \
		-m 2048 \
		-kernel $(BOOT_KERNEL_FILE) \
		-dtb $(OUT_DIR)/bootfiles/bcm2711-rpi-4-b.dtb \
		-initrd $(COMBINED_INITRAMFS) \
		-append "console=fbcon console=tty1 quiet loglevel=3 rdinit=/init" \
		-display sdl,gl=on

# QEMU debug mode: boots the AUDIOX_QEMU=1 build on a generic arm64 VM with a
# serial console (quit with Ctrl-A X), a persistent virtio disk, the web UI on
# http://127.0.0.1:$(QEMU_HTTP_PORT)/ and the USB gadget exported over USB/IP.
debug:
	@$(MAKE) --no-print-directory AUDIOX_QEMU=1 qemu_debug

# Same as debug, plus a window showing the 800x480 framebuffer.
debug_ui:
	@$(MAKE) --no-print-directory AUDIOX_QEMU=1 QEMU_EXTRA_ARGS="-display $(QEMU_UI_DISPLAY) $(QEMU_EXTRA_ARGS)" qemu_debug

debug-attach:
	@$(SCRIPTS_DIR)/qemu_usbip_attach.sh 127.0.0.1 "$(QEMU_USBIP_PORT)" usbip-vudc.0 30

debug-reset-disk:
	rm -f "$(QEMU_DISK)"

debug-clean:
	rm -rf "$(QEMU_OUT_DIR)"

qemu_require_debug_build:
	@if [ "$(AUDIOX_QEMU)" != "1" ]; then echo "This target needs AUDIOX_QEMU=1 (use make debug)."; exit 1; fi

qemu_disk: qemu_require_debug_build initramfs $(BOOT_CONFIG_FILE) $(BOOT_CMDLINE_FILE)
	@$(SCRIPTS_DIR)/build_qemu_disk.sh "$(QEMU_DISK)" "$(QEMU_DISK_SIZE_MB)" "$(INITRAMFS)" "$(PROGRAM_INITRAMFS)" \
		"$(BOOT_CONFIG_FILE)" "$(BOOT_CMDLINE_FILE)" "$(FFMPEG_BIN)"

qemu_debug: qemu_require_debug_build qemu_kernel qemu_disk $(COMBINED_INITRAMFS)
	@QEMU_KERNEL="$(QEMU_KERNEL_IMAGE)" QEMU_INITRD="$(COMBINED_INITRAMFS)" QEMU_DISK="$(QEMU_DISK)" \
		QEMU_CMDLINE="$(QEMU_CMDLINE)" QEMU_MEMORY="$(QEMU_MEMORY)" QEMU_SMP="$(QEMU_SMP)" \
		QEMU_HTTP_PORT="$(QEMU_HTTP_PORT)" QEMU_USBIP_PORT="$(QEMU_USBIP_PORT)" \
		QEMU_USBIP_ATTACH="$(QEMU_USBIP_ATTACH)" QEMU_EXTRA_ARGS="$(QEMU_EXTRA_ARGS)" AUDIOX_DEVICE_IP="$(AUDIOX_DEVICE_IP)" \
		$(SCRIPTS_DIR)/qemu_debug.sh

upload_ffmpeg: stage_ffmpeg
	@if [ ! -f "$(FFMPEG_STAGE_DIR)/ffmpeg" ]; then \
		echo "Error: ffmpeg binary not found at $(FFMPEG_STAGE_DIR)/ffmpeg"; \
		exit 1; \
	fi
	@echo "Uploading ffmpeg to device at $(PI_HOST):$(PI_PORT)..."
	curl -X PUT \
		--data-binary @$(FFMPEG_STAGE_DIR)/ffmpeg \
		http://$(PI_HOST):$(PI_PORT)/api/rootfs/ffmpeg
	@echo "ffmpeg uploaded successfully."

upload_bootloader: bootloader_initramfs
	@echo "Uploading bootloader to http://$(PI_HOST):$(PI_PORT)/api/boot/loader ..."
	@mkdir -p "$(OUT_DIR)"
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/upload-bootloader.response" --write-out "%{http_code}" -X PUT --data-binary @"$(INITRAMFS)" "http://$(PI_HOST):$(PI_PORT)/api/boot/loader") || { cat "$(OUT_DIR)/upload-bootloader.response"; exit 1; }; \
	cat "$(OUT_DIR)/upload-bootloader.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Bootloader upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Bootloader upload complete."

upload_kernel: fetch_deps
	@if [ ! -f "$(BOOT_KERNEL_FILE)" ]; then \
		echo "Error: kernel image not found at $(BOOT_KERNEL_FILE)"; \
		exit 1; \
	fi
	@echo "Uploading kernel to http://$(PI_HOST):$(PI_PORT)/api/boot/kernel ..."
	@mkdir -p "$(OUT_DIR)"
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/upload-kernel.response" --write-out "%{http_code}" -X PUT --data-binary @"$(BOOT_KERNEL_FILE)" "http://$(PI_HOST):$(PI_PORT)/api/boot/kernel?name=$(KERNEL_IMAGE)") || { cat "$(OUT_DIR)/upload-kernel.response"; exit 1; }; \
	cat "$(OUT_DIR)/upload-kernel.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Kernel upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Kernel upload complete."

upload_cmdline: $(BOOT_CMDLINE_FILE)
	@echo "Uploading kernel command line to http://$(PI_HOST):$(PI_PORT)/api/boot/cmdline.txt ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/upload-cmdline.response" --write-out "%{http_code}" -X PUT --data-binary @"$(BOOT_CMDLINE_FILE)" "http://$(PI_HOST):$(PI_PORT)/api/boot/cmdline.txt") || { cat "$(OUT_DIR)/upload-cmdline.response"; exit 1; }; \
	cat "$(OUT_DIR)/upload-cmdline.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Kernel command line upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Kernel command line upload complete."

fancyexport: initramfs
	@$(SCRIPTS_DIR)/wait_and_export.sh "$(SD_MOUNT_BOOT)" "$(CURDIR)"

export: initramfs $(BOOT_CONFIG_FILE) $(BOOT_CMDLINE_FILE)
	@if [ ! -d "$(SD_MOUNT_BOOT)" ]; then \
		echo "Error: SD mount point $(SD_MOUNT_BOOT) not found."; \
		exit 1; \
	fi
	@echo "Exporting build files to SD card..."
	cp $(OUT_DIR)/bootfiles/* $(SD_MOUNT_BOOT)/ -r
	cp $(INITRAMFS) $(SD_MOUNT_BOOT)/initramfs.cpio.gz
	cp $(PROGRAM_INITRAMFS) $(SD_MOUNT_BOOT)/program.cpio.gz
	cp "$(BOOT_CONFIG_FILE)" $(SD_MOUNT_BOOT)/config.txt
	cp "$(BOOT_CMDLINE_FILE)" $(SD_MOUNT_BOOT)/cmdline.txt
	sync
	@echo "SD Card Flashed and ready for hardware execution!"

$(BOOT_CONFIG_FILE): $(AUDIOX_CONFIG_FILE) $(CURDIR)/Makefile
	@mkdir -p "$(dir $@)"
	@printf '%s\n' $(BOOT_CONFIG_LINES) > "$@.tmp"
	@mv "$@.tmp" "$@"

$(BOOT_CMDLINE_FILE): $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	@printf '%s\n' '$(KERNEL_CMDLINE)' > "$@"

boot_config: $(BOOT_CONFIG_FILE) $(BOOT_CMDLINE_FILE)

$(UPDATE_PACK_BIN): $(UPDATE_PACK_SRC) include/update_archive.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<"

update_archive: $(UPDATE_PACK_BIN)
	@$(MAKE) fetch_deps
	@$(MAKE) initramfs
	@$(MAKE) boot_config
	@"$(UPDATE_PACK_BIN)" --board "$(AUDIOX_BOARD)" --kernel-name "$(KERNEL_IMAGE)" --version "$(AUDIOX_VERSION_STRING)" \
		--kernel-version "$$($(SCRIPTS_DIR)/detect_kernel_version.sh "$(OUT_DIR)" "6.18.37-v8+" "$(KERNEL_UNAME_FILE)")" \
		--cmdline "$(BOOT_CMDLINE_FILE)" \
		"$(UPDATE_ARCHIVE)" "$(BOOT_CONFIG_FILE)" "$(INITRAMFS)" "$(BOOT_KERNEL_FILE)" "$(PROGRAM_INITRAMFS)"

upload_update: update_archive
	@status=$$(curl --silent --show-error --max-time 5 --output /dev/null --write-out "%{http_code}" "http://$(PI_HOST):$(PI_PORT)/api/update") || exit 1; \
	if [ "$$status" = "404" ]; then \
		echo "Device runtime does not support /api/update yet. Run 'make dev' once to install a runtime with the update endpoint."; \
		exit 1; \
	fi
	@echo "Uploading update archive to http://$(PI_HOST):$(PI_PORT)/api/update ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/upload-update.response" --write-out "%{http_code}" -X PUT --data-binary @"$(UPDATE_ARCHIVE)" "http://$(PI_HOST):$(PI_PORT)/api/update") || { cat "$(OUT_DIR)/upload-update.response"; exit 1; }; \
	cat "$(OUT_DIR)/upload-update.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Update upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Update upload complete. Device will install the staged runtime and reboot."

dev: $(BOOT_CONFIG_FILE) $(BOOT_CMDLINE_FILE)
	@$(MAKE) fetch_deps
	@$(MAKE) initramfs
	@$(MAKE) boot_config
	@echo "Uploading kernel to http://$(PI_HOST):$(PI_PORT)/api/boot/kernel ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/dev-kernel.response" --write-out "%{http_code}" -X PUT --data-binary @"$(BOOT_KERNEL_FILE)" "http://$(PI_HOST):$(PI_PORT)/api/boot/kernel?name=$(KERNEL_IMAGE)") || { cat "$(OUT_DIR)/dev-kernel.response"; exit 1; }; \
	cat "$(OUT_DIR)/dev-kernel.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Kernel upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Uploading bootloader to http://$(PI_HOST):$(PI_PORT)/api/boot/loader ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/dev-bootloader.response" --write-out "%{http_code}" -X PUT --data-binary @"$(INITRAMFS)" "http://$(PI_HOST):$(PI_PORT)/api/boot/loader") || { cat "$(OUT_DIR)/dev-bootloader.response"; exit 1; }; \
	cat "$(OUT_DIR)/dev-bootloader.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Bootloader upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Uploading kernel command line to http://$(PI_HOST):$(PI_PORT)/api/boot/cmdline.txt ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/dev-cmdline.response" --write-out "%{http_code}" -X PUT --data-binary @"$(BOOT_CMDLINE_FILE)" "http://$(PI_HOST):$(PI_PORT)/api/boot/cmdline.txt") || { cat "$(OUT_DIR)/dev-cmdline.response"; exit 1; }; \
	cat "$(OUT_DIR)/dev-cmdline.response"; \
	if [ "$$status" = "404" ]; then \
		echo "Warning: device runtime has no cmdline endpoint yet; cmdline.txt left unchanged (it updates after this install)."; \
	elif [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Kernel command line upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "Uploading boot config to http://$(PI_HOST):$(PI_PORT)/api/boot/config.txt ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/dev-boot-config.response" --write-out "%{http_code}" -X PUT --data-binary @"$(BOOT_CONFIG_FILE)" "http://$(PI_HOST):$(PI_PORT)/api/boot/config.txt"); \
	cat $(OUT_DIR)/dev-boot-config.response; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Warning: boot config upload failed with HTTP $$status; continuing with runtime upload."; \
	else \
		echo "Boot config upload complete."; \
	fi
	@echo "Uploading initramfs to http://$(PI_HOST):$(PI_PORT)/api/initram ..."
	@status=$$(curl --silent --show-error --output "$(OUT_DIR)/dev-upload.response" --write-out "%{http_code}" -X PUT --data-binary @"$(PROGRAM_INITRAMFS)" "http://$(PI_HOST):$(PI_PORT)/api/initram") || { cat "$(OUT_DIR)/dev-upload.response"; exit 1; }; \
	cat "$(OUT_DIR)/dev-upload.response"; \
	if [ "$$status" -lt 200 ] || [ "$$status" -ge 300 ]; then \
		echo "Runtime initramfs upload failed with HTTP $$status"; \
		exit 1; \
	fi
	@echo "All uploads complete. Device will reboot once to install the staged runtime, then reboot into the new kernel and images."

image: initramfs ffmpeg $(BOOT_CONFIG_FILE) $(BOOT_CMDLINE_FILE)
	@$(SCRIPTS_DIR)/build_image.sh "$(IMG_FILE)" "$(IMG_SIZE_MB)" "$(OUT_DIR)" "$(INITRAMFS)" "$(PROGRAM_INITRAMFS)" "$(BOOT_CONFIG_FILE)" "$(BOOT_CMDLINE_FILE)" "$(FFMPEG_BIN)"

clean:
	rm -rf $(OUT_DIR)

-include $(RUNTIME_DEPS_FILES) $(BOOTLOADER_DEPS_FILES)