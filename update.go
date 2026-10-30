//go:build windows

package main

// Updater: prueft beim Start den neuesten GitHub-Release und tauscht die
// laufende exe (ZLauncher.exe, frueher ZL.exe / ZMS.exe) auf Wunsch gegen die neue Version aus.
//
// Ablauf beim Installieren:
//   1. neue exe in den App-Ordner herunterladen (%APPDATA%\RLMapSwap\update)
//   2. per ReplaceFile an Ort und Stelle ersetzen (Name und Platz auf dem Desktop bleiben)
//   3. neue Version starten ("--after-update <pid>")
//   4. die neue Version wartet, bis die alte zu ist, und raeumt auf

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Bei jedem Release mit hochzaehlen (passend zum Tag, z. B. v1.2.0)
const appVersion = "1.5.0"

const (
	releaseAPI = "https://api.github.com/repos/zusmori/Prism/releases/latest"
	assetName  = "Prism.exe"
	zlLauncher = "ZLauncher.exe" // Name v1.2.8 bis v1.3.4
	zlAsset    = "ZL.exe"        // Name v1.2.3 bis v1.2.7
	oldAsset   = "ZMS.exe" // Name bis v1.2.2
)

type UpdateInfo struct {
	Available bool   `json:"available"`
	Current   string `json:"current"`
	Version   string `json:"version"`
	Notes     string `json:"notes"`
	URL       string `json:"-"`
	Size      int64  `json:"size"`
	Page      string `json:"page"`
}

var lastUpdate UpdateInfo

func checkUpdate() (UpdateInfo, error) {
	info := UpdateInfo{Current: appVersion}
	req, _ := http.NewRequest("GET", releaseAPI, nil)
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "Prism/"+appVersion)
	c := &http.Client{Timeout: 12 * time.Second}
	resp, err := c.Do(req)
	if err != nil {
		return info, errors.New("Couldn't check for updates.")
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return info, fmt.Errorf("Couldn't check for updates (%d).", resp.StatusCode)
	}
	var rel struct {
		Tag     string `json:"tag_name"`
		Body    string `json:"body"`
		Page    string `json:"html_url"`
		Draft   bool   `json:"draft"`
		Pre     bool   `json:"prerelease"`
		Assets  []struct {
			Name string `json:"name"`
			URL  string `json:"browser_download_url"`
			Size int64  `json:"size"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 2<<20)).Decode(&rel); err != nil {
		return info, errors.New("Couldn't read the update info.")
	}
	if rel.Draft || rel.Pre {
		return info, nil
	}
	info.Version = tagVersion(rel.Tag)
	info.Notes = strings.TrimSpace(rel.Body)
	info.Page = rel.Page
	if info.Version == "" {
		return info, nil
	}
	// bevorzugt Prism.exe, dann die alten Namen, sonst irgendeine .exe im Release
	for _, want := range []string{assetName, zlLauncher, zlAsset, oldAsset, "*.exe"} {
		for _, a := range rel.Assets {
			ok := strings.EqualFold(a.Name, want) || (want == "*.exe" && strings.HasSuffix(strings.ToLower(a.Name), ".exe"))
			if info.URL == "" && ok && strings.HasPrefix(a.URL, "https://github.com/") {
				info.URL, info.Size = a.URL, a.Size
			}
		}
	}
	info.Available = info.URL != "" && newerVersion(info.Version, appVersion)
	lastUpdate = info
	return info, nil
}

// Versionsnummer aus dem Tag holen, egal ob "v1.2.8", "1.2.8" oder "ZLauncher v1.2.8"
var verRe = regexp.MustCompile(`\d+(?:\.\d+){1,2}`)

func tagVersion(tag string) string {
	return verRe.FindString(tag)
}

// "1.10.0" > "1.9.2"
func newerVersion(a, b string) bool {
	pa, pb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < 3; i++ {
		var x, y int
		if i < len(pa) {
			x, _ = strconv.Atoi(pa[i])
		}
		if i < len(pb) {
			y, _ = strconv.Atoi(pb[i])
		}
		if x != y {
			return x > y
		}
	}
	return false
}

// Laedt die neue Version herunter, tauscht die Dateien und startet sie.
// Danach muss sich die laufende App selbst schliessen.
func installUpdate(progress func(pct float64)) error {
	info := lastUpdate
	if !info.Available || info.URL == "" {
		return errors.New("No update available.")
	}
	exe, err := os.Executable()
	if err != nil {
		return errors.New("Couldn't find the Prism program file.")
	}
	// Download nicht neben die exe (sonst springen Desktop-Symbole), sondern in den App-Ordner
	upDir := filepath.Join(dataDir, "update")
	_ = os.MkdirAll(upDir, 0755)
	tmp := filepath.Join(upDir, "Prism.part")
	_ = os.Remove(tmp)

	req, _ := http.NewRequest("GET", info.URL, nil)
	req.Header.Set("User-Agent", "Prism/"+appVersion)
	c := &http.Client{Timeout: 10 * time.Minute}
	resp, err := c.Do(req)
	if err != nil {
		return errors.New("Couldn't download the update. Check your internet connection.")
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("Couldn't download the update (%d).", resp.StatusCode)
	}
	total := resp.ContentLength
	if total <= 0 {
		total = info.Size
	}
	f, err := os.Create(tmp)
	if err != nil {
		return errors.New("Couldn't save the update.")
	}
	var done int64
	buf := make([]byte, 64<<10)
	last := time.Time{}
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, werr := f.Write(buf[:n]); werr != nil {
				f.Close()
				os.Remove(tmp)
				return errors.New("Couldn't save the update.")
			}
			done += int64(n)
			if total > 0 && time.Since(last) > 80*time.Millisecond {
				last = time.Now()
				progress(float64(done) / float64(total) * 100)
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			f.Close()
			os.Remove(tmp)
			return errors.New("The download was interrupted. Try again.")
		}
	}
	f.Close()
	progress(100)

	// Plausibel? (richtige Groesse, Windows-Programm)
	if info.Size > 0 && done != info.Size {
		os.Remove(tmp)
		return errors.New("The update file is incomplete. Try again.")
	}
	if head, _ := readHead(tmp, 2); string(head) != "MZ" {
		os.Remove(tmp)
		return errors.New("The update file is damaged. Try again.")
	}

	// Dateien tauschen - die exe bleibt mit gleichem Namen an derselben Stelle
	old, err := swapExe(exe, tmp, upDir)
	if err != nil {
		os.Remove(tmp)
		return err
	}
	cmd := exec.Command(exe, "--after-update", strconv.Itoa(os.Getpid()))
	if err := cmd.Start(); err != nil {
		_ = os.Remove(exe)
		_ = os.Rename(old, exe)
		return errors.New("Couldn't start the new version.")
	}
	stopHelper()
	return nil
}

var pReplaceFile = windows.NewLazySystemDLL("kernel32.dll").NewProc("ReplaceFileW")

func hide(p string) {
	if u, err := windows.UTF16PtrFromString(p); err == nil {
		_ = windows.SetFileAttributes(u, windows.FILE_ATTRIBUTE_HIDDEN)
	}
}

// Tauscht die laufende exe gegen die neue. Gibt zurueck, wo die alte jetzt liegt.
//  1. ReplaceFile: Windows ersetzt die Datei an Ort und Stelle (Symbol bleibt, wo es ist)
//  2. sonst zwei schnelle Umbenennungen; die alte Datei kommt in den App-Ordner oder versteckt daneben
func swapExe(exe, newFile, upDir string) (string, error) {
	backup := filepath.Join(upDir, "Prism.old")
	_ = os.Remove(backup)
	// neue Datei muss auf demselben Laufwerk liegen wie die exe
	src := newFile
	if filepath.VolumeName(exe) != filepath.VolumeName(newFile) {
		src = exe + ".update"
		_ = os.Remove(src)
		if err := copyFile(newFile, src); err != nil {
			return "", errors.New("Couldn't save the update next to Prism.")
		}
		hide(src)
		backup = exe + ".old"
		_ = os.Remove(backup)
	}
	if pReplaceFile.Find() == nil {
		a, _ := windows.UTF16PtrFromString(exe)
		b, _ := windows.UTF16PtrFromString(src)
		c, _ := windows.UTF16PtrFromString(backup)
		r, _, _ := pReplaceFile.Call(uintptr(unsafe.Pointer(a)), uintptr(unsafe.Pointer(b)), uintptr(unsafe.Pointer(c)), 0x2 /* IGNORE_MERGE_ERRORS */, 0, 0)
		if r != 0 {
			_ = os.Remove(exe + ":Zone.Identifier") // "aus dem Internet"-Markierung nicht von der alten Datei erben
			if backup == exe+".old" {
				hide(backup)
			}
			return backup, nil
		}
		// halb fertig? alte Datei zurueckholen
		if _, err := os.Stat(exe); err != nil {
			_ = os.Rename(backup, exe)
		}
	}
	// Fallback: umbenennen
	if err := os.Rename(exe, backup); err != nil {
		backup = exe + ".old"
		_ = os.Remove(backup)
		if err := os.Rename(exe, backup); err != nil {
			return "", errors.New("Couldn't replace Prism. Close other copies of Prism and try again.")
		}
		hide(backup)
	}
	if err := os.Rename(src, exe); err != nil {
		_ = os.Rename(backup, exe) // zurueckrollen
		return "", errors.New("Couldn't replace Prism.")
	}
	return backup, nil
}

func readHead(p string, n int) ([]byte, error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b := make([]byte, n)
	_, err = io.ReadFull(f, b)
	return b, err
}

// Erster Start nach einem Update: auf die alte Version warten und aufraeumen.
func afterUpdate(args []string) {
	if len(args) > 2 && args[1] == "--after-update" {
		if pid, err := strconv.Atoi(args[2]); err == nil {
			if h, err := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid)); err == nil {
				windows.WaitForSingleObject(h, 15000)
				windows.CloseHandle(h)
			}
		}
	}
	if exe, err := os.Executable(); err == nil {
		for i := 0; i < 10; i++ {
			if err := os.Remove(exe + ".old"); err == nil || os.IsNotExist(err) {
				break
			}
			time.Sleep(300 * time.Millisecond)
		}
		_ = os.Remove(exe + ".update")
	}
	upDir := filepath.Join(dataDir, "update")
	for i := 0; i < 10; i++ {
		if err := os.RemoveAll(upDir); err == nil {
			break
		}
		time.Sleep(300 * time.Millisecond)
	}
}
