CC = aarch64-linux-gnu-gcc
CFLAGS = -static -O3 -Iinclude -Wall -Wextra -Werror

KV ?= 6.18.36-v8+
DSI_TOUCH_OVERLAY ?= vc4-kms-dsi-7inch
REQUIRED_MODULES = \
	sound/core/snd \
	sound/core/snd-timer \
	sound/core/snd-pcm \
	sound/core/oss/snd-pcm-oss \
	drivers/usb/gadget/libcomposite \
	drivers/usb/gadget/function/u_audio \
	drivers/usb/gadget/function/usb_f_uac2 \
	drivers/video/backlight/backlight \
	drivers/gpu/drm/drm_panel_orientation_quirks \
	drivers/gpu/drm/drm \
	drivers/gpu/drm/panel/panel-raspberrypi-touchscreen \
	drivers/base/regmap/regmap-i2c \
	drivers/regulator/rpi-panel-attiny-regulator \
	drivers/input/touchscreen/raspberrypi-ts \
	drivers/hid/hid-multitouch \
	drivers/input/touchscreen/usbtouchscreen \
	drivers/input/touchscreen/goodix_ts \
	drivers/input/touchscreen/edt-ft5x06 \
	drivers/hid/i2c-hid/i2c-hid \
	drivers/hid/i2c-hid/i2c-hid-of

# Paths
ARCH ?= aarch64
OUT_DIR = $(CURDIR)/out
ROOTFS_DIR = $(OUT_DIR)/rootfs
INITRAMFS = $(OUT_DIR)/initramfs.cpio.gz

SD_MOUNT_BOOT ?= /run/media/$(USER)/bootfs

IMG_FILE = $(OUT_DIR)/custom_audio_console.img
IMG_SIZE_MB = 128

# GitHub RAW base URLs
FW_URL = https://github.com/raspberrypi/firmware/raw/master/boot
MOD_URL = https://github.com/raspberrypi/firmware/raw/master/modules

# Debug build variables (native x86_64, runs in qemu-system-x86_64)
DEBUG_CC     = gcc
DEBUG_CFLAGS = -static -O0 -g -Iinclude -Wall -Wextra -Werror
DEBUG_OUT    = $(OUT_DIR)/debug
DEBUG_ROOTFS = $(DEBUG_OUT)/rootfs
DEBUG_INITRAMFS = $(DEBUG_OUT)/initramfs.cpio.gz
DEBUG_KERNEL ?= $(firstword $(wildcard /boot/vmlinuz-linux /boot/vmlinuz /boot/vmlinuz-generic))

.PHONY: all clean rootfs initramfs fetch_deps fetch_modules qemu fancyexport export image debug debug_rootfs debug_initramfs FORCE

all: fetch_deps fetch_modules initramfs

# 1. Fetch Core Bootloader and Kernel Files
fetch_deps:
	@echo "Fetching official kernel and boot blobs..."
	mkdir -p $(OUT_DIR)/bootfiles/overlays
	@setup_file() { \
		if [ ! -f $(OUT_DIR)/bootfiles/$$1 ]; then \
			curl -L $(FW_URL)/$$1 -o $(OUT_DIR)/bootfiles/$$1; \
		fi \
	}; \
	setup_file "kernel8.img"; \
	setup_file "bootcode.bin"; \
	setup_file "fixup.dat"; \
	setup_file "start.elf"; \
	setup_file "bcm2711-rpi-4-b.dtb"; \
	if [ ! -f $(OUT_DIR)/bootfiles/overlays/vc4-kms-v3d.dtbo ]; then \
		curl -L $(FW_URL)/overlays/vc4-kms-v3d.dtbo -o $(OUT_DIR)/bootfiles/overlays/vc4-kms-v3d.dtbo; \
	fi; \
	if [ ! -f $(OUT_DIR)/bootfiles/overlays/$(DSI_TOUCH_OVERLAY).dtbo ]; then \
		curl -L $(FW_URL)/overlays/$(DSI_TOUCH_OVERLAY).dtbo -o $(OUT_DIR)/bootfiles/overlays/$(DSI_TOUCH_OVERLAY).dtbo; \
	fi

# 2. Dynamically Fetch and Decompress Required Audio Gadget Modules
fetch_modules:
	@echo "Syncing module dependencies for Kernel Version: $(KV)..."
	@MOD_DIR="$(OUT_DIR)/modules_staging/$(KV)"; \
	mkdir -p $$MOD_DIR/kernel/drivers/usb/gadget/function; \
	mkdir -p $$MOD_DIR/kernel/drivers/usb/gadget/legacy; \
	mkdir -p $$MOD_DIR/kernel/drivers/input/touchscreen; \
	mkdir -p $$MOD_DIR/kernel/drivers/video/backlight; \
	mkdir -p $$MOD_DIR/kernel/drivers/gpu/drm; \
	mkdir -p $$MOD_DIR/kernel/drivers/gpu/drm/panel; \
	mkdir -p $$MOD_DIR/kernel/drivers/base/regmap; \
	mkdir -p $$MOD_DIR/kernel/drivers/regulator; \
	mkdir -p $$MOD_DIR/kernel/drivers/hid/i2c-hid; \
	mkdir -p $$MOD_DIR/kernel/drivers/hid; \
	mkdir -p $$MOD_DIR/kernel/sound/core/oss; \
	\
	setup_mod_meta() { \
		if [ ! -f $$MOD_DIR/$$1 ]; then \
			curl -L $(MOD_URL)/$(KV)/$$1 -o $$MOD_DIR/$$1; \
		fi \
	}; \
	setup_mod_meta "modules.dep"; \
	setup_mod_meta "modules.dep.bin"; \
	setup_mod_meta "modules.alias"; \
	setup_mod_meta "modules.alias.bin"; \
	setup_mod_meta "modules.builtin"; \
	setup_mod_meta "modules.builtin.bin"; \
	setup_mod_meta "modules.order"; \
	\
	$(foreach mod,$(REQUIRED_MODULES), \
		TARGET_FILE=$$MOD_DIR/kernel/$(mod).ko; \
		if [ ! -f $$TARGET_FILE ]; then \
			echo "Downloading: $(mod).ko.xz"; \
			curl -L $(MOD_URL)/$(KV)/kernel/$(mod).ko.xz -o $$TARGET_FILE.xz; \
			xz -d $$TARGET_FILE.xz; \
		fi; \
	)

$(ROOTFS_DIR)/init: FORCE src/init.c
	@echo "Compiling system architecture init binary..."
	mkdir -p $(ROOTFS_DIR)
	$(CC) $(CFLAGS) -DKERNEL_VERSION='"$(KV)"' -o $(ROOTFS_DIR)/init src/init.c -lm

# 3. Build the File System Structure and Stage Kernel Modules
rootfs: fetch_deps fetch_modules $(ROOTFS_DIR)/init
	@echo "Creating rootfs structure..."
	mkdir -p $(ROOTFS_DIR)/bin $(ROOTFS_DIR)/sbin $(ROOTFS_DIR)/etc
	mkdir -p $(ROOTFS_DIR)/proc $(ROOTFS_DIR)/sys $(ROOTFS_DIR)/dev
	mkdir -p $(ROOTFS_DIR)/lib/modules
	
	@echo "Staging kernel objects into target lib tree..."
	cp -r $(OUT_DIR)/modules_staging/* $(ROOTFS_DIR)/lib/modules/

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
	@echo "=================================================="
	@echo "   AUTOMATED AUDIO APPLICATION SD EXPORTER        "
	@echo "=================================================="
	@echo "--> Waiting for SD card or USB drive to be inserted..."
	@sudo mkdir -p $(SD_MOUNT_BOOT)
	@BASE_DEV=""; \
	for dev in /sys/block/sd* /sys/block/mmcblk*; do \
		if [ -e "$$dev" ]; then BASE_DEV="$$BASE_DEV $$(basename $$dev)"; fi; \
	done; \
	while true; do \
		sleep 0.5; \
		CUR_DEV=""; \
		for dev in /sys/block/sd* /sys/block/mmcblk*; do \
			if [ -e "$$dev" ]; then CUR_DEV="$$CUR_DEV $$(basename $$dev)"; fi; \
		done; \
		\
		NEW_DEVS=""; \
		for d in $$CUR_DEV; do \
			echo "$$BASE_DEV" | grep -q "\b$$d\b" || NEW_DEVS="$$NEW_DEVS $$d"; \
		done; \
		\
		CHOSEN_DEV=""; \
		TARGET=""; \
		for candidate in $$NEW_DEVS; do \
			if echo "$$candidate" | grep -q "mmcblk"; then \
				PART_NAME="$${candidate}p1"; \
			else \
				PART_NAME="$${candidate}1"; \
			fi; \
			if [ -e "/sys/class/block/$$PART_NAME" ]; then \
				CHOSEN_DEV="$$candidate"; \
				TARGET="/dev/$$PART_NAME"; \
				break; \
			fi; \
		done; \
		\
		if [ ! -z "$$CHOSEN_DEV" ]; then \
			echo "--> Valid hardware device detected: $$CHOSEN_DEV"; \
			echo "--> Found partition file node: $$TARGET"; \
			\
			RETRY_COUNT=0; \
			while [ ! -b "$$TARGET" ]; do \
				sleep 0.1; \
				RETRY_COUNT=$$((RETRY_COUNT + 1)); \
				if [ $$RETRY_COUNT -gt 30 ]; then \
					echo "ERROR: Partition $$TARGET failed to link in /dev."; \
					exit 1; \
				fi; \
			done; \
			\
			echo "--> Mounting $$TARGET to $(SD_MOUNT_BOOT)..."; \
			if sudo mount -o uid=$$(id -u),gid=$$(id -g) $$TARGET $(SD_MOUNT_BOOT); then \
				make export; \
				sync; \
				sudo umount $(SD_MOUNT_BOOT); \
				if command -v eject >/dev/null 2>&1; then \
					sudo eject /dev/$$CHOSEN_DEV 2>/dev/null || true; \
				fi; \
				echo "--> Export complete! SD card is ready for use."; \
			else \
				echo "ERROR: Failed to mount $$TARGET. Check partition structure."; \
			fi; \
			break; \
		fi; \
	done

export: initramfs
	@if [ ! -d "$(SD_MOUNT_BOOT)" ]; then \
		echo "Error: SD mount point $(SD_MOUNT_BOOT) not found."; \
		exit 1; \
	fi
	@echo "Exporting build files to SD card..."
	cp $(OUT_DIR)/bootfiles/* $(SD_MOUNT_BOOT)/ -r
	cp $(INITRAMFS) $(SD_MOUNT_BOOT)/initramfs.cpio.gz
	@echo "initramfs initramfs.cpio.gz followkernel" > $(SD_MOUNT_BOOT)/config.txt
	@echo "dtoverlay=vc4-kms-v3d" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "dtoverlay=$(DSI_TOUCH_OVERLAY)" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "dtoverlay=dwc2,dr_mode=peripheral" >> $(SD_MOUNT_BOOT)/config.txt
	@echo "console=fbcon console=tty1 quiet loglevel=3 rdinit=/init" > $(SD_MOUNT_BOOT)/cmdline.txt
	sync
	@echo "SD Card Flashed and ready for hardware execution!"

# Generate Standalone Flashable Disk Image File
image: initramfs
	@echo "Creating a blank $(IMG_SIZE_MB)MB image file..."
	dd if=/dev/zero of=$(IMG_FILE) bs=1M count=$(IMG_SIZE_MB) status=progress
	parted --script $(IMG_FILE) mklabel msdos
	parted --script $(IMG_FILE) mkpart primary fat32 2048s 98303s
	parted --script $(IMG_FILE) set 1 boot on
	parted --script $(IMG_FILE) mkpart primary fat32 98304s 100%
	@LOOP_DEV=$$(sudo losetup --find --show --partscan $(IMG_FILE)); \
	echo "Loop device assigned at: $$LOOP_DEV"; \
	echo "Formatting system partitions..."; \
	sudo mkfs.vfat -F 32 -n "BOOT" $${LOOP_DEV}p1; \
	sudo mkfs.vfat -F 32 -n "STORAGE" $${LOOP_DEV}p2; \
	echo "Mounting image internally to stage data..."; \
	mkdir -p $(OUT_DIR)/mnt_boot; \
	sudo mount $${LOOP_DEV}p1 $(OUT_DIR)/mnt_boot; \
	echo "Writing distribution files..."; \
	sudo cp $(OUT_DIR)/bootfiles/* $(OUT_DIR)/mnt_boot/ -r; \
	sudo cp $(INITRAMFS) $(OUT_DIR)/mnt_boot/initramfs.cpio.gz; \
	sudo sh -c "echo 'initramfs initramfs.cpio.gz followkernel' > $(OUT_DIR)/mnt_boot/config.txt"; \
	sudo sh -c "echo 'dtoverlay=vc4-kms-v3d' >> $(OUT_DIR)/mnt_boot/config.txt"; \
	sudo sh -c "echo 'dtoverlay=$(DSI_TOUCH_OVERLAY)' >> $(OUT_DIR)/mnt_boot/config.txt"; \
	sudo sh -c "echo 'dtoverlay=dwc2,dr_mode=peripheral' >> $(OUT_DIR)/mnt_boot/config.txt"; \
	sudo sh -c "echo 'console=fbcon console=tty1 quiet loglevel=3 rdinit=/init' > $(OUT_DIR)/mnt_boot/cmdline.txt"; \
	echo "Cleaning up allocations and unmounting loopback layers..."; \
	sudo umount $(OUT_DIR)/mnt_boot; \
	sudo losetup -d $$LOOP_DEV; \
	rm -rf $(OUT_DIR)/mnt_boot
	@echo "SUCCESS! Flashable image generated at: $(IMG_FILE)"

# Native x86_64 Debug Configurations
$(DEBUG_ROOTFS)/init: FORCE src/init.c
	@echo "Compiling debug init binary (x86_64)..."
	mkdir -p $(DEBUG_ROOTFS)
	$(DEBUG_CC) $(DEBUG_CFLAGS) -o $(DEBUG_ROOTFS)/init src/init.c -lm

debug_rootfs: $(DEBUG_ROOTFS)/init
	@echo "Creating debug rootfs structure..."
	mkdir -p $(DEBUG_ROOTFS)/bin $(DEBUG_ROOTFS)/sbin $(DEBUG_ROOTFS)/etc
	mkdir -p $(DEBUG_ROOTFS)/proc $(DEBUG_ROOTFS)/sys $(DEBUG_ROOTFS)/dev

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
		-append "console=tty1 vga=0x343 rdinit=/init quiet loglevel=0" \
		-display sdl

clean:
	rm -rf $(OUT_DIR)