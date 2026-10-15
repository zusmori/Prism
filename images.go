//go:build windows

package main

// Workshop-Vorschaubilder sind oft klein. Damit sie in der grossen Vorschau
// nicht matschig aussehen, werden sie einmal sauber hochskaliert
// (bikubisch, Catmull-Rom) und leicht nachgeschaerft. Das Ergebnis wird
// zwischengespeichert, Steam wird dafuer nicht erneut abgefragt.

import (
	"bytes"
	"image"
	"image/color"
	_ "image/gif"
	"image/jpeg"
	_ "image/png"
	"math"
	"os"
	"path/filepath"
)

const hqWidth = 1600

func hqPath(id string) string { return filepath.Join(previewDir, id+".hq.jpg") }

// upscaleAll erzeugt fehlende HQ-Bilder. true, wenn sich etwas geaendert hat.
func upscaleAll(ids []string) bool {
	changed := false
	for _, id := range ids {
		if id == "" {
			continue
		}
		if _, err := os.Stat(hqPath(id)); err == nil {
			continue
		}
		src, err := os.ReadFile(previewPath(id))
		if err != nil {
			continue
		}
		out, err := upscale(src)
		if err != nil || out == nil {
			continue
		}
		if os.WriteFile(hqPath(id), out, 0644) == nil {
			changed = true
		}
	}
	return changed
}

func upscale(src []byte) ([]byte, error) {
	img, _, err := image.Decode(bytes.NewReader(src))
	if err != nil {
		return nil, err
	}
	b := img.Bounds()
	sw, sh := b.Dx(), b.Dy()
	if sw < 16 || sh < 16 {
		return nil, nil
	}
	scale := float64(hqWidth) / float64(sw)
	if scale > 4 {
		scale = 4
	}
	if scale < 1.15 { // schon gross genug
		scale = 1
	}
	dw, dh := int(math.Round(float64(sw)*scale)), int(math.Round(float64(sh)*scale))

	// in Float-Puffer laden
	px := make([]float32, sw*sh*3)
	for y := 0; y < sh; y++ {
		for x := 0; x < sw; x++ {
			c := color.NRGBAModel.Convert(img.At(b.Min.X+x, b.Min.Y+y)).(color.NRGBA)
			i := (y*sw + x) * 3
			px[i], px[i+1], px[i+2] = float32(c.R), float32(c.G), float32(c.B)
		}
	}
	out := px
	if scale != 1 {
		tmp := resample1D(px, sw, sh, dw, true)  // horizontal
		out = resample1D(tmp, dw, sh, dh, false) // vertikal
	}
	out = unsharp(out, dw, dh, math.Max(0.8, math.Min(2.0, 0.6*scale)), 0.55)

	dst := image.NewRGBA(image.Rect(0, 0, dw, dh))
	for i, j := 0, 0; i < len(out); i, j = i+3, j+4 {
		dst.Pix[j], dst.Pix[j+1], dst.Pix[j+2], dst.Pix[j+3] = clamp8(out[i]), clamp8(out[i+1]), clamp8(out[i+2]), 255
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, dst, &jpeg.Options{Quality: 90}); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func cubicW(x float64) float64 { // Catmull-Rom
	x = math.Abs(x)
	switch {
	case x < 1:
		return 1.5*x*x*x - 2.5*x*x + 1
	case x < 2:
		return -0.5*x*x*x + 2.5*x*x - 4*x + 2
	}
	return 0
}

// skaliert entlang einer Achse (Vergroesserung)
func resample1D(src []float32, w, h, n int, horizontal bool) []float32 {
	var dst []float32
	srcLen := h
	if horizontal {
		dst = make([]float32, n*h*3)
		srcLen = w
	} else {
		dst = make([]float32, w*n*3)
	}
	type tap struct {
		idx [4]int
		wt  [4]float32
	}
	taps := make([]tap, n)
	for o := 0; o < n; o++ {
		s := (float64(o)+0.5)*float64(srcLen)/float64(n) - 0.5
		i0 := int(math.Floor(s))
		var sum float64
		var t tap
		for k := 0; k < 4; k++ {
			i := i0 - 1 + k
			wv := cubicW(s - float64(i))
			if i < 0 {
				i = 0
			}
			if i >= srcLen {
				i = srcLen - 1
			}
			t.idx[k], t.wt[k] = i, float32(wv)
			sum += wv
		}
		for k := range t.wt {
			t.wt[k] /= float32(sum)
		}
		taps[o] = t
	}
	if horizontal {
		for y := 0; y < h; y++ {
			row := y * w * 3
			for x := 0; x < n; x++ {
				t := taps[x]
				d := (y*n + x) * 3
				for c := 0; c < 3; c++ {
					dst[d+c] = src[row+t.idx[0]*3+c]*t.wt[0] + src[row+t.idx[1]*3+c]*t.wt[1] +
						src[row+t.idx[2]*3+c]*t.wt[2] + src[row+t.idx[3]*3+c]*t.wt[3]
				}
			}
		}
	} else {
		for y := 0; y < n; y++ {
			t := taps[y]
			for x := 0; x < w; x++ {
				d := (y*w + x) * 3
				for c := 0; c < 3; c++ {
					dst[d+c] = src[(t.idx[0]*w+x)*3+c]*t.wt[0] + src[(t.idx[1]*w+x)*3+c]*t.wt[1] +
						src[(t.idx[2]*w+x)*3+c]*t.wt[2] + src[(t.idx[3]*w+x)*3+c]*t.wt[3]
				}
			}
		}
	}
	return dst
}

// leichtes Nachschaerfen (Unscharf maskieren)
func unsharp(p []float32, w, h int, sigma, amount float64) []float32 {
	r := int(math.Ceil(sigma * 2))
	k := make([]float32, 2*r+1)
	var sum float32
	for i := -r; i <= r; i++ {
		v := float32(math.Exp(-float64(i*i) / (2 * sigma * sigma)))
		k[i+r] = v
		sum += v
	}
	for i := range k {
		k[i] /= sum
	}
	clampI := func(v, max int) int {
		if v < 0 {
			return 0
		}
		if v >= max {
			return max - 1
		}
		return v
	}
	tmp := make([]float32, len(p))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			for c := 0; c < 3; c++ {
				var acc float32
				for i := -r; i <= r; i++ {
					acc += p[(y*w+clampI(x+i, w))*3+c] * k[i+r]
				}
				tmp[(y*w+x)*3+c] = acc
			}
		}
	}
	out := make([]float32, len(p))
	a := float32(amount)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			for c := 0; c < 3; c++ {
				var acc float32
				for i := -r; i <= r; i++ {
					acc += tmp[(clampI(y+i, h)*w+x)*3+c] * k[i+r]
				}
				o := (y*w + x) * 3
				out[o+c] = p[o+c] + a*(p[o+c]-acc)
			}
		}
	}
	return out
}

func clamp8(v float32) uint8 {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return uint8(v + 0.5)
}
