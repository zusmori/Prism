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
	 "Game":{"Teams":[{"Name":"Blue","TeamNum":0,"Score":2},{"Name":"Orange","TeamNum":1,"Score":1}],"PlaylistId":10,"TimeSeconds":134.4,"bOvertime":false,"bReplay":false,"bHasWinner":false,"Arena":"EuroStadium_Night_P"}}}`), now)
	cfg := PresenceCfg{On: true, Match: true, Maps: true, Launcher: true, Button: true, Time: true}
	in := PresenceInput{Version: "1.2.6", RLRunning: true, GameStart: now.Add(-time.Minute), Match: &m, MyIDs: []string{"76561198000000001"}}
	a := BuildActivity(cfg, in)
	if a.Details != "Ranked Duel · Mannfield" || a.State != "1 : 2 · 2:14 left" {
		t.Fatalf("got %q / %q", a.Details, a.State)
	}
	// Overtime + Data als String
	m.Apply([]byte(`{"Event":"UpdateState","Data":"{\"MatchGuid\":\"abc\",\"Players\":[],\"Game\":{\"Teams\":[{\"TeamNum\":0,\"Score\":2},{\"TeamNum\":1,\"Score\":2}],\"PlaylistId\":10,\"TimeSeconds\":12,\"bOvertime\":true,\"Arena\":\"Stadium_P\"}}"}`), now)
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m, MyName: "nobody"})
	if a.State != "2 : 2 · Overtime" || !strings.HasPrefix(a.Details, "Ranked Duel · DFH Stadium") {
		t.Fatalf("ot got %q / %q", a.Details, a.State)
	}
	// Ende mit Gewinner
	m.Players = []statsPlayer{{Name: "zusmori", PrimaryID: "Steam|76561198000000001|0", TeamNum: 1}}
	m.Apply([]byte(`{"Event":"MatchEnded","Data":{"WinnerTeamNum":1}}`), now)
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m, MyIDs: []string{"76561198000000001"}})
	if a.State != "Won 2 : 2" {
		t.Fatalf("end got %q", a.State)
	}
	// Workshop-Map
	m2 := MatchState{InMatch: true, Arena: "Park_P", Seen: now}
	a = BuildActivity(cfg, PresenceInput{RLRunning: true, Match: &m2, Swaps: map[string]string{"Park_P.upk": "Speed Jump: Rings 3"}})
	if a.Details != "Workshop map · Speed Jump: Rings 3" {
		t.Fatalf("map got %q", a.Details)
	}
	// Launcher offen, Spiel zu
	a = BuildActivity(cfg, PresenceInput{Tab: "vBrowse", LaunchOpen: now})
	if a.Details != "In Z Launcher" || a.State != "Browsing workshop maps" || a.Buttons[0].Label != "Get Z Launcher" {
		t.Fatalf("launcher got %+v", a)
	}
	cfg.Launcher = false
	if BuildActivity(cfg, PresenceInput{}) != nil {
		t.Fatal("should be nil")
	}
	// Unbekannte Liste -> Teamgroesse
	m3 := MatchState{InMatch: true, Playlist: 999, Seen: now, Players: []statsPlayer{{TeamNum: 0}, {TeamNum: 0}, {TeamNum: 1}, {TeamNum: 1}}}
	if modeName(&m3) != "2v2" {
		t.Fatal(modeName(&m3))
	}
	if arenaPretty("Some_Cool_Map_P") != "Some Cool Map" || arenaPretty("Woods_Night_P") != "Drift Woods" {
		t.Fatal(arenaPretty("Some_Cool_Map_P"), arenaPretty("Woods_Night_P"))
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
	out2, _, ch2 := enableStatsIni(out)
	if ch2 || out2 != out {
		t.Fatal("second run should not change")
	}
	keep := "[TAGame.MatchStatsExporter_TA]\r\nPort=49123\r\nWebPort=49124\r\nPacketSendRate=1\r\n"
	if _, _, c := enableStatsIni(keep); c {
		t.Fatal("rate 1 should stay")
	}
	o3, p3, c3 := enableStatsIni("")
	if !c3 || p3 != 49123 || !strings.HasPrefix(o3, "[TAGame.MatchStatsExporter_TA]") {
		t.Fatal(o3)
	}
	o4, _, c4 := enableStatsIni("[Other]\nA=1\n")
	if !c4 || !strings.Contains(o4, "PacketSendRate=2") || !strings.Contains(o4, "[Other]") {
		t.Fatal(o4)
	}
}
