//go:build windows

package main

// Kulissen-Merge: die Kulisse (Gelaende, Berge, Himmel ...) einer Map in eine andere Arena einbauen.
// Rocket League laedt zu jeder Arena "Unter-Level" (z. B. Farm_OOBTerrain = die Berge von Farmstead).
// Prism traegt in der Arena-Datei zusaetzliche Unter-Level der anderen Map ein - Spielfeld, Tore und
// Boost-Pads bleiben unveraendert. Original wird gesichert (backup_merge), Restore setzt es zurueck.
//
// Gesteuert wird ueber Textdateien in %APPDATA%\RLMapSwap (das F8-Menue schreibt/liest sie auch):
//   merge_catalog.txt   Datei \t Name \t Kulisse(0/1) \t Arena(0/1)          (von Prism)
//   merge_requests.txt  Datei \t Kulissen-Datei \t ersetzen(0/1)            (gewuenscht; F8-Menue)
//   merge_status.txt    Datei \t Kulissen-Datei \t ersetzen \t Stand \t Text (von Prism)
// Gemergte Dateien gibt es nur ohne Anti-Cheat: vor einem Start mit Anti-Cheat und beim Beenden von
// Prism wird alles zurueckgesetzt (die Wuensche bleiben und werden danach wieder angewendet).

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/zusmori/zusmoris-map-swapper/upk"
)

var (
	mergeBackupDir = filepath.Join(dataDir, "backup_merge")
	mergeReqFile   = filepath.Join(dataDir, "merge_requests.txt")
	mergeStatFile  = filepath.Join(dataDir, "merge_status.txt")
	mergeCatFile   = filepath.Join(dataDir, "merge_catalog.txt")
	mergeCacheFile = filepath.Join(dataDir, "merge_cache3.json")
)

type mergeWant struct {
	Backdrop string
	Replace  bool
}

type mergeStat struct {
	Backdrop string
	Replace  bool
	State    string // applied | pending | error
	Msg      string
}

var mergeMu sync.Mutex
var mergeHold bool           // Prism endet: nichts mehr anwenden
var mergeHoldUntil time.Time // kurz nach einem Start mit Anti-Cheat nichts anwenden

func readLines(p string) [][]string {
	f, err := os.Open(p)
	if err != nil {
		return nil
	}
	defer f.Close()
	var out [][]string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		t := strings.TrimRight(sc.Text(), "\r")
		if strings.TrimSpace(t) == "" {
			continue
		}
		out = append(out, strings.Split(t, "\t"))
	}
	return out
}

func writeLines(p string, lines []string) {
	tmp := p + ".tmp"
	if os.WriteFile(tmp, []byte(strings.Join(lines, "\n")+"\n"), 0644) == nil {
		_ = replaceFile(tmp, p)
	}
}

func loadMergeWants() map[string]mergeWant {
	out := map[string]mergeWant{}
	for _, l := range readLines(mergeReqFile) {
		if len(l) >= 2 && l[0] != "" && l[1] != "" {
			out[l[0]] = mergeWant{l[1], len(l) > 2 && l[2] == "1"}
		}
	}
	return out
}

func loadMergeStats() map[string]mergeStat {
	out := map[string]mergeStat{}
	for _, l := range readLines(mergeStatFile) {
		if len(l) >= 4 {
			s := mergeStat{Backdrop: l[1], Replace: l[2] == "1", State: l[3]}
			if len(l) > 4 {
				s.Msg = l[4]
			}
			out[l[0]] = s
		}
	}
	return out
}

func saveMergeStats(m map[string]mergeStat) {
	var keys []string
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var lines []string
	for _, k := range keys {
		s := m[k]
		r := "0"
		if s.Replace {
			r = "1"
		}
		lines = append(lines, strings.Join([]string{k, s.Backdrop, r, s.State, strings.ReplaceAll(s.Msg, "\t", " ")}, "\t"))
	}
	writeLines(mergeStatFile, lines)
}

// ---------- Katalog: welche Maps haben eine eigene Kulisse? ----------

type mergeCacheEntry struct {
	Size   int64    `json:"size"`
	Mod    int64    `json:"mod"`
	Levels []string `json:"levels"` // immer geladene Unter-Level (in der Liste)
	Base   bool     `json:"base"`
	Embed  int      `json:"embed"` // Kulissen-Teile direkt in der Map (z. B. Beckwith Park)
	Err    string   `json:"err,omitempty"`
}

// Arenen aus dem normalen Fussball-Modus (Soccar) mit ihren Varianten. Andere Modi (Hoops, Dropshot,
// Snow Day, Knockout, Rocket Labs) und Menue-Hintergruende werden nicht angeboten.
var soccarArenas = map[string]string{
	"stadium_p": "DFH Stadium", "stadium_day_p": "DFH Stadium (Day)", "stadium_foggy_p": "DFH Stadium (Stormy)",
	"stadium_winter_p": "DFH Stadium (Snowy)", "stadium_10a_p": "DFH Stadium (10th Anniversary)", "stadium_race_day_p": "DFH Stadium (Circuit)",
	"eurostadium_p": "Mannfield", "eurostadium_night_p": "Mannfield (Night)", "eurostadium_rainy_p": "Mannfield (Stormy)",
	"eurostadium_snownight_p": "Mannfield (Snowy)", "eurostadium_dusk_p": "Mannfield (Dusk)",
	"park_p": "Beckwith Park", "park_night_p": "Beckwith Park (Midnight)", "park_rainy_p": "Beckwith Park (Stormy)",
	"park_snowy_p": "Beckwith Park (Snowy)", "park_bman_p": "Beckwith Park (Gotham Night)",
	"trainstation_p": "Urban Central", "trainstation_night_p": "Urban Central (Night)", "trainstation_dawn_p": "Urban Central (Dawn)",
	"haunted_trainstation_p": "Urban Central (Haunted)",
	"utopiastadium_p":        "Utopia Coliseum", "utopiastadium_dusk_p": "Utopia Coliseum (Dusk)", "utopiastadium_snow_p": "Utopia Coliseum (Snowy)",
	"utopiastadium_lux_p": "Utopia Coliseum (Gilded)",
	"beach_p":             "Salty Shores", "beach_night_p": "Salty Shores (Night)", "beach_night_grs_p": "Salty Shores (Salty Fest)",
	"chn_stadium_p": "Forbidden Temple", "chn_stadium_day_p": "Forbidden Temple (Day)",
	"cs_p": "Champions Field", "cs_day_p": "Champions Field (Day)", "cs_hw_p": "Rivals Arena",
	"farm_p": "Farmstead", "farm_night_p": "Farmstead (Night)", "farm_hw_p": "Farmstead (Spooky)", "farm_grs_p": "Farmstead (GRS)",
	"neotokyo_standard_p": "Neo Tokyo", "neotokyo_arcade_p": "Neo Tokyo (Arcade)", "neotokyo_hax_p": "Neo Tokyo (Hacked)",
	"neotokyo_toon_p": "Neo Tokyo (Comic)",
	"outlaw_p":        "Deadeye Canyon", "outlaw_oasis_p": "Deadeye Canyon (Oasis)",
	"wasteland_s_p": "Wasteland (Standard)", "wasteland_grs_p": "Wasteland (Pitched)", "wasteland_night_s_p": "Wasteland (Night)",
	"underwater_p": "AquaDome", "underwater_grs_p": "AquaDome (Salty Shallows)",
	"music_p": "Neon Fields", "street_p": "Sovereign Heights", "ff_dusk_p": "Estadio Vida (Dusk)",
	"woods_p": "Drift Woods", "woods_night_p": "Drift Woods (Night)", "arc_standard_p": "Starbase ARC",
	"throwbackstadium_p": "Throwback Stadium", "mall_day_p": "Boostfield Mall", "uf_day_p": "Futura Garden",
	"uf_night_p": "Futura Garden (Night)", "paname_dusk_p": "Paname (Dusk)", "fni_stadium_p": "FNI Stadium",
	"swoosh_p": "Champions Field (Nike FC)", "bb_p": "BB Arena",
}

var reVariant = regexp.MustCompile(`(?i)_(p)$`)

func prettyArena(file string) string {
	if n, ok := arenaName(file); ok {
		return n
	}
	b := reVariant.ReplaceAllString(strings.TrimSuffix(file, filepath.Ext(file)), "")
	// bekannte Grund-Arena + Variante: "Farm_Night" -> "Farmstead (Night)"
	parts := strings.Split(b, "_")
	for i := len(parts) - 1; i >= 1; i-- {
		if n, ok := friendly[strings.ToLower(strings.Join(parts[:i], "_"))+"_p"]; ok {
			return n + " (" + strings.Join(parts[i:], " ") + ")"
		}
	}
	return strings.ReplaceAll(b, "_", " ")
}

// Namen, die das Spiel selbst anzeigt (schreibt der Mod nach map_names.txt): Datei-Stamm (klein) -> Name
func gameMapNames() map[string]string {
	out := map[string]string{}
	for _, l := range readLines(filepath.Join(dataDir, "map_names.txt")) {
		if len(l) < 2 {
			continue
		}
		props := map[string]string{}
		keys := []string{strings.ToLower(l[0])}
		for _, kv := range l[1:] {
			k, v, ok := strings.Cut(kv, "=")
			if !ok {
				continue
			}
			props[strings.ToLower(k)] = strings.TrimSpace(v)
			keys = append(keys, strings.ToLower(strings.TrimSpace(v)))
		}
		name := ""
		for _, k := range []string{"displayname", "localizedname", "label", "title", "name"} {
			if v := props[k]; v != "" && !strings.Contains(v, "_") {
				name = v
				break
			}
		}
		if name == "" {
			for k, v := range props {
				if strings.Contains(k, "displayname") && v != "" {
					name = v
				}
			}
		}
		if name == "" {
			continue
		}
		for k, v := range props {
			if strings.Contains(k, "variant") && v != "" && !strings.Contains(v, "_") && !strings.Contains(strings.ToLower(name), strings.ToLower(v)) {
				name += " (" + v + ")"
				break
			}
		}
		for _, k := range keys {
			k = strings.TrimSuffix(k, "_p")
			if k != "" {
				out[k+"_p"] = name
			}
		}
	}
	return out
}

func buildMergeCatalog(cooked string) {
	if cooked == "" {
		return
	}
	gameNames := gameMapNames()
	cache := map[string]mergeCacheEntry{}
	loadJSON(mergeCacheFile, &cache)
	files, _ := filepath.Glob(filepath.Join(cooked, "*_P.upk"))
	type row struct{ file, name, bd, base, own string }
	var rows []row
	changed := false
	for _, f := range files {
		name := filepath.Base(f)
		lo := strings.ToLower(strings.TrimSuffix(name, filepath.Ext(name)))
		nice, ok := soccarArenas[lo]
		if !ok {
			continue // nur normale Fussball-Arenen
		}
		if n := gameNames[lo]; n != "" {
			nice = n // so heisst sie im Spiel
		}
		// gemergte/getauschte Arena: das Original aus der Sicherung lesen
		src := f
		if _, err := os.Stat(filepath.Join(mergeBackupDir, name)); err == nil {
			src = filepath.Join(mergeBackupDir, name)
		} else if _, err := os.Stat(filepath.Join(backupDir, name)); err == nil {
			src = filepath.Join(backupDir, name)
		}
		st, err := os.Stat(src)
		if err != nil {
			continue
		}
		c, ok := cache[name]
		if !ok || c.Size != st.Size() || c.Mod != st.ModTime().Unix() {
			c = mergeCacheEntry{Size: st.Size(), Mod: st.ModTime().Unix()}
			if lv, err := upk.ScanStreaming(src); err != nil {
				c.Err = err.Error()
			} else {
				for _, l := range lv {
					if l.Class == "LevelStreamingAlwaysLoaded" {
						c.Base = true
						if l.Listed {
							c.Levels = append(c.Levels, l.PackageName)
						}
					}
				}
			}
			if c.Err == "" {
				c.Embed, _ = upk.EmbeddedCount(src)
			}
			cache[name] = c
			changed = true
		}
		b := func(v bool) string {
			if v {
				return "1"
			}
			return "0"
		}
		var lv []upk.Streaming
		for _, n := range c.Levels {
			lv = append(lv, upk.Streaming{Class: "LevelStreamingAlwaysLoaded", PackageName: n, Listed: true})
		}
		// eigene Kulisse (in eigenen Unter-Leveln oder direkt in der Map): kann Kulisse sein und abgeschaltet werden
		hasBD := len(upk.Backdrop(lv)) > 0 || c.Embed >= minEmbedded
		rows = append(rows, row{name, nice, b(hasBD), b(c.Base), b(hasBD)})
	}
	if changed {
		_ = saveJSON(mergeCacheFile, cache)
	}
	sort.Slice(rows, func(i, j int) bool { return strings.ToLower(rows[i].name) < strings.ToLower(rows[j].name) })
	var lines []string
	for _, r := range rows {
		lines = append(lines, strings.Join([]string{r.file, r.name, r.bd, r.base, r.own}, "\t"))
	}
	writeLines(mergeCatFile, lines)
}

// ---------- Anwenden / Zuruecksetzen ----------

// ab so vielen eingebauten Kulissen-Teilen gilt eine Map als "hat Kulisse"
const minEmbedded = 5

// Kulisse, die direkt in einer Map steckt, wird als eigenes Paket neben die Arenen gelegt
func derivedName(src string) string {
	return "Prism_" + strings.TrimSuffix(src, filepath.Ext(src)) + "_BD"
}

// waehrend dieser Spiel-Sitzung neu angelegte Kulissen-Pakete: das Spiel kennt sie erst nach einem Neustart
var derivedNew = map[string]bool{}

// Originaldatei einer Arena (falls gemergt oder getauscht: die Sicherung)
func arenaOriginal(cooked, file string) string {
	if alt := filepath.Join(mergeBackupDir, file); fileExists(alt) {
		return alt
	}
	if alt := filepath.Join(backupDir, file); fileExists(alt) {
		return alt
	}
	return filepath.Join(cooked, file)
}

// Unter-Level, die die Kulisse einer Map ausmachen (baut bei Bedarf das Paket fuer eingebaute Kulisse)
func backdropLevels(cooked, file string) (add []string, wait bool, err error) {
	src := arenaOriginal(cooked, file)
	lv, err := upk.ScanStreaming(src)
	if err != nil {
		return nil, false, err
	}
	add = upk.Backdrop(lv)
	dn := derivedName(file)
	dp := filepath.Join(cooked, dn+".upk")
	if !fileExists(dp) {
		if n, _ := upk.EmbeddedCount(src); n >= minEmbedded {
			data, _, err := upk.BuildBackdropPackage(src)
			if err != nil {
				return nil, false, err
			}
			tmp := dp + ".tmp"
			if err := os.WriteFile(tmp, data, 0644); err != nil {
				return nil, false, friendlyErr(err)
			}
			if err := replaceFile(tmp, dp); err != nil {
				os.Remove(tmp)
				return nil, false, friendlyErr(err)
			}
			if rlRunning() {
				derivedNew[dn] = true
			}
		}
	}
	if fileExists(dp) {
		add = append(add, dn)
		if derivedNew[dn] && rlRunning() {
			wait = true
		}
	}
	if len(add) == 0 {
		return nil, false, fmt.Errorf("%s has no backdrop", prettyArena(file))
	}
	return add, wait, nil
}

func mergeApply(cooked, file string, w mergeWant) (state, msg string) {
	if strings.ContainsAny(file, `\/`) || strings.ContainsAny(w.Backdrop, `\/`) {
		return "error", "Invalid file."
	}
	target := filepath.Join(cooked, file)
	if _, err := os.Stat(target); err != nil {
		return "error", "Arena file not found."
	}
	if _, swapped := loadSwaps()[file]; swapped {
		return "error", "A workshop map is swapped into this arena. Restore it first."
	}
	if rlRunning() && fileLocked(target) {
		return "pending", "Rocket League has this arena open right now. Restart the game from Prism to see the backdrop."
	}
	bk := filepath.Join(mergeBackupDir, file)
	if _, err := os.Stat(bk); err != nil {
		_ = os.MkdirAll(mergeBackupDir, 0755)
		if err := copyFile(target, bk); err != nil {
			return "error", "Backup failed: " + friendlyErr(err).Error()
		}
	}
	var add []string
	if w.Backdrop != "-" { // "-": keine fremde Kulisse, nur die eigene entfernen
		if !fileExists(filepath.Join(cooked, w.Backdrop)) {
			return "error", "Backdrop map not found."
		}
		a, wait, err := backdropLevels(cooked, w.Backdrop)
		if err != nil {
			return "error", "Couldn't read the backdrop: " + err.Error()
		}
		if wait {
			return "pending", "Restart the game from Prism to see this backdrop."
		}
		add = a
	}
	data, err := upk.MergeInto(bk, add, w.Replace)
	if err != nil {
		return "error", "Couldn't merge: " + err.Error()
	}
	tmp := target + ".merge-tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return "error", friendlyErr(err).Error()
	}
	if err := replaceFile(tmp, target); err != nil {
		os.Remove(tmp)
		if rlRunning() {
			return "pending", "Rocket League has this arena open right now. Restart the game from Prism to see the backdrop."
		}
		return "error", friendlyErr(err).Error()
	}
	if len(add) == 0 {
		return "applied", "Own backdrop removed"
	}
	return "applied", "Backdrop: " + prettyArena(w.Backdrop)
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

func mergeRestore(cooked, file string) error {
	bk := filepath.Join(mergeBackupDir, file)
	if !fileExists(bk) {
		return nil
	}
	if err := copyFile(bk, filepath.Join(cooked, file)); err != nil {
		return friendlyErr(err)
	}
	os.Remove(bk)
	return nil
}

// Einmal abgleichen: gewuenschter Stand -> Dateien
func mergeSync(cooked string) {
	mergeMu.Lock()
	defer mergeMu.Unlock()
	if cooked == "" || mergeHold || time.Now().Before(mergeHoldUntil) || eacRunning() {
		return
	}
	wants := loadMergeWants()
	stats := loadMergeStats()
	out := map[string]mergeStat{}
	// nicht mehr gewuenscht: zuruecksetzen
	for f := range stats {
		if _, ok := wants[f]; ok {
			continue
		}
		if fileExists(filepath.Join(mergeBackupDir, f)) {
			if err := mergeRestore(cooked, f); err != nil {
				out[f] = mergeStat{State: "pending", Msg: "Restoring as soon as Rocket League lets go of the file."}
			}
		}
	}
	for f, w := range wants {
		s, had := stats[f]
		if had && s.State == "applied" && s.Backdrop == w.Backdrop && s.Replace == w.Replace && fileExists(filepath.Join(mergeBackupDir, f)) {
			out[f] = s
			continue
		}
		if had && s.State == "error" && s.Backdrop == w.Backdrop && s.Replace == w.Replace {
			out[f] = s // nicht staendig neu versuchen
			continue
		}
		st, msg := mergeApply(cooked, f, w)
		out[f] = mergeStat{Backdrop: w.Backdrop, Replace: w.Replace, State: st, Msg: msg}
		logf("merge %s <- %s: %s %s", f, w.Backdrop, st, msg)
	}
	saveMergeStats(out)
}

// Alles zuruecksetzen (vor Anti-Cheat, beim Beenden). Wuensche bleiben.
func mergeRestoreAll(cooked string) error {
	mergeMu.Lock()
	defer mergeMu.Unlock()
	if cooked == "" {
		return nil
	}
	ents, _ := os.ReadDir(mergeBackupDir)
	var firstErr error
	stats := loadMergeStats()
	for _, e := range ents {
		if e.IsDir() || strings.HasSuffix(e.Name(), ".tmp") {
			continue
		}
		if err := mergeRestore(cooked, e.Name()); err != nil && firstErr == nil {
			firstErr = err
		} else if s, ok := stats[e.Name()]; ok {
			s.State, s.Msg = "pending", "Merged again when you start the game from Prism without Anti-Cheat."
			stats[e.Name()] = s
		}
	}
	saveMergeStats(stats)
	// Kulissen-Pakete wieder weg (gibt es nur ohne Anti-Cheat)
	ds, _ := filepath.Glob(filepath.Join(cooked, "Prism_*_BD.upk"))
	for _, d := range ds {
		if err := os.Remove(d); err != nil && firstErr == nil {
			firstErr = friendlyErr(err)
		}
	}
	return firstErr
}

func logf(format string, a ...interface{}) {
	f, err := os.OpenFile(filepath.Join(dataDir, "prism.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "[%s] %s\n", time.Now().Format("2006-01-02 15:04:05"), fmt.Sprintf(format, a...))
}

// Hintergrund: Katalog bauen, Wuensche beobachten.
// Gemergte Dateien gibt es nur, solange Rocket League ohne Anti-Cheat laeuft (von Prism gestartet):
// beim Start werden alle Wuensche angewendet (mergeBeforeLaunch), waehrend des Spiels neue Wuensche
// sofort (wenn das Spiel die Arena nicht offen hat), und nach dem Spiel kommt alles wieder ins Original.
func mergeLoop(cookedFn func() string) {
	var lastReq time.Time
	var lastCat time.Time
	var lastNames time.Time
	wasRunning := false
	first := true
	for {
		cooked := cookedFn()
		namesMod := time.Time{}
		if st, err := os.Stat(filepath.Join(dataDir, "map_names.txt")); err == nil {
			namesMod = st.ModTime()
		}
		if cooked != "" && (time.Since(lastCat) > 10*time.Minute || !fileExists(mergeCatFile) || !namesMod.Equal(lastNames)) {
			lastNames = namesMod
			lastCat = time.Now()
			func() {
				defer func() { recover() }()
				buildMergeCatalog(cooked)
			}()
		}
		running := rlRunning()
		if cooked != "" {
			func() {
				defer func() { recover() }()
				switch {
				case !running && (wasRunning || first):
					// Spiel zu (oder Reste nach einem Absturz): Originale zurueck
					_ = mergeRestoreAll(cooked)
				case running && !eacRunning():
					st, _ := os.Stat(mergeReqFile)
					changed := st != nil && st.ModTime() != lastReq
					pending := false
					for _, s := range loadMergeStats() {
						if s.State == "pending" {
							pending = true
						}
					}
					if changed || pending {
						if st != nil {
							lastReq = st.ModTime()
						}
						mergeSync(cooked)
					}
				}
			}()
		}
		first = false
		wasRunning = running
		time.Sleep(1500 * time.Millisecond)
	}
}

// Direkt vor dem Start ohne Anti-Cheat: alle gewuenschten Kulissen einbauen (das Spiel hat noch nichts offen)
func mergeBeforeLaunch(cooked string) {
	mergeMu.Lock()
	mergeHoldUntil = time.Time{}
	derivedNew = map[string]bool{} // das Spiel startet neu und findet alle Pakete
	stats := loadMergeStats()
	for f, s := range stats { // alles neu anwenden
		s.State = ""
		stats[f] = s
	}
	saveMergeStats(stats)
	mergeMu.Unlock()
	mergeSync(cooked)
}

var _ = json.Marshal
