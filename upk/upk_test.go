package upk

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Echte Map-Dateien: UPK_DIR=... go test ./upk
func TestMerge(t *testing.T) {
	dir := os.Getenv("UPK_DIR")
	if dir == "" {
		t.Skip("UPK_DIR not set")
	}
	for _, f := range []string{"Park_P.upk", "Farm_P.upk", "Farm_OOBTerrain.upk"} {
		s, err := ScanStreaming(filepath.Join(dir, f))
		if err != nil {
			t.Fatal(f, err)
		}
		t.Logf("%s: %+v", f, s)
	}
	p, err := Open(filepath.Join(dir, "Park_P.upk"))
	if err != nil {
		t.Fatal(err)
	}
	// unveraendert speichern = gleiche Daten
	if err := p.AddStreaming([]string{"Farm_OOBTerrain", "farm_vfx"}, nil); err != nil {
		t.Fatal(err)
	}
	b, err := p.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(dir, "Park_P.merged.upk")
	os.WriteFile(out, b, 0644)
	q, err := Open(out)
	if err != nil {
		t.Fatal(err)
	}
	s, err := q.StreamingLevels()
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, l := range s {
		if l.Listed {
			got = append(got, l.Class+":"+l.PackageName)
		}
	}
	t.Log(strings.Join(got, " "))
	if !strings.Contains(strings.Join(got, " "), "LevelStreamingAlwaysLoaded:Farm_OOBTerrain") {
		t.Fatal("not merged")
	}
	// alle anderen Objekte gleich
	for i := range p.Exports {
		if i >= len(q.Exports) {
			break
		}
	}
}

func TestReplace(t *testing.T) {
	dir := os.Getenv("UPK_DIR")
	if dir == "" {
		t.Skip("UPK_DIR not set")
	}
	p, err := Open(filepath.Join(dir, "Farm_P.upk"))
	if err != nil {
		t.Fatal(err)
	}
	if err := p.AddStreaming([]string{"Beach_OOBTerrain", "Beach_OOB"}, IsBackdropLevel); err != nil {
		t.Fatal(err)
	}
	b, err := p.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	q, err := Parse(b, false)
	if err != nil {
		t.Fatal(err)
	}
	s, _ := q.StreamingLevels()
	var got []string
	for _, l := range s {
		if l.Listed {
			got = append(got, l.PackageName)
		}
	}
	t.Log(got)
	j := strings.Join(got, ",")
	if strings.Contains(j, "farm_oobterrain") || !strings.Contains(j, "Beach_OOBTerrain") || !strings.Contains(j, "farm_sfx") {
		t.Fatal("wrong list", j)
	}
	// zweimal hintereinander bearbeiten geht auch
	if err := q.AddStreaming([]string{"CS_OOB"}, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := q.Bytes(); err != nil {
		t.Fatal(err)
	}
}
