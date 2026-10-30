package upk

import (
	"fmt"
	"math"
	"os"
	"testing"
)

func TestTrail(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	p, _ := Open(f)
	l, _ := p.level()
	for _, c := range p.collections(l) {
		d := p.Data(&p.Exports[c.actor-1])
		fmt.Printf("coll %d comps=%d datalen=%d arrEnd=%d trailing=%d per=%.2f\n", c.actor, len(c.comps), len(d), c.arrOff+c.size, len(d)-(c.arrOff+c.size), float64(len(d)-(c.arrOff+c.size))/float64(len(c.comps)))
	}
}

func (p *Package) listProps(d []byte) []string {
	var out []string
	o := 4
	for o+24 <= len(d) {
		n := int32(binary_u32(d[o:]))
		name := p.NameOf(n, 0)
		if name == "None" {
			out = append(out, fmt.Sprintf("None@%d", o))
			break
		}
		typ := p.NameOf(int32(binary_u32(d[o+8:])), 0)
		size := int(int32(binary_u32(d[o+16:])))
		extra := 0
		switch typ {
		case "StructProperty", "ByteProperty":
			extra = 8
		case "BoolProperty":
			extra = 1
		}
		out = append(out, fmt.Sprintf("%s:%s(%d)", name, typ, size))
		o += 24 + extra + size
	}
	return out
}

func binary_u32(b []byte) uint32 {
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func TestProps(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	want := os.Getenv("UPK_CLASS")
	p, _ := Open(f)
	l, _ := p.level()
	n := 0
	for _, a := range p.actors(l) {
		if a <= 0 {
			continue
		}
		e := &p.Exports[a-1]
		if p.ClassOf(e) != want {
			continue
		}
		n++
		if n > 3 {
			break
		}
		fmt.Println(p.NameOf(e.Name, e.NameNum), len(p.Data(e)), p.listProps(p.Data(e)))
	}
}

// Nicht-Sammel-Actors und ihre Mesh-Bauteile
func TestActors(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	p, _ := Open(f)
	l, _ := p.level()
	inLevel := map[int32]bool{}
	for _, a := range p.actors(l) {
		inLevel[a] = true
	}
	for i := range p.Exports {
		e := &p.Exports[i]
		c := p.ClassOf(e)
		if c != "StaticMeshComponent" && c != "InstancedStaticMeshComponent" && c != "SkeletalMeshComponent" && c != "InteractiveFoliageComponent" && c != "InstancedFoliageComponent" {
			continue
		}
		if e.Outer <= 0 {
			continue
		}
		oe := &p.Exports[e.Outer-1]
		oc := p.ClassOf(oe)
		if oc == "StaticMeshCollectionActor" {
			continue
		}
		d := p.Data(e)
		mesh := ""
		for _, mp := range []string{"StaticMesh", "SkeletalMesh"} {
			if vo, _, ok := p.propValue(d, mp, "ObjectProperty"); ok {
				mesh = p.objPath(int32(binary_u32(d[vo:])))
			}
		}
		x, y, z, ok := p.vecProp(d, "Translation")
		ad := p.Data(oe)
		ax, ay, az, aok := p.vecProp(ad, "Location")
		fmt.Printf("%-28s %-26s %-30s comp=%v(%.0f %.0f %.0f) actor=%v(%.0f %.0f %.0f) r=%.2f lvl=%v\n", oc, p.NameOf(oe.Name, oe.NameNum), mesh, ok, x, y, z, aok, ax, ay, az, farFactor(ax+x, ay+y), inLevel[e.Outer])
	}
}

type mbounds struct {
	ox, oy, oz, ex, ey, ez, r float64
	ok                        bool
}

func (p *Package) tMeshBounds(idx int32, cache map[int32]mbounds) mbounds {
	if b, ok := cache[idx]; ok {
		return b
	}
	var b mbounds
	if idx > 0 && int(idx) <= len(p.Exports) {
		d := p.Data(&p.Exports[idx-1])
		end := p.propsEnd(d, 4)
		if end > 0 && end+28 <= len(d) {
			fl := func(o int) float64 { return float64(math.Float32frombits(binary_u32(d[end+o:]))) }
			b = mbounds{fl(0), fl(4), fl(8), fl(12), fl(16), fl(20), fl(24), true}
		}
	}
	cache[idx] = b
	return b
}

func TestCompBounds(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	p, _ := Open(f)
	l, _ := p.level()
	cache := map[int32]mbounds{}
	for _, c := range p.collections(l) {
		for i, ci := range c.comps {
			d := p.Data(&p.Exports[ci-1])
			x, y, _, _ := p.vecProp(d, "Translation")
			sc := 1.0
			if vo, _, ok := p.propValue(d, "Scale", "FloatProperty"); ok {
				sc = float64(math.Float32frombits(binary_u32(d[vo:])))
			}
			sx, sy := sc, sc
			if vo, sz, ok := p.propValue(d, "Scale3D", "StructProperty"); ok && sz >= 12 {
				sx *= float64(math.Float32frombits(binary_u32(d[vo+8:])))
				sy *= float64(math.Float32frombits(binary_u32(d[vo+12:])))
			}
			yaw := 0.0
			if vo, sz, ok := p.propValue(d, "Rotation", "StructProperty"); ok && sz >= 12 {
				yaw = float64(int32(binary_u32(d[vo+8+4:]))) / 65536 * 2 * math.Pi
			}
			mi := int32(0)
			mesh := ""
			if vo, _, ok := p.propValue(d, "StaticMesh", "ObjectProperty"); ok {
				mi = int32(binary_u32(d[vo:]))
				mesh = p.objPath(mi)
			}
			b := p.tMeshBounds(mi, cache)
			ox, oy := b.ox*sx, b.oy*sy
			cx := x + ox*math.Cos(yaw) - oy*math.Sin(yaw)
			cy := y + ox*math.Sin(yaw) + oy*math.Cos(yaw)
			ext := math.Max(math.Abs(b.ex*sx), math.Abs(b.ey*sy))
			nx := math.Max(0, math.Abs(cx)-ext)
			ny := math.Max(0, math.Abs(cy)-ext)
			fmt.Printf("%-34s piv=%.2f ctr=%.2f near=%.2f ext=%.0f ok=%v bd=%v\n", mesh, farFactor(x, y), farFactor(cx, cy), farFactor(nx, ny), ext, b.ok, c.backdrop[i])
		}
	}
}

func TestLoose(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	p, _ := Open(f)
	l, _ := p.level()
	for _, a := range p.actors(l) {
		if a <= 0 {
			continue
		}
		e := &p.Exports[a-1]
		c := p.ClassOf(e)
		if c == "StaticMeshCollectionActor" {
			continue
		}
		x, y, z, ok := p.vecProp(p.Data(e), "Location")
		fmt.Printf("%-28s %-30s loc=%v (%.0f %.0f %.0f) r=%.2f\n", c, p.NameOf(e.Name, e.NameNum), ok, x, y, z, farFactor(x, y))
	}
}
