//go:build windows

package main

// Item Swapper ueber BakkesMod.
//   - ZLauncher legt sein eigenes BakkesMod-Plugin (ZLauncherItems.dll, F8-Fenster) in den Plugin-Ordner
//     und traegt es in plugins.cfg ein.
//   - Beim Start ohne Anti-Cheat startet ZLauncher den BakkesMod-Injector (prueft selbst, ob BakkesMod
//     zur Spielversion passt). Gibt es keinen Injector, laedt ZLauncher bakkesmod.dll selbst ins Spiel.
//   - Das Loadout steht in %APPDATA%\RLMapSwap\items.json; das Plugin liest es live und
//     schreibt die Item-Liste des Spiels nach items_db.json.
// Mit Anti-Cheat passiert hier nie etwas.

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

//go:embed plugin/bin/ZLauncherItems.dll
var pluginDLL []byte

const pluginName = "zlauncheritems"

var (
	itemsFile   = filepath.Join(dataDir, "items.json")
	itemsDBFile = filepath.Join(dataDir, "items_db.json")
	itemsMu     sync.Mutex
)

func bmRoot() string { return filepath.Join(os.Getenv("APPDATA"), "bakkesmod", "bakkesmod") }

func bmInstalled() bool {
	fi, err := os.Stat(bmRoot())
	return err == nil && fi.IsDir()
}

// BakkesMod.exe suchen (Autostart-Eintrag, Deinstallations-Eintrag, uebliche Orte)
func bmExe() string {
	ok := func(p string) bool {
		p = strings.Trim(strings.TrimSpace(p), `"`)
		if fi, err := os.Stat(p); err == nil && !fi.IsDir() && strings.HasSuffix(strings.ToLower(p), ".exe") {
			return true
		}
		return false
	}
	clean := func(v string) string {
		v = strings.TrimSpace(v)
		if strings.HasPrefix(v, `"`) {
			if i := strings.Index(v[1:], `"`); i >= 0 {
				return v[1 : i+1]
			}
		}
		if i := strings.Index(strings.ToLower(v), ".exe"); i >= 0 {
			return v[:i+4]
		}
		return v
	}
	for _, root := range []registry.Key{registry.CURRENT_USER, registry.LOCAL_MACHINE} {
		if k, err := registry.OpenKey(root, `Software\Microsoft\Windows\CurrentVersion\Run`, registry.QUERY_VALUE); err == nil {
			v, _, err := k.GetStringValue("BakkesMod")
			k.Close()
			if err == nil && ok(clean(v)) {
				return clean(v)
			}
		}
	}
	for _, root := range []registry.Key{registry.CURRENT_USER, registry.LOCAL_MACHINE} {
		for _, base := range []string{`Software\Microsoft\Windows\CurrentVersion\Uninstall`, `Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall`} {
			k, err := registry.OpenKey(root, base, registry.ENUMERATE_SUB_KEYS)
			if err != nil {
				continue
			}
			names, _ := k.ReadSubKeyNames(-1)
			k.Close()
			for _, n := range names {
				sk, err := registry.OpenKey(root, base+`\`+n, registry.QUERY_VALUE)
				if err != nil {
					continue
				}
				dn, _, _ := sk.GetStringValue("DisplayName")
				loc, _, _ := sk.GetStringValue("InstallLocation")
				icon, _, _ := sk.GetStringValue("DisplayIcon")
				sk.Close()
				if !strings.Contains(strings.ToLower(dn), "bakkesmod") {
					continue
				}
				if ok(clean(icon)) {
					return clean(icon)
				}
				if p := filepath.Join(strings.Trim(loc, `"`), "BakkesMod.exe"); loc != "" && ok(p) {
					return p
				}
			}
		}
	}
	for _, p := range []string{
		filepath.Join(os.Getenv("ProgramFiles"), "BakkesMod", "BakkesMod.exe"),
		filepath.Join(os.Getenv("ProgramFiles(x86)"), "BakkesMod", "BakkesMod.exe"),
		filepath.Join(os.Getenv("LOCALAPPDATA"), "Programs", "BakkesMod", "BakkesMod.exe"),
		filepath.Join(os.Getenv("APPDATA"), "bakkesmod", "BakkesMod.exe"),
	} {
		if ok(p) {
			return p
		}
	}
	return ""
}

// bakkesmod.dll fuer den eigenen Injector
func bmDLL() string {
	for _, p := range []string{
		filepath.Join(bmRoot(), "dll", "bakkesmod.dll"),
		filepath.Join(bmRoot(), "bakkesmod.dll"),
	} {
		if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
			return p
		}
	}
	return ""
}

// Plugin installieren bzw. aktualisieren und in plugins.cfg eintragen
func installPlugin() error {
	if !bmInstalled() {
		return errors.New("BakkesMod isn't installed.")
	}
	pdir := filepath.Join(bmRoot(), "plugins")
	_ = os.MkdirAll(pdir, 0755)
	dst := filepath.Join(pdir, pluginName+".dll")
	if cur, err := os.ReadFile(dst); err != nil || !bytes.Equal(cur, pluginDLL) {
		tmp := dst + ".new"
		if err := os.WriteFile(tmp, pluginDLL, 0644); err != nil {
			return err
		}
		if err := os.Rename(tmp, dst); err != nil {
			os.Remove(tmp) // Plugin ist gerade im Spiel geladen; dann beim naechsten Mal
		}
	}
	cdir := filepath.Join(bmRoot(), "cfg")
	_ = os.MkdirAll(cdir, 0755)
	cfg := filepath.Join(cdir, "plugins.cfg")
	b, _ := os.ReadFile(cfg)
	line := "plugin load " + pluginName
	for _, l := range strings.Split(string(b), "\n") {
		if strings.EqualFold(strings.TrimSpace(l), line) {
			return nil
		}
	}
	s := string(b)
	if s != "" && !strings.HasSuffix(s, "\n") {
		s += "\n"
	}
	return os.WriteFile(cfg, []byte(s+line+"\n"), 0644)
}

// ---------- Loadout ----------

type itemsState struct {
	Enabled bool                      `json:"enabled"`
	Items   map[string]map[string]int `json:"items"`
}

func readItems() itemsState {
	st := itemsState{Enabled: true, Items: map[string]map[string]int{}}
	if b, err := os.ReadFile(itemsFile); err == nil {
		_ = json.Unmarshal(b, &st)
		if st.Items == nil {
			st.Items = map[string]map[string]int{}
		}
	}
	return st
}

func writeItems(st itemsState) error {
	itemsMu.Lock()
	defer itemsMu.Unlock()
	b, _ := json.MarshalIndent(st, "", "  ")
	tmp := itemsFile + ".tmp"
	if err := os.WriteFile(tmp, b, 0644); err != nil {
		return err
	}
	return replaceFile(tmp, itemsFile)
}

func itemsEnabled() bool { return readItems().Enabled }

type ItemsStatus struct {
	BakkesMod bool   `json:"bakkesmod"` // installiert
	Injector  bool   `json:"injector"`  // BakkesMod.exe gefunden
	Loaded    bool   `json:"loaded"`    // laeuft gerade im Spiel
	DB        bool   `json:"db"`        // Item-Liste vorhanden
	DBTime    int64  `json:"dbTime"`
	Enabled   bool   `json:"enabled"`
	Error     string `json:"error"`
}

func itemsStatus() ItemsStatus {
	st := ItemsStatus{BakkesMod: bmInstalled(), Injector: bmExe() != "", Enabled: itemsEnabled(), Error: lastInjectErr.Load().(string)}
	if fi, err := os.Stat(itemsDBFile); err == nil && fi.Size() > 100 {
		st.DB, st.DBTime = true, fi.ModTime().Unix()
	}
	if pid := rlPID(); pid != 0 && !eacRunning() {
		st.Loaded = moduleLoaded(pid, "bakkesmod.dll")
	}
	return st
}

// ---------- Ins Spiel bringen ----------

var (
	injectBusy    int32
	lastInjectErr atomic.Value
)

func init() { lastInjectErr.Store("") }

// Wird nach dem Start ohne Anti-Cheat aufgerufen
func itemsOnLaunch() {
	if !bmInstalled() || !itemsEnabled() {
		return
	}
	_ = installPlugin()
	if !atomic.CompareAndSwapInt32(&injectBusy, 0, 1) {
		return
	}
	go func() {
		defer atomic.StoreInt32(&injectBusy, 0)
		lastInjectErr.Store("")
		if exe := bmExe(); exe != "" {
			// offizieller Injector: prueft Version, aktualisiert sich selbst und injiziert automatisch
			if !processRunning(filepath.Base(exe)) {
				cmd := exec.Command(exe)
				cmd.Dir = filepath.Dir(exe)
				if err := cmd.Start(); err != nil {
					lastInjectErr.Store("Couldn't start BakkesMod.")
				} else {
					go cmd.Wait()
				}
			}
			return
		}
		dll := bmDLL()
		if dll == "" {
			lastInjectErr.Store("BakkesMod files not found. Reinstall BakkesMod.")
			return
		}
		// eigener Injector: warten, bis das Spiel laeuft und fertig geladen ist
		start := time.Now()
		var pid uint32
		for pid == 0 {
			if time.Since(start) > 3*time.Minute {
				return
			}
			time.Sleep(time.Second)
			pid = rlPID()
		}
		for i := 0; i < 90 && !windowOf(pid); i++ {
			time.Sleep(time.Second)
		}
		time.Sleep(8 * time.Second)
		if eacRunning() || rlPID() != pid {
			return
		}
		if moduleLoaded(pid, "bakkesmod.dll") {
			return
		}
		if err := injectDLL(pid, dll); err != nil {
			lastInjectErr.Store(err.Error())
		}
	}()
}

var (
	pEnumWindows       = user32.NewProc("EnumWindows")
	pGetWindowThreadPI = user32.NewProc("GetWindowThreadProcessId")
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

var (
	pVirtualAllocEx     = windows.NewLazySystemDLL("kernel32.dll").NewProc("VirtualAllocEx")
	pVirtualFreeEx      = windows.NewLazySystemDLL("kernel32.dll").NewProc("VirtualFreeEx")
	pCreateRemoteThread = windows.NewLazySystemDLL("kernel32.dll").NewProc("CreateRemoteThread")
)

// LoadLibraryW im Spielprozess aufrufen (klassisches DLL-Laden, nur ohne Anti-Cheat)
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
		return errors.New("Couldn't load BakkesMod into the game.")
	}
	defer pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
	var n uintptr
	if err := windows.WriteProcessMemory(h, mem, (*byte)(unsafe.Pointer(&path[0])), size, &n); err != nil {
		return errors.New("Couldn't load BakkesMod into the game.")
	}
	k32, _ := windows.LoadLibrary("kernel32.dll")
	lla, _ := windows.GetProcAddress(k32, "LoadLibraryW")
	th, _, _ := pCreateRemoteThread.Call(uintptr(h), 0, 0, lla, mem, 0, 0)
	if th == 0 {
		return errors.New("Couldn't load BakkesMod into the game.")
	}
	windows.WaitForSingleObject(windows.Handle(th), 15000)
	windows.CloseHandle(windows.Handle(th))
	return nil
}
