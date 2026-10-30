package upk

import (
	"encoding/binary"
	"fmt"
	"os"
	"sort"
	"strings"
	"testing"
)

// UPK_DUMP=file go test ./upk -run TestDump -v
func TestDump(t *testing.T) {
	f := os.Getenv("UPK_DUMP")
	if f == "" {
		t.Skip()
	}
	p, err := Open(f)
	if err != nil {
		t.Fatal(err)
	}
	lv, _ := p.StreamingLevels()
	for _, l := range lv {
		fmt.Printf("SUB %s %s listed=%v\n", l.Class, l.PackageName, l.Listed)
	}
	l, err := p.level()
	if err != nil {
		fmt.Println("level:", err)
		return
	}
	cls := map[string]int{}
	for _, a := range p.actors(l) {
		if a > 0 && int(a) <= len(p.Exports) {
			cls[p.ClassOf(&p.Exports[a-1])]++
		}
	}
	var ks []string
	for k := range cls {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	for _, k := range ks {
		fmt.Printf("ACTOR %s x%d\n", k, cls[k])
	}
	detail := os.Getenv("UPK_DETAIL") != ""
	meshes := map[string][3]float64{} // mesh -> count, near, far
	for _, c := range p.collections(l) {
		for i, ci := range c.comps {
			e := &p.Exports[ci-1]
			d := p.Data(e)
			x, y, z, _ := p.vecProp(d, "Translation")
			mesh := ""
			if vo, _, ok := p.propValue(d, "StaticMesh", "ObjectProperty"); ok {
				mesh = p.objPath(int32(binary.LittleEndian.Uint32(d[vo:])))
			}
			r := farFactor(x, y)
			m := meshes[mesh]
			m[0]++
			if m[1] == 0 || r < m[1] {
				m[1] = r
			}
			if r > m[2] {
				m[2] = r
			}
			meshes[mesh] = m
			if detail {
				fmt.Printf("COMP %s %.0f %.0f %.0f r=%.2f bd=%v\n", mesh, x, y, z, r, c.backdrop[i])
			}
		}
	}
	ks = ks[:0]
	for k := range meshes {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	for _, k := range ks {
		m := meshes[k]
		fmt.Printf("MESH %-50s n=%3.0f r=%.2f..%.2f\n", k, m[0], m[1], m[2])
	}
	fmt.Println("embedded backdrop:", p.EmbeddedBackdrop(), strings.Repeat("-", 3))
}
