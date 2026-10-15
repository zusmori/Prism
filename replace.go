//go:build windows

package main

// Dateien zuverlaessig ersetzen, auch wenn Rocket League sie gerade geoeffnet hat.
//
// Rocket League oeffnet viele Arena-Dateien beim Start und haelt sie offen
// (Lesen + "Loeschen erlaubt", aber kein Schreiben). Ein normales Ersetzen per
// Umbenennen scheitert dann, ein direktes Ueberschreiben auch. Funktioniert
// dagegen: Ersetzen mit POSIX-Semantik (Windows 10+) oder die alte Datei erst
// zur Seite schieben und dann die neue an ihren Platz legen.

import (
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

const oldSuffix = ".zms-old-"

func replaceFile(tmp, dst string) error {
	// 1) ganz normal
	err := os.Rename(tmp, dst)
	if err == nil {
		return nil
	}
	// 2) POSIX-Ersetzen: klappt auch, wenn das Ziel noch offen ist
	if posixReplace(tmp, dst) == nil {
		return nil
	}
	// 3) alte Datei zur Seite schieben, neue einsetzen
	aside := dst + oldSuffix + strconv.FormatInt(time.Now().UnixNano(), 36)
	if os.Rename(dst, aside) == nil {
		if e := os.Rename(tmp, dst); e == nil {
			_ = os.Remove(aside) // klappt erst, wenn RL die alte Datei loslaesst
			return nil
		}
		_ = os.Rename(aside, dst) // zurueck wie vorher
	}
	// 4) direkt ueberschreiben
	if overwriteInPlace(tmp, dst) == nil {
		return nil
	}
	return err
}

func posixReplace(src, dst string) error {
	p, err := windows.UTF16PtrFromString(src)
	if err != nil {
		return err
	}
	h, err := windows.CreateFile(p, windows.DELETE|windows.SYNCHRONIZE,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE|windows.FILE_SHARE_DELETE,
		nil, windows.OPEN_EXISTING, windows.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return err
	}
	defer windows.CloseHandle(h)

	abs, _ := filepath.Abs(dst)
	name, err := windows.UTF16FromString(abs)
	if err != nil {
		return err
	}
	name = name[:len(name)-1] // ohne abschliessende 0
	// FILE_RENAME_INFO (x64): Flags(4) + Pad(4) + RootDirectory(8) + FileNameLength(4) + FileName
	const hdr = 20
	buf := make([]byte, hdr+len(name)*2+2)
	*(*uint32)(unsafe.Pointer(&buf[0])) = 0x1 | 0x2 // REPLACE_IF_EXISTS | POSIX_SEMANTICS
	*(*uint32)(unsafe.Pointer(&buf[16])) = uint32(len(name) * 2)
	copy(unsafe.Slice((*uint16)(unsafe.Pointer(&buf[hdr])), len(name)), name)
	const fileRenameInfoEx = 22
	return windows.SetFileInformationByHandle(h, fileRenameInfoEx, &buf[0], uint32(len(buf)))
}

// Kann die Datei gerade ersetzt werden? (Nur pruefen, nichts aendern.)
func replaceable(path string) bool {
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return true
	}
	share := uint32(windows.FILE_SHARE_READ | windows.FILE_SHARE_WRITE | windows.FILE_SHARE_DELETE)
	for _, access := range []uint32{windows.DELETE, windows.GENERIC_WRITE} {
		h, err := windows.CreateFile(p, access, share, nil, windows.OPEN_EXISTING, windows.FILE_ATTRIBUTE_NORMAL, 0)
		if err == nil {
			windows.CloseHandle(h)
			return true
		}
		if !errors.Is(err, windows.ERROR_SHARING_VIOLATION) && !errors.Is(err, windows.ERROR_LOCK_VIOLATION) {
			return true // anderer Grund (z. B. Rechte) - nicht als "vom Spiel belegt" melden
		}
	}
	return false
}

// Zur Seite geschobene alte Dateien aufraeumen, sobald sie frei sind.
func cleanupAside(cooked string) {
	if cooked == "" {
		return
	}
	files, _ := filepath.Glob(filepath.Join(cooked, "*"+oldSuffix+"*"))
	for _, f := range files {
		if strings.Contains(filepath.Base(f), oldSuffix) {
			_ = os.Remove(f)
		}
	}
}
