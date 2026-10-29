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
	mergeCacheFile = filepath.Join(dataDir, "merge_cache.json")
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
	Size     int64    `json:"size"`
	Mod      int64    `json:"mod"`
	Backdrop []string `json:"backdrop"`
	Base     bool     `json:"base"`
	Err      string   `json:"err,omitempty"`
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

func buildMergeCatalog(cooked string) {
	if cooked == "" {
		return
	}
	cache := map[string]mergeCacheEntry{}
	loadJSON(mergeCacheFile, &cache)
	files, _ := filepath.Glob(filepath.Join(cooked, "*_P.upk"))
	type row struct{ file, name, bd, base string }
	var rows []row
	changed := false
	for _, f := range files {
		name := filepath.Base(f)
		lo := strings.ToLower(name)
		if strings.HasPrefix(lo, "menu_") {
			continue
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
				c.Backdrop = upk.Backdrop(lv)
				for _, l := range lv {
					if l.Class == "LevelStreamingAlwaysLoaded" {
						c.Base = true
					}
				}
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
		rows = append(rows, row{name, prettyArena(name), b(len(c.Backdrop) > 0), b(c.Base)})
	}
	if changed {
		_ = saveJSON(mergeCacheFile, cache)
	}
	sort.Slice(rows, func(i, j int) bool { return strings.ToLower(rows[i].name) < strings.ToLower(rows[j].name) })
	var lines []string
	for _, r := range rows {
		lines = append(lines, strings.Join([]string{r.file, r.name, r.bd, r.base}, "\t"))
	}
	writeLines(mergeCatFile, lines)
}

// ---------- Anwenden / Zuruecksetzen ----------

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
	bdPath := filepath.Join(cooked, w.Backdrop)
	if alt := filepath.Join(mergeBackupDir, w.Backdrop); fileExists(alt) {
		bdPath = alt // Kulissen-Map ist selbst gemergt: ihr Original nehmen
	} else if alt := filepath.Join(backupDir, w.Backdrop); fileExists(alt) {
		bdPath = alt
	}
	data, added, err := upk.MergeFiles(bk, bdPath, w.Replace)
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
	return "applied", "Backdrop: " + strings.Join(added, ", ")
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
	wasRunning := false
	first := true
	for {
		cooked := cookedFn()
		if cooked != "" && (time.Since(lastCat) > 10*time.Minute || !fileExists(mergeCatFile)) {
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
