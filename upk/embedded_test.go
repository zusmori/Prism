package upk

import (
	"os"
	"path/filepath"
	"testing"
)

func TestEmbedded(t *testing.T) {
	dir := os.Getenv("UPK_DIR")
	if dir == "" {
		t.Skip("UPK_DIR not set")
	}
	for _, f := range []string{"Park_P.upk", "Farm_P.upk"} {
		p, err := Open(filepath.Join(dir, f))
		if err != nil {
			t.Fatal(err)
		}
		l, err := p.level()
		if err != nil {
			t.Fatal(f, err)
		}
		t.Logf("%s: actors %d seq %d embedded backdrop %d", f, l.nActors, l.nSeq, p.EmbeddedBackdrop())
	}
	p, _ := Open(filepath.Join(dir, "Park_P.upk"))
	n, err := p.MakeBackdropPackage()
	if err != nil {
		t.Fatal(err)
	}
	b, err := p.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(dir, "Prism_Park_P_BD.upk"), b, 0644)
	q, err := Parse(b, false)
	if err != nil {
		t.Fatal(err)
	}
	l, err := q.level()
	if err != nil {
		t.Fatal(err)
	}
	kept := 0
	for _, a := range q.actors(l) {
		if a != 0 {
			kept++
		}
	}
	t.Logf("backdrop package: %d parts, %d actors left, seq %d, backdrop now %d", n, kept, l.nSeq, q.EmbeddedBackdrop())
	// Arena ohne eigene Kulisse
	r, _ := Open(filepath.Join(dir, "Park_P.upk"))
	m, err := r.RemoveEmbeddedBackdrop()
	if err != nil {
		t.Fatal(err)
	}
	rb, err := r.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	r2, _ := Parse(rb, false)
	t.Logf("removed %d, left %d", m, r2.EmbeddedBackdrop())
	if r2.EmbeddedBackdrop() != 0 {
		t.Fatal("still there")
	}
}
