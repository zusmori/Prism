package upk

// Kulissen, die direkt in der Haupt-Map stecken (z. B. Beckwith Park, DFH Stadium): dort gibt es kein
// eigenes OOB-Unter-Level. Die Berge, Hochhaeuser, Waelder usw. sind Teile ("Components") der
// StaticMeshCollectionActor-Objekte, mit ihrer Position im Level. Alles, was weit genug vom Spielfeld weg ist,
// gilt als Kulisse.
//   - eigene Kulisse abschalten: diese Teile aus den Sammel-Objekten austragen (werden dann nicht gezeichnet)
//   - als Kulisse fuer andere Arenen: eine Kopie der Map, in der NUR diese Teile uebrig sind (kein Spielfeld,
//     keine Tore, keine Boost-Pads, keine Skripte), wird als zusaetzliches Unter-Level geladen.

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"math"
	"strings"
)

// Spielfeld: 8192 x 10240 Einheiten (plus Tore). Weiter als das Doppelte weg = Kulisse.
const fieldX, fieldY = 4096.0, 5120.0

func (p *Package) objPath(idx int32) string {
	switch {
	case idx > 0 && int(idx) <= len(p.Exports):
		e := p.Exports[idx-1]
		return p.NameOf(e.Name, e.NameNum)
	case idx < 0 && int(-idx) <= len(p.Imports):
		im := p.Imports[-idx-1]
		return p.NameOf(im.Name, im.NameNum)
	}
	return ""
}

func (p *Package) vecProp(d []byte, prop string) (x, y, z float64, ok bool) {
	vo, size, ok := p.propValue(d, prop, "StructProperty")
	if !ok || size < 12 || vo+8+12 > len(d) {
		return 0, 0, 0, false
	}
	f := func(o int) float64 { return float64(math.Float32frombits(binary.LittleEndian.Uint32(d[o:]))) }
	return f(vo + 8), f(vo + 12), f(vo + 16), true
}

func farFactor(x, y float64) float64 { return math.Max(math.Abs(x)/fieldX, math.Abs(y)/fieldY) }

// Gehoert dieses Bauteil (StaticMeshComponent) zur Kulisse?
func (p *Package) backdropComp(idx int32) bool {
	if idx <= 0 || int(idx) > len(p.Exports) {
		return false
	}
	e := &p.Exports[idx-1]
	d := p.Data(e)
	x, y, _, ok := p.vecProp(d, "Translation")
	if !ok {
		return false
	}
	mesh := ""
	if vo, _, ok := p.propValue(d, "StaticMesh", "ObjectProperty"); ok && vo+4 <= len(d) {
		mesh = strings.ToLower(p.objPath(int32(binary.LittleEndian.Uint32(d[vo:]))))
	}
	if strings.Contains(mesh, "sky") {
		return false // Himmel bleibt immer bei der Arena
	}
	r := farFactor(x, y)
	return r > 2.0 || (strings.Contains(mesh, "oob") && r > 1.2)
}

// Aufbau des Level-Objekts: Actors-Liste und GameSequences (Skripte)
type levelLayout struct {
	idx                int // Export-Index (0-basiert)
	actorsOff, nActors int // Offset der Liste (nach der Anzahl)
	seqCountOff, nSeq  int
}

func (p *Package) level() (*levelLayout, error) {
	li := -1
	for i := range p.Exports {
		e := &p.Exports[i]
		if p.ClassOf(e) == "Level" && strings.EqualFold(p.NameOf(e.Name, e.NameNum), "PersistentLevel") {
			li = i
			break
		}
	}
	if li < 0 {
		return nil, errors.New("no level")
	}
	d := p.Data(&p.Exports[li])
	none := p.FindName("None")
	if none < 0 || len(d) < 16 {
		return nil, errors.New("level not understood")
	}
	// Eigenschaften bis "None" ueberspringen: das erste None-Tag nach dem NetIndex
	var pat [8]byte
	binary.LittleEndian.PutUint32(pat[:], uint32(none))
	j := bytes.Index(d[4:], pat[:])
	if j < 0 {
		return nil, errors.New("level not understood")
	}
	r := &rd{b: d, p: 4 + j + 8}
	r.i32() // Besitzer der Actors-Liste (das Level selbst)
	l := &levelLayout{idx: li}
	l.nActors = int(r.i32())
	l.actorsOff = r.p
	if l.nActors < 1 || l.nActors > 200000 {
		return nil, errors.New("level actors not understood")
	}
	r.skip(4 * l.nActors)
	// URL: Protocol, Host, Map, Portal, Op[], Port, Valid
	for i := 0; i < 4; i++ {
		r.fstring()
	}
	nOp := int(r.i32())
	for i := 0; i < nOp && r.e == nil; i++ {
		r.fstring()
	}
	r.i32()
	r.i32()
	r.i32() // Model
	nMC := int(r.i32())
	r.skip(4 * nMC)
	l.seqCountOff = r.p
	l.nSeq = int(r.i32())
	if r.e != nil || l.nSeq < 0 || l.nSeq > 64 {
		return nil, errors.New("level layout not understood")
	}
	return l, nil
}

func (p *Package) actors(l *levelLayout) []int32 {
	d := p.Data(&p.Exports[l.idx])
	out := make([]int32, l.nActors)
	for i := range out {
		out[i] = int32(binary.LittleEndian.Uint32(d[l.actorsOff+4*i:]))
	}
	return out
}

// Sammel-Objekte und ihre Kulissen-Teile
type collection struct {
	actor        int32 // Export (1-basiert)
	comps        []int32
	backdrop     []bool
	arrOff, size int // Lage des StaticMeshComponents-Werts im Objekt
}

func (p *Package) collections(l *levelLayout) []collection {
	var out []collection
	for _, a := range p.actors(l) {
		if a <= 0 || int(a) > len(p.Exports) {
			continue
		}
		e := &p.Exports[a-1]
		if p.ClassOf(e) != "StaticMeshCollectionActor" {
			continue
		}
		d := p.Data(e)
		vo, size, ok := p.propValue(d, "StaticMeshComponents", "ArrayProperty")
		if !ok {
			continue
		}
		n := int(int32(binary.LittleEndian.Uint32(d[vo:])))
		if n < 0 || 4+4*n != int(size) {
			continue
		}
		c := collection{actor: a, arrOff: vo, size: int(size)}
		for k := 0; k < n; k++ {
			ci := int32(binary.LittleEndian.Uint32(d[vo+4+4*k:]))
			c.comps = append(c.comps, ci)
			c.backdrop = append(c.backdrop, p.backdropComp(ci))
		}
		out = append(out, c)
	}
	return out
}

// Wie viele Kulissen-Teile stecken direkt in der Map?
func (p *Package) EmbeddedBackdrop() int {
	l, err := p.level()
	if err != nil {
		return 0
	}
	n := 0
	for _, c := range p.collections(l) {
		for _, b := range c.backdrop {
			if b {
				n++
			}
		}
	}
	return n
}

// Liste in einem Sammel-Objekt neu schreiben (nur Teile, fuer die keep true ist)
func (p *Package) rewriteCollection(c collection, keep func(i int) bool) {
	e := &p.Exports[c.actor-1]
	d := p.Data(e)
	var kept []int32
	for i, ci := range c.comps {
		if keep(i) {
			kept = append(kept, ci)
		}
	}
	tag := c.arrOff - 24
	var b bytes.Buffer
	b.Write(d[:tag+16])
	binary.Write(&b, binary.LittleEndian, int32(4+4*len(kept)))
	b.Write(d[tag+20 : tag+24])
	binary.Write(&b, binary.LittleEndian, int32(len(kept)))
	for _, k := range kept {
		binary.Write(&b, binary.LittleEndian, k)
	}
	b.Write(d[c.arrOff+c.size:])
	e.data = b.Bytes()
	e.SerialSize = int32(b.Len())
}

// RemoveEmbeddedBackdrop: die Kulisse, die in der Map selbst steckt, nicht mehr zeichnen
func (p *Package) RemoveEmbeddedBackdrop() (int, error) {
	l, err := p.level()
	if err != nil {
		return 0, err
	}
	n := 0
	for _, c := range p.collections(l) {
		any := false
		for _, b := range c.backdrop {
			if b {
				any = true
				n++
			}
		}
		if any {
			p.rewriteCollection(c, func(i int) bool { return !c.backdrop[i] })
		}
	}
	return n, nil
}

// MakeBackdropPackage: Kopie dieser Map, in der nur noch die Kulisse steckt (als Unter-Level fuer andere Arenen)
func (p *Package) MakeBackdropPackage() (int, error) {
	l, err := p.level()
	if err != nil {
		return 0, err
	}
	cols := p.collections(l)
	keepActor := map[int32]bool{}
	n := 0
	for _, c := range cols {
		k := 0
		for _, b := range c.backdrop {
			if b {
				k++
			}
		}
		if k == 0 {
			continue
		}
		n += k
		keepActor[c.actor] = true
		cc := c
		p.rewriteCollection(cc, func(i int) bool { return cc.backdrop[i] })
	}
	if n == 0 {
		return 0, errors.New("no backdrop in this map")
	}
	// Level: nur WorldInfo (immer an erster Stelle) und die Sammel-Objekte mit Kulisse; keine Skripte
	le := &p.Exports[l.idx]
	d := append([]byte(nil), p.Data(le)...)
	for i := 0; i < l.nActors; i++ {
		a := int32(binary.LittleEndian.Uint32(d[l.actorsOff+4*i:]))
		if i == 0 || keepActor[a] {
			continue
		}
		binary.LittleEndian.PutUint32(d[l.actorsOff+4*i:], 0)
	}
	var b bytes.Buffer
	b.Write(d[:l.seqCountOff])
	binary.Write(&b, binary.LittleEndian, int32(0))
	b.Write(d[l.seqCountOff+4+4*l.nSeq:])
	le.data = b.Bytes()
	le.SerialSize = int32(b.Len())
	// eigene Unter-Level (Ton usw.) nicht mitnehmen
	if wi, list, tagOff, err := p.streamingList(); err == nil && tagOff >= 0 && len(list) > 0 {
		we := &p.Exports[wi]
		wd := p.Data(we)
		oldSize := int32(binary.LittleEndian.Uint32(wd[tagOff+16:]))
		var nb bytes.Buffer
		nb.Write(wd[:tagOff+16])
		binary.Write(&nb, binary.LittleEndian, int32(4))
		nb.Write(wd[tagOff+20 : tagOff+24])
		binary.Write(&nb, binary.LittleEndian, int32(0))
		nb.Write(wd[tagOff+24+int(oldSize):])
		we.data = nb.Bytes()
		we.SerialSize = int32(nb.Len())
	}
	// neue Paket-GUID (zwei Pakete mit gleicher GUID koennten sich in die Quere kommen)
	p.newGUID = true
	return n, nil
}

func randomGUID() []byte {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return b
}
