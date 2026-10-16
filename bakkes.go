//go:build windows

package main

// Item Swapper ueber BakkesMod - ZLauncher richtet alles selbst ein:
//   - laedt BakkesMod vom offiziellen Update-Server (GitHub-Download) nach %APPDATA%\bakkesmod\bakkesmod
//     und haelt es aktuell (gleiche Logik wie der offizielle Injector, github.com/bakkesmodorg/BakkesModInjectorCpp)
//   - prueft, ob BakkesMod zur installierten Rocket-League-Version passt (Steam-Build-ID)
//   - legt das eigene Plugin (ZLauncherItems.dll, F8-Fenster) dazu und traegt es in plugins.cfg ein
//   - laedt bakkesmod.dll beim Start ohne Anti-Cheat selbst ins Spiel
// Mit Anti-Cheat passiert hier nie etwas. Das BakkesMod-Programm selbst wird nicht gebraucht.

import (
	"archive/zip"
	"bytes"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
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
func bmDLL() string  { return filepath.Join(bmRoot(), "dll", "bakkesmod.dll") }

func bmVersion() int {
	b, err := os.ReadFile(filepath.Join(bmRoot(), "version.txt"))
	if err != nil {
		return 0
	}
	v, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	return v
}

func bmInstalled() bool {
	_, err := os.Stat(bmDLL())
	return err == nil && bmVersion() > 0
}

// ---------- Einrichtung / Updates ----------

type bmInfo struct {
	Trainer   int
	URLs      []string
	BuildIDs  []string
	RLVersion string
}

var bmServers = []string{
	"https://updater.bakkesmod.com/updater/",
	"https://updater2.bakkesmod.com/updater/",
	"http://updater.bakkesmod.com/updater/",
}

func fetchBMInfo(version int) (bmInfo, error) {
	var info bmInfo
	c := &http.Client{Timeout: 15 * time.Second}
	var lastErr error = errors.New("Couldn't reach the BakkesMod update server.")
	for _, base := range bmServers {
		req, _ := http.NewRequest("GET", base+strconv.Itoa(version), nil)
		req.Header.Set("User-Agent", "ZLauncher/"+appVersion)
		resp, err := c.Do(req)
		if err != nil {
			continue
		}
		var raw struct {
			GameInfo struct {
				BuildID  string `json:"buildid"`
				BuildIDs string `json:"buildids"`
			} `json:"gameinfo"`
			Update struct {
				Trainer   json.Number `json:"trainer_version"`
				URL       string      `json:"download_url"`
				GitHubURL string      `json:"github_download_url"`
				RL        string      `json:"rocketleague_version"`
			} `json:"update_info"`
		}
		err = json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&raw)
		resp.Body.Close()
		if err != nil || raw.Update.Trainer == "" {
			lastErr = errors.New("The BakkesMod update server sent something unexpected.")
			continue
		}
		t, _ := raw.Update.Trainer.Int64()
		info.Trainer = int(t)
		info.RLVersion = raw.Update.RL
		for _, u := range []string{raw.Update.GitHubURL, raw.Update.URL} {
			if strings.HasPrefix(u, "http") {
				info.URLs = append(info.URLs, u)
			}
		}
		for _, b := range strings.Split(raw.GameInfo.BuildIDs+","+raw.GameInfo.BuildID, ",") {
			if b = strings.TrimSpace(b); b != "" {
				info.BuildIDs = append(info.BuildIDs, b)
			}
		}
		return info, nil
	}
	return info, lastErr
}

// Steam-Build-ID der installierten Rocket-League-Version
var buildRe = regexp.MustCompile(`"buildid"\s+"(\d+)"`)

func rlBuildID() string {
	for _, lib := range steamLibraries() {
		b, err := os.ReadFile(filepath.Join(lib, "steamapps", "appmanifest_"+appID+".acf"))
		if err != nil {
			continue
		}
		if m := buildRe.FindSubmatch(b); m != nil {
			return string(m[1])
		}
	}
	return ""
}

// Laufzeit von Microsoft, die BakkesMod braucht (Visual C++ 2015-2022, x64)
func vcRedistOK() bool {
	for _, p := range []string{`SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64`, `SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64`} {
		if k, err := registry.OpenKey(registry.LOCAL_MACHINE, p, registry.QUERY_VALUE); err == nil {
			v, _, err := k.GetIntegerValue("Installed")
			k.Close()
			if err == nil && v == 1 {
				return true
			}
		}
	}
	_, err := os.Stat(filepath.Join(os.Getenv("SystemRoot"), "System32", "vcruntime140_1.dll"))
	return err == nil
}

type bmSetupState struct {
	State string  `json:"state"` // idle | checking | downloading | ready | outdated | error
	Pct   float64 `json:"pct"`
	Msg   string  `json:"msg"`
}

var (
	setupMu    sync.Mutex
	setupSt    = bmSetupState{State: "idle"}
	setupBusy  int32
	setupAt    time.Time
	setupBuild []string
)

func setSetup(state, msg string, pct float64) {
	setupMu.Lock()
	setupSt = bmSetupState{State: state, Msg: msg, Pct: pct}
	setupMu.Unlock()
}

func getSetup() bmSetupState {
	setupMu.Lock()
	defer setupMu.Unlock()
	return setupSt
}

// BakkesMod installieren oder aktualisieren. force = auch wenn erst kuerzlich geprueft.
func bmSetup(force bool) {
	if !atomic.CompareAndSwapInt32(&setupBusy, 0, 1) {
		return
	}
	defer atomic.StoreInt32(&setupBusy, 0)
	if !force && time.Since(setupAt) < 20*time.Minute && getSetup().State == "ready" {
		return
	}
	setSetup("checking", "Checking for BakkesMod updates …", 0)
	ver := bmVersion()
	if _, err := os.Stat(bmDLL()); err != nil {
		ver = 0
	}
	info, err := fetchBMInfo(ver)
	if err != nil {
		if bmInstalled() {
			setSetup("ready", "", 100) // offline: vorhandene Version weiter nutzen
		} else {
			setSetup("error", err.Error(), 0)
		}
		return
	}
	setupMu.Lock()
	setupBuild = info.BuildIDs
	setupMu.Unlock()
	if ver < info.Trainer {
		if err := bmDownload(info); err != nil {
			setSetup("error", err.Error(), 0)
			return
		}
	}
	_ = installPlugin()
	setupAt = time.Now()
	if b := rlBuildID(); b != "" && len(info.BuildIDs) > 0 && !contains(info.BuildIDs, b) {
		setSetup("outdated", "BakkesMod isn't ready for this Rocket League update yet. Items come back once BakkesMod updates.", 100)
		return
	}
	setSetup("ready", "", 100)
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

func bmDownload(info bmInfo) error {
	if rlPID() != 0 && moduleLoaded(rlPID(), "bakkesmod.dll") {
		return errors.New("Close Rocket League to update BakkesMod.")
	}
	var data []byte
	var lastErr error = errors.New("Couldn't download BakkesMod.")
	for _, u := range info.URLs {
		setSetup("downloading", "Downloading BakkesMod …", 0)
		b, err := download(u, func(p float64) { setSetup("downloading", "Downloading BakkesMod …", p) })
		if err == nil && len(b) > 1000 && string(b[:2]) == "PK" {
			data = b
			break
		}
		if err != nil {
			lastErr = err
		}
	}
	if data == nil {
		return lastErr
	}
	setSetup("downloading", "Installing BakkesMod …", 100)
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return errors.New("The BakkesMod download is damaged. Try again.")
	}
	root := bmRoot()
	_ = os.MkdirAll(root, 0755)
	for _, f := range zr.File {
		name := filepath.FromSlash(f.Name)
		dst := filepath.Join(root, name)
		if !strings.HasPrefix(filepath.Clean(dst), filepath.Clean(root)+string(os.PathSeparator)) {
			continue // nichts ausserhalb des Ordners schreiben
		}
		if f.FileInfo().IsDir() {
			_ = os.MkdirAll(dst, 0755)
			continue
		}
		// eigene Einstellungen nicht ueberschreiben (wie der offizielle Installer)
		if strings.HasSuffix(strings.ToLower(name), ".cfg") {
			if _, err := os.Stat(dst); err == nil {
				continue
			}
		}
		_ = os.MkdirAll(filepath.Dir(dst), 0755)
		rc, err := f.Open()
		if err != nil {
			return errors.New("Couldn't install BakkesMod.")
		}
		b, err := io.ReadAll(io.LimitReader(rc, 200<<20))
		rc.Close()
		if err != nil {
			return errors.New("Couldn't install BakkesMod.")
		}
		if err := os.WriteFile(dst, b, 0644); err != nil {
			return fmt.Errorf("Couldn't write %s. Close Rocket League and try again.", filepath.Base(dst))
		}
	}
	if bmVersion() < info.Trainer {
		_ = os.WriteFile(filepath.Join(root, "version.txt"), []byte(strconv.Itoa(info.Trainer)), 0644)
	}
	return nil
}

func download(u string, progress func(float64)) ([]byte, error) {
	c := &http.Client{Timeout: 5 * time.Minute}
	req, _ := http.NewRequest("GET", u, nil)
	req.Header.Set("User-Agent", "ZLauncher/"+appVersion)
	resp, err := c.Do(req)
	if err != nil {
		return nil, errors.New("Couldn't download BakkesMod. Check your internet connection.")
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("Couldn't download BakkesMod (%d).", resp.StatusCode)
	}
	var buf bytes.Buffer
	total := resp.ContentLength
	chunk := make([]byte, 64<<10)
	for {
		n, err := resp.Body.Read(chunk)
		buf.Write(chunk[:n])
		if total > 0 {
			progress(float64(buf.Len()) / float64(total) * 100)
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, errors.New("The BakkesMod download was interrupted.")
		}
		if buf.Len() > 300<<20 {
			return nil, errors.New("The BakkesMod download is too big.")
		}
	}
	return buf.Bytes(), nil
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

// Item Swapper ist aktiv, sobald der Items-Tab einmal benutzt wurde (items.json existiert) und er an ist
func itemsEnabled() bool {
	if _, err := os.Stat(itemsFile); err != nil {
		return false
	}
	return readItems().Enabled
}

type ItemsStatus struct {
	BakkesMod bool         `json:"bakkesmod"` // eingerichtet
	Setup     bmSetupState `json:"setup"`
	VCRedist  bool         `json:"vcredist"`
	Loaded    bool         `json:"loaded"` // laeuft gerade im Spiel
	DB        bool         `json:"db"`     // Item-Liste vorhanden
	DBTime    int64        `json:"dbTime"`
	Enabled   bool         `json:"enabled"`
	Error     string       `json:"error"`
}

func itemsStatus() ItemsStatus {
	st := ItemsStatus{BakkesMod: bmInstalled(), Setup: getSetup(), VCRedist: vcRedistOK(), Enabled: itemsEnabled(), Error: lastInjectErr.Load().(string)}
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

// Wird nach dem Start ohne Anti-Cheat aufgerufen (und wenn Items im laufenden Spiel eingeschaltet werden)
func itemsOnLaunch() {
	if !itemsEnabled() {
		return
	}
	if !atomic.CompareAndSwapInt32(&injectBusy, 0, 1) {
		return
	}
	go func() {
		defer atomic.StoreInt32(&injectBusy, 0)
		lastInjectErr.Store("")
		bmSetup(false) // einrichten bzw. aktualisieren, bevor das Spiel BakkesMod braucht
		st := getSetup()
		if st.State != "ready" || !bmInstalled() {
			return
		}
		if !vcRedistOK() {
			lastInjectErr.Store("Missing Microsoft Visual C++ Runtime.")
			return
		}
		// auf das Spiel warten
		start := time.Now()
		var pid uint32
		for pid == 0 {
			if time.Since(start) > 3*time.Minute {
				return
			}
			time.Sleep(500 * time.Millisecond)
			pid = rlPID()
		}
		// wie der offizielle Injector: kurz nach dem Start laden
		time.Sleep(2500 * time.Millisecond)
		if eacRunning() || rlPID() != pid || moduleLoaded(pid, "bakkesmod.dll") {
			return
		}
		if err := injectDLL(pid, bmDLL()); err != nil {
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
	var n uintptr
	if err := windows.WriteProcessMemory(h, mem, (*byte)(unsafe.Pointer(&path[0])), size, &n); err != nil {
		return errors.New("Couldn't load BakkesMod into the game.")
	}
	k32, _ := windows.LoadLibrary("kernel32.dll")
	lla, _ := windows.GetProcAddress(k32, "LoadLibraryW")
	th, _, _ := pCreateRemoteThread.Call(uintptr(h), 0, 0, lla, mem, 0, 0)
	if th == 0 {
		pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
		return errors.New("Couldn't load BakkesMod into the game.")
	}
	// wie der offizielle Injector auf LoadLibrary warten; Speicher nur freigeben, wenn es fertig ist
	if ev, _ := windows.WaitForSingleObject(windows.Handle(th), 120000); ev == windows.WAIT_OBJECT_0 {
		pVirtualFreeEx.Call(uintptr(h), mem, 0, windows.MEM_RELEASE)
	}
	windows.CloseHandle(windows.Handle(th))
	return nil
}
