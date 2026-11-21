package main

import (
	"crypto/rand"
	"encoding/hex"
	"flag"
	"fmt"
	"html"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/godbus/dbus/v5"
)

type app struct {
	inst       installation
	address    string
	base       string
	token      string
	parent     int
	args       []string
	updating   sync.Mutex
	installErr error
}

func (a *app) open(page string) {
	address := a.base + "/" + a.token + "/" + page
	if page == "device" {
		address = a.address
	}
	if err := exec.Command("xdg-open", address).Run(); err == nil {
		return
	}
	conn, err := dbus.ConnectSessionBus()
	if err == nil {
		defer conn.Close()
		call := conn.Object("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop").
			Call("org.freedesktop.portal.OpenURI.OpenURI", 0, "", address, map[string]dbus.Variant{})
		if call.Err == nil {
			return
		}
	}
	log.Printf("AudioX: could not open a browser via xdg-open or the desktop portal")
}

func page(w http.ResponseWriter, title, body string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Referrer-Policy", "same-origin")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'")
	fmt.Fprintf(w, `<!doctype html><meta charset="utf-8"><title>%s</title><style>body{font:16px sans-serif;max-width:42rem;margin:3rem auto;padding:0 1rem;line-height:1.5}button,select{font:inherit;padding:.5rem}label{display:block;margin:1rem 0}</style><h1>%s</h1>%s`, html.EscapeString(title), html.EscapeString(title), body)
}

func (a *app) serve(w http.ResponseWriter, r *http.Request) {
	prefix := "/" + a.token + "/"
	if !strings.HasPrefix(r.URL.Path, prefix) || r.Host != strings.TrimPrefix(a.base, "http://") {
		http.NotFound(w, r)
		return
	}
	if r.Method == http.MethodPost {
		origin := r.Header.Get("Origin")
		if origin != a.base && (origin != "" || r.Referer() != a.base+r.URL.EscapedPath()) {
			http.Error(w, "Invalid origin", http.StatusForbidden)
			return
		}
	}
	if r.Method == http.MethodPost {
		r.Body = http.MaxBytesReader(w, r.Body, 4096)
	}
	if a.installErr != nil {
		page(w, "AudioX installation failed", "<p>"+html.EscapeString(a.installErr.Error())+"</p>")
		return
	}
	switch strings.TrimPrefix(r.URL.Path, prefix) {
	case "login":
		if r.Method == http.MethodGet {
			page(w, "Start AudioX at login?", `<p>AudioX is installed for your user. Enabling the service starts it automatically on future logins.</p><form method="post"><button name="action" value="enable">Enable at login</button> <button name="action" value="disable">Disable at login</button></form><p>You can close this page without enabling it.</p>`)
			return
		}
		if r.Method != http.MethodPost || r.ParseForm() != nil {
			http.Error(w, "Invalid request", http.StatusBadRequest)
			return
		}
		action := r.PostForm.Get("action")
		if action != "enable" && action != "disable" {
			http.Error(w, "Invalid action", http.StatusBadRequest)
			return
		}
		output, err := exec.Command("systemctl", "--user", action, "audiox-companion.service").CombinedOutput()
		if err != nil {
			page(w, "Login service error", "<p>"+html.EscapeString(strings.TrimSpace(string(output))+" "+err.Error())+"</p>")
			return
		}
		page(w, "Login service updated", "<p>AudioX login service "+html.EscapeString(action)+"d. This takes effect at the next login.</p>")
	case "update":
		if r.Method == http.MethodGet {
			page(w, "Update AudioX", `<p>The desktop and connected Pi will update to the same release. The Pi will reboot and audio will be interrupted. This can replace a newer version.</p><form method="post"><label>Release channel <select name="channel"><option value="stable">Stable</option><option value="nightly">Nightly</option></select></label><button name="action" value="check">Check release</button></form>`)
			return
		}
		if r.Method != http.MethodPost || r.ParseForm() != nil {
			http.Error(w, "Invalid request", http.StatusBadRequest)
			return
		}
		channel := r.PostForm.Get("channel")
		if r.PostForm.Get("action") == "check" {
			local := &http.Client{Timeout: 15 * time.Second, Transport: &http.Transport{Proxy: nil}}
			boardName, err := board(local, a.address)
			if err != nil {
				page(w, "Update check failed", "<p>"+html.EscapeString(err.Error())+"</p>")
				return
			}
			rel, err := latest(&http.Client{Timeout: 30 * time.Second}, githubAPI, channel)
			if err == nil {
				_, err = findAsset(rel, "audiox-companion-linux-x86_64.AppImage")
			}
			if err == nil {
				_, err = findAsset(rel, "audiox-update-"+boardName+".axu")
			}
			if err != nil {
				page(w, "Update check failed", "<p>"+html.EscapeString(err.Error())+"</p>")
				return
			}
			body := "<p>Selected: <strong>" + html.EscapeString(rel.Tag) + "</strong> for " +
				html.EscapeString(boardName) + `. This can replace a newer version. The Pi will reboot.</p><form method="post"><input type="hidden" name="channel" value="` +
				html.EscapeString(channel) + `"><input type="hidden" name="tag" value="` + html.EscapeString(rel.Tag) +
				`"><label><input type="checkbox" name="confirm" value="yes" required> I understand that the Pi will reboot.</label><button name="action" value="install">Download and update both</button></form><p>Keep this page open while the update runs.</p>`
			page(w, "Confirm AudioX update", body)
			return
		}
		tag := r.PostForm.Get("tag")
		if r.PostForm.Get("action") != "install" || r.PostForm.Get("confirm") != "yes" || !tagPattern.MatchString(tag) {
			http.Error(w, "Confirmation required", http.StatusBadRequest)
			return
		}
		if !a.updating.TryLock() {
			http.Error(w, "An update is already running", http.StatusConflict)
			return
		}
		defer a.updating.Unlock()
		if err := update(a.inst, a.address, channel, tag); err != nil {
			log.Printf("AudioX: update failed: %v", err)
			page(w, "Update failed", "<p>"+html.EscapeString(err.Error())+"</p><p>No desktop restart was requested.</p>")
			return
		}
		if err := a.restart(); err != nil {
			log.Printf("AudioX: update installed, but restart failed: %v", err)
			page(w, "Update installed; restart failed", "<p>The Pi is rebooting. Restart the desktop companion manually: "+html.EscapeString(err.Error())+"</p>")
			return
		}
		page(w, "Update installed", "<p>The Pi is rebooting. The desktop companion will restart shortly.</p>")
		if f, ok := w.(http.Flusher); ok {
			f.Flush()
		}
	default:
		http.NotFound(w, r)
	}
}

func (a *app) restart() error {
	if os.Getenv("INVOCATION_ID") == "" {
		const restartScript = `pid=$1; shift
attempts=0
while kill -0 "$pid" 2>/dev/null; do
  if [ "$attempts" -ge 30 ]; then echo "AudioX: old companion did not stop; restart manually." >&2; exit 1; fi
  sleep 1
  attempts=$((attempts + 1))
done
sleep 2
exec "$@"`
		args := append([]string{"-c", restartScript, "audiox-restart", strconv.Itoa(a.parent), a.inst.launcher}, a.args...)
		cmd := exec.Command("/bin/sh", args...)
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		if err := cmd.Start(); err != nil {
			return err
		}
	}
	go func() {
		time.Sleep(time.Second)
		if err := syscall.Kill(a.parent, syscall.SIGTERM); err != nil {
			log.Printf("AudioX: update installed, but could not stop old companion: %v", err)
		}
	}()
	return nil
}

func run() error {
	flags := flag.NewFlagSet("audiox-tray", flag.ContinueOnError)
	iconPath := flags.String("icon", "", "packaged icon")
	image := flags.String("appimage", "", "source AppImage")
	installOnly := flags.Bool("install-only", false, "install the AppImage without starting the tray")
	host := flags.String("host", "172.31.73.1", "Pi IPv4 address")
	port := flags.Uint("port", 80, "Pi HTTP port")
	if err := flags.Parse(os.Args[1:]); err != nil {
		return err
	}
	if *installOnly {
		if *image == "" {
			return fmt.Errorf("installation requires an AppImage path")
		}
		inst, _, err := installImage(*image)
		if err != nil {
			return fmt.Errorf("installation failed: %w", err)
		}
		fmt.Printf("AudioX: installed %s\n", inst.current)
		return nil
	}
	if net.ParseIP(*host) == nil || net.ParseIP(*host).To4() == nil || *port == 0 || *port > 65535 {
		return fmt.Errorf("invalid Pi address")
	}
	parent := os.Getppid()
	var inst installation
	var first bool
	var installErr error
	if *image == "" {
		installErr = fmt.Errorf("the AppImage path is unavailable; cannot install the desktop companion")
	} else {
		inst, first, installErr = installImage(*image)
	}
	err := installErr
	if err != nil {
		log.Printf("AudioX: installation: %v", err)
	}
	rawToken := make([]byte, 24)
	if _, err = rand.Read(rawToken); err != nil {
		return err
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	a := &app{inst: inst, address: "http://" + net.JoinHostPort(*host, strconv.Itoa(int(*port))),
		base: "http://" + listener.Addr().String(), token: hex.EncodeToString(rawToken), parent: parent,
		args: []string{"--host", *host, "--port", strconv.Itoa(int(*port))}, installErr: installErr}
	server := &http.Server{Handler: http.HandlerFunc(a.serve), ReadHeaderTimeout: 5 * time.Second}
	go func() {
		if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
			log.Printf("AudioX: local update page failed: %v", err)
		}
	}()
	conn, err := startTray(*iconPath, a.open)
	if err != nil {
		server.Close()
		return err
	}
	defer conn.Close()
	defer server.Close()
	if first {
		go a.open("login")
	}
	for os.Getppid() == parent {
		time.Sleep(time.Second)
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		log.Printf("AudioX: %v", err)
		os.Exit(1)
	}
}
