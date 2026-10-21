//go:build windows

package main

// Nur ein ZLauncher gleichzeitig: ein zweiter Start holt das laufende Fenster nach vorne
// (auch aus dem Tray) und beendet sich dann selbst. Ist die neu gestartete exe aber neuer
// (z. B. gerade heruntergeladen), beendet sich die alte und die neue startet.

import (
	"os"
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
	exeStamp               = exeTime()
)

// Aenderungszeit der eigenen exe (Sekunden), 0 = unbekannt
func exeTime() int64 {
	if exe, err := os.Executable(); err == nil {
		if fi, err := os.Stat(exe); err == nil {
			return fi.ModTime().Unix()
		}
	}
	return 0
}

// Nachricht der zweiten Instanz: lp = Zeit ihrer exe. Neuer als wir -> wir beenden uns (true)
func otherIsNewer(lp uintptr) bool {
	t := int64(lp)
	return t > 0 && exeStamp > 0 && t > exeStamp+1
}

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

// laufende Instanz bitten, ihr Fenster zu zeigen (erreicht auch das versteckte Tray-Fenster).
// Sie beendet sich, wenn unsere exe neuer ist.
func wakeOtherInstance() {
	if showMsg != 0 {
		pAllowSetForeground.Call(^uintptr(0)) // ASFW_ANY: die laufende Instanz darf ihr Fenster nach vorne holen
		pPostMessage.Call(0xFFFF /* HWND_BROADCAST */, showMsg, 0, uintptr(exeStamp))
	}
}
