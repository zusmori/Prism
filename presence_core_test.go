package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestStatsAndActivity(t *testing.T) {
	now := time.Now()
	var m MatchState
	m.Apply([]byte(`{"Event":"MatchCreated","Data":{"MatchGuid":"abc"}}`), now)
	m.Apply([]byte(`{"Event":"UpdateState","Data":{"MatchGuid":"abc","Players":[{"Name":"zusmori","PrimaryId":"Steam|76561198000000001|0","TeamNum":1},{"Name":"Bob","PrimaryId":"Epic|x|0","TeamNum":0}],
	 "Game":{"Teams":[{"Name":"Blue","TeamNum":0,"Score":2},{"Name":"Orange","TeamNum":1,"Score":1}],"PlaylistId":11,"TimeSeconds":134.4,"bOvertime":false,"bReplay":false,"bHasWinner":false,"Arena":"EuroStadium_Night_P"}}}`), now)
	cfg := PresenceCfg{On: true, Match: true, Maps: true, Launcher: true, Button: true, Time: true}
	in := PresenceInput{Version: "1.2.7", RLRunning: true, GameStart: now.Add(-time.Minute), Match: &m, MyIDs: []string{"76561198000000001"}}
	a := BuildActivity(cfg, in)
	if a.Details != "Rocket League · 2s Ranked" || a.State != "1 : 2" {
		t.Fatalf("got %q / %q", a.Details, a.State)
	}
	// Overtime + Data als String
	m.Apply([]byte(`{"Event":"UpdateState","Data":"{\"MatchGuid\":\"abc\",\"Players\":[],\"Game\":{\"Teams\":[{\"TeamNum\":0,\"Score\":2},{\"TeamNum\":1,\"Score\":2}],\"PlaylistId\":13,\"TimeSeconds\":12,\"bOvertime\":true,\"Arena\":\"Stadium_P\"}}"}`), now)
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m, MyName: "nobody"})
	if a.State != "2 : 2 (OT)" || a.Details != "Rocket League · 3s Ranked" {
		t.Fatalf("ot got %q / %q", a.Details, a.State)
	}
	// Ende mit Gewinner
	m.Players = []statsPlayer{{Name: "zusmori", PrimaryID: "Steam|76561198000000001|0", TeamNum: 1}}
	m.Apply([]byte(`{"Event":"MatchEnded","Data":{"WinnerTeamNum":1}}`), now)
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m, MyIDs: []string{"76561198000000001"}})
	if a.State != "Won 2 : 2" {
		t.Fatalf("end got %q", a.State)
	}
	// Freeplay: kein Spielstand
	fp := MatchState{InMatch: true, Playlist: 9, Arena: "Stadium_P", Seen: now, Teams: []statsTeam{{TeamNum: 0, Score: 3}}}
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &fp})
	if a.Details != "Rocket League · Freeplay" || a.State != "" {
		t.Fatalf("freeplay got %q / %q", a.Details, a.State)
	}
	// Workshop-Map
	m2 := MatchState{InMatch: true, Arena: "Park_P", Seen: now}
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m2, Swaps: map[string]string{"Park_P.upk": "Speed Jump: Rings 3"}})
	if a.Details != "Rocket League · Freeplay" || a.State != "Speed Jump: Rings 3" {
		t.Fatalf("map got %q / %q", a.Details, a.State)
	}
	// keine Matchdaten = Hauptmenue, mit und ohne Anti-Cheat
	if a = BuildActivity(cfg, PresenceInput{RLRunning: true}); a.Details != "Rocket League · Menu" || a.State != "" {
		t.Fatal(a.Details, a.State)
	}
	if a = BuildActivity(cfg, PresenceInput{RLRunning: true, EAC: true}); a.Details != "Rocket League · Menu" {
		t.Fatal(a.Details)
	}
	// Partie vorbei (MatchDestroyed) -> wieder Menue
	gone := MatchState{InMatch: true, Playlist: 11, Seen: now}
	gone.Apply([]byte(`{"Event":"MatchDestroyed","Data":{}}`), now)
	if a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &gone}); a.Details != "Rocket League · Menu" {
		t.Fatal("destroyed", a.Details)
	}
	// Launcher offen, Spiel zu
	a = BuildActivity(cfg, PresenceInput{Tab: "vBrowse", LaunchOpen: now})
	if a.Details != "Browsing workshop maps" || a.Buttons[0].Label != "Get ZLauncher" {
		t.Fatalf("launcher got %+v", a)
	}
	if a = BuildActivity(cfg, PresenceInput{Tab: "vDl", MapCount: 12}); a.State != "12 maps ready to play" {
		t.Fatal(a.State)
	}
	if a = BuildActivity(cfg, PresenceInput{Tab: "vHome"}); a.Details != "Idle" || a.State != "" {
		t.Fatal(a.Details, a.State)
	}
	if a = BuildActivity(cfg, PresenceInput{Tab: "vBrowse", Hidden: true}); a == nil || a.Details != "Idle" || a.State != "" {
		t.Fatal("tray should show Idle")
	}
	if a = BuildActivity(cfg, PresenceInput{Hidden: true, Starting: true}); a.Details != "Starting Rocket League" {
		t.Fatal(a.Details)
	}
	pm := MatchState{InMatch: true, Playlist: 6, Seen: now, Players: []statsPlayer{{TeamNum: 0}, {TeamNum: 0}, {TeamNum: 1}, {TeamNum: 1}}}
	if a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &pm}); a.Details != "Rocket League · 2s" {
		t.Fatal(a.Details)
	}
	cfg.Launcher = false
	if BuildActivity(cfg, PresenceInput{}) != nil {
		t.Fatal("should be nil")
	}
	m3 := MatchState{InMatch: true, Playlist: 999, Seen: now, Players: []statsPlayer{{TeamNum: 0}, {TeamNum: 0}, {TeamNum: 1}, {TeamNum: 1}}}
	if modeName(&m3) != "2s" {
		t.Fatal(modeName(&m3))
	}
	if !strings.Contains(arenaPretty("Woods_Night_P"), "Drift Woods") {
		t.Fatal(arenaPretty("Woods_Night_P"))
	}
}

func TestFrames(t *testing.T) {
	var b bytes.Buffer
	if err := writeFrame(&b, 1, []byte(`{"x":1}`)); err != nil {
		t.Fatal(err)
	}
	op, p, err := readFrame(&b)
	if err != nil || op != 1 || string(p) != `{"x":1}` {
		t.Fatal(op, string(p), err)
	}
}

func TestIni(t *testing.T) {
	in := "[TAGame.MatchStatsExporter_TA]\r\nPort=49123\r\nWebPort=49124\r\nPacketSendRate=0\r\n\r\n[IniVersion]\r\n0=1.0\r\n"
	out, port, ch := enableStatsIni(in)
	if !ch || port != 49123 || !strings.Contains(out, "PacketSendRate=2\r\n") || !strings.Contains(out, "[IniVersion]") {
		t.Fatalf("%v %d %q", ch, port, out)
	}
	if out2, _, ch2 := enableStatsIni(out); ch2 || out2 != out {
		t.Fatal("second run should not change")
	}
	if _, _, c := enableStatsIni("[TAGame.MatchStatsExporter_TA]\r\nPort=49123\r\nPacketSendRate=1\r\n"); c {
		t.Fatal("rate 1 should stay")
	}
	if o3, p3, c3 := enableStatsIni(""); !c3 || p3 != 49123 || !strings.HasPrefix(o3, "[TAGame.MatchStatsExporter_TA]") {
		t.Fatal(o3)
	}
}
