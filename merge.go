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
	"errors"
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
	mergeCacheFile = filepath.Join(dataDir, "merge_cache5.json")
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

// Spiel neu starten (setzt main): schliessen, warten, ohne Anti-Cheat starten
var mergeRestart func()
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
			k = strings.ToLower(k)
			props[k] = strings.TrimSpace(v)
			if k == "name" || k == "mapname" || k == "packagename" || k == "filename" {
				keys = append(keys, strings.ToLower(strings.TrimSpace(v)))
			}
		}
		// Name, den das Spiel anzeigt: Grundname + Variante, z. B. "Farmstead" + "Night" -> "Farmstead (Night)"
		base := props["fn:getlocalizedbasename"]
		variant := props["fn:getlocalizedvariantname"]
		if base == "" || strings.Contains(base, "_") {
			continue
		}
		name := base
		if variant != "" && !strings.Contains(variant, "_") && !strings.Contains(strings.ToLower(base), strings.ToLower(variant)) {
			name += " (" + variant + ")"
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

// ---------- Kombinationen ----------

// Feste Kombinationen: Arena (eigene Kulisse aus) + Kulisse einer anderen Map. Nur diese werden angeboten;
// alle noetigen Dateien baut Prism vor dem Spielstart, so geht das Umschalten im Spiel ohne Neustart.
type mergeCombo struct{ Base, Backdrop string }

var mergeCombos = []mergeCombo{
	{"Farm_GRS_P.upk", "Underwater_P.upk"},
	{"Farm_GRS_P.upk", "Park_Snowy_P.upk"},
	{"Farm_GRS_P.upk", "Street_P.upk"},
	{"Park_Night_P.upk", "Underwater_P.upk"},
	{"Park_Night_P.upk", "Street_P.upk"},
	{"Outlaw_Oasis_P.upk", "Underwater_P.upk"},
	{"Outlaw_Oasis_P.upk", "Park_Snowy_P.upk"},
	{"Outlaw_Oasis_P.upk", "Street_P.upk"},
}

// Wie eine Map-Datei aufgeteilt ist (zwischengespeichert, Lesen dauert)
type mergeCacheEntry struct {
	Size     int64    `json:"size"`
	Mod      int64    `json:"mod"`
	Levels   []string `json:"levels"` // immer geladene Unter-Level (in der Liste)
	Backdrop int      `json:"backdrop"`
	Rest     int      `json:"rest"`
	Err      string   `json:"err,omitempty"`
}

var mergeCacheMu sync.Mutex

// Datei im Spielordner zu einem Paketnamen (Gross/Klein egal)
func cookedFile(cooked, pkg string) string {
	p := filepath.Join(cooked, pkg+".upk")
	if fileExists(p) {
		return p
	}
	ents, _ := os.ReadDir(cooked)
	for _, e := range ents {
		if strings.EqualFold(e.Name(), pkg+".upk") {
			return filepath.Join(cooked, e.Name())
		}
	}
	return ""
}

// Aufteilung einer Datei (path = Original), mit Zwischenspeicher
func mapInfo(path string) (mergeCacheEntry, error) {
	st, err := os.Stat(path)
	if err != nil {
		return mergeCacheEntry{}, err
	}
	key := strings.ToLower(filepath.Base(path))
	mergeCacheMu.Lock()
	cache := map[string]mergeCacheEntry{}
	loadJSON(mergeCacheFile, &cache)
	c, ok := cache[key]
	mergeCacheMu.Unlock()
	if ok && c.Size == st.Size() && c.Mod == st.ModTime().Unix() {
		if c.Err != "" {
			return c, errors.New(c.Err)
		}
		return c, nil
	}
	c = mergeCacheEntry{Size: st.Size(), Mod: st.ModTime().Unix()}
	if lv, err := upk.ScanStreaming(path); err == nil {
		for _, l := range lv {
			if l.Class == "LevelStreamingAlwaysLoaded" && l.Listed {
				c.Levels = append(c.Levels, l.PackageName)
			}
		}
	}
	if b, r, err := upk.ClassifyFile(path); err != nil {
		c.Err = err.Error()
	} else {
		c.Backdrop, c.Rest = b, r
	}
	mergeCacheMu.Lock()
	cache = map[string]mergeCacheEntry{}
	loadJSON(mergeCacheFile, &cache)
	cache[key] = c
	_ = saveJSON(mergeCacheFile, cache)
	mergeCacheMu.Unlock()
	if c.Err != "" {
		return c, errors.New(c.Err)
	}
	return c, nil
}

func isSoundLevel(n string) bool {
	n = strings.ToLower(n)
	return strings.Contains(n, "sfx") || strings.Contains(n, "audio")
}

// Ein Paket, das Prism selbst baut (aus src): Kulisse (BD) oder ohne Kulisse (NB)
type mergePkg struct {
	Name, Src string
	NoBD      bool
}

// Plan fuer eine Kombination: was die Arena zusaetzlich laden soll, was nicht mehr, welche Pakete es braucht
type mergePlan struct {
	Add, Drop []string
	Pkgs      []mergePkg
}

// Eigene Kulisse der Arena aus: reine Kulissen-Unter-Level weg, gemischte durch eine Kopie ohne Kulisse ersetzen
func (pl *mergePlan) ownOff(cooked, base string) error {
	info, err := mapInfo(arenaOriginal(cooked, base))
	if err != nil {
		return err
	}
	for _, sub := range info.Levels {
		if isSoundLevel(sub) {
			continue
		}
		f := cookedFile(cooked, sub)
		if f == "" {
			continue
		}
		si, err := mapInfo(f)
		if err != nil || si.Backdrop == 0 {
			continue
		}
		pl.Drop = append(pl.Drop, sub)
		if si.Rest > 0 { // Boden, Spielfeld o. ae. steckt mit drin: das bleibt
			n := "Prism_" + sub + "_NB"
			pl.Add = append(pl.Add, n)
			pl.Pkgs = append(pl.Pkgs, mergePkg{n, f, true})
		}
	}
	return nil
}

// Kulisse einer Map dazu: reine Kulissen-Unter-Level direkt, sonst Kopien nur mit der Kulisse
func (pl *mergePlan) backdrop(cooked, src string) error {
	orig := arenaOriginal(cooked, src)
	info, err := mapInfo(orig)
	if err != nil {
		return err
	}
	before := len(pl.Add)
	if info.Backdrop > 0 {
		n := "Prism_" + strings.TrimSuffix(src, filepath.Ext(src)) + "_BD"
		pl.Add = append(pl.Add, n)
		pl.Pkgs = append(pl.Pkgs, mergePkg{n, orig, false})
	}
	for _, sub := range info.Levels {
		if isSoundLevel(sub) {
			continue
		}
		f := cookedFile(cooked, sub)
		if f == "" {
			continue
		}
		si, err := mapInfo(f)
		if err != nil || si.Backdrop == 0 {
			continue
		}
		if si.Rest == 0 {
			pl.Add = append(pl.Add, sub)
		} else {
			n := "Prism_" + sub + "_BD"
			pl.Add = append(pl.Add, n)
			pl.Pkgs = append(pl.Pkgs, mergePkg{n, f, false})
		}
	}
	if len(pl.Add) == before {
		return fmt.Errorf("%s has no backdrop", prettyArena(src))
	}
	return nil
}

func planFor(cooked, base string, w mergeWant) (*mergePlan, error) {
	pl := &mergePlan{}
	if w.Replace {
		if err := pl.ownOff(cooked, base); err != nil {
			return nil, err
		}
	}
	if w.Backdrop != "-" {
		if err := pl.backdrop(cooked, w.Backdrop); err != nil {
			return nil, err
		}
	}
	return pl, nil
}

// Gebaute Pakete liegen in Prism (merge_pkgs) und werden nur ohne Anti-Cheat in den Spielordner kopiert
var mergePkgDir = filepath.Join(dataDir, "merge_pkgs")

type pkgMeta struct {
	Src  string `json:"src"`
	Size int64  `json:"size"`
	Mod  int64  `json:"mod"`
	Ver  int    `json:"ver"`
}

const pkgVersion = 2 // erhoehen, wenn sich das Bauen aendert

// Paket bauen (falls noetig) und in den Spielordner legen. fresh: wurde gerade neu in den Spielordner gelegt
func ensurePkg(cooked string, pk mergePkg) (fresh bool, err error) {
	return ensurePkgTo(cooked, pk, true)
}

func ensurePkgTo(cooked string, pk mergePkg, place bool) (fresh bool, err error) {
	_ = os.MkdirAll(mergePkgDir, 0755)
	cached := filepath.Join(mergePkgDir, pk.Name+".upk")
	metaF := filepath.Join(mergePkgDir, pk.Name+".json")
	st, err := os.Stat(pk.Src)
	if err != nil {
		return false, err
	}
	want := pkgMeta{filepath.Base(pk.Src), st.Size(), st.ModTime().Unix(), pkgVersion}
	var have pkgMeta
	loadJSON(metaF, &have)
	if have != want || !fileExists(cached) {
		var data []byte
		if pk.NoBD {
			data, err = upk.BuildNoBackdropPackage(pk.Src)
		} else {
			data, _, err = upk.BuildBackdropPackage(pk.Src)
		}
		if err != nil {
			return false, err
		}
		if err := os.WriteFile(cached+".tmp", data, 0644); err != nil {
			return false, friendlyErr(err)
		}
		if err := replaceFile(cached+".tmp", cached); err != nil {
			return false, friendlyErr(err)
		}
		_ = saveJSON(metaF, want)
		logf("merge: built %s from %s", pk.Name, filepath.Base(pk.Src))
	}
	if !place {
		return false, nil
	}
	dst := filepath.Join(cooked, pk.Name+".upk")
	if cs, err := os.Stat(cached); err == nil {
		if ds, err := os.Stat(dst); err == nil && ds.Size() == cs.Size() && !ds.ModTime().Before(cs.ModTime()) {
			return false, nil
		}
	}
	if err := copyFile(cached, dst); err != nil {
		return false, friendlyErr(err)
	}
	return true, nil
}

// Alle Pakete fuer alle Kombinationen bereitlegen (vor dem Spielstart: dann kennt das Spiel sie alle)
func preparePackages(cooked string) { preparePackagesTo(cooked, true) }

// place=false: nur bauen (im Hintergrund, damit der Spielstart danach schnell geht)
func preparePackagesTo(cooked string, place bool) {
	seen := map[string]bool{}
	for _, c := range mergeCombos {
		if !fileExists(filepath.Join(cooked, c.Base)) || !fileExists(filepath.Join(cooked, c.Backdrop)) {
			continue
		}
		pl, err := planFor(cooked, c.Base, mergeWant{c.Backdrop, true})
		if err != nil {
			logf("merge: %s + %s: %v", c.Base, c.Backdrop, err)
			continue
		}
		for _, pk := range pl.Pkgs {
			if seen[pk.Name] {
				continue
			}
			seen[pk.Name] = true
			if _, err := ensurePkgTo(cooked, pk, place); err != nil {
				logf("merge: package %s: %v", pk.Name, err)
			}
		}
	}
}

func isCombo(base string, w mergeWant) bool {
	for _, c := range mergeCombos {
		if strings.EqualFold(c.Base, base) && strings.EqualFold(c.Backdrop, w.Backdrop) && w.Replace {
			return true
		}
	}
	return false
}

// Katalog fuer das F8-Menue: Arena \t Name \t Kulissen-Map \t Name
func buildMergeCatalog(cooked string) {
	if cooked == "" {
		return
	}
	gameNames := gameMapNames()
	name := func(file string) string {
		lo := strings.ToLower(strings.TrimSuffix(file, filepath.Ext(file)))
		if n := gameNames[lo]; n != "" {
			return n
		}
		if n, ok := soccarArenas[lo]; ok {
			return n
		}
		return prettyArena(file)
	}
	var lines []string
	for _, c := range mergeCombos {
		if !fileExists(filepath.Join(cooked, c.Base)) || !fileExists(filepath.Join(cooked, c.Backdrop)) {
			continue
		}
		lines = append(lines, strings.Join([]string{c.Base, name(c.Base), c.Backdrop, name(c.Backdrop)}, "\t"))
	}
	writeLines(mergeCatFile, lines)
}

// ---------- Anwenden / Zuruecksetzen ----------

// waehrend dieser Spiel-Sitzung neu in den Spielordner gelegte Pakete: das Spiel kennt sie erst nach einem Neustart
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
	if w.Backdrop != "-" && !fileExists(filepath.Join(cooked, w.Backdrop)) {
		return "error", "Scenery map not found."
	}
	if rlRunning() && fileLocked(target) {
		return "pending", "Rocket League has this arena open right now. Restart the game to see the scenery."
	}
	pl, err := planFor(cooked, file, w)
	if err != nil {
		return "error", "Couldn't read the maps: " + err.Error()
	}
	wait := false
	for _, pk := range pl.Pkgs {
		fresh, err := ensurePkg(cooked, pk)
		if err != nil {
			return "error", "Couldn't build " + pk.Name + ": " + err.Error()
		}
		if fresh && rlRunning() {
			derivedNew[pk.Name] = true
		}
		if derivedNew[pk.Name] && rlRunning() {
			wait = true
		}
	}
	if wait {
		return "pending", "Restart the game to see this scenery."
	}
	bk := filepath.Join(mergeBackupDir, file)
	if _, err := os.Stat(bk); err != nil {
		_ = os.MkdirAll(mergeBackupDir, 0755)
		if err := copyFile(target, bk); err != nil {
			return "error", "Backup failed: " + friendlyErr(err).Error()
		}
	}
	data, err := upk.MergeInto(bk, pl.Add, pl.Drop, w.Replace)
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
			return "pending", "Rocket League has this arena open right now. Restart the game to see the scenery."
		}
		return "error", friendlyErr(err).Error()
	}
	logf("merge %s: add %v drop %v", file, pl.Add, pl.Drop)
	if w.Backdrop == "-" {
		return "applied", "Own backdrop removed"
	}
	return "applied", "Scenery: " + prettyArena(w.Backdrop)
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
	// immer nur eine Kulisse gleichzeitig: die zuletzt gewuenschte (letzte Zeile der Datei) gilt
	if len(wants) > 1 {
		last := ""
		for _, l := range readLines(mergeReqFile) {
			if len(l) >= 2 && l[0] != "" {
				last = l[0]
			}
		}
		for f := range wants {
			if f != last {
				delete(wants, f)
			}
		}
	}
	stats := loadMergeStats()
	out := map[string]mergeStat{}
	// nicht mehr gewuenscht: zuruecksetzen
	for f := range stats {
		if w, ok := wants[f]; ok && isCombo(f, w) {
			continue
		}
		if fileExists(filepath.Join(mergeBackupDir, f)) {
			if err := mergeRestore(cooked, f); err != nil {
				out[f] = mergeStat{State: "pending", Msg: "Restoring as soon as Rocket League lets go of the file."}
			}
		}
	}
	for f, w := range wants {
		if !isCombo(f, w) {
			continue // nur die festen Kombinationen (aeltere Wuensche werden zurueckgesetzt)
		}
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
	ds, _ := filepath.Glob(filepath.Join(cooked, "Prism_*.upk"))
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
	warmed := false
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
			if !warmed {
				warmed = true
				go func() { // Pakete schon mal bauen, damit der Spielstart danach schnell geht
					defer func() { recover() }()
					preparePackagesTo(cooked, false)
				}()
			}
		}
		running := rlRunning()
		// "Restart game" im F8-Menue (Merge): Spiel schliessen und ohne Anti-Cheat neu starten
		if rf := filepath.Join(dataDir, "merge_restart.txt"); fileExists(rf) {
			os.Remove(rf)
			if running && !eacRunning() && cooked != "" && mergeRestart != nil {
				logf("merge: restart requested from the F8 menu")
				go mergeRestart()
			}
		}
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
	// alle Pakete fuer die Kombinationen in den Spielordner (das Spiel kennt beim Start dann alle)
	preparePackages(cooked)
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
