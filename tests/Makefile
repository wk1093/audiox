# Included by the root Makefile; invoke targets from the repository root.
.PHONY: test test-native test-web test-release test-audio-graph test-audio-graph-mailbox test-audio-graph-tsan test-build-dependencies test-storage-expand
test: test-native test-web test-release

test-native: test-audio-runtime test-builtin-effects test-audio-meters test-audio-graph test-audio-graph-mailbox \
	test-plugin-package test-plugin-host test-plugin-integration \
	test-audio-pcm-recover test-bluetooth-hci test-bluetooth-a2dp test-display-hotplug test-soundboard-config \
	test-boot-recovery test-config-backup test-system-logs test-system test-gadget-setup test-mdns \
	test-gadget-volume test-diagnostics test-midi-usb test-midi-script test-midi-script-storage test-midi-docs test-watchdog test-resolve-modules test-build-dependencies test-storage-expand

test-storage-expand: $(OUT_DIR)/host/audiox-storage-expand-test
	@"$<"

$(OUT_DIR)/host/audiox-storage-expand-test: tests/storage_expand_test.cpp src/init/storage.cpp include/storage_expand.hpp include/watchdog_heartbeat.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -Wl,--wrap=open -Wl,--wrap=realpath -Wl,--wrap=fstat \
		-Wl,--wrap=__realpath_chk -Wl,--wrap=pwrite -Wl,--wrap=ioctl -o "$@" tests/storage_expand_test.cpp src/init/storage.cpp

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
WATCHDOG_HOST_BIN := $(OUT_DIR)/host/audiox-watchdog-test
WATCHDOG_HUNG_CHILD_BIN := $(OUT_DIR)/host/audiox-watchdog-hung-child
AUDIO_EFFECT_SRCS = \
	src/audio/plugin_package.cpp \
	src/audio/plugin_registry.cpp \
	src/audio/plugin_host.cpp \
	src/audio/effects/slot.cpp \
	src/audio/effects/gain.cpp \
	src/audio/effects/distortion.cpp \
	src/audio/effects/gate.cpp \
	src/audio/effects/dynamics.cpp \
	src/audio/effects/equalizer.cpp \
	src/audio/effects/noise_reduction.cpp \
	src/audio/effects/pitch.cpp \
	src/audio/effects/reverb.cpp

HOST_CXXFLAGS += -DAUDIOX_PLUGIN_WORKER_PATH='"$(OUT_DIR)/host/audiox-plugin-worker"'

.PHONY: test-builtin-effects
test-builtin-effects: $(OUT_DIR)/host/audiox-dynamics-test $(OUT_DIR)/host/audiox-eq-noise-test $(OUT_DIR)/host/audiox-pitch-test
	@"$(OUT_DIR)/host/audiox-dynamics-test"
	@"$(OUT_DIR)/host/audiox-eq-noise-test"
	@"$(OUT_DIR)/host/audiox-pitch-test"

$(OUT_DIR)/host/audiox-dynamics-test: tests/audio_dynamics_test.cpp src/audio/effects/dynamics.cpp include/audio/effects/dynamics.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/audio_dynamics_test.cpp src/audio/effects/dynamics.cpp

$(OUT_DIR)/host/audiox-eq-noise-test: tests/audio_eq_noise_test.cpp src/audio/effects/equalizer.cpp src/audio/effects/noise_reduction.cpp include/audio/effects/equalizer.hpp include/audio/effects/noise_reduction.hpp include/audio/effects/fft.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/audio_eq_noise_test.cpp src/audio/effects/equalizer.cpp src/audio/effects/noise_reduction.cpp

$(OUT_DIR)/host/audiox-pitch-test: tests/audio_pitch_test.cpp src/audio/effects/pitch.cpp include/audio/effects/pitch.hpp include/audio/effects/state_bank.hpp include/audio/effects/fft.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/audio_pitch_test.cpp src/audio/effects/pitch.cpp

.PHONY: test-plugin-package test-plugin-host test-plugin-integration
test-plugin-package: $(OUT_DIR)/host/plugin-package-test
	@"$<"

$(OUT_DIR)/host/plugin-package-test: tests/plugin_package_test.cpp src/audio/plugin_package.cpp include/audio/plugin_package.hpp include/audio/plugin_api.h
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/plugin_package_test.cpp src/audio/plugin_package.cpp

$(OUT_DIR)/host/audiox-plugin-worker: src/plugin_worker/main.cpp include/audio/plugin_api.h include/audio/plugin_ipc.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" "$<" -ldl -lm

PLUGIN_FIXTURES := $(addprefix $(OUT_DIR)/host/plugins/plugin-fixture-,$(addsuffix .so,0 1 2 3 4 5 6 7 8 9 10))

$(OUT_DIR)/host/plugins/plugin-fixture-%.so: tests/plugin_host_test.cpp include/audio/plugin_api.h
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -fPIC -shared -DAUDIOX_PLUGIN_TEST_FIXTURE=$* -o "$@" "$<"

test-plugin-host: $(OUT_DIR)/host/plugin-host-test $(OUT_DIR)/host/audiox-plugin-worker $(PLUGIN_FIXTURES) $(OUT_DIR)/host/plugins/softclip.so
	@"$<" "$(OUT_DIR)/host/audiox-plugin-worker" "$(OUT_DIR)/host/plugins"

$(OUT_DIR)/host/plugin-host-test: tests/plugin_host_test.cpp src/audio/plugin_host.cpp src/audio/plugin_package.cpp include/audio/plugin_host.hpp include/audio/plugin_ipc.hpp include/audio/plugin_api.h
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/plugin_host_test.cpp src/audio/plugin_host.cpp src/audio/plugin_package.cpp -pthread -lm

$(OUT_DIR)/host/plugins/gain.so: examples/plugins/gain/gain.c include/audio/plugin_api.h
$(OUT_DIR)/host/plugins/softclip.so: examples/plugins/softclip/softclip.c include/audio/plugin_api.h
$(OUT_DIR)/host/plugins/gain.so $(OUT_DIR)/host/plugins/softclip.so:
	@mkdir -p "$(dir $@)"
	cc -std=c11 -O2 -fPIC -shared -fvisibility=hidden -Iinclude -o "$@" "$<"

$(OUT_DIR)/host/plugins/gain.axm: $(OUT_DIR)/host/plugins/gain.so $(OUT_DIR)/host/axm-pack $(wildcard examples/plugins/gain/*)
	@rm -f "$@"
	"$(OUT_DIR)/host/axm-pack" examples/plugins/gain/axm.txt "$<" "$@" examples/plugins/gain/index.html examples/plugins/gain/controls.js examples/plugins/gain/style.css

test-plugin-integration: $(OUT_DIR)/host/audiox-soundboard-config-test $(OUT_DIR)/host/audiox-plugin-worker $(OUT_DIR)/host/plugins/gain.axm $(OUT_DIR)/host/plugins/plugin-fixture-9.so
	@AUDIOX_TEST_PLUGIN_PACKAGE="$(OUT_DIR)/host/plugins/gain.axm" AUDIOX_TEST_PLUGIN_HANG_MODULE="$(OUT_DIR)/host/plugins/plugin-fixture-9.so" "$(OUT_DIR)/host/audiox-soundboard-config-test"

.PHONY: test-midi-usb test-display-hotplug test-soundboard-config \
	test-boot-recovery test-config-backup test-audio-pcm-recover test-system-logs test-system \
	test-gadget-setup test-mdns test-gadget-volume test-audio-runtime test-diagnostics \
	test-watchdog test-resolve-modules test-dnsmasq-config

test-midi-usb: $(OUT_DIR)/host/audiox-midi-usb-test
	@"$<"

.PHONY: test-midi-script test-midi-script-storage test-midi-docs
test-midi-script-storage: $(OUT_DIR)/host/audiox-midi-script-storage-test
	@"$<"

$(OUT_DIR)/host/audiox-midi-script-storage-test: tests/midi_script_storage_test.cpp src/midi/script_storage.cpp include/midi/script_storage.hpp include/midi/script.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -o "$@" tests/midi_script_storage_test.cpp src/midi/script_storage.cpp
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

$(OUT_DIR)/host/audiox-midi-docs-test: tests/midi_docs_test.cpp src/midi/script.cpp src/midi/script_storage.cpp src/midi/script_utilities.cpp src/midi/usb.cpp src/config/basic.cpp src/http/logs.cpp src/bluetooth/hci.cpp \
		$(wildcard include/midi/*.hpp include/bluetooth/*.hpp include/http/*.hpp) include/config/context.hpp include/defs.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/midi_docs_test.cpp src/midi/script.cpp src/midi/script_storage.cpp src/midi/script_utilities.cpp src/midi/usb.cpp src/config/basic.cpp src/http/logs.cpp src/bluetooth/hci.cpp $(HOST_LUA_STATIC_LIB) -lm

test-midi-script: $(OUT_DIR)/host/audiox-midi-script-test
	@"$<"

$(OUT_DIR)/host/audiox-midi-script-test: tests/midi_script_test.cpp src/midi/script.cpp src/midi/script_storage.cpp src/midi/script_utilities.cpp src/midi/usb.cpp src/config/basic.cpp src/http/logs.cpp src/bluetooth/hci.cpp \
		$(wildcard include/midi/*.hpp include/bluetooth/*.hpp include/http/*.hpp) include/config/context.hpp include/defs.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -Iinclude -I$(HOST_LUA_INCLUDE_DIR) -DROOT_MOUNT_POINT='"tests/.midi-script-test-storage"' \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/midi_script_test.cpp src/midi/script.cpp src/midi/script_storage.cpp src/midi/script_utilities.cpp src/midi/usb.cpp src/config/basic.cpp src/http/logs.cpp src/bluetooth/hci.cpp $(HOST_LUA_STATIC_LIB) -lm

$(OUT_DIR)/host/audiox-midi-usb-test: tests/midi_usb_test.cpp src/midi/usb.cpp include/midi/usb.hpp include/midi/script.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Iinclude -Wl,--wrap=fopen -Wl,--wrap=write \
		-o "$@" tests/midi_usb_test.cpp src/midi/usb.cpp

test-display-hotplug: $(OUT_DIR)/host/audiox-display-hotplug-test
	@"$<"

.PHONY: test-audio-meters
test-audio-meters: $(OUT_DIR)/host/audiox-audio-meters-test
	@"$<"

$(OUT_DIR)/host/audiox-audio-meters-test: tests/audio_meters_test.cpp src/audio/graph.cpp src/audio/plugin_host.cpp include/json.hpp $(wildcard include/audio/*.hpp include/bluetooth/*.hpp include/config/*.hpp)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Iinclude \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" "$<" src/audio/plugin_host.cpp $(shell pkg-config --cflags alsa) -pthread

$(OUT_DIR)/host/audiox-display-hotplug-test: tests/display_hotplug_test.cpp src/context.cpp \
		src/audio/plugin_host.cpp \
		src/framebuffer/hardware.cpp src/framebuffer/drawing.cpp src/framebuffer/ui.cpp src/touch/touch.cpp \
		$(wildcard include/framebuffer/*.hpp include/touch/*.hpp include/audio/*.hpp include/config/*.hpp include/midi/*.hpp) include/context.hpp include/defs.hpp
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-parameter -Iinclude \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-o "$@" tests/display_hotplug_test.cpp src/context.cpp src/audio/plugin_host.cpp $(shell pkg-config --cflags alsa) -pthread

test-soundboard-config: $(OUT_DIR)/host/audiox-soundboard-config-test
	@"$<"

SOUNDBOARD_CONFIG_TEST_SRCS = tests/soundboard_config_test.cpp src/context.cpp \
	src/config/basic.cpp src/config/midi_map.cpp src/config/backup.cpp \
	src/midi/midi.cpp src/midi/usb.cpp src/midi/script.cpp src/midi/script_storage.cpp src/midi/script_utilities.cpp src/audio/setup.cpp src/audio/plugins.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp \
	src/http/server.cpp src/http/logs.cpp src/init/recovery.cpp src/touch/touch.cpp src/bluetooth/hci.cpp \
	$(AUDIO_EFFECT_SRCS)

test-audio-graph: $(OUT_DIR)/host/audiox-audio-graph-test
	@"$<"

$(OUT_DIR)/host/audiox-audio-graph-test: tests/audio_graph_test.cpp $(filter-out tests/soundboard_config_test.cpp,$(SOUNDBOARD_CONFIG_TEST_SRCS)) \
		tests/script_device_stubs.hpp src/http/api.cpp src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp include/json.hpp \
		$(wildcard include/audio/*.hpp include/config/*.hpp include/bluetooth/*.hpp) $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-Wl,--wrap=opendir -Wl,--wrap=stat -Wl,--wrap=fsync -Wl,--wrap=close \
		-o "$@" tests/audio_graph_test.cpp $(filter-out tests/soundboard_config_test.cpp,$(SOUNDBOARD_CONFIG_TEST_SRCS)) \
		src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp $(HOST_LUA_STATIC_LIB) \
		$(shell pkg-config --cflags --libs alsa) -pthread -lm

$(OUT_DIR)/host/audiox-soundboard-config-test: $(SOUNDBOARD_CONFIG_TEST_SRCS) src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp tests/script_device_stubs.hpp src/http/api.cpp src/framebuffer/ui.cpp $(wildcard include/config/*.hpp include/midi/*.hpp include/bluetooth/*.hpp include/audio/*.hpp include/audio/effects/*.hpp include/http/*.hpp include/touch/*.hpp include/framebuffer/*.hpp) include/defs.hpp include/json.hpp include/watchdog_heartbeat.hpp $(HOST_LUA_STATIC_LIB) $(HOST_LUA_HEADERS)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-format-truncation -Wno-unused-parameter -Iinclude -I$(HOST_LUA_INCLUDE_DIR) \
		-DROOT_MOUNT_POINT='"."' -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o "$@" $(SOUNDBOARD_CONFIG_TEST_SRCS) src/audio/graph.cpp src/audio/hotplug.cpp src/config/router.cpp \
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

test-gadget-volume: $(OUT_DIR)/host/audiox-gadget-volume-test
	@"$<"

test-audio-runtime: $(OUT_DIR)/host/audiox-audio-runtime-test $(OUT_DIR)/host/audiox-plugin-worker $(OUT_DIR)/host/plugins/plugin-fixture-0.so
	@"$<"

$(OUT_DIR)/host/audiox-audio-runtime-test: tests/audio_runtime_test.cpp src/audio/processing.cpp src/audio/processing_fx.cpp src/audio/gadget_volume.cpp $(AUDIO_EFFECT_SRCS) $(wildcard include/audio/*.hpp include/audio/*.h include/audio/effects/*.hpp) $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter \
		-Iinclude -DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) -DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o "$@" \
		tests/audio_runtime_test.cpp src/audio/processing_fx.cpp src/audio/gadget_volume.cpp \
		$(AUDIO_EFFECT_SRCS) $(shell pkg-config --cflags --libs alsa) -pthread -lm

$(OUT_DIR)/host/audiox-gadget-volume-test: tests/gadget_volume_test.cpp src/audio/processing.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp src/audio/plugin_host.cpp $(wildcard include/audio/*.hpp include/audio/*.h) $(AUDIOX_CONFIG_FILE)
	@mkdir -p "$(dir $@)"
	$(HOSTCXX) $(HOST_CXXFLAGS) -Wno-unused-function -Wno-format-truncation -Wno-unused-parameter \
		-Iinclude -DAUDIOX_SAMPLE_RATE=$(AUDIOX_SAMPLE_RATE) -DAUDIOX_BUFFER_FRAMES=$(AUDIOX_BUFFER_FRAMES) \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -o "$@" \
		tests/gadget_volume_test.cpp src/audio/gadget_volume.cpp src/audio/processing_fx.cpp src/audio/plugin_host.cpp $(shell pkg-config --cflags --libs alsa) -pthread -lm

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
