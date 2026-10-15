//go:build windows

package main

// Updater: prueft beim Start den neuesten GitHub-Release und tauscht die
// laufende ZMS.exe auf Wunsch gegen die neue Version aus.
//
// Ablauf beim Installieren:
//   1. neue ZMS.exe neben die alte herunterladen (ZMS.exe.update)
//   2. laufende ZMS.exe in ZMS.exe.old umbenennen (geht unter Windows auch im Betrieb)
//   3. neue Datei an ihren Platz schieben und starten ("--after-update <pid>")
//   4. die neue Version wartet, bis die alte zu ist, und loescht ZMS.exe.old

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"golang.org/x/sys/windows"
)

// Bei jedem Release mit hochzaehlen (passend zum Tag, z. B. v1.2.0)
const appVersion = "1.2.3"

const (
	releaseAPI = "https://api.github.com/repos/zusmori/zusmoris-map-swapper/releases/latest"
	assetName  = "ZMS.exe"
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
	req.Header.Set("User-Agent", "ZMS/"+appVersion)
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
	info.Version = strings.TrimPrefix(strings.TrimSpace(rel.Tag), "v")
	info.Notes = strings.TrimSpace(rel.Body)
	info.Page = rel.Page
	for _, a := range rel.Assets {
		if strings.EqualFold(a.Name, assetName) && strings.HasPrefix(a.URL, "https://github.com/") {
			info.URL, info.Size = a.URL, a.Size
		}
	}
	info.Available = info.URL != "" && newerVersion(info.Version, appVersion)
	lastUpdate = info
	return info, nil
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
		return errors.New("Couldn't find ZMS.exe.")
	}
	tmp, old := exe+".update", exe+".old"
	_ = os.Remove(tmp)

	req, _ := http.NewRequest("GET", info.URL, nil)
	req.Header.Set("User-Agent", "ZMS/"+appVersion)
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
		return errors.New("ZMS can't write to its own folder. Move ZMS.exe somewhere like Downloads or Desktop and try again.")
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

	// Dateien tauschen
	_ = os.Remove(old)
	if err := os.Rename(exe, old); err != nil {
		os.Remove(tmp)
		return errors.New("Couldn't replace ZMS.exe. Close other copies of ZMS and try again.")
	}
	if err := os.Rename(tmp, exe); err != nil {
		_ = os.Rename(old, exe) // zurueckrollen
		os.Remove(tmp)
		return errors.New("Couldn't replace ZMS.exe.")
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
}
