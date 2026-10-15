//go:build windows

package main

// Configs: benannte Presets aus mehreren Swaps (Arena -> Map),
// die sich mit einem Klick wieder anwenden lassen.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type CfgMap struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Title string `json:"title"`
}

type CfgEntry struct {
	Arena     string `json:"arena"`
	ArenaName string `json:"arenaName"`
	Map       CfgMap `json:"map"`
}

type Config struct {
	ID      string     `json:"id"`
	Name    string     `json:"name"`
	Created int64      `json:"created"`
	Entries []CfgEntry `json:"entries"`
}

var (
	cfgMu       sync.Mutex
	configsFile = filepath.Join(dataDir, "configs.json")
)

func loadConfigs() []Config {
	list := []Config{}
	loadJSON(configsFile, &list)
	if list == nil {
		list = []Config{}
	}
	return list
}

func saveConfig(c Config) (Config, error) {
	c.Name = strings.TrimSpace(c.Name)
	if c.Name == "" {
		return c, errors.New("Give your config a name.")
	}
	if len([]rune(c.Name)) > 40 {
		return c, errors.New("That name is a bit long (max 40 characters).")
	}
	if len(c.Entries) == 0 {
		return c, errors.New("Add at least one swap.")
	}
	seen := map[string]bool{}
	for _, e := range c.Entries {
		if e.Arena == "" || strings.ContainsAny(e.Arena, `\/`) || (e.Map.ID == "" && e.Map.Name == "") {
			return c, errors.New("Every swap needs an arena and a map.")
		}
		k := strings.ToLower(e.Arena)
		if seen[k] {
			return c, fmt.Errorf("%s is used twice. Each arena can only hold one map.", e.ArenaName)
		}
		seen[k] = true
		mk := "map:" + strings.ToLower(e.Map.ID+"|"+e.Map.Name)
		if e.Map.ID != "" {
			mk = "map:" + e.Map.ID
		}
		if seen[mk] {
			t := e.Map.Title
			if t == "" {
				t = e.Map.Name
			}
			return c, fmt.Errorf("%s is used twice. A map can only be in one arena.", t)
		}
		seen[mk] = true
	}

	cfgMu.Lock()
	defer cfgMu.Unlock()
	list := loadConfigs()
	for _, o := range list {
		if o.ID != c.ID && strings.EqualFold(o.Name, c.Name) {
			return c, errors.New("You already have a config with that name.")
		}
	}
	if c.ID == "" {
		c.ID = strconv.FormatInt(time.Now().UnixNano(), 36)
		c.Created = time.Now().Unix()
		list = append(list, c)
	} else {
		found := false
		for i := range list {
			if list[i].ID == c.ID {
				c.Created = list[i].Created
				list[i] = c
				found = true
			}
		}
		if !found {
			list = append(list, c)
		}
	}
	return c, saveJSON(configsFile, list)
}

func deleteConfig(id string) error {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	list := loadConfigs()
	out := list[:0]
	for _, c := range list {
		if c.ID != id {
			out = append(out, c)
		}
	}
	return saveJSON(configsFile, out)
}

// Wendet eine Config an: erst alles zuruecksetzen, dann alle Swaps der Config.
func (a *App) ApplyConfig(id string) error {
	if a.cooked == "" {
		return errors.New("Rocket League folder not found.")
	}
	if err := canTouchFiles(); err != nil {
		return err
	}
	var cfg *Config
	for _, c := range loadConfigs() {
		if c.ID == id {
			c := c
			cfg = &c
		}
	}
	if cfg == nil {
		return errors.New("Config not found.")
	}

	// Maps und Arenen vorher pruefen, damit nichts halb angewendet wird
	maps := workshopMaps()
	type job struct{ arena, path string }
	var jobs []job
	var missing []string
	for _, e := range cfg.Entries {
		path := ""
		for _, m := range maps {
			if (e.Map.ID != "" && m.ID == e.Map.ID) || (e.Map.ID == "" && strings.EqualFold(m.Name, e.Map.Name)) {
				path = m.Path
				break
			}
		}
		if path == "" {
			t := e.Map.Title
			if t == "" {
				t = e.Map.Name
			}
			missing = append(missing, t)
			continue
		}
		if !isSwappable(e.Arena) {
			return fmt.Errorf("%s can't be swapped anymore. Edit the config and pick another arena.", e.ArenaName)
		}
		if _, err := os.Stat(filepath.Join(a.cooked, e.Arena)); err != nil {
			missing = append(missing, e.ArenaName+" (arena)")
			continue
		}
		jobs = append(jobs, job{e.Arena, path})
	}
	if len(missing) > 0 {
		return fmt.Errorf("Missing: %s. Download it in Browse Maps first.", strings.Join(missing, ", "))
	}

	if err := a.Restore(); err != nil && !strings.HasPrefix(err.Error(), "Backup missing") {
		return err
	}
	for _, j := range jobs {
		if err := a.Swap(j.arena, j.path); err != nil {
			return err
		}
	}
	return nil
}
