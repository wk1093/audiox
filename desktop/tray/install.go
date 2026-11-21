package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

type installation struct {
	dir      string
	launcher string
	current  string
}

func userDirs() (string, string, error) {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return "", "", fmt.Errorf("cannot determine home directory")
	}
	data := os.Getenv("XDG_DATA_HOME")
	config := os.Getenv("XDG_CONFIG_HOME")
	if data == "" {
		data = filepath.Join(home, ".local", "share")
	}
	if config == "" {
		config = filepath.Join(home, ".config")
	}
	if !filepath.IsAbs(data) || !filepath.IsAbs(config) {
		return "", "", fmt.Errorf("XDG data and configuration directories must be absolute")
	}
	return data, config, nil
}

func atomicWrite(path string, contents []byte, mode os.FileMode) error {
	f, err := os.CreateTemp(filepath.Dir(path), ".audiox-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if err = f.Chmod(mode); err == nil {
		_, err = f.Write(contents)
	}
	if err == nil {
		err = f.Sync()
	}
	if closeErr := f.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(name, path)
}

func activate(current, target string) error {
	tmp, err := os.CreateTemp(filepath.Dir(current), ".audiox-link-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	if err = tmp.Close(); err != nil {
		os.Remove(name)
		return err
	}
	if err = os.Remove(name); err != nil {
		return err
	}
	defer os.Remove(name)
	if err = os.Symlink(target, name); err != nil {
		return err
	}
	return os.Rename(name, current)
}

func installImage(source string) (installation, bool, error) {
	data, config, err := userDirs()
	if err != nil {
		return installation{}, false, err
	}
	dir := filepath.Join(data, "audiox")
	images := filepath.Join(dir, "appimages")
	unitDir := filepath.Join(config, "systemd", "user")
	for _, path := range []string{images, unitDir} {
		if err = os.MkdirAll(path, 0755); err != nil {
			return installation{}, false, err
		}
	}
	inst := installation{dir: dir, launcher: filepath.Join(dir, "audiox-desktop"), current: filepath.Join(images, "current.AppImage")}
	if strings.ContainsAny(inst.launcher, "\r\n") {
		return inst, false, fmt.Errorf("installation path contains a newline")
	}
	_, statErr := os.Lstat(inst.current)
	first := os.IsNotExist(statErr)
	if statErr != nil && !first {
		return inst, false, statErr
	}
	in, err := os.Open(source)
	if err != nil {
		return inst, false, err
	}
	defer in.Close()
	tmp, err := os.CreateTemp(images, ".audiox-image-*")
	if err != nil {
		return inst, false, err
	}
	defer os.Remove(tmp.Name())
	hash := sha256.New()
	_, err = io.Copy(io.MultiWriter(tmp, hash), in)
	if err == nil {
		err = tmp.Chmod(0755)
	}
	if err == nil {
		err = tmp.Sync()
	}
	if closeErr := tmp.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return inst, false, err
	}
	name := "audiox-" + hex.EncodeToString(hash.Sum(nil)) + ".AppImage"
	target := filepath.Join(images, name)
	if err = os.Rename(tmp.Name(), target); err != nil {
		return inst, false, err
	}
	// The launcher resolves its own directory, so it remains valid with custom XDG paths.
	launcher := "#!/bin/sh\nset -eu\nbase=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nexec \"$base/appimages/current.AppImage\" --appimage-extract-and-run \"$@\"\n"
	if err = atomicWrite(inst.launcher, []byte(launcher), 0755); err != nil {
		return inst, false, err
	}
	unitPath := filepath.Join(unitDir, "audiox-companion.service")
	unit := "[Unit]\nDescription=AudioX Companion\nAfter=graphical-session.target\n\n[Service]\nType=simple\nExecStart=\"" +
		strings.NewReplacer("\\", "\\\\", "\"", "\\\"", "%", "%%", "$", "$$").Replace(inst.launcher) +
		"\"\nRestart=always\nRestartSec=5\n\n[Install]\nWantedBy=default.target\n"
	existing, readErr := os.ReadFile(unitPath)
	if readErr != nil && !os.IsNotExist(readErr) {
		return inst, false, readErr
	}
	legacyUnit := "[Unit]\nDescription=AudioX Companion\nAfter=graphical-session.target\n\n[Service]\nType=simple\nExecStart=\"%h/.local/bin/audiox-companion.AppImage\" --appimage-extract-and-run\nRestart=on-failure\nRestartSec=5\n\n[Install]\nWantedBy=default.target\n"
	packagedUnit := "[Unit]\nDescription=AudioX Companion\nAfter=graphical-session.target\n\n[Service]\nType=simple\nExecStart=\"%h/.local/share/audiox/audiox-desktop\"\nRestart=always\nRestartSec=5\n\n[Install]\nWantedBy=default.target\n"
	if readErr == nil && string(existing) != unit && string(existing) != legacyUnit && string(existing) != packagedUnit {
		return inst, false, fmt.Errorf("refusing to overwrite customized user service %s", unitPath)
	}
	var reloadErr error
	if string(existing) != unit {
		if err = atomicWrite(unitPath, []byte(unit), 0644); err != nil {
			return inst, false, err
		}
		if output, err := exec.Command("systemctl", "--user", "daemon-reload").CombinedOutput(); err != nil {
			reloadErr = fmt.Errorf("systemd user daemon reload failed: %s: %w", strings.TrimSpace(string(output)), err)
		}
	}
	if err = activate(inst.current, name); err != nil {
		return inst, false, err
	}
	return inst, first, reloadErr
}
