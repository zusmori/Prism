//go:build windows

package main

// Home-Tab: Rocket League starten (mit / ohne Easy Anti-Cheat) und Steam-Freunde anzeigen.

import (
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/sys/windows/registry"
)

// ---------- Rocket League starten ----------

type RLStatus struct {
	Found   bool `json:"found"`
	Running bool `json:"running"`
	EAC     bool `json:"eac"`
	Steam   bool `json:"steam"`
}

func rlBinDir(cooked string) string {
	if cooked == "" {
		return ""
	}
	// <rocketleague>\TAGame\CookedPCConsole -> <rocketleague>\Binaries\Win64
	return filepath.Join(filepath.Dir(filepath.Dir(cooked)), "Binaries", "Win64")
}

func (a *App) RLStatus() RLStatus {
	if a.cooked == "" {
		a.cooked = findCooked()
	}
	st := RLStatus{Found: a.cooked != "", Running: rlRunning(), Steam: steamRunning()}
	if st.Running {
		st.EAC = eacRunning()
	}
	return st
}

// Startet Rocket League direkt ueber die Spieldateien. Steam muss laufen;
// die App-ID kommt ueber die Umgebung mit, so wie Steam es selbst macht.
func (a *App) LaunchRL(eac bool) error {
	if rlRunning() {
		return errors.New("Rocket League is already running.")
	}
	if !steamRunning() {
		return errSteamClosed
	}
	if a.cooked == "" {
		a.cooked = findCooked()
	}
	bin := rlBinDir(a.cooked)
	exe := filepath.Join(bin, "RocketLeague.exe")
	args := []string{"-noeac"}
	if eac {
		exe = filepath.Join(bin, "RocketLeague_EAC.exe")
		args = nil
	}
	if _, err := os.Stat(exe); bin == "" || err != nil {
		// Fallback: Steam starten lassen (fragt dann selbst nach Anti-Cheat)
		open("steam://rungameid/" + appID)
		return nil
	}
	cmd := exec.Command(exe, args...)
	cmd.Dir = bin
	cmd.Env = append(os.Environ(), "SteamAppId="+appID, "SteamGameId="+appID)
	if err := cmd.Start(); err != nil {
		return errors.New("Couldn't start Rocket League.")
	}
	go cmd.Wait() // Prozess laeuft unabhaengig von ZMS weiter
	return nil
}

// ---------- Steam-Freunde ----------

type Friend struct {
	ID     string `json:"id"`
	Name   string `json:"name"`
	Avatar string `json:"avatar"`
	State  string `json:"state"` // ingame | online | away | offline | unknown
	Game   string `json:"game"`
	Note   string `json:"note"` // z. B. "Last online 3 days ago"
}

type FriendsResult struct {
	Me      string   `json:"me"`
	MeName  string   `json:"meName"`
	Friends []Friend `json:"friends"`
	Source  string   `json:"source"` // api | page | local
	Hint    string   `json:"hint"`
}

// angemeldeter Steam-Account (SteamID64)
func steamUser() (string, string) {
	const base = 76561197960265728
	if k, err := registry.OpenKey(registry.CURRENT_USER, `Software\Valve\Steam\ActiveProcess`, registry.QUERY_VALUE); err == nil {
		v, _, err := k.GetIntegerValue("ActiveUser")
		k.Close()
		if err == nil && v != 0 {
			id := strconv.FormatUint(base+v, 10)
			return id, personaName(id)
		}
	}
	// sonst: zuletzt angemeldeter Account aus loginusers.vdf
	sp := steamPath()
	b, err := os.ReadFile(filepath.Join(sp, "config", "loginusers.vdf"))
	if err != nil {
		return "", ""
	}
	users := vdfChild(parseVDF(string(b)), "users")
	for id, v := range users {
		m, _ := v.(map[string]interface{})
		if vdfStr(m, "MostRecent") == "1" {
			return id, vdfStr(m, "PersonaName")
		}
	}
	return "", ""
}

func personaName(id string) string {
	b, err := os.ReadFile(filepath.Join(steamPath(), "config", "loginusers.vdf"))
	if err != nil {
		return ""
	}
	m, _ := vdfChild(parseVDF(string(b)), "users")[id].(map[string]interface{})
	return vdfStr(m, "PersonaName")
}

// Freunde aus dem lokalen Steam-Client (geht auch bei privater Freundesliste)
func localFriends(me string) []Friend {
	id64, err := strconv.ParseUint(me, 10, 64)
	if err != nil {
		return nil
	}
	acc := strconv.FormatUint(id64-76561197960265728, 10)
	b, err := os.ReadFile(filepath.Join(steamPath(), "userdata", acc, "config", "localconfig.vdf"))
	if err != nil {
		return nil
	}
	fr := vdfChild(vdfChild(parseVDF(string(b)), "UserLocalConfigStore"), "friends")
	if len(fr) == 0 {
		fr = vdfChild(parseVDF(string(b)), "friends")
	}
	var out []Friend
	for k, v := range fr {
		n, err := strconv.ParseUint(k, 10, 64)
		m, ok := v.(map[string]interface{})
		if err != nil || !ok || n == 0 || n >= 1<<32 || k == acc {
			continue
		}
		f := Friend{ID: strconv.FormatUint(n+76561197960265728, 10), Name: vdfStr(m, "name"), State: "unknown"}
		if h := vdfStr(m, "avatar"); len(h) == 40 && h != strings.Repeat("0", 40) {
			f.Avatar = "https://avatars.steamstatic.com/" + h + "_medium.jpg"
		}
		out = append(out, f)
	}
	return out
}

var friendsClient = &http.Client{Timeout: 12 * time.Second}

func steamGet(u string) ([]byte, int, error) {
	req, _ := http.NewRequest("GET", u, nil)
	req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ZMS")
	req.Header.Set("Accept-Language", "en-US,en;q=0.8")
	resp, err := friendsClient.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	return b, resp.StatusCode, err
}

// --- mit API-Key: offizielle Steam Web API ---

func friendsViaAPI(key, me string, fallback []Friend) ([]Friend, error) {
	var ids []string
	b, code, err := steamGet("https://api.steampowered.com/ISteamUser/GetFriendList/v1/?relationship=friend&key=" + url.QueryEscape(key) + "&steamid=" + me)
	if err != nil {
		return nil, errors.New("Couldn't reach Steam.")
	}
	if code == 403 {
		return nil, errors.New("Your Steam API key isn't valid.")
	}
	if code == 200 {
		var fl struct {
			FriendsList struct {
				Friends []struct {
					ID string `json:"steamid"`
				} `json:"friends"`
			} `json:"friendslist"`
		}
		_ = json.Unmarshal(b, &fl)
		for _, f := range fl.FriendsList.Friends {
			ids = append(ids, f.ID)
		}
	}
	if len(ids) == 0 { // Liste privat -> lokale Liste
		for _, f := range fallback {
			ids = append(ids, f.ID)
		}
	}
	var out []Friend
	for i := 0; i < len(ids); i += 100 {
		end := i + 100
		if end > len(ids) {
			end = len(ids)
		}
		b, code, err := steamGet("https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/?key=" + url.QueryEscape(key) + "&steamids=" + strings.Join(ids[i:end], ","))
		if err != nil || code != 200 {
			return nil, errors.New("Couldn't load your friends from Steam.")
		}
		var ps struct {
			Response struct {
				Players []struct {
					ID     string `json:"steamid"`
					Name   string `json:"personaname"`
					Avatar string `json:"avatarmedium"`
					State  int    `json:"personastate"`
					Game   string `json:"gameextrainfo"`
					GameID string `json:"gameid"`
					Last   int64  `json:"lastlogoff"`
				} `json:"players"`
			} `json:"response"`
		}
		_ = json.Unmarshal(b, &ps)
		for _, p := range ps.Response.Players {
			f := Friend{ID: p.ID, Name: p.Name, Avatar: p.Avatar, Game: p.Game}
			switch {
			case p.GameID != "" || p.Game != "":
				f.State = "ingame"
			case p.State == 1 || p.State == 5 || p.State == 6:
				f.State = "online"
			case p.State == 2 || p.State == 3 || p.State == 4:
				f.State = "away"
			default:
				f.State = "offline"
				if p.Last > 0 {
					f.Note = "Last online " + ago(time.Unix(p.Last, 0))
				}
			}
			out = append(out, f)
		}
	}
	return out, nil
}

func ago(t time.Time) string {
	d := time.Since(t)
	switch {
	case d < time.Hour:
		return fmt.Sprintf("%d min. ago", int(d.Minutes())+1)
	case d < 48*time.Hour:
		return fmt.Sprintf("%d hrs. ago", int(d.Hours()))
	default:
		return fmt.Sprintf("%d days ago", int(d.Hours()/24))
	}
}

// --- ohne Key: oeffentliche Freundesseite ---

var (
	fbSplit   = regexp.MustCompile(`class="[^"]*friend_block_v2[^"]*"`)
	fbClass   = regexp.MustCompile(`^class="([^"]*)"`)
	fbID      = regexp.MustCompile(`data-steamid="(\d+)"`)
	fbImg     = regexp.MustCompile(`<img[^>]+src="([^"]+)"`)
	fbContent = regexp.MustCompile(`(?s)class="friend_block_content"[^>]*>(.*?)<span class="friend_small_text"[^>]*>(.*?)</span>`)
	tagRe     = regexp.MustCompile(`<[^>]+>`)
)

func cleanHTML(s string) string {
	s = strings.ReplaceAll(s, "<br>", " · ")
	s = strings.ReplaceAll(s, "<br/>", " · ")
	s = htmlUnescape(tagRe.ReplaceAllString(s, ""))
	return strings.Join(strings.Fields(s), " ")
}

func friendsViaPage(me string) ([]Friend, bool) {
	b, code, err := steamGet("https://steamcommunity.com/profiles/" + me + "/friends/")
	if err != nil || code != 200 {
		return nil, false
	}
	html := string(b)
	locs := fbSplit.FindAllStringIndex(html, -1)
	if len(locs) == 0 {
		return nil, false // privat oder Seite anders aufgebaut
	}
	var out []Friend
	for i, l := range locs {
		end := len(html)
		if i+1 < len(locs) {
			end = locs[i+1][0]
		}
		blk := html[l[0]:end]
		cls := ""
		if m := fbClass.FindStringSubmatch(blk); m != nil {
			cls = m[1]
		}
		idm := fbID.FindStringSubmatch(blk)
		if idm == nil {
			continue
		}
		f := Friend{ID: idm[1], State: "offline"}
		if m := fbImg.FindStringSubmatch(blk); m != nil {
			f.Avatar = m[1]
		}
		if m := fbContent.FindStringSubmatch(blk); m != nil {
			f.Name = strings.TrimSpace(strings.TrimSuffix(cleanHTML(m[1]), "\u00b7"))
			f.Note = cleanHTML(m[2])
		}
		switch {
		case strings.Contains(cls, "in-game"):
			f.State = "ingame"
			f.Game = strings.TrimSpace(strings.TrimPrefix(f.Note, "In-Game ·"))
			f.Note = ""
		case strings.Contains(cls, "online"):
			f.State = "online"
			if strings.EqualFold(f.Note, "Online") {
				f.Note = ""
			}
		}
		if f.Name != "" {
			out = append(out, f)
		}
	}
	return out, true
}

// --- Fallback: Status einzeln ueber die oeffentlichen Profile ---

type profileXML struct {
	State   string `xml:"onlineState"`
	Msg     string `xml:"stateMessage"`
	Name    string `xml:"steamID"`
	Avatar  string `xml:"avatarMedium"`
	InGame  struct {
		Name string `xml:"gameName"`
	} `xml:"inGameInfo"`
}

func fillStatus(fr []Friend) {
	var wg sync.WaitGroup
	sem := make(chan struct{}, 8)
	for i := range fr {
		wg.Add(1)
		go func(f *Friend) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			b, code, err := steamGet("https://steamcommunity.com/profiles/" + f.ID + "?xml=1")
			if err != nil || code != 200 {
				return
			}
			var p profileXML
			if xml.Unmarshal(b, &p) != nil {
				return
			}
			if p.Name != "" {
				f.Name = strings.TrimSpace(p.Name)
			}
			if p.Avatar != "" {
				f.Avatar = strings.TrimSpace(p.Avatar)
			}
			switch p.State {
			case "in-game":
				f.State, f.Game = "ingame", strings.TrimSpace(p.InGame.Name)
			case "online":
				f.State = "online"
			case "offline":
				f.State = "offline"
				f.Note = cleanHTML(p.Msg)
			}
		}(&fr[i])
	}
	wg.Wait()
}

func steamFriends(key string) (FriendsResult, error) {
	res := FriendsResult{Friends: []Friend{}}
	if !steamRunning() {
		return res, errSteamClosed
	}
	me, name := steamUser()
	if me == "" {
		return res, errors.New("Couldn't find your Steam account. Log in to Steam and try again.")
	}
	res.Me, res.MeName = me, name
	local := localFriends(me)
	key = strings.TrimSpace(key)
	switch {
	case key != "":
		fr, err := friendsViaAPI(key, me, local)
		if err != nil {
			return res, err
		}
		res.Friends, res.Source = fr, "api"
	default:
		if fr, ok := friendsViaPage(me); ok {
			res.Friends, res.Source = fr, "page"
		} else {
			if len(local) > 120 {
				local = local[:120]
			}
			fillStatus(local)
			res.Friends, res.Source = local, "local"
			res.Hint = "Your Steam friends list is private, so status may be missing. Make it public in Steam or add a Steam API key in Settings."
		}
	}
	rank := map[string]int{"ingame": 0, "online": 1, "away": 2, "unknown": 3, "offline": 4}
	sort.SliceStable(res.Friends, func(i, j int) bool {
		a, b := res.Friends[i], res.Friends[j]
		if a.Game == "Rocket League" != (b.Game == "Rocket League") {
			return a.Game == "Rocket League"
		}
		if rank[a.State] != rank[b.State] {
			return rank[a.State] < rank[b.State]
		}
		return strings.ToLower(a.Name) < strings.ToLower(b.Name)
	})
	return res, nil
}

// ---------- kleiner VDF-Leser (Steam-Textformat) ----------

func parseVDF(s string) map[string]interface{} {
	toks := vdfTokens(s)
	pos := 0
	var obj func() map[string]interface{}
	obj = func() map[string]interface{} {
		m := map[string]interface{}{}
		for pos < len(toks) {
			t := toks[pos]
			pos++
			if t == "}" {
				return m
			}
			if pos >= len(toks) {
				break
			}
			if toks[pos] == "{" {
				pos++
				m[t] = obj()
			} else {
				m[t] = toks[pos]
				pos++
			}
		}
		return m
	}
	return obj()
}

func vdfTokens(s string) []string {
	var out []string
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case c == '{' || c == '}':
			out = append(out, string(c))
		case c == '"':
			var sb strings.Builder
			for i++; i < len(s) && s[i] != '"'; i++ {
				if s[i] == '\\' && i+1 < len(s) {
					i++
					switch s[i] {
					case 'n':
						sb.WriteByte('\n')
					case 't':
						sb.WriteByte('\t')
					default:
						sb.WriteByte(s[i])
					}
					continue
				}
				sb.WriteByte(s[i])
			}
			out = append(out, sb.String())
		case c == '/' && i+1 < len(s) && s[i+1] == '/':
			for i < len(s) && s[i] != '\n' {
				i++
			}
		}
	}
	return out
}

// Kind-Objekt, Gross-/Kleinschreibung egal
func vdfChild(m map[string]interface{}, key string) map[string]interface{} {
	for k, v := range m {
		if strings.EqualFold(k, key) {
			if c, ok := v.(map[string]interface{}); ok {
				return c
			}
		}
	}
	return map[string]interface{}{}
}

func vdfStr(m map[string]interface{}, key string) string {
	for k, v := range m {
		if strings.EqualFold(k, key) {
			s, _ := v.(string)
			return s
		}
	}
	return ""
}
