//go:build windows

package main

// Kleiner lokaler Server (nur 127.0.0.1) fuer die Oberflaeche und die
// Hintergrund-Videos des Home-Tabs. Seite und Videos kommen so vom gleichen
// Ursprung, WebView2 kann die Videos normal streamen und loopen.

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"io/fs"
	"net"
	"net/http"
	"strings"
	"sync"
)

//go:embed media/*.mp4
var mediaFiles embed.FS

var (
	srvOnce sync.Once
	srvBase string // z. B. http://127.0.0.1:51234/1a2b3c4d5e6f7a8b/
)

func localServer() string {
	srvOnce.Do(func() {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			return
		}
		// zufaelliger Pfad, damit andere Programme die Adresse nicht erraten
		b := make([]byte, 8)
		_, _ = rand.Read(b)
		prefix := "/" + hex.EncodeToString(b) + "/"
		sub, _ := fs.Sub(mediaFiles, "media")
		videos := http.StripPrefix(prefix+"media/", http.FileServer(http.FS(sub)))
		mux := http.NewServeMux()
		mux.HandleFunc(prefix, func(w http.ResponseWriter, r *http.Request) {
			p := strings.TrimPrefix(r.URL.Path, prefix)
			switch {
			case p == "" || p == "index.html":
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				w.Header().Set("Cache-Control", "no-store")
				_, _ = w.Write([]byte(uiHTML))
			case strings.HasPrefix(p, "media/") && strings.HasSuffix(p, ".mp4"):
				w.Header().Set("Content-Type", "video/mp4")
				w.Header().Set("Cache-Control", "max-age=86400")
				videos.ServeHTTP(w, r)
			default:
				http.NotFound(w, r)
			}
		})
		go http.Serve(ln, mux)
		srvBase = "http://" + ln.Addr().String() + prefix
	})
	return srvBase
}

func mediaBase() string {
	if b := localServer(); b != "" {
		return b + "media/"
	}
	return ""
}
