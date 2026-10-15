//go:build windows

package main

// Discord Rich Presence fuer Z Launcher:
//   - spricht ueber die lokale Named Pipe mit dem Discord-Client (discord-ipc-0..9)
//   - liest den Spielstatus ueber die offizielle Rocket League Stats API (TCP auf 127.0.0.1)
//   - aktualisiert die Anzeige hoechstens alle paar Sekunden (Discord-Limit)

import (
	"encoding/json"
	"errors"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf16"

	"golang.org/x/sys/windows"
)

const discordAppID = "1560431037614530642"

// ---------- Discord-Verbindung ----------

type discordIPC struct{ f *os.File }

var errNoDiscord = errors.New("Discord isn't running")

// Antwort lesen, notfalls nach Zeitlimit abbrechen (Pipe schliessen loest das Lesen)
func (d *discordIPC) read(timeout time.Duration) (uint32, []byte, error) {
	type res struct {
		op  uint32
		b   []byte
		err error
	}
	ch := make(chan res, 1)
	go func() { op, b, err := readFrame(d.f); ch <- res{op, b, err} }()
	select {
	case r := <-ch:
		return r.op, r.b, r.err
	case <-time.After(timeout):
		d.f.Close()
		return 0, nil, errors.New("Discord didn't answer")
	}
}

func dialDiscord(appID string) (*discordIPC, error) {
	for i := 0; i < 10; i++ {
		f, err := os.OpenFile(`\\.\pipe\discord-ipc-`+strconv.Itoa(i), os.O_RDWR, 0)
		if err != nil {
			continue
		}
		d := &discordIPC{f}
		hs, _ := json.Marshal(map[string]interface{}{"v": 1, "client_id": appID})
		if writeFrame(f, ipcHandshake, hs) != nil {
			f.Close()
			continue
		}
		op, _, err := d.read(5 * time.Second)
		if err != nil || op == ipcClose {
			f.Close()
			continue
		}
		return d, nil
	}
	return nil, errNoDiscord
}

var nonceN int64

func (d *discordIPC) setActivity(a *Activity) error {
	nonceN++
	args := map[string]interface{}{"pid": os.Getpid(), "activity": a}
	p, _ := json.Marshal(map[string]interface{}{"cmd": "SET_ACTIVITY", "args": args, "nonce": strconv.FormatInt(time.Now().UnixNano(), 36) + strconv.FormatInt(nonceN, 10)})
	if err := writeFrame(d.f, ipcFrame, p); err != nil {
		return err
	}
	op, b, err := d.read(5 * time.Second)
	if err != nil {
		return err
	}
	if op == ipcClose {
		return errors.New("Discord closed the connection")
	}
	var r struct {
		Evt  string `json:"evt"`
		Data struct {
			Message string `json:"message"`
		} `json:"data"`
	}
	if json.Unmarshal(b, &r) == nil && r.Evt == "ERROR" {
		return errors.New(r.Data.Message)
	}
	return nil
}

func (d *discordIPC) close() { d.f.Close() }

// ---------- Stats API ----------

func documentsDir() string {
	if p, err := windows.KnownFolderPath(windows.FOLDERID_Documents, 0); err == nil && p != "" {
		return p
	}
	return filepath.Join(os.Getenv("USERPROFILE"), "Documents")
}

func statsIniPath() string {
	return filepath.Join(documentsDir(), "My Games", "Rocket League", "TAGame", "Config", "TAStatsAPI.ini")
}

// ini lesen (ASCII/UTF-8 oder UTF-16 mit BOM) -> Text + ob UTF-16
func readIniText(p string) (string, bool, error) {
	b, err := os.ReadFile(p)
	if err != nil {
		return "", false, err
	}
	if len(b) >= 2 && b[0] == 0xFF && b[1] == 0xFE {
		u := make([]uint16, (len(b)-2)/2)
		for i := range u {
			u[i] = uint16(b[2+2*i]) | uint16(b[3+2*i])<<8
		}
		return string(utf16.Decode(u)), true, nil
	}
	return strings.TrimPrefix(string(b), "\ufeff"), false, nil
}

func writeIniText(p, s string, wide bool) error {
	_ = os.MkdirAll(filepath.Dir(p), 0755)
	if !wide {
		return os.WriteFile(p, []byte(s), 0644)
	}
	u := utf16.Encode([]rune(s))
	b := []byte{0xFF, 0xFE}
	for _, c := range u {
		b = append(b, byte(c), byte(c>>8))
	}
	return os.WriteFile(p, b, 0644)
}

// Stats API einschalten (wirkt ab dem naechsten Spielstart)
func ensureStatsAPI() (port int, status string) {
	p := statsIniPath()
	txt, wide, err := readIniText(p)
	if err != nil && !os.IsNotExist(err) {
		return 49123, "error"
	}
	out, port, changed := enableStatsIni(txt)
	if !changed {
		return port, "on"
	}
	if err := writeIniText(p, out, wide); err != nil {
		return port, "error"
	}
	return port, "enabled"
}

type statsClient struct {
	mu        sync.Mutex
	st        MatchState
	port      int
	iniStatus string // on | enabled | error
	connected bool
}

func (c *statsClient) snapshot() (MatchState, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.st, c.connected
}

func (c *statsClient) loop() {
	for {
		if !rlRunning() {
			c.mu.Lock()
			c.st, c.connected = MatchState{}, false
			c.mu.Unlock()
			time.Sleep(3 * time.Second)
			continue
		}
		conn, err := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(c.port), time.Second)
		if err != nil {
			time.Sleep(3 * time.Second)
			continue
		}
		c.mu.Lock()
		c.connected = true
		c.mu.Unlock()
		dec := json.NewDecoder(conn)
		for {
			var raw json.RawMessage
			if err := dec.Decode(&raw); err != nil {
				break
			}
			c.mu.Lock()
			c.st.Apply(raw, time.Now())
			c.mu.Unlock()
		}
		conn.Close()
		c.mu.Lock()
		c.connected = false
		c.mu.Unlock()
		time.Sleep(2 * time.Second)
	}
}

// ---------- Steuerung ----------

type PresenceStatus struct {
	On        bool      `json:"on"`
	Discord   string    `json:"discord"` // connected | missing | error | off
	Error     string    `json:"error,omitempty"`
	Stats     string    `json:"stats"` // live | on | enabled | error
	Activity  *Activity `json:"activity"`
	RLRunning bool      `json:"rlRunning"`
}

type presenceMgr struct {
	mu         sync.Mutex
	cfg        PresenceCfg
	tab        string
	hidden     bool
	starting   bool
	mapCount   int
	opened     time.Time
	gameStart  time.Time
	ipc        *discordIPC
	lastJSON   string
	lastSend   time.Time
	lastDial   time.Time
	status     PresenceStatus
	stats      *statsClient
	swaps      map[string]string
	swapsAt    time.Time
	steamID    string
	steamName  string
	steamAt    time.Time
}

var presence = &presenceMgr{
	cfg:    PresenceCfg{On: true, Match: true, Maps: true, Launcher: true, Button: true, Time: true},
	tab:    "vHome",
	opened: time.Now(),
}

func presenceInit() {
	st := map[string]interface{}{}
	loadJSON(filepath.Join(dataDir, "settings.json"), &st)
	b := func(k string, def bool) bool {
		if v, ok := st[k].(bool); ok {
			return v
		}
		return def
	}
	presence.cfg = PresenceCfg{On: b("rpc", true), Match: b("rpcMatch", true), Maps: b("rpcMaps", true),
		Launcher: b("rpcLauncher", true), Button: b("rpcButton", true), Time: b("rpcTime", true)}
	port, ini := ensureStatsAPI()
	presence.stats = &statsClient{port: port, iniStatus: ini}
	go presence.stats.loop()
	go func() {
		for {
			presence.tick()
			time.Sleep(2 * time.Second)
		}
	}()
}

func (p *presenceMgr) swapTitles() map[string]string {
	if time.Since(p.swapsAt) < 15*time.Second && p.swaps != nil {
		return p.swaps
	}
	out := map[string]string{}
	sw := loadSwaps()
	if len(sw) > 0 {
		titles := map[string]string{}
		for _, m := range workshopMaps() {
			titles[strings.ToLower(m.Name)] = m.Title
		}
		for arena, name := range sw {
			if t := titles[strings.ToLower(name)]; t != "" {
				out[arena] = t
			} else {
				out[arena] = strings.TrimSuffix(name, filepath.Ext(name))
			}
		}
	}
	p.swaps, p.swapsAt = out, time.Now()
	return out
}

func (p *presenceMgr) tick() {
	p.mu.Lock()
	defer p.mu.Unlock()
	running := rlRunning()
	if running && p.gameStart.IsZero() {
		p.gameStart = time.Now()
	} else if !running {
		p.gameStart = time.Time{}
	}
	if time.Since(p.steamAt) > time.Minute {
		p.steamID, p.steamName = steamUser()
		p.steamAt = time.Now()
	}
	match, live := p.stats.snapshot()
	in := PresenceInput{Version: appVersion, RLRunning: running, GameStart: p.gameStart, Match: &match,
		MyIDs: []string{p.steamID}, MyName: p.steamName, Tab: p.tab, LaunchOpen: p.opened,
		Hidden: p.hidden, Starting: p.starting && !running, MapCount: p.mapCount}
	if running {
		in.EAC = eacRunning()
		if p.cfg.Maps {
			in.Swaps = p.swapTitles()
		}
	}
	act := BuildActivity(p.cfg, in)

	p.status.On, p.status.RLRunning, p.status.Activity = p.cfg.On, running, act
	switch {
	case live:
		p.status.Stats = "live"
	default:
		p.status.Stats = p.stats.iniStatus
	}
	if !p.cfg.On {
		if p.ipc != nil {
			_ = p.ipc.setActivity(nil)
			p.ipc.close()
			p.ipc = nil
		}
		p.status.Discord, p.status.Error, p.lastJSON = "off", "", ""
		return
	}
	// verbinden (bei Bedarf), hoechstens alle 10 s versuchen
	if p.ipc == nil {
		if time.Since(p.lastDial) < 10*time.Second {
			return
		}
		p.lastDial = time.Now()
		d, err := dialDiscord(discordAppID)
		if err != nil {
			p.status.Discord, p.status.Error = "missing", ""
			return
		}
		p.ipc, p.lastJSON = d, "-"
	}
	js, _ := json.Marshal(act)
	if string(js) == p.lastJSON || time.Since(p.lastSend) < 4*time.Second {
		p.status.Discord = "connected"
		return
	}
	if err := p.ipc.setActivity(act); err != nil {
		p.ipc.close()
		p.ipc = nil
		p.status.Discord, p.status.Error = "error", err.Error()
		return
	}
	p.lastJSON, p.lastSend = string(js), time.Now()
	p.status.Discord, p.status.Error = "connected", ""
}

func getPresence() PresenceStatus {
	presence.mu.Lock()
	defer presence.mu.Unlock()
	return presence.status
}

func setPresenceCfg(m map[string]bool) {
	presence.mu.Lock()
	c := &presence.cfg
	for k, v := range m {
		switch k {
		case "rpc":
			c.On = v
		case "rpcMatch":
			c.Match = v
		case "rpcMaps":
			c.Maps = v
		case "rpcLauncher":
			c.Launcher = v
		case "rpcButton":
			c.Button = v
		case "rpcTime":
			c.Time = v
		}
	}
	presence.lastSend = time.Time{} // sofort aktualisieren
	presence.mu.Unlock()
	go presence.tick()
}

func setPresenceTab(t string) {
	presence.mu.Lock()
	presence.tab = t
	presence.lastSend = time.Time{}
	presence.mu.Unlock()
}

// Zustand aus der Oberflaeche: Spiel startet gerade, Anzahl Maps
func setPresenceCtx(starting bool, maps int) {
	presence.mu.Lock()
	if presence.starting != starting || presence.mapCount != maps {
		presence.lastSend = time.Time{}
	}
	presence.starting, presence.mapCount = starting, maps
	presence.mu.Unlock()
}

// ZL im Tray versteckt / wieder sichtbar
func setPresenceHidden(h bool) {
	presence.mu.Lock()
	if presence.hidden != h {
		presence.lastSend = time.Time{}
	}
	presence.hidden = h
	presence.mu.Unlock()
	go presence.tick()
}

func presenceShutdown() {
	presence.mu.Lock()
	defer presence.mu.Unlock()
	if presence.ipc != nil {
		_ = presence.ipc.setActivity(nil)
		presence.ipc.close()
		presence.ipc = nil
	}
}
