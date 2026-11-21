package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestInstallationAndCustomizedUnit(t *testing.T) {
	dir := t.TempDir()
	data := filepath.Join(dir, "custom data")
	config := filepath.Join(dir, "config")
	bin := filepath.Join(dir, "bin")
	if err := os.Mkdir(bin, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bin, "systemctl"), []byte("#!/bin/sh\nexit 0\n"), 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", dir)
	t.Setenv("XDG_DATA_HOME", data)
	t.Setenv("XDG_CONFIG_HOME", config)
	t.Setenv("PATH", bin+":/usr/bin:/bin")
	source := filepath.Join(dir, "download.AppImage")
	if err := os.WriteFile(source, []byte("first image"), 0755); err != nil {
		t.Fatal(err)
	}
	inst, first, err := installImage(source)
	if err != nil || !first {
		t.Fatalf("first install: first=%v error=%v", first, err)
	}
	got, err := os.ReadFile(inst.current)
	if err != nil || string(got) != "first image" {
		t.Fatalf("installed image: %q, %v", got, err)
	}
	if _, first, err = installImage(source); err != nil || first {
		t.Fatalf("repeat install: first=%v error=%v", first, err)
	}
	unit := filepath.Join(config, "systemd/user/audiox-companion.service")
	if err := os.WriteFile(unit, []byte("custom unit"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(source, []byte("new image"), 0755); err != nil {
		t.Fatal(err)
	}
	if _, _, err = installImage(source); err == nil {
		t.Fatal("customized unit was overwritten")
	}
	got, _ = os.ReadFile(inst.current)
	if string(got) != "first image" {
		t.Fatal("failed installation changed active image")
	}
}

func TestCombinedUpdateAndFailures(t *testing.T) {
	dir := t.TempDir()
	images := filepath.Join(dir, "appimages")
	if err := os.Mkdir(images, 0755); err != nil {
		t.Fatal(err)
	}
	inst := installation{dir: dir, current: filepath.Join(images, "current.AppImage")}
	if err := os.Symlink("old.AppImage", inst.current); err != nil {
		t.Fatal(err)
	}
	desktop := []byte("\x7fELF test AppImage")
	firmware := []byte("AXUPDATE test firmware")
	assets := map[string][]byte{
		"audiox-companion-linux-x86_64.AppImage": desktop,
		"audiox-update-pi4.axu":                  firmware,
	}
	release := release{Tag: "v2.0.0"}
	for name, content := range assets {
		sum := sha256.Sum256(content)
		release.Assets = append(release.Assets, asset{Name: name, State: "uploaded", Size: int64(len(content)), Digest: "sha256:" + hex.EncodeToString(sum[:])})
	}
	corrupt := false
	published := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/releases/latest" {
			json.NewEncoder(w).Encode(release)
			return
		}
		name := filepath.Base(r.URL.Path)
		isChecksum := strings.HasSuffix(name, ".sha256")
		name = strings.TrimSuffix(name, ".sha256")
		content, ok := assets[name]
		if !ok {
			http.NotFound(w, r)
			return
		}
		if isChecksum {
			sum := sha256.Sum256(content)
			fmt.Fprintf(w, "%x  %s\n", sum, name)
		} else if corrupt && name == "audiox-update-pi4.axu" {
			w.Write([]byte("corrupt"))
		} else {
			w.Write(content)
		}
	}))
	defer published.Close()
	uploads := 0
	failUpload := false
	pi := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/system/info" {
			fmt.Fprint(w, `{"board":"pi4","version":"1.0.0"}`)
		} else if r.URL.Path == "/api/update" {
			uploads++
			body, err := io.ReadAll(r.Body)
			if err != nil || string(body) != string(firmware) || r.ContentLength != int64(len(firmware)) {
				t.Errorf("incorrect firmware upload: %q, %v", body, err)
			}
			if failUpload {
				http.Error(w, "rejected", http.StatusBadRequest)
			}
		} else {
			http.NotFound(w, r)
		}
	}))
	defer pi.Close()
	run := func() error {
		return performUpdate(inst, pi.URL, "stable", "v2.0.0", published.URL+"/api", published.URL+"/download/", published.Client(), pi.Client())
	}
	if err := performUpdate(inst, pi.URL, "stable", "v1.0.0", published.URL+"/api", published.URL+"/download/", published.Client(), pi.Client()); err == nil || uploads != 0 {
		t.Fatal("changed release was not rejected")
	}
	corrupt = true
	if err := run(); err == nil || uploads != 0 {
		t.Fatalf("corrupt firmware: err=%v uploads=%d", err, uploads)
	}
	corrupt = false
	failUpload = true
	if err := run(); err == nil || uploads != 1 {
		t.Fatalf("rejected upload: err=%v uploads=%d", err, uploads)
	}
	if target, _ := os.Readlink(inst.current); target != "old.AppImage" {
		t.Fatal("failed Pi upload did not restore old desktop")
	}
	failUpload = false
	if err := run(); err != nil || uploads != 2 {
		t.Fatalf("combined update: err=%v uploads=%d", err, uploads)
	}
	got, err := os.ReadFile(inst.current)
	if err != nil || string(got) != string(desktop) {
		t.Fatalf("updated desktop: %q, %v", got, err)
	}
	pi.Close()
	if err := run(); err == nil || uploads != 2 {
		t.Fatalf("disconnected Pi: err=%v uploads=%d", err, uploads)
	}
}

func TestUpdatePageRequiresConfirmationAndOrigin(t *testing.T) {
	a := &app{base: "http://127.0.0.1:1234", token: "secret"}
	request := httptest.NewRequest(http.MethodPost, a.base+"/secret/update", strings.NewReader("channel=stable&confirm=yes"))
	request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	response := httptest.NewRecorder()
	a.serve(response, request)
	if response.Code != http.StatusForbidden {
		t.Fatalf("untrusted origin returned %d", response.Code)
	}

	request = httptest.NewRequest(http.MethodPost, a.base+"/secret/update", strings.NewReader("channel=stable"))
	request.Header.Set("Origin", a.base)
	request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	response = httptest.NewRecorder()
	a.serve(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("missing confirmation returned %d", response.Code)
	}
}

func TestLoginFormOriginValidation(t *testing.T) {
	dir := t.TempDir()
	logPath := filepath.Join(dir, "calls")
	if err := os.WriteFile(filepath.Join(dir, "systemctl"), []byte("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$TEST_LOG\"\n"), 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir)
	t.Setenv("TEST_LOG", logPath)
	a := &app{base: "http://127.0.0.1:1234", token: "secret"}
	page := httptest.NewRecorder()
	a.serve(page, httptest.NewRequest(http.MethodGet, a.base+"/secret/login", nil))
	if page.Header().Get("Referrer-Policy") != "same-origin" {
		t.Fatal("login page must preserve same-origin form origins")
	}
	for _, tc := range []struct {
		name, origin, referer string
		status                int
	}{
		{"same origin", a.base, "", http.StatusOK},
		{"missing origin with same-page referer", "", a.base + "/secret/login", http.StatusOK},
		{"missing origin and referer", "", "", http.StatusForbidden},
		{"null origin", "null", a.base + "/secret/login", http.StatusForbidden},
		{"foreign origin", "https://example.test", a.base + "/secret/login", http.StatusForbidden},
		{"foreign referer", "", "https://example.test/secret/login", http.StatusForbidden},
		{"wrong local page", "", a.base + "/secret/update", http.StatusForbidden},
	} {
		t.Run(tc.name, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodPost, a.base+"/secret/login", strings.NewReader("action=enable"))
			request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			if tc.origin != "" {
				request.Header.Set("Origin", tc.origin)
			}
			if tc.referer != "" {
				request.Header.Set("Referer", tc.referer)
			}
			response := httptest.NewRecorder()
			a.serve(response, request)
			if response.Code != tc.status {
				t.Fatalf("got HTTP %d: %s", response.Code, response.Body.String())
			}
			if tc.status == http.StatusOK && !strings.Contains(response.Body.String(), "login service enabled") {
				t.Fatalf("enable was not acknowledged: %s", response.Body.String())
			}
		})
	}
	calls, err := os.ReadFile(logPath)
	if err != nil || string(calls) != "--user enable audiox-companion.service\n--user enable audiox-companion.service\n" {
		t.Fatalf("unexpected systemctl calls: %q, %v", calls, err)
	}
}

func TestManualUpdateRestartsAfterOldCompanionStops(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "started")
	launcher := filepath.Join(dir, "launcher")
	if err := os.WriteFile(launcher, []byte("#!/bin/sh\nprintf '%s\\n' \"$*\" > \"$TEST_MARK\"\n"), 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("TEST_MARK", marker)
	t.Setenv("INVOCATION_ID", "")
	old := exec.Command("sleep", "30")
	if err := old.Start(); err != nil {
		t.Fatal(err)
	}
	defer old.Process.Kill()
	a := &app{inst: installation{launcher: launcher}, parent: old.Process.Pid, args: []string{"--host", "127.0.0.1"}}
	if err := a.restart(); err != nil {
		t.Fatal(err)
	}
	old.Wait()
	for i := 0; i < 80; i++ {
		data, err := os.ReadFile(marker)
		if err == nil {
			if strings.TrimSpace(string(data)) != "--host 127.0.0.1" {
				t.Fatalf("wrong restart arguments: %q", data)
			}
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("updated companion did not restart")
}
