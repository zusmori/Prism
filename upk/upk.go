// Package upk liest und schreibt die Map-Dateien von Rocket League (Unreal Engine 3, .upk).
//
// Aufbau einer RL-Map-Datei:
//   - Kopf (Summary) unverschluesselt bis NameOffset
//   - Namen-, Import- und Export-Tabelle AES-256-ECB-verschluesselt, direkt danach (auch verschluesselt)
//     die Liste der komprimierten Bloecke (Chunks)
//   - etwas Fuellmaterial ("garbage"), dann die Objekt-Daten als zlib-komprimierte Chunks
//
// Fuer den Kulissen-Merge wird nur sehr wenig geaendert: neue Namen, neue Unter-Level-Eintraege
// (LevelStreamingAlwaysLoaded) und die Liste der Unter-Level in WorldInfo. Alles andere bleibt Byte fuer Byte gleich.
package upk

import (
	"bytes"
	"compress/zlib"
	"crypto/aes"
	_ "embed"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// Schluessel, mit dem Rocket League seine Pakete verschluesselt (oeffentlich bekannt, z. B. aus UModel/RLUPKTools)
// Rocket League verschluesselt Map-Kopfdaten mit einem von vielen Schluesseln (Standard zuerst, dann die bekannte Liste)
//
//go:embed keys.txt
var keysTxt string

var keyOnce sync.Once
var keyList [][]byte
var lastKey []byte // zuletzt passender Schluessel (meist derselbe fuer die naechste Datei)
var keyMu sync.Mutex

func allKeys() [][]byte {
	keyOnce.Do(func() {
		seen := map[string]bool{}
		for _, k := range append([]string{"x99rEyUqzHFHu1HJitfjS3/lALd/pfqyk+LyTmsX53k="}, strings.Fields(keysTxt)...) {
			kb, err := base64.StdEncoding.DecodeString(k)
			if err != nil || len(kb) != 32 || seen[string(kb)] {
				continue
			}
			seen[string(kb)] = true
			keyList = append(keyList, kb)
		}
	})
	return keyList
}

const fileTag = 0x9E2A83C1

type Name struct {
	Text  string
	Flags uint64
}

type Import struct {
	ClassPkg, ClassPkgNum, ClassName, ClassNameNum int32
	Outer                                          int32
	Name, NameNum                                  int32
}

type Export struct {
	Class, Super, Outer int32
	Name, NameNum       int32
	Archetype           int32
	ObjectFlags         uint64
	SerialSize          int32
	SerialOffset        int64
	ExportFlags         int32
	NetObjects          []int32
	Guid                [16]byte
	PackageFlags        int32

	data []byte // neue Daten (sonst aus dem Original)
}

type chunk struct {
	uoff  int64
	usize int32
	coff  int64
	csize int32
	extra []byte
}

type Package struct {
	raw []byte // Originaldatei (beim schnellen Lesen nur der Kopf)
	src io.ReaderAt
	img []byte // entschluesselt + entpackt (gleiche Offsets wie im Spiel)
	key []byte

	// Kopf
	totalHeader, nameCount, nameOffset, exportCount, exportOffset, importCount, importOffset, depends                      int32
	offTotal, offNameCount, offExportCount, offExportOffset, offImportOffset, offDepends, offIEGuids, offGenCount, metaOff int
	ieGuids, genCount                                                                                                      int32
	garbage, chunkTable, lastBlock                                                                                         int32
	chunks                                                                                                                 []chunk
	entrySize                                                                                                              int
	blockSize                                                                                                              int32

	Names   []Name
	Imports []Import
	Exports []Export
	plain   bool // unverschluesselt/unkomprimiert (z. B. Workshop-Map)
	newGUID bool // beim Speichern eine neue Paket-GUID vergeben
	boxes   map[int32]meshBox
	keepFog bool // Nebel gehoert zur Kulisse (nur AquaDome)
}

type rd struct {
	b []byte
	p int
	e error
}

func (r *rd) need(n int) bool {
	if r.e != nil {
		return false
	}
	if r.p < 0 || r.p+n > len(r.b) || n < 0 {
		r.e = io.ErrUnexpectedEOF
		return false
	}
	return true
}
func (r *rd) u16() uint16 {
	if !r.need(2) {
		return 0
	}
	v := binary.LittleEndian.Uint16(r.b[r.p:])
	r.p += 2
	return v
}
func (r *rd) i32() int32 {
	if !r.need(4) {
		return 0
	}
	v := int32(binary.LittleEndian.Uint32(r.b[r.p:]))
	r.p += 4
	return v
}
func (r *rd) u64() uint64 {
	if !r.need(8) {
		return 0
	}
	v := binary.LittleEndian.Uint64(r.b[r.p:])
	r.p += 8
	return v
}
func (r *rd) i64() int64 { return int64(r.u64()) }
func (r *rd) skip(n int) {
	if r.need(n) {
		r.p += n
	}
}
func (r *rd) fstring() string {
	n := int(r.i32())
	if n == 0 {
		return ""
	}
	if n < 0 {
		if !r.need(-n * 2) {
			return ""
		}
		s := make([]rune, 0, -n)
		for i := 0; i < -n-1; i++ {
			s = append(s, rune(binary.LittleEndian.Uint16(r.b[r.p+i*2:])))
		}
		r.p += -n * 2
		return string(s)
	}
	if !r.need(n) {
		return ""
	}
	s := string(r.b[r.p : r.p+n-1])
	r.p += n
	return s
}

func putFString(w *bytes.Buffer, s string) {
	ascii := true
	for _, c := range s {
		if c > 0x7e {
			ascii = false
		}
	}
	if ascii {
		binary.Write(w, binary.LittleEndian, int32(len(s)+1))
		w.WriteString(s)
		w.WriteByte(0)
		return
	}
	rs := []rune(s)
	binary.Write(w, binary.LittleEndian, int32(-(len(rs) + 1)))
	for _, c := range rs {
		binary.Write(w, binary.LittleEndian, uint16(c))
	}
	w.Write([]byte{0, 0})
}

func ecb(key, data []byte, enc bool) []byte {
	c, _ := aes.NewCipher(key)
	out := make([]byte, len(data))
	for i := 0; i+16 <= len(data); i += 16 {
		if enc {
			c.Encrypt(out[i:], data[i:])
		} else {
			c.Decrypt(out[i:], data[i:])
		}
	}
	return out
}

// Open liest eine Map-Datei komplett ein (entschluesselt und entpackt).
func Open(path string) (*Package, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return Parse(raw, false)
}

// Parse: headerOnly = nur Kopf und Tabellen (schnell, zum Durchsuchen vieler Maps)
func Parse(raw []byte, headerOnly bool) (*Package, error) { return parse(raw, headerOnly, nil) }

func parse(raw []byte, headerOnly bool, src io.ReaderAt) (*Package, error) {
	p := &Package{raw: raw, src: src}
	r := &rd{b: raw}
	if uint32(r.i32()) != fileTag {
		return nil, errors.New("not an Unreal package")
	}
	r.u16()
	r.u16()
	p.offTotal = r.p
	p.totalHeader = r.i32()
	r.fstring()
	r.i32() // Paket-Flags
	p.offNameCount = r.p
	p.nameCount = r.i32()
	p.nameOffset = r.i32()
	p.offExportCount = r.p
	p.exportCount = r.i32()
	p.offExportOffset = r.p
	p.exportOffset = r.i32()
	p.importCount = r.i32()
	p.offImportOffset = r.p
	p.importOffset = r.i32()
	p.offDepends = r.p
	p.depends = r.i32()
	p.offIEGuids = r.p
	p.ieGuids = r.i32()
	r.i32()
	r.i32()
	r.i32() // Thumbnails
	r.skip(16)
	p.offGenCount = r.p
	p.genCount = r.i32()
	r.skip(int(p.genCount) * 12)
	r.i32()
	r.i32()
	compression := r.i32()
	nc := r.i32()
	r.skip(int(nc) * 16)
	r.i32()
	na := r.i32()
	for i := int32(0); i < na && r.e == nil; i++ {
		r.fstring()
	}
	nt := r.i32()
	for i := int32(0); i < nt && r.e == nil; i++ {
		r.skip(20)
		n := r.i32()
		r.skip(int(n) * 4)
	}
	p.metaOff = r.p
	if r.e != nil {
		return nil, fmt.Errorf("broken header: %v", r.e)
	}
	if compression == 0 && nc == 0 {
		// unverschluesselt und unkomprimiert (Workshop-Map): kein Kulissen-Merge moeglich
		p.plain = true
		if src != nil {
			return nil, errors.New("plain package")
		}
		p.img = raw
		if err := p.parseTables(); err != nil {
			return nil, err
		}
		return p, nil
	}
	p.garbage = r.i32()
	p.chunkTable = r.i32()
	p.lastBlock = r.i32()
	encLen := int(p.totalHeader - p.garbage - p.nameOffset)
	if encLen <= 0 || int(p.nameOffset)+((encLen+15)&^15) > len(raw) {
		return nil, errors.New("unexpected package layout")
	}
	padded := (encLen + 15) &^ 15
	cipher := raw[p.nameOffset : int(p.nameOffset)+padded]
	var plain []byte
	// nur die 32 Bytes um den Anfang der Chunk-Liste pruefen, dann mit dem passenden Schluessel alles entschluesseln
	ct := int(p.chunkTable) &^ 15
	if ct+32 > len(cipher) {
		return nil, errors.New("unexpected package layout")
	}
	probe := cipher[ct : ct+32]
	match := func(kb []byte) bool {
		d := ecb(kb, probe, false)
		o := int(p.chunkTable) - ct
		cnt := int32(binary.LittleEndian.Uint32(d[o:]))
		first := int64(binary.LittleEndian.Uint64(d[o+4:]))
		return cnt >= 1 && first == int64(p.depends)
	}
	keyMu.Lock()
	cands := allKeys()
	if lastKey != nil {
		cands = append([][]byte{lastKey}, cands...)
	}
	keyMu.Unlock()
	for _, kb := range cands {
		if match(kb) {
			plain, p.key = ecb(kb, cipher, false), kb
			keyMu.Lock()
			lastKey = kb
			keyMu.Unlock()
			break
		}
	}
	if plain == nil {
		return nil, errors.New("unknown encryption key")
	}
	// Chunk-Liste (Eintragsgroesse aus dem Platz bestimmen: Rocket League hat inzwischen 36 statt 24 Bytes)
	cr := &rd{b: plain, p: int(p.chunkTable)}
	cnt := int(cr.i32())
	if cnt <= 0 || cnt > 100000 {
		return nil, errors.New("bad chunk table")
	}
	p.entrySize = (encLen - int(p.chunkTable) - 4) / cnt
	if p.entrySize < 24 {
		p.entrySize = 24
	}
	for i := 0; i < cnt; i++ {
		start := cr.p
		c := chunk{uoff: cr.i64(), usize: cr.i32(), coff: cr.i64(), csize: cr.i32()}
		extra := p.entrySize - 24
		if cr.need(extra) {
			c.extra = append([]byte(nil), plain[cr.p:cr.p+extra]...)
			cr.p += extra
		}
		_ = start
		p.chunks = append(p.chunks, c)
	}
	if cr.e != nil {
		return nil, errors.New("bad chunk table")
	}
	last := p.chunks[len(p.chunks)-1]
	size := last.uoff + int64(last.usize)
	if headerOnly {
		size = int64(p.depends)
	}
	img := make([]byte, size)
	copy(img, raw[:p.nameOffset])
	copy(img[p.nameOffset:], plain[:int(p.depends-p.nameOffset)])
	p.img = img
	if !headerOnly {
		for i := range p.chunks {
			if err := p.inflate(i); err != nil {
				return nil, err
			}
		}
	}
	if err := p.parseTables(); err != nil {
		return nil, err
	}
	return p, nil
}

// Einen Chunk entpacken (in img)
func (p *Package) inflate(i int) error {
	c := p.chunks[i]
	if int64(len(p.img)) < c.uoff+int64(c.usize) {
		n := make([]byte, c.uoff+int64(c.usize))
		copy(n, p.img)
		p.img = n
	}
	buf := p.raw
	base := 0
	if int64(len(p.raw)) < c.coff+int64(c.csize) {
		if p.src == nil {
			return errors.New("file cut off")
		}
		buf = make([]byte, c.csize)
		if _, err := p.src.ReadAt(buf, c.coff); err != nil {
			return err
		}
		base = int(c.coff)
	}
	r := &rd{b: buf, p: int(c.coff) - base}
	if uint32(r.i32()) != fileTag {
		return errors.New("bad chunk")
	}
	bs := r.i32()
	if p.blockSize == 0 {
		p.blockSize = bs
	}
	r.i32()
	total := r.i32()
	type blk struct{ c, u int32 }
	var blocks []blk
	for sum := int32(0); sum < total && r.e == nil; {
		b := blk{r.i32(), r.i32()}
		blocks = append(blocks, b)
		sum += b.u
	}
	pos := c.uoff
	for _, b := range blocks {
		if !r.need(int(b.c)) {
			return errors.New("chunk cut off")
		}
		zr, err := zlib.NewReader(bytes.NewReader(buf[r.p : r.p+int(b.c)]))
		if err != nil {
			return err
		}
		n, err := io.ReadFull(zr, p.img[pos:pos+int64(b.u)])
		zr.Close()
		if err != nil || n != int(b.u) {
			return errors.New("chunk data broken")
		}
		r.p += int(b.c)
		pos += int64(b.u)
	}
	return nil
}

func (p *Package) parseTables() error {
	r := &rd{b: p.img, p: int(p.nameOffset)}
	p.Names = make([]Name, 0, p.nameCount)
	for i := int32(0); i < p.nameCount; i++ {
		s := r.fstring()
		p.Names = append(p.Names, Name{s, r.u64()})
	}
	r.p = int(p.importOffset)
	p.Imports = make([]Import, 0, p.importCount)
	for i := int32(0); i < p.importCount; i++ {
		p.Imports = append(p.Imports, Import{r.i32(), r.i32(), r.i32(), r.i32(), r.i32(), r.i32(), r.i32()})
	}
	r.p = int(p.exportOffset)
	p.Exports = make([]Export, 0, p.exportCount)
	for i := int32(0); i < p.exportCount; i++ {
		e := Export{Class: r.i32(), Super: r.i32(), Outer: r.i32(), Name: r.i32(), NameNum: r.i32(), Archetype: r.i32(),
			ObjectFlags: r.u64(), SerialSize: r.i32(), SerialOffset: r.i64(), ExportFlags: r.i32()}
		n := r.i32()
		if n < 0 || n > 1<<20 {
			return errors.New("bad export table")
		}
		for k := int32(0); k < n; k++ {
			e.NetObjects = append(e.NetObjects, r.i32())
		}
		if r.need(16) {
			copy(e.Guid[:], p.img[r.p:])
			r.p += 16
		}
		e.PackageFlags = r.i32()
		p.Exports = append(p.Exports, e)
	}
	if r.e != nil {
		return fmt.Errorf("broken tables: %v", r.e)
	}
	return nil
}

// ---------- Namen/Objekte ----------

func (p *Package) NameOf(i, num int32) string {
	if i < 0 || int(i) >= len(p.Names) {
		return "?"
	}
	s := p.Names[i].Text
	if num > 0 {
		s += fmt.Sprintf("_%d", num-1)
	}
	return s
}

func (p *Package) FindName(s string) int32 {
	for i, n := range p.Names {
		if strings.EqualFold(n.Text, s) {
			return int32(i)
		}
	}
	return -1
}

func (p *Package) ClassOf(e *Export) string {
	switch {
	case e.Class < 0 && int(-e.Class-1) < len(p.Imports):
		im := p.Imports[-e.Class-1]
		return p.NameOf(im.Name, im.NameNum)
	case e.Class > 0 && int(e.Class-1) < len(p.Exports):
		x := p.Exports[e.Class-1]
		return p.NameOf(x.Name, x.NameNum)
	}
	return "Class"
}

// Daten eines Objekts (Original oder bereits geaendert)
func (p *Package) Data(e *Export) []byte {
	if e.data != nil {
		return e.data
	}
	end := e.SerialOffset + int64(e.SerialSize)
	if e.SerialOffset < 0 || end > int64(len(p.img)) {
		return nil
	}
	return p.img[e.SerialOffset:end]
}

// Fuer headerOnly: den Chunk mit diesen Daten nachladen
func (p *Package) ensure(off, size int64) error {
	for i, c := range p.chunks {
		if off+size > c.uoff && off < c.uoff+int64(c.usize) {
			if int64(len(p.img)) >= c.uoff+int64(c.usize) && !allZero(p.img[c.uoff:c.uoff+int64(c.usize)]) {
				continue
			}
			if err := p.inflate(i); err != nil {
				return err
			}
		}
	}
	return nil
}

func allZero(b []byte) bool {
	for _, x := range b {
		if x != 0 {
			return false
		}
	}
	return true
}

// ---------- Unter-Level ----------

type Streaming struct {
	Export      int    // Index in Exports
	Class       string // z. B. LevelStreamingAlwaysLoaded
	PackageName string // z. B. Farm_OOBTerrain
	Listed      bool   // steht in WorldInfo.StreamingLevels
}

// Position eines Property-Werts im Objekt (Suche nach dem Tag "Name ... Typ")
func (p *Package) propValue(data []byte, prop, typ string) (valOff int, size int32, ok bool) {
	ni, ti := p.FindName(prop), p.FindName(typ)
	if ni < 0 || ti < 0 {
		return 0, 0, false
	}
	var pat [16]byte
	binary.LittleEndian.PutUint32(pat[0:], uint32(ni))
	binary.LittleEndian.PutUint32(pat[8:], uint32(ti))
	for start := 0; ; {
		i := bytes.Index(data[start:], pat[:])
		if i < 0 {
			return 0, 0, false
		}
		i += start
		if i+24 <= len(data) {
			size = int32(binary.LittleEndian.Uint32(data[i+16:]))
			if size >= 0 && i+24+int(size) <= len(data) {
				return i + 24, size, true
			}
		}
		start = i + 1
	}
}

func (p *Package) worldInfo() int {
	for i := range p.Exports {
		if p.ClassOf(&p.Exports[i]) == "WorldInfo" {
			return i
		}
	}
	return -1
}

// Liste der Unter-Level in WorldInfo (Export-Indizes, 1-basiert)
func (p *Package) streamingList() (wi int, list []int32, off int, err error) {
	wi = p.worldInfo()
	if wi < 0 {
		return -1, nil, 0, errors.New("no WorldInfo")
	}
	e := &p.Exports[wi]
	if err := p.ensure(e.SerialOffset, int64(e.SerialSize)); err != nil {
		return wi, nil, 0, err
	}
	d := p.Data(e)
	vo, size, ok := p.propValue(d, "StreamingLevels", "ArrayProperty")
	if !ok {
		return wi, nil, -1, nil // keine Unter-Level
	}
	n := int(int32(binary.LittleEndian.Uint32(d[vo:])))
	if n < 0 || 4+4*n != int(size) {
		return wi, nil, 0, errors.New("unexpected StreamingLevels")
	}
	for k := 0; k < n; k++ {
		list = append(list, int32(binary.LittleEndian.Uint32(d[vo+4+4*k:])))
	}
	return wi, list, vo - 24, nil
}

// StreamingLevels: alle Unter-Level dieser Map
func (p *Package) StreamingLevels() ([]Streaming, error) {
	_, listed, _, err := p.streamingList()
	if err != nil {
		return nil, err
	}
	inList := map[int32]bool{}
	for _, x := range listed {
		inList[x] = true
	}
	var out []Streaming
	for i := range p.Exports {
		e := &p.Exports[i]
		c := p.ClassOf(e)
		if !strings.HasPrefix(c, "LevelStreaming") {
			continue
		}
		if err := p.ensure(e.SerialOffset, int64(e.SerialSize)); err != nil {
			return nil, err
		}
		d := p.Data(e)
		vo, _, ok := p.propValue(d, "PackageName", "NameProperty")
		if !ok || vo+8 > len(d) {
			continue
		}
		ni := int32(binary.LittleEndian.Uint32(d[vo:]))
		nn := int32(binary.LittleEndian.Uint32(d[vo+4:]))
		out = append(out, Streaming{Export: i, Class: c, PackageName: p.NameOf(ni, nn), Listed: inList[int32(i+1)]})
	}
	return out, nil
}

// ScanStreaming: schnell nur die Unter-Level einer Map-Datei lesen (Kopf + die paar noetigen Bloecke)
func ScanStreaming(path string) ([]Streaming, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	head := make([]byte, 64<<10)
	n, _ := f.ReadAt(head, 0)
	head = head[:n]
	if n < 12 {
		return nil, errors.New("file too small")
	}
	total := int(int32(binary.LittleEndian.Uint32(head[8:])))
	if total > n {
		if total <= 0 || total > 64<<20 {
			return nil, errors.New("bad header size")
		}
		head = make([]byte, total)
		if _, err := f.ReadAt(head, 0); err != nil {
			return nil, err
		}
	}
	p, err := parse(head, true, f)
	if err != nil {
		return nil, err
	}
	return p.StreamingLevels()
}

// AddStreaming: diese Unter-Level zusaetzlich immer laden. drop: eigene Unter-Level, die nicht mehr geladen werden sollen.
func (p *Package) AddStreaming(add []string, drop func(name string) bool) error {
	if p.plain {
		return errors.New("this arena is a swapped workshop map")
	}
	levels, err := p.StreamingLevels()
	if err != nil {
		return err
	}
	wi, list, tagOff, err := p.streamingList()
	if err != nil {
		return err
	}
	if tagOff < 0 {
		return errors.New("this arena has no sub-level list")
	}
	// Vorlage: ein vorhandener LevelStreamingAlwaysLoaded-Eintrag
	tpl := -1
	for _, l := range levels {
		if l.Class == "LevelStreamingAlwaysLoaded" {
			tpl = l.Export
			break
		}
	}
	if tpl < 0 && len(add) > 0 {
		return errors.New("this arena has no always-loaded sub-level to copy")
	}
	if tpl < 0 {
		if len(levels) == 0 {
			return errors.New("this arena has no sub-levels")
		}
		tpl = levels[0].Export
	}
	tplE := p.Exports[tpl]
	tplData := append([]byte(nil), p.Data(&tplE)...)
	pnOff, _, ok := p.propValue(tplData, "PackageName", "NameProperty")
	if !ok {
		return errors.New("sub-level entry not understood")
	}
	tplNameIdx := int32(binary.LittleEndian.Uint32(tplData[pnOff:]))
	nameFlags := p.Names[tplNameIdx].Flags

	// Liste neu: eigene behalten (ausser drop), neue anhaengen
	have := map[string]bool{}
	var newList []int32
	for _, x := range list {
		name := ""
		for _, l := range levels {
			if int32(l.Export+1) == x {
				name = l.PackageName
			}
		}
		if name != "" && drop != nil && drop(name) {
			continue
		}
		newList = append(newList, x)
		have[strings.ToLower(name)] = true
	}
	maxNum := int32(0)
	for _, e := range p.Exports {
		if e.Name == tplE.Name && e.NameNum > maxNum {
			maxNum = e.NameNum
		}
	}
	for _, sub := range add {
		if have[strings.ToLower(sub)] {
			continue
		}
		have[strings.ToLower(sub)] = true
		ni := p.FindName(sub)
		if ni < 0 {
			p.Names = append(p.Names, Name{sub, nameFlags})
			ni = int32(len(p.Names) - 1)
		}
		d := append([]byte(nil), tplData...)
		binary.LittleEndian.PutUint32(d[0:], 0xFFFFFFFF) // NetIndex: keiner
		binary.LittleEndian.PutUint32(d[pnOff:], uint32(ni))
		binary.LittleEndian.PutUint32(d[pnOff+4:], 0)
		maxNum++
		ne := tplE
		ne.NameNum = maxNum
		ne.NetObjects = append([]int32(nil), tplE.NetObjects...)
		ne.data = d
		ne.SerialSize = int32(len(d))
		p.Exports = append(p.Exports, ne)
		newList = append(newList, int32(len(p.Exports)))
	}
	// WorldInfo mit neuer Liste
	wiE := &p.Exports[wi]
	old := p.Data(wiE)
	oldSize := int32(binary.LittleEndian.Uint32(old[tagOff+16:]))
	var nb bytes.Buffer
	nb.Write(old[:tagOff+16])
	binary.Write(&nb, binary.LittleEndian, int32(4+4*len(newList)))
	nb.Write(old[tagOff+20 : tagOff+24]) // ArrayIndex
	binary.Write(&nb, binary.LittleEndian, int32(len(newList)))
	for _, x := range newList {
		binary.Write(&nb, binary.LittleEndian, x)
	}
	nb.Write(old[tagOff+24+int(oldSize):])
	wiE.data = nb.Bytes()
	wiE.SerialSize = int32(nb.Len())
	return nil
}

// ---------- Speichern ----------

func (p *Package) tables() (names, imports, exports []byte) {
	var b bytes.Buffer
	for _, n := range p.Names {
		putFString(&b, n.Text)
		binary.Write(&b, binary.LittleEndian, n.Flags)
	}
	names = append([]byte(nil), b.Bytes()...)
	b.Reset()
	for _, im := range p.Imports {
		binary.Write(&b, binary.LittleEndian, im)
	}
	imports = append([]byte(nil), b.Bytes()...)
	b.Reset()
	for _, e := range p.Exports {
		for _, v := range []any{e.Class, e.Super, e.Outer, e.Name, e.NameNum, e.Archetype, e.ObjectFlags, e.SerialSize, e.SerialOffset, e.ExportFlags, int32(len(e.NetObjects))} {
			binary.Write(&b, binary.LittleEndian, v)
		}
		for _, x := range e.NetObjects {
			binary.Write(&b, binary.LittleEndian, x)
		}
		b.Write(e.Guid[:])
		binary.Write(&b, binary.LittleEndian, e.PackageFlags)
	}
	exports = append([]byte(nil), b.Bytes()...)
	return
}

func put32(b []byte, off int, v int32) { binary.LittleEndian.PutUint32(b[off:], uint32(v)) }

// Bytes baut die Datei neu (gleiche Verschluesselung und Komprimierung wie das Original).
func (p *Package) Bytes() ([]byte, error) {
	if p.plain {
		return nil, errors.New("plain packages are not supported")
	}
	origDepends := int64(p.depends)
	// Groesse der Tabellen (Offsets stehen darin, aendern aber die Groesse nicht)
	n0, i0, e0 := p.tables()
	newImportOff := int64(p.nameOffset) + int64(len(n0))
	newExportOff := newImportOff + int64(len(i0))
	newDepends := newExportOff + int64(len(e0))
	delta := newDepends - origDepends

	// Objekt-Daten: Original-Bereich verschoben, geaenderte/neue Objekte hinten an
	body := append([]byte(nil), p.img[origDepends:]...)
	end := newDepends + int64(len(body))
	for i := range p.Exports {
		e := &p.Exports[i]
		if e.data != nil {
			e.SerialOffset = end
			e.SerialSize = int32(len(e.data))
			body = append(body, e.data...)
			end += int64(len(e.data))
			e.data = nil
			continue
		}
		if e.SerialOffset >= origDepends {
			e.SerialOffset += delta
		}
	}
	names, imports, exports := p.tables()
	if int64(len(names)+len(imports)+len(exports)) != newDepends-int64(p.nameOffset) {
		return nil, errors.New("table size changed")
	}
	// neues Bild fuer die naechste Bearbeitung
	img := make([]byte, 0, int(newDepends)+len(body))
	img = append(img, p.img[:p.nameOffset]...)
	img = append(img, names...)
	img = append(img, imports...)
	img = append(img, exports...)
	img = append(img, body...)

	// Chunks: gleiche Grenzen wie im Original (verschoben), der letzte geht bis zum Ende
	type out struct {
		c       chunk
		payload []byte
	}
	var outs []out
	bs := p.blockSize
	if bs <= 0 {
		bs = 0x20000
	}
	for i, c := range p.chunks {
		start := c.uoff + delta
		stop := int64(len(img))
		if i+1 < len(p.chunks) {
			stop = p.chunks[i+1].uoff + delta
		}
		if start < newDepends || stop < start || stop > int64(len(img)) {
			return nil, errors.New("bad chunk bounds")
		}
		pl, err := compressChunk(img[start:stop], bs)
		if err != nil {
			return nil, err
		}
		outs = append(outs, out{chunk{uoff: start, usize: int32(stop - start), extra: c.extra}, pl})
	}
	// verschluesselter Teil: Tabellen + Chunk-Liste
	tableOff := newDepends - int64(p.nameOffset)
	encLen := int(tableOff) + 4 + len(outs)*p.entrySize
	padded := (encLen + 15) &^ 15
	totalHeader := int64(p.nameOffset) + int64(encLen) + int64(p.garbage)
	coff := totalHeader
	for i := range outs {
		outs[i].c.coff = coff
		outs[i].c.csize = int32(len(outs[i].payload))
		coff += int64(len(outs[i].payload))
	}
	plain := make([]byte, padded)
	copy(plain, img[p.nameOffset:newDepends])
	tw := bytes.NewBuffer(nil)
	binary.Write(tw, binary.LittleEndian, int32(len(outs)))
	for _, o := range outs {
		binary.Write(tw, binary.LittleEndian, o.c.uoff)
		binary.Write(tw, binary.LittleEndian, o.c.usize)
		binary.Write(tw, binary.LittleEndian, o.c.coff)
		binary.Write(tw, binary.LittleEndian, o.c.csize)
		ex := make([]byte, p.entrySize-24)
		copy(ex, o.c.extra)
		tw.Write(ex)
	}
	copy(plain[tableOff:], tw.Bytes())
	// Rest des letzten Blocks wie im Original (Fuellbytes) statt Nullen
	cipher := ecb(p.key, plain, true)

	// Kopf anpassen
	prefix := append([]byte(nil), p.raw[:p.nameOffset]...)
	put32(prefix, p.offTotal, int32(totalHeader))
	put32(prefix, p.offNameCount, int32(len(p.Names)))
	put32(prefix, p.offExportCount, int32(len(p.Exports)))
	put32(prefix, p.offExportOffset, int32(newExportOff))
	put32(prefix, p.offImportOffset, int32(newImportOff))
	put32(prefix, p.offDepends, int32(newDepends))
	if p.ieGuids == p.depends {
		put32(prefix, p.offIEGuids, int32(newDepends))
	}
	if p.genCount > 0 {
		g := p.offGenCount + 4 + int(p.genCount-1)*12
		put32(prefix, g, int32(len(p.Exports)))
		put32(prefix, g+4, int32(len(p.Names)))
	}
	put32(prefix, p.metaOff+4, int32(tableOff))
	if p.newGUID {
		copy(prefix[p.offGenCount-16:p.offGenCount], randomGUID())
	}

	// Fuellbereich aus dem Original (beginnt hinter dem unaufgefuellten verschluesselten Teil)
	origEnc := int(p.totalHeader - p.garbage - p.nameOffset)
	gStart := int(p.nameOffset) + origEnc
	garbage := p.raw[gStart : gStart+int(p.garbage)]

	var w bytes.Buffer
	w.Write(prefix)
	w.Write(cipher)
	pad := padded - encLen
	if pad > len(garbage) {
		return nil, errors.New("garbage too small")
	}
	w.Write(garbage[pad:])
	if int64(w.Len()) != totalHeader {
		return nil, errors.New("header size mismatch")
	}
	for _, o := range outs {
		w.Write(o.payload)
	}
	// Stand uebernehmen
	p.img = img
	p.depends = int32(newDepends)
	return w.Bytes(), nil
}

func compressChunk(data []byte, bs int32) ([]byte, error) {
	var blocks [][]byte
	var sizes []int32
	total := 0
	for i := 0; i < len(data); i += int(bs) {
		j := i + int(bs)
		if j > len(data) {
			j = len(data)
		}
		var b bytes.Buffer
		zw, _ := zlib.NewWriterLevel(&b, 6)
		zw.Write(data[i:j])
		zw.Close()
		blocks = append(blocks, b.Bytes())
		sizes = append(sizes, int32(j-i))
		total += b.Len()
	}
	var w bytes.Buffer
	binary.Write(&w, binary.LittleEndian, uint32(fileTag))
	binary.Write(&w, binary.LittleEndian, bs)
	binary.Write(&w, binary.LittleEndian, int32(total))
	binary.Write(&w, binary.LittleEndian, int32(len(data)))
	for k, b := range blocks {
		binary.Write(&w, binary.LittleEndian, int32(len(b)))
		binary.Write(&w, binary.LittleEndian, sizes[k])
	}
	for _, b := range blocks {
		w.Write(b)
	}
	return w.Bytes(), nil
}

// ---------- Kulissen ----------

// Kulisse einer Map: ihre immer geladenen Unter-Level ohne Ton (Gelaende, Berge, Himmel, Effekte)
func Backdrop(levels []Streaming) []string {
	var out []string
	for _, l := range levels {
		n := strings.ToLower(l.PackageName)
		if !l.Listed || l.Class != "LevelStreamingAlwaysLoaded" {
			continue
		}
		// nur, was ausserhalb des Spielfelds liegt (OOB = "out of bounds"): kein Stadion, Licht, Spielfeld oder Ton
		if !IsBackdropLevel(n) || strings.Contains(n, "sfx") || strings.Contains(n, "audio") {
			continue
		}
		out = append(out, l.PackageName)
	}
	return out
}

// Gehoert dieses Unter-Level zur Kulisse? (fuer "eigene Kulisse entfernen")
func IsBackdropLevel(name string) bool {
	n := strings.ToLower(name)
	for _, k := range []string{"oob", "terrain", "mountain", "sky"} {
		if strings.Contains(n, k) {
			return true
		}
	}
	return false
}

// MergeInto: diese Unter-Level in der Arena zusaetzlich laden, die Unter-Level in drop nicht mehr.
// replace: die Kulissen-Teile, die direkt in der Arena-Map stecken, abschalten.
func MergeInto(basePath string, add, drop []string, replace bool) ([]byte, error) {
	p, err := Open(basePath)
	if err != nil {
		return nil, fmt.Errorf("arena: %v", err)
	}
	dropSet := map[string]bool{}
	for _, d := range drop {
		dropSet[strings.ToLower(d)] = true
	}
	removed := 0
	if replace {
		removed, _ = p.RemoveEmbeddedBackdrop()
	}
	dropFn := func(n string) bool { return dropSet[strings.ToLower(n)] }
	if err := p.AddStreaming(add, dropFn); err != nil && (len(add) > 0 || len(drop) > 0 || removed == 0) {
		return nil, err
	}
	return p.Bytes()
}

// BuildNoBackdropPackage: Kopie eines Unter-Levels ohne seine Kulissen-Teile (Boden, Spielfeld usw. bleiben)
func BuildNoBackdropPackage(srcPath string) ([]byte, error) {
	p, err := Open(srcPath)
	if err != nil {
		return nil, err
	}
	if _, err := p.RemoveEmbeddedBackdrop(); err != nil {
		return nil, err
	}
	p.newGUID = true
	p.key = allKeys()[0] // neuer Paketname: das Spiel nimmt den Standard-Schluessel
	return p.Bytes()
}

// ClassifyFile: Kulissen-Teile und uebrige Teile einer Map-Datei
func ClassifyFile(path string) (backdrop, rest int, err error) {
	p, err := Open(path)
	if err != nil {
		return 0, 0, err
	}
	b, r := p.Classify()
	return b, r, nil
}

// BuildBackdropPackage: Kopie der Map nur mit ihrer eingebauten Kulisse (als Unter-Level fuer andere Arenen)
func BuildBackdropPackage(srcPath string) ([]byte, int, error) {
	p, err := Open(srcPath)
	if err != nil {
		return nil, 0, err
	}
	p.keepFog = strings.Contains(strings.ToLower(filepath.Base(srcPath)), "underwater")
	n, err := p.MakeBackdropPackage()
	if err != nil {
		return nil, 0, err
	}
	// Rocket League waehlt den Schluessel nach dem Paketnamen; fuer unbekannte Namen (unser neues Paket) nimmt es
	// den Standard-Schluessel. Mit dem Schluessel der Quell-Map waere das Paket fuer das Spiel kaputt (Absturz).
	p.key = allKeys()[0]
	b, err := p.Bytes()
	return b, n, err
}

// EmbeddedCount: wie viele Kulissen-Teile direkt in der Map stecken (liest die ganze Datei)
func EmbeddedCount(path string) (int, error) {
	p, err := Open(path)
	if err != nil {
		return 0, err
	}
	return p.EmbeddedBackdrop(), nil
}
