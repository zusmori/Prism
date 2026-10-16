//go:build windows

package main

// Tray-Symbol: X schliesst ZLauncher nicht, sondern legt es in den Infobereich der
// Taskleiste. Von dort: Linksklick = oeffnen, Rechtsklick = Menue (Oeffnen / Beenden).

import (
	"os"
	"path/filepath"
	"unsafe"

	"golang.org/x/sys/windows"
)

type notifyIconData struct {
	CbSize           uint32
	HWnd             uintptr
	UID              uint32
	UFlags           uint32
	UCallbackMessage uint32
	HIcon            uintptr
	SzTip            [128]uint16
	DwState          uint32
	DwStateMask      uint32
	SzInfo           [256]uint16
	UVersion         uint32
	SzInfoTitle      [64]uint16
	DwInfoFlags      uint32
	GuidItem         windows.GUID
	HBalloonIcon     uintptr
}

const (
	wmTray       = 0x8000 + 1 // WM_APP + 1
	nimAdd       = 0
	nimModify    = 1
	nimDelete    = 2
	nifMessage   = 0x1
	nifIcon      = 0x2
	nifTip       = 0x4
	nifInfo      = 0x10
	trayCmdOpen  = 1
	trayCmdPlay  = 2
	trayCmdQuit  = 3
)

var (
	shell32         = windows.NewLazySystemDLL("shell32.dll")
	pNotify         = shell32.NewProc("Shell_NotifyIconW")
	pLoadImage      = user32.NewProc("LoadImageW")
	pCreatePopup    = user32.NewProc("CreatePopupMenu")
	pAppendMenu     = user32.NewProc("AppendMenuW")
	pTrackPopup     = user32.NewProc("TrackPopupMenu")
	pDestroyMenu    = user32.NewProc("DestroyMenu")
	pSetForeground  = user32.NewProc("SetForegroundWindow")
	pGetCursorPos   = user32.NewProc("GetCursorPos")
	pRegisterMsg    = user32.NewProc("RegisterWindowMessageW")
	pIsVisible      = user32.NewProc("IsWindowVisible")
	kernel32        = windows.NewLazySystemDLL("kernel32.dll")
	pGetModule      = kernel32.NewProc("GetModuleHandleW")
	trayHwnd        uintptr
	trayIcon        uintptr
	trayOn          bool
	wmTaskbarCreate uintptr
)

func trayData() notifyIconData {
	var d notifyIconData
	d.CbSize = uint32(unsafe.Sizeof(d))
	d.HWnd = trayHwnd
	d.UID = 1
	return d
}

func setUTF16(dst []uint16, s string) {
	u, _ := windows.UTF16FromString(s)
	if len(u) > len(dst) {
		u = append(u[:len(dst)-1], 0)
	}
	copy(dst, u)
}

func trayAdd(hwnd uintptr) {
	trayHwnd = hwnd
	if wmTaskbarCreate == 0 {
		name, _ := windows.UTF16PtrFromString("TaskbarCreated")
		wmTaskbarCreate, _, _ = pRegisterMsg.Call(uintptr(unsafe.Pointer(name)))
	}
	if trayIcon == 0 {
		inst, _, _ := pGetModule.Call(0)
		cx := uintptr(metric(49)) // SM_CXSMICON
		trayIcon, _, _ = pLoadImage.Call(inst, 1, 1, cx, cx, 0)
	}
	d := trayData()
	d.UFlags = nifMessage | nifIcon | nifTip
	d.UCallbackMessage = wmTray
	d.HIcon = trayIcon
	setUTF16(d.SzTip[:], "zusmori's Launcher")
	r, _, _ := pNotify.Call(nimAdd, uintptr(unsafe.Pointer(&d)))
	trayOn = r != 0
}

func trayRemove() {
	if !trayOn {
		return
	}
	d := trayData()
	pNotify.Call(nimDelete, uintptr(unsafe.Pointer(&d)))
	trayOn = false
}

// Einmaliger Hinweis beim ersten Verstecken
func trayBalloonOnce() {
	flag := filepath.Join(dataDir, "tray_hint")
	if _, err := os.Stat(flag); err == nil || !trayOn {
		return
	}
	_ = os.WriteFile(flag, []byte("1"), 0644)
	d := trayData()
	d.UFlags = nifInfo
	setUTF16(d.SzInfoTitle[:], "ZLauncher is still running")
	setUTF16(d.SzInfo[:], "ZLauncher keeps running here. Right-click the icon to quit.")
	d.DwInfoFlags = 0x4 // NIIF_USER (App-Icon)
	pNotify.Call(nimModify, uintptr(unsafe.Pointer(&d)))
}

func hideToTray(hwnd uintptr) {
	closeAck, allowClose = true, false
	if !trayOn {
		trayAdd(hwnd)
	}
	pShow.Call(hwnd, 0) // SW_HIDE
	setPresenceHidden(true)
	if gw != nil {
		w := gw
		go w.Dispatch(func() { w.Eval("window.__tray && window.__tray(true)") })
	}
	trayBalloonOnce()
}

func showFromTray(hwnd uintptr) {
	setPresenceHidden(false)
	if gw != nil {
		gw.Eval("window.__tray && window.__tray(false)")
	}
	if v, _, _ := pIsVisible.Call(hwnd); v == 0 {
		pShow.Call(hwnd, 5) // SW_SHOW
	} else if isMinimized(hwnd) {
		pShow.Call(hwnd, 9) // SW_RESTORE
	}
	pSetForeground.Call(hwnd)
}

var pIsIconic = user32.NewProc("IsIconic")

func isMinimized(hwnd uintptr) bool { r, _, _ := pIsIconic.Call(hwnd); return r != 0 }

func trayMenu(hwnd uintptr) {
	m, _, _ := pCreatePopup.Call()
	if m == 0 {
		return
	}
	defer pDestroyMenu.Call(m)
	add := func(id uintptr, text string) {
		t, _ := windows.UTF16PtrFromString(text)
		pAppendMenu.Call(m, 0, id, uintptr(unsafe.Pointer(t)))
	}
	add(trayCmdOpen, "Open ZLauncher")
	add(trayCmdPlay, "Play Rocket League")
	pAppendMenu.Call(m, 0x800, 0, 0) // MF_SEPARATOR
	add(trayCmdQuit, "Quit")
	var pt [2]int32
	pGetCursorPos.Call(uintptr(unsafe.Pointer(&pt)))
	pSetForeground.Call(hwnd)
	// TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN
	cmd, _, _ := pTrackPopup.Call(m, 0x0100|0x0002|0x0020, uintptr(pt[0]), uintptr(pt[1]), 0, hwnd, 0)
	pPost.Call(hwnd, 0, 0, 0) // WM_NULL, damit das Menue sauber schliesst
	switch cmd {
	case trayCmdOpen:
		showFromTray(hwnd)
	case trayCmdPlay:
		showFromTray(hwnd)
		if gw != nil {
			w := gw
			go w.Dispatch(func() { w.Eval("window.__trayPlay && window.__trayPlay()") })
		}
	case trayCmdQuit:
		trayQuit(hwnd)
	}
}

// Richtig beenden: Oberflaeche fragt ggf. noch nach (getauschte Maps), dann zu.
func trayQuit(hwnd uintptr) {
	if gw == nil {
		allowClose = true
		pPost.Call(hwnd, 0x0010, 0, 0)
		return
	}
	w := gw
	go w.Dispatch(func() { w.Eval("window.__quit ? window.__quit() : 0") })
}

// Nachrichten des Tray-Symbols (aus subclassProc)
func trayMessage(hwnd, msg, lp uintptr) bool {
	if msg == wmTaskbarCreate && wmTaskbarCreate != 0 { // Explorer neu gestartet
		trayOn = false
		trayAdd(hwnd)
		return true
	}
	if msg != wmTray {
		return false
	}
	switch lp & 0xFFFF {
	case 0x0202, 0x0203: // WM_LBUTTONUP, WM_LBUTTONDBLCLK
		showFromTray(hwnd)
	case 0x0205: // WM_RBUTTONUP
		trayMenu(hwnd)
	}
	return true
}
