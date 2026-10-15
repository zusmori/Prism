//go:build windows

package main

// Download von Workshop-Maps direkt ueber den laufenden Steam-Client (Steamworks).
// ZMS meldet sich dafuer kurz als Rocket League (App 252950) bei Steam an und
// trennt sich wieder, sobald alle Downloads fertig sind.

import (
	"bufio"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

//go:embed steam_api64.dll
var steamDLL []byte

const (
	stSubscribed  = 1
	stInstalled   = 4
	stNeedsUpdate = 8
	stDownloading = 16
	stPending     = 32
)

type DlEvent struct {
	ID    string  `json:"id"`
	State string  `json:"state"` // queued | downloading | done | error
	Pct   float64 `json:"pct"`
	Msg   string  `json:"msg,omitempty"`
}


// ---------- Hauptprogramm: steuert einen kurzlebigen Hilfsprozess ----------
//
// Wichtig: Steam haelt Rocket League fuer "laufend", solange der Prozess lebt,
// der sich als App 252950 angemeldet hat - auch nach SteamAPI_Shutdown. Darum
// laeuft der Steam-Teil in einem eigenen Prozess ("ZMS.exe --steam-helper"),
// der sich beendet, sobald alle Downloads fertig sind. Dann startet Rocket
// League wieder ohne Wartezeit.

var (
	helperMu  sync.Mutex
	helperIn  *bufio.Writer
	helperCmd *exec.Cmd
	pending   = map[string]bool{}
	dlNotify  func(DlEvent)
)

func emitUI(e DlEvent) {
	if dlNotify != nil {
		dlNotify(e)
	}
}

func queueDownload(idStr string) error {
	if _, err := strconv.ParseUint(idStr, 10, 64); err != nil {
		return errors.New("Invalid map id.")
	}
	helperMu.Lock()
	defer helperMu.Unlock()
	if helperCmd == nil {
		if err := startHelper(); err != nil {
			return err
		}
	}
	pending[idStr] = true
	if _, err := helperIn.WriteString(idStr + "\n"); err == nil {
		err = helperIn.Flush()
		if err == nil {
			return nil
		}
	}
	delete(pending, idStr)
	return errors.New("Couldn't start the download.")
}

func startHelper() error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	cmd := exec.Command(exe, "--steam-helper")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	cmd.Dir = filepath.Join(dataDir, "steam")
	_ = os.MkdirAll(cmd.Dir, 0755)
	cmd.Env = append(os.Environ(), "SteamAppId="+appID, "SteamGameId="+appID)
	in, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return errors.New("Couldn't start the Steam helper.")
	}
	helperCmd = cmd
	helperIn = bufio.NewWriter(in)

	go func() {
		sc := bufio.NewScanner(out)
		for sc.Scan() {
			var e DlEvent
			if json.Unmarshal(sc.Bytes(), &e) != nil {
				continue
			}
			helperMu.Lock()
			if e.State == "done" || e.State == "error" {
				delete(pending, e.ID)
			}
			helperMu.Unlock()
			emitUI(e)
		}
		_ = cmd.Wait()
		helperMu.Lock()
		if helperCmd == cmd {
			helperCmd, helperIn = nil, nil
		}
		left := pending
		pending = map[string]bool{}
		helperMu.Unlock()
		for id := range left {
			emitUI(DlEvent{ID: id, State: "error", Msg: "The download was interrupted. Try again."})
		}
	}()
	return nil
}

// Beim Beenden von ZMS den Hilfsprozess nicht verwaist zuruecklassen
func stopHelper() {
	helperMu.Lock()
	defer helperMu.Unlock()
	if helperCmd != nil && helperCmd.Process != nil {
		_ = helperCmd.Process.Kill()
	}
}

// ---------- Hilfsprozess ----------

func runSteamHelper() {
	enc := json.NewEncoder(os.Stdout)
	var encMu sync.Mutex
	emit := func(e DlEvent) { encMu.Lock(); _ = enc.Encode(e); encMu.Unlock() }
	queue := make(chan uint64, 64)
	go func() {
		sc := bufio.NewScanner(os.Stdin)
		for sc.Scan() {
			if id, err := strconv.ParseUint(strings.TrimSpace(sc.Text()), 10, 64); err == nil {
				queue <- id
			}
		}
		close(queue) // Hauptprogramm wurde geschlossen
	}()
	steamWorker(queue, emit)
}

type steamAPI struct {
	dll      *windows.DLL
	ugc      uintptr
	subscribe, download, state, dlInfo *windows.Proc
	runCB, shutdown                    *windows.Proc
}

func loadSteam() (*steamAPI, error) {
	dir := filepath.Join(dataDir, "steam")
	_ = os.MkdirAll(dir, 0755)
	dllPath := filepath.Join(dir, "steam_api64.dll")
	if fi, err := os.Stat(dllPath); err != nil || fi.Size() != int64(len(steamDLL)) {
		if err := os.WriteFile(dllPath, steamDLL, 0644); err != nil {
			return nil, fmt.Errorf("Could not prepare Steam: %v", err)
		}
	}
	_ = os.WriteFile(filepath.Join(dir, "steam_appid.txt"), []byte(appID), 0644)
	_ = os.Chdir(dir)
	_ = os.Setenv("SteamAppId", appID)
	_ = os.Setenv("SteamGameId", appID)

	dll, err := windows.LoadDLL(dllPath)
	if err != nil {
		return nil, fmt.Errorf("Could not load Steam: %v", err)
	}
	s := &steamAPI{dll: dll}
	find := func(n string) *windows.Proc {
		p, e := dll.FindProc(n)
		if e != nil && err == nil {
			err = e
		}
		return p
	}
	isRunning := find("SteamAPI_IsSteamRunning")
	initFlat := find("SteamAPI_InitFlat")
	getUGC := find("SteamAPI_SteamUGC_v021")
	s.subscribe = find("SteamAPI_ISteamUGC_SubscribeItem")
	s.download = find("SteamAPI_ISteamUGC_DownloadItem")
	s.state = find("SteamAPI_ISteamUGC_GetItemState")
	s.dlInfo = find("SteamAPI_ISteamUGC_GetItemDownloadInfo")
	s.runCB = find("SteamAPI_RunCallbacks")
	s.shutdown = find("SteamAPI_Shutdown")
	if err != nil {
		return nil, fmt.Errorf("Steam component incompatible: %v", err)
	}
	if r, _, _ := isRunning.Call(); r&0xff == 0 {
		return nil, errors.New("Steam isn't running. Start Steam and try again.")
	}
	var msg [1024]byte
	if r, _, _ := initFlat.Call(uintptr(unsafe.Pointer(&msg[0]))); r != 0 {
		m := windows.BytePtrToString(&msg[0])
		if m == "" {
			m = "make sure you're logged in and own Rocket League on Steam"
		}
		return nil, errors.New("Couldn't connect to Steam: " + m)
	}
	s.ugc, _, _ = getUGC.Call()
	if s.ugc == 0 {
		s.shutdown.Call()
		return nil, errors.New("Couldn't access the Steam Workshop.")
	}
	return s, nil
}

func (s *steamAPI) itemState(id uint64) uint32 {
	r, _, _ := s.state.Call(s.ugc, uintptr(id))
	return uint32(r)
}

func steamWorker(queue chan uint64, emit func(DlEvent)) {
	runtime.LockOSThread() // Steam-Aufrufe immer vom selben Thread
	var api *steamAPI
	active := map[uint64]time.Time{}
	idleSince := time.Now()
	stdinOpen := true

	tick := time.NewTicker(150 * time.Millisecond)
	defer tick.Stop()
	finish := func() {
		if api != nil {
			api.shutdown.Call()
		}
		os.Exit(0) // Prozess endet -> Steam sieht Rocket League sofort als beendet
	}

	for {
		select {
		case id, ok := <-queue:
			if !ok {
				stdinOpen = false
				queue = nil
				if len(active) == 0 {
					finish()
				}
				continue
			}
			idleSince = time.Now()
			req := struct{ id uint64 }{id}
			idStr := strconv.FormatUint(req.id, 10)
			if _, busy := active[req.id]; busy {
				continue
			}
			if api == nil {
				a, err := loadSteam()
				if err != nil {
					emit(DlEvent{ID: idStr, State: "error", Msg: err.Error()})
					continue
				}
				api = a
			}
			emit(DlEvent{ID: idStr, State: "queued"})
			api.subscribe.Call(api.ugc, uintptr(req.id))
			api.download.Call(api.ugc, uintptr(req.id), 1)
			active[req.id] = time.Now()

		case <-tick.C:
			if api == nil {
				if time.Since(idleSince) > 3*time.Second {
					finish()
				}
				continue
			}
			api.runCB.Call()
			for id, started := range active {
				idStr := strconv.FormatUint(id, 10)
				st := api.itemState(id)
				var done, total uint64
				api.dlInfo.Call(api.ugc, uintptr(id), uintptr(unsafe.Pointer(&done)), uintptr(unsafe.Pointer(&total)))
				switch {
				case st&stInstalled != 0 && st&(stDownloading|stPending|stNeedsUpdate) == 0:
					emit(DlEvent{ID: idStr, State: "done", Pct: 100})
					delete(active, id)
				case time.Since(started) > 15*time.Minute:
					emit(DlEvent{ID: idStr, State: "error", Msg: "Download timed out."})
					delete(active, id)
				default:
					pct := 0.0
					if total > 0 {
						pct = float64(done) / float64(total) * 100
					}
					emit(DlEvent{ID: idStr, State: "downloading", Pct: pct})
				}
			}
			if len(active) > 0 {
				idleSince = time.Now()
			} else if !stdinOpen || time.Since(idleSince) > 3*time.Second {
				// nichts mehr zu tun -> Prozess beenden, damit Rocket League normal startet
				finish()
			}
		}
	}
}
