package main

import (
	"os"
	"testing"
	"time"

	"github.com/godbus/dbus/v5"
)

type testWatcher struct {
	registered chan string
}

func (w *testWatcher) RegisterStatusNotifierItem(name string) *dbus.Error {
	w.registered <- name
	return nil
}

func TestStatusNotifier(t *testing.T) {
	if os.Getenv("DBUS_SESSION_BUS_ADDRESS") == "" {
		t.Skip("requires an isolated session bus")
	}
	watcher, err := dbus.ConnectSessionBus()
	if err != nil {
		t.Fatal(err)
	}
	defer watcher.Close()
	if _, err = watcher.RequestName("org.kde.StatusNotifierWatcher", dbus.NameFlagDoNotQueue); err != nil {
		t.Fatal(err)
	}
	w := &testWatcher{registered: make(chan string, 1)}
	if err = watcher.Export(w, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher"); err != nil {
		t.Fatal(err)
	}
	clicked := make(chan string, 1)
	tray, err := startTray("../../assets/branding/logo_small.png", func(page string) { clicked <- page })
	if err != nil {
		t.Fatal(err)
	}
	defer tray.Close()
	select {
	case name := <-w.registered:
		if name != "org.wk1093.AudioX.Tray" {
			t.Fatalf("registered wrong item %q", name)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("tray did not register")
	}
	item := watcher.Object("org.wk1093.AudioX.Tray", "/StatusNotifierItem")
	var pixmap dbus.Variant
	if err := item.Call("org.freedesktop.DBus.Properties.Get", 0, itemInterface, "IconPixmap").Store(&pixmap); err != nil {
		t.Fatal(err)
	}
	var pixels []iconPixmap
	if err := dbus.Store([]interface{}{pixmap.Value()}, &pixels); err != nil || len(pixels) != 1 || pixels[0].Width != 64 || len(pixels[0].Data) != 64*64*4 {
		t.Fatalf("invalid icon pixmap: %v", err)
	}
	menu := watcher.Object("org.wk1093.AudioX.Tray", "/Menu")
	var revision uint32
	var layout menuLayout
	if err := menu.Call(menuInterface+".GetLayout", 0, int32(0), int32(1), []string{}).Store(&revision, &layout); err != nil {
		t.Fatal(err)
	}
	if revision != 1 || len(layout.Children) != 3 {
		t.Fatalf("invalid tray menu: %#v", layout)
	}
	if err := menu.Call(menuInterface+".Event", 0, int32(2), "clicked", dbus.MakeVariant(int32(0)), uint32(0)).Err; err != nil {
		t.Fatal(err)
	}
	select {
	case page := <-clicked:
		if page != "update" {
			t.Fatalf("wrong tray action: %s", page)
		}
	case <-time.After(time.Second):
		t.Fatal("tray action was not delivered")
	}
}
