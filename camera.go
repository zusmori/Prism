//go:build windows

package main

// Kamera-Einstellungen direkt in der Rocket-League-Speicherdatei (Profil) setzen, in feinen Schritten.
// So gelten sie auch, wenn mit Anti-Cheat gespielt wird: das Spiel liest einfach deine gespeicherten Werte.
// Nur bei geschlossenem Spiel, und nur innerhalb der Grenzen, die das Spiel selbst erlaubt.
//
// Aufbau der Datei (Documents\My Games\Rocket League\TAGame\SaveData\DBE_Production\<SteamID>.save):
//   uint32 Groesse (Rest der Datei), uint32 Pruefsumme (UE3-CRC mit Startwert 0xEFCBF201 ueber den Rest),
//   danach AES-256-ECB verschluesselt die Eigenschaften des Profils. Darin: Camera (ProfileCameraSettings)
//   mit FOV, Height, Pitch, Distance, Stiffness, SwivelSpeed, TransitionSpeed als FloatProperty.

import (
	"bytes"
	"crypto/aes"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

const saveKey = "14wySpRClDxtZc6YgYVMQWiZIgzHoUZAk5uWPJMqb68="

type camField struct {
	Key, Prop      string
	Min, Max, Step float64
}

// Grenzen wie im Spiel (Camera_TA: FOVLimits, HeightLimits, ...), nur feinere Schritte
var camFields = []camField{
	{"fov", "FOV", 60, 110, 1},
	{"height", "Height", 40, 200, 1},
	{"angle", "Pitch", -15, 0, 0.1},
	{"distance", "Distance", 100, 400, 1},
	{"stiffness", "Stiffness", 0, 1, 0.01},
	{"swivel", "SwivelSpeed", 1, 10, 0.01},
	{"transition", "TransitionSpeed", 1, 2, 0.01},
}

var camCRC [256]uint32

func init() {
	for i := range camCRC {
		c := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if c&0x80000000 != 0 {
				c = c<<1 ^ 0x04C11DB7
			} else {
				c <<= 1
			}
		}
		camCRC[i] = c
	}
}

func ue3CRC(b []byte) uint32 {
	c := ^uint32(0xEFCBF201)
	for _, x := range b {
		c = c<<8 ^ camCRC[byte(c>>24)^x]
	}
	return ^c
}

var reProfileSave = regexp.MustCompile(`^\d{10,20}(_2)?\.save$`)

// Profil-Speicherdateien (meist <SteamID>.save und die Ausweichkopie <SteamID>_2.save)
func cameraSaveFiles() []string {
	dir := filepath.Join(documentsDir(), "My Games", "Rocket League", "TAGame", "SaveData", "DBE_Production")
	ents, _ := os.ReadDir(dir)
	var out []string
	for _, e := range ents {
		if !e.IsDir() && reProfileSave.MatchString(e.Name()) {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

type camSave struct {
	path  string
	raw   []byte
	plain []byte
	offs  map[string]int // Eigenschaft -> Lage des float-Werts in plain
}

func readCamSave(path string) (*camSave, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(raw) < 24 || int(binary.LittleEndian.Uint32(raw)) != len(raw)-8 || (len(raw)-8)%16 != 0 {
		return nil, errors.New("unknown save format")
	}
	if ue3CRC(raw[8:]) != binary.LittleEndian.Uint32(raw[4:]) {
		return nil, errors.New("save checksum doesn't match")
	}
	key, _ := base64.StdEncoding.DecodeString(saveKey)
	plain := ecbCrypt(key, raw[8:], false)
	s := &camSave{path: path, raw: raw, plain: plain, offs: map[string]int{}}
	at := bytes.Index(plain, []byte("ProfileCameraSettings\x00"))
	if at < 0 {
		return nil, errors.New("no camera settings in this save")
	}
	end := at + 600
	if end > len(plain) {
		end = len(plain)
	}
	for _, f := range camFields {
		// FString-Name, dann "FloatProperty", int64 Groesse 4, float
		pat := append([]byte(f.Prop+"\x00"), 0x0e, 0, 0, 0)
		pat = append(pat, []byte("FloatProperty\x00")...)
		i := bytes.Index(plain[at:end], pat)
		if i < 0 {
			continue
		}
		v := at + i + len(pat)
		if v+12 > len(plain) || binary.LittleEndian.Uint64(plain[v:]) != 4 {
			continue
		}
		s.offs[f.Key] = v + 8
	}
	if len(s.offs) == 0 {
		return nil, errors.New("camera settings not understood")
	}
	return s, nil
}

func ecbCrypt(key, data []byte, enc bool) []byte {
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

type CameraState struct {
	Found  bool               `json:"found"`
	Values map[string]float64 `json:"values"`
	Err    string             `json:"err,omitempty"`
}

// neueste lesbare Profil-Datei
func newestCamSave() (*camSave, error) {
	var best *camSave
	var bestT int64
	var lastErr error
	for _, p := range cameraSaveFiles() {
		s, err := readCamSave(p)
		if err != nil {
			lastErr = err
			continue
		}
		st, _ := os.Stat(p)
		if best == nil || st.ModTime().UnixNano() > bestT {
			best, bestT = s, st.ModTime().UnixNano()
		}
	}
	if best == nil {
		if lastErr == nil {
			lastErr = errors.New("No Rocket League profile found. Start the game once first.")
		}
		return nil, lastErr
	}
	return best, nil
}

func GetCamera() CameraState {
	s, err := newestCamSave()
	if err != nil {
		return CameraState{Err: err.Error()}
	}
	vals := map[string]float64{}
	for k, o := range s.offs {
		v := float64(math.Float32frombits(binary.LittleEndian.Uint32(s.plain[o:])))
		vals[k] = math.Round(v*100) / 100
	}
	return CameraState{Found: true, Values: vals}
}

func SetCamera(vals map[string]float64) error {
	if rlRunning() {
		return errors.New("Close Rocket League first. It saves your settings when it quits and would overwrite these.")
	}
	return setCameraFiles(cameraSaveFiles(), vals)
}

func setCameraFiles(files []string, vals map[string]float64) error {
	if len(files) == 0 {
		return errors.New("No Rocket League profile found. Start the game once first.")
	}
	key, _ := base64.StdEncoding.DecodeString(saveKey)
	done := 0
	var firstErr error
	for _, p := range files {
		s, err := readCamSave(p)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		for _, f := range camFields {
			v, ok := vals[f.Key]
			o, has := s.offs[f.Key]
			if !ok || !has || math.IsNaN(v) {
				continue
			}
			v = math.Max(f.Min, math.Min(f.Max, math.Round(v/f.Step)*f.Step)) // nur innerhalb der Spiel-Grenzen
			binary.LittleEndian.PutUint32(s.plain[o:], math.Float32bits(float32(v)))
		}
		enc := ecbCrypt(key, s.plain, true)
		out := make([]byte, 8+len(enc))
		binary.LittleEndian.PutUint32(out, uint32(len(enc)))
		binary.LittleEndian.PutUint32(out[4:], ue3CRC(enc))
		copy(out[8:], enc)
		// Sicherung der Originaldatei (einmal pro Datei)
		bk := filepath.Join(dataDir, "camera_backup")
		_ = os.MkdirAll(bk, 0755)
		if b := filepath.Join(bk, filepath.Base(p)); !fileExists(b) {
			_ = os.WriteFile(b, s.raw, 0644)
		}
		tmp := p + ".prism-tmp"
		if err := os.WriteFile(tmp, out, 0644); err != nil {
			return friendlyErr(err)
		}
		if err := replaceFile(tmp, p); err != nil {
			os.Remove(tmp)
			return friendlyErr(err)
		}
		done++
	}
	if done == 0 {
		if firstErr != nil {
			return firstErr
		}
		return errors.New("Couldn't write the camera settings.")
	}
	var parts []string
	for _, f := range camFields {
		if v, ok := vals[f.Key]; ok {
			parts = append(parts, f.Key+"="+strconv.FormatFloat(v, 'f', -1, 64))
		}
	}
	logf("camera: saved %s to %d file(s)", strings.Join(parts, " "), done)
	return nil
}
