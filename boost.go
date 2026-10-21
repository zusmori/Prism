//go:build windows

package main

// Game-Boost: solange Rocket League laeuft
//   - hohe CPU-Prioritaet fuer RocketLeague.exe (nur ohne Anti-Cheat, ZLauncher greift nie in den EAC-Prozess ein)
//   - Windows-Energiemodus "Hoechstleistung" bzw. "Beste Leistung"
//   - keine Energiespar-Drosselung (EcoQoS) fuer Rocket League, damit es auf den schnellen Kernen bleibt (nur ohne Anti-Cheat)
//   - ZLauncher selbst tritt in den Hintergrund (niedrige Prioritaet, Energiesparmodus)
// dauerhaft (solange Boost an ist):
//   - Windows-Grafikeinstellung "Hohe Leistung" fuer Rocket League (Laptops mit 2 Grafikchips)
//   - Vollbildoptimierungen fuer RocketLeague.exe aus (echtes Vollbild, weniger Eingabeverzoegerung)
//   - Intro-Videos beim Start ueberspringen (-nomovie)
// Alles wird automatisch zurueckgesetzt, wenn das Spiel endet, Boost ausgeschaltet wird oder ZLauncher beendet wird.
// Grafik-Einstellungen von Rocket League werden nicht veraendert.

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

var (
	powrprof         = windows.NewLazySystemDLL("powrprof.dll")
	pGetActiveScheme = powrprof.NewProc("PowerGetActiveScheme")
	pSetActiveScheme = powrprof.NewProc("PowerSetActiveScheme")
	pGetOverlay      = powrprof.NewProc("PowerGetActualOverlayScheme")
	pSetOverlay      = powrprof.NewProc("PowerSetActiveOverlayScheme")

	guidHighPerf     = windows.GUID{Data1: 0x8c5e7fda, Data2: 0xe8bf, Data3: 0x4a96, Data4: [8]byte{0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}}
	guidUltimate     = windows.GUID{Data1: 0xe9a42b02, Data2: 0xd5df, Data3: 0x448d, Data4: [8]byte{0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}}
	guidOverlayBest  = windows.GUID{Data1: 0xded574b5, Data2: 0x45a0, Data3: 0x4f42, Data4: [8]byte{0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}}
)

const gpuPrefKey = `Software\Microsoft\DirectX\UserGpuPreferences`

// gespeicherter Zustand (ueberlebt Abstuerze, damit nichts haengen bleibt)
type boostSaved struct {
	PowerSet   bool              `json:"powerSet"`
	Scheme     string            `json:"scheme"`
	OverlaySet bool              `json:"overlaySet"`
	Overlay    string            `json:"overlay"`
	GpuPrev    map[string]string `json:"gpuPrev"` // exe-Pfad -> vorheriger Wert ("" = gab es nicht)
	FsoPrev    map[string]string `json:"fsoPrev"` // exe-Pfad -> vorheriger AppCompat-Wert
}

type BoostStatus struct {
	Enabled  bool   `json:"enabled"`
	Active   bool   `json:"active"`
	Priority string `json:"priority"` // on | waiting | eac | failed
	Power    string `json:"power"`    // on | waiting | unavailable
	Gpu      string `json:"gpu"`      // set | off | nopath
	Fso      string `json:"fso"`      // set | off | nopath
	Quiet    bool   `json:"quiet"`    // ZLauncher laeuft gerade gedrosselt im Hintergrund
}

var (
	boostMu      sync.Mutex
	boostOn      = true
	boostSt      = BoostStatus{Enabled: true, Priority: "waiting", Power: "waiting", Gpu: "off"}
	boostState   boostSaved
	boostFile    = filepath.Join(dataDir, "boost_state.json")
	boostPID     uint32
	boostCooked  func() string
)

func saveBoostState() { _ = saveJSON(boostFile, boostState) }

func guidStr(g windows.GUID) string { return g.String() }

func parseGUID(s string) (windows.GUID, bool) {
	g, err := windows.GUIDFromString(s)
	return g, err == nil
}

// ---------- Energiemodus ----------

func activeScheme() (windows.GUID, bool) {
	if pGetActiveScheme.Find() != nil {
		return windows.GUID{}, false
	}
	var p *windows.GUID
	if r, _, _ := pGetActiveScheme.Call(0, uintptr(unsafe.Pointer(&p))); r != 0 || p == nil {
		return windows.GUID{}, false
	}
	g := *p
	windows.LocalFree(windows.Handle(unsafe.Pointer(p)))
	return g, true
}

func setScheme(g windows.GUID) bool {
	if pSetActiveScheme.Find() != nil {
		return false
	}
	r, _, _ := pSetActiveScheme.Call(0, uintptr(unsafe.Pointer(&g)))
	return r == 0
}

func activeOverlay() (windows.GUID, bool) {
	if pGetOverlay.Find() != nil {
		return windows.GUID{}, false
	}
	var g windows.GUID
	r, _, _ := pGetOverlay.Call(uintptr(unsafe.Pointer(&g)))
	return g, r == 0
}

func setOverlay(g windows.GUID) bool {
	if pSetOverlay.Find() != nil {
		return false
	}
	r, _, _ := pSetOverlay.Call(uintptr(unsafe.Pointer(&g))) // 16-Byte-Struktur -> per Zeiger (x64)
	return r == 0
}

// Hoechstleistung einschalten; vorherigen Zustand merken
func powerBoostOn() bool {
	if boostState.PowerSet || boostState.OverlaySet {
		return true
	}
	cur, ok := activeScheme()
	if ok && (cur == guidHighPerf || cur == guidUltimate) {
		return true // laeuft schon auf Hoechstleistung
	}
	if ok {
		for _, g := range []windows.GUID{guidUltimate, guidHighPerf} {
			if setScheme(g) {
				boostState.PowerSet, boostState.Scheme = true, guidStr(cur)
				saveBoostState()
				return true
			}
		}
	}
	// Viele neuere PCs haben nur "Ausbalanciert" -> Energiemodus "Beste Leistung" (Windows 10/11)
	if ov, ok := activeOverlay(); ok {
		if ov == guidOverlayBest {
			return true
		}
		if setOverlay(guidOverlayBest) {
			boostState.OverlaySet, boostState.Overlay = true, guidStr(ov)
			saveBoostState()
			return true
		}
	}
	return false
}

func powerBoostOff() {
	if boostState.PowerSet {
		if g, ok := parseGUID(boostState.Scheme); ok {
			setScheme(g)
		}
		boostState.PowerSet, boostState.Scheme = false, ""
	}
	if boostState.OverlaySet {
		if g, ok := parseGUID(boostState.Overlay); ok {
			setOverlay(g)
		}
		boostState.OverlaySet, boostState.Overlay = false, ""
	}
	saveBoostState()
}

// ---------- CPU-Prioritaet ----------

func rlPID() uint32 {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return 0
	}
	defer windows.CloseHandle(snap)
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		if strings.EqualFold(windows.UTF16ToString(e.ExeFile[:]), "RocketLeague.exe") {
			return e.ProcessID
		}
	}
	return 0
}

func raisePriority(pid uint32) bool {
	h, err := windows.OpenProcess(windows.PROCESS_SET_INFORMATION|windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(h)
	return windows.SetPriorityClass(h, windows.HIGH_PRIORITY_CLASS) == nil
}

// ---------- Grafikkarte (Windows-Grafikeinstellung pro Programm) ----------

func rlExePaths() []string {
	if boostCooked == nil {
		return nil
	}
	bin := rlBinDir(boostCooked())
	if bin == "" {
		return nil
	}
	var out []string
	for _, n := range []string{"RocketLeague.exe", "RocketLeague_EAC.exe"} {
		p := filepath.Join(bin, n)
		if _, err := os.Stat(p); err == nil {
			out = append(out, p)
		}
	}
	return out
}

func gpuPrefOn() string {
	paths := rlExePaths()
	if len(paths) == 0 {
		return "nopath"
	}
	k, _, err := registry.CreateKey(registry.CURRENT_USER, gpuPrefKey, registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return "off"
	}
	defer k.Close()
	if boostState.GpuPrev == nil {
		boostState.GpuPrev = map[string]string{}
	}
	for _, p := range paths {
		cur, _, err := k.GetStringValue(p)
		if cur == "GpuPreference=2;" {
			continue
		}
		if _, saved := boostState.GpuPrev[p]; !saved {
			if err != nil {
				cur = ""
			}
			boostState.GpuPrev[p] = cur
		}
		_ = k.SetStringValue(p, "GpuPreference=2;")
	}
	saveBoostState()
	return "set"
}

func gpuPrefOff() {
	if len(boostState.GpuPrev) == 0 {
		return
	}
	k, err := registry.OpenKey(registry.CURRENT_USER, gpuPrefKey, registry.SET_VALUE)
	if err == nil {
		for p, prev := range boostState.GpuPrev {
			if prev == "" {
				_ = k.DeleteValue(p)
			} else {
				_ = k.SetStringValue(p, prev)
			}
		}
		k.Close()
	}
	boostState.GpuPrev = nil
	saveBoostState()
}

// ---------- Vollbildoptimierungen (AppCompat-Flag, nur fuer RocketLeague.exe) ----------

const fsoKey = `Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers`
const fsoFlag = "DISABLEDXMAXIMIZEDWINDOWEDMODE"

func rlGameExe() string {
	for _, p := range rlExePaths() {
		if strings.EqualFold(filepath.Base(p), "RocketLeague.exe") {
			return p
		}
	}
	return ""
}

func fsoOn() string {
	p := rlGameExe()
	if p == "" {
		return "nopath"
	}
	k, _, err := registry.CreateKey(registry.CURRENT_USER, fsoKey, registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return "off"
	}
	defer k.Close()
	cur, _, err := k.GetStringValue(p)
	if err != nil {
		cur = ""
	}
	if strings.Contains(strings.ToUpper(cur), fsoFlag) {
		return "set"
	}
	if boostState.FsoPrev == nil {
		boostState.FsoPrev = map[string]string{}
	}
	if _, saved := boostState.FsoPrev[p]; !saved {
		boostState.FsoPrev[p] = cur
	}
	val := "~ " + fsoFlag
	if t := strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(cur), "~")); t != "" {
		val = "~ " + t + " " + fsoFlag // vorhandene Kompatibilitaets-Einstellungen behalten
	}
	if k.SetStringValue(p, val) != nil {
		return "off"
	}
	saveBoostState()
	return "set"
}

func fsoOff() {
	if len(boostState.FsoPrev) == 0 {
		return
	}
	if k, err := registry.OpenKey(registry.CURRENT_USER, fsoKey, registry.SET_VALUE); err == nil {
		for p, prev := range boostState.FsoPrev {
			if prev == "" {
				_ = k.DeleteValue(p)
			} else {
				_ = k.SetStringValue(p, prev)
			}
		}
		k.Close()
	}
	boostState.FsoPrev = nil
	saveBoostState()
}

// ---------- Energiespar-Drosselung (EcoQoS) und Prioritaet ----------

var pSetProcInfo = windows.NewLazySystemDLL("kernel32.dll").NewProc("SetProcessInformation")

// mode: 0 = Windows entscheidet, 1 = nie drosseln, 2 = immer sparsam (EcoQoS)
func setThrottle(h windows.Handle, mode int) bool {
	if pSetProcInfo.Find() != nil {
		return false
	}
	st := struct{ Version, ControlMask, StateMask uint32 }{Version: 1}
	switch mode {
	case 1:
		st.ControlMask = 1 // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
	case 2:
		st.ControlMask, st.StateMask = 1, 1
	}
	r, _, _ := pSetProcInfo.Call(uintptr(h), 4 /* ProcessPowerThrottling */, uintptr(unsafe.Pointer(&st)), unsafe.Sizeof(st))
	return r != 0
}

func noThrottle(pid uint32) {
	h, err := windows.OpenProcess(windows.PROCESS_SET_INFORMATION, false, pid)
	if err != nil {
		return
	}
	setThrottle(h, 1)
	windows.CloseHandle(h)
}

// ZLauncher und seine WebView2-Prozesse
func ownProcs() []uint32 {
	me := windows.GetCurrentProcessId()
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return []uint32{me}
	}
	defer windows.CloseHandle(snap)
	parent := map[uint32]uint32{}
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		parent[e.ProcessID] = e.ParentProcessID
	}
	out := []uint32{me}
	for pid := range parent {
		for p, n := parent[pid], 0; p != 0 && n < 6; p, n = parent[p], n+1 {
			if p == me {
				out = append(out, pid)
				break
			}
		}
	}
	return out
}

var quietNow bool

// ZLauncher waehrend des Spiels in den Hintergrund schicken bzw. zurueckholen
func setQuiet(on bool) {
	if on == quietNow {
		return
	}
	quietNow = on
	prio, mode := uint32(windows.NORMAL_PRIORITY_CLASS), 0
	if on {
		prio, mode = windows.BELOW_NORMAL_PRIORITY_CLASS, 2
	}
	for _, pid := range ownProcs() {
		h, err := windows.OpenProcess(windows.PROCESS_SET_INFORMATION, false, pid)
		if err != nil {
			continue
		}
		_ = windows.SetPriorityClass(h, prio)
		setThrottle(h, mode)
		windows.CloseHandle(h)
	}
}

// Startoptionen, solange Boost an ist (veraendern keine Spieleinstellungen)
func boostLaunchArgs() []string {
	boostMu.Lock()
	defer boostMu.Unlock()
	if !boostOn {
		return nil
	}
	return []string{"-nomovie"}
}

// ---------- Steuerung ----------

func boostInit(cooked func() string, enabled bool) {
	boostMu.Lock()
	defer boostMu.Unlock()
	boostCooked = cooked
	loadJSON(boostFile, &boostState)
	// Reste von einem Absturz aufraeumen, wenn das Spiel nicht laeuft
	if rlPID() == 0 {
		powerBoostOff()
	}
	boostOn = enabled
	boostSt.Enabled = enabled
	if enabled {
		boostSt.Gpu = gpuPrefOn()
		boostSt.Fso = fsoOn()
	} else {
		gpuPrefOff()
		fsoOff()
	}
	go boostLoop()
}

func boostLoop() {
	for {
		boostTick()
		time.Sleep(2 * time.Second)
	}
}

func boostTick() {
	boostMu.Lock()
	defer boostMu.Unlock()
	pid := rlPID()
	if !boostOn || pid == 0 {
		if boostSt.Active || boostState.PowerSet || boostState.OverlaySet {
			powerBoostOff()
		}
		boostPID = 0
		boostSt.Active = false
		boostSt.Priority, boostSt.Power = "waiting", "waiting"
		setQuiet(false)
		boostSt.Quiet = false
		return
	}
	boostSt.Active = true
	setQuiet(true)
	boostSt.Quiet = true
	if pid != boostPID {
		boostPID = pid
		switch {
		case eacRunning():
			boostSt.Priority = "eac" // ZLauncher greift nicht in den Anti-Cheat-Prozess ein
		case raisePriority(pid):
			boostSt.Priority = "on"
			noThrottle(pid)
		default:
			boostSt.Priority = "failed"
		}
	}
	if powerBoostOn() {
		boostSt.Power = "on"
	} else {
		boostSt.Power = "unavailable"
	}
}

func getBoost() BoostStatus {
	boostMu.Lock()
	defer boostMu.Unlock()
	return boostSt
}

func setBoost(on bool) BoostStatus {
	boostMu.Lock()
	boostOn = on
	boostSt.Enabled = on
	if on {
		boostSt.Gpu = gpuPrefOn()
		boostSt.Fso = fsoOn()
		boostPID = 0 // Prioritaet beim naechsten Tick neu setzen
	} else {
		powerBoostOff()
		gpuPrefOff()
		fsoOff()
		setQuiet(false)
		boostSt.Quiet = false
		boostSt.Gpu, boostSt.Fso = "off", "off"
		boostSt.Active = false
		boostSt.Priority, boostSt.Power = "waiting", "waiting"
	}
	boostMu.Unlock()
	if on {
		boostTick()
	}
	return getBoost()
}

// beim Beenden von ZLauncher: Energiemodus zuruecksetzen (Grafikeinstellung bleibt, solange Boost an ist)
func boostShutdown() {
	boostMu.Lock()
	defer boostMu.Unlock()
	powerBoostOff()
	setQuiet(false)
}
