package main

import (
	"fmt"
	"image"
	"image/color"
	_ "image/png"
	"log"
	"os"
	"time"

	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"github.com/godbus/dbus/v5/prop"
)

const itemInterface = "org.kde.StatusNotifierItem"
const menuInterface = "com.canonical.dbusmenu"

type iconPixmap struct {
	Width  int32
	Height int32
	Data   []byte
}

func icon(path string) ([]iconPixmap, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return nil, err
	}
	bounds := img.Bounds()
	if bounds.Empty() {
		return nil, fmt.Errorf("empty icon")
	}
	const size = 64
	data := make([]byte, size*size*4)
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			p := color.NRGBAModel.Convert(img.At(bounds.Min.X+x*bounds.Dx()/size, bounds.Min.Y+y*bounds.Dy()/size)).(color.NRGBA)
			i := (y*size + x) * 4
			data[i], data[i+1], data[i+2], data[i+3] = p.A, p.R, p.G, p.B
		}
	}
	return []iconPixmap{{size, size, data}}, nil
}

type menuLayout struct {
	ID         int32
	Properties map[string]dbus.Variant
	Children   []dbus.Variant
}

type menuEntry struct {
	ID         int32
	Properties map[string]dbus.Variant
}

type tray struct {
	open func(string)
}

func (*tray) Activate(int32, int32) *dbus.Error          { return nil }
func (*tray) SecondaryActivate(int32, int32) *dbus.Error { return nil }
func (*tray) Scroll(int32, string) *dbus.Error           { return nil }
func (*tray) ContextMenu(int32, int32) *dbus.Error       { return nil }

func menuProps(id int32) map[string]dbus.Variant {
	props := map[string]dbus.Variant{"enabled": dbus.MakeVariant(true), "visible": dbus.MakeVariant(true)}
	switch id {
	case 0:
		props["children-display"] = dbus.MakeVariant("submenu")
	case 1:
		props["label"] = dbus.MakeVariant("Open AudioX")
	case 2:
		props["label"] = dbus.MakeVariant("Update desktop and Pi…")
	case 3:
		props["label"] = dbus.MakeVariant("Login service…")
	}
	return props
}

func (*tray) GetLayout(parent, depth int32, names []string) (uint32, menuLayout, *dbus.Error) {
	if parent < 0 || parent > 3 {
		return 0, menuLayout{}, dbus.NewError("com.canonical.dbusmenu.Error.InvalidMenuItem", nil)
	}
	layout := menuLayout{parent, menuProps(parent), []dbus.Variant{}}
	if parent == 0 && depth != 0 {
		for id := int32(1); id <= 3; id++ {
			layout.Children = append(layout.Children, dbus.MakeVariant(menuLayout{id, menuProps(id), []dbus.Variant{}}))
		}
	}
	return 1, layout, nil
}

func (*tray) GetGroupProperties(ids []int32, names []string) ([]menuEntry, *dbus.Error) {
	entries := make([]menuEntry, 0, len(ids))
	for _, id := range ids {
		if id >= 0 && id <= 3 {
			entries = append(entries, menuEntry{id, menuProps(id)})
		}
	}
	return entries, nil
}

func (*tray) GetProperty(id int32, name string) (dbus.Variant, *dbus.Error) {
	value, ok := menuProps(id)[name]
	if !ok {
		return dbus.Variant{}, dbus.NewError("com.canonical.dbusmenu.Error.InvalidProperty", nil)
	}
	return value, nil
}

func (t *tray) Event(id int32, event string, data dbus.Variant, timestamp uint32) *dbus.Error {
	if event == "clicked" {
		switch id {
		case 1:
			go t.open("device")
		case 2:
			go t.open("update")
		case 3:
			go t.open("login")
		}
	}
	return nil
}

func (*tray) AboutToShow(int32) (bool, *dbus.Error) { return false, nil }

func startTray(iconPath string, open func(string)) (*dbus.Conn, error) {
	pixels, err := icon(iconPath)
	if err != nil {
		return nil, err
	}
	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		return nil, err
	}
	reply, err := conn.RequestName("org.wk1093.AudioX.Tray", dbus.NameFlagDoNotQueue)
	if err != nil || reply != dbus.RequestNameReplyPrimaryOwner {
		conn.Close()
		return nil, fmt.Errorf("another AudioX tray is already running")
	}
	t := &tray{open: open}
	itemPath := dbus.ObjectPath("/StatusNotifierItem")
	menuPath := dbus.ObjectPath("/Menu")
	if err = conn.Export(t, itemPath, itemInterface); err != nil {
		conn.Close()
		return nil, err
	}
	if err = conn.Export(t, menuPath, menuInterface); err != nil {
		conn.Close()
		return nil, err
	}
	itemProps, err := prop.Export(conn, itemPath, prop.Map{itemInterface: {
		"Category": {Value: "ApplicationStatus"}, "Id": {Value: "audiox-companion"},
		"Title": {Value: "AudioX Companion"}, "Status": {Value: "Active"},
		"WindowId": {Value: uint32(0)}, "IconName": {Value: ""},
		"IconPixmap": {Value: pixels}, "OverlayIconName": {Value: ""},
		"OverlayIconPixmap": {Value: []iconPixmap{}}, "AttentionIconName": {Value: ""},
		"AttentionIconPixmap": {Value: []iconPixmap{}},
		"AttentionMovieName":  {Value: ""},
		"ToolTip": {Value: struct {
			IconName string
			Pixmaps  []iconPixmap
			Title    string
			Text     string
		}{"", pixels, "AudioX", "AudioX desktop companion"}},
		"ItemIsMenu": {Value: true}, "Menu": {Value: menuPath},
	}})
	if err != nil {
		conn.Close()
		return nil, err
	}
	menuPropsDBus, err := prop.Export(conn, menuPath, prop.Map{menuInterface: {
		"Version": {Value: uint32(4)}, "TextDirection": {Value: "ltr"},
		"Status": {Value: "normal"}, "IconThemePath": {Value: []string{}},
	}})
	if err != nil {
		conn.Close()
		return nil, err
	}
	itemMethods := introspect.Methods(t)
	menuMethods := introspect.Methods(t)
	for _, obj := range []struct {
		path  dbus.ObjectPath
		iface introspect.Interface
	}{
		{itemPath, introspect.Interface{Name: itemInterface, Methods: itemMethods, Properties: itemProps.Introspection(itemInterface)}},
		{menuPath, introspect.Interface{Name: menuInterface, Methods: menuMethods, Properties: menuPropsDBus.Introspection(menuInterface)}},
	} {
		node := &introspect.Node{Interfaces: []introspect.Interface{obj.iface, introspect.IntrospectData, prop.IntrospectData}}
		if err = conn.Export(introspect.NewIntrospectable(node), obj.path, "org.freedesktop.DBus.Introspectable"); err != nil {
			conn.Close()
			return nil, err
		}
	}
	go func() {
		watcher := conn.Object("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher")
		lastOwner := ""
		for {
			var owner string
			err := conn.BusObject().Call("org.freedesktop.DBus.GetNameOwner", 0, "org.kde.StatusNotifierWatcher").Store(&owner)
			if err == nil && owner != lastOwner {
				err = watcher.Call("org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem", 0, "org.wk1093.AudioX.Tray").Err
				if err != nil {
					log.Printf("AudioX: could not register system tray: %v", err)
				}
			}
			if err != nil {
				lastOwner = ""
			} else {
				lastOwner = owner
			}
			time.Sleep(3 * time.Second)
		}
	}()
	return conn, nil
}
