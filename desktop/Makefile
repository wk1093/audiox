CXX = c++
CXXFLAGS ?= -O2 -Wall -Wextra -Werror -std=c++17
OUT_DIR ?= ../out/desktop
APPIMAGETOOL ?= appimagetool
ARCH ?= $(shell uname -m)

.DEFAULT_GOAL := all
.PHONY: all linux windows macos clean

all:
	@echo "Choose a platform target: linux, windows, or macos." >&2
	@exit 1

linux: $(OUT_DIR)/audiox-companion-linux-$(ARCH).AppImage
windows: $(OUT_DIR)/audiox-companion-windows-x86_64.exe
macos: $(OUT_DIR)/audiox-companion-macos-$(ARCH).tar.gz

$(OUT_DIR)/linux/$(ARCH)/audiox-companion: src/main.cpp ../include/http/websocket.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_LINUX -static -o "$@" "$<"

$(OUT_DIR)/audiox-companion-linux-$(ARCH).AppImage: $(OUT_DIR)/linux/$(ARCH)/audiox-companion audiox-companion.desktop audiox-companion.service ../assets/branding/logo_small.png
	@mkdir -p "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/lib/systemd/user"
	cp "$<" "$(OUT_DIR)/linux/$(ARCH)/AppDir/audiox-companion"
	cp audiox-companion.desktop "$(OUT_DIR)/linux/$(ARCH)/AppDir/"
	cp audiox-companion.service "$(OUT_DIR)/linux/$(ARCH)/AppDir/usr/lib/systemd/user/"
	cp ../assets/branding/logo_small.png "$(OUT_DIR)/linux/$(ARCH)/AppDir/audiox-companion.png"
	ln -sf audiox-companion "$(OUT_DIR)/linux/$(ARCH)/AppDir/AppRun"
	ARCH="$(ARCH)" APPIMAGE_EXTRACT_AND_RUN=1 "$(APPIMAGETOOL)" "$(OUT_DIR)/linux/$(ARCH)/AppDir" "$@"

$(OUT_DIR)/audiox-companion-windows-x86_64.exe: src/main.cpp ../include/http/websocket.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_WINDOWS -static -o "$@" "$<" -lws2_32

$(OUT_DIR)/macos/$(ARCH)/audiox-companion: src/main.cpp ../include/http/websocket.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -DAUDIOX_DESKTOP_MACOS -o "$@" "$<"

$(OUT_DIR)/audiox-companion-macos-$(ARCH).tar.gz: $(OUT_DIR)/macos/$(ARCH)/audiox-companion
	tar -czf "$@" -C "$(OUT_DIR)/macos/$(ARCH)" audiox-companion

clean:
	rm -rf "$(OUT_DIR)/linux" "$(OUT_DIR)/macos"
	rm -f "$(OUT_DIR)"/audiox-companion-linux-*.AppImage "$(OUT_DIR)"/audiox-companion-windows-x86_64.exe "$(OUT_DIR)"/audiox-companion-macos-*.tar.gz