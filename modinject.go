//go:build windows

package main

// Eigener In-Game-Mod (zlmod.dll, Quellcode in mod/):
// ZLauncher legt die DLL in den App-Ordner und laedt sie nach dem Start ohne Anti-Cheat ins Spiel.
// Im Spiel oeffnet F8 das Item-Menue. Mit Anti-Cheat passiert hier nie etwas.

import (
	"bytes"
	_ "embed"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

//go:embed mod/bin/zlmod.dll
var modDLL []byte

var (
	modBusy    int32
	modLastErr atomic.Value
)

func init() { modLastErr.Store("") }

// Das Prism-Menue (F8) kommt immer mit, wenn ohne Anti-Cheat gestartet wird (mit Anti-Cheat nie)
func modEnabled() bool { return true }

// DLL in den App-Ordner schreiben (nur wenn sie sich geaendert hat)
func modFile() (string, error) {
	p := filepath.Join(dataDir, "zlmod.dll")
	if cur, err := os.ReadFile(p); err == nil && bytes.Equal(cur, modDLL) {
		return p, nil
	}
	tmp := p + ".new"
	if err := os.WriteFile(tmp, modDLL, 0644); err != nil {
		return "", err
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		if _, e := os.Stat(p); e == nil {
			return p, nil // alte Version ist gerade geladen, die nehmen wir
		}
		return "", err
	}
	return p, nil
}

// Nach dem Start ohne Anti-Cheat aufrufen
func modOnLaunch() {
	if !modEnabled() || len(modDLL) < 1000 {
		return
	}
	if !atomic.CompareAndSwapInt32(&modBusy, 0, 1) {
		return
	}
	go func() {
		defer atomic.StoreInt32(&modBusy, 0)
		modLastErr.Store("")
		dll, err := modFile()
		if err != nil {
			modLastErr.Store("Couldn't prepare the item menu.")
			return
		}
		start := time.Now()
		var pid uint32
		for pid == 0 {
			if time.Since(start) > 3*time.Minute {
				return
			}
			time.Sleep(500 * time.Millisecond)
			pid = rlPID()
		}
		// warten, bis das Spielfenster da ist; der Mod wartet danach selbst auf die Engine
		for i := 0; i < 120 && !windowOf(pid); i++ {
			time.Sleep(time.Second)
		}
		time.Sleep(3 * time.Second)
		if eacRunning() || rlPID() != pid || moduleLoaded(pid, "zlmod.dll") {
			return
		}
		if err := injectDLL(pid, dll); err != nil {
			modLastErr.Store(err.Error())
		}
	}()
}

var (
	pEnumWindows        = user32.NewProc("EnumWindows")
	pGetWindowThreadPI  = user32.NewProc("GetWindowThreadProcessId")
	pVirtualAllocEx     = windows.NewLazySystemDLL("kernel32.dll").NewProc("VirtualAllocEx")
	pVirtualFreeEx      = windows.NewLazySystemDLL("kernel32.dll").NewProc("VirtualFreeEx")
	pCreateRemoteThread = windows.NewLazySystemDLL("kernel32.dll").NewProc("CreateRemoteThread")
)

// Hat der Prozess schon ein sichtbares Fenster?
func windowOf(pid uint32) bool {
	found := false
	cb := windows.NewCallback(func(h uintptr, _ uintptr) uintptr {
		var p uint32
		pGetWindowThreadPI.Call(h, uintptr(unsafe.Pointer(&p)))
		if p == pid {
			if v, _, _ := pIsVisible.Call(h); v != 0 {
				found = true
				return 0
			}
		}
		return 1
	})
	pEnumWindows.Call(cb, 0)
	return found
}

func moduleLoaded(pid uint32, name string) bool {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPMODULE|windows.TH32CS_SNAPMODULE32, pid)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(snap)
	var m windows.ModuleEntry32
	m.Size = uint32(unsafe.Sizeof(m))
	for err = windows.Module32First(snap, &m); err == nil; err = windows.Module32Next(snap, &m) {
		if strings.EqualFold(windows.UTF16ToString(m.Module[:]), name) {
			return true
		}
	}
	return false
}

// LoadLibraryW im Spielprozess aufrufen (nur ohne Anti-Cheat)
func injectDLL(pid uint32, dll string) error {
	if eacRunning() {
		return errors.New("Rocket League runs with Anti-Cheat.")
	}
	const access = windows.PROCESS_CREATE_THREAD | windows.PROCESS_QUERY_INFORMATION | windows.PROCESS_VM_OPERATION | windows.PROCESS_VM_WRITE | windows.PROCESS_VM_READ
	h, err := windows.OpenProcess(access, false, pid)
	if err != nil {
		return errors.New("Couldn't open Rocket League.")
	}
	defer windows.CloseHandle(h)
	path, _ := windows.UTF16FromString(dll)
	size := uintptr(len(path) * 2)
	mem, _, _ := pVirtualAllocEx.Call(uintptr(h), 0, size, windows.MEM_COMMIT|windows.MEM_RESERVE, windows.PAGE_READWRITE)
	if mem == 0 {
		return errors.New("Couldn't load the item menu.")
	}
	var n uintptr
	if err := windows.WriteProcessMemory(h, mem, (*byte)(unsafe.Pointer(&path[0])), size, &n); err != nil {
		pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
		return errors.New("Couldn't load the item menu.")
	}
	k32, _ := windows.LoadLibrary("kernel32.dll")
	lla, _ := windows.GetProcAddress(k32, "LoadLibraryW")
	th, _, _ := pCreateRemoteThread.Call(uintptr(h), 0, 0, lla, mem, 0, 0)
	if th == 0 {
		pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
		return errors.New("Couldn't load the item menu.")
	}
	if ev, _ := windows.WaitForSingleObject(windows.Handle(th), 120000); ev == windows.WAIT_OBJECT_0 {
		pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
	}
	windows.CloseHandle(windows.Handle(th))
	return nil
}
