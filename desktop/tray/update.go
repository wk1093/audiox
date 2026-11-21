package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const githubAPI = "https://api.github.com/repos/wk1093/audiox"
const githubDownload = "https://github.com/wk1093/audiox/releases/download/"

var tagPattern = regexp.MustCompile(`^v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9]+)?$`)
var nightlyTag = regexp.MustCompile(`audiox-release-tag:\s*([^\s]+)`)
var modelPattern = regexp.MustCompile(`^Raspberry Pi ([45]) Model B(?:\s|$)`)

type asset struct {
	Name   string `json:"name"`
	State  string `json:"state"`
	Size   int64  `json:"size"`
	Digest string `json:"digest"`
}
type release struct {
	Tag        string  `json:"tag_name"`
	Body       string  `json:"body"`
	Draft      bool    `json:"draft"`
	Prerelease bool    `json:"prerelease"`
	Assets     []asset `json:"assets"`
}
type deviceInfo struct {
	Board   string `json:"board"`
	Model   string `json:"model"`
	Version string `json:"version"`
}

func fetchJSON(client *http.Client, address string, result interface{}) error {
	response, err := client.Get(address)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("%s returned HTTP %d", address, response.StatusCode)
	}
	return json.NewDecoder(io.LimitReader(response.Body, 2<<20)).Decode(result)
}

func latest(client *http.Client, api, channel string) (release, error) {
	var r release
	switch channel {
	case "stable":
		if err := fetchJSON(client, api+"/releases/latest", &r); err != nil {
			return r, err
		}
		if r.Prerelease || !regexp.MustCompile(`^v[0-9]+\.[0-9]+\.[0-9]+$`).MatchString(r.Tag) {
			return r, fmt.Errorf("latest release is not a stable AudioX release")
		}
	case "nightly":
		var rolling release
		if err := fetchJSON(client, api+"/releases/tags/nightly", &rolling); err != nil {
			return r, err
		}
		match := nightlyTag.FindStringSubmatch(rolling.Body)
		if rolling.Draft || !rolling.Prerelease || len(match) != 2 || !tagPattern.MatchString(match[1]) || !strings.Contains(match[1], "nightly") {
			return r, fmt.Errorf("nightly release does not reference an immutable nightly tag")
		}
		if err := fetchJSON(client, api+"/releases/tags/"+url.PathEscape(match[1]), &r); err != nil {
			return r, err
		}
		if !r.Prerelease || r.Tag != match[1] {
			return r, fmt.Errorf("nightly release metadata does not match its tag")
		}
	default:
		return r, fmt.Errorf("unknown release channel")
	}
	if r.Draft || !tagPattern.MatchString(r.Tag) {
		return r, fmt.Errorf("release is invalid or unpublished")
	}
	return r, nil
}

func board(client *http.Client, address string) (string, error) {
	var info deviceInfo
	if err := fetchJSON(client, address+"/api/system/info", &info); err != nil {
		return "", fmt.Errorf("cannot reach the connected Pi: %w", err)
	}
	if info.Version == "" {
		return "", fmt.Errorf("Pi did not report its firmware version")
	}
	if info.Board == "pi4" || info.Board == "pi5" {
		return info.Board, nil
	}
	if match := modelPattern.FindStringSubmatch(info.Model); len(match) == 2 {
		return "pi" + match[1], nil
	}
	return "", fmt.Errorf("unsupported or unknown Pi hardware")
}

func findAsset(r release, name string) (asset, error) {
	for _, a := range r.Assets {
		if a.Name == name && a.State == "uploaded" && a.Size >= 16 && a.Size <= 256<<20 {
			return a, nil
		}
	}
	return asset{}, fmt.Errorf("%s does not contain a valid %s", r.Tag, name)
}

func download(client *http.Client, downloads string, r release, a asset, dir string) (string, error) {
	base := downloads + url.PathEscape(r.Tag) + "/" + a.Name
	response, err := client.Get(base + ".sha256")
	if err != nil {
		return "", err
	}
	checksum, readErr := io.ReadAll(io.LimitReader(response.Body, 1024))
	response.Body.Close()
	if response.StatusCode != http.StatusOK || readErr != nil {
		return "", fmt.Errorf("cannot download checksum for %s", a.Name)
	}
	fields := strings.Fields(string(checksum))
	if len(fields) != 2 || fields[1] != a.Name || len(fields[0]) != 64 {
		return "", fmt.Errorf("invalid checksum for %s", a.Name)
	}
	expected, err := hex.DecodeString(fields[0])
	if err != nil || len(expected) != sha256.Size {
		return "", fmt.Errorf("invalid checksum for %s", a.Name)
	}
	if a.Digest != "" && a.Digest != "sha256:"+fields[0] {
		return "", fmt.Errorf("GitHub digest does not match checksum for %s", a.Name)
	}
	response, err = client.Get(base)
	if err != nil {
		return "", err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return "", fmt.Errorf("download %s returned HTTP %d", a.Name, response.StatusCode)
	}
	f, err := os.CreateTemp(dir, ".audiox-update-*")
	if err != nil {
		return "", err
	}
	name := f.Name()
	ok := false
	defer func() {
		f.Close()
		if !ok {
			os.Remove(name)
		}
	}()
	hash := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, hash), io.LimitReader(response.Body, a.Size+1))
	if err != nil || n != a.Size || !bytes.Equal(hash.Sum(nil), expected) {
		return "", fmt.Errorf("downloaded %s failed size or SHA-256 verification", a.Name)
	}
	if err = f.Sync(); err != nil {
		return "", err
	}
	ok = true
	return name, nil
}

func update(inst installation, piAddress, channel, expectedTag string) error {
	internet := &http.Client{Timeout: 10 * time.Minute}
	local := &http.Client{Timeout: 5 * time.Minute, Transport: &http.Transport{Proxy: nil}}
	return performUpdate(inst, piAddress, channel, expectedTag, githubAPI, githubDownload, internet, local)
}

func performUpdate(inst installation, piAddress, channel, expectedTag, api, downloads string, internet, local *http.Client) error {
	deviceBoard, err := board(local, piAddress)
	if err != nil {
		return err
	}
	r, err := latest(internet, api, channel)
	if err != nil {
		return err
	}
	if expectedTag != "" && r.Tag != expectedTag {
		return fmt.Errorf("the selected release changed; check and confirm the update again")
	}
	desktopAsset, err := findAsset(r, "audiox-companion-linux-x86_64.AppImage")
	if err != nil {
		return err
	}
	piAsset, err := findAsset(r, "audiox-update-"+deviceBoard+".axu")
	if err != nil {
		return err
	}
	images := filepath.Dir(inst.current)
	desktopFile, err := download(internet, downloads, r, desktopAsset, images)
	if err != nil {
		return err
	}
	defer os.Remove(desktopFile)
	piFile, err := download(internet, downloads, r, piAsset, inst.dir)
	if err != nil {
		return err
	}
	defer os.Remove(piFile)
	image, err := os.Open(desktopFile)
	if err != nil {
		return err
	}
	header := make([]byte, 4)
	_, headerErr := io.ReadFull(image, header)
	image.Close()
	if headerErr != nil || !bytes.Equal(header, []byte{0x7f, 'E', 'L', 'F'}) {
		return fmt.Errorf("invalid desktop AppImage")
	}
	f, err := os.Open(piFile)
	if err != nil {
		return err
	}
	defer f.Close()
	magic := make([]byte, 8)
	if _, err = io.ReadFull(f, magic); err != nil || string(magic) != "AXUPDATE" {
		return fmt.Errorf("invalid firmware archive")
	}
	if _, err = f.Seek(0, io.SeekStart); err != nil {
		return err
	}
	if currentBoard, err := board(local, piAddress); err != nil || currentBoard != deviceBoard {
		return fmt.Errorf("Pi disconnected or changed before upload")
	}
	if err = os.Chmod(desktopFile, 0755); err != nil {
		return err
	}
	imageName := "audiox-" + strings.TrimPrefix(desktopAsset.Digest, "sha256:") + ".AppImage"
	if desktopAsset.Digest == "" {
		contents, err := os.ReadFile(desktopFile)
		if err != nil {
			return err
		}
		hash := sha256.Sum256(contents)
		imageName = "audiox-" + hex.EncodeToString(hash[:]) + ".AppImage"
	}
	if err = os.Rename(desktopFile, filepath.Join(images, imageName)); err != nil {
		return err
	}
	old, err := os.Readlink(inst.current)
	if err != nil {
		return err
	}
	if err = activate(inst.current, imageName); err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodPost, piAddress+"/api/update", f)
	if err == nil {
		req.ContentLength = piAsset.Size
		req.Header.Set("Content-Type", "application/octet-stream")
		var response *http.Response
		response, err = local.Do(req)
		if err == nil {
			body, readErr := io.ReadAll(io.LimitReader(response.Body, 4096))
			response.Body.Close()
			if readErr != nil || response.StatusCode < 200 || response.StatusCode >= 300 {
				err = fmt.Errorf("Pi rejected update (HTTP %d): %s", response.StatusCode, strings.TrimSpace(string(body)))
			}
		}
	}
	if err != nil {
		if rollbackErr := activate(inst.current, old); rollbackErr != nil {
			return fmt.Errorf("firmware upload failed (%v), and desktop rollback failed: %w", err, rollbackErr)
		}
		return fmt.Errorf("firmware upload failed; the Pi may have rebooted, check its version: %w", err)
	}
	return nil
}
