//go:build windows

package main

// Nur ein ZLauncher gleichzeitig: ein zweiter Start holt das laufende Fenster nach vorne
// (auch aus dem Tray) und beendet sich dann selbst.

import (
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	pRegisterWindowMessage = user32.NewProc("RegisterWindowMessageW")
	pPostMessage           = user32.NewProc("PostMessageW")
	pAllowSetForeground    = user32.NewProc("AllowSetForegroundWindow")
	instanceMutex          windows.Handle
	showMsg                uintptr // eigene Nachricht "ZLauncher.Show"
)

func init() {
	if p, err := windows.UTF16PtrFromString("ZLauncher.Show"); err == nil {
		showMsg, _, _ = pRegisterWindowMessage.Call(uintptr(unsafe.Pointer(p)))
	}
}

// true = wir sind die einzige Instanz. wait: so lange auf eine alte Instanz warten (z. B. nach einem Update)
func singleInstance(wait time.Duration) bool {
	name, err := windows.UTF16PtrFromString(`Local\ZLauncher.SingleInstance`)
	if err != nil {
		return true
	}
	deadline := time.Now().Add(wait)
	for {
		h, err := windows.CreateMutex(nil, false, name)
		if err == nil {
			instanceMutex = h
			return true
		}
		if h != 0 {
			windows.CloseHandle(h)
		}
		if err != windows.ERROR_ALREADY_EXISTS {
			return true // im Zweifel normal starten
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(250 * time.Millisecond)
	}
}

// laufende Instanz bitten, ihr Fenster zu zeigen (erreicht auch das versteckte Tray-Fenster)
func wakeOtherInstance() {
	if showMsg != 0 {
		pAllowSetForeground.Call(^uintptr(0)) // ASFW_ANY: die laufende Instanz darf ihr Fenster nach vorne holen
		pPostMessage.Call(0xFFFF /* HWND_BROADCAST */, showMsg, 0, 0)
	}
}
