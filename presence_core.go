package main

// Plattformunabhaengiger Kern fuer Discord Rich Presence:
//   - liest Nachrichten der offiziellen Rocket League Stats API
//   - baut daraus die Discord-Anzeige
//   - Discord-IPC-Rahmen (Frames)
//   - schaltet die Stats API in TAStatsAPI.ini ein
// (ohne Build-Tag, damit es sich auch unter Linux testen laesst)

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// ---------- Zustand aus der Stats API ----------

type statsTeam struct {
	Name    string `json:"Name"`
	TeamNum int    `json:"TeamNum"`
	Score   int    `json:"Score"`
}

type statsPlayer struct {
	Name      string `json:"Name"`
	PrimaryID string `json:"PrimaryId"`
	TeamNum   int    `json:"TeamNum"`
}

type MatchState struct {
	InMatch   bool
	Guid      string
	Playlist  int
	Arena     string
	Teams     []statsTeam
	Players   []statsPlayer
	TimeLeft  int
	Overtime  bool
	Replay    bool
	Ended     bool
	HasWinner bool
	Winner    string
	Started   time.Time
	Seen      time.Time // letzte Nachricht
}

type statsMsg struct {
	Event string          `json:"Event"`
	Data  json.RawMessage `json:"Data"`
}

// Data kann ein Objekt oder ein JSON-String mit Objekt sein
func statsData(raw json.RawMessage) json.RawMessage {
	raw = bytes.TrimSpace(raw)
	if len(raw) > 0 && raw[0] == '"' {
		var s string
		if json.Unmarshal(raw, &s) == nil {
			return json.RawMessage(s)
		}
	}
	return raw
}

// Verarbeitet eine Nachricht der Stats API
func (m *MatchState) Apply(raw []byte, now time.Time) {
	var msg statsMsg
	if json.Unmarshal(raw, &msg) != nil || msg.Event == "" {
		return
	}
	m.Seen = now
	data := statsData(msg.Data)
	switch msg.Event {
	case "MatchCreated", "MatchInitialized":
		if !m.InMatch || m.Ended {
			*m = MatchState{InMatch: true, Started: now, Seen: now}
		}
	case "MatchDestroyed":
		*m = MatchState{Seen: now}
	case "MatchEnded":
		var d struct {
			WinnerTeamNum *int `json:"WinnerTeamNum"`
		}
		_ = json.Unmarshal(data, &d)
		m.Ended = true
		if d.WinnerTeamNum != nil {
			m.HasWinner, m.Winner = true, strconv.Itoa(*d.WinnerTeamNum)
		}
	case "GoalReplayStart":
		m.Replay = true
	case "GoalReplayEnd", "GoalReplayWillEnd", "RoundStarted":
		m.Replay = false
	case "UpdateState":
		var d struct {
			MatchGuid string        `json:"MatchGuid"`
			Players   []statsPlayer `json:"Players"`
			Game      struct {
				Teams      []statsTeam     `json:"Teams"`
				PlaylistID json.RawMessage `json:"PlaylistId"`
				TimeSec    float64         `json:"TimeSeconds"`
				Overtime   bool            `json:"bOvertime"`
				Replay     bool            `json:"bReplay"`
				HasWinner  bool            `json:"bHasWinner"`
				Winner     json.RawMessage `json:"Winner"`
				Arena      string          `json:"Arena"`
			} `json:"Game"`
		}
		if json.Unmarshal(data, &d) != nil {
			return
		}
		if !m.InMatch || (d.MatchGuid != "" && m.Guid != "" && d.MatchGuid != m.Guid) {
			*m = MatchState{InMatch: true, Started: now, Seen: now}
		}
		if d.MatchGuid != "" {
			m.Guid = d.MatchGuid
		}
		m.Players = d.Players
		m.Teams = d.Game.Teams
		if n, err := strconv.Atoi(strings.Trim(string(d.Game.PlaylistID), `" `)); err == nil {
			m.Playlist = n
		}
		m.TimeLeft = int(d.Game.TimeSec + 0.5)
		m.Overtime = d.Game.Overtime
		m.Replay = d.Game.Replay
		if d.Game.Arena != "" {
			m.Arena = d.Game.Arena
		}
		if d.Game.HasWinner {
			m.HasWinner = true
			m.Winner = strings.Trim(string(d.Game.Winner), `" `)
		}
	}
}

// ---------- Namen ----------

// Einfache Namen: "2s Ranked", "3s Casual", "Freeplay" ...
var playlistNames = map[int]string{
	1: "1s Casual", 2: "2s Casual", 3: "3s Casual", 4: "4s Casual", 7: "Season", 8: "Exhibition",
	9: "Freeplay", 10: "1s Ranked", 11: "2s Ranked", 12: "3s Ranked", 13: "3s Ranked",
	15: "Snow Day", 16: "Rocket Labs", 17: "Hoops", 18: "Rumble", 19: "Freeplay", 20: "Training",
	21: "Training", 22: "Tournament", 23: "Dropshot", 24: "Exhibition", 26: "Ranked",
	27: "Hoops Ranked", 28: "Rumble Ranked", 29: "Dropshot Ranked", 30: "Snow Day Ranked", 34: "Tournament",
	38: "Heatseeker", 41: "Boomer Ball", 43: "Heatseeker", 44: "Winter Breakaway", 46: "Gridiron",
	47: "Super Cube", 48: "Tactical Rumble", 49: "Spring Loaded", 50: "Speed Demon", 54: "Knockout",
}

// Modi ohne Spielstand in der Anzeige
func noScore(mode string) bool { return mode == "Freeplay" || mode == "Training" }

func modeName(m *MatchState) string {
	if n, ok := playlistNames[m.Playlist]; ok {
		return n
	}
	// unbekannte Liste: aus der Teamgroesse ableiten
	size := 0
	for _, p := range m.Players {
		if p.TeamNum == 0 {
			size++
		}
	}
	if size >= 1 && size <= 4 {
		return strconv.Itoa(size) + "s"
	}
	return "Match"
}

var arenaBase = map[string]string{
	"park": "Beckwith Park", "stadium": "DFH Stadium", "eurostadium": "Mannfield", "trainstation": "Urban Central",
	"utopiastadium": "Utopia Coliseum", "beach": "Salty Shores", "chinatown": "Forbidden Temple", "cs": "Champions Field",
	"farm": "Farmstead", "neotokyo": "Neo Tokyo", "outlaw": "Deadeye Canyon", "wasteland": "Wasteland",
	"labs_underpass": "Underpass", "throwbackstadium": "Throwback Stadium", "arc": "Starbase ARC", "underwater": "AquaDome",
	"music": "Neon Fields", "street": "Sovereign Heights", "ff": "Estadio Vida", "woods": "Drift Woods",
	"hoopsstadium": "Dunk House", "shattershot": "Core 707", "swoosh": "Champions Field",
}

var arenaVariant = regexp.MustCompile(`_(p|night|rainy|dusk|day|dawn|foggy|snowy|snow|snownight|winter|spooky|haunted|hw|toon|standard|s|oasis|grs|v2|sunset)$`)

func arenaPretty(raw string) string {
	b := strings.ToLower(strings.TrimSpace(raw))
	if b == "" {
		return ""
	}
	for i := 0; i < 4; i++ {
		if n, ok := arenaBase[b]; ok {
			return n
		}
		nb := arenaVariant.ReplaceAllString(b, "")
		if nb == b {
			break
		}
		b = nb
	}
	if n, ok := arenaBase[b]; ok {
		return n
	}
	// Fallback: "Some_Map_P" -> "Some Map"
	s := arenaVariant.ReplaceAllString(strings.ToLower(raw), "")
	parts := strings.Fields(strings.ReplaceAll(s, "_", " "))
	for i, p := range parts {
		parts[i] = strings.ToUpper(p[:1]) + p[1:]
	}
	return strings.Join(parts, " ")
}

func clock(sec int) string {
	if sec < 0 {
		sec = 0
	}
	return fmt.Sprintf("%d:%02d", sec/60, sec%60)
}

// ---------- Discord-Anzeige ----------

type PresenceCfg struct {
	On       bool
	Match    bool // Modus, Spielstand, Zeit
	Maps     bool // Workshop-Map-Namen
	Launcher bool // auch ohne Spiel
	Button   bool
	Time     bool
}

type PresenceInput struct {
	Version    string
	RLRunning  bool
	EAC        bool
	GameStart  time.Time
	Match      *MatchState
	MyIDs      []string          // eigene Steam-IDs (fuer "mein Team")
	MyName     string
	Swaps      map[string]string // Arena-Datei -> Map-Titel
	Tab        string            // vHome | vDl | vBrowse
	LaunchOpen time.Time
	Hidden     bool // ZLauncher liegt im Tray
	Starting   bool // Play wurde gedrueckt, Spiel startet gerade
	MapCount   int  // heruntergeladene Workshop-Maps
}

type Activity struct {
	Details    string             `json:"details,omitempty"`
	State      string             `json:"state,omitempty"`
	Timestamps *ActivityTimestamps `json:"timestamps,omitempty"`
	Assets     ActivityAssets     `json:"assets"`
	Buttons    []ActivityButton   `json:"buttons,omitempty"`
	Type       int                `json:"type"`
}
type ActivityTimestamps struct {
	Start int64 `json:"start,omitempty"`
}
type ActivityAssets struct {
	LargeImage string `json:"large_image,omitempty"`
	LargeText  string `json:"large_text,omitempty"`
	SmallImage string `json:"small_image,omitempty"`
	SmallText  string `json:"small_text,omitempty"`
}
type ActivityButton struct {
	Label string `json:"label"`
	URL   string `json:"url"`
}

const zlReleaseURL = "https://github.com/zusmori/zusmoris-map-swapper/releases/latest"

func myTeam(m *MatchState, ids []string, name string) int {
	for _, p := range m.Players {
		for _, id := range ids {
			if id != "" && strings.Contains(p.PrimaryID, id) {
				return p.TeamNum
			}
		}
	}
	for _, p := range m.Players {
		if name != "" && strings.EqualFold(p.Name, name) {
			return p.TeamNum
		}
	}
	return -1
}

func swapTitleFor(swaps map[string]string, arena string) string {
	a := strings.ToLower(arena)
	for file, title := range swaps {
		f := strings.ToLower(strings.TrimSuffix(file, ".upk"))
		f = strings.TrimSuffix(f, ".udk")
		if f == a || strings.HasPrefix(a, f) {
			return title
		}
	}
	return ""
}

func cut128(s string) string {
	r := []rune(s)
	if len(r) > 120 {
		return string(r[:117]) + "…"
	}
	return s
}

// BuildActivity: nil = nichts anzeigen
func BuildActivity(cfg PresenceCfg, in PresenceInput) *Activity {
	if !cfg.On {
		return nil
	}
	a := &Activity{Assets: ActivityAssets{LargeImage: "logo", LargeText: "ZLauncher " + strings.TrimSpace("v"+in.Version)}}
	if cfg.Button {
		a.Buttons = []ActivityButton{{Label: "Get ZLauncher", URL: zlReleaseURL}}
	}
	start := time.Time{}
	switch {
	case in.RLRunning:
		start = in.GameStart
		m := in.Match
		fresh := m != nil && m.InMatch && time.Since(m.Seen) < 30*time.Second
		// Workshop-Map im Freeplay
		if fresh && cfg.Maps && in.Swaps != nil && m.Arena != "" {
			if t := swapTitleFor(in.Swaps, m.Arena); t != "" {
				a.Details, a.State = "Rocket League · Freeplay", t
				break
			}
		}
		if fresh && cfg.Match {
			mode := modeName(m)
			a.Details = "Rocket League · " + mode
			if !m.Started.IsZero() {
				start = m.Started
			}
			if noScore(mode) {
				break
			}
			my := myTeam(m, in.MyIDs, in.MyName)
			var us, them int
			for _, t := range m.Teams {
				if t.TeamNum == my || (my < 0 && t.TeamNum == 0) {
					us = t.Score
				} else {
					them = t.Score
				}
			}
			a.State = fmt.Sprintf("%d : %d", us, them)
			switch {
			case m.Ended || m.HasWinner:
				if w, err := strconv.Atoi(m.Winner); err == nil && my >= 0 {
					if w == my {
						a.State = "Won " + a.State
					} else {
						a.State = "Lost " + a.State
					}
				}
			case m.Overtime:
				a.State += " (OT)"
			}
			break
		}
		// keine laufende Partie: Hauptmenue (mit und ohne Anti-Cheat)
		a.Details = "Rocket League · Menu"
	case in.Starting:
		a.Details, a.State = "Starting Rocket League", "Almost ready …"
	case cfg.Launcher && in.Hidden:
		// im Tray: nur noch "Idle"
		start = in.LaunchOpen
		a.Details = "Idle"
	case cfg.Launcher:
		start = in.LaunchOpen
		switch in.Tab {
		case "vDl":
			a.Details = "Swapping workshop maps"
			switch {
			case in.MapCount == 1:
				a.State = "1 map ready to play"
			case in.MapCount > 1:
				a.State = fmt.Sprintf("%d maps ready to play", in.MapCount)
			default:
				a.State = "Picking a map"
			}
		case "vBrowse":
			a.Details, a.State = "Browsing workshop maps", "Looking for something new"
		default: // Home
			a.Details = "Idle"
		}
	default:
		return nil
	}
	a.Details, a.State = cut128(a.Details), cut128(a.State)
	if cfg.Time && !start.IsZero() {
		a.Timestamps = &ActivityTimestamps{Start: start.Unix()}
	}
	return a
}

// ---------- Discord-IPC-Rahmen ----------

const (
	ipcHandshake = 0
	ipcFrame     = 1
	ipcClose     = 2
)

func writeFrame(w io.Writer, op uint32, payload []byte) error {
	buf := make([]byte, 8+len(payload))
	binary.LittleEndian.PutUint32(buf[0:], op)
	binary.LittleEndian.PutUint32(buf[4:], uint32(len(payload)))
	copy(buf[8:], payload)
	_, err := w.Write(buf)
	return err
}

func readFrame(r io.Reader) (uint32, []byte, error) {
	var h [8]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return 0, nil, err
	}
	op := binary.LittleEndian.Uint32(h[0:])
	n := binary.LittleEndian.Uint32(h[4:])
	if n > 1<<20 {
		return 0, nil, errors.New("frame too large")
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(r, b); err != nil {
		return 0, nil, err
	}
	return op, b, nil
}

// ---------- TAStatsAPI.ini ----------

// Schaltet die Stats API ein (PacketSendRate > 0, Port gesetzt). Gibt neuen Inhalt, Port und ob sich etwas geaendert hat zurueck.
func enableStatsIni(content string) (string, int, bool) {
	const sec = "[TAGame.MatchStatsExporter_TA]"
	nl := "\r\n"
	if content != "" && !strings.Contains(content, "\r\n") {
		nl = "\n"
	}
	lines := strings.Split(strings.ReplaceAll(content, "\r\n", "\n"), "\n")
	start := -1
	for i, l := range lines {
		if strings.EqualFold(strings.TrimSpace(l), sec) {
			start = i
			break
		}
	}
	port, changed := 49123, false
	if start < 0 {
		head := []string{sec, "Port=49123", "WebPort=49124", "PacketSendRate=2", ""}
		if len(lines) == 1 && lines[0] == "" {
			lines = nil
		}
		lines = append(head, lines...)
		return strings.Join(lines, nl), port, true
	}
	end := len(lines)
	for i := start + 1; i < len(lines); i++ {
		if strings.HasPrefix(strings.TrimSpace(lines[i]), "[") {
			end = i
			break
		}
	}
	haveRate, havePort := false, false
	for i := start + 1; i < end; i++ {
		k, v, ok := strings.Cut(strings.TrimSpace(lines[i]), "=")
		if !ok {
			continue
		}
		switch strings.ToLower(strings.TrimSpace(k)) {
		case "packetsendrate":
			haveRate = true
			if f, err := strconv.ParseFloat(strings.TrimSpace(v), 64); err != nil || f <= 0 {
				lines[i] = "PacketSendRate=2"
				changed = true
			}
		case "port":
			havePort = true
			if p, err := strconv.Atoi(strings.TrimSpace(v)); err == nil && p > 0 {
				port = p
			} else {
				lines[i] = "Port=49123"
				changed = true
			}
		}
	}
	var add []string
	if !havePort {
		add = append(add, "Port=49123")
	}
	if !haveRate {
		add = append(add, "PacketSendRate=2")
	}
	if len(add) > 0 {
		changed = true
		ins := start + 1
		lines = append(lines[:ins], append(add, lines[ins:]...)...)
	}
	return strings.Join(lines, nl), port, changed
}
