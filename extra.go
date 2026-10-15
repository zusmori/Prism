//go:build windows

package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"

	webview2 "github.com/jchv/go-webview2"
	"golang.org/x/sys/windows"
)

// ================= Steam-Workshop: Titel & Vorschaubilder =================

type Meta struct {
	Title   string `json:"title"`
	Preview string `json:"preview"`
	Creator string `json:"creator,omitempty"`
	Author  string `json:"author,omitempty"`
}

var (
	metaMu     sync.Mutex
	metaCache  = map[string]Meta{}
	metaFile   = filepath.Join(dataDir, "meta.json")
	previewDir = filepath.Join(dataDir, "previews")
)

func loadMeta() {
	metaMu.Lock()
	defer metaMu.Unlock()
	loadJSON(metaFile, &metaCache)
}

func getMeta(id string) (Meta, bool) {
	if id == "" {
		return Meta{}, false
	}
	metaMu.Lock()
	defer metaMu.Unlock()
	m, ok := metaCache[id]
	return m, ok
}

// Workshop-Ordner heisst wie die Item-ID: ...\content\252950\<ID>\map.udk
func workshopID(root, p string) string {
	rel, err := filepath.Rel(root, p)
	if err != nil {
		return ""
	}
	first := strings.Split(rel, string(filepath.Separator))[0]
	if _, err := strconv.ParseUint(first, 10, 64); err == nil && first != rel {
		return first
	}
	return ""
}

func previewPath(id string) string { return filepath.Join(previewDir, id+".img") }

// Holt fehlende Titel/Vorschaubilder von Steam (oeffentliche API, kein Login noetig).
func fetchMeta(ids []string) bool {
	var need []string
	for _, id := range ids {
		if id == "" {
			continue
		}
		m, ok := getMeta(id)
		_, err := os.Stat(previewPath(id))
		if !ok || err != nil || m.Creator == "" {
			need = append(need, id)
		}
	}
	if len(need) == 0 {
		return false
	}
	_ = os.MkdirAll(previewDir, 0755)
	client := &http.Client{Timeout: 15 * time.Second}

	form := url.Values{"itemcount": {strconv.Itoa(len(need))}}
	for i, id := range need {
		form.Set(fmt.Sprintf("publishedfileids[%d]", i), id)
	}
	resp, err := client.PostForm("https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/", form)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	var out struct {
		Response struct {
			Details []struct {
				ID      string `json:"publishedfileid"`
				Title   string `json:"title"`
				Preview string `json:"preview_url"`
				Creator string `json:"creator"`
			} `json:"publishedfiledetails"`
		} `json:"response"`
	}
	if json.NewDecoder(resp.Body).Decode(&out) != nil {
		return false
	}
	changed := false
	for _, d := range out.Response.Details {
		if d.ID == "" {
			continue
		}
		metaMu.Lock()
		old := metaCache[d.ID]
		author := old.Author
		if old.Creator != d.Creator {
			author = ""
		}
		metaCache[d.ID] = Meta{Title: strings.TrimSpace(d.Title), Preview: d.Preview, Creator: d.Creator, Author: author}
		metaMu.Unlock()
		changed = true
		if author == "" && d.Creator != "" {
			if n := profileName(d.Creator); n != "" {
				metaMu.Lock()
				m := metaCache[d.ID]
				m.Author = n
				metaCache[d.ID] = m
				metaMu.Unlock()
			}
		}
		if _, err := os.Stat(previewPath(d.ID)); err == nil && old.Preview == d.Preview {
			continue // Bild ist schon da und unveraendert
		}
		_ = os.Remove(hqPath(d.ID))
		if d.Preview != "" {
			if r, err := client.Get(d.Preview); err == nil {
				b, _ := io.ReadAll(io.LimitReader(r.Body, 8<<20))
				r.Body.Close()
				if len(b) > 0 {
					_ = os.WriteFile(previewPath(d.ID), b, 0644)
				}
			}
		}
	}
	if changed {
		metaMu.Lock()
		_ = saveJSON(metaFile, metaCache)
		metaMu.Unlock()
	}
	return changed
}

func dataURL(b []byte) string {
	ct := http.DetectContentType(b)
	if !strings.HasPrefix(ct, "image/") {
		return ""
	}
	return "data:" + ct + ";base64," + base64.StdEncoding.EncodeToString(b)
}

// Vorschaubild zu einer Map: Steam-Bild, sonst ein Bild im selben Ordner.
func previewFor(id, mapPath string) string {
	if id != "" {
		// hochskalierte Version bevorzugen
		if b, err := os.ReadFile(hqPath(id)); err == nil {
			if u := dataURL(b); u != "" {
				return u
			}
		}
		if b, err := os.ReadFile(previewPath(id)); err == nil {
			if u := dataURL(b); u != "" {
				return u
			}
		}
	}
	dir := filepath.Dir(mapPath)
	for _, pat := range []string{"*.jpg", "*.jpeg", "*.png", "*.webp"} {
		files, _ := filepath.Glob(filepath.Join(dir, pat))
		for _, f := range files {
			if fi, err := os.Stat(f); err == nil && fi.Size() < 8<<20 {
				if b, err := os.ReadFile(f); err == nil {
					if u := dataURL(b); u != "" {
						return u
					}
				}
			}
		}
	}
	return ""
}

// ================= Rahmenloses, modernes Fenster =================

var (
	user32  = windows.NewLazySystemDLL("user32.dll")
	dwmapi  = windows.NewLazySystemDLL("dwmapi.dll")
	pSetWL  = user32.NewProc("SetWindowLongPtrW")
	pSetPos = user32.NewProc("SetWindowPos")
	pShow   = user32.NewProc("ShowWindow")
	pRelCap = user32.NewProc("ReleaseCapture")
	pSend   = user32.NewProc("SendMessageW")
	pPost   = user32.NewProc("PostMessageW")
	pMetric = user32.NewProc("GetSystemMetrics")
	pDpi    = user32.NewProc("GetDpiForWindow")
	pDwmAtt = dwmapi.NewProc("DwmSetWindowAttribute")
	pDwmExt = dwmapi.NewProc("DwmExtendFrameIntoClientArea")
)

const (
	winW = 960
	winH = 620
	minW = 780
	minH = 520
)

var (
	pIsZoomed = user32.NewProc("IsZoomed")
	pCallProc = user32.NewProc("CallWindowProcW")
	pMetricD  = user32.NewProc("GetSystemMetricsForDpi")
	oldProc   uintptr
	gw        webview2.WebView
	curDpi    = uintptr(96)
	allowClose bool
	closeAck   bool
)

func isMax(hwnd uintptr) bool { r, _, _ := pIsZoomed.Call(hwnd); return r != 0 }

func metric(idx uintptr) int32 {
	if pMetricD.Find() == nil {
		r, _, _ := pMetricD.Call(idx, curDpi)
		return int32(r)
	}
	r, _, _ := pMetric.Call(idx)
	return int32(r)
}

// Eigene Fensterprozedur: entfernt die Windows-Titelleiste, behaelt aber
// Groesse aendern, Maximieren (respektiert die Taskleiste) und Aero Snap.
func subclassProc(hwnd, msg, wp, lp uintptr) uintptr {
	if trayMessage(hwnd, msg, lp) {
		return 0
	}
	switch msg {
	case 0x0083: // WM_NCCALCSIZE
		if wp != 0 {
			if isMax(hwnd) {
				// maximiert ragt das Fenster um den Rahmen ueber den Bildschirm -> wieder einruecken
				fx := metric(32) + metric(92) // SM_CXSIZEFRAME + SM_CXPADDEDBORDER
				fy := metric(33) + metric(92)
				r := (*[4]int32)(unsafe.Pointer(lp))
				r[0] += fx
				r[1] += fy
				r[2] -= fx
				r[3] -= fy
			}
			return 0
		}
	case 0x0024: // WM_GETMINMAXINFO
		mmi := (*[10]int32)(unsafe.Pointer(lp))
		mmi[6] = int32(minW * int(curDpi) / 96)
		mmi[7] = int32(minH * int(curDpi) / 96)
		return 0
	case 0x0010: // WM_CLOSE -> erst die Schliess-Animation abspielen
		if !allowClose && gw != nil {
			allowClose = true // zweites Schliessen (z. B. erneutes Alt+F4) geht sofort
			w := gw
			go w.Dispatch(func() { w.Eval("window.__requestClose ? window.__requestClose() : 0") })
			closeAck = false
			go func() { // Sicherheitsnetz, falls die Oberflaeche gar nicht reagiert
				time.Sleep(4 * time.Second)
				if !closeAck {
					pPost.Call(hwnd, 0x0010, 0, 0)
				}
			}()
			return 0
		}
	case 0x02E0: // WM_DPICHANGED
		curDpi = wp & 0xFFFF
	case 0x0005: // WM_SIZE
		r, _, _ := pCallProc.Call(oldProc, hwnd, msg, wp, lp)
		if gw != nil && (wp == 0 || wp == 2) {
			m := "false"
			if wp == 2 {
				m = "true"
			}
			w := gw
			go w.Dispatch(func() { w.Eval("window.__max && window.__max(" + m + ")") })
		}
		return r
	}
	r, _, _ := pCallProc.Call(oldProc, hwnd, msg, wp, lp)
	return r
}

func setupWindow(hwnd uintptr) {
	if pDpi.Find() == nil {
		if d, _, _ := pDpi.Call(hwnd); d > 0 {
			curDpi = d
		}
	}
	// Fensterprozedur einhaengen (GWLP_WNDPROC = -4)
	gwlpWndProc := -4
	oldProc, _, _ = pSetWL.Call(hwnd, uintptr(gwlpWndProc), windows.NewCallback(subclassProc))

	// Dunkler Modus, runde Ecken (Windows 11) und Schatten
	one := int32(1)
	pDwmAtt.Call(hwnd, 20, uintptr(unsafe.Pointer(&one)), 4)
	round := int32(2)
	pDwmAtt.Call(hwnd, 33, uintptr(unsafe.Pointer(&round)), 4)
	border := uint32(0x00221A1F)
	pDwmAtt.Call(hwnd, 34, uintptr(unsafe.Pointer(&border)), 4)
	margins := [4]int32{0, 0, 0, 1}
	pDwmExt.Call(hwnd, uintptr(unsafe.Pointer(&margins)))

	// Startgroesse passend zur Skalierung, mittig
	w := winW * int(curDpi) / 96
	h := winH * int(curDpi) / 96
	sw, _, _ := pMetric.Call(0)
	sh, _, _ := pMetric.Call(1)
	x := (int(sw) - w) / 2
	y := (int(sh) - h) / 2
	const swpFrameChanged, swpNoZ, swpHide = 0x20, 0x4, 0x80
	pSetPos.Call(hwnd, 0, uintptr(x), uintptr(y), uintptr(w), uintptr(h), swpFrameChanged|swpNoZ|swpHide)
	pShow.Call(hwnd, 0) // erst zeigen, wenn die Oberflaeche geladen ist
}

func bindWindow(w webview2.WebView, hwnd uintptr) {
	gw = w
	shown := false
	var once sync.Once
	show := func() {
		once.Do(func() {
			shown = true
			pShow.Call(hwnd, 5)
		})
	}
	w.Bind("appReady", func() { show() })
	go func() {
		time.Sleep(2500 * time.Millisecond)
		if !shown {
			w.Dispatch(show)
		}
	}()
	w.Bind("dragWindow", func() {
		pRelCap.Call()
		pSend.Call(hwnd, 0x00A1, 2, 0) // WM_NCLBUTTONDOWN, HTCAPTION
	})
	// Rand ziehen = Groesse aendern (HTLEFT=10 ... HTBOTTOMRIGHT=17)
	w.Bind("resizeWindow", func(edge int) {
		if edge < 10 || edge > 17 || isMax(hwnd) {
			return
		}
		pRelCap.Call()
		pSend.Call(hwnd, 0x00A1, uintptr(edge), 0)
	})
	w.Bind("minimizeWindow", func() { pShow.Call(hwnd, 6) })
	w.Bind("toggleMaximize", func() bool {
		if isMax(hwnd) {
			pShow.Call(hwnd, 9) // SW_RESTORE
			return false
		}
		pShow.Call(hwnd, 3) // SW_MAXIMIZE
		return true
	})
	w.Bind("isMaximized", func() bool { return isMax(hwnd) })
	w.Bind("closeWindow", func() { allowClose = true; pPost.Call(hwnd, 0x0010, 0, 0) })
	// X / Alt+F4: in den Tray statt beenden
	w.Bind("hideToTray", func() { hideToTray(hwnd) })
	w.Bind("showWindow", func() { showFromTray(hwnd) })
	trayAdd(hwnd)
	// Oberflaeche hat das Schliessen uebernommen (z. B. Nachfrage-Dialog offen)
	w.Bind("closeAck", func() { closeAck = true; allowClose = true })
	// Schliessen abgebrochen -> naechstes Alt+F4 wieder mit Animation/Nachfrage
	w.Bind("cancelClose", func() { allowClose = false })

	// Einstellung: Intro-Sound an/aus
	settingsFile := filepath.Join(dataDir, "settings.json")
	// Einstellungen (z. B. "keepSwaps": Swaps beim Schliessen behalten, ohne Nachfrage)
	// Werte koennen Schalter (bool) oder Text sein (z. B. "theme")
	w.Bind("getSettings", func() map[string]interface{} {
		st := map[string]interface{}{}
		loadJSON(settingsFile, &st)
		return st
	})
	w.Bind("setSetting", func(key string, val interface{}) error {
		st := map[string]interface{}{}
		loadJSON(settingsFile, &st)
		st[key] = val
		return saveJSON(settingsFile, st)
	})
}
