//go:build windows

package main

// "Start with Windows": Eintrag unter HKCU\...\Run, Prism startet dann still im Tray (--tray).

import (
	"os"
	"strings"

	"golang.org/x/sys/windows/registry"
)

const runKey = `Software\Microsoft\Windows\CurrentVersion\Run`
const runName = "Prism"

var startTray bool // mit --tray gestartet: Fenster nicht zeigen, nur Tray-Symbol

func autostartCmd() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return `"` + exe + `" --tray`
}

func autostartOn() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, runKey, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	_, _, err = k.GetStringValue(runName)
	return err == nil
}

func setAutostart(on bool) error {
	k, _, err := registry.CreateKey(registry.CURRENT_USER, runKey, registry.SET_VALUE|registry.QUERY_VALUE)
	if err != nil {
		return err
	}
	defer k.Close()
	if !on {
		if err := k.DeleteValue(runName); err != nil && err != registry.ErrNotExist {
			return err
		}
		return nil
	}
	cmd := autostartCmd()
	if cmd == "" {
		return os.ErrNotExist
	}
	return k.SetStringValue(runName, cmd)
}

// Beim Start: Pfad im Eintrag aktuell halten (z. B. nach Umbenennen/Verschieben der exe)
func autostartRefresh() {
	k, err := registry.OpenKey(registry.CURRENT_USER, runKey, registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return
	}
	defer k.Close()
	cur, _, err := k.GetStringValue(runName)
	if err != nil {
		return
	}
	if want := autostartCmd(); want != "" && !strings.EqualFold(cur, want) {
		_ = k.SetStringValue(runName, want)
	}
}

func hasArg(a string) bool {
	for _, x := range os.Args[1:] {
		if strings.EqualFold(x, a) {
			return true
		}
	}
	return false
}
