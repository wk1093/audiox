# Included by the root Makefile; invoke targets from the repository root.
.PHONY: test test-native test-web test-release test-audio-graph test-audio-graph-mailbox test-audio-graph-tsan test-build-dependencies test-storage-expand
test: test-native test-web test-release

test-native: test-audio-engine test-audio-runtime test-audio-meters test-audio-graph test-audio-graph-mailbox \
	test-audio-pcm-recover test-bluetooth-hci test-bluetooth-a2dp test-display-hotplug test-soundboard-config \
	test-boot-recovery test-config-backup test-system-logs test-system test-gadget-setup test-mdns \
	test-gadget-volume test-diagnostics test-midi-usb test-midi-script test-midi-docs test-watchdog test-resolve-modules test-build-dependencies test-storage-expand

test-storage-expand: $(OUT_DIR)/host/audiox-storage-expand-test
	@"$<"

$(OUT_DIR)/host/audiox-storage-expand-test: tests/storage_expand_test.cpp src/init/storage.cpp include/storage_expand.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -Wl,--wrap=open -Wl,--wrap=realpath -Wl,--wrap=fstat \
		-Wl,--wrap=pwrite -Wl,--wrap=ioctl -o "$@" tests/storage_expand_test.cpp src/init/storage.cpp

test-build-dependencies:
	bash tests/build_dependencies_test.sh

test-web:
	node --test tests/*.js

test-release:
	bash tests/release_workflows_test.sh

test-audio-graph-mailbox: $(OUT_DIR)/host/audiox-audio-graph-mailbox-test
	@"$<"

$(OUT_DIR)/host/audiox-audio-graph-mailbox-test: tests/audio_graph_mailbox_test.cpp $(wildcard include/audio/*.hpp include/config/*.hpp)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<" -pthread

test-audio-graph-tsan: $(OUT_DIR)/host/audiox-audio-graph-mailbox-tsan
	@"$<"

$(OUT_DIR)/host/audiox-audio-graph-mailbox-tsan: tests/audio_graph_mailbox_test.cpp $(wildcard include/audio/*.hpp include/config/*.hpp)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -O1 -g -fsanitize=thread -Iinclude -o "$@" "$<" -pthread

DNSMASQ_TEST_RUNNER ?= $(shell command -v qemu-aarch64 2>/dev/null)
HOST_AUDIO_CFLAGS ?= $(shell pkg-config --cflags alsa sndfile 2>/dev/null)
HOST_AUDIO_LIBS ?= $(shell pkg-config --libs alsa sndfile 2>/dev/null)
AUDIO_BENCH_BIN = $(OUT_DIR)/host/audiox-audio-bench
AUDIO_ENGINE_TEST_BIN = $(OUT_DIR)/host/audiox-audio-engine-test
WATCHDOG_HOST_BIN := $(OUT_DIR)/host/audiox-watchdog-test
WATCHDOG_HUNG_CHILD_BIN := $(OUT_DIR)/host/audiox-watchdog-hung-child
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

.PHONY: audio-bench run-audio-bench test-midi-usb test-audio-engine test-display-hotplug test-soundboard-config \
	test-boot-recovery test-config-backup test-audio-pcm-recover test-system-logs test-system \
	test-gadget-setup test-mdns test-gadget-volume test-audio-runtime test-diagnostics \
	test-watchdog test-resolve-modules test-dnsmasq-config

audio-bench: $(AUDIO_BENCH_BIN)

test-midi-usb: $(OUT_DIR)/host/audiox-midi-usb-test
	@"$<"

.PHONY: test-midi-script test-midi-docs
.PHONY: test-bluetooth-hci
test-bluetooth-hci: $(OUT_DIR)/host/audiox-bluetooth-hci-test
	@"$<"

$(OUT_DIR)/host/audiox-bluetooth-hci-test: tests/bluetooth_hci_test.cpp src/bluetooth/hci.cpp include/bluetooth/hci.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude \
		-Wl,--wrap=socket -Wl,--wrap=bind -Wl,--wrap=setsockopt -Wl,--wrap=write \
		-o "$@" tests/bluetooth_hci_test.cpp src/bluetooth/hci.cpp

.PHONY: test-bluetooth-a2dp
test-bluetooth-a2dp: $(OUT_DIR)/host/audiox-bluetooth-a2dp-test
	@"$<"

$(OUT_DIR)/host/audiox-bluetooth-a2dp-test: tests/bluetooth_a2dp_test.cpp src/bluetooth/a2dp.cpp include/bluetooth/a2dp.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/bluetooth_a2dp_test.cpp src/bluetooth/a2dp.cpp

test-midi-docs: $(OUT_DIR)/host/audiox-midi-docs-test
	@"$<"

$(OUT_DIR)/host/audiox-midi-docs-test: tests/midi_docs_test.cpp src/midi/script.cpp src/midi/usb.cpp \
		include/midi/script.hpp include/midi/usb.hpp include/defs.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-o "$@" tests/midi_docs_test.cpp src/midi/script.cpp src/midi/usb.cpp $(HOST_LUA_STATIC_LIB) -lm

test-midi-script: $(OUT_DIR)/host/audiox-midi-script-test
	@"$<"

$(OUT_DIR)/host/audiox-midi-script-test: tests/midi_script_test.cpp src/midi/script.cpp src/midi/usb.cpp \
		include/midi/script.hpp include/midi/usb.hpp include/defs.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/midi_script_test.cpp src/midi/script.cpp src/midi/usb.cpp $(HOST_LUA_STATIC_LIB) -lm

$(OUT_DIR)/host/audiox-midi-usb-test: tests/midi_usb_test.cpp src/midi/usb.cpp include/midi/usb.hpp include/midi/script.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -Wl,--wrap=fopen -Wl,--wrap=write \
		-o "$@" tests/midi_usb_test.cpp src/midi/usb.cpp

run-audio-bench: $(AUDIO_BENCH_BIN)
	@"$(AUDIO_BENCH_BIN)" "$(if $(BENCH_INPUT),$(BENCH_INPUT),-)" "$(BENCH_ALSA_DEVICE)" "$(BENCH_PORT)"

test-audio-engine: $(AUDIO_ENGINE_TEST_BIN)
	@$(AUDIO_ENGINE_TEST_BIN)

test-display-hotplug: $(OUT_DIR)/host/audiox-display-hotplug-test
	@"$<"

.PHONY: test-audio-meters
test-audio-meters: $(OUT_DIR)/host/audiox-audio-meters-test
	@"$<"

$(OUT_DIR)/host/audiox-audio-meters-test: tests/audio_meters_test.cpp src/audio/graph.cpp include/json.hpp $(wildcard include/audio/*.hpp include/bluetooth/*.hpp include/config/*.hpp)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Iinclude \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" "$<" $(shell pkg-config --cflags alsa) -pthread

$(OUT_DIR)/host/audiox-display-hotplug-test: tests/display_hotplug_test.cpp src/context.cpp \
		src/framebuffer/hardware.cpp src/framebuffer/drawing.cpp src/framebuffer/ui.cpp src/touch/touch.cpp \
		$(wildcard include/framebuffer/*.hpp include/touch/*.hpp include/audio/*.hpp include/config/*.hpp include/midi/*.hpp) include/context.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-parameter -Iinclude \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/display_hotplug_test.cpp src/context.cpp $(shell pkg-config --cflags alsa)

test-soundboard-config: $(OUT_DIR)/host/audiox-soundboard-config-test
	@"$<"

SOUNDBOARD_CONFIG_TEST_SRCS = tests/soundboard_config_test.cpp src/context.cpp \
	src/config/basic.cpp src/config/midi_map.cpp src/config/backup.cpp \
	src/midi/midi.cpp src/midi/usb.cpp src/midi/script.cpp src/audio/setup.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp \
	src/http/server.cpp src/http/logs.cpp src/init/recovery.cpp src/touch/touch.cpp \
	$(filter-out tools/audiox-audio-bench.cpp src/audio/engine.cpp src/audio/graph_processor.cpp,$(AUDIO_BENCH_SRCS))

test-audio-graph: $(OUT_DIR)/host/audiox-audio-graph-test
	@"$<"

$(OUT_DIR)/host/audiox-audio-graph-test: tests/audio_graph_test.cpp $(filter-out tests/soundboard_config_test.cpp,$(SOUNDBOARD_CONFIG_TEST_SRCS)) \
		src/http/api.cpp src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp src/bluetooth/hci.cpp include/json.hpp \
		$(wildcard include/audio/*.hpp include/config/*.hpp include/bluetooth/*.hpp) $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-Wl,--wrap=opendir -Wl,--wrap=stat -Wl,--wrap=fsync -Wl,--wrap=close \
		-o "$@" tests/audio_graph_test.cpp $(filter-out tests/soundboard_config_test.cpp,$(SOUNDBOARD_CONFIG_TEST_SRCS)) \
		src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp src/bluetooth/hci.cpp $(HOST_LUA_STATIC_LIB) \
		$(shell pkg-config --cflags --libs alsa) -pthread -lm

$(OUT_DIR)/host/audiox-soundboard-config-test: $(SOUNDBOARD_CONFIG_TEST_SRCS) src/http/api.cpp src/framebuffer/ui.cpp $(wildcard include/config/*.hpp include/midi/*.hpp include/audio/*.hpp include/audio/effects/*.hpp include/http/*.hpp include/touch/*.hpp include/framebuffer/*.hpp) include/defs.hpp include/json.hpp include/watchdog_heartbeat.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Wno-unused-parameter -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o "$@" $(SOUNDBOARD_CONFIG_TEST_SRCS) \
		$(HOST_LUA_STATIC_LIB) $(shell pkg-config --cflags --libs alsa) -pthread -lm

test-boot-recovery: $(OUT_DIR)/host/audiox-boot-recovery-test
	@"$<"

$(OUT_DIR)/host/audiox-boot-recovery-test: tests/boot_recovery_test.cpp include/boot_recovery.hpp include/defs.hpp include/update_archive.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<"

test-config-backup: $(OUT_DIR)/host/audiox-config-backup-test $(OUT_DIR)/host/audiox-config-backup-http-test
	@"$(OUT_DIR)/host/audiox-config-backup-test"
	@"$(OUT_DIR)/host/audiox-config-backup-http-test"

$(OUT_DIR)/host/audiox-config-backup-test: tests/config_backup_test.cpp src/config/backup.cpp include/config/backup.hpp include/update_archive.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/config_backup_test.cpp src/config/backup.cpp

$(OUT_DIR)/host/audiox-config-backup-http-test: tests/config_backup_http_test.cpp src/config/backup.cpp src/http/server.cpp src/http/api.cpp include/config/backup.hpp include/http/context.hpp include/json.hpp include/update_archive.hpp include/watchdog_heartbeat.hpp $(wildcard include/midi/*.hpp)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Wno-unused-parameter -Iinclude \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/config_backup_http_test.cpp src/config/backup.cpp -pthread

test-audio-pcm-recover: $(OUT_DIR)/host/audiox-audio-pcm-recover-test
	@"$<"

test-system-logs: $(OUT_DIR)/host/audiox-system-logs-test
	@"$<"

$(OUT_DIR)/host/audiox-system-logs-test: tests/system_logs_test.cpp src/http/logs.cpp include/http/logs.hpp include/http/context.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -DROOT_MOUNT_POINT='"."' -o "$@" "$<"

test-system: $(OUT_DIR)/host/audiox-system-test
	@"$<"

$(OUT_DIR)/host/audiox-system-test: tests/system_test.cpp include/system.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<"

test-gadget-setup: $(OUT_DIR)/host/audiox-gadget-setup-test
	@"$<"

$(OUT_DIR)/host/audiox-gadget-setup-test: tests/gadget_setup_test.cpp src/init/gadget.cpp $(wildcard include/audio/*.hpp include/config/*.hpp include/midi/*.hpp) include/context.hpp include/init.hpp include/system.hpp include/defs.hpp
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

test-audio-runtime: $(OUT_DIR)/host/audiox-audio-runtime-test-0 $(OUT_DIR)/host/audiox-audio-runtime-test-1
	@"$(OUT_DIR)/host/audiox-audio-runtime-test-0"
	@"$(OUT_DIR)/host/audiox-audio-runtime-test-1"

$(OUT_DIR)/host/audiox-audio-runtime-test-%: tests/audio_runtime_test.cpp src/audio/processing.cpp src/audio/graph_processor.cpp src/audio/processing_fx.cpp src/audio/gadget_volume.cpp $(wildcard src/audio/effects/*.cpp include/audio/*.hpp include/audio/*.h) $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter \
		-Iinclude -DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) -DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) -DAUDIOX_USE_SHARED_GRAPH_PROCESSOR=$* \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o "$@" \
		tests/audio_runtime_test.cpp src/audio/graph_processor.cpp src/audio/processing_fx.cpp src/audio/gadget_volume.cpp \
		$(wildcard src/audio/effects/*.cpp) $(shell pkg-config --cflags --libs alsa) -pthread -lm

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

$(OUT_DIR)/host/audiox-audio-pcm-recover-test: tests/audio_pcm_recover_test.cpp include/audio/alsa_pcm.h include/audio/health.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-parameter -Iinclude $(shell pkg-config --cflags alsa) -o "$@" "$<" $(shell pkg-config --libs alsa)

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
