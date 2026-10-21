#pragma once
// Maps tauschen im F8-Menue: dieselben Dateien wie der Launcher (state.json, backup\<Arena>),
// damit "Restore" im Launcher und im Spiel immer zusammenpassen.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace maps {

struct Map {
	std::string id;      // Workshop-ID ("" bei eigenen Dateien ohne ID)
	std::string name;    // Dateiname ohne Endung (so steht er in state.json)
	std::string title;   // Anzeigename
	std::string author;
	std::string lower;   // fuer die Suche: Titel + Autor klein
	std::wstring path;
	float mb = 0;
};

struct Arena {
	std::wstring file;   // z. B. Park_P.upk
	std::string fileU8;
	std::string name;    // Beckwith Park
	std::string swap;    // getauschte Map (Dateiname ohne Endung), "" = Original
	std::string swapTitle;
	bool locked = false; // gerade vom Spiel geladen
};

// mit mu sperren
extern std::mutex mu;
extern std::vector<Map> list;     // nach Titel sortiert
extern std::vector<Arena> arenas; // feste Reihenfolge
extern bool scanned;              // mindestens einmal gesucht
extern bool found;                // Rocket-League-Ordner gefunden
extern std::string busyArena;     // waehrend des Kopierens: Arena-Datei
extern std::string busyMap;       // und Map-Name
extern std::string msg;           // letzte Meldung
extern bool msgErr;
extern double msgAt;              // GetTickCount64()/1000 der Meldung

std::atomic<bool>& Busy();
void Refresh();                                           // im Hintergrund neu einlesen
void Swap(const std::wstring& arenaFile, const std::wstring& mapPath);
void Restore(const std::wstring& arenaFile);
void RestoreAll();
void Play(const std::string& arenaFileU8);                // Freeplay in dieser Arena starten (Spiel-Thread)
unsigned long long Thumb(const std::string& id);          // Vorschaubild als ImTextureID, 0 = (noch) keins

} // namespace maps
