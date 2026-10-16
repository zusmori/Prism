//go:build windows

package main

// "Browse Maps": liest die oeffentliche Workshop-Liste von Rocket League
// (kein Login, kein API-Key) und holt Details ueber die oeffentliche Steam-API.

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type BrowseItem struct {
	ID      string  `json:"id"`
	Title   string  `json:"title"`
	Preview string  `json:"preview"`
	MB      float64 `json:"mb"`
	Subs    int     `json:"subs"`
	Fav     int     `json:"favs"`
	Created int64   `json:"created"`
	Author  string  `json:"author"`
}

type BrowsePage struct {
	Items   []BrowseItem `json:"items"`
	HasMore bool         `json:"hasMore"`
}

var (
	idRe      = regexp.MustCompile(`data-publishedfileid="(\d+)"|sharedfiles/filedetails/\?id=(\d+)`)
	webClient = &http.Client{Timeout: 20 * time.Second}
)

// Von zusmori empfohlene Maps (oben im Browse-Tab)
var recommendedIDs = []string{"3766065896", "3527331867", "3784853552", "3766064185", "3496414470"}

func recommendedMaps() ([]BrowseItem, error) {
	items, err := fileDetails(recommendedIDs)
	if err != nil {
		return nil, err
	}
	fillAuthors(items)
	return items, nil
}

// Zeitraum in Tagen (-1 = alles)
func periodDays(period string) int {
	switch period {
	case "day":
		return 1
	case "week":
		return 7
	case "month":
		return 30
	case "year":
		return 365
	}
	return -1
}

func browseWorkshop(sort, period, query string, page int) (BrowsePage, error) {
	if page < 1 {
		page = 1
	}
	q := url.Values{
		"appid":      {appID},
		"section":    {"readytouseitems"},
		"p":          {strconv.Itoa(page)},
		"numperpage": {"30"},
	}
	query = strings.TrimSpace(query)
	days := periodDays(period)
	steamSort := "trend"
	switch sort {
	case "subs":
		steamSort = "totaluniquesubscribers"
	case "new":
		steamSort = "mostrecent"
	default:
		if query != "" {
			steamSort = "textsearch" // "Popular" + Suche = beste Treffer
		}
	}
	q.Set("browsesort", steamSort)
	q.Set("actualsort", steamSort)
	if query != "" {
		q.Set("searchtext", query)
	}
	if steamSort == "trend" {
		// Beliebt im gewaehlten Zeitraum (auch aeltere Maps)
		q.Set("days", strconv.Itoa(days))
	} else if days > 0 {
		// sonst: nur Maps, die im Zeitraum veroeffentlicht wurden
		q.Set("created_date_range_filter_start", strconv.FormatInt(time.Now().Add(-time.Duration(days)*24*time.Hour).Unix(), 10))
		q.Set("created_date_range_filter_end", "0")
	}
	req, _ := http.NewRequest("GET", "https://steamcommunity.com/workshop/browse/?"+q.Encode(), nil)
	req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ZLauncher")
	req.Header.Set("Accept-Language", "en-US,en;q=0.8")
	resp, err := webClient.Do(req)
	if err != nil {
		return BrowsePage{}, errors.New("Couldn't reach the Steam Workshop. Check your internet connection.")
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 6<<20))

	pageAuthors := authorsFromPage(string(body))
	var ids []string
	seen := map[string]bool{}
	for _, m := range idRe.FindAllStringSubmatch(string(body), -1) {
		id := m[1]
		if id == "" {
			id = m[2]
		}
		if !seen[id] {
			seen[id] = true
			ids = append(ids, id)
		}
	}
	if len(ids) == 0 {
		return BrowsePage{Items: []BrowseItem{}}, nil
	}
	items, err := fileDetails(ids)
	if err != nil {
		return BrowsePage{}, err
	}
	for i := range items {
		if a := pageAuthors[items[i].ID]; a != "" {
			items[i].Author = a
		}
	}
	fillAuthors(items)
	return BrowsePage{Items: items, HasMore: len(ids) >= 25}, nil
}

func fileDetails(ids []string) ([]BrowseItem, error) {
	form := url.Values{"itemcount": {strconv.Itoa(len(ids))}}
	for i, id := range ids {
		form.Set(fmt.Sprintf("publishedfileids[%d]", i), id)
	}
	resp, err := webClient.PostForm("https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/", form)
	if err != nil {
		return nil, errors.New("Couldn't load map details from Steam.")
	}
	defer resp.Body.Close()
	var out struct {
		Response struct {
			Details []struct {
				ID      string          `json:"publishedfileid"`
				Result  int             `json:"result"`
				AppID   int             `json:"consumer_app_id"`
				Title   string          `json:"title"`
				Preview string          `json:"preview_url"`
				Size    json.RawMessage `json:"file_size"`
				Subs    int             `json:"lifetime_subscriptions"`
				Favs    int             `json:"lifetime_favorited"`
				Created int64           `json:"time_created"`
				Creator string          `json:"creator"`
			} `json:"publishedfiledetails"`
		} `json:"response"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, errors.New("Steam sent an unexpected answer.")
	}
	byID := map[string]BrowseItem{}
	for _, d := range out.Response.Details {
		if d.Result != 1 || (d.AppID != 0 && strconv.Itoa(d.AppID) != appID) {
			continue
		}
		size, _ := strconv.ParseFloat(strings.Trim(string(d.Size), `"`), 64)
		byID[d.ID] = BrowseItem{ID: d.ID, Title: strings.TrimSpace(d.Title), Preview: d.Preview,
			MB: float64(int(size/1048576*10)) / 10, Subs: d.Subs, Fav: d.Favs, Created: d.Created}
		creatorOf.Store(d.ID, d.Creator)
		// gleich fuer die Vorschau in "Downloaded Maps" merken
		metaMu.Lock()
		if _, ok := metaCache[d.ID]; !ok {
			metaCache[d.ID] = Meta{Title: strings.TrimSpace(d.Title), Preview: d.Preview}
		}
		metaMu.Unlock()
	}
	items := []BrowseItem{}
	for _, id := range ids { // Reihenfolge der Workshop-Seite beibehalten
		if it, ok := byID[id]; ok {
			items = append(items, it)
		}
	}
	return items, nil
}

// ---------- Favoriten ----------

var (
	favMu   sync.Mutex
	favFile = filepath.Join(dataDir, "favorites.json")
)

func loadFavs() map[string]bool {
	m := map[string]bool{}
	loadJSON(favFile, &m)
	return m
}

func toggleFav(key string) bool {
	favMu.Lock()
	defer favMu.Unlock()
	m := loadFavs()
	if m[key] {
		delete(m, key)
	} else {
		m[key] = true
	}
	_ = saveJSON(favFile, m)
	return m[key]
}

// ---------- Detailansicht ----------

type MapDetails struct {
	ID          string   `json:"id"`
	Title       string   `json:"title"`
	Description string   `json:"description"`
	Preview     string   `json:"preview"`
	MB          float64  `json:"mb"`
	Subs        int      `json:"subs"`
	Favs        int      `json:"favs"`
	Views       int      `json:"views"`
	Created     int64    `json:"created"`
	Updated     int64    `json:"updated"`
	Tags        []string `json:"tags"`
	Author      string   `json:"author"`
	AuthorURL   string   `json:"authorUrl"`
	URL         string   `json:"url"`
}

var authorRe = regexp.MustCompile(`<steamID><!\[CDATA\[(.*?)\]\]></steamID>`)

func mapDetails(id string) (MapDetails, error) {
	if _, err := strconv.ParseUint(id, 10, 64); err != nil {
		return MapDetails{}, errors.New("Invalid map id.")
	}
	form := url.Values{"itemcount": {"1"}, "publishedfileids[0]": {id}}
	resp, err := webClient.PostForm("https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/", form)
	if err != nil {
		return MapDetails{}, errors.New("Couldn't load map details from Steam.")
	}
	defer resp.Body.Close()
	var out struct {
		Response struct {
			Details []struct {
				Result  int             `json:"result"`
				Title   string          `json:"title"`
				Desc    string          `json:"description"`
				Preview string          `json:"preview_url"`
				Size    json.RawMessage `json:"file_size"`
				Subs    int             `json:"lifetime_subscriptions"`
				Favs    int             `json:"lifetime_favorited"`
				Views   int             `json:"views"`
				Created int64           `json:"time_created"`
				Updated int64           `json:"time_updated"`
				Creator string          `json:"creator"`
				Tags    []struct {
					Tag string `json:"tag"`
				} `json:"tags"`
			} `json:"publishedfiledetails"`
		} `json:"response"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil || len(out.Response.Details) == 0 || out.Response.Details[0].Result != 1 {
		return MapDetails{}, errors.New("This map isn't available anymore.")
	}
	d := out.Response.Details[0]
	size, _ := strconv.ParseFloat(strings.Trim(string(d.Size), `"`), 64)
	md := MapDetails{ID: id, Title: strings.TrimSpace(d.Title), Description: d.Desc, Preview: d.Preview,
		MB: float64(int(size/1048576*10)) / 10, Subs: d.Subs, Favs: d.Favs, Views: d.Views,
		Created: d.Created, Updated: d.Updated, Tags: []string{},
		URL: "https://steamcommunity.com/sharedfiles/filedetails/?id=" + id}
	for _, t := range d.Tags {
		if t.Tag != "" {
			md.Tags = append(md.Tags, t.Tag)
		}
	}
	// Name des Erstellers ueber das oeffentliche Profil (ohne API-Key)
	if _, err := strconv.ParseUint(d.Creator, 10, 64); err == nil {
		md.AuthorURL = "https://steamcommunity.com/profiles/" + d.Creator
		md.Author = profileName(d.Creator)
	}
	return md, nil
}

// ---------- Ersteller-Namen ----------

var (
	authorTagRe = regexp.MustCompile(`(?s)workshopItemAuthorName[^>]*>.*?<a[^>]*>(.*?)</a>`)
	creatorOf   sync.Map // Item-ID -> SteamID des Erstellers
	authorCache sync.Map // SteamID -> Name
)

// Die Workshop-Seite zeigt "by <Name>" direkt unter jedem Eintrag.
func authorsFromPage(html string) map[string]string {
	out := map[string]string{}
	parts := strings.Split(html, `class="workshopItem"`)
	for _, p := range parts[1:] {
		idm := idRe.FindStringSubmatch(p)
		am := authorTagRe.FindStringSubmatch(p)
		if idm == nil || am == nil {
			continue
		}
		id := idm[1]
		if id == "" {
			id = idm[2]
		}
		name := strings.TrimSpace(htmlUnescape(regexp.MustCompile(`<[^>]+>`).ReplaceAllString(am[1], "")))
		if name != "" && out[id] == "" {
			out[id] = name
		}
	}
	return out
}

func htmlUnescape(s string) string {
	r := strings.NewReplacer("&amp;", "&", "&lt;", "<", "&gt;", ">", "&quot;", `"`, "&#39;", "'", "&nbsp;", " ")
	return r.Replace(s)
}

// Fehlende Namen ueber die oeffentlichen Profile nachladen (parallel, mit Cache).
func fillAuthors(items []BrowseItem) {
	var wg sync.WaitGroup
	sem := make(chan struct{}, 6)
	for i := range items {
		if items[i].Author != "" {
			continue
		}
		v, ok := creatorOf.Load(items[i].ID)
		sid, _ := v.(string)
		if !ok || sid == "" {
			continue
		}
		if n, ok := authorCache.Load(sid); ok {
			items[i].Author = n.(string)
			continue
		}
		wg.Add(1)
		go func(i int, sid string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			items[i].Author = profileName(sid)
		}(i, sid)
	}
	wg.Wait()
}

func profileName(sid string) string {
	if n, ok := authorCache.Load(sid); ok {
		return n.(string)
	}
	c := &http.Client{Timeout: 6 * time.Second}
	r, err := c.Get("https://steamcommunity.com/profiles/" + sid + "?xml=1")
	if err != nil {
		return ""
	}
	b, _ := io.ReadAll(io.LimitReader(r.Body, 256<<10))
	r.Body.Close()
	m := authorRe.FindSubmatch(b)
	if m == nil {
		return ""
	}
	name := strings.TrimSpace(string(m[1]))
	authorCache.Store(sid, name)
	return name
}
