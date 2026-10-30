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

// Ende der Eigenschaften eines Objekts (nach "None"), -1 = nicht verstanden
func (p *Package) propsEnd(d []byte, start int) int {
	none := p.FindName("None")
	o := start
	for o+8 <= len(d) {
		if int32(binary.LittleEndian.Uint32(d[o:])) == none {
			return o + 8
		}
		if o+24 > len(d) {
			return -1
		}
		typ := p.NameOf(int32(binary.LittleEndian.Uint32(d[o+8:])), 0)
		size := int(int32(binary.LittleEndian.Uint32(d[o+16:])))
		if size < 0 || size > len(d) {
			return -1
		}
		extra := 0
		switch typ {
		case "StructProperty", "ByteProperty":
			extra = 8
		case "BoolProperty":
			extra = 1
		}
		o += 24 + extra + size
	}
	return -1
}

// Groesse eines Meshes (aus der Map selbst; fuer Meshes aus anderen Paketen unbekannt)
type meshBox struct {
	ox, oy, ex, ey float64
	ok             bool
}

func (p *Package) meshBounds(idx int32) meshBox {
	if p.boxes == nil {
		p.boxes = map[int32]meshBox{}
	}
	if b, ok := p.boxes[idx]; ok {
		return b
	}
	var b meshBox
	if idx > 0 && int(idx) <= len(p.Exports) && p.ClassOf(&p.Exports[idx-1]) == "StaticMesh" {
		d := p.Data(&p.Exports[idx-1])
		if end := p.propsEnd(d, 4); end > 0 && end+28 <= len(d) {
			f := func(o int) float64 { return float64(math.Float32frombits(binary.LittleEndian.Uint32(d[end+o:]))) }
			b = meshBox{f(0), f(4), f(12), f(16), true}
			if math.IsNaN(b.ex) || math.IsNaN(b.ey) || b.ex < 0 || b.ey < 0 || b.ex > 1e8 {
				b = meshBox{}
			}
		}
	}
	p.boxes[idx] = b
	return b
}

func (p *Package) floatProp(d []byte, prop string, def float64) float64 {
	if vo, _, ok := p.propValue(d, prop, "FloatProperty"); ok && vo+4 <= len(d) {
		return float64(math.Float32frombits(binary.LittleEndian.Uint32(d[vo:])))
	}
	return def
}

// Gehoert dieses Bauteil (StaticMeshComponent) zur Kulisse?
//   - Mitte des Meshes weiter als das Doppelte des Spielfelds weg
//   - riesig (Gelaende, Stadt, Wasseroberflaeche ...)
//   - ganz ausserhalb des Spielfelds und ein Stueck weg (Baeume, Laternen, Baenke am Rand)
//   - Himmel bleibt immer bei der Arena
func (p *Package) backdropComp(idx int32) bool {
	if idx <= 0 || int(idx) > len(p.Exports) {
		return false
	}
	d := p.Data(&p.Exports[idx-1])
	x, y, _, ok := p.vecProp(d, "Translation")
	if !ok {
		return false
	}
	mesh := ""
	mi := int32(0)
	if vo, _, ok := p.propValue(d, "StaticMesh", "ObjectProperty"); ok && vo+4 <= len(d) {
		mi = int32(binary.LittleEndian.Uint32(d[vo:]))
		mesh = strings.ToLower(p.objPath(mi))
	}
	if strings.Contains(mesh, "sky") {
		return false
	}
	b := p.meshBounds(mi)
	if !b.ok {
		r := farFactor(x, y)
		return r > 2.0 || (strings.Contains(mesh, "oob") && r > 1.2)
	}
	sc := p.floatProp(d, "Scale", 1)
	sx, sy := sc, sc
	if vo, sz, ok := p.propValue(d, "Scale3D", "StructProperty"); ok && sz >= 12 && vo+20 <= len(d) {
		sx *= float64(math.Float32frombits(binary.LittleEndian.Uint32(d[vo+8:])))
		sy *= float64(math.Float32frombits(binary.LittleEndian.Uint32(d[vo+12:])))
	}
	yaw := 0.0
	if vo, sz, ok := p.propValue(d, "Rotation", "StructProperty"); ok && sz >= 12 && vo+20 <= len(d) {
		yaw = float64(int32(binary.LittleEndian.Uint32(d[vo+12:]))) / 65536 * 2 * math.Pi
	}
	ox, oy := b.ox*sx, b.oy*sy
	cx := x + ox*math.Cos(yaw) - oy*math.Sin(yaw)
	cy := y + ox*math.Sin(yaw) + oy*math.Cos(yaw)
	ext := math.Max(math.Abs(b.ex*sx), math.Abs(b.ey*sy))
	ctr := farFactor(cx, cy)
	near := farFactor(math.Max(0, math.Abs(cx)-ext), math.Max(0, math.Abs(cy)-ext))
	switch {
	case ctr > 2.0:
		return true
	case ext > 40000:
		return true
	case ctr > 1.3 && near > 1.05:
		return true
	case strings.Contains(mesh, "oob") && ctr > 1.2:
		return true
	}
	return false
}

// Einzelne Actors, die zur Kulisse gehoeren (Baeume, Fischschwaerme, Tiere, Lichtreflexe, Nebel ...)
func (p *Package) backdropActor(a int32) bool {
	if a <= 0 || int(a) > len(p.Exports) {
		return false
	}
	e := &p.Exports[a-1]
	c := p.ClassOf(e)
	lim := 0.0
	switch c {
	case "ExponentialHeightFog", "HeightFog":
		return true // Stimmung (z. B. Wasser bei AquaDome)
	case "InteractiveFoliageActor":
		lim = 1.15
	case "FlockActor_TA", "SkeletalMeshActor", "SkeletalMeshActorMAT", "StaticLensFlareSource", "LensFlareSource", "Emitter", "SplineActor", "InterpActor":
		lim = 1.5
	default:
		return false
	}
	x, y, _, ok := p.vecProp(p.Data(e), "Location")
	return ok && farFactor(x, y) > lim
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

// Einzelne Kulissen-Actors (Platz in der Actors-Liste)
func (p *Package) looseBackdrop(l *levelLayout) []int {
	var out []int
	for i, a := range p.actors(l) {
		if i > 0 && p.backdropActor(a) {
			out = append(out, i)
		}
	}
	return out
}

// Wie viele Kulissen-Teile stecken direkt in der Map?
func (p *Package) EmbeddedBackdrop() int {
	n, _ := p.Classify()
	return n
}

// Classify: Kulissen-Teile und uebrige Mesh-Teile (z. B. Spielfeld, Boden)
func (p *Package) Classify() (backdrop, rest int) {
	l, err := p.level()
	if err != nil {
		return 0, 0
	}
	for _, c := range p.collections(l) {
		for _, b := range c.backdrop {
			if b {
				backdrop++
			} else {
				rest++
			}
		}
	}
	backdrop += len(p.looseBackdrop(l))
	// Spielfeld-Teile ausserhalb der Sammel-Objekte (Boden, Waende)
	for _, a := range p.actors(l) {
		if a > 0 && int(a) <= len(p.Exports) {
			switch p.ClassOf(&p.Exports[a-1]) {
			case "DecalBlockingActor_TA", "GroundStaticMeshActor_TA", "DynamicMeshActor_TA", "GoalVolume_TA", "VehiclePickup_Boost_TA":
				rest++
			}
		}
	}
	return
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
	// einzelne Kulissen-Actors aus der Actors-Liste nehmen
	if loose := p.looseBackdrop(l); len(loose) > 0 {
		le := &p.Exports[l.idx]
		d := append([]byte(nil), p.Data(le)...)
		for _, i := range loose {
			binary.LittleEndian.PutUint32(d[l.actorsOff+4*i:], 0)
		}
		le.data = d
		n += len(loose)
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
	acts := p.actors(l)
	for _, i := range p.looseBackdrop(l) {
		keepActor[acts[i]] = true
		n++
	}
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
