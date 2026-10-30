//go:build windows

package main

import (
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"

	webview2 "github.com/jchv/go-webview2"
	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

//go:embed ui.html
var uiHTML string

const appID = "252950"

var (
	dataDir   = filepath.Join(os.Getenv("APPDATA"), "RLMapSwap")
	backupDir = filepath.Join(dataDir, "backup")
	ownDir    = filepath.Join(dataDir, "My Maps")
	stateFile = filepath.Join(dataDir, "state.json")
	cfgFile   = filepath.Join(dataDir, "config.json")
)

var friendly = map[string]string{
	"park_p":              "Beckwith Park",
	"stadium_p":           "DFH Stadium",
	"eurostadium_p":       "Mannfield",
	"trainstation_p":      "Urban Central",
	"utopiastadium_p":     "Utopia Coliseum",
	"beach_p":             "Salty Shores",
	"chinatown_p":         "Forbidden Temple",
	"cs_p":                "Champions Field",
	"farm_p":              "Farmstead",
	"neotokyo_standard_p": "Neo Tokyo",
	"outlaw_p":            "Deadeye Canyon",
	"wasteland_s_p":       "Wasteland",
	"labs_underpass_p":    "Underpass",
}

// ---------- Daten ----------

type MapInfo struct {
	Name  string  `json:"name"`
	Title  string  `json:"title"`
	Author string  `json:"author"`
	ID    string  `json:"id"`
	Path  string  `json:"path"`
	MB    float64 `json:"mb"`
}
type Arena struct {
	File  string `json:"file"`
	Name  string `json:"name"`
	Known bool   `json:"known"`
}
type State struct {
	Found     bool              `json:"found"`
	Cooked    string            `json:"cooked"`
	Maps      []MapInfo         `json:"maps"`
	Arenas    []Arena           `json:"arenas"`
	Swaps     map[string]string `json:"swaps"`
	Favs      map[string]bool   `json:"favs"`
	Locked    map[string]bool   `json:"locked"`
	SwapNames map[string]string `json:"swapNames"`
}

func loadJSON(path string, v interface{}) {
	b, err := os.ReadFile(path)
	if err == nil {
		_ = json.Unmarshal(b, v)
	}
}
func saveJSON(path string, v interface{}) error {
	b, _ := json.MarshalIndent(v, "", "  ")
	return os.WriteFile(path, b, 0644)
}

func loadSwaps() map[string]string {
	m := map[string]string{}
	loadJSON(stateFile, &m)
	return m
}

// ---------- Steam finden ----------

func steamPath() string {
	keys := []struct {
		root registry.Key
		path string
		val  string
	}{
		{registry.CURRENT_USER, `Software\Valve\Steam`, "SteamPath"},
		{registry.LOCAL_MACHINE, `SOFTWARE\WOW6432Node\Valve\Steam`, "InstallPath"},
		{registry.LOCAL_MACHINE, `SOFTWARE\Valve\Steam`, "InstallPath"},
	}
	for _, k := range keys {
		key, err := registry.OpenKey(k.root, k.path, registry.QUERY_VALUE)
		if err != nil {
			continue
		}
		v, _, err := key.GetStringValue(k.val)
		key.Close()
		if err == nil && v != "" {
			return filepath.Clean(strings.ReplaceAll(v, "/", `\`))
		}
	}
	return ""
}

var pathRe = regexp.MustCompile(`"path"\s+"([^"]+)"`)

func steamLibraries() []string {
	seen := map[string]bool{}
	var libs []string
	add := func(p string) {
		p = filepath.Clean(p)
		if !seen[strings.ToLower(p)] {
			seen[strings.ToLower(p)] = true
			libs = append(libs, p)
		}
	}
	sp := steamPath()
	if sp == "" {
		return libs
	}
	add(sp)
	b, err := os.ReadFile(filepath.Join(sp, "steamapps", "libraryfolders.vdf"))
	if err == nil {
		for _, m := range pathRe.FindAllStringSubmatch(string(b), -1) {
			add(strings.ReplaceAll(m[1], `\\`, `\`))
		}
	}
	return libs
}

func isCooked(p string) bool {
	matches, _ := filepath.Glob(filepath.Join(p, "*_P.upk"))
	return len(matches) > 0
}

func findCooked() string {
	var cfg struct{ Cooked string }
	loadJSON(cfgFile, &cfg)
	if cfg.Cooked != "" && isCooked(cfg.Cooked) {
		return cfg.Cooked
	}
	for _, lib := range steamLibraries() {
		c := filepath.Join(lib, "steamapps", "common", "rocketleague", "TAGame", "CookedPCConsole")
		if isCooked(c) {
			return c
		}
	}
	return ""
}

func workshopMaps() []MapInfo {
	roots := []string{ownDir}
	for _, lib := range steamLibraries() {
		roots = append(roots, filepath.Join(lib, "steamapps", "workshop", "content", appID))
	}
	var maps []MapInfo
	seen := map[string]bool{}
	for _, r := range roots {
		_ = filepath.WalkDir(r, func(p string, d os.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			ext := strings.ToLower(filepath.Ext(p))
			if ext != ".udk" && ext != ".upk" {
				return nil
			}
			if seen[strings.ToLower(p)] {
				return nil
			}
			seen[strings.ToLower(p)] = true
			info, _ := d.Info()
			size := 0.0
			if info != nil {
				size = float64(info.Size()) / 1048576
			}
			name := strings.TrimSuffix(filepath.Base(p), filepath.Ext(p))
			id := workshopID(r, p)
			title, author := name, ""
			if m, ok := getMeta(id); ok {
				if m.Title != "" {
					title = m.Title
				}
				author = m.Author
			}
			maps = append(maps, MapInfo{
				Name:  name,
				Title:  title,
				Author: author,
				ID:    id,
				Path:  p,
				MB:    float64(int(size*10)) / 10,
			})
			return nil
		})
	}
	sort.Slice(maps, func(i, j int) bool { return strings.ToLower(maps[i].Title) < strings.ToLower(maps[j].Title) })
	return maps
}

func arenaName(file string) (string, bool) {
	base := strings.ToLower(strings.TrimSuffix(file, filepath.Ext(file)))
	if n, ok := friendly[base]; ok {
		return n, true
	}
	return strings.TrimSuffix(file, filepath.Ext(file)), false
}

// Nur diese Arenen haelt Rocket League nicht dauerhaft offen - nur die werden angeboten.
var swappable = map[string]bool{
	"park_p":           true, // Beckwith Park
	"farm_p":           true, // Farmstead
	"beach_p":          true, // Salty Shores
	"labs_underpass_p": true, // Underpass
	"utopiastadium_p":  true, // Utopia Coliseum
	"wasteland_s_p":    true, // Wasteland
}

func isSwappable(file string) bool {
	return swappable[strings.ToLower(strings.TrimSuffix(file, filepath.Ext(file)))]
}

func arenas(cooked string) []Arena {
	files, _ := filepath.Glob(filepath.Join(cooked, "*_P.upk"))
	var out []Arena
	for _, f := range files {
		name := filepath.Base(f)
		if !isSwappable(name) {
			continue
		}
		n, known := arenaName(name)
		out = append(out, Arena{File: name, Name: n, Known: known})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Known != out[j].Known {
			return out[i].Known
		}
		return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name)
	})
	return out
}

// ---------- Rocket League laeuft? ----------

func processRunning(names ...string) bool {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(snap)
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		exe := windows.UTF16ToString(e.ExeFile[:])
		for _, n := range names {
			if strings.EqualFold(exe, n) {
				return true
			}
		}
	}
	return false
}

func rlRunning() bool { return processRunning("RocketLeague.exe") }

// Laeuft Rocket League mit Easy Anti-Cheat? Dann Spieldateien auf keinen Fall anfassen.
func eacRunning() bool {
	return processRunning("EasyAntiCheat_EOS.exe", "EasyAntiCheat.exe", "start_protected_game.exe")
}

// Tauschen ist erlaubt, wenn RL zu ist oder ohne Anti-Cheat laeuft.
func canTouchFiles() error {
	if rlRunning() && eacRunning() {
		return errors.New("Rocket League is running with Easy Anti-Cheat. Restart it without Anti-Cheat to swap maps.")
	}
	return nil
}

// ---------- Aktionen ----------

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	tmp := dst + ".rlswap-tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err = io.Copy(out, in); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err = out.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err = replaceFile(tmp, dst); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func overwriteInPlace(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_TRUNC, 0)
	if err != nil {
		return err
	}
	if _, err = io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// Arena-Datei ist so belegt, dass sie sich gar nicht ersetzen laesst.
func fileLocked(path string) bool { return !replaceable(path) }

func lockedArenas(cooked string, list []Arena) map[string]bool {
	out := map[string]bool{}
	if !rlRunning() {
		return out
	}
	for _, a := range list {
		if fileLocked(filepath.Join(cooked, a.File)) {
			out[a.File] = true
		}
	}
	return out
}

func friendlyErr(err error) error {
	inUse := errors.Is(err, windows.ERROR_SHARING_VIOLATION) || errors.Is(err, windows.ERROR_LOCK_VIOLATION) ||
		errors.Is(err, windows.ERROR_USER_MAPPED_FILE)
	if inUse || (errors.Is(err, os.ErrPermission) && rlRunning()) {
		return errors.New("Rocket League has this arena loaded right now (often it's your main-menu background). Pick another arena, or close Rocket League to swap this one.")
	}
	if errors.Is(err, os.ErrPermission) {
		return fmt.Errorf("No write access to the Rocket League folder. Right-click the app and choose \"Run as administrator\".")
	}
	return err
}

type App struct{ cooked string }

func (a *App) GetState() State {
	s := State{Swaps: loadSwaps(), SwapNames: map[string]string{}, Favs: loadFavs()}
	for f := range s.Swaps {
		n, _ := arenaName(f)
		s.SwapNames[f] = n
	}
	s.Maps = workshopMaps()
	if a.cooked == "" {
		a.cooked = findCooked()
	}
	if a.cooked != "" {
		s.Found = true
		s.Cooked = a.cooked
		s.Arenas = arenas(a.cooked)
		s.Locked = lockedArenas(a.cooked, s.Arenas)
		go cleanupAside(a.cooked)
	}
	return s
}

func (a *App) Swap(arenaFile, mapPath string) error {
	if a.cooked == "" {
		return errors.New("Rocket League folder not found.")
	}
	if err := canTouchFiles(); err != nil {
		return err
	}
	if strings.ContainsAny(arenaFile, `\/`) {
		return errors.New("Invalid arena.")
	}
	if !isSwappable(arenaFile) {
		n, _ := arenaName(arenaFile)
		return fmt.Errorf("%s can't be swapped. Pick Beckwith Park, Farmstead, Salty Shores, Underpass, Utopia Coliseum or Wasteland.", n)
	}
	target := filepath.Join(a.cooked, arenaFile)
	if _, err := os.Stat(target); err != nil {
		return errors.New("Arena file not found.")
	}
	if rlRunning() && fileLocked(target) {
		return errors.New("Rocket League has this arena loaded right now (often it's your main-menu background). Pick another arena, or close Rocket League to swap this one.")
	}
	if fileExists(filepath.Join(mergeBackupDir, arenaFile)) {
		return errors.New("This arena has a merged backdrop right now. Restore it in the Merge tab first.")
	}
	swaps := loadSwaps()
	if _, already := swaps[arenaFile]; !already {
		if err := copyFile(target, filepath.Join(backupDir, arenaFile)); err != nil {
			return fmt.Errorf("Backup failed: %v", friendlyErr(err))
		}
	}
	if err := copyFile(mapPath, target); err != nil {
		return friendlyErr(err)
	}
	mapName := strings.TrimSuffix(filepath.Base(mapPath), filepath.Ext(mapPath))
	swaps[arenaFile] = mapName
	paths := map[string]string{}
	loadJSON(swapPathsFile, &paths)
	paths[arenaFile] = mapPath
	_ = saveJSON(swapPathsFile, paths)
	// Eine Workshop-Map darf nur in einer Arena stecken: alte Arena zuruecksetzen
	var moveErr error
	for f, m := range swaps {
		if f != arenaFile && strings.EqualFold(m, mapName) {
			if _, err := a.restoreOne(swaps, f); err != nil {
				n, _ := arenaName(f)
				moveErr = fmt.Errorf("Swapped, but %s couldn't be restored: %v", n, err)
			}
		}
	}
	if err := saveJSON(stateFile, swaps); err != nil {
		return err
	}
	return moveErr
}

// Getauschte Maps gibt es nur ohne Anti-Cheat: vor einem Start mit Anti-Cheat und beim Beenden von Prism
// werden sie zurueckgesetzt, Prism merkt sich aber, welche es waren, und tauscht sie beim naechsten Start
// ohne Anti-Cheat wieder ein.
var (
	swapPathsFile  = filepath.Join(dataDir, "swap_paths.json")  // Arena -> Map-Datei (beim Tauschen)
	swapParkedFile = filepath.Join(dataDir, "swap_parked.json") // gemerkte Swaps, solange zurueckgesetzt
)

func (a *App) parkSwaps() error {
	swaps := loadSwaps()
	if len(swaps) == 0 {
		return nil
	}
	paths := map[string]string{}
	loadJSON(swapPathsFile, &paths)
	parked := map[string]string{}
	loadJSON(swapParkedFile, &parked)
	var wm []MapInfo // erst bei Bedarf suchen (Swaps aus dem F8-Menue stehen nicht in swap_paths)
	for f, name := range swaps {
		p := paths[f]
		if p == "" || !fileExists(p) || !strings.EqualFold(strings.TrimSuffix(filepath.Base(p), filepath.Ext(p)), name) {
			p = ""
			if wm == nil {
				wm = workshopMaps()
			}
			for _, m := range wm {
				if strings.EqualFold(m.Name, name) {
					p = m.Path
					break
				}
			}
		}
		if p != "" {
			parked[f] = p
		}
	}
	_ = saveJSON(swapParkedFile, parked)
	logf("swaps: parked %d", len(parked))
	return a.restoreSwaps()
}

func (a *App) unparkSwaps() {
	parked := map[string]string{}
	loadJSON(swapParkedFile, &parked)
	if len(parked) == 0 {
		return
	}
	swaps := loadSwaps()
	for f, p := range parked {
		if _, already := swaps[f]; already || !fileExists(p) {
			continue
		}
		if err := a.Swap(f, p); err != nil {
			logf("swaps: couldn't swap %s back in: %v", f, err)
		}
	}
	os.Remove(swapParkedFile)
	logf("swaps: swapped %d back in", len(parked))
}

func (a *App) restoreOne(swaps map[string]string, f string) (missing bool, err error) {
	bk := filepath.Join(backupDir, f)
	if _, e := os.Stat(bk); e != nil {
		delete(swaps, f)
		return true, nil
	}
	if e := copyFile(bk, filepath.Join(a.cooked, f)); e != nil {
		return false, friendlyErr(e)
	}
	os.Remove(bk)
	delete(swaps, f)
	return false, nil
}

// Restore: "Restore all" im Launcher - auch die gemerkten Swaps vergessen (kommen nicht wieder)
func (a *App) Restore() error {
	os.Remove(swapParkedFile)
	return a.restoreSwaps()
}

func (a *App) restoreSwaps() error {
	if err := canTouchFiles(); err != nil {
		return err
	}
	swaps := loadSwaps()
	var missing []string
	for f := range swaps {
		miss, err := a.restoreOne(swaps, f)
		if err != nil {
			_ = saveJSON(stateFile, swaps)
			return err
		}
		if miss {
			missing = append(missing, f)
		}
	}
	if err := saveJSON(stateFile, swaps); err != nil {
		return err
	}
	if len(missing) > 0 {
		return fmt.Errorf("Backup missing for %s. Use \"Verify integrity of game files\" in Steam.", strings.Join(missing, ", "))
	}
	return nil
}

func (a *App) RestoreArena(f string) error {
	if err := canTouchFiles(); err != nil {
		return err
	}
	swaps := loadSwaps()
	if _, ok := swaps[f]; !ok {
		return nil
	}
	miss, err := a.restoreOne(swaps, f)
	if err != nil {
		return err
	}
	if err := saveJSON(stateFile, swaps); err != nil {
		return err
	}
	if miss {
		return fmt.Errorf("Backup missing for %s. Use \"Verify integrity of game files\" in Steam.", f)
	}
	return nil
}

func (a *App) SetFolder(p string) error {
	p = strings.Trim(strings.TrimSpace(p), `"`)
	if !isCooked(p) {
		if c := filepath.Join(p, "TAGame", "CookedPCConsole"); isCooked(c) {
			p = c
		} else {
			return errors.New("No Rocket League arenas found there. Choose the \"rocketleague\" folder.")
		}
	}
	a.cooked = p
	return saveJSON(cfgFile, map[string]string{"Cooked": p})
}

// Ordner, Links und steam://-Adressen mit dem Standardprogramm oeffnen.
// (Frueher explorer.exe mit verstecktem Fenster - dadurch blieben Ordnerfenster unsichtbar.)
func open(target string) {
	verb, _ := windows.UTF16PtrFromString("open")
	file, err := windows.UTF16PtrFromString(target)
	if err != nil {
		return
	}
	if windows.ShellExecute(0, verb, file, nil, nil, windows.SW_SHOWNORMAL) != nil {
		_ = exec.Command("explorer.exe", target).Start()
	}
}

// ---------- Start ----------

func msgBox(text string) {
	user32 := windows.NewLazySystemDLL("user32.dll")
	t, _ := windows.UTF16PtrFromString(text)
	c, _ := windows.UTF16PtrFromString("Prism")
	user32.NewProc("MessageBoxW").Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), 0x40)
}


func main() {
	if len(os.Args) > 1 && os.Args[1] == "--steam-helper" {
		runSteamHelper()
		return
	}
	afterUpdate(os.Args)
	// nur eine Instanz: nach einem Update kurz warten, bis die alte Version zu ist
	wait := time.Duration(0)
	if len(os.Args) > 1 && os.Args[1] == "--after-update" {
		wait = 20 * time.Second
	}
	startTray = hasArg("--tray")
	go autostartRefresh()
	if !singleInstance(wait) {
		wakeOtherInstance()
		// ist unsere exe neuer, macht die alte Platz: kurz warten und dann selbst starten
		if !singleInstance(15 * time.Second) {
			return
		}
	}
	_ = os.MkdirAll(backupDir, 0755)
	_ = os.MkdirAll(ownDir, 0755)
	loadMeta()
	_ = os.Setenv("WEBVIEW2_DEFAULT_BACKGROUND_COLOR", "FF09090B")
	_ = os.Setenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "--autoplay-policy=no-user-gesture-required")

	w := webview2.NewWithOptions(webview2.WebViewOptions{
		Debug:     false,
		AutoFocus: true,
		DataPath:  filepath.Join(dataDir, "webview"),
		WindowOptions: webview2.WindowOptions{
			Title:  "Prism",
			Width:  winW,
			Height: winH,
			IconId: 1,
			Center: true,
		},
	})
	if w == nil {
		msgBox("Prism needs the Microsoft Edge WebView2 Runtime.\n\nDownload it for free from Microsoft (search: \"WebView2 Runtime\") and start the app again.")
		return
	}
	defer w.Destroy()
	hwnd := uintptr(w.Window())
	setupWindow(hwnd)
	bindWindow(w, hwnd)

	app := &App{}
	var restarting sync.Mutex
	mergeRestart = func() {
		if !restarting.TryLock() {
			return
		}
		defer restarting.Unlock()
		if err := closeRL(); err != nil {
			return
		}
		for i := 0; i < 100 && rlRunning(); i++ { // bis zu 20 s
			time.Sleep(200 * time.Millisecond)
		}
		if rlRunning() {
			logf("merge restart: Rocket League didn't close")
			return
		}
		time.Sleep(3 * time.Second) // Originale zurueck (mergeLoop), Dateien frei
		if err := app.LaunchRL(false); err != nil {
			logf("merge restart: %v", err)
		}
	}
	go func() {
		var cooked string
		var at time.Time
		mergeLoop(func() string {
			if cooked == "" || time.Since(at) > 30*time.Second {
				cooked, at = findCooked(), time.Now()
			}
			return cooked
		})
	}()
	w.Bind("getState", func() State { return app.GetState() })
	w.Bind("getPreview", func(id, mapPath string) string { return previewFor(id, mapPath) })

	// Steam-Titel und Vorschaubilder im Hintergrund nachladen
	w.Bind("loadMeta", func() {
		go func() {
			var ids []string
			for _, m := range workshopMaps() {
				ids = append(ids, m.ID)
			}
			if fetchMeta(ids) {
				w.Dispatch(func() { w.Eval("window.__metaReady && window.__metaReady()") })
			}
			// Vorschaubilder im Hintergrund hochskalieren
			if upscaleAll(ids) {
				w.Dispatch(func() { w.Eval("window.__metaReady && window.__metaReady(true)") })
			}
		}()
	})

	// Datei-Operationen laufen im Hintergrund, damit die Oberflaeche fluessig bleibt.
	finish := func(id int, err error) {
		msg := ""
		if err != nil {
			msg = err.Error()
		}
		b, _ := json.Marshal(msg)
		w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__done(%d,%s)", id, b)) })
	}
	w.Bind("swapAsync", func(id int, arena, mapPath string) {
		go func() { finish(id, app.Swap(arena, mapPath)) }()
	})
	w.Bind("restoreArenaAsync", func(id int, arena string) {
		go func() { finish(id, app.RestoreArena(arena)) }()
	})
	w.Bind("restoreAsync", func(id int) {
		go func() { finish(id, app.Restore()) }()
	})
	w.Bind("setFolder", func(p string) error { return app.SetFolder(p) })
	w.Bind("isRLRunning", func() bool { return rlRunning() })
	w.Bind("getCamera", func() CameraState { return GetCamera() })
	w.Bind("setCamera", func(v map[string]float64) error { return SetCamera(v) })
	w.Bind("setCameraOn", func(on bool) error { return SetCameraOn(on) })

	// ---- Browse Maps / Downloads / Favoriten ----
	w.Bind("recommendedAsync", func(id int) {
		go func() {
			res, err := recommendedMaps()
			payload, msg := "null", ""
			if err != nil {
				msg = err.Error()
			} else {
				b, _ := json.Marshal(res)
				payload = string(b)
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,%s,%s)", id, payload, m)) })
		}()
	})
	w.Bind("browseAsync", func(id int, sort, period, query string, page int) {
		go func() {
			res, err := browseWorkshop(sort, period, query, page)
			payload := "null"
			msg := ""
			if err != nil {
				msg = err.Error()
			} else {
				b, _ := json.Marshal(res)
				payload = string(b)
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,%s,%s)", id, payload, m)) })
		}()
	})
	dlNotify = func(e DlEvent) {
		b, _ := json.Marshal(e)
		w.Dispatch(func() { w.Eval("window.__dl && window.__dl(" + string(b) + ")") })
	}
	w.Bind("downloadMap", func(id string) error { return queueDownload(id) })
	w.Bind("detailsAsync", func(cb int, id string) {
		go func() {
			res, err := mapDetails(id)
			payload, msg := "null", ""
			if err != nil {
				msg = err.Error()
			} else {
				b, _ := json.Marshal(res)
				payload = string(b)
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,%s,%s)", cb, payload, m)) })
		}()
	})
	// Links (Steam-Seite, Ersteller) im Standard-Browser oeffnen - nur http(s)
	w.Bind("openURL", func(u string) {
		if strings.HasPrefix(u, "https://") || strings.HasPrefix(u, "http://") {
			open(u)
		}
	})
	// ---- Steam / Updates ----
	w.Bind("steamRunning", func() bool { return steamRunning() })
	w.Bind("openSteam", func() { open("steam://open/main") })
	w.Bind("getVersion", func() string { return appVersion })
	w.Bind("checkUpdateAsync", func(cb int) {
		go func() {
			res, err := checkUpdate()
			payload, msg := "null", ""
			if err != nil {
				msg = err.Error()
			} else {
				b, _ := json.Marshal(res)
				payload = string(b)
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,%s,%s)", cb, payload, m)) })
		}()
	})
	w.Bind("installUpdateAsync", func(cb int) {
		go func() {
			err := installUpdate(func(pct float64) {
				w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__upd && window.__upd(%.1f)", pct)) })
			})
			msg := ""
			if err != nil {
				msg = err.Error()
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,true,%s)", cb, m)) })
		}()
	})
	w.Bind("toggleFav", func(key string) bool { return toggleFav(key) })
	// ---- Home: Rocket League starten, Steam-Freunde ----
	w.Bind("getMediaBase", func() string { return mediaBase() })
	w.Bind("getRLLogo", func() string { return rlLogoURL() })
	w.Bind("rlStatus", func() RLStatus { return app.RLStatus() })
	// ---- Game-Boost (standardmaessig an) ----
	boostEnabled := true
	{
		st := map[string]interface{}{}
		loadJSON(filepath.Join(dataDir, "settings.json"), &st)
		if v, ok := st["boost"].(bool); ok {
			boostEnabled = v
		}
	}
	boostInit(findCooked, boostEnabled)
	w.Bind("getBoost", func() BoostStatus { return getBoost() })
	// ---- Discord Rich Presence ----
	presenceInit()
	w.Bind("getPresence", func() PresenceStatus { return getPresence() })
	w.Bind("setPresenceCfg", func(m map[string]bool) { setPresenceCfg(m) })
	w.Bind("setPresenceTab", func(t string) { setPresenceTab(t) })
	w.Bind("setPresenceCtx", func(starting bool, maps int) { setPresenceCtx(starting, maps) })
	w.Bind("setBoost", func(on bool) BoostStatus { return setBoost(on) })
	w.Bind("launchRL", func(eac bool) error { return app.LaunchRL(eac) })
	w.Bind("closeRL", func() error { return closeRL() })
	w.Bind("friendsAsync", func(cb int, key string) {
		go func() {
			res, err := steamFriends(key)
			payload, msg := "null", ""
			if err != nil {
				msg = err.Error()
			} else {
				b, _ := json.Marshal(res)
				payload = string(b)
			}
			m, _ := json.Marshal(msg)
			w.Dispatch(func() { w.Eval(fmt.Sprintf("window.__browsed(%d,%s,%s)", cb, payload, m)) })
		}()
	})
	w.Bind("openFriendChat", func(id string) {
		if _, err := strconv.ParseUint(id, 10, 64); err == nil {
			open("steam://friends/message/" + id)
		}
	})
	w.Bind("openSteamProfile", func(id string) {
		if _, err := strconv.ParseUint(id, 10, 64); err == nil {
			open("https://steamcommunity.com/profiles/" + id)
		}
	})
	w.Bind("openOwn", func() { open(ownDir) })
	w.Bind("openWorkshopDir", func() error {
		// Workshop-Ordner von Rocket League in der ersten Steam-Bibliothek, die einen hat
		var fallback string
		for _, lib := range steamLibraries() {
			d := filepath.Join(lib, "steamapps", "workshop", "content", appID)
			if fi, err := os.Stat(d); err == nil && fi.IsDir() {
				open(d)
				return nil
			}
			if w := filepath.Join(lib, "steamapps", "workshop"); fallback == "" {
				if fi, err := os.Stat(w); err == nil && fi.IsDir() {
					fallback = w
				}
			}
		}
		if fallback != "" {
			open(fallback)
			return nil
		}
		return errors.New("Couldn't find the Steam workshop folder. Download a map first.")
	})
	w.Bind("openWorkshop", func() { open("steam://url/SteamWorkshopPage/" + appID) })

	// Oberflaeche vom lokalen Server laden (gleicher Ursprung wie die Videos); sonst wie bisher direkt
	if base := localServer(); base != "" {
		w.Navigate(base)
	} else {
		w.SetHtml(uiHTML)
	}
	w.Run()
	// Prism endet: gemergte Arenen zuruecksetzen (beim naechsten Start ohne Anti-Cheat wieder da)
	mergeMu.Lock()
	mergeHold = true
	mergeMu.Unlock()
	_ = mergeRestoreAll(app.cooked)
	_ = app.parkSwaps()
	trayRemove()
	boostShutdown()
	presenceShutdown()
	stopHelper()
}
