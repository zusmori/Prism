#pragma once
// Kulissen-Merge im F8-Menue. Die eigentliche Arbeit (Map-Dateien entschluesseln, Unter-Level eintragen,
// neu packen) macht der Prism-Launcher; hier werden nur Wuensche geschrieben und der Stand gelesen:
//   merge_catalog.txt  (Prism)  Arena \t Name \t Kulissen-Map \t Name
//   merge_requests.txt (hier)   Datei \t Kulissen-Datei \t eigene Kulisse entfernen
//   merge_status.txt   (Prism)  Datei \t Kulissen-Datei \t ersetzen \t Stand \t Text
#include <mutex>
#include <string>
#include <vector>

namespace merge {

// Feste Kombination: Arena (eigene Kulisse aus) + Kulisse einer anderen Map
struct Combo {
	std::string base, baseName, backdrop, backdropName;
	bool active = false;          // gewuenscht
	std::string state, stateMsg;  // applied | pending | error | working | "" (noch nicht bearbeitet)
};

extern std::mutex mu;
extern std::vector<Combo> list; // mit mu sperren
extern bool loaded;             // Katalog gefunden
extern unsigned long long launcherSeen; // letzte Aenderung von Prism (ms), 0 = nie

void Refresh();                                                   // Dateien neu lesen (hoechstens alle 700 ms)
void Request(const std::string& file, const std::string& backdrop, bool replace); // backdrop "" = zuruecksetzen
void Toggle(const Combo& c); // an/aus (andere Kombination derselben Arena wird ersetzt)
void RestoreAll();
void RequestRestart(); // Prism soll das Spiel neu starten (neue Kulissen-Pakete kennt das Spiel erst danach)
bool NeedsRestart(const Combo& c);
extern unsigned long long restartAsked; // GetTickCount64, 0 = nie
void TickNames(); // Spiel-Thread: Map-Namen aus dem Spiel lesen (map_names.txt fuer Prism)

} // namespace merge
