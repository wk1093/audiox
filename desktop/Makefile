CXX = c++
CXXFLAGS ?= -O2 -Wall -Wextra -Werror -std=c++17
OUT_DIR ?= ../out/desktop
APPIMAGETOOL ?= appimagetool
ARCH ?= $(shell uname -m)

.DEFAULT_GOAL := all
.PHONY: all linux linux-update windows macos clean

all:
	@echo "Choose a platform target: linux, windows, or macos." >&2
	@exit 1

linux: $(OUT_DIR)/audiox-companion-linux-$(ARCH).AppImage
linux-update: linux
	"$(OUT_DIR)/linux/$(ARCH)/audiox-tray" --install-only --appimage "$(abspath $(OUT_DIR)/audiox-companion-linux-$(ARCH).AppImage)"
	systemctl --user daemon-reload
	systemctl --user restart audiox-companion.service

windows: $(OUT_DIR)/audiox-companion-windows-x86_64.exe
macos: $(OUT_DIR)/audiox-companion-macos-$(ARCH).tar.gz

$(OUT_DIR)/linux/$(ARCH)/audiox-companion: src/main.cpp src/routing.hpp src/inventory.hpp src/default_sink.hpp src/auto_routing.hpp ../include/http/websocket.hpp ../include/desktop/routing.hpp ../include/desktop/json.hpp ../include/desktop/rules.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_LINUX -static -o "$@" "$<"

$(OUT_DIR)/linux/$(ARCH)/audiox-tray: tray/*.go tray/go.mod tray/go.sum
	@mkdir -p "$(@D)"
	cd tray && CGO_ENABLED=0 go build -trimpath -o "$(abspath $@)" .

$(OUT_DIR)/audiox-companion-linux-$(ARCH).AppImage: $(OUT_DIR)/linux/$(ARCH)/audiox-companion $(OUT_DIR)/linux/$(ARCH)/audiox-tray AppRun audiox-companion.desktop audiox-companion.service wireplumber/audiox-routing.lua wireplumber/90-audiox-routing.conf ../assets/branding/logo_small.png
	@mkdir -p "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/lib/systemd/user" "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/share/audiox/wireplumber"
	@mkdir -p "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/bin"
	cp "$<" "$(OUT_DIR)/linux/$(ARCH)/AppDir/audiox-companion"
	cp "$(OUT_DIR)/linux/$(ARCH)/audiox-tray" "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/bin/audiox-tray"
	cp audiox-companion.desktop "$(OUT_DIR)/linux/$(ARCH)/AppDir/"
	cp audiox-companion.service "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/lib/systemd/user/"
	cp wireplumber/audiox-routing.lua wireplumber/90-audiox-routing.conf "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/share/audiox/wireplumber/"
	cp ../assets/branding/logo_small.png "$(OUT_DIR)/linux/$(ARCH)/AppDir/audiox-companion.png"
	rm -f "$(OUT_DIR)/linux/$(ARCH)/AppDir/AppRun"
	install -m 755 AppRun "$(OUT_DIR)/linux/$(ARCH)/AppDir/AppRun"
	ARCH="$(ARCH)" APPIMAGE_EXTRACT_AND_RUN=1 "$(APPIMAGETOOL)" "$(OUT_DIR)/linux/$(ARCH)/AppDir" "$@"

$(OUT_DIR)/audiox-companion-windows-x86_64.exe: src/main.cpp ../include/http/websocket.hpp ../include/desktop/routing.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_WINDOWS -static -o "$@" "$<" -lws2_32

$(OUT_DIR)/macos/$(ARCH)/audiox-companion: src/main.cpp ../include/http/websocket.hpp ../include/desktop/routing.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_MACOS -o "$@" "$<"

$(OUT_DIR)/audiox-companion-macos-$(ARCH).tar.gz: $(OUT_DIR)/macos/$(ARCH)/audiox-companion
	tar -czf "$@" -C "$(OUT_DIR)/macos/$(ARCH)" audiox-companion

clean:
	rm -rf "$(OUT_DIR)/linux" "$(OUT_DIR)/macos"
	rm -f "$(OUT_DIR)"/audiox-companion-linux-*.AppImage "$(OUT_DIR)"/audiox-companion-windows-x86_64.exe "$(OUT_DIR)"/audiox-companion-macos-*.tar.gz